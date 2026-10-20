#define VK_USE_PLATFORM_ANDROID_KHR
#include "native_vulkan_renderer.h"
#include "bl2_buffer_pool_core.h"
#include "native_vulkan_shader_pack.h"
#include "obb_sha256.h"
#include <vulkan/vulkan.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <time.h>
#include <sys/system_properties.h>
#ifndef BL2_VULKAN_BACKEND_ID
#define BL2_VULKAN_BACKEND_ID "unidentified-development-backend"
#endif

namespace bl2vk {
namespace {
constexpr GLbitfield GL_MAP_PERSISTENT_BIT=0x0040,GL_MAP_COHERENT_BIT=0x0080,GL_DYNAMIC_STORAGE_BIT=0x0100,GL_CLIENT_STORAGE_BIT=0x0200;
constexpr GLenum GL_SRGB_ALPHA=0x8C42;
using bl2::vk::Stage;
constexpr GLenum kVS = 0x8620, kFS = 0x8804;
constexpr std::size_t kMaxBuffer = 256u * 1024 * 1024;
constexpr std::size_t kMaxPipelines = 16384;
constexpr std::size_t kMaxResidentPipelines = 2048;
bool fatalDispatch=false;
struct Context;
struct DiagnosticSnapshot{std::uint32_t id{};std::string firstFile,details;bool complete=true;};
struct Device {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t family{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceMemoryProperties memory{};
    bufferpool::Budget memoryBudget;
    PFN_vkGetBufferMemoryRequirements2 getBufferMemoryRequirements2{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout pipelineLayout{};
    VkPipelineCache pipelineCache{};
    std::shared_ptr<bl2::vk::ShaderPack> pack;
    std::string cacheDirectory, identity, failure;
    std::vector<std::string> extensions;
    std::string extensionString;
    bool failed = false, gfxCapture = false;
    std::uint32_t gfxCaptures{}, gfxTextPairs{}, gfxOtherPairs{}, unknownParameters{}, drawErrorReports{}, particleCaptures{}, particleAtlasFiles{}, diagnosticPayloadFiles{}, shadowDescriptorReports{};
    std::size_t gfxAtlasBytes{}, diagnosticBytes{};
    std::unordered_map<std::string,std::uint32_t> gfxPairs, particlePairs;
    std::unordered_map<std::string,std::string> gfxAtlases;
    std::unordered_map<std::string,DiagnosticSnapshot> particleAtlases;
} gpu;
std::recursive_mutex mutex;
thread_local Context* current{};
Context* recordingOwner{};
thread_local GLenum orphanError = GL_NO_ERROR;
bool presentationPaused=false;
std::uint64_t presentationGeneration=1;
// WSI loss does not invalidate the Vulkan device or the current GL resources.
struct SurfaceUnavailable {};

void Error(GLenum e);
[[noreturn]] void Fail(const std::string& message) {
    gpu.failure = message;
    gpu.failed = true;
    Error(GL_INVALID_OPERATION);
    __android_log_print(ANDROID_LOG_ERROR, "BL2Vulkan", "%s", message.c_str());
    throw std::runtime_error(message);
}
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) Fail(std::string(operation) + " VkResult=" + std::to_string(result));
}
struct InvalidGlArgument : std::invalid_argument {
    GLenum error;std::uint32_t line;
    InvalidGlArgument(GLenum code,std::uint32_t sourceLine):std::invalid_argument("invalid GL argument"),error(code),line(sourceLine){}
};
void Require(bool valid, GLenum error = GL_INVALID_VALUE,std::uint32_t line=__builtin_LINE()) {
    if (!valid) { Error(error); throw InvalidGlArgument(error,line); }
}
template<class F> void Call(F&& f) {
    std::lock_guard lock(mutex);
    try { f(); } catch (const SurfaceUnavailable&) {} catch (const std::invalid_argument&) {} catch (const std::exception& e) {
        if (!gpu.failed) { gpu.failure = e.what(); gpu.failed = true; Error(GL_OUT_OF_MEMORY); }
        if(fatalDispatch&&gpu.failed)std::abort();
    }
}
std::uint32_t MemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) {
    for (std::uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (gpu.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    Fail("No Vulkan memory type satisfies resource allocation");
}
// Internal allocator only. Every caller still receives its own VkBuffer with
// logical size and buffer-relative offsets; only VkDeviceMemory is shared.
void AllocateDeviceMemory(const VkMemoryAllocateInfo& info, VkDeviceMemory& memory, const char* operation) {
    if (!gpu.memoryBudget.CanAllocate()) Fail("Vulkan device memory allocation count limit reached");
    VkDeviceMemory result{};
    const auto status = vkAllocateMemory(gpu.device, &info, nullptr, &result);
    Check(status, operation);
    // Calls and destruction run under the existing renderer mutex. No other
    // allocation can consume this checked slot between the call and increment.
    if (!gpu.memoryBudget.Allocated()) { vkFreeMemory(gpu.device, result, nullptr); Fail("Vulkan memory accounting invariant"); }
    memory = result;
}
void FreeDeviceMemory(VkDeviceMemory memory) {
    if (!memory) return;
    vkFreeMemory(gpu.device, memory, nullptr);
    if (!gpu.memoryBudget.Freed())
        __android_log_print(ANDROID_LOG_ERROR, "BL2Vulkan", "Vulkan device memory accounting underflow");
}
struct SmallBufferPage {
    VkDevice device{}; VkDeviceMemory memory{}; void* mapped{};
    bufferpool::PageKey key; bufferpool::Slots slots;
    ~SmallBufferPage() {
        if (!gpu.device || gpu.device != device) return;
        if (mapped) vkUnmapMemory(device, memory);
        FreeDeviceMemory(memory);
    }
};
// Weak references never extend a page's lifetime. A last Allocation lease
// releases the page; stale index entries are erased on each pooled request.
std::vector<std::weak_ptr<SmallBufferPage>> smallBufferPages;
struct Allocation {
    VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{}; VkDeviceSize size{};
    std::shared_ptr<SmallBufferPage> page; std::uint32_t slot{};
    ~Allocation() {
        if (!gpu.device) return;
        if (page) {
            if (buffer) vkDestroyBuffer(gpu.device, buffer, nullptr);
            if (!page->slots.Release(slot))
                __android_log_print(ANDROID_LOG_ERROR, "BL2Vulkan", "Vulkan small buffer slot release invariant");
            page.reset();
        } else {
            if (mapped) vkUnmapMemory(gpu.device, memory);
            if (buffer) vkDestroyBuffer(gpu.device, buffer, nullptr);
            FreeDeviceMemory(memory);
        }
    }
};
std::shared_ptr<Allocation> Allocate(VkDeviceSize bytes, VkBufferUsageFlags usage) {
    Require(bytes > 0 && bytes <= kMaxBuffer, GL_OUT_OF_MEMORY);
    auto out = std::make_shared<Allocation>(); out->size = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes; info.usage = usage; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Check(vkCreateBuffer(gpu.device, &info, nullptr, &out->buffer), "vkCreateBuffer");
    VkBufferMemoryRequirementsInfo2 query{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2}; query.buffer = out->buffer;
    VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; requirements.pNext = &dedicated;
    gpu.getBufferMemoryRequirements2(gpu.device, &query, &requirements);
    const auto& req = requirements.memoryRequirements;
    const auto type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const auto slotBytes = bufferpool::SlotClass(req.size, req.alignment,
        dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation);
    if (slotBytes) {
        smallBufferPages.erase(std::remove_if(smallBufferPages.begin(), smallBufferPages.end(),
            [](const auto& page) { return page.expired(); }), smallBufferPages.end());
        const bufferpool::PageKey key{type, slotBytes};
        std::shared_ptr<SmallBufferPage> page; std::uint32_t slot = 0;
        for (const auto& candidate : smallBufferPages) {
            auto live = candidate.lock();
            if (live && live->device == gpu.device && live->key == key && live->slots.Acquire(slot)) {
                page = std::move(live); break;
            }
        }
        if (!page) {
            page = std::make_shared<SmallBufferPage>(); page->device = gpu.device; page->key = key;
            Require(page->slots.Configure(slotBytes), GL_OUT_OF_MEMORY);
            VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            memory.allocationSize = bufferpool::kPageBytes; memory.memoryTypeIndex = type;
            AllocateDeviceMemory(memory, page->memory, "vkAllocateMemory(small buffer page)");
            void* mapped = nullptr;
            Check(vkMapMemory(gpu.device, page->memory, 0, bufferpool::kPageBytes, 0, &mapped), "vkMapMemory(small buffer page)");
            page->mapped = mapped;
            // If index growth throws, the local page frees its mapped memory.
            smallBufferPages.emplace_back(page);
            Require(page->slots.Acquire(slot), GL_OUT_OF_MEMORY);
        }
        // Assignment is noexcept. Once this lease is installed, any later
        // binding error rolls back buffer, slot and possibly the new page.
        out->page = page; out->slot = slot; out->memory = page->memory;
        const auto offset = page->slots.Offset(slot);
        out->mapped = static_cast<std::uint8_t*>(page->mapped) + offset;
        Check(vkBindBufferMemory(gpu.device, out->buffer, page->memory, offset), "vkBindBufferMemory(pooled buffer)");
    } else {
        VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicatedInfo.buffer = out->buffer;
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize = req.size; memory.memoryTypeIndex = type;
        if (dedicated.requiresDedicatedAllocation || dedicated.prefersDedicatedAllocation) memory.pNext = &dedicatedInfo;
        AllocateDeviceMemory(memory, out->memory, "vkAllocateMemory(buffer)");
        Check(vkBindBufferMemory(gpu.device, out->buffer, out->memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        Check(vkMapMemory(gpu.device, out->memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        out->mapped = mapped;
    }
    return out;
}

// Diagnostic projection: one physical 2D color image, mip0/layer0 only.
enum class RecordedColorOperation : std::uint32_t { Unknown, Draw, Clear, Upload, RawUpload, Blit, CopyImage };
struct RecordedColorWrite {
    RecordedColorOperation operation{};
    std::uint64_t sequence{}, draw{};
    bl2::vk::Digest vertex{}, fragment{};
    std::array<GLint,4> rectangle{};
    GLuint framebuffer{}, colorMask{};
    bool blendEnabled{}; std::array<GLenum,6> blend{};
    GLenum uploadFormat{}, uploadType{}; std::array<GLfloat,4> clearValue{};
};
struct RecordedImageWriter {
    std::uint64_t imageId{};
    RecordedColorWrite last{}, source{};
    std::uint64_t sourceImage{}, sourceImageId{};
    GLint sourceMip{}, sourceLayer{};
    std::array<GLint,4> sourceRectangle{};
};
bool RecordedColorScope(bool enabled,VkImageType type,std::uint32_t layers,VkImageAspectFlags aspect,GLint mip,GLint layer){
    return enabled&&type==VK_IMAGE_TYPE_2D&&layers==1&&aspect==VK_IMAGE_ASPECT_COLOR_BIT&&mip==0&&layer==0;
}
// Borrowed scalar projection only; no Image ownership or new writer state.
struct RecordedColorImage {
    VkImageType type{}; std::uint32_t layers{}; VkImageAspectFlags aspect{};
    VkFormat format{}; std::uint32_t width{},height{}; std::uint64_t image{};
    GLint mip{},layer{}; const RecordedImageWriter* writer{};
};
constexpr bl2::vk::Digest kObservedOpaqueVertex={0x37,0xcd,0xd5,0xd9,0x31,0x55,0x86,0x2e,0xcd,0xf7,0x92,0x2d,0x24,0x5b,0xec,0x4b,0x68,0xc4,0xb0,0x34,0x2c,0x69,0x5c,0x11,0x32,0x3b,0x2b,0x00,0x39,0x2a,0x0e,0xdf};
constexpr bl2::vk::Digest kObservedOpaqueFragment={0x99,0xcf,0x99,0x2e,0xd3,0x4e,0x10,0xc1,0x4a,0xad,0x30,0xb1,0xf8,0x51,0xb1,0x44,0xf4,0xe3,0xe9,0xbc,0x00,0xde,0x00,0x0b,0xc2,0xc4,0x8a,0xea,0xac,0xb1,0x1b,0xb7};
bool RecordedRenderedInputAvailable(const RecordedColorImage& image,std::uint64_t draw){
    if(!RecordedColorScope(true,image.type,image.layers,image.aspect,image.mip,image.layer)||!image.image||!image.width||!image.height||!image.writer)return false;
    const auto& writer=*image.writer;
    if(!writer.imageId||!writer.last.sequence||!writer.last.draw||writer.last.draw>=draw)return false;
    if(writer.last.operation==RecordedColorOperation::Draw)return true;
    return (writer.last.operation==RecordedColorOperation::Blit||writer.last.operation==RecordedColorOperation::CopyImage)&&
        writer.source.operation==RecordedColorOperation::Draw&&writer.source.sequence&&writer.source.sequence<writer.last.sequence&&
        writer.source.draw&&writer.source.draw<=writer.last.draw&&writer.sourceMip==0&&writer.sourceLayer==0;
}
bool RecordedPriorBlurInputAvailable(const RecordedColorImage& image,std::uint64_t draw,const bl2::vk::Digest& vertex,const bl2::vk::Digest& fragment){
    if(!RecordedRenderedInputAvailable(image,draw))return false;
    const auto& last=image.writer->last;
    return last.operation==RecordedColorOperation::Draw&&last.vertex==vertex&&last.fragment==fragment;
}
bool RecordedPriorBlurWitnessSelfCheck(){
    const bl2::vk::Digest vertex{1},fragment{2};RecordedImageWriter writer;writer.imageId=1;
    writer.last.operation=RecordedColorOperation::Draw;writer.last.sequence=1;writer.last.draw=1;writer.last.vertex=vertex;writer.last.fragment=fragment;
    RecordedColorImage input{VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_FORMAT_R16G16_SFLOAT,600,272,10,0,0,&writer};
    if(!RecordedPriorBlurInputAvailable(input,2,vertex,fragment))return false;
    writer.last.vertex[31]^=1;if(RecordedPriorBlurInputAvailable(input,2,vertex,fragment))return false;writer.last.vertex[31]^=1;
    writer.last.fragment[31]^=1;if(RecordedPriorBlurInputAvailable(input,2,vertex,fragment))return false;writer.last.fragment[31]^=1;
    writer.last.draw=2;if(RecordedPriorBlurInputAvailable(input,2,vertex,fragment))return false;writer.last.draw=1;
    for(auto operation:{RecordedColorOperation::Unknown,RecordedColorOperation::Clear,RecordedColorOperation::Upload,RecordedColorOperation::RawUpload,RecordedColorOperation::Blit,RecordedColorOperation::CopyImage}){writer.last.operation=operation;if(RecordedPriorBlurInputAvailable(input,2,vertex,fragment))return false;}
    writer.last.operation=RecordedColorOperation::Draw;input.writer=nullptr;return !RecordedPriorBlurInputAvailable(input,2,vertex,fragment);
}
bool RecordedOpaqueCopyAvailable(const RecordedColorImage& copy,const RecordedColorImage& target,std::uint64_t draw){
    if(!RecordedRenderedInputAvailable(copy,draw)||!RecordedColorScope(true,target.type,target.layers,target.aspect,target.mip,target.layer)||
       copy.format!=VK_FORMAT_R16G16B16A16_SFLOAT||target.format!=copy.format||!target.image||!target.writer||!target.writer->imageId||
       copy.width!=target.width||copy.height!=target.height||copy.image==target.image)return false;
    const auto& writer=*copy.writer;
    return (writer.last.operation==RecordedColorOperation::Blit||writer.last.operation==RecordedColorOperation::CopyImage)&&
        writer.imageId!=target.writer->imageId&&writer.sourceImageId==target.writer->imageId&&writer.sourceImage==target.image&&
        writer.source.vertex==kObservedOpaqueVertex&&writer.source.fragment==kObservedOpaqueFragment&&writer.source.colorMask==15&&!writer.source.blendEnabled;
}
bool RecordedDrawColorSlot(const std::array<GLenum,4>& draw,std::uint32_t& slot){
    if(draw[0]<GL_COLOR_ATTACHMENT0||draw[0]>GL_COLOR_ATTACHMENT3)return false;
    for(std::size_t i=1;i<draw.size();++i)if(draw[i]!=GL_NONE)return false;
    slot=draw[0]-GL_COLOR_ATTACHMENT0;return true;
}
bool RecordedAoWitnessSelfCheck(){
    RecordedImageWriter input,target,copy;input.imageId=1;target.imageId=2;copy.imageId=3;
    input.last.operation=RecordedColorOperation::Draw;input.last.sequence=1;input.last.draw=1;
    RecordedColorImage in{VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_FORMAT_R8_UNORM,64,64,10,0,0,&input};
    RecordedColorImage out{VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_FORMAT_R16G16B16A16_SFLOAT,1200,544,20,0,0,&target};
    RecordedColorImage sibling=out;sibling.image=30;sibling.writer=&copy;
    copy.last.operation=RecordedColorOperation::Blit;copy.last.sequence=3;copy.last.draw=1;
    copy.source.operation=RecordedColorOperation::Draw;copy.source.sequence=2;copy.source.draw=1;copy.source.vertex=kObservedOpaqueVertex;copy.source.fragment=kObservedOpaqueFragment;copy.source.colorMask=15;copy.sourceImageId=2;copy.sourceImage=20;
    if(!RecordedRenderedInputAvailable(in,2)||!RecordedOpaqueCopyAvailable(sibling,out,2)||RecordedOpaqueCopyAvailable(out,out,2))return false;
    for(auto operation:{RecordedColorOperation::Unknown,RecordedColorOperation::Clear,RecordedColorOperation::Upload,RecordedColorOperation::RawUpload}){input.last.operation=operation;if(RecordedRenderedInputAvailable(in,2))return false;}
    input.last.operation=RecordedColorOperation::Draw;input.last.draw=2;if(RecordedRenderedInputAvailable(in,2))return false;input.last.draw=1;
    copy.last.operation=RecordedColorOperation::CopyImage;if(!RecordedOpaqueCopyAvailable(sibling,out,2))return false;
    copy.source.operation=RecordedColorOperation::Clear;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.source.operation=RecordedColorOperation::Draw;
    copy.sourceImageId=3;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.sourceImageId=2;
    copy.sourceImage=21;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.sourceImage=20;
    copy.source.colorMask=7;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.source.colorMask=15;
    copy.source.blendEnabled=true;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.source.blendEnabled=false;
    copy.source.vertex[0]^=1;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.source.vertex[0]^=1;
    copy.source.fragment[0]^=1;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.source.fragment[0]^=1;
    sibling.width=1199;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;sibling.width=1200;
    sibling.mip=1;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;sibling.mip=0;
    copy.sourceLayer=1;if(RecordedOpaqueCopyAvailable(sibling,out,2))return false;copy.sourceLayer=0;
    std::uint32_t slot=99;if(!RecordedDrawColorSlot({GL_COLOR_ATTACHMENT2,GL_NONE,GL_NONE,GL_NONE},slot)||slot!=2||RecordedDrawColorSlot({GL_NONE,GL_NONE,GL_NONE,GL_NONE},slot)||RecordedDrawColorSlot({GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1,GL_NONE,GL_NONE},slot))return false;
    out.writer=nullptr;return !RecordedOpaqueCopyAvailable(sibling,out,2);
}
bool RecordedBlurTargetAvailable(const RecordedColorImage& target){
    return RecordedColorScope(true,target.type,target.layers,target.aspect,target.mip,target.layer)&&
        target.format==VK_FORMAT_R16G16_SFLOAT&&target.image&&target.width&&target.height&&target.writer&&target.writer->imageId;
}
bool RecordedBlurSceneCopyAvailable(const RecordedColorImage& copy,const RecordedColorImage& scene,const RecordedColorImage& target,std::uint64_t draw){
    if(!RecordedBlurTargetAvailable(target)||!RecordedOpaqueCopyAvailable(copy,scene,draw)||
       target.image==copy.image||target.image==scene.image||target.writer->imageId==copy.writer->imageId||target.writer->imageId==scene.writer->imageId)return false;
    return std::uint64_t(target.width)*2==scene.width&&std::uint64_t(target.height)*2==scene.height;
}
bool RecordedBlurWitnessSelfCheck(){
    RecordedImageWriter copy,scene,target;copy.imageId=1;scene.imageId=2;target.imageId=3;
    copy.last.operation=RecordedColorOperation::Blit;copy.last.sequence=3;copy.last.draw=1;
    copy.source.operation=RecordedColorOperation::Draw;copy.source.sequence=2;copy.source.draw=1;
    copy.source.vertex=kObservedOpaqueVertex;copy.source.fragment=kObservedOpaqueFragment;copy.source.colorMask=15;copy.sourceImageId=2;copy.sourceImage=20;
    RecordedColorImage source{VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_FORMAT_R16G16B16A16_SFLOAT,1200,544,20,0,0,&scene};
    RecordedColorImage sibling=source;sibling.image=10;sibling.writer=&copy;
    RecordedColorImage out{VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_FORMAT_R16G16_SFLOAT,600,272,30,0,0,&target};
    if(!RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;
    out.width=0x80000258u;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;out.width=600;
    out.height=271;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;out.height=272;
    out.format=VK_FORMAT_R16G16B16A16_SFLOAT;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;out.format=VK_FORMAT_R16G16_SFLOAT;
    source.writer=nullptr;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;source.writer=&scene;
    scene.imageId=4;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;scene.imageId=2;
    source.image=21;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;source.image=20;
    out.image=10;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;out.image=30;
    target.imageId=2;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;target.imageId=3;
    out.mip=1;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;out.mip=0;
    copy.source.operation=RecordedColorOperation::Clear;if(RecordedBlurSceneCopyAvailable(sibling,source,out,2))return false;
    copy.source.operation=RecordedColorOperation::Draw;copy.last.operation=RecordedColorOperation::CopyImage;
    return RecordedBlurSceneCopyAvailable(sibling,source,out,2);
}
void AssignRecordedColorWriter(RecordedImageWriter& destination,const RecordedColorWrite& next,const RecordedImageWriter* source,std::uint64_t sourceImage,GLint sourceMip,GLint sourceLayer,std::array<GLint,4> sourceRectangle={}){
    // Snapshot before assignment also preserves the pre-copy writer for self copies.
    const auto prior=source?source->last:RecordedColorWrite{};const auto sourceId=source?source->imageId:0;
    destination.last=next;destination.source=prior;destination.sourceImage=sourceImage;destination.sourceImageId=sourceId;
    destination.sourceMip=sourceMip;destination.sourceLayer=sourceLayer;destination.sourceRectangle=sourceRectangle;
}
bool RecordedImageWriterSelfCheck(){
    if(!RecordedColorScope(true,VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,0,0)||
       RecordedColorScope(false,VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,0,0)||
       RecordedColorScope(true,VK_IMAGE_TYPE_3D,1,VK_IMAGE_ASPECT_COLOR_BIT,0,0)||
       RecordedColorScope(true,VK_IMAGE_TYPE_2D,2,VK_IMAGE_ASPECT_COLOR_BIT,0,0)||
       RecordedColorScope(true,VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_DEPTH_BIT,0,0)||
       RecordedColorScope(true,VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,1,0)||
       RecordedColorScope(true,VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,0,1))return false;
    RecordedImageWriter producer,copy,unknown;producer.imageId=101;copy.imageId=102;unknown.imageId=103;
    RecordedColorWrite draw{.operation=RecordedColorOperation::Draw,.sequence=11,.draw=17,.vertex={1},.fragment={2},.rectangle={0,4,1200,540},.colorMask=15,.blendEnabled=true,.blend={GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ZERO,GL_ONE,GL_FUNC_ADD,GL_FUNC_ADD}};
    AssignRecordedColorWriter(producer,draw,nullptr,0,0,0);
    RecordedColorWrite transfer{.operation=RecordedColorOperation::CopyImage,.sequence=12,.draw=18,.rectangle={0,0,1200,544},.colorMask=15};
    AssignRecordedColorWriter(copy,transfer,&producer,0x1234,0,0,{0,0,1200,544});
    if(copy.imageId!=102||copy.last.sequence!=12||copy.sourceImageId!=101||copy.sourceImage!=0x1234||copy.source.sequence!=11||copy.source.vertex!=draw.vertex||copy.source.fragment!=draw.fragment||copy.source.blend[2]!=GL_ZERO||copy.source.blend[3]!=GL_ONE||copy.sourceRectangle!=transfer.rectangle)return false;
    RecordedColorWrite clear{.operation=RecordedColorOperation::Clear,.sequence=13,.draw=19,.rectangle={1,2,3,4},.colorMask=7,.clearValue={0,0,0,1}};
    AssignRecordedColorWriter(producer,clear,nullptr,0,0,0);
    if(copy.source.sequence!=11||copy.source.operation!=RecordedColorOperation::Draw||producer.sourceImageId||producer.source.sequence)return false;
    transfer.sequence=14;AssignRecordedColorWriter(producer,transfer,&producer,0x1234,0,0);
    if(producer.imageId!=101||producer.last.sequence!=14||producer.source.sequence!=13||producer.source.operation!=RecordedColorOperation::Clear||producer.sourceImageId!=101)return false;
    transfer.sequence=15;AssignRecordedColorWriter(copy,transfer,&producer,0x1234,0,0);
    if(copy.source.sequence!=14||copy.source.operation!=RecordedColorOperation::CopyImage)return false;
    transfer.sequence=16;AssignRecordedColorWriter(copy,transfer,&unknown,0x5678,0,0);
    if(copy.sourceImageId!=103||copy.source.sequence||copy.source.operation!=RecordedColorOperation::Unknown)return false;
    transfer.sequence=17;AssignRecordedColorWriter(copy,transfer,nullptr,0x5678,1,0);
    if(copy.sourceImageId||copy.source.sequence||copy.sourceImage!=0x5678||copy.sourceMip!=1)return false;
    RecordedColorWrite upload{.operation=RecordedColorOperation::Upload,.sequence=18,.draw=20,.rectangle={0,0,64,64},.colorMask=15,.uploadFormat=GL_RGBA,.uploadType=GL_HALF_FLOAT};
    AssignRecordedColorWriter(copy,upload,nullptr,0,0,0);
    return copy.imageId==102&&copy.last.uploadFormat==GL_RGBA&&copy.last.uploadType==GL_HALF_FLOAT&&!copy.source.sequence&&!copy.sourceImage;
}
struct Image {
    VkImage image{}; VkDeviceMemory memory{};
    VkFormat format{}; VkImageAspectFlags aspect{}; VkFormatFeatureFlags features{};
    std::uint32_t width{}, height{}, depth{1}, levels{1}, layers{1};
    VkImageType type = VK_IMAGE_TYPE_2D;
    std::vector<VkImageLayout> layouts;
    std::unique_ptr<RecordedImageWriter> recordedWriter;
    ~Image() {
        if (!gpu.device) return;
        if (image) vkDestroyImage(gpu.device, image, nullptr);
        if (memory) FreeDeviceMemory(memory);
    }
};
VkImageAspectFlags Aspect(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
bool Compressed(VkFormat f) { return f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }
std::uint32_t Mips(std::uint32_t w,std::uint32_t h,std::uint32_t d);
std::shared_ptr<Image> NewImage(VkFormat format, std::uint32_t w, std::uint32_t h,
                              std::uint32_t d, std::uint32_t levels, std::uint32_t layers, bool cube,bool volume=false) {
    Require(w && h && d && levels && levels <= 16 && w <= gpu.properties.limits.maxImageDimension2D && h <= gpu.properties.limits.maxImageDimension2D);
    auto out = std::make_shared<Image>(); out->format = format; out->aspect = Aspect(format);
    out->width = w; out->height = h; out->depth = d; out->levels = levels; out->layers = layers;
    out->type = volume||d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    Require(layers&&layers<=gpu.properties.limits.maxImageArrayLayers&&levels<=Mips(w,h,d)&&(out->type!=VK_IMAGE_TYPE_3D||layers==1));
    Require(out->type!=VK_IMAGE_TYPE_3D||(w<=gpu.properties.limits.maxImageDimension3D&&h<=gpu.properties.limits.maxImageDimension3D&&d<=gpu.properties.limits.maxImageDimension3D));
    Require(!cube||w==h);
    VkFormatProperties props{}; vkGetPhysicalDeviceFormatProperties(gpu.physical, format, &props);
    out->features=props.optimalTilingFeatures;
    constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((props.optimalTilingFeatures&required)!=required)Fail("Vulkan image format lacks sampled/transfer support: "+std::to_string(format));
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : out->type==VK_IMAGE_TYPE_3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0;
    info.imageType = out->type; info.format = format; info.extent = {w,h,d};
    info.mipLevels = levels; info.arrayLayers = layers; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (out->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (!Compressed(format) && (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(gpu.device, &info, nullptr, &out->image), "vkCreateImage");
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(gpu.device, out->image, &req);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = req.size; memory.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AllocateDeviceMemory(memory, out->memory, "vkAllocateMemory(image)");
    Check(vkBindImageMemory(gpu.device, out->image, out->memory, 0), "vkBindImageMemory");
    out->layouts.assign(levels * layers, VK_IMAGE_LAYOUT_UNDEFINED);
    return out;
}
struct View {
    VkImageView view{}; std::shared_ptr<Image> image;
    ~View() { if (view && gpu.device) vkDestroyImageView(gpu.device, view, nullptr); }
};
std::shared_ptr<View> NewView(const std::shared_ptr<Image>& image, VkImageViewType type,
                            std::uint32_t level, std::uint32_t levels, std::uint32_t layer,
                            std::uint32_t layers, VkImageAspectFlags aspect = 0,
                            VkComponentMapping swizzle = {}) {
    auto out = std::make_shared<View>(); out->image = image;
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image->image; info.viewType = type; info.format = image->format; info.components = swizzle;
    info.subresourceRange = {aspect ? aspect : image->aspect, level, levels, layer, layers};
    Check(vkCreateImageView(gpu.device, &info, nullptr, &out->view), "vkCreateImageView");
    return out;
}
struct Sampling {
    GLint min = GL_NEAREST_MIPMAP_LINEAR, mag = GL_LINEAR, s = GL_REPEAT, t = GL_REPEAT, r = GL_REPEAT;
    GLint compare = GL_NONE, compareFunc = GL_LEQUAL, base = 0, maximum = 1000;
    GLfloat aniso = 1, minLod = -1000, maxLod = 1000, lodBias = 0;
    std::array<GLfloat,4> border{};
};
struct SamplerObject {
    VkSampler sampler{};
    ~SamplerObject() { if (sampler && gpu.device) vkDestroySampler(gpu.device, sampler, nullptr); }
};
struct Texture {
    GLenum target{}, internal{}; bool immutable = false;
    GLenum uploadFormat{}, uploadType{};
    std::uint64_t uploadRevision{};
    std::array<GLint,7> uploadRegion{};
    Sampling sampling; std::shared_ptr<Image> image; std::shared_ptr<View> sampled;
    std::unordered_map<std::uint64_t,std::shared_ptr<View>> attachmentViews;
};
struct Buffer {
    std::vector<std::uint8_t> bytes, mapping; std::shared_ptr<Allocation> allocation;
    bool dirty = true, immutable = false, mapped = false;
    GLbitfield storageFlags{}, access{}; std::size_t mapOffset{}, mapLength{};
    GLenum usage = GL_STATIC_DRAW;
    std::uint64_t snapshotEpoch{};
};
struct Shader {
    Stage stage; VkShaderModule module{};
    ~Shader() { if (module && gpu.device) vkDestroyShaderModule(gpu.device, module, nullptr); }
};
struct Program {
    GLenum target{}; std::shared_ptr<Shader> shader;
    GLsizei length{};
    std::string diagnosticLabel, diagnosticSource;
    bool diagnosticParticleSprite{}, diagnosticTranslucent{};
    std::array<ConstantGroup,9> groups{};
    std::array<std::int16_t,16> samplerRegisters;
    std::vector<std::uint32_t> locals;
    Program() { samplerRegisters.fill(-1); }
};
struct ShareGroup {
    GLuint nextBuffer=1, nextTexture=1, nextProgram=1, nextSampler=1, nextRenderbuffer=1;
    std::unordered_map<GLuint,std::shared_ptr<Buffer>> buffers;
    std::unordered_map<GLuint,std::shared_ptr<Texture>> textures, renderbuffers;
    std::unordered_map<GLuint,std::shared_ptr<Program>> programs;
    std::unordered_map<GLuint,Sampling> samplers;
};
struct Attribute {
    bool enabled{}, integer{}; GLint size=4; GLenum type=GL_FLOAT;
    GLboolean normalized{}; GLsizei stride{}; GLuint divisor{}, name{};
    std::uintptr_t pointer{}; std::shared_ptr<Buffer> buffer;
    std::array<GLfloat,4> constant{0,0,0,1};
};
struct VertexArray {
    std::array<Attribute,16> attributes;
    GLuint elementName{}; std::shared_ptr<Buffer> elements;
};
struct Attachment {
    GLuint name{}; GLenum object=GL_NONE, textureTarget=GL_TEXTURE_2D;
    GLint level{}, layer{}; std::shared_ptr<Texture> texture;
};
struct Framebuffer {
    std::array<Attachment,4> colors; Attachment depth, stencil;
    std::array<GLenum,4> draw{GL_COLOR_ATTACHMENT0,GL_NONE,GL_NONE,GL_NONE};
    GLenum read=GL_COLOR_ATTACHMENT0;
};
struct Stencil { GLenum function=GL_ALWAYS, fail=GL_KEEP, depthFail=GL_KEEP, pass=GL_KEEP; GLint reference{}; GLuint compareMask=~0u, writeMask=~0u; };
struct QueryAllocation {
    VkQueryPool pool{};
    ~QueryAllocation(){if(pool&&gpu.device)vkDestroyQueryPool(gpu.device,pool,nullptr);}
};
struct Query {
    std::shared_ptr<QueryAllocation> allocation; bool active{}, ended{}; std::uint32_t pieces{};GLenum target{};
};
struct Frame {
    VkCommandPool pool{}; VkCommandBuffer command{}; VkFence fence{}; VkDescriptorPool descriptors{};
    VkSemaphore acquired{};
    bool recording{}, submitted{}, renderPass{};
    std::uint32_t drawCount{};
    std::vector<std::shared_ptr<void>> retain;
    std::unordered_set<const void*> retained;
    struct Chunk { std::shared_ptr<Allocation> buffer; std::size_t used{}; };
    std::vector<Chunk> chunks;
    std::vector<VkFramebuffer> framebuffers;
    ~Frame() {
        if (!gpu.device) return;
        for(auto f:framebuffers) vkDestroyFramebuffer(gpu.device,f,nullptr);
        if(descriptors) vkDestroyDescriptorPool(gpu.device,descriptors,nullptr);
        if(acquired) vkDestroySemaphore(gpu.device,acquired,nullptr);
        if(fence) vkDestroyFence(gpu.device,fence,nullptr);
        if(pool) vkDestroyCommandPool(gpu.device,pool,nullptr);
    }
};
struct Context {
    std::shared_ptr<ShareGroup> share;
    GLenum error=GL_NO_ERROR;
    std::thread::id owner{}; bool owned{};
    ANativeWindow* window{};
    std::uintptr_t pbuffer{};
    std::array<std::unique_ptr<Frame>,3> frames; std::size_t frameIndex{};
    std::unordered_map<GLenum,std::pair<GLuint,std::shared_ptr<Buffer>>> boundBuffers;
    GLuint activeTexture{}, vertexProgram{}, fragmentProgram{}, boundRenderbuffer{}, vao{}, drawFbo{}, readFbo{};
    std::array<std::unordered_map<GLenum,std::pair<GLuint,std::shared_ptr<Texture>>>,32> textureUnits;
    std::array<GLuint,32> samplerUnits{};
    GLuint nextVao=1,nextFbo=1,nextQuery=1;
    std::unordered_map<GLuint,VertexArray> vaos;
    std::unordered_map<GLuint,Framebuffer> fbos;
    std::unordered_map<GLuint,std::shared_ptr<Query>> queries;
    std::shared_ptr<Query> activeQuery;
    std::unordered_set<GLenum> enabled;
    std::array<GLint,4> viewport{0,0,0,0}, scissor{0,0,0,0};
    GLfloat nearDepth=0, farDepth=1, minDepthBounds=0, maxDepthBounds=1;
    GLenum depthFunction=GL_LESS,cull=GL_BACK,front=GL_CCW;
    GLboolean depthWrite=GL_TRUE;
    Stencil frontStencil,backStencil;
    GLenum srcRgb=GL_ONE,dstRgb=GL_ZERO,srcAlpha=GL_ONE,dstAlpha=GL_ZERO,rgbEquation=GL_FUNC_ADD,alphaEquation=GL_FUNC_ADD;
    std::array<GLfloat,4> blendColor{},clearColor{};
    std::array<GLboolean,4> colorWrite{GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
    GLfloat polygonFactor{},polygonUnits{},lineWidth=1,clearDepth=1; GLint clearStencil{};
    GLint unpackAlignment=4,unpackRow=0,unpackImage=0,unpackSkipRows=0,unpackSkipPixels=0,unpackSkipImages=0;
    GLint packAlignment=4,packRow=0,packSkipRows=0,packSkipPixels=0;
    std::shared_ptr<Texture> defaultColor, defaultDepth;
    std::string activePass;
    Context() { vaos.emplace(0,VertexArray{}); }
};
std::unordered_map<std::uintptr_t,std::unique_ptr<Context>> contexts;
std::uintptr_t nextContext=1;
struct Window {
    ANativeWindow* native{}; VkSurfaceKHR surface{}; VkSwapchainKHR swapchain{};
    VkFormat format{}; VkExtent2D extent{};
    std::vector<VkImage> images; std::vector<VkImageLayout> layouts;
    // One semaphore pair per swapchain image; each pair is reused after its present retires.
    std::vector<VkSemaphore> ready; VkSemaphore acquired{};
    int interval=1;
    bool unavailable{};
    std::uint64_t generation{};
    Context* owner{};
    std::shared_ptr<Texture> color,depth;
};
std::unordered_map<ANativeWindow*,Window> windows;
struct Pbuffer {Context* owner{};std::shared_ptr<Texture> color,depth;};
std::unordered_map<std::uintptr_t,Pbuffer> pbuffers;
int swapInterval=1;
std::unordered_map<std::string,VkRenderPass> renderPasses;
struct PipelineObject {
    VkPipeline pipeline{};std::uint64_t lastUse{};bool pinned{}, successRemembered{};std::size_t postDrawSlot=SIZE_MAX;
    ~PipelineObject(){if(pipeline&&gpu.device)vkDestroyPipeline(gpu.device,pipeline,nullptr);}
};
std::uint64_t pipelineUse=0;
std::uint64_t drawEpoch=1;
std::uint64_t diagnosticImageSequence=0,diagnosticWriteSequence=0;
RecordedImageWriter& ImageWriter(Image& image){
    if(!image.recordedWriter){image.recordedWriter=std::make_unique<RecordedImageWriter>();image.recordedWriter->imageId=++diagnosticImageSequence;}
    return *image.recordedWriter;
}
void RecordColorWrite(const std::shared_ptr<Image>& image,GLint mip,GLint layer,RecordedColorWrite next,const std::shared_ptr<Image>& source={},GLint sourceMip=0,GLint sourceLayer=0,std::array<GLint,4> sourceRectangle={}){
    if(!image||!RecordedColorScope(gpu.gfxCapture,image->type,image->layers,image->aspect,mip,layer))return;
    auto& destination=ImageWriter(*image);
    const auto* prior=source&&RecordedColorScope(true,source->type,source->layers,source->aspect,sourceMip,sourceLayer)?&ImageWriter(*source):nullptr;
    next.sequence=++diagnosticWriteSequence;next.draw=drawEpoch;
    AssignRecordedColorWriter(destination,next,prior,source?static_cast<std::uint64_t>(source->image):0,sourceMip,sourceLayer,sourceRectangle);
}
std::unordered_map<std::string,std::shared_ptr<PipelineObject>> pipelines;
std::unordered_map<std::string,std::weak_ptr<Shader>> shaders;
std::unordered_map<std::string,std::shared_ptr<SamplerObject>> samplers;
struct Sync { VkFence fence{}; };
std::unordered_set<Sync*> syncs;
void SaveCaches();
void Prewarm();
void ResetPrewarmState(bool beginRun=false);
std::size_t CandidateCount();
void PrewarmMandatory(const seed::Request& request);
void SavePostDraw();
bool initializationComplete=false, mandatoryReady=false, postDrawCapture=false;
std::unordered_set<std::string> mandatoryKeys;
std::uint64_t startupCreates=0, runtimeCreates=0, lateMandatoryCreates=0, successfulDraws=0, rejectedDraws=0;
std::uint64_t MonotonicNs(){timespec now{};if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)Fail("CLOCK_MONOTONIC unavailable for PSO receipt");return std::uint64_t(now.tv_sec)*1000000000u+now.tv_nsec;}


void Error(GLenum e) { auto& value=current?current->error:orphanError; if(value==GL_NO_ERROR)value=e; }
Context& C() { Require(current && gpu.device,GL_INVALID_OPERATION); return *current; }
VertexArray& Vao() { return C().vaos.at(C().vao); }
template<class T> void Retain(const std::shared_ptr<T>& object);
Frame& Recording();
void Submit(bool wait,VkSemaphore acquired,VkSemaphore ready);
void EndPass() {
    auto& c=C(); auto& f=*c.frames[c.frameIndex];
    if(f.renderPass) {
        if(c.activeQuery && c.activeQuery->active) {
            vkCmdEndQuery(f.command,c.activeQuery->allocation->pool,c.activeQuery->pieces-1);
            c.activeQuery->active=false;
        }
        vkCmdEndRenderPass(f.command); f.renderPass=false; c.activePass.clear();
    }
}
Frame& Recording() {
    if(recordingOwner&&recordingOwner!=current){auto* next=current;current=recordingOwner;Submit(false,VK_NULL_HANDLE,VK_NULL_HANDLE);current=next;}
    auto& c=C(); auto& f=*c.frames[c.frameIndex];
    if(!f.recording) {
        if(f.submitted) { Check(vkWaitForFences(gpu.device,1,&f.fence,VK_TRUE,UINT64_MAX),"frame fence"); f.submitted=false; }
        for(auto framebuffer:f.framebuffers)vkDestroyFramebuffer(gpu.device,framebuffer,nullptr);
        f.framebuffers.clear(); f.retain.clear(); f.retained.clear();
        for(auto& chunk:f.chunks)chunk.used=0;
        f.drawCount=0;
        Check(vkResetCommandPool(gpu.device,f.pool,0),"vkResetCommandPool");
        Check(vkResetDescriptorPool(gpu.device,f.descriptors,0),"vkResetDescriptorPool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(f.command,&begin),"vkBeginCommandBuffer");f.recording=true;recordingOwner=&c;
    }
    return f;
}
template<class T> void Retain(const std::shared_ptr<T>& object) { if(object){auto& f=Recording();if(f.retained.insert(object.get()).second)f.retain.emplace_back(object);} }
struct Slice { std::shared_ptr<Allocation> buffer; VkDeviceSize offset{},size{}; void* data{}; };
Slice Upload(std::size_t bytes,std::size_t alignment=4) {
    Require(bytes && bytes<=kMaxBuffer,GL_OUT_OF_MEMORY);
    auto& f=Recording();
    for(auto& chunk:f.chunks) {
        auto start=(chunk.used+alignment-1)/alignment*alignment;
        if(start+bytes<=chunk.buffer->size) {chunk.used=start+bytes;return {chunk.buffer,start,bytes,static_cast<std::uint8_t*>(chunk.buffer->mapped)+start};}
    }
    auto allocation=Allocate(std::max<std::size_t>(4*1024*1024,(bytes+alignment-1)/alignment*alignment),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    f.chunks.push_back({allocation,bytes});return {allocation,0,bytes,allocation->mapped};
}
void Submit(bool wait=false,VkSemaphore acquired=VK_NULL_HANDLE,VkSemaphore ready=VK_NULL_HANDLE) {
    auto& c=C();auto& f=*c.frames[c.frameIndex];
    if(!f.recording)Recording();
    EndPass();Check(vkEndCommandBuffer(f.command),"vkEndCommandBuffer");
    Check(vkResetFences(gpu.device,1,&f.fence),"vkResetFences");
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1;submit.pCommandBuffers=&f.command;
    if(acquired) {submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acquired;submit.pWaitDstStageMask=&stage;}
    if(ready) {submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&ready;}
    Check(vkQueueSubmit(gpu.queue,1,&submit,f.fence),"vkQueueSubmit");f.recording=false;f.submitted=true;
    if(recordingOwner==&c)recordingOwner=nullptr;
    if(wait)Check(vkWaitForFences(gpu.device,1,&f.fence,VK_TRUE,UINT64_MAX),"submit fence");
    c.frameIndex=(c.frameIndex+1)%c.frames.size();
}
void Transition(const std::shared_ptr<Image>& image,VkImageLayout layout,
                std::uint32_t level=0,std::uint32_t levels=~0u,std::uint32_t layer=0,std::uint32_t layers=~0u) {
    auto& f=Recording();
    levels=std::min(levels,image->levels-level);layers=std::min(layers,image->layers-layer);
    bool changed=false;
    for(std::uint32_t l=layer;l<layer+layers;++l)for(std::uint32_t m=level;m<level+levels;++m)changed|=image->layouts[l*image->levels+m]!=layout;
    if(!changed&&(layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL||layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL||layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)){Retain(image);return;}
    EndPass();
    for(std::uint32_t l=layer;l<layer+layers;++l)for(std::uint32_t m=level;m<level+levels;++m) {
        auto& old=image->layouts[l*image->levels+m];
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask=old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout=old;barrier.newLayout=layout;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image->image;barrier.subresourceRange={image->aspect,m,1,l,1};
        VkPipelineStageFlags srcStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags dstStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        // Explicit scopes avoid observed ARM32 Adreno AO corruption with the spec-valid broad dependency.
        if(old==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL&&layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&image->aspect==VK_IMAGE_ASPECT_COLOR_BIT){
            srcStage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstStage=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(f.command,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&barrier);old=layout;
    }
    Retain(image);
}
std::uint32_t Mips(std::uint32_t w,std::uint32_t h,std::uint32_t d=1) {
    std::uint32_t n=1;for(auto v=std::max({w,h,d});v>1;v>>=1)++n;return n;
}
std::string Hex(const bl2::vk::Digest& digest) {
    static constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(64);
    for(auto b:digest){out.push_back(hex[b>>4]);out.push_back(hex[b&15]);}return out;
}
std::shared_ptr<Shader> Module(Stage stage) {
    const auto key=std::to_string(stage.target)+Hex(stage.normalizedDigest);
    if(auto it=shaders.find(key);it!=shaders.end())if(auto existing=it->second.lock())return existing;
    auto shader=std::make_shared<Shader>();shader->stage=std::move(stage);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize=shader->stage.spirv.size()*4;info.pCode=shader->stage.spirv.data();
    Check(vkCreateShaderModule(gpu.device,&info,nullptr,&shader->module),"vkCreateShaderModule");
    shader->stage.spirv.clear();shader->stage.spirv.shrink_to_fit();shaders[key]=shader;return shader;
}
std::unique_ptr<Frame> NewFrame() {
    auto frame=std::make_unique<Frame>();
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex=gpu.family;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    Check(vkCreateCommandPool(gpu.device,&pool,nullptr,&frame->pool),"vkCreateCommandPool");
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool=frame->pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;
    Check(vkAllocateCommandBuffers(gpu.device,&command,&frame->command),"vkAllocateCommandBuffers");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(gpu.device,&fence,nullptr,&frame->fence),"vkCreateFence");
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    Check(vkCreateSemaphore(gpu.device,&semaphore,nullptr,&frame->acquired),"vkCreateSemaphore");
    std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,8192},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,131072}}};
    VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.maxSets=4096;descriptors.poolSizeCount=sizes.size();descriptors.pPoolSizes=sizes.data();
    Check(vkCreateDescriptorPool(gpu.device,&descriptors,nullptr,&frame->descriptors),"vkCreateDescriptorPool");
    return frame;
}
void DropSwapchain(Window& window) {
    if(!gpu.device)return;
    if(window.swapchain||!window.ready.empty())Check(vkQueueWaitIdle(gpu.queue),"swapchain retirement");
    for(auto s:window.ready)vkDestroySemaphore(gpu.device,s,nullptr);
    window.ready.clear();window.images.clear();window.layouts.clear();
    if(window.swapchain)vkDestroySwapchainKHR(gpu.device,window.swapchain,nullptr);
    window.swapchain=VK_NULL_HANDLE;
}
void DropSurface(Window& window){
    DropSwapchain(window);if(window.surface)vkDestroySurfaceKHR(gpu.instance,window.surface,nullptr);window.surface=VK_NULL_HANDLE;
}
void UnavailableSurface(Window& window,const char* operation,VkResult reason){
    const bool report=!window.unavailable||window.generation!=presentationGeneration;
    DropSurface(window);window.unavailable=true;window.generation=presentationGeneration;
    if(report)__android_log_print(ANDROID_LOG_WARN,"BL2Vulkan","Presentation unavailable: operation=%s VkResult=%d window=%p generation=%llu paused=%u; Vulkan context/resources retained",operation,reason,window.native,static_cast<unsigned long long>(presentationGeneration),presentationPaused);
}
bool RecoverableSurfaceStatus(VkResult result){return result==VK_ERROR_SURFACE_LOST_KHR;}
void SurfaceCheck(Window& window,VkResult result,const char* operation){
    if(RecoverableSurfaceStatus(result)){UnavailableSurface(window,operation,result);throw SurfaceUnavailable{};}
    Check(result,operation);
}
void MarkPresentationSurface(Window& window,ANativeWindow* fresh){
    window.unavailable=!fresh||window.native!=fresh;window.generation=window.unavailable?presentationGeneration:0;
}
bool RetrySurface(const Window& window){return !window.unavailable||window.generation!=presentationGeneration;}
VkSurfaceTransformFlagBitsKHR PresentationTransform(VkSurfaceTransformFlagsKHR supported){return static_cast<VkSurfaceTransformFlagBitsKHR>(supported&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);}
VkExtent2D RequestedExtent(const VkSurfaceCapabilitiesKHR& caps,std::uint32_t width,std::uint32_t height){
    if(caps.currentExtent.width!=~0u)return caps.currentExtent;
    return {std::clamp(width,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(height,caps.minImageExtent.height,caps.maxImageExtent.height)};
}
bool ExtentChanged(VkExtent2D current,VkExtent2D requested){return current.width!=requested.width||current.height!=requested.height;}
bool SurfaceChanged(Window& window){
    if(window.interval!=swapInterval)return true;
    VkSurfaceCapabilitiesKHR caps{};SurfaceCheck(window,vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical,window.surface,&caps),"suboptimal surface capabilities");
    if(!PresentationTransform(caps.supportedTransforms))Fail("Android surface no longer supports identity presentation transform");
    return ExtentChanged(window.extent,RequestedExtent(caps,ANativeWindow_getWidth(window.native),ANativeWindow_getHeight(window.native)));
}
void Resize(Window& window) {
    DropSwapchain(window);
    VkSurfaceCapabilitiesKHR caps{};SurfaceCheck(window,vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical,window.surface,&caps),"surface capabilities");
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))Fail("Android Vulkan surface cannot receive the required final blit");
    std::uint32_t n=0;SurfaceCheck(window,vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical,window.surface,&n,nullptr),"surface format count");
    std::vector<VkSurfaceFormatKHR> formats(n);SurfaceCheck(window,vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical,window.surface,&n,formats.data()),"surface formats");
    Require(!formats.empty(),GL_INVALID_OPERATION);
    auto format=formats[0];for(auto f:formats)if(f.format==VK_FORMAT_R8G8B8A8_UNORM||f.format==VK_FORMAT_B8G8R8A8_UNORM){format=f;break;}
    VkFormatProperties formatProps{};vkGetPhysicalDeviceFormatProperties(gpu.physical,format.format,&formatProps);
    if(!(formatProps.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))Fail("Android surface format lacks Vulkan blit destination support");
    VkExtent2D extent=RequestedExtent(caps,ANativeWindow_getWidth(window.native),ANativeWindow_getHeight(window.native));
    if(!extent.width||!extent.height){UnavailableSurface(window,"zero surface extent",VK_NOT_READY);throw SurfaceUnavailable{};}
    const auto transform=PresentationTransform(caps.supportedTransforms);
    __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Surface resize: native=%dx%d currentExtent=%ux%u swapchain=%ux%u currentTransform=0x%x supportedTransforms=0x%x selectedTransform=0x%x",
        ANativeWindow_getWidth(window.native),ANativeWindow_getHeight(window.native),caps.currentExtent.width,caps.currentExtent.height,extent.width,extent.height,caps.currentTransform,caps.supportedTransforms,transform);
    if(!transform)Fail("Android surface lacks identity presentation transform for the unrotated legacy framebuffer");
    VkPresentModeKHR mode=VK_PRESENT_MODE_FIFO_KHR;
    SurfaceCheck(window,vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical,window.surface,&n,nullptr),"present mode count");
    std::vector<VkPresentModeKHR> modes(n);SurfaceCheck(window,vkGetPhysicalDeviceSurfacePresentModesKHR(gpu.physical,window.surface,&n,modes.data()),"present modes");
    if(swapInterval==0&&std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=modes.end())mode=VK_PRESENT_MODE_MAILBOX_KHR;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for(auto option:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if(caps.supportedCompositeAlpha&option){alpha=option;break;}
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface=window.surface;info.minImageCount=std::max(caps.minImageCount,3u);
    if(caps.maxImageCount)info.minImageCount=std::min(info.minImageCount,caps.maxImageCount);
    info.imageFormat=format.format;info.imageColorSpace=format.colorSpace;info.imageExtent=extent;
    info.imageArrayLayers=1;info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Legacy render targets retain the guest's logical window orientation.
    // Selecting currentTransform would require rotating the final image first.
    info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;info.preTransform=transform;
    info.compositeAlpha=alpha;info.presentMode=mode;info.clipped=VK_TRUE;
    SurfaceCheck(window,vkCreateSwapchainKHR(gpu.device,&info,nullptr,&window.swapchain),"vkCreateSwapchainKHR");
    window.extent=extent;window.format=format.format;window.interval=swapInterval;
    Check(vkGetSwapchainImagesKHR(gpu.device,window.swapchain,&n,nullptr),"swapchain image count");
    window.images.resize(n);Check(vkGetSwapchainImagesKHR(gpu.device,window.swapchain,&n,window.images.data()),"swapchain images");
    window.layouts.assign(n,VK_IMAGE_LAYOUT_UNDEFINED);window.ready.resize(n);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(auto& s:window.ready)Check(vkCreateSemaphore(gpu.device,&semaphore,nullptr,&s),"present semaphore");
}
Window& Surface(ANativeWindow* native) {
    Require(native!=nullptr,GL_INVALID_OPERATION);
    auto [it,created]=windows.try_emplace(native);auto& window=it->second;
    if(created){window.native=native;ANativeWindow_acquire(native);}
    if(presentationPaused){
        if(!window.extent.width||!window.extent.height){const auto width=ANativeWindow_getWidth(native),height=ANativeWindow_getHeight(native);if(width<=0||height<=0)throw SurfaceUnavailable{};window.extent={static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)};}
        return window;
    }
    if(!RetrySurface(window))throw SurfaceUnavailable{};
    if(window.generation!=presentationGeneration){DropSurface(window);window.unavailable=false;window.generation=presentationGeneration;}
    if(!window.surface){
        VkAndroidSurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};surface.window=native;
        SurfaceCheck(window,vkCreateAndroidSurfaceKHR(gpu.instance,&surface,nullptr,&window.surface),"vkCreateAndroidSurfaceKHR");
        VkBool32 supported=VK_FALSE;SurfaceCheck(window,vkGetPhysicalDeviceSurfaceSupportKHR(gpu.physical,gpu.family,window.surface,&supported),"surface queue support");
        if(!supported)Fail("Selected Vulkan graphics queue cannot present to Android");
    }
    if(!window.swapchain||window.interval!=swapInterval)Resize(window);
    return window;
}
void DefaultTarget(Context& context,std::uint32_t width,std::uint32_t height) {
    if(context.defaultColor&&context.defaultColor->image->width==width&&context.defaultColor->image->height==height)return;
    context.defaultColor=std::make_shared<Texture>();context.defaultDepth=std::make_shared<Texture>();
    context.defaultColor->target=GL_TEXTURE_2D;context.defaultColor->internal=GL_RGBA8;
    context.defaultColor->image=NewImage(VK_FORMAT_R8G8B8A8_UNORM,width,height,1,1,1,false);
    context.defaultDepth->target=GL_TEXTURE_2D;context.defaultDepth->internal=GL_DEPTH24_STENCIL8;
    context.defaultDepth->image=NewImage(VK_FORMAT_D24_UNORM_S8_UINT,width,height,1,1,1,false);
    if(context.viewport[2]==0&&context.viewport[3]==0)context.viewport={0,0,static_cast<GLint>(width),static_cast<GLint>(height)};
    if(context.scissor[2]==0&&context.scissor[3]==0)context.scissor=context.viewport;
}
std::vector<std::uint8_t> ReadFile(const std::string& path,std::size_t limit) {
    FILE* f=std::fopen(path.c_str(),"rb");if(!f)return {};
    if(std::fseek(f,0,SEEK_END)!=0){std::fclose(f);return {};}
    const auto length=std::ftell(f);if(length<0||static_cast<unsigned long>(length)>limit){std::fclose(f);return {};}
    std::rewind(f);std::vector<std::uint8_t> bytes(length);
    const auto count=std::fread(bytes.data(),1,bytes.size(),f);std::fclose(f);if(count!=bytes.size())return {};return bytes;
}
bool AtomicFile(const std::string& path,const void* data,std::size_t size) {
    const auto temporary=path+".tmp";FILE* f=std::fopen(temporary.c_str(),"wb");if(!f)return false;
    bool good=std::fwrite(data,1,size,f)==size&&std::fflush(f)==0&&fsync(fileno(f))==0;
    good=std::fclose(f)==0&&good;if(good)good=std::rename(temporary.c_str(),path.c_str())==0;if(!good)std::remove(temporary.c_str());return good;
}
ObbSha256::Digest Hash(const void* bytes,std::size_t size){ObbSha256 hash;hash.Update(static_cast<const std::uint8_t*>(bytes),size);return hash.Finalize();}
std::string ShaderLabel(std::string_view raw){
    constexpr std::string_view marker="//ShaderFileName:";const auto offset=raw.find(marker);if(offset==std::string_view::npos)return {};
    auto label=raw.substr(offset+marker.size());label=label.substr(0,label.find_first_of("\r\n"));
    while(!label.empty()&&(label.front()==' '||label.front()=='\t'))label.remove_prefix(1);
    while(!label.empty()&&(label.back()==' '||label.back()=='\t'))label.remove_suffix(1);
    return std::string(label.substr(0,96));
}
std::string_view ShaderCommentValue(std::string_view raw,std::string_view name){
    for(std::size_t offset=0;(offset=raw.find("//",offset))!=std::string_view::npos;offset+=2){
        auto line=raw.substr(offset+2);line=line.substr(0,line.find_first_of("\r\n"));
        while(!line.empty()&&(line.front()==' '||line.front()=='\t'))line.remove_prefix(1);
        if(!line.starts_with(name))continue;line.remove_prefix(name.size());
        while(!line.empty()&&(line.front()==' '||line.front()=='\t'))line.remove_prefix(1);
        if(!line.starts_with("="))continue;line.remove_prefix(1);
        while(!line.empty()&&(line.front()==' '||line.front()=='\t'))line.remove_prefix(1);
        while(!line.empty()&&(line.back()==' '||line.back()=='\t'))line.remove_suffix(1);
        return line;
    }
    return {};
}
bool ParticleSpriteSource(std::string_view raw){
    if(ShaderLabel(raw)!="BasePassVertexShader")return false;
    return ShaderCommentValue(raw,"PARTICLES_ALLOW_AXIS_ROTATION")=="1"&&ShaderCommentValue(raw,"NUM_AXIS_ROTATION_VECTORS")=="2"&&ShaderCommentValue(raw,"USE_OCCLUSION_PERCENTAGE")=="1"&&ShaderCommentValue(raw,"USE_PARTICLE_VERTEX_INDEX")=="0";
}
bool TranslucentBasePassSource(std::string_view raw){
    return ShaderLabel(raw)=="BasePassPixelShader"&&ShaderCommentValue(raw,"MATERIALBLENDING_TRANSLUCENT")=="1";
}
bool TakeShadowDescriptorReport(bool enabled,bool shadow,std::uint32_t& reports){
    if(!enabled||!shadow||reports>=8)return false;++reports;return true;
}
bool SelectedDeepCapture(std::string_view vertexLabel,std::string_view fragmentLabel,std::string_view vertexDigest,std::string_view fragmentDigest){
    return vertexLabel=="AmbientOcclusionShader"&&fragmentLabel=="AmbientOcclusionShader"&&vertexDigest=="bb95115f9e48266950c02a252308be05e1d485cc8e78c12c0e3d6441801b04b3"&&fragmentDigest=="d26d89b92f67348605297147f10e67c46eb20bd95bd756cff5323ab9903ff433";
}
bool TakeParticleCapture(std::unordered_map<std::string,std::uint32_t>& pairs,std::uint32_t& captures,const std::string& pair){
    if(captures>=4)return false;auto found=pairs.find(pair);
    if(found==pairs.end()){if(pairs.size()>=4)return false;found=pairs.emplace(pair,0).first;}
    if(found->second>=1)return false;++found->second;++captures;return true;
}
bool TakeDiagnosticPayload(std::uint32_t& files,std::size_t bytes){
    if(files>=471||bytes>4*1024*1024)return false;++files;return true;
}
bool CaptureBytesAllowed(std::size_t used,std::size_t bytes,bool report){
    const std::size_t limit=(report?32u:31u)*1024*1024;return bytes<=4*1024*1024&&used<=limit&&bytes<=limit-used;
}
enum class DriverCacheError { None, Envelope, Identity };
DriverCacheError ValidateDriverCache(const std::vector<std::uint8_t>& data,const VkPhysicalDeviceProperties& properties,std::uint64_t& length){
    length=0;if(data.size()<80||data.size()>64*1024*1024+48||std::memcmp(data.data(),"BL2VKC01",8)!=0)return DriverCacheError::Envelope;
    std::memcpy(&length,data.data()+8,8);ObbSha256::Digest expected{};std::memcpy(expected.data(),data.data()+16,32);
    if(length!=data.size()-48||Hash(data.data()+48,length)!=expected)return DriverCacheError::Envelope;
    std::uint32_t headerLength=0,version=0,vendor=0,device=0;std::memcpy(&headerLength,data.data()+48,4);std::memcpy(&version,data.data()+52,4);std::memcpy(&vendor,data.data()+56,4);std::memcpy(&device,data.data()+60,4);
    if(headerLength<32||headerLength>length||version!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE||vendor!=properties.vendorID||device!=properties.deviceID||std::memcmp(data.data()+64,properties.pipelineCacheUUID,16)!=0)return DriverCacheError::Identity;
    return DriverCacheError::None;
}
std::vector<std::uint8_t> DriverCache(const std::string& path){
    auto data=ReadFile(path,64*1024*1024+48);if(data.empty())return {};
    std::uint64_t length=0;const auto error=ValidateDriverCache(data,gpu.properties,length);
    if(error!=DriverCacheError::None){__android_log_print(ANDROID_LOG_WARN,"BL2Vulkan","%s",error==DriverCacheError::Envelope?"Rejected corrupt/incomplete Vulkan driver cache envelope":"Rejected Vulkan driver cache identity/header mismatch");return {};}
    std::memmove(data.data(),data.data()+48,length);data.resize(length);return data;
}
}

bool Initialize(std::shared_ptr<bl2::vk::ShaderPack> pack,const char* cache_directory) { return Initialize(std::move(pack),cache_directory,nullptr); }
bool Initialize(std::shared_ptr<bl2::vk::ShaderPack> pack,const char* cache_directory,const seed::Request* mandatory) {
    bool result=false;
    Call([&] {
        if(gpu.device){if(mandatory)Fail("Mandatory seed cannot reconfigure an initialized device");result=!gpu.failed;return;}
        ResetPrewarmState(true);
        Require(pack&&pack->Ready(),GL_INVALID_OPERATION);gpu.pack=std::move(pack);
        gpu.cacheDirectory=cache_directory?cache_directory:"";gpu.failed=false;gpu.failure.clear();
        presentationPaused=false;presentationGeneration=1;
        std::array<char,PROP_VALUE_MAX> captureProperty{};__system_property_get("debug.bl2.vk_gfx_capture",captureProperty.data());gpu.gfxCapture=std::strcmp(captureProperty.data(),"1")==0;
        if(gpu.gfxCapture)__android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","After-only capture enabled: otherDiagnosticPairs=0 deepPairs=4 deepDraws=4 deepSelection=exact-ao-bilateral-blur-after-prior-blur-and-opaque-copy readback=sampled-after-and-target-after targetBeforeUnobserved=1 sharedAtlasBytes=16777216 sharedAtlasSnapshots=8 particleAtlasFiles=32 maxFileBytes=4194304 directory=%s",gpu.cacheDirectory.c_str());
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};application.pApplicationName="Borderlands2 direct ARM32";
        application.applicationVersion=1;application.pEngineName="BL2 legacy Vulkan";application.engineVersion=1;application.apiVersion=VK_API_VERSION_1_1;
        const char* instanceExtensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance.pApplicationInfo=&application;
        instance.enabledExtensionCount=2;instance.ppEnabledExtensionNames=instanceExtensions;
        const char* validationLayer="VK_LAYER_KHRONOS_validation";const char* validation=std::getenv("BL2_VK_VALIDATION");
        std::array<char,PROP_VALUE_MAX> validationProperty{};__system_property_get("debug.bl2.vk_validation",validationProperty.data());
        if((validation&&std::strcmp(validation,"1")==0)||std::strcmp(validationProperty.data(),"1")==0){
            std::uint32_t count=0;Check(vkEnumerateInstanceLayerProperties(&count,nullptr),"validation layer count");std::vector<VkLayerProperties> layers(count);Check(vkEnumerateInstanceLayerProperties(&count,layers.data()),"validation layer enumeration");
            const bool available=std::any_of(layers.begin(),layers.end(),[&](const auto& layer){return std::strcmp(layer.layerName,validationLayer)==0;});
            if(available){instance.enabledLayerCount=1;instance.ppEnabledLayerNames=&validationLayer;}
            __android_log_print(available?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"BL2Vulkan","Validation layer opt-in: available=%d enabled=%u",available,instance.enabledLayerCount);
        }
        Check(vkCreateInstance(&instance,nullptr,&gpu.instance),"vkCreateInstance");
        std::uint32_t n=0;Check(vkEnumeratePhysicalDevices(gpu.instance,&n,nullptr),"physical device count");
        std::vector<VkPhysicalDevice> devices(n);Check(vkEnumeratePhysicalDevices(gpu.instance,&n,devices.data()),"physical devices");
        Require(!devices.empty(),GL_INVALID_OPERATION);gpu.physical=devices[0];
        vkGetPhysicalDeviceProperties(gpu.physical,&gpu.properties);Require(gpu.memoryBudget.Configure(gpu.properties.limits.maxMemoryAllocationCount),GL_OUT_OF_MEMORY);vkGetPhysicalDeviceFeatures(gpu.physical,&gpu.features);vkGetPhysicalDeviceMemoryProperties(gpu.physical,&gpu.memory);
        gpu.extensions.clear();gpu.extensionString.clear();
        if(gpu.properties.apiVersion<VK_API_VERSION_1_1||gpu.properties.limits.maxUniformBufferRange<65536||gpu.properties.limits.maxDescriptorSetSamplers<32)
            Fail("Vulkan device lacks required Vulkan 1.1 / 64 KiB stage constants / 32 samplers");
        vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical,&n,families.data());
        bool found=false;for(std::uint32_t i=0;i<n;++i)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){gpu.family=i;found=true;break;}
        Require(found,GL_INVALID_OPERATION);
        GLfloat priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex=gpu.family;queue.queueCount=1;queue.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures enabled{};enabled.samplerAnisotropy=gpu.features.samplerAnisotropy;enabled.depthClamp=gpu.features.depthClamp;
        enabled.depthBounds=gpu.features.depthBounds;enabled.geometryShader=gpu.features.geometryShader;enabled.occlusionQueryPrecise=gpu.features.occlusionQueryPrecise;
        enabled.textureCompressionETC2=gpu.features.textureCompressionETC2;enabled.textureCompressionASTC_LDR=gpu.features.textureCompressionASTC_LDR;
        const char* extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queue;
        device.enabledExtensionCount=1;device.ppEnabledExtensionNames=&extension;device.pEnabledFeatures=&enabled;
        Check(vkCreateDevice(gpu.physical,&device,nullptr,&gpu.device),"vkCreateDevice");
        gpu.getBufferMemoryRequirements2=reinterpret_cast<PFN_vkGetBufferMemoryRequirements2>(vkGetDeviceProcAddr(gpu.device,"vkGetBufferMemoryRequirements2"));
        if(!gpu.getBufferMemoryRequirements2)Fail("vkGetBufferMemoryRequirements2 unavailable for Vulkan 1.1");vkGetDeviceQueue(gpu.device,gpu.family,0,&gpu.queue);
        std::array<VkDescriptorSetLayoutBinding,34> bindings{};
        for(std::uint32_t i=0;i<bindings.size();++i) {
            bindings[i].binding=i;bindings[i].descriptorCount=1;
            bindings[i].descriptorType=i<2?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].stageFlags=i==0||(i>=2&&i<18)?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptors.bindingCount=bindings.size();descriptors.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(gpu.device,&descriptors,nullptr,&gpu.descriptors),"vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&gpu.descriptors;
        Check(vkCreatePipelineLayout(gpu.device,&layout,nullptr,&gpu.pipelineLayout),"vkCreatePipelineLayout");
        gpu.identity="bl2vk-v1-" BL2_VULKAN_BACKEND_ID "-"+std::to_string(gpu.properties.vendorID)+"-"+std::to_string(gpu.properties.deviceID)+"-"+std::to_string(gpu.properties.driverVersion)+"-"+Hex(gpu.pack->Identity());
        for(auto byte:gpu.properties.pipelineCacheUUID){char hex[3];std::snprintf(hex,sizeof(hex),"%02x",byte);gpu.identity+=hex;}
        auto data=DriverCache(gpu.cacheDirectory+"/"+gpu.identity+".cache");
        VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};cache.initialDataSize=data.size();cache.pInitialData=data.data();
        auto cacheResult=vkCreatePipelineCache(gpu.device,&cache,nullptr,&gpu.pipelineCache);
        if(cacheResult!=VK_SUCCESS&&!data.empty()){cache.initialDataSize=0;cache.pInitialData=nullptr;cacheResult=vkCreatePipelineCache(gpu.device,&cache,nullptr,&gpu.pipelineCache);}
        Check(cacheResult,"vkCreatePipelineCache");
        if(mandatory)PrewarmMandatory(*mandatory);
        Prewarm();initializationComplete=true;result=true;
        __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Startup PSO receipt: mandatory=%zu resident=%zu startupCreates=%llu ready=%u runtimeCreates=%llu lateMandatory=%llu",mandatoryKeys.size(),pipelines.size(),static_cast<unsigned long long>(startupCreates),mandatoryReady,static_cast<unsigned long long>(runtimeCreates),static_cast<unsigned long long>(lateMandatoryCreates));
        __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Initialized native ARM32 %s Vulkan=%u driver=%u cachedPipelines=%zu",gpu.properties.deviceName,gpu.properties.apiVersion,gpu.properties.driverVersion,pipelines.size());
    });return result;
}
PipelineStats GetPipelineStats(){std::lock_guard lock(mutex);return {startupCreates,runtimeCreates,lateMandatoryCreates,successfulDraws,rejectedDraws,pipelines.size(),mandatoryKeys.size(),CandidateCount()};}
bool IsInitialized(){std::lock_guard lock(mutex);return gpu.device&&!gpu.failed;}
void EnableFatalDispatch(){std::lock_guard lock(mutex);fatalDispatch=true;}
const char* LastFailure(){return gpu.failure.c_str();}
std::uintptr_t CreateContext(std::uintptr_t share) {
    std::uintptr_t out=0;Call([&] {
        Require(gpu.device&&!gpu.failed,GL_INVALID_OPERATION);auto context=std::make_unique<Context>();
        if(share){auto it=contexts.find(share);Require(it!=contexts.end(),GL_INVALID_VALUE);context->share=it->second->share;}
        else context->share=std::make_shared<ShareGroup>();
        for(auto& frame:context->frames)frame=NewFrame();out=nextContext++;contexts.emplace(out,std::move(context));
    });return out;
}
bool MakeCurrent(std::uintptr_t context,ANativeWindow* window,std::uint32_t width,std::uint32_t height,std::uintptr_t pbuffer) {
    bool result=false;Call([&] {
        if(!context){if(current){if(current->frames[current->frameIndex]->recording)Submit();if(current->window){auto it=windows.find(current->window);if(it!=windows.end()&&it->second.owner==current)it->second.owner=nullptr;}if(current->pbuffer){auto it=pbuffers.find(current->pbuffer);if(it!=pbuffers.end()&&it->second.owner==current)it->second.owner=nullptr;}current->owned=false;current=nullptr;SaveCaches();}result=true;return;}
        auto it=contexts.find(context);Require(it!=contexts.end(),GL_INVALID_VALUE);auto& next=*it->second;
        Require(!next.owned||next.owner==std::this_thread::get_id(),GL_INVALID_OPERATION);
        if(window){auto& surface=Surface(window);Require(!surface.owner||surface.owner==&next||surface.owner==current,GL_INVALID_OPERATION);}
        if(!window&&pbuffer){auto& surface=pbuffers[pbuffer];Require(!surface.owner||surface.owner==&next||surface.owner==current,GL_INVALID_OPERATION);}
        if(current){if(current!=&next&&current->frames[current->frameIndex]->recording)Submit();if(current->window){auto it=windows.find(current->window);if(it!=windows.end()&&it->second.owner==current)it->second.owner=nullptr;}if(current->pbuffer){auto it=pbuffers.find(current->pbuffer);if(it!=pbuffers.end()&&it->second.owner==current)it->second.owner=nullptr;}if(current!=&next)current->owned=false;}
        current=&next;next.owned=true;next.owner=std::this_thread::get_id();next.window=window;next.pbuffer=window?0:pbuffer;
        if(window){auto& surface=Surface(window);surface.owner=&next;next.defaultColor=surface.color;next.defaultDepth=surface.depth;DefaultTarget(next,surface.extent.width,surface.extent.height);surface.color=next.defaultColor;surface.depth=next.defaultDepth;}
        else if(pbuffer){auto& surface=pbuffers[pbuffer];surface.owner=&next;next.defaultColor=surface.color;next.defaultDepth=surface.depth;DefaultTarget(next,std::max(width,1u),std::max(height,1u));surface.color=next.defaultColor;surface.depth=next.defaultDepth;}
        else DefaultTarget(next,std::max(width,1u),std::max(height,1u));
        result=true;
    });return result;
}
bool DestroyContext(std::uintptr_t context) {
    bool result=false;Call([&] {
        auto it=contexts.find(context);Require(it!=contexts.end(),GL_INVALID_VALUE);auto& c=*it->second;
        Require(!c.owned||c.owner==std::this_thread::get_id(),GL_INVALID_OPERATION);
        auto* previous=current;current=&c;if(c.frames[c.frameIndex]->recording)Submit(true);
        for(auto& f:c.frames)if(f->submitted)Check(vkWaitForFences(gpu.device,1,&f->fence,VK_TRUE,UINT64_MAX),"destroy context fence");
        if(c.window){auto w=windows.find(c.window);if(w!=windows.end()&&w->second.owner==&c)w->second.owner=nullptr;}
        if(c.pbuffer){auto p=pbuffers.find(c.pbuffer);if(p!=pbuffers.end()&&p->second.owner==&c)p->second.owner=nullptr;}
        current=previous==&c?nullptr:previous;contexts.erase(it);SaveCaches();result=true;
    });return result;
}
void ReleaseWindow(ANativeWindow* window) { Call([&] {
    auto it=windows.find(window);if(it==windows.end())return;DropSwapchain(it->second);
    vkDestroySurfaceKHR(gpu.instance,it->second.surface,nullptr);ANativeWindow_release(window);windows.erase(it);SaveCaches();
    for(auto& [id,c]:contexts)if(c->window==window)c->window=nullptr;
});}
void ReleasePbuffer(std::uintptr_t id){Call([&]{auto it=pbuffers.find(id);if(it==pbuffers.end())return;Require(!it->second.owner||!it->second.owner->owned,GL_INVALID_OPERATION);pbuffers.erase(it);});}
void SwapInterval(int interval){Call([&]{Require(interval>=0&&interval<=1);swapInterval=interval;});}
void Flush(){Call([&]{if(C().frames[C().frameIndex]->recording)Submit();});}
void Finish(){Call([&]{if(C().frames[C().frameIndex]->recording)Submit();Check(vkQueueWaitIdle(gpu.queue),"Finish");SaveCaches();});}
void PersistCaches(){Call([&]{SaveCaches();});}
// Resume retains WSI; only the actual Android surface callback authorizes reconnection.
void SetPresentationPaused(bool paused){Call([&]{if(presentationPaused==paused)return;presentationPaused=paused;__android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Presentation lifecycle: paused=%u generation=%llu windows=%zu",paused,static_cast<unsigned long long>(presentationGeneration),windows.size());});}
void NotifyPresentationSurfaceChanged(ANativeWindow* fresh){Call([&]{++presentationGeneration;for(auto& [native,window]:windows)MarkPresentationSurface(window,fresh);__android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Presentation surface notification: generation=%llu paused=%u windows=%zu fresh=%p",static_cast<unsigned long long>(presentationGeneration),presentationPaused,windows.size(),fresh);});}
GLenum GetError(){std::lock_guard lock(mutex);auto& error=current?current->error:orphanError;auto out=error;error=GL_NO_ERROR;return out;}
void RecordError(GLenum error){std::lock_guard lock(mutex);Error(error);}
void Unsupported(const char* operation){Call([&]{Fail(std::string("Unsupported direct Vulkan operation: ")+operation);});}
void Shutdown(){std::lock_guard lock(mutex);
    if(gpu.device) {
        for(auto& [id,c]:contexts) {current=c.get();if(c->frames[c->frameIndex]->recording)try{Submit();}catch(...){} }
        vkDeviceWaitIdle(gpu.device);SaveCaches();current=nullptr;recordingOwner=nullptr;contexts.clear();pbuffers.clear();
        for(auto& [native,w]:windows){for(auto s:w.ready)vkDestroySemaphore(gpu.device,s,nullptr);if(w.swapchain)vkDestroySwapchainKHR(gpu.device,w.swapchain,nullptr);if(w.surface)vkDestroySurfaceKHR(gpu.instance,w.surface,nullptr);ANativeWindow_release(native);}windows.clear();
        for(auto* sync:syncs){vkDestroyFence(gpu.device,sync->fence,nullptr);delete sync;}syncs.clear();
        shaders.clear();samplers.clear();
        pipelines.clear();
        for(auto& [key,p]:renderPasses)vkDestroyRenderPass(gpu.device,p,nullptr);renderPasses.clear();
        if(gpu.pipelineCache)vkDestroyPipelineCache(gpu.device,gpu.pipelineCache,nullptr);
        if(gpu.pipelineLayout)vkDestroyPipelineLayout(gpu.device,gpu.pipelineLayout,nullptr);
        if(gpu.descriptors)vkDestroyDescriptorSetLayout(gpu.device,gpu.descriptors,nullptr);
        smallBufferPages.clear();
        __android_log_print(gpu.memoryBudget.Live()?ANDROID_LOG_ERROR:ANDROID_LOG_INFO,"BL2Vulkan",
            "Device memory receipt: live=%u peak=%u limit=%u",gpu.memoryBudget.Live(),gpu.memoryBudget.Peak(),gpu.memoryBudget.Limit());
        vkDestroyDevice(gpu.device,nullptr);
    }
    if(gpu.instance)vkDestroyInstance(gpu.instance,nullptr);ResetPrewarmState();auto failure=gpu.failure;gpu=Device{};gpu.failure=std::move(failure);
}

namespace {
std::shared_ptr<Buffer> BoundBuffer(GLenum target) {
    if(target==GL_ELEMENT_ARRAY_BUFFER){Require(Vao().elements!=nullptr,GL_INVALID_OPERATION);return Vao().elements;}
    auto it=C().boundBuffers.find(target);Require(it!=C().boundBuffers.end()&&it->second.second,GL_INVALID_OPERATION);return it->second.second;
}
bool ValidBufferTarget(GLenum target) {return target==GL_ARRAY_BUFFER||target==GL_ELEMENT_ARRAY_BUFFER||target==GL_PIXEL_UNPACK_BUFFER||target==GL_PIXEL_PACK_BUFFER||target==GL_COPY_READ_BUFFER||target==GL_COPY_WRITE_BUFFER||target==GL_UNIFORM_BUFFER;}
bool Range(std::size_t total,GLintptr offset,GLsizeiptr size) {return offset>=0&&size>=0&&static_cast<std::size_t>(offset)<=total&&static_cast<std::size_t>(size)<=total-static_cast<std::size_t>(offset);}
std::shared_ptr<Allocation> BufferGpu(const std::shared_ptr<Buffer>& buffer) {
    Require(!buffer->bytes.empty(),GL_INVALID_OPERATION);
    const bool coherent=buffer->mapped&&(buffer->access&GL_MAP_COHERENT_BIT)&&buffer->snapshotEpoch!=drawEpoch;
    const bool changed=coherent&&buffer->allocation&&std::memcmp(static_cast<std::uint8_t*>(buffer->allocation->mapped)+buffer->mapOffset,buffer->bytes.data()+buffer->mapOffset,buffer->mapLength)!=0;
    if(buffer->dirty||changed||!buffer->allocation) {
        if(!buffer->allocation||buffer->allocation.use_count()>1||buffer->allocation->size!=buffer->bytes.size())
            buffer->allocation=Allocate(buffer->bytes.size(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memcpy(buffer->allocation->mapped,buffer->bytes.data(),buffer->bytes.size());buffer->dirty=false;
    }
    buffer->snapshotEpoch=drawEpoch;
    Retain(buffer->allocation);return buffer->allocation;
}
Slice BufferRangeGpu(const std::shared_ptr<Buffer>& buffer,std::size_t offset,std::size_t bytes) {
    Require(offset<=buffer->bytes.size()&&bytes<=buffer->bytes.size()-offset,GL_INVALID_OPERATION);
    if(buffer->mapped&&(buffer->access&GL_MAP_COHERENT_BIT)) {
        auto slice=Upload(bytes,4);std::memcpy(slice.data,buffer->bytes.data()+offset,bytes);
        // The former full snapshot stays stale, including after an explicit-flush unmap.
        buffer->dirty=true;return slice;
    }
    auto allocation=BufferGpu(buffer);return {allocation,offset,bytes,static_cast<std::uint8_t*>(allocation->mapped)+offset};
}
template<class M> void Generate(GLsizei count,GLuint* names,GLuint& next,M& map) {
    Require(count>=0&&count<=1048576&&(!count||names));
    for(GLsizei i=0;i<count;++i){while(!next||map.contains(next))++next;names[i]=next++;map.try_emplace(names[i]);}
}
}
void GenBuffers(GLsizei count,GLuint* names){Call([&]{auto& s=*C().share;Generate(count,names,s.nextBuffer,s.buffers);for(GLsizei i=0;i<count;++i)s.buffers[names[i]]=std::make_shared<Buffer>();});}
void DeleteBuffers(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));auto& c=C();for(GLsizei i=0;i<count;++i){if(!names[i])continue;c.share->buffers.erase(names[i]);for(auto& [target,b]:c.boundBuffers)if(b.first==names[i])b={0,{}};for(auto& [id,v]:c.vaos){if(v.elementName==names[i]){v.elementName=0;v.elements.reset();}for(auto& a:v.attributes)if(a.name==names[i]){a.name=0;a.buffer.reset();}}}});}
void BindBuffer(GLenum target,GLuint name){Call([&]{Require(ValidBufferTarget(target),GL_INVALID_ENUM);auto& c=C();std::shared_ptr<Buffer> buffer;if(name){auto& value=c.share->buffers[name];if(!value)value=std::make_shared<Buffer>();buffer=value;}if(target==GL_ELEMENT_ARRAY_BUFFER){Vao().elementName=name;Vao().elements=buffer;}else c.boundBuffers[target]={name,buffer};});}
GLboolean IsBuffer(GLuint name){GLboolean out=GL_FALSE;Call([&]{out=name&&C().share->buffers.contains(name);});return out;}
void BufferData(GLenum target,GLsizeiptr size,const void* data,GLenum usage){Call([&]{Require(size>=0&&static_cast<std::size_t>(size)<=kMaxBuffer);auto b=BoundBuffer(target);Require(!b->immutable&&!b->mapped,GL_INVALID_OPERATION);b->bytes.resize(size);if(data&&size)std::memcpy(b->bytes.data(),data,size);else std::fill(b->bytes.begin(),b->bytes.end(),0);b->allocation.reset();b->dirty=true;b->usage=usage;});}
void BufferStorage(GLenum target,GLsizeiptr size,const void* data,GLbitfield flags){Call([&]{Require(size>0&&static_cast<std::size_t>(size)<=kMaxBuffer);Require(!(flags&~(GL_MAP_READ_BIT|GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT|GL_DYNAMIC_STORAGE_BIT|GL_CLIENT_STORAGE_BIT)));Require(!(flags&GL_MAP_COHERENT_BIT)||(flags&GL_MAP_PERSISTENT_BIT));Require(!(flags&GL_MAP_PERSISTENT_BIT)||(flags&(GL_MAP_READ_BIT|GL_MAP_WRITE_BIT)));auto b=BoundBuffer(target);Require(!b->immutable&&!b->mapped,GL_INVALID_OPERATION);b->bytes.resize(size);if(data)std::memcpy(b->bytes.data(),data,size);else std::fill(b->bytes.begin(),b->bytes.end(),0);b->allocation.reset();b->dirty=true;b->immutable=true;b->storageFlags=flags;});}
void BufferSubData(GLenum target,GLintptr offset,GLsizeiptr size,const void* data){Call([&]{auto b=BoundBuffer(target);Require(Range(b->bytes.size(),offset,size)&&(!size||data));Require(!b->immutable||(b->storageFlags&GL_DYNAMIC_STORAGE_BIT),GL_INVALID_OPERATION);Require(!b->mapped||(b->access&GL_MAP_PERSISTENT_BIT),GL_INVALID_OPERATION);if(size)std::memcpy(b->bytes.data()+offset,data,size);b->dirty=true;});}
void CopyBufferSubData(GLenum read,GLenum write,GLintptr ro,GLintptr wo,GLsizeiptr size){Call([&]{auto r=BoundBuffer(read),w=BoundBuffer(write);Require(Range(r->bytes.size(),ro,size)&&Range(w->bytes.size(),wo,size));Require(r!=w||size==0||ro+size<=wo||wo+size<=ro);if(size)std::memcpy(w->bytes.data()+wo,r->bytes.data()+ro,size);w->dirty=true;});}
void* MapBufferRange(GLenum target,GLintptr offset,GLsizeiptr length,GLbitfield access){void* out=nullptr;Call([&]{auto b=BoundBuffer(target);Require(length>0&&Range(b->bytes.size(),offset,length));Require(!b->mapped,GL_INVALID_OPERATION);Require((access&(GL_MAP_READ_BIT|GL_MAP_WRITE_BIT))!=0&&!(access&~(GL_MAP_READ_BIT|GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT|GL_MAP_FLUSH_EXPLICIT_BIT|GL_MAP_UNSYNCHRONIZED_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT)));Require(!(access&GL_MAP_READ_BIT)||!(access&(GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT|GL_MAP_UNSYNCHRONIZED_BIT)));Require(!(access&GL_MAP_FLUSH_EXPLICIT_BIT)||(access&GL_MAP_WRITE_BIT));Require(!(access&GL_MAP_COHERENT_BIT)||(access&GL_MAP_PERSISTENT_BIT));Require(!(access&GL_MAP_PERSISTENT_BIT)||b->immutable,GL_INVALID_OPERATION);if(b->immutable)Require((access&(GL_MAP_READ_BIT|GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT))==(access&b->storageFlags&(GL_MAP_READ_BIT|GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT)),GL_INVALID_OPERATION);b->mapped=true;b->mapOffset=offset;b->mapLength=length;b->access=access;if(access&GL_MAP_COHERENT_BIT)out=b->bytes.data()+offset;else{b->mapping.assign(b->bytes.begin()+offset,b->bytes.begin()+offset+length);out=b->mapping.data();}});return out;}
void* MapBuffer(GLenum target,GLenum access){void* out=nullptr;Call([&]{auto b=BoundBuffer(target);GLbitfield flags=access==0x88B8?GL_MAP_READ_BIT:access==0x88B9?GL_MAP_WRITE_BIT:access==0x88BA?GL_MAP_READ_BIT|GL_MAP_WRITE_BIT:0;Require(flags!=0,GL_INVALID_ENUM);out=MapBufferRange(target,0,b->bytes.size(),flags);});return out;}
GLboolean UnmapBuffer(GLenum target){GLboolean out=GL_FALSE;Call([&]{auto b=BoundBuffer(target);Require(b->mapped,GL_INVALID_OPERATION);if((b->access&GL_MAP_WRITE_BIT)&&!(b->access&GL_MAP_FLUSH_EXPLICIT_BIT)){if(!(b->access&GL_MAP_COHERENT_BIT))std::memcpy(b->bytes.data()+b->mapOffset,b->mapping.data(),b->mapLength);b->dirty=true;}b->mapped=false;b->access=0;b->mapOffset=b->mapLength=0;b->mapping.clear();out=GL_TRUE;});return out;}
void FlushMappedBufferRange(GLenum target,GLintptr offset,GLsizeiptr size){Call([&]{auto b=BoundBuffer(target);Require(b->mapped&&(b->access&GL_MAP_WRITE_BIT)&&(b->access&GL_MAP_FLUSH_EXPLICIT_BIT),GL_INVALID_OPERATION);Require(Range(b->mapLength,offset,size));if(size&&!(b->access&GL_MAP_COHERENT_BIT))std::memcpy(b->bytes.data()+b->mapOffset+offset,b->mapping.data()+offset,size);b->dirty=true;});}
void GetBufferParameteriv(GLenum target,GLenum pname,GLint* output){Call([&]{Require(output);auto b=BoundBuffer(target);switch(pname){case GL_BUFFER_SIZE:*output=b->bytes.size();break;case GL_BUFFER_USAGE:*output=b->usage;break;case GL_BUFFER_MAPPED:*output=b->mapped;break;case GL_BUFFER_ACCESS_FLAGS:*output=b->access;break;case GL_BUFFER_MAP_OFFSET:*output=b->mapOffset;break;case GL_BUFFER_MAP_LENGTH:*output=b->mapLength;break;case 0x821F:*output=b->immutable;break;case 0x8220:*output=b->storageFlags;break;default:Require(false,GL_INVALID_ENUM);}});}
void GetBufferPointerv(GLenum target,GLenum pname,void** output){Call([&]{Require(output);Require(pname==GL_BUFFER_MAP_POINTER,GL_INVALID_ENUM);auto b=BoundBuffer(target);*output=b->mapped?(b->access&GL_MAP_COHERENT_BIT?b->bytes.data()+b->mapOffset:b->mapping.data()):nullptr;});}
void GetBufferSubData(GLenum target,GLintptr offset,GLsizeiptr size,void* output){Call([&]{auto b=BoundBuffer(target);Require(Range(b->bytes.size(),offset,size)&&(!size||output));if(size)std::memcpy(output,b->bytes.data()+offset,size);});}

void GenPrograms(GLsizei count,GLuint* names){Call([&]{auto& s=*C().share;Generate(count,names,s.nextProgram,s.programs);for(GLsizei i=0;i<count;++i)s.programs[names[i]]=std::make_shared<Program>();});}
void DeletePrograms(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));auto& c=C();for(GLsizei i=0;i<count;++i){c.share->programs.erase(names[i]);if(c.vertexProgram==names[i])c.vertexProgram=0;if(c.fragmentProgram==names[i])c.fragmentProgram=0;}});}
void BindProgram(GLenum target,GLuint name){Call([&]{Require(target==kVS||target==kFS,GL_INVALID_ENUM);auto& c=C();if(name){auto& p=c.share->programs[name];if(!p)p=std::make_shared<Program>();Require(!p->target||p->target==target,GL_INVALID_OPERATION);p->target=target;}(target==kVS?c.vertexProgram:c.fragmentProgram)=name;});}
bool ValidConstantGroup(const ConstantGroup& group){return group.first_register>=-1&&(group.first_register<0||(group.byte_count%16==0&&static_cast<std::uint32_t>(group.first_register)+group.byte_count/16<=4096));}
bool NamedProgramString(GLuint name,GLenum target,std::string_view raw,const ConstantGroup* groups,const std::int16_t* samplerRegisters,GLsizei length){bool out=false;Call([&]{Require(name&&groups&&samplerRegisters&&length>=0);Require(target==kVS||target==kFS,GL_INVALID_ENUM);Stage data;std::string error;if(!gpu.pack->LookupRaw(target,raw,data,&error))Fail("Unbaked legacy stage "+std::to_string(name)+": "+error);auto shader=Module(std::move(data));auto& p=C().share->programs[name];if(!p)p=std::make_shared<Program>();Require(!p->target||p->target==target,GL_INVALID_OPERATION);for(std::size_t i=0;i<9;++i)Require(ValidConstantGroup(groups[i]));for(std::size_t i=0;i<16;++i)Require(samplerRegisters[i]>=-1&&samplerRegisters[i]<4096);for(auto index:shader->stage.attribute_locations)Require(index<16,GL_INVALID_OPERATION);p->target=target;p->shader=shader;p->length=length?length:raw.size();std::copy_n(groups,9,p->groups.begin());std::copy_n(samplerRegisters,16,p->samplerRegisters.begin());if(gpu.gfxCapture){p->diagnosticLabel=ShaderLabel(raw);p->diagnosticSource.assign(raw);p->diagnosticParticleSprite=target==kVS&&ParticleSpriteSource(raw);p->diagnosticTranslucent=target==kFS&&TranslucentBasePassSource(raw);}out=true;});return out;}
void NamedProgramLocalParametersI4uiv(GLuint name,GLenum target,GLuint index,GLsizei count,const GLuint* words){Call([&]{
    Require(count>=0&&index<=4096&&static_cast<std::uint32_t>(count)<=4096-index&&(!count||words));
    auto it=C().share->programs.find(name);Require(it!=C().share->programs.end()&&it->second->target==target,GL_INVALID_OPERATION);auto& p=*it->second;
    static std::uint32_t shadowLocalReports=0;
    if(gpu.gfxCapture&&shadowLocalReports<8&&target==kFS&&p.shader&&count>0&&index<16&&index+count>14&&
       Hex(p.shader->stage.normalizedDigest)=="c4300c3556056a36ad12941743dae4febce91b26a5bbf02a0016cf80f317d03b"){
        for(GLuint row=14;row<=15&&shadowLocalReports<8;++row){
            if(row<index||row>=index+count)continue;
            const auto* incoming=words+(row-index)*4;std::array<GLuint,4> prior{};
            const bool present=p.locals.size()>=row*4+4;if(present)std::copy_n(p.locals.data()+row*4,4,prior.begin());
            __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan",
                "Shadow sampler local upload: report=%u program=%u target=0x%04x first=%u count=%d row=%u incoming={%08x,%08x,%08x,%08x} priorPresent=%u prior={%08x,%08x,%08x,%08x} localWords=%zu share=%p draw=%llu unit0=%u/%u unit1=%u/%u",
                ++shadowLocalReports,name,target,index,count,row,incoming[0],incoming[1],incoming[2],incoming[3],static_cast<unsigned>(present),prior[0],prior[1],prior[2],prior[3],p.locals.size(),static_cast<void*>(C().share.get()),static_cast<unsigned long long>(drawEpoch),
                C().textureUnits[0].contains(GL_TEXTURE_2D)?C().textureUnits[0].at(GL_TEXTURE_2D).first:0,C().samplerUnits[0],C().textureUnits[1].contains(GL_TEXTURE_2D)?C().textureUnits[1].at(GL_TEXTURE_2D).first:0,C().samplerUnits[1]);
        }
    }
    const auto end=(index+count)*4;if(p.locals.size()<end)p.locals.resize(end);if(count)std::memcpy(p.locals.data()+index*4,words,count*16);
});}
void GetProgramiv(GLenum target,GLenum pname,GLint* output){Call([&]{Require(output);Require(target==kVS||target==kFS,GL_INVALID_ENUM);auto name=target==kVS?C().vertexProgram:C().fragmentProgram;switch(pname){case 0x8677:*output=name;break;case 0x88B4:*output=4096;break;case 0x88B6:*output=1;break;case 0x8627:Require(name&&C().share->programs.contains(name),GL_INVALID_OPERATION);*output=C().share->programs.at(name)->length;break;case 0x8876:*output=0x8875;break;default:Require(false,GL_INVALID_ENUM);}});}

void GenVertexArrays(GLsizei count,GLuint* names){Call([&]{auto& c=C();Generate(count,names,c.nextVao,c.vaos);});}
void DeleteVertexArrays(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));auto& c=C();for(GLsizei i=0;i<count;++i)if(names[i]){c.vaos.erase(names[i]);if(c.vao==names[i])c.vao=0;}});}
void BindVertexArray(GLuint name){Call([&]{auto& c=C();c.vaos.try_emplace(name);c.vao=name;});}
void EnableVertexAttribArray(GLuint index){Call([&]{Require(index<16);Vao().attributes[index].enabled=true;});}
void DisableVertexAttribArray(GLuint index){Call([&]{Require(index<16);Vao().attributes[index].enabled=false;});}
void VertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void* pointer){Call([&]{Require(index<16&&size>=1&&size<=4&&stride>=0&&stride<=2048);Require(type==GL_FLOAT||type==GL_HALF_FLOAT||type==GL_BYTE||type==GL_UNSIGNED_BYTE||type==GL_SHORT||type==GL_UNSIGNED_SHORT||type==GL_INT||type==GL_UNSIGNED_INT||type==GL_FIXED||type==GL_INT_2_10_10_10_REV||type==GL_UNSIGNED_INT_2_10_10_10_REV,GL_INVALID_ENUM);auto& a=Vao().attributes[index];a.size=size;a.type=type;a.normalized=normalized;a.stride=stride;a.pointer=reinterpret_cast<std::uintptr_t>(pointer);a.integer=false;auto it=C().boundBuffers.find(GL_ARRAY_BUFFER);a.name=it==C().boundBuffers.end()?0:it->second.first;a.buffer=it==C().boundBuffers.end()?nullptr:it->second.second;});}
void VertexAttribIPointer(GLuint index,GLint size,GLenum type,GLsizei stride,const void* pointer){VertexAttribPointer(index,size,type,GL_FALSE,stride,pointer);Call([&]{Require(index<16);Vao().attributes[index].integer=true;});}
void VertexAttribDivisor(GLuint index,GLuint divisor){Call([&]{Require(index<16);Vao().attributes[index].divisor=divisor;});}
void VertexAttrib4fv(GLuint index,const GLfloat* value){Call([&]{Require(index<16&&value);std::copy_n(value,4,Vao().attributes[index].constant.begin());});}
void GetVertexAttribiv(GLuint index,GLenum pname,GLint* value){Call([&]{Require(index<16&&value);auto& a=Vao().attributes[index];switch(pname){case GL_VERTEX_ATTRIB_ARRAY_ENABLED:*value=a.enabled;break;case GL_VERTEX_ATTRIB_ARRAY_SIZE:*value=a.size;break;case GL_VERTEX_ATTRIB_ARRAY_STRIDE:*value=a.stride;break;case GL_VERTEX_ATTRIB_ARRAY_TYPE:*value=a.type;break;case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:*value=a.normalized;break;case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:*value=a.name;break;case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:*value=a.divisor;break;default:Require(false,GL_INVALID_ENUM);}});}
void GetVertexAttribPointerv(GLuint index,GLenum pname,void** value){Call([&]{Require(index<16&&value);Require(pname==GL_VERTEX_ATTRIB_ARRAY_POINTER,GL_INVALID_ENUM);*value=reinterpret_cast<void*>(Vao().attributes[index].pointer);});}

namespace {
GLenum TextureTarget(GLenum target){return target>=GL_TEXTURE_CUBE_MAP_POSITIVE_X&&target<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z?GL_TEXTURE_CUBE_MAP:target;}
std::uint32_t Face(GLenum target){return target>=GL_TEXTURE_CUBE_MAP_POSITIVE_X&&target<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z?target-GL_TEXTURE_CUBE_MAP_POSITIVE_X:0;}
bool ValidTextureTarget(GLenum target){return target==GL_TEXTURE_2D||target==GL_TEXTURE_CUBE_MAP||target==GL_TEXTURE_3D||target==GL_TEXTURE_2D_ARRAY;}
std::shared_ptr<Texture> BoundTexture(GLenum target){target=TextureTarget(target);Require(ValidTextureTarget(target),GL_INVALID_ENUM);auto it=C().textureUnits[C().activeTexture].find(target);Require(it!=C().textureUnits[C().activeTexture].end()&&it->second.second,GL_INVALID_OPERATION);return it->second.second;}
VkFormat Format(GLenum internal,GLenum type=GL_UNSIGNED_BYTE) {
    switch(internal) {
        case GL_ALPHA:case GL_LUMINANCE:case GL_RED:case GL_R8:return VK_FORMAT_R8_UNORM;
        case GL_LUMINANCE_ALPHA:case GL_RG:case GL_RG8:return VK_FORMAT_R8G8_UNORM;
        case GL_R8_SNORM:return VK_FORMAT_R8_SNORM;case GL_RG8_SNORM:return VK_FORMAT_R8G8_SNORM;
        case GL_RGB8_SNORM:case GL_RGBA8_SNORM:return VK_FORMAT_R8G8B8A8_SNORM;
        case 0x8F98:return VK_FORMAT_R16_SNORM;case 0x8F99:return VK_FORMAT_R16G16_SNORM;
        case 0x8F9A:case 0x8F9B:return VK_FORMAT_R16G16B16A16_SNORM;
        case GL_RGB:case GL_RGBA:return type==GL_HALF_FLOAT||type==GL_HALF_FLOAT_OES?VK_FORMAT_R16G16B16A16_SFLOAT:type==GL_FLOAT?VK_FORMAT_R32G32B32A32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
        case GL_RGB8:case GL_RGBA8:case GL_RGB565:case GL_RGBA4:case GL_RGB5_A1:return VK_FORMAT_R8G8B8A8_UNORM;
        case GL_SRGB8:case GL_SRGB8_ALPHA8:case GL_SRGB_ALPHA:return VK_FORMAT_R8G8B8A8_SRGB;
        case GL_R16F:return VK_FORMAT_R16_SFLOAT;case GL_RG16F:return VK_FORMAT_R16G16_SFLOAT;
        case GL_RGB16F:case GL_RGBA16F:return VK_FORMAT_R16G16B16A16_SFLOAT;
        case GL_R32F:return VK_FORMAT_R32_SFLOAT;case GL_RG32F:return VK_FORMAT_R32G32_SFLOAT;
        case GL_RGB32F:case GL_RGBA32F:return VK_FORMAT_R32G32B32A32_SFLOAT;
        case GL_DEPTH_COMPONENT16:return VK_FORMAT_D16_UNORM;
        case GL_DEPTH_COMPONENT24:return VK_FORMAT_X8_D24_UNORM_PACK32;
        case GL_DEPTH_COMPONENT:case GL_DEPTH_COMPONENT32F:return VK_FORMAT_D32_SFLOAT;
        case GL_DEPTH_STENCIL:case GL_DEPTH24_STENCIL8:return VK_FORMAT_D24_UNORM_S8_UINT;
        case GL_DEPTH32F_STENCIL8:return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case GL_STENCIL_INDEX8:return VK_FORMAT_S8_UINT;
        case GL_COMPRESSED_RGB8_ETC2:return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        case GL_COMPRESSED_SRGB8_ETC2:return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
        case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
        case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:return VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
        case GL_COMPRESSED_RGBA8_ETC2_EAC:return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
        case GL_COMPRESSED_R11_EAC:return VK_FORMAT_EAC_R11_UNORM_BLOCK;case GL_COMPRESSED_SIGNED_R11_EAC:return VK_FORMAT_EAC_R11_SNORM_BLOCK;
        case GL_COMPRESSED_RG11_EAC:return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;case GL_COMPRESSED_SIGNED_RG11_EAC:return VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
        case 0x8D64:return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        default:
            if(internal>=0x93B0&&internal<=0x93BD)return static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK+(internal-0x93B0)*2);
            if(internal>=0x93D0&&internal<=0x93DD)return static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_SRGB_BLOCK+(internal-0x93D0)*2);
            Fail("Unsupported Vulkan texture internal format "+std::to_string(internal));
    }
}
struct Block{std::uint32_t x,y,bytes;};
Block FormatBlock(VkFormat format) {
    if(format>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK&&format<=VK_FORMAT_ASTC_12x12_SRGB_BLOCK){
        static constexpr std::array<std::array<std::uint32_t,2>,14> dimensions{{{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}}};
        auto size=dimensions[(format-VK_FORMAT_ASTC_4x4_UNORM_BLOCK)/2];return {size[0],size[1],16};
    }
    if(Compressed(format))return {4,4,format==VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK||format==VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK||format==VK_FORMAT_EAC_R11G11_UNORM_BLOCK||format==VK_FORMAT_EAC_R11G11_SNORM_BLOCK?16u:8u};
    switch(format){case VK_FORMAT_R8_UNORM:case VK_FORMAT_R8_SNORM:case VK_FORMAT_S8_UINT:return {1,1,1};case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R8G8_SNORM:case VK_FORMAT_R16_SNORM:case VK_FORMAT_D16_UNORM:case VK_FORMAT_R16_SFLOAT:return {1,1,2};case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R16G16_SFLOAT:case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_D32_SFLOAT:case VK_FORMAT_X8_D24_UNORM_PACK32:case VK_FORMAT_D24_UNORM_S8_UINT:return {1,1,4};case VK_FORMAT_R16G16B16A16_SNORM:case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R32G32_SFLOAT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return {1,1,8};case VK_FORMAT_R32G32B32A32_SFLOAT:return {1,1,16};default:return {1,1,4};}
}
std::uint32_t SnormBytes(VkFormat format){switch(format){case VK_FORMAT_R8_SNORM:case VK_FORMAT_R8G8_SNORM:case VK_FORMAT_R8G8B8A8_SNORM:return 1;case VK_FORMAT_R16_SNORM:case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R16G16B16A16_SNORM:return 2;default:return 0;}}
bool HalfColor(VkFormat format){return format==VK_FORMAT_R16_SFLOAT||format==VK_FORMAT_R16G16_SFLOAT||format==VK_FORMAT_R16G16B16A16_SFLOAT;}
bool FloatColor(VkFormat format){return format==VK_FORMAT_R32_SFLOAT||format==VK_FORMAT_R32G32_SFLOAT||format==VK_FORMAT_R32G32B32A32_SFLOAT;}
std::uint32_t ColorComponentBytes(VkFormat format){return HalfColor(format)||SnormBytes(format)==2?2:FloatColor(format)?4:1;}
std::uint16_t Half(float value){std::uint32_t bits;std::memcpy(&bits,&value,4);auto sign=static_cast<std::uint16_t>((bits>>16)&0x8000);int exponent=((bits>>23)&255)-127+15;auto mantissa=bits&0x7fffff;if(exponent>=31)return sign|0x7c00|(mantissa?0x200:0);if(exponent<=0){if(exponent<-10)return sign;mantissa=(mantissa|0x800000)>>(1-exponent);return sign|((mantissa+0xfff+((mantissa>>13)&1))>>13);}auto rounded=mantissa+0xfff+((mantissa>>13)&1);if(rounded&0x800000){rounded=0;if(++exponent>=31)return sign|0x7c00;}return sign|(exponent<<10)|(rounded>>13);}
float Unhalf(std::uint16_t value){int exponent=(value>>10)&31,mantissa=value&1023;float f=exponent==0?std::ldexp(static_cast<float>(mantissa),-24):exponent==31?(mantissa?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity()):std::ldexp(1.f+static_cast<float>(mantissa)/1024,exponent-15);return value&0x8000?-f:f;}
std::size_t Components(GLenum format){switch(format){case GL_RED:case GL_ALPHA:case GL_LUMINANCE:case GL_DEPTH_COMPONENT:return 1;case GL_RG:case GL_LUMINANCE_ALPHA:return 2;case GL_RGB:return 3;case GL_RGBA:case GL_BGRA_EXT:return 4;case GL_DEPTH_STENCIL:return 1;default:Fail("Unsupported texture external format "+std::to_string(format));}}
std::size_t PixelBytes(GLenum format,GLenum type){switch(type){case GL_UNSIGNED_BYTE:case GL_BYTE:return Components(format);case GL_FLOAT:case GL_INT:case GL_UNSIGNED_INT:return Components(format)*4;case GL_HALF_FLOAT:case GL_HALF_FLOAT_OES:case GL_SHORT:case GL_UNSIGNED_SHORT:return Components(format)*2;case GL_UNSIGNED_SHORT_5_6_5:case GL_UNSIGNED_SHORT_4_4_4_4:case GL_UNSIGNED_SHORT_5_5_5_1:return 2;case GL_UNSIGNED_INT_24_8:return 4;case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:return 8;default:Fail("Unsupported pixel type "+std::to_string(type));}}
const std::uint8_t* SourcePixels(const void* pixels,std::size_t span,std::size_t skip){
    auto it=C().boundBuffers.find(GL_PIXEL_UNPACK_BUFFER);
    if(it!=C().boundBuffers.end()&&it->second.second){auto& b=*it->second.second;const auto offset=reinterpret_cast<std::uintptr_t>(pixels);Require(offset<=b.bytes.size()&&skip<=b.bytes.size()-offset&&span<=b.bytes.size()-offset-skip,GL_INVALID_OPERATION);return b.bytes.data()+offset+skip;}
    if(!pixels)return nullptr;Require(skip<=kMaxBuffer&&span<=kMaxBuffer-skip);return static_cast<const std::uint8_t*>(pixels)+skip;
}
std::uint64_t BaseDepth(GLenum target,GLint level,GLsizei depth){return TextureTarget(target)==GL_TEXTURE_3D?static_cast<std::uint64_t>(depth)<<level:1;}
bool MatchesMip(const Image& image,GLenum target,GLint level,VkFormat format,GLsizei width,GLsizei height,GLsizei depth,GLsizei levels){
    const bool array=TextureTarget(target)==GL_TEXTURE_2D_ARRAY,cube=TextureTarget(target)==GL_TEXTURE_CUBE_MAP,volume=TextureTarget(target)==GL_TEXTURE_3D;
    return level>=0&&static_cast<std::uint32_t>(level)<image.levels&&(!levels||image.levels==static_cast<std::uint32_t>(levels))&&image.format==format&&
        std::max(1u,image.width>>level)==static_cast<std::uint32_t>(width)&&std::max(1u,image.height>>level)==static_cast<std::uint32_t>(height)&&
        image.layers==(cube?6u:array?static_cast<std::uint32_t>(depth):1u)&&
        (!volume||std::max(1u,image.depth>>level)==static_cast<std::uint32_t>(depth))&&
        image.type==(volume?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D);
}
void TextureImage(const std::shared_ptr<Texture>& texture,GLenum target,GLint level,GLenum internal,GLenum type,GLsizei width,GLsizei height,GLsizei depth,GLsizei levels=0){
    Require(level>=0&&level<16&&width>=0&&height>=0&&depth>0);if(!width||!height)return;
    const auto format=Format(internal,type);const bool array=TextureTarget(target)==GL_TEXTURE_2D_ARRAY,cube=TextureTarget(target)==GL_TEXTURE_CUBE_MAP;
    if(texture->image&&MatchesMip(*texture->image,target,level,format,width,height,depth,levels)){texture->internal=internal;return;}
    const auto baseWidth=static_cast<std::uint64_t>(width)<<level,baseHeight=static_cast<std::uint64_t>(height)<<level,baseDepth=BaseDepth(target,level,depth);
    Require(baseWidth<=gpu.properties.limits.maxImageDimension2D&&baseHeight<=gpu.properties.limits.maxImageDimension2D&&baseDepth<=gpu.properties.limits.maxImageDimension3D);
    const auto mipLevels=levels?levels:Mips(baseWidth,baseHeight,baseDepth),layers=cube?6u:array?static_cast<std::uint32_t>(depth):1u;
    if(!texture->image||texture->image->format!=format||texture->image->width!=baseWidth||texture->image->height!=baseHeight||texture->image->depth!=baseDepth||texture->image->layers!=layers){
        Require(!texture->immutable,GL_INVALID_OPERATION);texture->image=NewImage(format,baseWidth,baseHeight,baseDepth,mipLevels,layers,cube,TextureTarget(target)==GL_TEXTURE_3D);texture->sampled.reset();texture->attachmentViews.clear();
    }
    texture->internal=internal;
}
std::array<float,4> DecodePixel(const std::uint8_t* p,GLenum format,GLenum type){
    std::array<float,4> out{0,0,0,1};auto n=Components(format);
    if(type==GL_UNSIGNED_SHORT_5_6_5||type==GL_UNSIGNED_SHORT_4_4_4_4||type==GL_UNSIGNED_SHORT_5_5_5_1){std::uint16_t v;std::memcpy(&v,p,2);if(type==GL_UNSIGNED_SHORT_5_6_5)out={float(v>>11)/31,float((v>>5)&63)/63,float(v&31)/31,1};else if(type==GL_UNSIGNED_SHORT_4_4_4_4)out={float(v>>12)/15,float((v>>8)&15)/15,float((v>>4)&15)/15,float(v&15)/15};else out={float(v>>11)/31,float((v>>6)&31)/31,float((v>>1)&31)/31,float(v&1)};}
    else for(std::size_t i=0;i<n;++i){if(type==GL_FLOAT)std::memcpy(&out[i],p+i*4,4);else if(type==GL_HALF_FLOAT||type==GL_HALF_FLOAT_OES){std::uint16_t v;std::memcpy(&v,p+i*2,2);out[i]=Unhalf(v);}else if(type==GL_BYTE){std::int8_t v;std::memcpy(&v,p+i,1);out[i]=std::max(-1.f,v/127.f);}else if(type==GL_SHORT){std::int16_t v;std::memcpy(&v,p+i*2,2);out[i]=std::max(-1.f,v/32767.f);}else if(type==GL_INT){std::int32_t v;std::memcpy(&v,p+i*4,4);out[i]=std::max(-1.,double(v)/2147483647.);}else if(type==GL_UNSIGNED_SHORT){std::uint16_t v;std::memcpy(&v,p+i*2,2);out[i]=float(v)/65535;}else if(type==GL_UNSIGNED_INT){std::uint32_t v;std::memcpy(&v,p+i*4,4);out[i]=double(v)/4294967295.;}else out[i]=p[i]/255.f;}
    if(format==GL_BGRA_EXT)std::swap(out[0],out[2]);return out;
}
void EncodeColor(std::uint8_t* output,VkFormat format,const std::array<float,4>& values){
    const auto bytes=ColorComponentBytes(format),channels=FormatBlock(format).bytes/bytes;Require(channels<=4,GL_INVALID_OPERATION);
    for(std::size_t k=0;k<channels;++k){if(HalfColor(format)){const auto v=Half(values[k]);std::memcpy(output+k*2,&v,2);}else if(FloatColor(format))std::memcpy(output+k*4,&values[k],4);else if(SnormBytes(format)==1){const auto v=static_cast<std::int8_t>(std::lround(std::clamp(values[k],-1.f,1.f)*127));std::memcpy(output+k,&v,1);}else if(SnormBytes(format)==2){const auto v=static_cast<std::int16_t>(std::lround(std::clamp(values[k],-1.f,1.f)*32767));std::memcpy(output+k*2,&v,2);}else output[k]=std::clamp<int>(std::lround(std::clamp(values[k],0.f,1.f)*255),0,255);}
}
void StoreColor(std::uint8_t* output,VkFormat destination,const std::uint8_t* source,GLenum format,GLenum type){
    const auto signedBytes=SnormBytes(destination),componentBytes=ColorComponentBytes(destination),channels=FormatBlock(destination).bytes/componentBytes;Require(channels<=4,GL_INVALID_OPERATION);
    if((signedBytes==1&&type==GL_BYTE)||(signedBytes==2&&type==GL_SHORT)){
        const auto sourceChannels=Components(format);for(std::size_t k=0;k<channels;++k){const auto sourceChannel=format==GL_BGRA_EXT&&k<3?2-k:k;if(sourceChannel<sourceChannels)std::memcpy(output+k*signedBytes,source+sourceChannel*signedBytes,signedBytes);else if(signedBytes==1){const std::int8_t v=k==3?127:0;std::memcpy(output+k,&v,1);}else{const std::int16_t v=k==3?32767:0;std::memcpy(output+k*2,&v,2);}}return;
    }
    EncodeColor(output,destination,DecodePixel(source,format,type));
}
std::array<float,4> DecodeColor(const std::uint8_t* input,VkFormat format){
    std::array<float,4> values{0,0,0,1};const auto bytes=ColorComponentBytes(format),channels=FormatBlock(format).bytes/bytes;Require(channels<=4,GL_INVALID_OPERATION);
    for(std::size_t k=0;k<channels;++k){if(HalfColor(format)){std::uint16_t v;std::memcpy(&v,input+k*2,2);values[k]=Unhalf(v);}else if(FloatColor(format))std::memcpy(&values[k],input+k*4,4);else if(SnormBytes(format)==1){std::int8_t v;std::memcpy(&v,input+k,1);values[k]=std::max(-1.f,v/127.f);}else if(SnormBytes(format)==2){std::int16_t v;std::memcpy(&v,input+k*2,2);values[k]=std::max(-1.f,v/32767.f);}else values[k]=input[k]/255.f;}return values;
}
void UploadImage(const std::shared_ptr<Texture>& texture,GLenum target,GLint level,GLint x,GLint y,GLint z,GLsizei width,GLsizei height,GLsizei depth,GLenum format,GLenum type,const void* pixels,GLsizei compressedSize=-1){
    auto image=texture->image;Require(image&&level>=0&&static_cast<std::uint32_t>(level)<image->levels&&x>=0&&y>=0&&z>=0&&width>=0&&height>=0&&depth>=0,GL_INVALID_OPERATION);
    const auto iw=std::max(1u,image->width>>level),ih=std::max(1u,image->height>>level),id=image->type==VK_IMAGE_TYPE_3D?std::max(1u,image->depth>>level):TextureTarget(target)==GL_TEXTURE_2D_ARRAY?image->layers:1u;
    Require(static_cast<std::uint32_t>(x)<=iw&&static_cast<std::uint32_t>(width)<=iw-x&&static_cast<std::uint32_t>(y)<=ih&&static_cast<std::uint32_t>(height)<=ih-y&&static_cast<std::uint32_t>(z)<=id&&static_cast<std::uint32_t>(depth)<=id-z);
    if(!width||!height||!depth)return;
    const auto block=FormatBlock(image->format);const auto rowBytes=static_cast<std::uint64_t>((width+block.x-1)/block.x)*block.bytes;
    const auto rows=static_cast<std::uint64_t>((height+block.y-1)/block.y),bytes=rowBytes*rows*depth;
    Require(bytes<=kMaxBuffer,GL_OUT_OF_MEMORY);
    const std::uint8_t* source=nullptr;std::uint64_t rowStride=0,imageStride=0;std::size_t pixelBytes=0;
    if(compressedSize>=0){Require(Compressed(image->format)&&static_cast<std::size_t>(compressedSize)==bytes);Require(x%block.x==0&&y%block.y==0&&(width%block.x==0||x+width==static_cast<GLint>(iw))&&(height%block.y==0||y+height==static_cast<GLint>(ih)));source=SourcePixels(pixels,bytes,0);}
    else {Require(!Compressed(image->format),GL_INVALID_OPERATION);auto& c=C();pixelBytes=PixelBytes(format,type);const auto sourceRow=static_cast<std::uint64_t>(c.unpackRow?c.unpackRow:width)*pixelBytes;Require(sourceRow<=kMaxBuffer);rowStride=(sourceRow+c.unpackAlignment-1)/c.unpackAlignment*c.unpackAlignment;imageStride=static_cast<std::uint64_t>(c.unpackImage?c.unpackImage:height)*rowStride;const auto skip=static_cast<std::uint64_t>(c.unpackSkipImages)*imageStride+static_cast<std::uint64_t>(c.unpackSkipRows)*rowStride+static_cast<std::uint64_t>(c.unpackSkipPixels)*pixelBytes;const auto span=static_cast<std::uint64_t>(depth-1)*imageStride+static_cast<std::uint64_t>(height-1)*rowStride+static_cast<std::size_t>(width)*pixelBytes;Require(span<=kMaxBuffer&&skip<=kMaxBuffer-span);source=SourcePixels(pixels,span,skip);}
    if(!source)return;
    auto slice=Upload(bytes,16);auto* dest=static_cast<std::uint8_t*>(slice.data);
    std::vector<std::uint8_t> stencil;
    if(compressedSize>=0)std::memcpy(dest,source,bytes);
    else if(image->aspect&(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if(image->aspect&VK_IMAGE_ASPECT_STENCIL_BIT)stencil.resize(static_cast<std::size_t>(width)*height*depth);
        for(GLint zi=0;zi<depth;++zi)for(GLint yi=0;yi<height;++yi)for(GLint xi=0;xi<width;++xi){const auto index=(static_cast<std::size_t>(zi)*height+yi)*width+xi;const auto* p=source+zi*imageStride+yi*rowStride+xi*pixelBytes;
            if(type==GL_UNSIGNED_INT_24_8){std::uint32_t packed;std::memcpy(&packed,p,4);const auto value=packed>>8;std::memcpy(dest+index*4,&value,4);if(!stencil.empty())stencil[index]=packed&255;}
            else {const auto f=DecodePixel(p,format,type)[0];if(image->format==VK_FORMAT_D16_UNORM){auto value=static_cast<std::uint16_t>(std::clamp(f,0.f,1.f)*65535.f+.5f);std::memcpy(dest+index*2,&value,2);}else if(image->format==VK_FORMAT_X8_D24_UNORM_PACK32){auto value=static_cast<std::uint32_t>(std::clamp(f,0.f,1.f)*16777215.f+.5f);std::memcpy(dest+index*4,&value,4);}else if(image->format==VK_FORMAT_D32_SFLOAT)std::memcpy(dest+index*4,&f,4);else Fail("Unsupported combined depth/stencil upload type");}
        }
    } else {
        for(GLint zi=0;zi<depth;++zi)for(GLint yi=0;yi<height;++yi)for(GLint xi=0;xi<width;++xi){const auto index=((static_cast<std::size_t>(zi)*height+yi)*width+xi)*block.bytes;StoreColor(dest+index,image->format,source+zi*imageStride+yi*rowStride+xi*pixelBytes,format,type);}
    }
    const bool array=TextureTarget(target)==GL_TEXTURE_2D_ARRAY;const auto layer=array?static_cast<std::uint32_t>(z):Face(target);
    Transition(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,level,1,layer,array?depth:1);
    VkBufferImageCopy copy{};copy.bufferOffset=slice.offset;copy.imageSubresource={image->aspect&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,static_cast<std::uint32_t>(level),layer,array?static_cast<std::uint32_t>(depth):1u};copy.imageOffset={x,y,array?0:z};copy.imageExtent={static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),array?1u:static_cast<std::uint32_t>(depth)};
    vkCmdCopyBufferToImage(Recording().command,slice.buffer->buffer,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    if(!stencil.empty()){auto st=Upload(stencil.size(),4);std::memcpy(st.data,stencil.data(),stencil.size());copy.bufferOffset=st.offset;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;vkCmdCopyBufferToImage(Recording().command,st.buffer->buffer,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);}
    if(gpu.gfxCapture)RecordColorWrite(image,level,layer,{.operation=RecordedColorOperation::Upload,.rectangle={x,y,width,height},.colorMask=15,.uploadFormat=format,.uploadType=type});
    if(gpu.gfxCapture){texture->uploadFormat=format;texture->uploadType=type;texture->uploadRegion={level,x,y,z,width,height,depth};++texture->uploadRevision;}
}
void Parameter(Sampling& s,GLenum pname,GLfloat value,const char* caller) {
    switch(pname){case GL_TEXTURE_MIN_FILTER:Require(value==GL_NEAREST||value==GL_LINEAR||value==GL_NEAREST_MIPMAP_NEAREST||value==GL_NEAREST_MIPMAP_LINEAR||value==GL_LINEAR_MIPMAP_NEAREST||value==GL_LINEAR_MIPMAP_LINEAR,GL_INVALID_ENUM);s.min=value;break;case GL_TEXTURE_MAG_FILTER:Require(value==GL_NEAREST||value==GL_LINEAR,GL_INVALID_ENUM);s.mag=value;break;
        case GL_TEXTURE_WRAP_S:case GL_TEXTURE_WRAP_T:case GL_TEXTURE_WRAP_R:Require(value==GL_REPEAT||value==GL_MIRRORED_REPEAT||value==GL_CLAMP_TO_EDGE||value==0x812D,GL_INVALID_ENUM);(pname==GL_TEXTURE_WRAP_S?s.s:pname==GL_TEXTURE_WRAP_T?s.t:s.r)=value;break;
        case GL_TEXTURE_COMPARE_MODE:Require(value==GL_NONE||value==GL_COMPARE_REF_TO_TEXTURE,GL_INVALID_ENUM);s.compare=value;break;case GL_TEXTURE_COMPARE_FUNC:Require(value>=GL_NEVER&&value<=GL_ALWAYS,GL_INVALID_ENUM);s.compareFunc=value;break;
        case GL_TEXTURE_BASE_LEVEL:Require(value>=0);s.base=value;break;case GL_TEXTURE_MAX_LEVEL:Require(value>=0);s.maximum=value;break;case GL_TEXTURE_MIN_LOD:s.minLod=value;break;case GL_TEXTURE_MAX_LOD:s.maxLod=value;break;case 0x8501:s.lodBias=value;break;
        case 0x84FE:Require(value>=1);s.aniso=std::min(value,gpu.properties.limits.maxSamplerAnisotropy);break;
        default:if(gpu.gfxCapture&&gpu.unknownParameters<16){++gpu.unknownParameters;__android_log_print(ANDROID_LOG_WARN,"BL2Vulkan","Unsupported Parameter: caller=%s pname=0x%04x value=%.9g activeUnit=%u V=%u P=%u",caller,pname,value,C().activeTexture,C().vertexProgram,C().fragmentProgram);}Require(false,GL_INVALID_ENUM);
    }
}
}
void GenTextures(GLsizei count,GLuint* names){Call([&]{auto& s=*C().share;Generate(count,names,s.nextTexture,s.textures);for(GLsizei i=0;i<count;++i)s.textures[names[i]]=std::make_shared<Texture>();});}
void DeleteTextures(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));auto& c=C();for(GLsizei i=0;i<count;++i)if(names[i]){c.share->textures.erase(names[i]);for(auto& unit:c.textureUnits)for(auto& [target,b]:unit)if(b.first==names[i])b={0,{}};for(auto& [id,f]:c.fbos){for(auto& a:f.colors)if(a.object==GL_TEXTURE&&a.name==names[i])a=Attachment{};for(auto* a:{&f.depth,&f.stencil})if(a->object==GL_TEXTURE&&a->name==names[i])*a=Attachment{};}}});}
void BindTexture(GLenum target,GLuint name){Call([&]{Require(ValidTextureTarget(target),GL_INVALID_ENUM);std::shared_ptr<Texture> texture;if(name){auto& t=C().share->textures[name];if(!t)t=std::make_shared<Texture>();Require(!t->target||t->target==target,GL_INVALID_OPERATION);t->target=target;texture=t;}C().textureUnits[C().activeTexture][target]={name,texture};});}
void BindMultiTexture(GLenum unit,GLenum target,GLuint name){Call([&]{Require(unit>=GL_TEXTURE0&&unit<GL_TEXTURE0+32,GL_INVALID_ENUM);auto old=C().activeTexture;C().activeTexture=unit-GL_TEXTURE0;BindTexture(target,name);C().activeTexture=old;});}
void ActiveTexture(GLenum unit){Call([&]{Require(unit>=GL_TEXTURE0&&unit<GL_TEXTURE0+32,GL_INVALID_ENUM);C().activeTexture=unit-GL_TEXTURE0;});}
GLboolean IsTexture(GLuint name){GLboolean out=GL_FALSE;Call([&]{out=name&&C().share->textures.contains(name);});return out;}
void TexImage2D(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const void* pixels){Call([&]{Require(border==0);auto t=BoundTexture(target);Require(!t->immutable,GL_INVALID_OPERATION);TextureImage(t,target,level,internal,type,w,h,1);if(w&&h)UploadImage(t,target,level,0,0,0,w,h,1,format,type,pixels);});}
void TexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,const void* pixels){Call([&]{UploadImage(BoundTexture(target),target,level,x,y,0,w,h,1,format,type,pixels);});}
void CompressedTexImage2D(GLenum target,GLint level,GLenum internal,GLsizei w,GLsizei h,GLint border,GLsizei size,const void* pixels){Call([&]{Require(border==0&&size>=0);auto t=BoundTexture(target);Require(!t->immutable,GL_INVALID_OPERATION);TextureImage(t,target,level,internal,GL_UNSIGNED_BYTE,w,h,1);if(w&&h)UploadImage(t,target,level,0,0,0,w,h,1,internal,GL_UNSIGNED_BYTE,pixels,size);});}
void CompressedTexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLsizei size,const void* pixels){Call([&]{auto t=BoundTexture(target);Require(t->image&&t->image->format==Format(format),GL_INVALID_OPERATION);Require(size>=0);UploadImage(t,target,level,x,y,0,w,h,1,format,GL_UNSIGNED_BYTE,pixels,size);});}
void TexStorage2D(GLenum target,GLsizei levels,GLenum internal,GLsizei w,GLsizei h){Call([&]{Require(levels>0&&w>0&&h>0&&static_cast<std::uint32_t>(levels)<=Mips(w,h));auto t=BoundTexture(target);Require(!t->immutable,GL_INVALID_OPERATION);TextureImage(t,target,0,internal,GL_UNSIGNED_BYTE,w,h,1,levels);t->immutable=true;});}
void TexImage3D(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLsizei d,GLint border,GLenum format,GLenum type,const void* pixels){Call([&]{Require(border==0&&(target==GL_TEXTURE_3D||target==GL_TEXTURE_2D_ARRAY));auto t=BoundTexture(target);Require(!t->immutable,GL_INVALID_OPERATION);TextureImage(t,target,level,internal,type,w,h,d);if(w&&h&&d)UploadImage(t,target,level,0,0,0,w,h,d,format,type,pixels);});}
void TexSubImage3D(GLenum target,GLint level,GLint x,GLint y,GLint z,GLsizei w,GLsizei h,GLsizei d,GLenum format,GLenum type,const void* pixels){Call([&]{UploadImage(BoundTexture(target),target,level,x,y,z,w,h,d,format,type,pixels);});}
void CompressedTexImage3D(GLenum target,GLint level,GLenum internal,GLsizei w,GLsizei h,GLsizei d,GLint border,GLsizei size,const void* pixels){Call([&]{Require(border==0&&size>=0);auto t=BoundTexture(target);Require(!t->immutable,GL_INVALID_OPERATION);TextureImage(t,target,level,internal,GL_UNSIGNED_BYTE,w,h,d);if(w&&h&&d)UploadImage(t,target,level,0,0,0,w,h,d,internal,GL_UNSIGNED_BYTE,pixels,size);});}
void CompressedTexSubImage3D(GLenum target,GLint level,GLint x,GLint y,GLint z,GLsizei w,GLsizei h,GLsizei d,GLenum format,GLsizei size,const void* pixels){Call([&]{auto t=BoundTexture(target);Require(t->image&&t->image->format==Format(format)&&size>=0);UploadImage(t,target,level,x,y,z,w,h,d,format,GL_UNSIGNED_BYTE,pixels,size);});}
void TexStorage3D(GLenum target,GLsizei levels,GLenum internal,GLsizei w,GLsizei h,GLsizei d){Call([&]{Require(levels>0&&w>0&&h>0&&d>0&&static_cast<std::uint32_t>(levels)<=Mips(w,h,target==GL_TEXTURE_2D_ARRAY?1:d));auto t=BoundTexture(target);Require(!t->immutable,GL_INVALID_OPERATION);TextureImage(t,target,0,internal,GL_UNSIGNED_BYTE,w,h,d,levels);t->immutable=true;});}
void TexParameteri(GLenum target,GLenum pname,GLint value){Call([&]{auto t=BoundTexture(target);Parameter(t->sampling,pname,value,"texture");if(pname==GL_TEXTURE_BASE_LEVEL||pname==GL_TEXTURE_MAX_LEVEL)t->sampled.reset();});}
void TexParameterf(GLenum target,GLenum pname,GLfloat value){Call([&]{auto t=BoundTexture(target);Parameter(t->sampling,pname,value,"texture");if(pname==GL_TEXTURE_BASE_LEVEL||pname==GL_TEXTURE_MAX_LEVEL)t->sampled.reset();});}
void TexParameterfv(GLenum target,GLenum pname,const GLfloat* values){Call([&]{Require(values);if(pname==0x1004)std::copy_n(values,4,BoundTexture(target)->sampling.border.begin());else TexParameterf(target,pname,*values);});}
void GetTexParameteriv(GLenum target,GLenum pname,GLint* value){Call([&]{Require(value);const auto& s=BoundTexture(target)->sampling;switch(pname){case GL_TEXTURE_MIN_FILTER:*value=s.min;break;case GL_TEXTURE_MAG_FILTER:*value=s.mag;break;case GL_TEXTURE_WRAP_S:*value=s.s;break;case GL_TEXTURE_WRAP_T:*value=s.t;break;case GL_TEXTURE_WRAP_R:*value=s.r;break;case GL_TEXTURE_BASE_LEVEL:*value=s.base;break;case GL_TEXTURE_MAX_LEVEL:*value=s.maximum;break;case GL_TEXTURE_COMPARE_MODE:*value=s.compare;break;case GL_TEXTURE_COMPARE_FUNC:*value=s.compareFunc;break;default:Require(false,GL_INVALID_ENUM);}});}
void GetTexLevelParameteriv(GLenum target,GLint level,GLenum pname,GLint* value){Call([&]{Require(value&&level>=0&&level<16);auto t=BoundTexture(target);switch(pname){case 0x1000:*value=t->image?std::max(1u,t->image->width>>level):0;break;case 0x1001:*value=t->image?std::max(1u,t->image->height>>level):0;break;case 0x1003:*value=t->internal;break;case 0x86A1:*value=t->image&&Compressed(t->image->format);break;default:Require(false,GL_INVALID_ENUM);}});}
void GenerateMipmap(GLenum target){Call([&]{auto t=BoundTexture(target);auto image=t->image;Require(image&&!Compressed(image->format),GL_INVALID_OPERATION);VkFormatProperties props{};vkGetPhysicalDeviceFormatProperties(gpu.physical,image->format,&props);Require((props.optimalTilingFeatures&(VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT))==(VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT),GL_INVALID_OPERATION);auto filter=image->aspect==VK_IMAGE_ASPECT_COLOR_BIT&&(props.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;const auto base=std::min<std::uint32_t>(t->sampling.base,image->levels-1),end=std::min<std::uint32_t>(t->sampling.maximum,image->levels-1);for(auto m=base+1;m<=end;++m){Transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,m-1,1);Transition(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,m,1);VkImageBlit blit{};blit.srcSubresource={image->aspect,m-1,0,image->layers};blit.dstSubresource={image->aspect,m,0,image->layers};blit.srcOffsets[1]={static_cast<GLint>(std::max(1u,image->width>>(m-1))),static_cast<GLint>(std::max(1u,image->height>>(m-1))),static_cast<GLint>(std::max(1u,image->depth>>(m-1)))};blit.dstOffsets[1]={static_cast<GLint>(std::max(1u,image->width>>m)),static_cast<GLint>(std::max(1u,image->height>>m)),static_cast<GLint>(std::max(1u,image->depth>>m))};vkCmdBlitImage(Recording().command,image->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);}});}
void PixelStorei(GLenum pname,GLint value){Call([&]{auto& c=C();if(pname==GL_UNPACK_ALIGNMENT||pname==GL_PACK_ALIGNMENT)Require(value==1||value==2||value==4||value==8);else Require(value>=0&&value<=16384);switch(pname){case GL_UNPACK_ALIGNMENT:c.unpackAlignment=value;break;case GL_UNPACK_ROW_LENGTH:c.unpackRow=value;break;case GL_UNPACK_IMAGE_HEIGHT:c.unpackImage=value;break;case GL_UNPACK_SKIP_ROWS:c.unpackSkipRows=value;break;case GL_UNPACK_SKIP_PIXELS:c.unpackSkipPixels=value;break;case GL_UNPACK_SKIP_IMAGES:c.unpackSkipImages=value;break;case GL_PACK_ALIGNMENT:c.packAlignment=value;break;case GL_PACK_ROW_LENGTH:c.packRow=value;break;case GL_PACK_SKIP_ROWS:c.packSkipRows=value;break;case GL_PACK_SKIP_PIXELS:c.packSkipPixels=value;break;default:Require(false,GL_INVALID_ENUM);}});}
void GenSamplers(GLsizei count,GLuint* names){Call([&]{auto& s=*C().share;Generate(count,names,s.nextSampler,s.samplers);});}
void DeleteSamplers(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));for(GLsizei i=0;i<count;++i){C().share->samplers.erase(names[i]);for(auto& s:C().samplerUnits)if(s==names[i])s=0;}});}
void BindSampler(GLuint unit,GLuint name){Call([&]{Require(unit<32&&(!name||C().share->samplers.contains(name)));C().samplerUnits[unit]=name;});}
void SamplerParameteri(GLuint name,GLenum pname,GLint value){Call([&]{Require(C().share->samplers.contains(name),GL_INVALID_OPERATION);Parameter(C().share->samplers.at(name),pname,value,"sampler");});}
void SamplerParameterf(GLuint name,GLenum pname,GLfloat value){Call([&]{Require(C().share->samplers.contains(name),GL_INVALID_OPERATION);Parameter(C().share->samplers.at(name),pname,value,"sampler");});}

namespace {
Framebuffer& Fbo(GLenum target){Require(target==GL_FRAMEBUFFER||target==GL_DRAW_FRAMEBUFFER||target==GL_READ_FRAMEBUFFER,GL_INVALID_ENUM);auto name=target==GL_READ_FRAMEBUFFER?C().readFbo:C().drawFbo;Require(name!=0,GL_INVALID_OPERATION);return C().fbos.at(name);}
Attachment& Attach(Framebuffer& f,GLenum attachment){if(attachment>=GL_COLOR_ATTACHMENT0&&attachment<GL_COLOR_ATTACHMENT0+4)return f.colors[attachment-GL_COLOR_ATTACHMENT0];if(attachment==GL_DEPTH_ATTACHMENT||attachment==GL_DEPTH_STENCIL_ATTACHMENT)return f.depth;if(attachment==GL_STENCIL_ATTACHMENT)return f.stencil;Fail("Unsupported framebuffer attachment "+std::to_string(attachment));}
struct Targets {
    std::array<Attachment,4> colors;Attachment depth;
    std::array<VkFormat,4> formats{};VkFormat depthFormat{};
    VkImageAspectFlags logicalDsAspects{};
    std::uint32_t width{},height{},colorCount{};
    std::array<std::shared_ptr<View>,4> colorViews;std::shared_ptr<View> depthView;
};
VkImageAspectFlags LogicalDsAspects(const Framebuffer& framebuffer){
    return (framebuffer.depth.texture?VK_IMAGE_ASPECT_DEPTH_BIT:0u)|(framebuffer.stencil.texture?VK_IMAGE_ASPECT_STENCIL_BIT:0u);
}
bool SameAttachmentImage(const Attachment& a,const Attachment& b){return a.texture==b.texture&&a.level==b.level&&a.layer==b.layer;}
bool HasDsAspect(const Targets& targets,VkImageAspectFlagBits aspect){return (targets.logicalDsAspects&aspect)!=0;}
void IntersectExtent(VkExtent2D& extent,std::uint32_t width,std::uint32_t height){
    if(!extent.width){extent={width,height};return;}
    extent.width=std::min(extent.width,width);extent.height=std::min(extent.height,height);
}
GLenum FramebufferExtent(const Framebuffer& framebuffer,VkExtent2D& extent){
    GLenum status=GL_FRAMEBUFFER_COMPLETE;bool any=false;extent={};
    auto include=[&](const Attachment& attachment,VkImageAspectFlagBits aspect){
        if(!attachment.texture)return;any=true;const auto& image=attachment.texture->image;
        if(!image||!(image->aspect&aspect)||attachment.level<0||static_cast<std::uint32_t>(attachment.level)>=image->levels||attachment.layer<0||static_cast<std::uint32_t>(attachment.layer)>=image->layers){status=GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;return;}
        IntersectExtent(extent,std::max(1u,image->width>>attachment.level),std::max(1u,image->height>>attachment.level));
    };
    // Framebuffer bounds include every attached image, independently of which
    // color attachment the current draw/read buffer selection addresses.
    for(const auto& attachment:framebuffer.colors)include(attachment,VK_IMAGE_ASPECT_COLOR_BIT);
    include(framebuffer.depth,VK_IMAGE_ASPECT_DEPTH_BIT);include(framebuffer.stencil,VK_IMAGE_ASPECT_STENCIL_BIT);
    return any?status:GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}
std::uint32_t AspectBits(VkFormat format,VkImageAspectFlagBits aspect){
    if(aspect==VK_IMAGE_ASPECT_STENCIL_BIT)return (format==VK_FORMAT_S8_UINT||format==VK_FORMAT_D24_UNORM_S8_UINT||format==VK_FORMAT_D32_SFLOAT_S8_UINT)?8u:0u;
    if(aspect!=VK_IMAGE_ASPECT_DEPTH_BIT)return 0;
    switch(format){case VK_FORMAT_D16_UNORM:return 16;case VK_FORMAT_X8_D24_UNORM_PACK32:case VK_FORMAT_D24_UNORM_S8_UINT:return 24;case VK_FORMAT_D32_SFLOAT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return 32;default:return 0;}
}
void EffectiveDsState(std::array<std::uint32_t,26>& state,VkImageAspectFlags aspects){
    state[0]&=(aspects&VK_IMAGE_ASPECT_DEPTH_BIT)!=0;
    state[1]&=(aspects&VK_IMAGE_ASPECT_DEPTH_BIT)!=0;
    state[3]&=(aspects&VK_IMAGE_ASPECT_STENCIL_BIT)!=0;
    state[24]&=(aspects&VK_IMAGE_ASPECT_DEPTH_BIT)!=0;
}
std::shared_ptr<View> AttachmentView(const Attachment& a,VkImageAspectFlags aspect){
    const std::uint64_t key=(static_cast<std::uint64_t>(aspect)<<48)|(static_cast<std::uint64_t>(a.level)<<32)|static_cast<std::uint32_t>(a.layer);
    auto& map=a.texture->attachmentViews;auto it=map.find(key);if(it!=map.end())return it->second;
    auto view=NewView(a.texture->image,VK_IMAGE_VIEW_TYPE_2D,a.level,1,a.layer,1,aspect);map.emplace(key,view);return view;
}
Targets Target(GLuint name,bool read=false){
    Targets out;Framebuffer fb;
    if(!name){auto& c=C();Require(c.defaultColor&&c.defaultDepth,GL_INVALID_OPERATION);fb.colors[0]={0,GL_TEXTURE,GL_TEXTURE_2D,0,0,c.defaultColor};fb.depth={0,GL_TEXTURE,GL_TEXTURE_2D,0,0,c.defaultDepth};fb.stencil=fb.depth;}
    else {Require(C().fbos.contains(name),GL_INVALID_OPERATION);fb=C().fbos.at(name);}
    if(fb.depth.texture&&fb.stencil.texture&&!SameAttachmentImage(fb.depth,fb.stencil))Fail("Separate framebuffer depth and stencil subresources are unsupported");
    VkExtent2D extent{};Require(FramebufferExtent(fb,extent)==GL_FRAMEBUFFER_COMPLETE,GL_INVALID_FRAMEBUFFER_OPERATION);out.width=extent.width;out.height=extent.height;
    if(read){if(fb.read!=GL_NONE){Require(fb.read>=GL_COLOR_ATTACHMENT0&&fb.read<GL_COLOR_ATTACHMENT0+4,GL_INVALID_ENUM);out.colors[0]=fb.colors[fb.read-GL_COLOR_ATTACHMENT0];out.colorCount=1;}}
    else for(std::uint32_t i=0;i<4;++i)if(fb.draw[i]!=GL_NONE){Require(fb.draw[i]>=GL_COLOR_ATTACHMENT0&&fb.draw[i]<GL_COLOR_ATTACHMENT0+4,GL_INVALID_ENUM);out.colors[i]=fb.colors[fb.draw[i]-GL_COLOR_ATTACHMENT0];out.colorCount=i+1;}
    for(std::size_t i=0;i<4;++i){auto& a=out.colors[i];if(a.texture&&a.texture->image){out.formats[i]=a.texture->image->format;out.colorViews[i]=AttachmentView(a,VK_IMAGE_ASPECT_COLOR_BIT);}}
    out.depth=fb.depth.texture?fb.depth:fb.stencil;out.logicalDsAspects=LogicalDsAspects(fb);
    if(out.depth.texture&&out.depth.texture->image){out.depthFormat=out.depth.texture->image->format;out.depthView=AttachmentView(out.depth,out.depth.texture->image->aspect);}
    Require(out.width&&out.height,GL_INVALID_FRAMEBUFFER_OPERATION);return out;
}
template<class T> void Bytes(std::string& out,const T& value){static_assert(std::is_trivially_copyable_v<T>);out.append(reinterpret_cast<const char*>(&value),sizeof(value));}
std::string PassKey(const std::array<VkFormat,4>& colors,std::uint32_t colorCount,VkFormat depth){std::string key;Bytes(key,colors);Bytes(key,colorCount);Bytes(key,depth);return key;}
VkRenderPass Pass(const std::array<VkFormat,4>& colors,std::uint32_t colorCount,VkFormat depth){
    const auto key=PassKey(colors,colorCount,depth);if(auto it=renderPasses.find(key);it!=renderPasses.end())return it->second;
    std::vector<VkAttachmentDescription> attachments;std::array<VkAttachmentReference,4> refs{};
    for(std::uint32_t i=0;i<colorCount;++i){refs[i]={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};if(colors[i]){refs[i].attachment=attachments.size();VkAttachmentDescription description{};description.format=colors[i];description.samples=VK_SAMPLE_COUNT_1_BIT;description.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;description.storeOp=VK_ATTACHMENT_STORE_OP_STORE;description.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;description.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;description.initialLayout=description.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;attachments.push_back(description);}}
    VkAttachmentReference depthRef{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if(depth){depthRef.attachment=attachments.size();VkAttachmentDescription description{};description.format=depth;description.samples=VK_SAMPLE_COUNT_1_BIT;description.loadOp=description.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;description.storeOp=description.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;description.initialLayout=description.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;attachments.push_back(description);}
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=colorCount;subpass.pColorAttachments=refs.data();subpass.pDepthStencilAttachment=depth?&depthRef:nullptr;
    std::array<VkSubpassDependency,2> dependencies{};
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;dependencies[1].srcStageMask=dependencies[0].dstStageMask;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependencies[1].srcAccessMask=dependencies[0].dstAccessMask;dependencies[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=attachments.size();info.pAttachments=attachments.data();info.subpassCount=1;info.pSubpasses=&subpass;info.dependencyCount=dependencies.size();info.pDependencies=dependencies.data();
    VkRenderPass pass{};Check(vkCreateRenderPass(gpu.device,&info,nullptr,&pass),"vkCreateRenderPass");renderPasses.emplace(key,pass);return pass;
}
void BeginPass(const Targets& targets){
    std::string key=PassKey(targets.formats,targets.colorCount,targets.depthFormat);Bytes(key,targets.width);Bytes(key,targets.height);
    for(auto& a:targets.colors)if(a.texture&&a.texture->image){auto handle=a.texture->image->image;Bytes(key,handle);Bytes(key,a.level);Bytes(key,a.layer);}if(targets.depth.texture){auto handle=targets.depth.texture->image->image;Bytes(key,handle);Bytes(key,targets.depth.level);Bytes(key,targets.depth.layer);}
    auto& c=C();auto& f=Recording();if(f.renderPass&&key==c.activePass)return;EndPass();
    std::vector<VkImageView> attachments;
    for(std::size_t i=0;i<targets.colorCount;++i)if(targets.colorViews[i]){auto& a=targets.colors[i];Transition(a.texture->image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,a.level,1,a.layer,1);attachments.push_back(targets.colorViews[i]->view);Retain(targets.colorViews[i]);}
    if(targets.depthView){auto& a=targets.depth;Transition(a.texture->image,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,a.level,1,a.layer,1);attachments.push_back(targets.depthView->view);Retain(targets.depthView);}
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};framebuffer.renderPass=Pass(targets.formats,targets.colorCount,targets.depthFormat);framebuffer.attachmentCount=attachments.size();framebuffer.pAttachments=attachments.data();framebuffer.width=targets.width;framebuffer.height=targets.height;framebuffer.layers=1;
    VkFramebuffer handle{};Check(vkCreateFramebuffer(gpu.device,&framebuffer,nullptr,&handle),"vkCreateFramebuffer");f.framebuffers.push_back(handle);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=framebuffer.renderPass;begin.framebuffer=handle;begin.renderArea.extent={targets.width,targets.height};
    vkCmdBeginRenderPass(f.command,&begin,VK_SUBPASS_CONTENTS_INLINE);f.renderPass=true;c.activePass=std::move(key);
    if(c.activeQuery&&!c.activeQuery->active){Require(c.activeQuery->pieces<256,GL_OUT_OF_MEMORY);vkCmdBeginQuery(f.command,c.activeQuery->allocation->pool,c.activeQuery->pieces++,gpu.features.occlusionQueryPrecise?VK_QUERY_CONTROL_PRECISE_BIT:0);c.activeQuery->active=true;Retain(c.activeQuery->allocation);}
}
}
void GenFramebuffers(GLsizei count,GLuint* names){Call([&]{auto& c=C();Generate(count,names,c.nextFbo,c.fbos);});}
void DeleteFramebuffers(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));EndPass();auto& c=C();for(GLsizei i=0;i<count;++i)if(names[i]){c.fbos.erase(names[i]);if(c.drawFbo==names[i])c.drawFbo=0;if(c.readFbo==names[i])c.readFbo=0;}});}
void BindFramebuffer(GLenum target,GLuint name){Call([&]{Require(target==GL_FRAMEBUFFER||target==GL_DRAW_FRAMEBUFFER||target==GL_READ_FRAMEBUFFER,GL_INVALID_ENUM);EndPass();auto& c=C();if(name)c.fbos.try_emplace(name);if(target!=GL_READ_FRAMEBUFFER)c.drawFbo=name;if(target!=GL_DRAW_FRAMEBUFFER)c.readFbo=name;});}
void FramebufferTexture2D(GLenum target,GLenum attachment,GLenum textureTarget,GLuint texture,GLint level){Call([&]{Require(level>=0&&level<16);EndPass();auto& fb=Fbo(target);Attachment a;if(texture){auto it=C().share->textures.find(texture);Require(it!=C().share->textures.end(),GL_INVALID_OPERATION);Require(it->second->target==TextureTarget(textureTarget),GL_INVALID_OPERATION);a={texture,GL_TEXTURE,textureTarget,level,static_cast<GLint>(Face(textureTarget)),it->second};}Attach(fb,attachment)=a;if(attachment==GL_DEPTH_STENCIL_ATTACHMENT)fb.stencil=a;});}
void FramebufferTextureLayer(GLenum target,GLenum attachment,GLuint texture,GLint level,GLint layer){Call([&]{Require(level>=0&&level<16&&layer>=0);EndPass();auto& fb=Fbo(target);Attachment a;if(texture){auto it=C().share->textures.find(texture);Require(it!=C().share->textures.end()&&it->second->target==GL_TEXTURE_2D_ARRAY,GL_INVALID_OPERATION);a={texture,GL_TEXTURE,GL_TEXTURE_2D_ARRAY,level,layer,it->second};}Attach(fb,attachment)=a;if(attachment==GL_DEPTH_STENCIL_ATTACHMENT)fb.stencil=a;});}
GLenum CheckFramebufferStatus(GLenum target){GLenum out=GL_FRAMEBUFFER_COMPLETE;Call([&]{Require(target==GL_FRAMEBUFFER||target==GL_DRAW_FRAMEBUFFER||target==GL_READ_FRAMEBUFFER,GL_INVALID_ENUM);auto name=target==GL_READ_FRAMEBUFFER?C().readFbo:C().drawFbo;if(!name)return;VkExtent2D extent{};out=FramebufferExtent(C().fbos.at(name),extent);});return out;}
void GetFramebufferAttachmentParameteriv(GLenum target,GLenum attachment,GLenum pname,GLint* value){Call([&]{Require(value);auto& a=Attach(Fbo(target),attachment);switch(pname){case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:*value=a.object;break;case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:*value=a.name;break;case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:*value=a.level;break;case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:*value=a.textureTarget>=GL_TEXTURE_CUBE_MAP_POSITIVE_X&&a.textureTarget<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z?a.textureTarget:0;break;default:Require(false,GL_INVALID_ENUM);}});}
void DrawBuffers(GLsizei count,const GLenum* buffers){Call([&]{Require(count>=0&&count<=4&&(!count||buffers));EndPass();if(!C().drawFbo){Require(count==1&&(buffers[0]==GL_BACK||buffers[0]==GL_NONE),GL_INVALID_OPERATION);return;}auto& f=Fbo(GL_DRAW_FRAMEBUFFER);f.draw.fill(GL_NONE);for(GLsizei i=0;i<count;++i){Require(buffers[i]==GL_NONE||(buffers[i]>=GL_COLOR_ATTACHMENT0&&buffers[i]<GL_COLOR_ATTACHMENT0+4),GL_INVALID_ENUM);f.draw[i]=buffers[i];}});}
void ReadBuffer(GLenum buffer){Call([&]{if(!C().readFbo){Require(buffer==GL_BACK||buffer==GL_NONE,GL_INVALID_ENUM);return;}Require(buffer==GL_NONE||(buffer>=GL_COLOR_ATTACHMENT0&&buffer<GL_COLOR_ATTACHMENT0+4),GL_INVALID_ENUM);Fbo(GL_READ_FRAMEBUFFER).read=buffer;});}
void InvalidateFramebuffer(GLenum target,GLsizei count,const GLenum* attachments){Call([&]{Require(count>=0&&(!count||attachments));Require(target==GL_FRAMEBUFFER||target==GL_DRAW_FRAMEBUFFER||target==GL_READ_FRAMEBUFFER,GL_INVALID_ENUM);for(GLsizei i=0;i<count;++i)Require(attachments[i]==GL_COLOR||attachments[i]==GL_DEPTH||attachments[i]==GL_STENCIL||attachments[i]==GL_DEPTH_ATTACHMENT||attachments[i]==GL_STENCIL_ATTACHMENT||(attachments[i]>=GL_COLOR_ATTACHMENT0&&attachments[i]<GL_COLOR_ATTACHMENT0+4),GL_INVALID_ENUM);});}
void GenRenderbuffers(GLsizei count,GLuint* names){Call([&]{auto& s=*C().share;Generate(count,names,s.nextRenderbuffer,s.renderbuffers);for(GLsizei i=0;i<count;++i)s.renderbuffers[names[i]]=std::make_shared<Texture>();});}
void DeleteRenderbuffers(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));for(GLsizei i=0;i<count;++i)if(names[i]){C().share->renderbuffers.erase(names[i]);if(C().boundRenderbuffer==names[i])C().boundRenderbuffer=0;for(auto& [id,f]:C().fbos){for(auto& a:f.colors)if(a.object==GL_RENDERBUFFER&&a.name==names[i])a=Attachment{};for(auto* a:{&f.depth,&f.stencil})if(a->object==GL_RENDERBUFFER&&a->name==names[i])*a=Attachment{};}}});}
void BindRenderbuffer(GLenum target,GLuint name){Call([&]{Require(target==GL_RENDERBUFFER,GL_INVALID_ENUM);if(name){auto& t=C().share->renderbuffers[name];if(!t)t=std::make_shared<Texture>();}C().boundRenderbuffer=name;});}
void RenderbufferStorage(GLenum target,GLenum format,GLsizei w,GLsizei h){Call([&]{Require(target==GL_RENDERBUFFER,GL_INVALID_ENUM);Require(C().boundRenderbuffer&&w>0&&h>0,GL_INVALID_OPERATION);auto t=C().share->renderbuffers.at(C().boundRenderbuffer);t->internal=format;t->image=NewImage(Format(format),w,h,1,1,1,false);t->sampled.reset();t->attachmentViews.clear();});}
void RenderbufferStorageMultisample(GLenum target,GLsizei samples,GLenum format,GLsizei w,GLsizei h){Call([&]{if(samples>1)Fail("Multisample framebuffer storage is not supported by the experimental Vulkan backend");Require(samples>=0);RenderbufferStorage(target,format,w,h);});}
void FramebufferRenderbuffer(GLenum target,GLenum attachment,GLenum renderbufferTarget,GLuint name){Call([&]{Require(renderbufferTarget==GL_RENDERBUFFER,GL_INVALID_ENUM);EndPass();auto& f=Fbo(target);Attachment a;if(name){Require(C().share->renderbuffers.contains(name),GL_INVALID_OPERATION);a={name,GL_RENDERBUFFER,GL_TEXTURE_2D,0,0,C().share->renderbuffers.at(name)};}Attach(f,attachment)=a;if(attachment==GL_DEPTH_STENCIL_ATTACHMENT)f.stencil=a;});}
void GetRenderbufferParameteriv(GLenum target,GLenum pname,GLint* value){Call([&]{Require(target==GL_RENDERBUFFER&&value,GL_INVALID_ENUM);Require(C().boundRenderbuffer,GL_INVALID_OPERATION);auto t=C().share->renderbuffers.at(C().boundRenderbuffer);switch(pname){case GL_RENDERBUFFER_WIDTH:*value=t->image?t->image->width:0;break;case GL_RENDERBUFFER_HEIGHT:*value=t->image?t->image->height:0;break;case GL_RENDERBUFFER_INTERNAL_FORMAT:*value=t->internal;break;case GL_RENDERBUFFER_SAMPLES:*value=0;break;case GL_RENDERBUFFER_DEPTH_SIZE:*value=t->image&&(t->image->aspect&VK_IMAGE_ASPECT_DEPTH_BIT)?t->internal==GL_DEPTH_COMPONENT16?16:t->internal==GL_DEPTH_COMPONENT32F?32:24:0;break;case GL_RENDERBUFFER_STENCIL_SIZE:*value=t->image&&(t->image->aspect&VK_IMAGE_ASPECT_STENCIL_BIT)?8:0;break;case GL_RENDERBUFFER_RED_SIZE:case GL_RENDERBUFFER_GREEN_SIZE:case GL_RENDERBUFFER_BLUE_SIZE:case GL_RENDERBUFFER_ALPHA_SIZE:*value=8;break;default:Require(false,GL_INVALID_ENUM);}});}

namespace {
VkCompareOp Compare(GLenum op){Require(op>=GL_NEVER&&op<=GL_ALWAYS,GL_INVALID_ENUM);return static_cast<VkCompareOp>(op-GL_NEVER);}
VkStencilOp StencilOp(GLenum op){switch(op){case GL_KEEP:return VK_STENCIL_OP_KEEP;case GL_ZERO:return VK_STENCIL_OP_ZERO;case GL_REPLACE:return VK_STENCIL_OP_REPLACE;case GL_INCR:return VK_STENCIL_OP_INCREMENT_AND_CLAMP;case GL_DECR:return VK_STENCIL_OP_DECREMENT_AND_CLAMP;case GL_INVERT:return VK_STENCIL_OP_INVERT;case GL_INCR_WRAP:return VK_STENCIL_OP_INCREMENT_AND_WRAP;case GL_DECR_WRAP:return VK_STENCIL_OP_DECREMENT_AND_WRAP;default:Fail("Unsupported stencil operation");}}
VkBlendFactor BlendFactor(GLenum op){switch(op){case GL_ZERO:return VK_BLEND_FACTOR_ZERO;case GL_ONE:return VK_BLEND_FACTOR_ONE;case GL_SRC_COLOR:return VK_BLEND_FACTOR_SRC_COLOR;case GL_ONE_MINUS_SRC_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;case GL_DST_COLOR:return VK_BLEND_FACTOR_DST_COLOR;case GL_ONE_MINUS_DST_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;case GL_SRC_ALPHA:return VK_BLEND_FACTOR_SRC_ALPHA;case GL_ONE_MINUS_SRC_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;case GL_DST_ALPHA:return VK_BLEND_FACTOR_DST_ALPHA;case GL_ONE_MINUS_DST_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;case GL_CONSTANT_COLOR:return VK_BLEND_FACTOR_CONSTANT_COLOR;case GL_ONE_MINUS_CONSTANT_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;case GL_CONSTANT_ALPHA:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case GL_ONE_MINUS_CONSTANT_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;case GL_SRC_ALPHA_SATURATE:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;default:Fail("Unsupported blend factor");}}
VkBlendOp BlendOp(GLenum op){switch(op){case GL_FUNC_ADD:return VK_BLEND_OP_ADD;case GL_FUNC_SUBTRACT:return VK_BLEND_OP_SUBTRACT;case GL_FUNC_REVERSE_SUBTRACT:return VK_BLEND_OP_REVERSE_SUBTRACT;case GL_MIN:return VK_BLEND_OP_MIN;case GL_MAX:return VK_BLEND_OP_MAX;default:Fail("Unsupported blend equation");}}
VkPrimitiveTopology Topology(GLenum mode){switch(mode){case GL_POINTS:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;case GL_LINES:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case GL_LINE_STRIP:case GL_LINE_LOOP:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;case GL_TRIANGLES:case 0x0007:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case GL_TRIANGLE_STRIP:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;case GL_TRIANGLE_FAN:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;default:Fail("Unsupported draw topology "+std::to_string(mode));}}
VkSamplerAddressMode Address(GLenum value){return value==GL_REPEAT?VK_SAMPLER_ADDRESS_MODE_REPEAT:value==GL_MIRRORED_REPEAT?VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:value==0x812D?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;}
std::shared_ptr<SamplerObject> Sampler(const Sampling& state,std::uint32_t levels,bool shadow){
    std::string key;Bytes(key,state.min);Bytes(key,state.mag);Bytes(key,state.s);Bytes(key,state.t);Bytes(key,state.r);Bytes(key,state.compare);Bytes(key,state.compareFunc);Bytes(key,state.aniso);Bytes(key,state.minLod);Bytes(key,state.maxLod);Bytes(key,state.lodBias);Bytes(key,state.border);Bytes(key,levels);Bytes(key,shadow);
    if(auto it=samplers.find(key);it!=samplers.end())return it->second;
    auto out=std::make_shared<SamplerObject>();VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter=state.mag==GL_NEAREST?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
    info.minFilter=state.min==GL_NEAREST||state.min==GL_NEAREST_MIPMAP_NEAREST||state.min==GL_NEAREST_MIPMAP_LINEAR?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
    info.mipmapMode=state.min==GL_NEAREST_MIPMAP_LINEAR||state.min==GL_LINEAR_MIPMAP_LINEAR?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=Address(state.s);info.addressModeV=Address(state.t);info.addressModeW=Address(state.r);
    info.mipLodBias=std::clamp(state.lodBias,-gpu.properties.limits.maxSamplerLodBias,gpu.properties.limits.maxSamplerLodBias);
    info.anisotropyEnable=gpu.features.samplerAnisotropy&&state.aniso>1;info.maxAnisotropy=std::clamp(state.aniso,1.f,gpu.properties.limits.maxSamplerAnisotropy);
    info.compareEnable=shadow;info.compareOp=Compare(state.compareFunc);
    info.minLod=std::max(0.f,state.minLod);info.maxLod=state.min==GL_NEAREST||state.min==GL_LINEAR?0.f:std::min(state.maxLod,static_cast<float>(levels-1));
    info.maxLod=std::max(info.minLod,info.maxLod);
    if(state.border==std::array<float,4>{0,0,0,0})info.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if(state.border==std::array<float,4>{0,0,0,1})info.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if(state.border==std::array<float,4>{1,1,1,1})info.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if(state.s==0x812D||state.t==0x812D||state.r==0x812D)Fail("Custom sampler border color requires an unavailable Vulkan extension");
    Check(vkCreateSampler(gpu.device,&info,nullptr,&out->sampler),"vkCreateSampler");
    if(samplers.size()>4096)samplers.clear();samplers.emplace(key,out);return out;
}
std::shared_ptr<View> SampleView(const std::shared_ptr<Texture>& t){
    Require(t&&t->image,GL_INVALID_OPERATION);auto image=t->image;
    const auto base=std::min<std::uint32_t>(t->sampling.base,image->levels-1),maximum=std::min<std::uint32_t>(t->sampling.maximum,image->levels-1);Require(maximum>=base,GL_INVALID_OPERATION);
    if(!t->sampled){VkComponentMapping swizzle{};
        if(t->internal==GL_ALPHA)swizzle={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};
        else if(t->internal==GL_LUMINANCE)swizzle={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
        else if(t->internal==GL_LUMINANCE_ALPHA)swizzle={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G};
        else if(t->internal==GL_RGB8_SNORM||t->internal==0x8F9A)swizzle.a=VK_COMPONENT_SWIZZLE_ONE;
        auto type=t->target==GL_TEXTURE_CUBE_MAP?VK_IMAGE_VIEW_TYPE_CUBE:t->target==GL_TEXTURE_3D?VK_IMAGE_VIEW_TYPE_3D:t->target==GL_TEXTURE_2D_ARRAY?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
        t->sampled=NewView(image,type,base,maximum-base+1,0,image->layers,image->aspect&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,swizzle);
    }
    Retain(t->sampled);return t->sampled;
}
void Parameters(const Program& program,void* output,std::size_t bytes){
    std::memset(output,0,bytes);
    for(const auto& field:program.shader->stage.fields){Require(field.groupIndex<9&&field.offset<=bytes&&field.count*16<=bytes-field.offset,GL_INVALID_OPERATION);const auto& group=program.groups[field.groupIndex];if(group.first_register<0||!group.byte_count)continue;const auto first=static_cast<std::size_t>(group.first_register)*4;const auto count=std::min<std::size_t>({field.count,static_cast<std::size_t>(group.byte_count/16),program.locals.size()>first?(program.locals.size()-first)/4:0});auto* dst=static_cast<std::uint8_t*>(output)+field.offset;for(std::size_t i=0;i<count;++i)if(field.type==bl2::vk::FieldType::Bool){std::uint32_t value=program.locals[first+i*4]!=0;std::memcpy(dst+i*16,&value,4);}else std::memcpy(dst+i*16,program.locals.data()+first+i*4,16);}
}
GLenum SamplerTarget(bl2::vk::SamplerType type){return type==bl2::vk::SamplerType::SamplerCube?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_2D;}
void ApplyLegacyBindings(Context& context,const Program& program){
    for(const auto& sampler:program.shader->stage.samplers){
        Require(sampler.unit<16,GL_INVALID_OPERATION);const auto reg=program.samplerRegisters[sampler.unit];
        if(reg<0||static_cast<std::size_t>(reg)*4+1>=program.locals.size())continue;
        const auto name=program.locals[reg*4],samplerName=program.locals[reg*4+1];if(!name)continue;
        auto texture=context.share->textures.find(name);Require(texture!=context.share->textures.end(),GL_INVALID_OPERATION);
        const auto target=SamplerTarget(sampler.type);Require(texture->second->target==target&&(!samplerName||context.share->samplers.contains(samplerName)),GL_INVALID_OPERATION);
        context.textureUnits[sampler.unit][target]={name,texture->second};context.samplerUnits[sampler.unit]=samplerName;
    }
}
bool HasShadowWithoutDepth(const Context& context,const Program& program){
    for(const auto& sampler:program.shader->stage.samplers){
        if(sampler.type!=bl2::vk::SamplerType::Sampler2DShadow)continue;
        const auto& unit=context.textureUnits[sampler.unit];const auto bound=unit.find(SamplerTarget(sampler.type));
        if(bound!=unit.end()&&bound->second.second&&bound->second.second->image&&!(bound->second.second->image->aspect&VK_IMAGE_ASPECT_DEPTH_BIT))return true;
    }
    return false;
}
void MissingSampler(const Program& vertex,const Program& fragment,const Program& program,const bl2::vk::Sampler& sampler,GLuint name,const std::shared_ptr<Texture>& texture){
    const auto reg=program.samplerRegisters[sampler.unit];const bool range=reg>=0&&static_cast<std::size_t>(reg)*4+1<program.locals.size();std::array<std::uint32_t,4> words{};
    if(range)std::copy_n(program.locals.data()+reg*4,std::min<std::size_t>(4,program.locals.size()-reg*4),words.begin());
    __android_log_print(ANDROID_LOG_ERROR,"BL2Vulkan","Missing sampler: draw=%llu V=%u/%s P=%u/%s stage=%s unit=%u binding=%u target=0x%04x reg=%d localWords=%zu range=%u raw={%08x,%08x,%08x,%08x} boundTexture=%u boundSampler=%u object=%u objectTarget=0x%04x internal=0x%04x image=%u",
        static_cast<unsigned long long>(drawEpoch),C().vertexProgram,Hex(vertex.shader->stage.normalizedDigest).c_str(),C().fragmentProgram,Hex(fragment.shader->stage.normalizedDigest).c_str(),program.target==kVS?"V":"P",sampler.unit,sampler.binding,SamplerTarget(sampler.type),reg,program.locals.size(),range,words[0],words[1],words[2],words[3],name,C().samplerUnits[sampler.unit],bool(texture),texture?texture->target:0,texture?texture->internal:0,bool(texture&&texture->image));
    Fail("Required sampler has no uploaded Vulkan texture, binding="+std::to_string(sampler.binding));
}
void DiagnosticLine(std::string&,const char*,...) __attribute__((format(printf,2,3)));
VkDescriptorSet Descriptors(const Program& vertex,const Program& fragment,const Targets& targets,std::string* diagnostic=nullptr){
    // The original legacy path applies V then P handles to persistent GL units.
    // An absent/zero handle retains the previously bound texture and sampler.
    ApplyLegacyBindings(C(),vertex);ApplyLegacyBindings(C(),fragment);
    auto& frame=Recording();VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=frame.descriptors;allocation.descriptorSetCount=1;allocation.pSetLayouts=&gpu.descriptors;
    VkDescriptorSet set{};Check(vkAllocateDescriptorSets(gpu.device,&allocation,&set),"vkAllocateDescriptorSets");
    std::vector<VkWriteDescriptorSet> writes;std::array<VkDescriptorBufferInfo,2> buffers{};std::array<VkDescriptorImageInfo,32> images{};std::uint32_t imageCount=0;
    bool freshUniform=false;
    if(diagnostic){
        std::array<char,PROP_VALUE_MAX> property{};__system_property_get("debug.bl2.vk_ao_ubo_fresh",property.data());
        freshUniform=std::strcmp(property.data(),"1")==0;
        DiagnosticLine(*diagnostic,"boundUniformMode fresh=%u property=debug.bl2.vk_ao_ubo_fresh default=0\n",freshUniform);
    }
    for(const auto* p:{&vertex,&fragment}){
        const auto binding=p->shader->stage.parameterBinding;Require(binding<2,GL_INVALID_OPERATION);
        const auto bytes=std::max(16u,p->shader->stage.parameterBytes);Slice slice;
        if(freshUniform){
            auto buffer=Allocate(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            Retain(buffer);slice={buffer,0,bytes,buffer->mapped};
        }else slice=Upload(bytes,gpu.properties.limits.minUniformBufferOffsetAlignment);
        Parameters(*p,slice.data,bytes);
        buffers[binding]={slice.buffer->buffer,slice.offset,bytes};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=binding;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&buffers[binding];writes.push_back(write);
        if(diagnostic){
            const auto& bound=buffers[binding];
            const bool complete=bound.buffer==slice.buffer->buffer&&slice.buffer->mapped&&bound.range==bytes&&bound.offset<=slice.buffer->size&&bound.range<=slice.buffer->size-bound.offset;
            const auto* mapped=complete?static_cast<const std::uint8_t*>(slice.buffer->mapped)+bound.offset:nullptr;
            DiagnosticLine(*diagnostic,"boundUniform stage=%s binding=%u descriptorSet=%llx vkBuffer=%llx allocation=%p frame=%zu offset=%llu range=%llu allocationBytes=%llu complete=%u writtenSha256=%s mappedDescriptorSha256=%s writtenEqualsMapped=%u hardwareRead=unproven\n",p->target==kVS?"V":"P",binding,static_cast<unsigned long long>(set),static_cast<unsigned long long>(bound.buffer),static_cast<void*>(slice.buffer.get()),C().frameIndex,static_cast<unsigned long long>(bound.offset),static_cast<unsigned long long>(bound.range),static_cast<unsigned long long>(slice.buffer->size),static_cast<unsigned>(complete),Hex(Hash(slice.data,bytes)).c_str(),complete?Hex(Hash(mapped,bytes)).c_str():"unavailable",static_cast<unsigned>(complete&&!std::memcmp(slice.data,mapped,bytes)));
        }
        for(const auto& sampler:p->shader->stage.samplers){Require(sampler.unit<16&&sampler.binding>=2&&sampler.binding<34,GL_INVALID_OPERATION);
            const GLenum target=SamplerTarget(sampler.type);
            std::shared_ptr<Texture> texture;GLuint name=0,samplerName=C().samplerUnits[sampler.unit];
            auto it=C().textureUnits[sampler.unit].find(target);if(it!=C().textureUnits[sampler.unit].end()){name=it->second.first;texture=it->second.second;}
            if(!texture||!texture->image)MissingSampler(vertex,fragment,*p,sampler,name,texture);
            Require(texture->target==target,GL_INVALID_OPERATION);
            for(const auto& a:targets.colors)if(a.texture&&a.texture->image==texture->image)Fail("Framebuffer feedback loop is unsupported");
            if(targets.depth.texture&&targets.depth.texture->image==texture->image)Fail("Depth framebuffer feedback loop is unsupported");
            auto view=SampleView(texture);auto state=texture->sampling;
            if(samplerName){auto it=C().share->samplers.find(samplerName);Require(it!=C().share->samplers.end(),GL_INVALID_OPERATION);state=it->second;}
            const bool shadow=sampler.type==bl2::vk::SamplerType::Sampler2DShadow;
            Require(!shadow||(texture->image->aspect&VK_IMAGE_ASPECT_DEPTH_BIT),GL_INVALID_OPERATION);
            const bool linear=state.mag==GL_LINEAR||state.min==GL_LINEAR||state.min==GL_LINEAR_MIPMAP_NEAREST||state.min==GL_LINEAR_MIPMAP_LINEAR||state.min==GL_NEAREST_MIPMAP_LINEAR||(gpu.features.samplerAnisotropy&&state.aniso>1);
            if(linear&&!shadow&&!(texture->image->features&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))Fail("Vulkan image format lacks requested linear filtering support: "+std::to_string(texture->image->format));
            auto vkSampler=Sampler(state,std::min<std::uint32_t>(texture->sampling.maximum,texture->image->levels-1)-std::min<std::uint32_t>(texture->sampling.base,texture->image->levels-1)+1,shadow);
            auto layout=texture->image->aspect&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            Transition(texture->image,layout);Retain(vkSampler);
            images[imageCount]={vkSampler->sampler,view->view,layout};VkWriteDescriptorSet imageWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};imageWrite.dstSet=set;imageWrite.dstBinding=sampler.binding;imageWrite.descriptorCount=1;imageWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;imageWrite.pImageInfo=&images[imageCount++];writes.push_back(imageWrite);
            if(diagnostic)DiagnosticLine(*diagnostic,"boundSampledImage stage=%s unit=%u binding=%u descriptorSet=%llx vkSampler=%llx vkView=%llx viewImage=%llx textureImage=%llx viewMatchesTexture=%u layout=%u base=%u maximum=%u hardwareRead=unproven\n",p->target==kVS?"V":"P",sampler.unit,sampler.binding,static_cast<unsigned long long>(set),static_cast<unsigned long long>(vkSampler->sampler),static_cast<unsigned long long>(view->view),static_cast<unsigned long long>(view->image->image),static_cast<unsigned long long>(texture->image->image),static_cast<unsigned>(view->image==texture->image),layout,texture->sampling.base,texture->sampling.maximum);
            if(TakeShadowDescriptorReport(gpu.gfxCapture,shadow,gpu.shadowDescriptorReports)){
                const auto reg=p->samplerRegisters[sampler.unit];std::array<std::uint32_t,4> local{};bool localPresent=false;
                if(reg>=0&&static_cast<std::size_t>(reg)*4+4<=p->locals.size()){std::copy_n(p->locals.data()+static_cast<std::size_t>(reg)*4,4,local.begin());localPresent=true;}
                __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Shadow descriptor accepted: reports=%u draw=%llu V=%u P=%u stage=%s unit=%u binding=%u reg=%d localPresent=%u raw=%08x,%08x,%08x,%08x texture=%u sampler=%u internal=%x vkFormat=%u aspect=%x compareMode=%x compareFunction=%x actualDref=1 actualCompareOp=%u",gpu.shadowDescriptorReports,static_cast<unsigned long long>(drawEpoch),C().vertexProgram,C().fragmentProgram,p->target==kVS?"V":"P",sampler.unit,sampler.binding,reg,static_cast<unsigned>(localPresent),local[0],local[1],local[2],local[3],name,samplerName,texture->internal,texture->image->format,texture->image->aspect,state.compare,state.compareFunc,static_cast<unsigned>(Compare(state.compareFunc)));
            }
        }
    }
    vkUpdateDescriptorSets(gpu.device,writes.size(),writes.data(),0,nullptr);return set;
}
using Recipe=seed::Recipe;
static_assert(sizeof(Recipe)==460);
std::vector<Recipe> recipes;
bool cacheDirty=false;
std::vector<seed::PostDraw> postDrawRecords;
std::unordered_map<std::string,std::size_t> postDrawIndex;
bool postDrawOverflow=false, postDrawDirty=false;
std::uint64_t postDrawStart=0;
std::size_t CandidateCount(){return recipes.size();}
void ResetPrewarmState(bool beginRun){
    recipes.clear();mandatoryKeys.clear();postDrawRecords.clear();postDrawIndex.clear();
    initializationComplete=false;mandatoryReady=false;cacheDirty=false;postDrawOverflow=false;postDrawDirty=false;
    startupCreates=runtimeCreates=lateMandatoryCreates=successfulDraws=rejectedDraws=0;
    postDrawCapture=false;postDrawStart=0;
    if(beginRun){std::array<char,PROP_VALUE_MAX> property{};__system_property_get("debug.bl2.vk_pso_capture",property.data());postDrawCapture=std::strcmp(property.data(),"1")==0;postDrawStart=postDrawCapture?MonotonicNs():0;}
}
void RememberRecordedDraw(const Recipe& recipe,const std::shared_ptr<PipelineObject>& pipeline){
    ++successfulDraws;
    if(!pipeline->successRemembered){
        auto existing=std::find_if(recipes.begin(),recipes.end(),[&](const auto& r){return std::memcmp(&r,&recipe,sizeof(recipe))==0;});
        if(existing==recipes.end()){if(recipes.size()==kMaxPipelines)recipes.erase(recipes.begin());recipes.push_back(recipe);cacheDirty=true;}
        pipeline->successRemembered=true;
    }
    if(!postDrawCapture)return;
    const auto now=MonotonicNs();
    if(pipeline->postDrawSlot==SIZE_MAX){
        const auto encoded=seed::EncodeRecipe(recipe);const auto digest=Hash(encoded.data(),encoded.size());
        const std::string key(reinterpret_cast<const char*>(digest.data()),digest.size());
        auto found=postDrawIndex.find(key);
        if(found!=postDrawIndex.end()){
            if(seed::EncodeRecipe(postDrawRecords[found->second].recipe)!=encoded)Fail("Postdraw SHA256 identity collision");pipeline->postDrawSlot=found->second;
        }else{
            if(postDrawRecords.size()==seed::kMaxCandidates){postDrawOverflow=true;postDrawDirty=true;return;}
            pipeline->postDrawSlot=postDrawRecords.size();postDrawIndex.emplace(key,pipeline->postDrawSlot);postDrawRecords.push_back({recipe,drawEpoch,drawEpoch,0,now,now});
        }
    }
    auto& entry=postDrawRecords[pipeline->postDrawSlot];entry.lastDraw=drawEpoch;entry.lastNs=now;++entry.uses;postDrawDirty=true;
}
void SavePostDraw(){
    if(!postDrawCapture||!postDrawDirty||gpu.cacheDirectory.empty())return;
    seed::PostDrawRun run{seed::ParseDigest(BL2_VULKAN_BACKEND_ID),gpu.pack->Identity(),static_cast<std::uint32_t>(getpid()),postDrawOverflow,postDrawStart,rejectedDraws,lateMandatoryCreates,runtimeCreates,startupCreates};
    const auto bytes=seed::EncodePostDraw(postDrawRecords,run);
    const auto path=gpu.cacheDirectory+"/"+gpu.identity+"-"+std::to_string(getpid())+".postdraw";
    if(AtomicFile(path,bytes.data(),bytes.size()))postDrawDirty=false;
    __android_log_print(postDrawDirty?ANDROID_LOG_WARN:ANDROID_LOG_INFO,"BL2Vulkan","Postdraw PSO receipt: count=%zu bytes=%zu overflow=%u rejected=%llu lateMandatory=%llu successful=%llu written=%u file=%s sha256=%s",postDrawRecords.size(),bytes.size(),postDrawOverflow,static_cast<unsigned long long>(rejectedDraws),static_cast<unsigned long long>(lateMandatoryCreates),static_cast<unsigned long long>(successfulDraws),!postDrawDirty,path.c_str(),Hex(Hash(bytes.data(),bytes.size())).c_str());
}
Recipe MakeRecipe(const Program& v,const Program& f,const Targets& t,GLenum mode,const std::vector<VkVertexInputAttributeDescription>& attributes,const std::vector<VkVertexInputBindingDescription>& bindings){
    Recipe recipe{};recipe.vertex=v.shader->stage.normalizedDigest;recipe.fragment=f.shader->stage.normalizedDigest;for(std::size_t i=0;i<4;++i)recipe.colors[i]=t.formats[i];recipe.colorCount=t.colorCount;recipe.depthFormat=t.depthFormat;recipe.topology=Topology(mode);recipe.restart=C().enabled.contains(GL_PRIMITIVE_RESTART_FIXED_INDEX)&&(recipe.topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP||recipe.topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN||recipe.topology==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);recipe.attributeCount=attributes.size();for(std::size_t i=0;i<attributes.size();++i)recipe.attributes[i]={attributes[i].location,static_cast<std::uint32_t>(attributes[i].format),bindings[i].stride,static_cast<std::uint32_t>(bindings[i].inputRate)};
    auto& c=C();recipe.state={c.enabled.contains(GL_DEPTH_TEST),c.depthWrite,Compare(c.depthFunction),c.enabled.contains(GL_STENCIL_TEST),StencilOp(c.frontStencil.fail),StencilOp(c.frontStencil.depthFail),StencilOp(c.frontStencil.pass),Compare(c.frontStencil.function),StencilOp(c.backStencil.fail),StencilOp(c.backStencil.depthFail),StencilOp(c.backStencil.pass),Compare(c.backStencil.function),c.enabled.contains(GL_BLEND),BlendFactor(c.srcRgb),BlendFactor(c.dstRgb),BlendFactor(c.srcAlpha),BlendFactor(c.dstAlpha),BlendOp(c.rgbEquation),BlendOp(c.alphaEquation),static_cast<std::uint32_t>((c.colorWrite[0]?1:0)|(c.colorWrite[1]?2:0)|(c.colorWrite[2]?4:0)|(c.colorWrite[3]?8:0)),c.enabled.contains(GL_CULL_FACE)?c.cull==GL_FRONT?VK_CULL_MODE_FRONT_BIT:c.cull==GL_BACK?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_FRONT_AND_BACK:VK_CULL_MODE_NONE,c.front==GL_CCW?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE,c.enabled.contains(0x864F),c.enabled.contains(GL_POLYGON_OFFSET_FILL),c.enabled.contains(0x8890),c.enabled.contains(GL_RASTERIZER_DISCARD)};
    // A packed image can supply only one logical GL attachment. The inactive
    // physical aspect must neither reject fragments nor receive draw writes.
    EffectiveDsState(recipe.state,t.logicalDsAspects);
    return recipe;
}
bool ValidRecipe(const Recipe& r){
    if(r.colorCount>4||r.attributeCount>16||r.topology>VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN||r.restart>1)return false;
    for(std::size_t i=0;i<r.attributeCount;++i){if(r.attributes[i][0]>=16||r.attributes[i][2]>2048||r.attributes[i][3]>1||!r.attributes[i][1]||r.attributes[i][1]>VK_FORMAT_R32G32B32A32_SFLOAT)return false;VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(gpu.physical,static_cast<VkFormat>(r.attributes[i][1]),&p);if(!(p.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))return false;}
    for(std::size_t i=0;i<4;++i)if(r.colors[i]){VkFormatProperties p{};if(r.colors[i]>VK_FORMAT_R32G32B32A32_SFLOAT)return false;vkGetPhysicalDeviceFormatProperties(gpu.physical,static_cast<VkFormat>(r.colors[i]),&p);if(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))return false;}
    if(r.depthFormat&&r.depthFormat!=VK_FORMAT_D16_UNORM&&r.depthFormat!=VK_FORMAT_X8_D24_UNORM_PACK32&&r.depthFormat!=VK_FORMAT_D32_SFLOAT&&r.depthFormat!=VK_FORMAT_D24_UNORM_S8_UINT&&r.depthFormat!=VK_FORMAT_D32_SFLOAT_S8_UINT&&r.depthFormat!=VK_FORMAT_S8_UINT)return false;
    auto& s=r.state;return s[0]<=1&&s[1]<=1&&s[2]<=7&&s[3]<=1&&s[4]<=7&&s[5]<=7&&s[6]<=7&&s[7]<=7&&s[8]<=7&&s[9]<=7&&s[10]<=7&&s[11]<=7&&s[12]<=1&&s[13]<=VK_BLEND_FACTOR_SRC_ALPHA_SATURATE&&s[14]<=VK_BLEND_FACTOR_SRC_ALPHA_SATURATE&&s[15]<=VK_BLEND_FACTOR_SRC_ALPHA_SATURATE&&s[16]<=VK_BLEND_FACTOR_SRC_ALPHA_SATURATE&&s[17]<=4&&s[18]<=4&&s[19]<=15&&s[20]<=3&&s[21]<=1&&s[22]<=1&&(!s[22]||gpu.features.depthClamp)&&s[23]<=1&&s[24]<=1&&(!s[24]||gpu.features.depthBounds)&&s[25]<=1;
}
std::shared_ptr<PipelineObject> Pipeline(const Recipe& recipe,const std::shared_ptr<Shader>& vertex,const std::shared_ptr<Shader>& fragment){
    std::string key;Bytes(key,recipe);if(auto it=pipelines.find(key);it!=pipelines.end()){it->second->lastUse=++pipelineUse;return it->second;}
    if(pipelines.size()>=kMaxResidentPipelines){
        auto oldest=pipelines.end();for(auto it=pipelines.begin();it!=pipelines.end();++it)if(!it->second->pinned&&(oldest==pipelines.end()||it->second->lastUse<oldest->second->lastUse))oldest=it;
        if(oldest==pipelines.end())Fail("Resident pipeline map has no evictable entry");pipelines.erase(oldest);
    }
    Require(ValidRecipe(recipe),GL_INVALID_OPERATION);
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};for(std::size_t i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?fragment->module:vertex->module;stages[i].pName="main";}
    std::array<VkVertexInputBindingDescription,16> bindings{};std::array<VkVertexInputAttributeDescription,16> attributes{};
    for(std::size_t i=0;i<recipe.attributeCount;++i){auto& a=recipe.attributes[i];bindings[i]={static_cast<std::uint32_t>(i),a[2],static_cast<VkVertexInputRate>(a[3])};attributes[i]={a[0],static_cast<std::uint32_t>(i),static_cast<VkFormat>(a[1]),0};}
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};input.vertexBindingDescriptionCount=input.vertexAttributeDescriptionCount=recipe.attributeCount;input.pVertexBindingDescriptions=bindings.data();input.pVertexAttributeDescriptions=attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=static_cast<VkPrimitiveTopology>(recipe.topology);assembly.primitiveRestartEnable=recipe.restart;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
    auto& s=recipe.state;VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.depthClampEnable=s[22];raster.rasterizerDiscardEnable=s[25];raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=s[20];raster.frontFace=static_cast<VkFrontFace>(s[21]);raster.depthBiasEnable=s[23];raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depth.depthTestEnable=s[0];depth.depthWriteEnable=s[1];depth.depthCompareOp=static_cast<VkCompareOp>(s[2]);depth.depthBoundsTestEnable=s[24];depth.stencilTestEnable=s[3];depth.front={static_cast<VkStencilOp>(s[4]),static_cast<VkStencilOp>(s[6]),static_cast<VkStencilOp>(s[5]),static_cast<VkCompareOp>(s[7]),~0u,~0u,0};depth.back={static_cast<VkStencilOp>(s[8]),static_cast<VkStencilOp>(s[10]),static_cast<VkStencilOp>(s[9]),static_cast<VkCompareOp>(s[11]),~0u,~0u,0};depth.maxDepthBounds=1;
    std::array<VkPipelineColorBlendAttachmentState,4> blendAttachments{};for(std::size_t i=0;i<recipe.colorCount;++i)blendAttachments[i]={s[12],static_cast<VkBlendFactor>(s[13]),static_cast<VkBlendFactor>(s[14]),static_cast<VkBlendOp>(s[17]),static_cast<VkBlendFactor>(s[15]),static_cast<VkBlendFactor>(s[16]),static_cast<VkBlendOp>(s[18]),s[19]};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=recipe.colorCount;blend.pAttachments=blendAttachments.data();
    std::array<VkDynamicState,9> states{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_DEPTH_BOUNDS,VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=states.size();dynamic.pDynamicStates=states.data();
    std::array<VkFormat,4> formats{};for(std::size_t i=0;i<4;++i)formats[i]=static_cast<VkFormat>(recipe.colors[i]);
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=2;info.pStages=stages.data();info.pVertexInputState=&input;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&multisample;info.pDepthStencilState=&depth;info.pColorBlendState=&blend;info.pDynamicState=&dynamic;info.layout=gpu.pipelineLayout;info.renderPass=Pass(formats,recipe.colorCount,static_cast<VkFormat>(recipe.depthFormat));
    auto out=std::make_shared<PipelineObject>();out->lastUse=++pipelineUse;out->pinned=mandatoryKeys.contains(key);
    Check(vkCreateGraphicsPipelines(gpu.device,gpu.pipelineCache,1,&info,nullptr,&out->pipeline),"vkCreateGraphicsPipelines");
    if(initializationComplete){++runtimeCreates;if(out->pinned){++lateMandatoryCreates;__android_log_print(ANDROID_LOG_ERROR,"BL2Vulkan","Mandatory PSO residency guarantee breached: draw=%llu late=%llu",static_cast<unsigned long long>(drawEpoch),static_cast<unsigned long long>(lateMandatoryCreates));}}else ++startupCreates;
    pipelines.emplace(key,out);cacheDirty=true;return out;
}
void SaveCaches(){
    SavePostDraw();
    if(!gpu.device||!gpu.pipelineCache||gpu.cacheDirectory.empty()||!cacheDirty)return;
    bool driverWritten=false;std::size_t n=0;std::size_t driverBytes=0;auto driverStatus=vkGetPipelineCacheData(gpu.device,gpu.pipelineCache,&n,nullptr);
    if(driverStatus==VK_SUCCESS&&n<=64*1024*1024){std::vector<std::uint8_t> bytes(n+48);driverStatus=vkGetPipelineCacheData(gpu.device,gpu.pipelineCache,&n,bytes.data()+48);if(driverStatus==VK_SUCCESS){std::memcpy(bytes.data(),"BL2VKC01",8);const std::uint64_t length=n;std::memcpy(bytes.data()+8,&length,8);const auto digest=Hash(bytes.data()+48,n);std::memcpy(bytes.data()+16,digest.data(),32);driverBytes=n+48;driverWritten=AtomicFile(gpu.cacheDirectory+"/"+gpu.identity+".cache",bytes.data(),driverBytes);}}
    std::vector<std::uint8_t> bytes(48+recipes.size()*sizeof(Recipe));std::memcpy(bytes.data(),"BL2VKRP2",8);const std::uint32_t count=recipes.size(),stride=sizeof(Recipe);std::memcpy(bytes.data()+8,&count,4);std::memcpy(bytes.data()+12,&stride,4);if(count)std::memcpy(bytes.data()+48,recipes.data(),count*stride);const auto digest=Hash(bytes.data()+48,count*stride);std::memcpy(bytes.data()+16,digest.data(),32);const bool recipesWritten=AtomicFile(gpu.cacheDirectory+"/"+gpu.identity+".recipes",bytes.data(),bytes.size());cacheDirty=!(driverWritten&&recipesWritten);
    __android_log_print(cacheDirty?ANDROID_LOG_WARN:ANDROID_LOG_INFO,"BL2Vulkan","Cache persistence: identity=%s resident=%zu recipes=%zu driverResult=%d driverBytes=%zu driverWritten=%u recipeBytes=%zu recipesWritten=%u dirty=%u",gpu.identity.c_str(),pipelines.size(),recipes.size(),driverStatus,driverBytes,driverWritten,bytes.size(),recipesWritten,cacheDirty);
}
bool RecipeStages(const Recipe& r,Stage& vertex,Stage& fragment){
    std::string error;if(!gpu.pack->LookupDigest(kVS,r.vertex,vertex,&error)||!gpu.pack->LookupDigest(kFS,r.fragment,fragment,&error))return false;
    if(vertex.attribute_locations.size()!=r.attributeCount)return false;
    std::uint32_t seen=0;for(std::size_t i=0;i<r.attributeCount;++i){const auto location=r.attributes[i][0];if(location>=16||(seen&(1u<<location)))return false;seen|=1u<<location;}
    for(auto location:vertex.attribute_locations)if(location>=16||!(seen&(1u<<location)))return false;
    return true;
}
void PrewarmMandatory(const seed::Request& request){
    seed::Identity expected{seed::ParseDigest(BL2_VULKAN_BACKEND_ID),gpu.pack->Identity(),request.content,request.scope,request.receipt,(gpu.features.depthClamp?seed::kDepthClamp:0)|(gpu.features.depthBounds?seed::kDepthBounds:0)};
    const auto required=seed::Decode(request.bytes,expected);
    // Complete semantic/device/lookup validation precedes the first module/PSO.
    for(const auto& recipe:required){Stage vertex,fragment;if(!ValidRecipe(recipe)||!RecipeStages(recipe,vertex,fragment))Fail("Mandatory recipe stage/attribute/device validation failed");
        for(auto format:recipe.colors)if(format){VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(gpu.physical,static_cast<VkFormat>(format),&properties);VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;if(recipe.state[12])needed|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;if((properties.optimalTilingFeatures&needed)!=needed)Fail("Mandatory color attachment/blend format unsupported");}
        if(recipe.depthFormat){VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(gpu.physical,static_cast<VkFormat>(recipe.depthFormat),&properties);if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))Fail("Mandatory depth format unsupported");}}
    for(const auto& recipe:required){std::string key;Bytes(key,recipe);mandatoryKeys.insert(std::move(key));}
    for(const auto& recipe:required){Stage vertex,fragment;if(!RecipeStages(recipe,vertex,fragment))Fail("Mandatory stage changed during initialization");Pipeline(recipe,Module(std::move(vertex)),Module(std::move(fragment)));}
    std::size_t resident=0;for(const auto& [key,pipeline]:pipelines)if(pipeline->pinned)++resident;
    if(resident!=required.size()||mandatoryKeys.size()!=required.size())Fail("Mandatory prewarm did not prepare the complete required set");mandatoryReady=true;
}
void Prewarm(){
    recipes.clear();auto bytes=ReadFile(gpu.cacheDirectory+"/"+gpu.identity+".recipes",48+kMaxPipelines*sizeof(Recipe));if(bytes.size()<48||std::memcmp(bytes.data(),"BL2VKRP2",8)!=0)return;
    std::uint32_t count=0,stride=0;ObbSha256::Digest digest{};std::memcpy(&count,bytes.data()+8,4);std::memcpy(&stride,bytes.data()+12,4);std::memcpy(digest.data(),bytes.data()+16,32);
    if(count>kMaxPipelines||stride!=sizeof(Recipe)||bytes.size()!=48+count*stride||Hash(bytes.data()+48,count*stride)!=digest)return;
    for(std::size_t i=0;i<count;++i){Recipe recipe{};std::memcpy(&recipe,bytes.data()+48+i*stride,stride);if(seed::CanonicalRecipe(recipe)&&ValidRecipe(recipe))recipes.push_back(recipe);}
    // Mandatory objects already occupy their share of the unchanged 2048 map.
    const auto available=kMaxResidentPipelines-mandatoryKeys.size();std::size_t warmed=0;
    for(auto it=recipes.rbegin();it!=recipes.rend()&&warmed<available;++it){std::string key;Bytes(key,*it);if(pipelines.contains(key))continue;Stage vertex,fragment;if(!RecipeStages(*it,vertex,fragment))continue;Pipeline(*it,Module(std::move(vertex)),Module(std::move(fragment)));++warmed;}
    __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Prewarmed mandatory=%zu learned=%zu resident=%zu candidates=%zu before drawing",mandatoryKeys.size(),warmed,pipelines.size(),recipes.size());
}
std::size_t AttributeBytes(const Attribute& a){if(a.type==GL_INT_2_10_10_10_REV||a.type==GL_UNSIGNED_INT_2_10_10_10_REV)return 4;return a.size*(a.type==GL_BYTE||a.type==GL_UNSIGNED_BYTE?1:a.type==GL_SHORT||a.type==GL_UNSIGNED_SHORT||a.type==GL_HALF_FLOAT?2:4);}
std::array<float,4> AttributeValue(const Attribute& a,const std::uint8_t* data){
    std::array<float,4> out{0,0,0,1};
    if(a.type==GL_INT_2_10_10_10_REV||a.type==GL_UNSIGNED_INT_2_10_10_10_REV){std::uint32_t packed;std::memcpy(&packed,data,4);for(std::size_t i=0;i<4;++i){const std::uint32_t width=i==3?2:10,shift=i==3?30:i*10,mask=(1u<<width)-1;auto v=(packed>>shift)&mask;if(a.type==GL_INT_2_10_10_10_REV){auto signedValue=static_cast<std::int32_t>(v<<(32-width))>>(32-width);out[i]=a.normalized?std::max(-1.f,float(signedValue)/((1<<(width-1))-1)):float(signedValue);}else out[i]=a.normalized?float(v)/mask:float(v);}return out;}
    for(GLint i=0;i<a.size;++i)switch(a.type){case GL_FLOAT:std::memcpy(&out[i],data+i*4,4);break;case GL_FIXED:{std::int32_t v;std::memcpy(&v,data+i*4,4);out[i]=v/65536.f;break;}case GL_HALF_FLOAT:{std::uint16_t v;std::memcpy(&v,data+i*2,2);out[i]=Unhalf(v);break;}case GL_UNSIGNED_BYTE:out[i]=a.normalized?data[i]/255.f:data[i];break;case GL_BYTE:{std::int8_t v;std::memcpy(&v,data+i,1);out[i]=a.normalized?std::max(-1.f,v/127.f):v;break;}case GL_SHORT:{std::int16_t v;std::memcpy(&v,data+i*2,2);out[i]=a.normalized?std::max(-1.f,v/32767.f):v;break;}case GL_UNSIGNED_SHORT:{std::uint16_t v;std::memcpy(&v,data+i*2,2);out[i]=a.normalized?v/65535.f:v;break;}case GL_INT:{std::int32_t v;std::memcpy(&v,data+i*4,4);out[i]=a.normalized?std::max(-1.,double(v)/2147483647.):v;break;}case GL_UNSIGNED_INT:{std::uint32_t v;std::memcpy(&v,data+i*4,4);out[i]=a.normalized?double(v)/4294967295.:v;break;}default:Fail("Unsupported vertex attribute decoding");}return out;
}
VkFormat AttributeFormat(const Attribute& a){
    auto n=a.size-1;Require(n>=0&&n<4);
    if(a.integer)return VK_FORMAT_UNDEFINED;
    switch(a.type){
        case GL_FLOAT:return std::array<VkFormat,4>{VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT}[n];
        case GL_HALF_FLOAT:return std::array<VkFormat,4>{VK_FORMAT_R16_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16B16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT}[n];
        case GL_UNSIGNED_BYTE:return a.normalized?std::array<VkFormat,4>{VK_FORMAT_R8_UNORM,VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8A8_UNORM}[n]:std::array<VkFormat,4>{VK_FORMAT_R8_USCALED,VK_FORMAT_R8G8_USCALED,VK_FORMAT_R8G8B8_USCALED,VK_FORMAT_R8G8B8A8_USCALED}[n];
        case GL_BYTE:return a.normalized?std::array<VkFormat,4>{VK_FORMAT_R8_SNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8B8_SNORM,VK_FORMAT_R8G8B8A8_SNORM}[n]:std::array<VkFormat,4>{VK_FORMAT_R8_SSCALED,VK_FORMAT_R8G8_SSCALED,VK_FORMAT_R8G8B8_SSCALED,VK_FORMAT_R8G8B8A8_SSCALED}[n];
        case GL_SHORT:return a.normalized?std::array<VkFormat,4>{VK_FORMAT_R16_SNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16B16_SNORM,VK_FORMAT_R16G16B16A16_SNORM}[n]:std::array<VkFormat,4>{VK_FORMAT_R16_SSCALED,VK_FORMAT_R16G16_SSCALED,VK_FORMAT_R16G16B16_SSCALED,VK_FORMAT_R16G16B16A16_SSCALED}[n];
        case GL_UNSIGNED_SHORT:return a.normalized?std::array<VkFormat,4>{VK_FORMAT_R16_UNORM,VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16A16_UNORM}[n]:std::array<VkFormat,4>{VK_FORMAT_R16_USCALED,VK_FORMAT_R16G16_USCALED,VK_FORMAT_R16G16B16_USCALED,VK_FORMAT_R16G16B16A16_USCALED}[n];
        default:return VK_FORMAT_UNDEFINED;
    }
}
struct ConsumerTargetPlan {
    std::size_t rowBytes{},imageBytes{},totalBytes{};
    std::uint32_t rowsPerTile{},tilesPerImage{};
};
std::uint32_t ConsumerTargetPixelBytes(VkFormat format){
    return format==VK_FORMAT_R16G16_SFLOAT?4u:format==VK_FORMAT_R16G16B16A16_SFLOAT?8u:0u;
}
bool ConsumerAfterImagePairAvailable(const RecordedColorImage& source,const RecordedColorImage& target){
    return RecordedColorScope(true,source.type,source.layers,source.aspect,source.mip,source.layer)&&
        RecordedColorScope(true,target.type,target.layers,target.aspect,target.mip,target.layer)&&
        source.image&&target.image&&source.image!=target.image&&source.writer&&target.writer&&
        source.writer->imageId&&target.writer->imageId&&source.writer->imageId!=target.writer->imageId&&
        source.width&&source.height&&source.width==target.width&&source.height==target.height&&
        source.format==target.format&&ConsumerTargetPixelBytes(source.format);
}
bool ConsumerAfterImagePairSelfCheck(){
    RecordedImageWriter sourceWriter,targetWriter;sourceWriter.imageId=1;targetWriter.imageId=2;
    RecordedColorImage source{VK_IMAGE_TYPE_2D,1,VK_IMAGE_ASPECT_COLOR_BIT,VK_FORMAT_R16G16_SFLOAT,600,272,11,0,0,&sourceWriter};
    auto target=source;target.image=12;target.writer=&targetWriter;
    if(!ConsumerAfterImagePairAvailable(source,target))return false;
    auto invalid=source;invalid.image=target.image;if(ConsumerAfterImagePairAvailable(invalid,target))return false;
    sourceWriter.imageId=targetWriter.imageId;if(ConsumerAfterImagePairAvailable(source,target))return false;sourceWriter.imageId=1;
    invalid=source;invalid.writer=nullptr;if(ConsumerAfterImagePairAvailable(invalid,target))return false;
    invalid=source;invalid.format=VK_FORMAT_R8G8B8A8_UNORM;if(ConsumerAfterImagePairAvailable(invalid,target))return false;
    invalid=source;invalid.width=601;if(ConsumerAfterImagePairAvailable(invalid,target))return false;
    invalid=source;invalid.mip=1;if(ConsumerAfterImagePairAvailable(invalid,target))return false;
    invalid=target;invalid.layer=1;if(ConsumerAfterImagePairAvailable(source,invalid))return false;
    source.format=target.format=VK_FORMAT_R16G16B16A16_SFLOAT;return ConsumerAfterImagePairAvailable(source,target);
}
bool PlanConsumerTarget(std::uint32_t width,std::uint32_t height,std::size_t atlasBytes,std::size_t snapshots,std::uint32_t atlasFiles,std::uint32_t payloadFiles,std::size_t diagnosticBytes,ConsumerTargetPlan& plan,VkFormat format=VK_FORMAT_R16G16B16A16_SFLOAT){
    plan={};constexpr std::size_t maxFile=4*1024*1024,maxAtlas=16*1024*1024,maxPayload=31*1024*1024,reportReserve=65536;
    const auto pixelBytes=ConsumerTargetPixelBytes(format);
    if(!pixelBytes||!width||!height||atlasBytes>maxAtlas||snapshots>6||atlasFiles>32||payloadFiles>471||diagnosticBytes>maxPayload-reportReserve)return false;
    const auto rowBytes=std::uint64_t(width)*pixelBytes;if(rowBytes>maxFile)return false;
    const auto rowsPerTile=maxFile/rowBytes,tiles=(std::uint64_t(height)+rowsPerTile-1)/rowsPerTile;
    const auto imageBytes=rowBytes*height,totalBytes=imageBytes*2;
    if(totalBytes>maxAtlas-atlasBytes||tiles*2>32-atlasFiles||tiles*2>471-payloadFiles||totalBytes>maxPayload-reportReserve-diagnosticBytes)return false;
    plan={static_cast<std::size_t>(rowBytes),static_cast<std::size_t>(imageBytes),static_cast<std::size_t>(totalBytes),static_cast<std::uint32_t>(rowsPerTile),static_cast<std::uint32_t>(tiles)};return true;
}
bool ConsumerRange(std::uint64_t total,std::uint64_t offset,std::uint64_t bytes){return bytes&&bytes<=65536&&offset<=total&&bytes<=total-offset;}
bool ConsumerSamples(const void* data,std::uint64_t total,std::uint64_t offset,std::size_t stride,std::size_t bytes,const std::vector<std::uint32_t>& references,std::uint32_t minimum,std::vector<std::uint8_t>& out){
    out.clear();if(!data||references.empty()||references.size()>6||!stride||stride>2048||!bytes||bytes>16)return false;
    for(auto reference:references){if(reference<minimum){out.clear();return false;}const auto relative=std::uint64_t(reference-minimum)*stride;
        if(offset>total||relative>total-offset||!ConsumerRange(total,offset+relative,bytes)){out.clear();return false;}
        const auto* source=static_cast<const std::uint8_t*>(data)+static_cast<std::size_t>(offset+relative);out.insert(out.end(),source,source+bytes);
    }return true;
}
bool ConsumerReportGrowth(std::size_t used,std::size_t previous,std::size_t current,std::size_t& extra){
    extra=0;if(current>65536||previous>current||used>32*1024*1024)return false;extra=current-previous;return extra<=32*1024*1024-used;
}
// Only the existing selected capture owns this scope; normal draws allocate no probe.
struct ConsumerCapture {
    std::uint32_t number{};std::uint64_t epoch{};
    GLsizei count{},instances{};GLenum indexType{};GLint baseVertex{};
    std::string prefix,report;std::size_t reportAccounted{};
    std::vector<std::uint32_t> references;std::vector<std::uint8_t> indexCpu;
    std::array<std::vector<std::uint8_t>,16> attributeCpu;
    Attachment target;std::shared_ptr<Image> sampledImage;GLuint sampledName{};ConsumerTargetPlan plan;std::array<std::string,2> targetKeys;
    bool planned{},sourceComplete{},afterComplete{},indexObserved{};
    std::uint32_t vertexObserved{},vertexExpected{};
};
void DiagnosticLine(std::string& text,const char* format,...) __attribute__((format(printf,2,3)));
void CompleteConsumerCapture(ConsumerCapture& capture);
void ObserveConsumerVertex(ConsumerCapture& capture,std::uint32_t location,std::uint32_t binding,const Attribute& attribute,const std::shared_ptr<Allocation>& allocation,VkDeviceSize cpuOffset,VkDeviceSize offset,std::uint32_t stride,std::uint32_t minimum,std::uint32_t maximum,VkFormat format){
    std::vector<std::uint8_t> cpu,bound;const auto bytes=AttributeBytes(attribute);
    const bool cpuPresent=ConsumerSamples(attribute.buffer->bytes.data(),attribute.buffer->bytes.size(),cpuOffset,stride,bytes,capture.references,minimum,cpu);
    const bool boundPresent=ConsumerSamples(allocation->mapped,allocation->size,offset,stride,bytes,capture.references,minimum,bound);
    const auto& captured=capture.attributeCpu[location];const bool complete=!attribute.divisor&&capture.instances==1&&capture.count==static_cast<GLsizei>(capture.references.size())&&captured.size()==capture.references.size()*bytes&&cpuPresent&&boundPresent;
    if(complete)++capture.vertexObserved;
    const auto span=std::uint64_t(maximum-minimum)*stride+bytes;const bool spanPresent=ConsumerRange(attribute.buffer->bytes.size(),cpuOffset,span)&&ConsumerRange(allocation->size,offset,span)&&allocation->mapped;
    DiagnosticLine(capture.report,"boundVertex location=%u binding=%u buffer=%u vkBuffer=%llx allocation=%p allocationBytes=%llu snapshotEpoch=%llu offset=%llu cpuOffset=%llu fullBufferSnapshot=%u stride=%u format=%u rate=%s minimum=%u maximum=%u samples=%zu sampleBytes=%zu spanBytes=%llu fullDraw=%u complete=%u capturedCpuSha256=%s currentCpuSha256=%s mappedGpuSha256=%s capturedEqualsGpu=%u currentEqualsGpu=%u capturedEqualsCurrent=%u cpuSpanSha256=%s mappedSpanSha256=%s\n",location,binding,attribute.name,static_cast<unsigned long long>(allocation->buffer),allocation.get(),static_cast<unsigned long long>(allocation->size),static_cast<unsigned long long>(allocation==attribute.buffer->allocation?attribute.buffer->snapshotEpoch:drawEpoch),static_cast<unsigned long long>(offset),static_cast<unsigned long long>(cpuOffset),unsigned(allocation==attribute.buffer->allocation),stride,format,attribute.divisor?"instance":"vertex",minimum,maximum,capture.references.size(),bytes,static_cast<unsigned long long>(span),capture.count==static_cast<GLsizei>(capture.references.size()),complete,Hex(Hash(captured.data(),captured.size())).c_str(),cpuPresent?Hex(Hash(cpu.data(),cpu.size())).c_str():"unavailable",boundPresent?Hex(Hash(bound.data(),bound.size())).c_str():"unavailable",complete&&captured==bound,complete&&cpu==bound,complete&&captured==cpu,spanPresent?Hex(Hash(attribute.buffer->bytes.data()+cpuOffset,span)).c_str():"unavailable",spanPresent?Hex(Hash(static_cast<const std::uint8_t*>(allocation->mapped)+offset,span)).c_str():"unavailable");
}
void ObserveConsumerIndex(ConsumerCapture& capture,const std::shared_ptr<Allocation>& allocation,VkDeviceSize cpuOffset,VkDeviceSize offset,VkIndexType type,std::uint32_t minimum,std::uint32_t maximum){
    const auto& elements=Vao().elements;const auto bytes=std::uint64_t(capture.count)*(capture.indexType==GL_UNSIGNED_SHORT?2u:4u);
    const bool complete=capture.instances==1&&capture.count==static_cast<GLsizei>(capture.references.size())&&capture.indexCpu.size()==bytes&&allocation->mapped&&ConsumerRange(allocation->size,offset,bytes)&&ConsumerRange(elements->bytes.size(),cpuOffset,bytes);capture.indexObserved=complete;
    const auto* cpu=complete?elements->bytes.data()+cpuOffset:nullptr;const auto* bound=complete?static_cast<const std::uint8_t*>(allocation->mapped)+offset:nullptr;
    DiagnosticLine(capture.report,"boundIndex buffer=%u vkBuffer=%llx allocation=%p allocationBytes=%llu snapshotEpoch=%llu offset=%llu cpuOffset=%llu fullBufferSnapshot=%u indexType=%u originalIndexType=%x count=%d instances=%d baseVertex=%d vertexOffset=%d minimum=%u maximum=%u bytes=%llu complete=%u capturedCpuSha256=%s currentCpuSha256=%s mappedGpuSha256=%s capturedEqualsGpu=%u currentEqualsGpu=%u capturedEqualsCurrent=%u\n",Vao().elementName,static_cast<unsigned long long>(allocation->buffer),allocation.get(),static_cast<unsigned long long>(allocation->size),static_cast<unsigned long long>(allocation==elements->allocation?elements->snapshotEpoch:drawEpoch),static_cast<unsigned long long>(offset),static_cast<unsigned long long>(cpuOffset),unsigned(allocation==elements->allocation),type,capture.indexType,capture.count,capture.instances,capture.baseVertex,static_cast<std::int32_t>(static_cast<std::int64_t>(capture.baseVertex)-minimum),minimum,maximum,static_cast<unsigned long long>(bytes),complete,Hex(Hash(capture.indexCpu.data(),capture.indexCpu.size())).c_str(),complete?Hex(Hash(cpu,bytes)).c_str():"unavailable",complete?Hex(Hash(bound,bytes)).c_str():"unavailable",complete&&!std::memcmp(capture.indexCpu.data(),bound,bytes),complete&&!std::memcmp(cpu,bound,bytes),complete&&!std::memcmp(capture.indexCpu.data(),cpu,bytes));
}
struct VertexInputs {
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkBuffer> buffers;std::vector<VkDeviceSize> offsets;
};
VertexInputs Vertices(const Stage& stage,std::uint32_t minimum,std::uint32_t maximum,std::uint32_t instances,ConsumerCapture* consumer){
    VertexInputs out;
    for(auto location:stage.attribute_locations){Require(location<16,GL_INVALID_OPERATION);const auto& a=Vao().attributes[location];const std::uint32_t binding=out.bindings.size();VkFormat format=VK_FORMAT_R32G32B32A32_SFLOAT;std::uint32_t stride=0;VkBuffer buffer{};VkDeviceSize offset=0;const auto rate=a.divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX;
        if(!a.enabled){auto slice=Upload(16,16);std::memcpy(slice.data,a.constant.data(),16);buffer=slice.buffer->buffer;offset=slice.offset;}
        else {Require(!a.integer,GL_INVALID_OPERATION);const auto bytes=AttributeBytes(a),sourceStride=a.stride?static_cast<std::size_t>(a.stride):bytes;const auto first=a.divisor?0u:minimum,last=a.divisor?(instances-1)/a.divisor:maximum;
            Require(last>=first&&static_cast<std::uint64_t>(last)*sourceStride+bytes<=kMaxBuffer,GL_INVALID_OPERATION);
            const auto start=static_cast<std::uint64_t>(a.pointer)+static_cast<std::uint64_t>(first)*sourceStride,end=static_cast<std::uint64_t>(a.pointer)+static_cast<std::uint64_t>(last)*sourceStride+bytes;
            const std::uint8_t* source;
            if(a.buffer){Require(end<=a.buffer->bytes.size(),GL_INVALID_OPERATION);Require(!a.buffer->mapped||(a.buffer->access&GL_MAP_PERSISTENT_BIT),GL_INVALID_OPERATION);source=a.buffer->bytes.data()+static_cast<std::size_t>(start);}
            else {Require(a.pointer&&start<=std::numeric_limits<std::uintptr_t>::max()&&end<=std::numeric_limits<std::uintptr_t>::max(),GL_INVALID_OPERATION);source=reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(start));}
            format=AttributeFormat(a);VkFormatProperties properties{};if(format)vkGetPhysicalDeviceFormatProperties(gpu.physical,format,&properties);
            const auto component=a.type==GL_BYTE||a.type==GL_UNSIGNED_BYTE?1u:a.type==GL_HALF_FLOAT||a.type==GL_SHORT||a.type==GL_UNSIGNED_SHORT?2u:4u;
            const bool direct=a.buffer&&a.divisor<=1&&format&&(properties.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)&&start%component==0&&sourceStride%component==0&&sourceStride<=2048;
            if(direct){auto slice=BufferRangeGpu(a.buffer,start,end-start);buffer=slice.buffer->buffer;offset=slice.offset;stride=sourceStride;if(consumer)ObserveConsumerVertex(*consumer,location,binding,a,slice.buffer,start,offset,stride,minimum,maximum,format);}
            else {const auto count=a.divisor?instances:maximum-minimum+1;Require(count<=kMaxBuffer/16,GL_OUT_OF_MEMORY);auto slice=Upload(static_cast<std::size_t>(count)*16,16);auto* destination=static_cast<std::uint8_t*>(slice.data);for(std::uint32_t i=0;i<count;++i){const auto value=AttributeValue(a,source+static_cast<std::size_t>(a.divisor?i/a.divisor:i)*sourceStride);std::memcpy(destination+i*16,value.data(),16);}buffer=slice.buffer->buffer;offset=slice.offset;stride=16;format=VK_FORMAT_R32G32B32A32_SFLOAT;if(consumer)DiagnosticLine(consumer->report,"boundVertex location=%u binding=%u complete=0 status=converted-or-client-input\n",location,binding);}
        }
        if(consumer&&!a.enabled)DiagnosticLine(consumer->report,"boundVertex location=%u binding=%u complete=0 status=constant-attribute\n",location,binding);
        out.attributes.push_back({location,binding,format,0});out.bindings.push_back({binding,stride,rate});out.buffers.push_back(buffer);out.offsets.push_back(offset);
    }
    return out;
}
VkRect2D ClipScissor(VkExtent2D extent){auto& c=C();const auto x0=c.enabled.contains(GL_SCISSOR_TEST)?std::clamp<std::int64_t>(c.scissor[0],0,extent.width):0,y0=c.enabled.contains(GL_SCISSOR_TEST)?std::clamp<std::int64_t>(c.scissor[1],0,extent.height):0;const auto x1=c.enabled.contains(GL_SCISSOR_TEST)?std::clamp<std::int64_t>(static_cast<std::int64_t>(c.scissor[0])+c.scissor[2],x0,extent.width):extent.width,y1=c.enabled.contains(GL_SCISSOR_TEST)?std::clamp<std::int64_t>(static_cast<std::int64_t>(c.scissor[1])+c.scissor[3],y0,extent.height):extent.height;return {{static_cast<GLint>(x0),static_cast<GLint>(y0)},{static_cast<std::uint32_t>(x1-x0),static_cast<std::uint32_t>(y1-y0)}};}
VkRect2D ClipScissor(const Targets& targets){return ClipScissor(VkExtent2D{targets.width,targets.height});}
std::uint32_t CaptureGfx(const Program& vertex,const Program& fragment,GLenum mode,GLint first,GLsizei count,GLenum indexType,const void* indexPointer,GLsizei instances,GLint baseVertex,std::uint32_t minimum,std::uint32_t maximum,std::unique_ptr<ConsumerCapture>& consumer);
void Draw(GLenum mode,GLint first,GLsizei count,GLenum indexType,const void* indexPointer,GLsizei instances,GLint baseVertex){
    Require(!gpu.failed,GL_INVALID_OPERATION);Require(count>=0&&instances>=0&&first>=0);Topology(mode);if(!count||!instances)return;
    auto& initialFrame=Recording();std::size_t used=0;for(auto& chunk:initialFrame.chunks)used+=chunk.used;
    if(initialFrame.drawCount>=3500||used>=24*1024*1024)Submit(false);
    ++Recording().drawCount;
    ++drawEpoch;
    auto& c=C();Require(c.vertexProgram&&c.fragmentProgram,GL_INVALID_OPERATION);auto vi=c.share->programs.find(c.vertexProgram),fi=c.share->programs.find(c.fragmentProgram);Require(vi!=c.share->programs.end()&&fi!=c.share->programs.end()&&vi->second->shader&&fi->second->shader,GL_INVALID_OPERATION);auto vertex=vi->second,fragment=fi->second;
    std::uint32_t minimum=first,maximum=static_cast<std::uint64_t>(first)+count-1;Require(static_cast<std::uint64_t>(first)+count<=std::numeric_limits<std::uint32_t>::max());
    const bool indexed=indexType!=0;bool directIndices=false;VkBuffer indices{};VkDeviceSize indexOffset{};VkIndexType vkIndex=VK_INDEX_TYPE_UINT32;std::vector<std::uint32_t> expansion;
    if(indexed){Require(indexType==GL_UNSIGNED_BYTE||indexType==GL_UNSIGNED_SHORT||indexType==GL_UNSIGNED_INT,GL_INVALID_ENUM);const auto indexBytes=indexType==GL_UNSIGNED_BYTE?1u:indexType==GL_UNSIGNED_SHORT?2u:4u;const auto byteCount=static_cast<std::uint64_t>(count)*indexBytes;Require(byteCount<=kMaxBuffer);const auto& elements=Vao().elements;const std::uint8_t* source;
        if(elements){auto offset=reinterpret_cast<std::uintptr_t>(indexPointer);Require(offset<=elements->bytes.size()&&byteCount<=elements->bytes.size()-offset,GL_INVALID_OPERATION);source=elements->bytes.data()+offset;}else {Require(indexPointer,GL_INVALID_OPERATION);source=static_cast<const std::uint8_t*>(indexPointer);}
        directIndices=mode!=0x0007&&mode!=GL_LINE_LOOP&&indexType!=GL_UNSIGNED_BYTE&&elements&&reinterpret_cast<std::uintptr_t>(indexPointer)%indexBytes==0;
        const std::uint32_t sentinel=indexBytes==1?255u:indexBytes==2?65535u:~0u;const bool restart=c.enabled.contains(GL_PRIMITIVE_RESTART_FIXED_INDEX)&&(mode==GL_LINE_STRIP||mode==GL_TRIANGLE_STRIP||mode==GL_TRIANGLE_FAN);
        minimum=~0u;maximum=0;if(!directIndices)expansion.reserve(count);
        for(GLsizei i=0;i<count;++i){std::uint32_t value=0;std::memcpy(&value,source+static_cast<std::size_t>(i)*indexBytes,indexBytes);if(restart&&value==sentinel){if(!directIndices)expansion.push_back(~0u);continue;}const auto adjusted=static_cast<std::int64_t>(value)+baseVertex;Require(adjusted>=0&&adjusted<=std::numeric_limits<std::uint32_t>::max(),GL_INVALID_OPERATION);minimum=std::min(minimum,static_cast<std::uint32_t>(adjusted));maximum=std::max(maximum,static_cast<std::uint32_t>(adjusted));if(!directIndices)expansion.push_back(value);}
        if(minimum==~0u)minimum=maximum=0;
    }
    // Diagnostic readbacks can submit the current frame. Run them before any
    // buffer allocation/snapshot belonging to this draw is retained or uploaded.
    std::unique_ptr<ConsumerCapture> consumer;
    const auto capture=gpu.gfxCapture?CaptureGfx(*vertex,*fragment,mode,first,count,indexType,indexPointer,instances,baseVertex,minimum,maximum,consumer):0;
    if(consumer&&consumer->planned){
        std::array<char,PROP_VALUE_MAX> boundaryProperty{};__system_property_get("debug.bl2.vk_ao_boundary",boundaryProperty.data());
        const bool split=std::strcmp(boundaryProperty.data(),"1")==0;
        DiagnosticLine(consumer->report,"boundaryMode split=%u property=debug.bl2.vk_ao_boundary default=0\n",split);
        if(split){
        const auto& next=*c.frames[(c.frameIndex+1)%c.frames.size()];
        const auto nextStatus=next.submitted?vkGetFenceStatus(gpu.device,next.fence):VK_SUCCESS;
        Require(nextStatus==VK_SUCCESS||nextStatus==VK_NOT_READY,GL_INVALID_OPERATION);
        DiagnosticLine(consumer->report,"boundaryNextSlot submitted=%u recording=%u fenceResult=%d gpuStatusQuery=%u\n",next.submitted,next.recording,nextStatus,next.submitted);
        __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","AO boundary next slot: draw=%llu submitted=%u recording=%u fenceResult=%d",static_cast<unsigned long long>(drawEpoch),next.submitted,next.recording,nextStatus);
        Submit(false);
        DiagnosticLine(consumer->report,"boundaryExperiment=submit-without-wait beforeDrawResources=1\n");
        __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","AO boundary experiment: draw=%llu submitWait=0 beforeDrawResources=1",static_cast<unsigned long long>(drawEpoch));
        }
    }
    if(directIndices){const auto cpuOffset=reinterpret_cast<std::uintptr_t>(indexPointer);auto slice=BufferRangeGpu(Vao().elements,cpuOffset,static_cast<std::size_t>(count)*(indexType==GL_UNSIGNED_SHORT?2u:4u));indices=slice.buffer->buffer;indexOffset=slice.offset;vkIndex=indexType==GL_UNSIGNED_SHORT?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;if(consumer)ObserveConsumerIndex(*consumer,slice.buffer,cpuOffset,indexOffset,vkIndex,minimum,maximum);}
    if(mode==0x0007){std::vector<std::uint32_t> quad;const auto n=count/4;quad.reserve(n*6);for(GLsizei i=0;i<n;++i){auto at=[&](int offset){return indexed?expansion[i*4+offset]:static_cast<std::uint32_t>(first+i*4+offset);};quad.insert(quad.end(),{at(0),at(1),at(2),at(0),at(2),at(3)});}expansion=std::move(quad);mode=GL_TRIANGLES;}
    else if(mode==GL_LINE_LOOP){if(!indexed){expansion.reserve(count+1);for(GLsizei i=0;i<count;++i)expansion.push_back(first+i);}expansion.push_back(expansion.front());mode=GL_LINE_STRIP;}
    if(!expansion.empty()){auto slice=Upload(expansion.size()*4,4);std::memcpy(slice.data,expansion.data(),expansion.size()*4);indices=slice.buffer->buffer;indexOffset=slice.offset;vkIndex=VK_INDEX_TYPE_UINT32;count=expansion.size();}
    auto targets=Target(c.drawFbo);auto inputs=Vertices(vertex->shader->stage,minimum,maximum,instances,consumer.get());auto recipe=MakeRecipe(*vertex,*fragment,targets,mode,inputs.attributes,inputs.bindings);auto pipeline=Pipeline(recipe,vertex->shader,fragment->shader);auto descriptors=Descriptors(*vertex,*fragment,targets,consumer?&consumer->report:nullptr);if(consumer)DiagnosticLine(consumer->report,"boundDraw pipeline=%llx descriptorSet=%llx drawFbo=%u targetExtent=%u,%u indexed=%u indexOffset=%llu vertexOffset=%d\n",static_cast<unsigned long long>(pipeline->pipeline),static_cast<unsigned long long>(descriptors),c.drawFbo,targets.width,targets.height,bool(indices),static_cast<unsigned long long>(indexOffset),static_cast<std::int32_t>(static_cast<std::int64_t>(indexed?baseVertex:0)-minimum));BeginPass(targets);Retain(pipeline);Retain(vertex->shader);Retain(fragment->shader);
    auto command=Recording().command;vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,gpu.pipelineLayout,0,1,&descriptors,0,nullptr);
    if(!inputs.buffers.empty())vkCmdBindVertexBuffers(command,0,inputs.buffers.size(),inputs.buffers.data(),inputs.offsets.data());
    VkViewport viewport{static_cast<float>(c.viewport[0]),static_cast<float>(c.viewport[1]),static_cast<float>(std::max(c.viewport[2],1)),static_cast<float>(std::max(c.viewport[3],1)),c.nearDepth,c.farDepth};vkCmdSetViewport(command,0,1,&viewport);auto scissor=ClipScissor(targets);if(!c.viewport[2]||!c.viewport[3])scissor.extent={0,0};vkCmdSetScissor(command,0,1,&scissor);
    vkCmdSetDepthBias(command,c.polygonUnits,0,c.polygonFactor);vkCmdSetBlendConstants(command,c.blendColor.data());vkCmdSetStencilCompareMask(command,VK_STENCIL_FACE_FRONT_BIT,c.frontStencil.compareMask);vkCmdSetStencilCompareMask(command,VK_STENCIL_FACE_BACK_BIT,c.backStencil.compareMask);vkCmdSetStencilWriteMask(command,VK_STENCIL_FACE_FRONT_BIT,c.frontStencil.writeMask);vkCmdSetStencilWriteMask(command,VK_STENCIL_FACE_BACK_BIT,c.backStencil.writeMask);vkCmdSetStencilReference(command,VK_STENCIL_FACE_FRONT_BIT,c.frontStencil.reference);vkCmdSetStencilReference(command,VK_STENCIL_FACE_BACK_BIT,c.backStencil.reference);if(gpu.features.depthBounds)vkCmdSetDepthBounds(command,c.minDepthBounds,c.maxDepthBounds);vkCmdSetLineWidth(command,c.lineWidth);
    if(indices){vkCmdBindIndexBuffer(command,indices,indexOffset,vkIndex);vkCmdDrawIndexed(command,count,instances,0,static_cast<std::int64_t>(indexed?baseVertex:0)-minimum,0);}else vkCmdDraw(command,count,instances,0,0);
    if(gpu.gfxCapture&&c.viewport[2]&&c.viewport[3]&&scissor.extent.width&&scissor.extent.height&&!c.enabled.contains(GL_RASTERIZER_DISCARD)){
        const auto mask=static_cast<GLuint>((c.colorWrite[0]?1:0)|(c.colorWrite[1]?2:0)|(c.colorWrite[2]?4:0)|(c.colorWrite[3]?8:0));
        if(mask)for(std::uint32_t i=0;i<targets.colorCount;++i)if(targets.colorViews[i]){const auto& a=targets.colors[i];RecordColorWrite(a.texture->image,a.level,a.layer,{.operation=RecordedColorOperation::Draw,.vertex=vertex->shader->stage.normalizedDigest,.fragment=fragment->shader->stage.normalizedDigest,.rectangle={scissor.offset.x,scissor.offset.y,static_cast<GLint>(scissor.extent.width),static_cast<GLint>(scissor.extent.height)},.framebuffer=c.drawFbo,.colorMask=mask,.blendEnabled=c.enabled.contains(GL_BLEND),.blend={c.srcRgb,c.dstRgb,c.srcAlpha,c.dstAlpha,c.rgbEquation,c.alphaEquation}});}
    }
    RememberRecordedDraw(recipe,pipeline);
    if(capture)__android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","Captured Vulkan draw recorded: capture=%u draw=%llu V=%u P=%u indexed=%u count=%d instances=%d",capture,static_cast<unsigned long long>(drawEpoch),c.vertexProgram,c.fragmentProgram,bool(indices),count,instances);
    if(consumer)CompleteConsumerCapture(*consumer);
}
template<class F> void DrawCall(const char* operation,F&& function){Call([&]{try{function();}catch(const InvalidGlArgument& error){++rejectedDraws;if(postDrawCapture)postDrawDirty=true;if(gpu.gfxCapture&&gpu.drawErrorReports<16){++gpu.drawErrorReports;__android_log_print(ANDROID_LOG_ERROR,"BL2Vulkan","Rejected Vulkan draw: operation=%s error=0x%04x rendererLine=%u draw=%llu V=%u P=%u reports=%u",operation,error.error,error.line,static_cast<unsigned long long>(drawEpoch),current?current->vertexProgram:0,current?current->fragmentProgram:0,gpu.drawErrorReports);}throw;}});}
}
void DrawArrays(GLenum mode,GLint first,GLsizei count){DrawCall("glDrawArrays",[&]{Draw(mode,first,count,0,nullptr,1,0);});}
void DrawArraysInstanced(GLenum mode,GLint first,GLsizei count,GLsizei instances){DrawCall("glDrawArraysInstanced",[&]{Draw(mode,first,count,0,nullptr,instances,0);});}
void DrawElements(GLenum mode,GLsizei count,GLenum type,const void* indices){DrawCall("glDrawElements",[&]{Draw(mode,0,count,type,indices,1,0);});}
void DrawElementsBaseVertex(GLenum mode,GLsizei count,GLenum type,const void* indices,GLint baseVertex){DrawCall("glDrawElementsBaseVertex",[&]{Draw(mode,0,count,type,indices,1,baseVertex);});}
void DrawElementsInstancedBaseVertex(GLenum mode,GLsizei count,GLenum type,const void* indices,GLsizei instances,GLint baseVertex){DrawCall("glDrawElementsInstancedBaseVertex",[&]{Draw(mode,0,count,type,indices,instances,baseVertex);});}

namespace {
bool Capability(GLenum cap){return cap==GL_BLEND||cap==GL_CULL_FACE||cap==GL_DEPTH_TEST||cap==GL_STENCIL_TEST||cap==GL_SCISSOR_TEST||cap==GL_POLYGON_OFFSET_FILL||cap==GL_DITHER||cap==0x809D||cap==GL_SAMPLE_ALPHA_TO_COVERAGE||cap==GL_SAMPLE_COVERAGE||cap==GL_RASTERIZER_DISCARD||cap==GL_PRIMITIVE_RESTART_FIXED_INDEX||cap==0x864F||cap==0x8890||cap==0x8DB9||cap==kVS||cap==kFS;}
std::vector<std::uint8_t> Download(const Attachment& a,VkImageAspectFlagBits aspect,GLint x,GLint y,GLsizei w,GLsizei h){
    Require(a.texture&&a.texture->image&&x>=0&&y>=0&&w>0&&h>0,GL_INVALID_OPERATION);auto image=a.texture->image;Require(x+w<=static_cast<GLint>(std::max(1u,image->width>>a.level))&&y+h<=static_cast<GLint>(std::max(1u,image->height>>a.level)));
    auto block=FormatBlock(image->format);Require(block.x==1&&block.y==1,GL_INVALID_OPERATION);const auto bytes=static_cast<std::size_t>(w)*h*(aspect==VK_IMAGE_ASPECT_STENCIL_BIT?1:aspect==VK_IMAGE_ASPECT_DEPTH_BIT&&image->format==VK_FORMAT_D32_SFLOAT_S8_UINT?4:block.bytes);auto allocation=Allocate(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);Transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,a.level,1,a.layer,1);
    VkBufferImageCopy copy{};copy.imageSubresource={aspect,static_cast<std::uint32_t>(a.level),static_cast<std::uint32_t>(a.layer),1};copy.imageOffset={x,y,0};copy.imageExtent={static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(h),1};vkCmdCopyImageToBuffer(Recording().command,image->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,allocation->buffer,1,&copy);Retain(allocation);Submit(true);std::vector<std::uint8_t> out(bytes);std::memcpy(out.data(),allocation->mapped,bytes);return out;
}
void DiagnosticLine(std::string& text,const char* format,...) __attribute__((format(printf,2,3)));
void DiagnosticLine(std::string& text,const char* format,...){
    std::array<char,4096> line{};va_list arguments;va_start(arguments,format);const int size=std::vsnprintf(line.data(),line.size(),format,arguments);va_end(arguments);
    if(size>0)text.append(line.data(),std::min<std::size_t>(size,line.size()-1));
}
const char* RecordedOperationName(RecordedColorOperation operation){
    switch(operation){case RecordedColorOperation::Draw:return "draw";case RecordedColorOperation::Clear:return "clear";case RecordedColorOperation::Upload:return "upload";case RecordedColorOperation::RawUpload:return "raw-upload";case RecordedColorOperation::Blit:return "blit";case RecordedColorOperation::CopyImage:return "copy-image";default:return "unknown";}
}
void DiagnosticColorWrite(std::string& report,const char* label,const RecordedColorWrite& write){
    DiagnosticLine(report,"%s operation=%s sequence=%llu draw=%llu V=%s P=%s possibleRect=%d,%d,%d,%d fbo=%u colorMask=%x blendEnabled=%u blend=%x,%x,%x,%x,%x,%x upload=%x,%x clear=%.9g,%.9g,%.9g,%.9g\n",label,RecordedOperationName(write.operation),static_cast<unsigned long long>(write.sequence),static_cast<unsigned long long>(write.draw),Hex(write.vertex).c_str(),Hex(write.fragment).c_str(),write.rectangle[0],write.rectangle[1],write.rectangle[2],write.rectangle[3],write.framebuffer,write.colorMask,static_cast<unsigned>(write.blendEnabled),write.blend[0],write.blend[1],write.blend[2],write.blend[3],write.blend[4],write.blend[5],write.uploadFormat,write.uploadType,write.clearValue[0],write.clearValue[1],write.clearValue[2],write.clearValue[3]);
}
void DiagnosticImageWriter(std::string& report,const std::shared_ptr<Image>& image,std::uint32_t base){
    const bool scope=RecordedColorScope(gpu.gfxCapture,image->type,image->layers,image->aspect,base,0);
    DiagnosticLine(report,"imageWriter scope=color2D-mip0-layer0 status=%s interpretation=last-recorded-command coverageUnproven=1 image=%llx mip=%u layer=0\n",scope?"in-scope":"unknown-subresource",static_cast<unsigned long long>(image->image),base);
    if(!scope)return;const auto& writer=ImageWriter(*image);
    DiagnosticLine(report,"imageWriter imageId=%llu sourceImage=%llx sourceImageId=%llu sourceMip=%d sourceLayer=%d sourcePossibleRect=%d,%d,%d,%d sourceKnown=%u\n",static_cast<unsigned long long>(writer.imageId),static_cast<unsigned long long>(writer.sourceImage),static_cast<unsigned long long>(writer.sourceImageId),writer.sourceMip,writer.sourceLayer,writer.sourceRectangle[0],writer.sourceRectangle[1],writer.sourceRectangle[2],writer.sourceRectangle[3],static_cast<unsigned>(writer.source.sequence!=0));
    DiagnosticColorWrite(report,"imageWriterCurrent",writer.last);DiagnosticColorWrite(report,"imageWriterImmediateSource",writer.source);
}
std::size_t ParticleMipBytes(Block block,std::uint32_t width,std::uint32_t height,std::size_t used,std::uint32_t files){
    constexpr std::size_t maxFile=4*1024*1024,maxTotal=16*1024*1024;
    if(!width||!height||!block.x||!block.y||!block.bytes||used>maxTotal||files>=32)return 0;
    const auto rowBytes=((std::uint64_t(width)+block.x-1)/block.x)*block.bytes;
    if(rowBytes>maxFile)return 0;const auto rows=(std::uint64_t(height)+block.y-1)/block.y;
    if(rows>maxTotal/rowBytes)return 0;const auto bytes=rows*rowBytes,tiles=(rows+(maxFile/rowBytes)-1)/(maxFile/rowBytes);
    return bytes<=maxTotal-used&&tiles<=32-files?static_cast<std::size_t>(bytes):0;
}
std::vector<std::uint8_t> DownloadParticleRows(const std::shared_ptr<Image>& image,std::uint32_t mip,VkImageAspectFlagBits aspect,std::uint32_t y,std::uint32_t height,std::size_t bytes){
    auto allocation=Allocate(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);Transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,mip,1,0,1);
    VkBufferImageCopy copy{};copy.imageSubresource={aspect,mip,0,1};copy.imageOffset={0,static_cast<GLint>(y),0};copy.imageExtent={std::max(1u,image->width>>mip),height,1};
    vkCmdCopyImageToBuffer(Recording().command,image->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,allocation->buffer,1,&copy);Retain(allocation);Submit(true);
    std::vector<std::uint8_t> out(bytes);std::memcpy(out.data(),allocation->mapped,bytes);return out;
}
bool CaptureFile(const std::string& path,const void* data,std::size_t bytes,bool report=false){
    if(!CaptureBytesAllowed(gpu.diagnosticBytes,bytes,report)||(!report&&!TakeDiagnosticPayload(gpu.diagnosticPayloadFiles,bytes)))return false;gpu.diagnosticBytes+=bytes;
    return AtomicFile(path,data,bytes);
}
bool CaptureConsumerAfterImage(ConsumerCapture& capture,unsigned phase){
    const auto& image=phase?capture.target.texture->image:capture.sampledImage;const auto name=phase?capture.target.name:capture.sampledName;const auto object=phase?capture.target.object:GL_TEXTURE;const char* role=phase?"target":"sampled";
    auto& snapshot=gpu.particleAtlases.at(capture.targetKeys[phase]);bool complete=true;
    DiagnosticLine(capture.report,"imageWriterScope role=%s phase=after name=%u object=%x\n",role,name,object);DiagnosticImageWriter(capture.report,image,0);
    for(std::uint32_t y=0;y<image->height;){const auto rows=std::min(capture.plan.rowsPerTile,image->height-y);const auto bytes=rows*capture.plan.rowBytes;
        const auto file=capture.prefix+"-atlas"+std::to_string(++gpu.particleAtlasFiles)+".bin";auto pixels=DownloadParticleRows(image,0,VK_IMAGE_ASPECT_COLOR_BIT,y,rows,bytes);gpu.gfxAtlasBytes+=pixels.size();
        const bool written=CaptureFile(gpu.cacheDirectory+"/"+file,pixels.data(),pixels.size());complete=complete&&written;
        DiagnosticLine(capture.report,"afterSnapshot role=%s phase=after id=%u name=%u object=%x image=%llx mip=0 layer=0 aspect=%x x=0 y=%u width=%u height=%u fullWidth=%u fullHeight=%u rowBytes=%zu storageFormat=%u file=%s bytes=%zu written=%u sha256=%s rowOrder=legacy bottomRowFirst=1\n",role,snapshot.id,name,object,static_cast<unsigned long long>(image->image),VK_IMAGE_ASPECT_COLOR_BIT,y,image->width,rows,image->width,image->height,capture.plan.rowBytes,image->format,file.c_str(),pixels.size(),written,Hex(Hash(pixels.data(),pixels.size())).c_str());y+=rows;
    }snapshot.complete=complete;DiagnosticLine(capture.report,"afterSnapshotComplete role=%s phase=after id=%u complete=%u gpuWaitComplete=1\n",role,snapshot.id,complete);return complete;
}
void PlanConsumerCapture(ConsumerCapture& capture){
    auto& c=C();if(!c.drawFbo)capture.target={0,GL_TEXTURE,GL_TEXTURE_2D,0,0,c.defaultColor};
    else if(auto found=c.fbos.find(c.drawFbo);found!=c.fbos.end()){
        const auto slot=found->second.draw[0];if(slot>=GL_COLOR_ATTACHMENT0&&slot<=GL_COLOR_ATTACHMENT3)capture.target=found->second.colors[slot-GL_COLOR_ATTACHMENT0];
        for(std::size_t i=1;i<4;++i)if(found->second.draw[i]!=GL_NONE){DiagnosticLine(capture.report,"consumerTarget complete=0 status=multiple-active-color-targets\n");return;}
    }
    if(capture.report.size()>32768){DiagnosticLine(capture.report,"consumerTarget complete=0 status=report-size-preflight\n");return;}
    const auto image=capture.target.texture?capture.target.texture->image:nullptr;
    const auto project=[](const std::shared_ptr<Image>& value,GLint mip,GLint layer)->RecordedColorImage{
        if(!value)return {};return {value->type,value->layers,value->aspect,value->format,value->width,value->height,static_cast<std::uint64_t>(value->image),mip,layer,value->recordedWriter.get()};
    };
    if(gpu.cacheDirectory.empty()||!ConsumerAfterImagePairAvailable(project(capture.sampledImage,0,0),project(image,capture.target.level,capture.target.layer))){DiagnosticLine(capture.report,"consumerTarget complete=0 status=unsupported-or-aliased-after-images-or-cache-directory\n");return;}
    if(!PlanConsumerTarget(image->width,image->height,gpu.gfxAtlasBytes,gpu.gfxAtlases.size()+gpu.particleAtlases.size(),gpu.particleAtlasFiles,gpu.diagnosticPayloadFiles,gpu.diagnosticBytes,capture.plan,image->format)){DiagnosticLine(capture.report,"consumerTarget complete=0 status=paired-budget-preflight sharedAtlasBytes=%zu snapshots=%zu atlasFiles=%u payloadFiles=%u diagnosticBytes=%zu\n",gpu.gfxAtlasBytes,gpu.gfxAtlases.size()+gpu.particleAtlases.size(),gpu.particleAtlasFiles,gpu.diagnosticPayloadFiles,gpu.diagnosticBytes);return;}
    capture.targetKeys={capture.prefix+"/sampled-after",capture.prefix+"/target-after"};
    if(gpu.particleAtlases.contains(capture.targetKeys[0])||gpu.particleAtlases.contains(capture.targetKeys[1])){DiagnosticLine(capture.report,"consumerTarget complete=0 status=duplicate-snapshot-key\n");return;}
    for(const auto& key:capture.targetKeys){DiagnosticSnapshot snapshot;snapshot.id=gpu.particleAtlases.size()+1;snapshot.complete=false;gpu.particleAtlases.emplace(key,std::move(snapshot));}
    capture.planned=true;DiagnosticLine(capture.report,"consumerTarget complete=pending name=%u object=%x image=%llx sampledName=%u sampledImage=%llx fullWidth=%u fullHeight=%u storageFormat=%u imageBytes=%zu pairBytes=%zu rowsPerTile=%u pairFiles=%u groups=2 preflight=all-or-none readbackSubmission=after-draw-only targetBeforeUnobserved=1 afterSourceRole=unchanged-sampled-image-after-draw timingPerturbed=1\n",capture.target.name,capture.target.object,static_cast<unsigned long long>(image->image),capture.sampledName,static_cast<unsigned long long>(capture.sampledImage->image),image->width,image->height,image->format,capture.plan.imageBytes,capture.plan.totalBytes,capture.plan.rowsPerTile,capture.plan.tilesPerImage*2);
    DiagnosticLine(capture.report,"imageWriterScope role=target phase=before name=%u object=%x metadataOnly=1 pixelsUnobserved=1\n",capture.target.name,capture.target.object);DiagnosticImageWriter(capture.report,image,0);
}
void CompleteConsumerCapture(ConsumerCapture& capture){
    DiagnosticLine(capture.report,"consumerDrawRecorded=1 capture=%u draw=%llu indexedWitness=%u vertexWitnesses=%u expectedVertexWitnesses=%u bufferWitnessScope=referenced-raw-bytes-and-bound-span mappedHostCoherent=1 hardwareFetch=unproven\n",capture.number,static_cast<unsigned long long>(capture.epoch),capture.indexObserved,capture.vertexObserved,capture.vertexExpected);
    if(capture.planned){capture.sourceComplete=CaptureConsumerAfterImage(capture,0);capture.afterComplete=CaptureConsumerAfterImage(capture,1);}
    DiagnosticLine(capture.report,"consumerWitness capture=%u draw=%llu targetBeforeUnobserved=1 sourceBeforePixelsUnobserved=1 afterSourceRole=unchanged-sampled-image-after-draw sourceAfterComplete=%u afterComplete=%u afterImagesComplete=%u drawCommandRecorded=1 readbackInitiatedAfterDraw=1 drawGpuWaitComplete=%u\n",capture.number,static_cast<unsigned long long>(capture.epoch),capture.sourceComplete,capture.afterComplete,capture.planned&&capture.sourceComplete&&capture.afterComplete,capture.planned);
    std::size_t extra=0;const bool allowed=ConsumerReportGrowth(gpu.diagnosticBytes,capture.reportAccounted,capture.report.size(),extra);bool written=false;
    if(allowed&&!gpu.cacheDirectory.empty()){gpu.diagnosticBytes+=extra;written=AtomicFile(gpu.cacheDirectory+"/"+capture.prefix+".txt",capture.report.data(),capture.report.size());}
    __android_log_print(written?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"BL2Vulkan","AO bilateral blur after-only witness: capture=%u draw=%llu targetBeforeUnobserved=1 sampledAfter=%u targetAfter=%u index=%u vertices=%u/%u reportWritten=%u reportBudgetAllowed=%u sharedAtlasBytes=%zu snapshots=%zu atlasFiles=%u",capture.number,static_cast<unsigned long long>(capture.epoch),capture.sourceComplete,capture.afterComplete,capture.indexObserved,capture.vertexObserved,capture.vertexExpected,written,allowed,gpu.gfxAtlasBytes,gpu.gfxAtlases.size()+gpu.particleAtlases.size(),gpu.particleAtlasFiles);
}
std::uint32_t CaptureGfx(const Program& vertex,const Program& fragment,GLenum mode,GLint first,GLsizei count,GLenum indexType,const void* indexPointer,GLsizei instances,GLint baseVertex,std::uint32_t minimum,std::uint32_t maximum,std::unique_ptr<ConsumerCapture>& consumer){
    const auto vertexDigest=Hex(vertex.shader->stage.normalizedDigest),fragmentDigest=Hex(fragment.shader->stage.normalizedDigest);
    const bool deepCapture=SelectedDeepCapture(vertex.diagnosticLabel,fragment.diagnosticLabel,vertexDigest,fragmentDigest);
    if(!deepCapture)return 0; // Omit other diagnostic pairs before the selected command; all production draws continue.
    if(deepCapture?gpu.particleCaptures>=4:gpu.gfxCaptures>=16)return 0;
    auto& c=C();ApplyLegacyBindings(c,vertex);ApplyLegacyBindings(c,fragment);
    GLuint aoInputName=0,opaqueCopyName=0,sceneSourceName=0;Attachment aoTarget;RecordedColorImage aoInput,opaqueCopy,aoTargetImage,sceneSource;
    if(deepCapture){
        const auto project=[](const std::shared_ptr<Image>& image,GLint mip,GLint layer)->RecordedColorImage{
            if(!image)return {};return {image->type,image->layers,image->aspect,image->format,image->width,image->height,static_cast<std::uint64_t>(image->image),mip,layer,image->recordedWriter.get()};
        };
        const auto bound=c.textureUnits[0].find(GL_TEXTURE_2D);
        if(bound==c.textureUnits[0].end()||!bound->second.second||!bound->second.second->image||!bound->second.second->image->levels)return 0;
        const auto& texture=bound->second.second;aoInputName=bound->second.first;
        aoInput=project(texture->image,std::min<std::uint32_t>(texture->sampling.base,texture->image->levels-1),0);
        if(!RecordedPriorBlurInputAvailable(aoInput,drawEpoch,vertex.shader->stage.normalizedDigest,fragment.shader->stage.normalizedDigest))return 0;
        if(!c.drawFbo)aoTarget={0,GL_TEXTURE,GL_TEXTURE_2D,0,0,c.defaultColor};
        else if(auto found=c.fbos.find(c.drawFbo);found!=c.fbos.end()){
            std::uint32_t slot=0;if(!RecordedDrawColorSlot(found->second.draw,slot))return 0;aoTarget=found->second.colors[slot];
        }
        aoTargetImage=project(aoTarget.texture?aoTarget.texture->image:nullptr,aoTarget.level,aoTarget.layer);
        if(!RecordedBlurTargetAvailable(aoTargetImage))return 0;
        for(const auto& [name,candidate]:c.share->textures){
            if(!candidate||candidate->target!=GL_TEXTURE_2D||!candidate->image)continue;
            const auto scope=project(candidate->image,0,0);
            if(!scope.writer||!scope.writer->sourceImageId||!scope.writer->sourceImage)continue;
            for(const auto& [sourceName,sourceTexture]:c.share->textures){
                if(!sourceTexture||sourceTexture->target!=GL_TEXTURE_2D||!sourceTexture->image||static_cast<std::uint64_t>(sourceTexture->image->image)!=scope.writer->sourceImage)continue;
                const auto sourceScope=project(sourceTexture->image,0,0);
                if(RecordedBlurSceneCopyAvailable(scope,sourceScope,aoTargetImage,drawEpoch)){opaqueCopyName=name;opaqueCopy=scope;sceneSourceName=sourceName;sceneSource=sourceScope;break;}
            }
            if(opaqueCopy.writer)break;
        }
        if(!opaqueCopy.writer)return 0;
    }
    const bool gfxPair=vertex.diagnosticLabel.starts_with("GFx")||fragment.diagnosticLabel.starts_with("GFx");
    const bool shadowWithoutDepth=HasShadowWithoutDepth(c,vertex)||HasShadowWithoutDepth(c,fragment);
    if(!gfxPair&&!shadowWithoutDepth&&!deepCapture)return 0;
    const bool textPair=vertex.diagnosticLabel.find("_text")!=std::string::npos||fragment.diagnosticLabel.find("_text")!=std::string::npos;
    const auto pair=vertexDigest+fragmentDigest;
    if(deepCapture){if(!TakeParticleCapture(gpu.particlePairs,gpu.particleCaptures,pair))return 0;}
    else{auto it=gpu.gfxPairs.find(pair);if(it==gpu.gfxPairs.end()){auto& pairs=textPair?gpu.gfxTextPairs:gpu.gfxOtherPairs;if(pairs>=8)return 0;++pairs;it=gpu.gfxPairs.emplace(pair,0).first;}if(it->second>=2)return 0;++it->second;++gpu.gfxCaptures;}
    const auto number=gpu.gfxCaptures+gpu.particleCaptures;const auto prefix="bl2vk-gfx-"+std::to_string(getpid())+"-"+std::to_string(number);std::string report;report.reserve(16384);
    if(deepCapture){consumer=std::make_unique<ConsumerCapture>();consumer->number=number;consumer->epoch=drawEpoch;consumer->prefix=prefix;consumer->count=count;consumer->instances=instances;consumer->indexType=indexType;consumer->baseVertex=baseVertex;consumer->vertexExpected=vertex.shader->stage.attribute_locations.size();consumer->sampledName=aoInputName;consumer->sampledImage=c.textureUnits[0].at(GL_TEXTURE_2D).second->image;}
    const auto write=[&](const std::string& suffix,const void* data,std::size_t bytes){const auto name=prefix+suffix;const bool written=!gpu.cacheDirectory.empty()&&CaptureFile(gpu.cacheDirectory+"/"+name,data,bytes);DiagnosticLine(report,"file=%s bytes=%zu written=%u sha256=%s\n",name.c_str(),bytes,written,Hex(Hash(data,bytes)).c_str());return written;};
    DiagnosticLine(report,"format=bl2vk-gfx-capture-v1 capture=%u draw=%llu backend=%s\n",number,static_cast<unsigned long long>(drawEpoch),gpu.identity.c_str());
    DiagnosticLine(report,"reason=%s\ncapturePoint=metadata-before-vkDraw drawCommandRecorded=unproven deepCaptureFamily=%s\nobservation=after-draw-only targetBeforeUnobserved=1 sourceBeforePixelsUnobserved=1 earlyOtherDiagnosticPairsOmitted=1\n",deepCapture?"ao-bilateral-blur":shadowWithoutDepth?"shadow-without-depth":"gfx",deepCapture?"ao-bilateral-blur-exact-pair-after-prior-blur-and-opaque-copy":"none");
    if(deepCapture){
        const auto& input=*aoInput.writer;const auto& copy=*opaqueCopy.writer;
        DiagnosticLine(report,"deepAdmission scope=ao-blur-after-prior-blur-and-historical-live-scene-copy coverageUnproven=1 currentFrameUnproven=1 inputName=%u inputImage=%llx inputImageId=%llu inputOperation=%s inputSequence=%llu inputDraw=%llu targetName=%u targetObject=%x targetImage=%llx targetImageId=%llu targetMip=%d targetLayer=%d\n",aoInputName,static_cast<unsigned long long>(aoInput.image),static_cast<unsigned long long>(input.imageId),RecordedOperationName(input.last.operation),static_cast<unsigned long long>(input.last.sequence),static_cast<unsigned long long>(input.last.draw),aoTarget.name,aoTarget.object,static_cast<unsigned long long>(aoTargetImage.image),static_cast<unsigned long long>(aoTargetImage.writer->imageId),aoTargetImage.mip,aoTargetImage.layer);
        DiagnosticLine(report,"opaqueCopy name=%u scope=color2D-mip0-layer0 format=%u size=%ux%u image=%llx imageId=%llu sourceImage=%llx sourceImageId=%llu sourceMip=%d sourceLayer=%d sourcePossibleRect=%d,%d,%d,%d\n",opaqueCopyName,opaqueCopy.format,opaqueCopy.width,opaqueCopy.height,static_cast<unsigned long long>(opaqueCopy.image),static_cast<unsigned long long>(copy.imageId),static_cast<unsigned long long>(copy.sourceImage),static_cast<unsigned long long>(copy.sourceImageId),copy.sourceMip,copy.sourceLayer,copy.sourceRectangle[0],copy.sourceRectangle[1],copy.sourceRectangle[2],copy.sourceRectangle[3]);
        DiagnosticLine(report,"liveSceneSource name=%u scope=color2D-mip0-layer0 format=%u size=%ux%u image=%llx imageId=%llu targetScale=2x historicalOnly=1 coverageUnproven=1 currentFrameUnproven=1\n",sceneSourceName,sceneSource.format,sceneSource.width,sceneSource.height,static_cast<unsigned long long>(sceneSource.image),static_cast<unsigned long long>(sceneSource.writer->imageId));
        DiagnosticColorWrite(report,"opaqueCopyLastRecorded",copy.last);DiagnosticColorWrite(report,"opaqueCopyImmediateSource",copy.source);
    }
    DiagnosticLine(report,"mode=0x%x first=%d count=%d indexType=0x%x indexPointer=0x%llx baseVertex=%d instances=%d minimum=%u maximum=%u vao=%u elementBuffer=%u fbo=%u\n",mode,first,count,indexType,static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(indexPointer)),baseVertex,instances,minimum,maximum,c.vao,Vao().elementName,c.drawFbo);
    DiagnosticLine(report,"viewport=%d,%d,%d,%d scissor=%d,%d,%d,%d scissorEnabled=%u blendEnabled=%u blend=%x,%x,%x,%x equations=%x,%x colorWrite=%u,%u,%u,%u depthTest=%u depthWrite=%u cullEnabled=%u front=%x\n",c.viewport[0],c.viewport[1],c.viewport[2],c.viewport[3],c.scissor[0],c.scissor[1],c.scissor[2],c.scissor[3],c.enabled.contains(GL_SCISSOR_TEST),c.enabled.contains(GL_BLEND),c.srcRgb,c.dstRgb,c.srcAlpha,c.dstAlpha,c.rgbEquation,c.alphaEquation,c.colorWrite[0],c.colorWrite[1],c.colorWrite[2],c.colorWrite[3],c.enabled.contains(GL_DEPTH_TEST),c.depthWrite,c.enabled.contains(GL_CULL_FACE),c.front);
    DiagnosticLine(report,"depthFunction=%x depthRange=%.9g,%.9g depthBoundsEnabled=%u depthBounds=%.9g,%.9g stencilTest=%u\n",c.depthFunction,c.nearDepth,c.farDepth,c.enabled.contains(0x8890),c.minDepthBounds,c.maxDepthBounds,c.enabled.contains(GL_STENCIL_TEST));
    for(const auto* stencil:{&c.frontStencil,&c.backStencil})DiagnosticLine(report,"stencilFace=%s function=%x reference=%d compareMask=%08x writeMask=%08x fail=%x depthFail=%x pass=%x\n",stencil==&c.frontStencil?"front":"back",stencil->function,stencil->reference,stencil->compareMask,stencil->writeMask,stencil->fail,stencil->depthFail,stencil->pass);
    const auto attachment=[&](const char* slot,const Attachment& a){const auto texture=a.texture;const auto image=texture?texture->image:nullptr;DiagnosticLine(report,"targetSlot=%s name=%u object=%x target=%x level=%d layer=%d internal=%x vkFormat=%u image=%llx aspect=%x size=%ux%ux%u levels=%u\n",slot,a.name,a.object,a.textureTarget,a.level,a.layer,texture?texture->internal:0,image?image->format:VK_FORMAT_UNDEFINED,image?static_cast<unsigned long long>(image->image):0,image?image->aspect:0,image?image->width:0,image?image->height:0,image?image->depth:0,image?image->levels:0);};
    if(!c.drawFbo){attachment("color0",{0,GL_TEXTURE,GL_TEXTURE_2D,0,0,c.defaultColor});attachment("depth",{0,GL_TEXTURE,GL_TEXTURE_2D,0,0,c.defaultDepth});attachment("stencil",{0,GL_TEXTURE,GL_TEXTURE_2D,0,0,c.defaultDepth});DiagnosticLine(report,"drawBuffer[0]=%x\n",GL_BACK);}
    else if(auto found=c.fbos.find(c.drawFbo);found!=c.fbos.end()){for(std::size_t index=0;index<4;++index){attachment(("color"+std::to_string(index)).c_str(),found->second.colors[index]);DiagnosticLine(report,"drawBuffer[%zu]=%x\n",index,found->second.draw[index]);}attachment("depth",found->second.depth);attachment("stencil",found->second.stencil);}
    for(const auto* p:{&vertex,&fragment}){
        const char* stage=p->target==kVS?"V":"P";const auto suffix=p->target==kVS?"-v":"-p";
        DiagnosticLine(report,"stage=%s program=%u label=%s normalized=%s raw=%s parameterBinding=%u parameterBytes=%u\n",stage,p->target==kVS?c.vertexProgram:c.fragmentProgram,p->diagnosticLabel.c_str(),Hex(p->shader->stage.normalizedDigest).c_str(),Hex(Hash(p->diagnosticSource.data(),p->diagnosticSource.size())).c_str(),p->shader->stage.parameterBinding,p->shader->stage.parameterBytes);
        if(!p->diagnosticSource.empty())write(std::string(suffix)+".glsl",p->diagnosticSource.data(),p->diagnosticSource.size());
        write(std::string(suffix)+"-locals.bin",p->locals.data(),p->locals.size()*4);std::vector<std::uint8_t> parameters(std::max(16u,p->shader->stage.parameterBytes));Parameters(*p,parameters.data(),parameters.size());write(std::string(suffix)+"-ubo.bin",parameters.data(),parameters.size());
        for(std::size_t group=0;group<9;++group){const auto& bank=p->groups[group];DiagnosticLine(report,"stage=%s group=%zu first=%d bytes=%u\n",stage,group,bank.first_register,bank.byte_count);if(bank.first_register<0||bank.byte_count==0xffff)continue;for(std::size_t row=0;row<std::min<std::size_t>(4,bank.byte_count/16);++row){const auto offset=static_cast<std::size_t>(bank.first_register)*4+row*4;if(offset+4>p->locals.size())break;std::array<float,4> value{};std::memcpy(value.data(),p->locals.data()+offset,16);DiagnosticLine(report,"stage=%s group=%zu row=%zu raw=%08x,%08x,%08x,%08x float=%.9g,%.9g,%.9g,%.9g\n",stage,group,row,p->locals[offset],p->locals[offset+1],p->locals[offset+2],p->locals[offset+3],value[0],value[1],value[2],value[3]);}}
        for(std::size_t unit=0;unit<16;++unit)DiagnosticLine(report,"stage=%s samplerRegister[%zu]=%d\n",stage,unit,p->samplerRegisters[unit]);
        for(const auto& field:p->shader->stage.fields)DiagnosticLine(report,"stage=%s uboField group=%u type=%u count=%u offset=%u\n",stage,field.groupIndex,static_cast<unsigned>(field.type),field.count,field.offset);
    }
    std::vector<std::uint32_t> references;references.reserve(6);const auto indexBytes=indexType==GL_UNSIGNED_BYTE?1u:indexType==GL_UNSIGNED_SHORT?2u:4u;
    const auto* indexSource=indexType?(Vao().elements?Vao().elements->bytes.data()+reinterpret_cast<std::uintptr_t>(indexPointer):static_cast<const std::uint8_t*>(indexPointer)):nullptr;
    const bool restart=c.enabled.contains(GL_PRIMITIVE_RESTART_FIXED_INDEX)&&(mode==GL_LINE_STRIP||mode==GL_TRIANGLE_STRIP||mode==GL_TRIANGLE_FAN);const auto sentinel=indexBytes==1?255u:indexBytes==2?65535u:~0u;
    for(GLsizei index=0;index<count&&references.size()<6;++index){std::uint32_t value=first+index;if(indexType){value=0;std::memcpy(&value,indexSource+static_cast<std::size_t>(index)*indexBytes,indexBytes);if(restart&&value==sentinel)continue;value=static_cast<std::uint32_t>(static_cast<std::int64_t>(value)+baseVertex);}references.push_back(value);}
    write("-references.bin",references.data(),references.size()*4);
    if(consumer){consumer->references=references;if(indexSource&&count<=6&&references.size()==static_cast<std::size_t>(count))consumer->indexCpu.assign(indexSource,indexSource+static_cast<std::size_t>(count)*indexBytes);}
    for(auto location:vertex.shader->stage.attribute_locations){
        const auto& a=Vao().attributes[location];const auto bytes=AttributeBytes(a),stride=a.stride?static_cast<std::size_t>(a.stride):bytes;
        DiagnosticLine(report,"attribute=%u enabled=%u size=%d type=%x normalized=%u integer=%u stride=%zu divisor=%u buffer=%u pointer=0x%llx constant=%.9g,%.9g,%.9g,%.9g\n",location,a.enabled,a.size,a.type,a.normalized,a.integer,stride,a.divisor,a.name,static_cast<unsigned long long>(a.pointer),a.constant[0],a.constant[1],a.constant[2],a.constant[3]);
        if(!a.enabled)continue;std::vector<std::uint8_t> raw;raw.reserve(references.size()*bytes);
        for(std::size_t sample=0;sample<references.size();++sample){const auto record=a.divisor?0u:references[sample];const auto offset=static_cast<std::uint64_t>(a.pointer)+static_cast<std::uint64_t>(record)*stride;const std::uint8_t* source=nullptr;
            if(a.buffer){if(offset>a.buffer->bytes.size()||bytes>a.buffer->bytes.size()-offset){DiagnosticLine(report,"attribute=%u sample=%zu record=%u outOfBounds=1\n",location,sample,record);continue;}source=a.buffer->bytes.data()+static_cast<std::size_t>(offset);}else if(offset&&offset<=std::numeric_limits<std::uintptr_t>::max()-bytes)source=reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(offset));
            if(!source)continue;const auto value=AttributeValue(a,source);raw.insert(raw.end(),source,source+bytes);DiagnosticLine(report,"attribute=%u sample=%zu record=%u offset=0x%llx decoded=%.9g,%.9g,%.9g,%.9g raw=",location,sample,record,static_cast<unsigned long long>(offset),value[0],value[1],value[2],value[3]);for(std::size_t byte=0;byte<bytes;++byte)DiagnosticLine(report,"%02x",source[byte]);report+='\n';
        }
        if(!raw.empty())write("-a"+std::to_string(location)+".bin",raw.data(),raw.size());
        if(consumer)consumer->attributeCpu[location]=std::move(raw);
    }
    for(const auto* p:{&vertex,&fragment})for(const auto& sampler:p->shader->stage.samplers){
        const auto target=SamplerTarget(sampler.type);auto bound=c.textureUnits[sampler.unit].find(target);const auto samplerName=c.samplerUnits[sampler.unit];
        if(bound==c.textureUnits[sampler.unit].end()||!bound->second.second||!bound->second.second->image){DiagnosticLine(report,"samplerStage=%s unit=%u binding=%u target=%x uploaded=0\n",p->target==kVS?"V":"P",sampler.unit,sampler.binding,target);continue;}
        const auto& texture=bound->second.second;const auto& image=texture->image;auto sampling=texture->sampling;if(samplerName){auto named=c.share->samplers.find(samplerName);if(named!=c.share->samplers.end())sampling=named->second;}
        const auto reg=p->samplerRegisters[sampler.unit];std::array<std::uint32_t,4> words{};if(reg>=0&&static_cast<std::size_t>(reg)*4<p->locals.size())std::copy_n(p->locals.data()+reg*4,std::min<std::size_t>(4,p->locals.size()-reg*4),words.begin());
        VkComponentMapping swizzle{};if(texture->internal==GL_ALPHA)swizzle={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};else if(texture->internal==GL_LUMINANCE)swizzle={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};else if(texture->internal==GL_LUMINANCE_ALPHA)swizzle={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G};else if(texture->internal==GL_RGB8_SNORM||texture->internal==0x8F9A)swizzle.a=VK_COMPONENT_SWIZZLE_ONE;
        const auto base=std::min<std::uint32_t>(texture->sampling.base,image->levels-1);const std::string atlas;
        DiagnosticLine(report,"samplerStage=%s unit=%u binding=%u target=%x reg=%d raw=%08x,%08x,%08x,%08x texture=%u sampler=%u internal=%x vkFormat=%u size=%ux%ux%u levels=%u base=%d maximum=%d swizzle=%u,%u,%u,%u min=%x mag=%x wrap=%x,%x,%x aniso=%.9g lod=%.9g,%.9g,%.9g compare=%x,%x uploadExternal=%x uploadType=%x uploadRegion=%d,%d,%d,%d,%d,%d,%d atlasFile=%s samplerType=%u aspect=%x\n",p->target==kVS?"V":"P",sampler.unit,sampler.binding,target,reg,words[0],words[1],words[2],words[3],bound->second.first,samplerName,texture->internal,image->format,image->width,image->height,image->depth,image->levels,texture->sampling.base,texture->sampling.maximum,swizzle.r,swizzle.g,swizzle.b,swizzle.a,sampling.min,sampling.mag,sampling.s,sampling.t,sampling.r,sampling.aniso,sampling.minLod,sampling.maxLod,sampling.lodBias,sampling.compare,sampling.compareFunc,texture->uploadFormat,texture->uploadType,texture->uploadRegion[0],texture->uploadRegion[1],texture->uploadRegion[2],texture->uploadRegion[3],texture->uploadRegion[4],texture->uploadRegion[5],texture->uploadRegion[6],atlas.empty()?"uncaptured":atlas.c_str(),static_cast<unsigned>(sampler.type),image->aspect);
        if(deepCapture&&p==&fragment&&sampler.unit==0){DiagnosticLine(report,"imageWriterScope role=sampled stage=P unit=0 phase=before metadataOnly=1 pixelsUnobserved=1\n");DiagnosticImageWriter(report,image,base);}
        DiagnosticLine(report,"texture=%u uploadRevision=%llu\n",bound->second.first,static_cast<unsigned long long>(texture->uploadRevision));
    }
    if(consumer){consumer->report=std::move(report);PlanConsumerCapture(*consumer);report=std::move(consumer->report);}
    const auto diagnosticBefore=gpu.diagnosticBytes;
    const bool written=!gpu.cacheDirectory.empty()&&CaptureFile(gpu.cacheDirectory+"/"+prefix+".txt",report.data(),report.size(),true);
    if(consumer){consumer->report=report;consumer->reportAccounted=gpu.diagnosticBytes-diagnosticBefore;}
    __android_log_print(written?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"BL2Vulkan","AO bilateral blur CPU metadata: number=%u draw=%llu V=%u/%s P=%u/%s text=%u file=%s.txt bytes=%zu written=%u atlasBytes=%zu",number,static_cast<unsigned long long>(drawEpoch),c.vertexProgram,vertex.diagnosticLabel.c_str(),c.fragmentProgram,fragment.diagnosticLabel.c_str(),textPair,prefix.c_str(),report.size(),written,gpu.gfxAtlasBytes);
    if(deepCapture)__android_log_print(ANDROID_LOG_INFO,"BL2Vulkan","AO bilateral blur deep capture: number=%u draw=%llu pairs=%zu draws=%u atlasFiles=%u sharedAtlasBytes=%zu written=%u",number,static_cast<unsigned long long>(drawEpoch),gpu.particlePairs.size(),gpu.particleCaptures,gpu.particleAtlasFiles,gpu.gfxAtlasBytes,written);
    return number;
}
void UploadRaw(const Attachment& a,VkImageAspectFlagBits aspect,GLint x,GLint y,GLsizei w,GLsizei h,const std::vector<std::uint8_t>& data){auto slice=Upload(data.size(),16);std::memcpy(slice.data,data.data(),data.size());auto image=a.texture->image;Transition(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,a.level,1,a.layer,1);VkBufferImageCopy copy{};copy.bufferOffset=slice.offset;copy.imageSubresource={aspect,static_cast<std::uint32_t>(a.level),static_cast<std::uint32_t>(a.layer),1};copy.imageOffset={x,y,0};copy.imageExtent={static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(h),1};vkCmdCopyBufferToImage(Recording().command,slice.buffer->buffer,image->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);if(gpu.gfxCapture&&aspect==VK_IMAGE_ASPECT_COLOR_BIT)RecordColorWrite(image,a.level,a.layer,{.operation=RecordedColorOperation::RawUpload,.rectangle={x,y,w,h},.colorMask=15});}
void ClearTargets(GLbitfield mask,const std::array<float,4>& color,float depth,GLint stencil,GLint colorIndex=-1,bool useWriteMasks=true){
    auto& c=C();auto targets=Target(c.drawFbo);auto rect=ClipScissor(targets);if(!rect.extent.width||!rect.extent.height)return;
    std::vector<VkClearAttachment> clear;
    if(mask&GL_COLOR_BUFFER_BIT)for(std::uint32_t i=0;i<targets.colorCount;++i)if(targets.colorViews[i]&&(colorIndex<0||static_cast<std::uint32_t>(colorIndex)==i)){
        const bool masked=useWriteMasks&&c.colorWrite!=std::array<GLboolean,4>{GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
        if(masked){
            if(c.colorWrite==std::array<GLboolean,4>{GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE})continue;
            auto& a=targets.colors[i];auto bytes=Download(a,VK_IMAGE_ASPECT_COLOR_BIT,rect.offset.x,rect.offset.y,rect.extent.width,rect.extent.height);const auto format=a.texture->image->format;const auto block=FormatBlock(format);const auto channelBytes=ColorComponentBytes(format),channels=block.bytes/channelBytes;Require(channels<=4,GL_INVALID_OPERATION);
            auto converted=color;if(format==VK_FORMAT_R8G8B8A8_SRGB)for(std::size_t k=0;k<3;++k)converted[k]=converted[k]<=.0031308f?12.92f*converted[k]:1.055f*std::pow(converted[k],1/2.4f)-.055f;
            if(a.texture->internal==GL_RGB8_SNORM||a.texture->internal==0x8F9A)converted[3]=1;
            std::array<std::uint8_t,16> encoded{};EncodeColor(encoded.data(),format,converted);
            for(std::size_t p=0;p<bytes.size();p+=block.bytes)for(std::size_t k=0;k<channels;++k)if(c.colorWrite[k])std::memcpy(bytes.data()+p+k*channelBytes,encoded.data()+k*channelBytes,channelBytes);
            UploadRaw(a,VK_IMAGE_ASPECT_COLOR_BIT,rect.offset.x,rect.offset.y,rect.extent.width,rect.extent.height,bytes);
            if(gpu.gfxCapture&&a.level==0&&a.layer==0&&a.texture->image->recordedWriter){auto& write=a.texture->image->recordedWriter->last;write.operation=RecordedColorOperation::Clear;write.framebuffer=c.drawFbo;write.colorMask=(c.colorWrite[0]?1:0)|(c.colorWrite[1]?2:0)|(c.colorWrite[2]?4:0)|(c.colorWrite[3]?8:0);write.clearValue=color;}
        }
        else {VkClearAttachment a{};a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;a.colorAttachment=i;std::copy_n(color.data(),4,a.clearValue.color.float32);if(targets.colors[i].texture->internal==GL_RGB8_SNORM||targets.colors[i].texture->internal==0x8F9A)a.clearValue.color.float32[3]=1;clear.push_back(a);}
    }
    VkClearAttachment ds{};ds.clearValue.depthStencil={depth,static_cast<std::uint32_t>(stencil)};
    if(targets.depthView){if((mask&GL_DEPTH_BUFFER_BIT)&&(!useWriteMasks||c.depthWrite)&&HasDsAspect(targets,VK_IMAGE_ASPECT_DEPTH_BIT))ds.aspectMask|=VK_IMAGE_ASPECT_DEPTH_BIT;
        if((mask&GL_STENCIL_BUFFER_BIT)&&HasDsAspect(targets,VK_IMAGE_ASPECT_STENCIL_BIT)){
            const auto writeMask=useWriteMasks?c.frontStencil.writeMask&255u:255u;
            if(writeMask==255u)ds.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;
            else if(writeMask){auto bytes=Download(targets.depth,VK_IMAGE_ASPECT_STENCIL_BIT,rect.offset.x,rect.offset.y,rect.extent.width,rect.extent.height);for(auto& byte:bytes)byte=(byte&~writeMask)|(stencil&writeMask);UploadRaw(targets.depth,VK_IMAGE_ASPECT_STENCIL_BIT,rect.offset.x,rect.offset.y,rect.extent.width,rect.extent.height,bytes);}
        }
    }
    if(ds.aspectMask)clear.push_back(ds);if(clear.empty())return;BeginPass(targets);VkClearRect clearRect{rect,0,1};vkCmdClearAttachments(Recording().command,clear.size(),clear.data(),1,&clearRect);
    if(gpu.gfxCapture)for(const auto& clearAttachment:clear)if(clearAttachment.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT){const auto& a=targets.colors[clearAttachment.colorAttachment];RecordColorWrite(a.texture->image,a.level,a.layer,{.operation=RecordedColorOperation::Clear,.rectangle={rect.offset.x,rect.offset.y,static_cast<GLint>(rect.extent.width),static_cast<GLint>(rect.extent.height)},.framebuffer=c.drawFbo,.colorMask=15,.clearValue=color});}
}
}
void Enable(GLenum cap){Call([&]{Require(Capability(cap),GL_INVALID_ENUM);if(cap==0x864F&&!gpu.features.depthClamp)Fail("Depth clamp is unsupported by Vulkan device");if(cap==0x8890&&!gpu.features.depthBounds)Fail("Depth bounds is unsupported by Vulkan device");C().enabled.insert(cap);});}
void Disable(GLenum cap){Call([&]{Require(Capability(cap),GL_INVALID_ENUM);C().enabled.erase(cap);});}
GLboolean IsEnabled(GLenum cap){GLboolean out=GL_FALSE;Call([&]{Require(Capability(cap),GL_INVALID_ENUM);out=C().enabled.contains(cap);});return out;}
void Viewport(GLint x,GLint y,GLsizei w,GLsizei h){Call([&]{Require(w>=0&&h>=0&&static_cast<std::uint32_t>(w)<=gpu.properties.limits.maxViewportDimensions[0]&&static_cast<std::uint32_t>(h)<=gpu.properties.limits.maxViewportDimensions[1]);Require(x>=gpu.properties.limits.viewportBoundsRange[0]&&y>=gpu.properties.limits.viewportBoundsRange[0]&&static_cast<double>(x)+w<=gpu.properties.limits.viewportBoundsRange[1]&&static_cast<double>(y)+h<=gpu.properties.limits.viewportBoundsRange[1]);C().viewport={x,y,w,h};});}
void Scissor(GLint x,GLint y,GLsizei w,GLsizei h){Call([&]{Require(w>=0&&h>=0);C().scissor={x,y,w,h};});}
void DepthRangef(GLfloat n,GLfloat f){Call([&]{C().nearDepth=std::clamp(n,0.f,1.f);C().farDepth=std::clamp(f,0.f,1.f);});}
void DepthFunc(GLenum function){Call([&]{Compare(function);C().depthFunction=function;});}
void DepthMask(GLboolean enable){Call([&]{C().depthWrite=enable!=0;});}
void DepthBounds(double minimum,double maximum){Call([&]{Require(minimum<=maximum);C().minDepthBounds=std::clamp(minimum,0.,1.);C().maxDepthBounds=std::clamp(maximum,0.,1.);});}
void StencilFuncSeparate(GLenum face,GLenum function,GLint reference,GLuint mask){Call([&]{Require(face==GL_FRONT||face==GL_BACK||face==GL_FRONT_AND_BACK,GL_INVALID_ENUM);Compare(function);for(auto* s:{&C().frontStencil,&C().backStencil})if(face==GL_FRONT_AND_BACK||(face==GL_FRONT&&s==&C().frontStencil)||(face==GL_BACK&&s==&C().backStencil)){s->function=function;s->reference=std::clamp(reference,0,255);s->compareMask=mask;}});}
void StencilOpSeparate(GLenum face,GLenum fail,GLenum depthFail,GLenum pass){Call([&]{Require(face==GL_FRONT||face==GL_BACK||face==GL_FRONT_AND_BACK,GL_INVALID_ENUM);StencilOp(fail);StencilOp(depthFail);StencilOp(pass);for(auto* s:{&C().frontStencil,&C().backStencil})if(face==GL_FRONT_AND_BACK||(face==GL_FRONT&&s==&C().frontStencil)||(face==GL_BACK&&s==&C().backStencil)){s->fail=fail;s->depthFail=depthFail;s->pass=pass;}});}
void StencilMaskSeparate(GLenum face,GLuint mask){Call([&]{Require(face==GL_FRONT||face==GL_BACK||face==GL_FRONT_AND_BACK,GL_INVALID_ENUM);if(face!=GL_BACK)C().frontStencil.writeMask=mask;if(face!=GL_FRONT)C().backStencil.writeMask=mask;});}
void BlendFuncSeparate(GLenum srcRgb,GLenum dstRgb,GLenum srcAlpha,GLenum dstAlpha){Call([&]{BlendFactor(srcRgb);BlendFactor(dstRgb);BlendFactor(srcAlpha);BlendFactor(dstAlpha);C().srcRgb=srcRgb;C().dstRgb=dstRgb;C().srcAlpha=srcAlpha;C().dstAlpha=dstAlpha;});}
void BlendEquationSeparate(GLenum rgb,GLenum alpha){Call([&]{BlendOp(rgb);BlendOp(alpha);C().rgbEquation=rgb;C().alphaEquation=alpha;});}
void BlendColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){Call([&]{C().blendColor={std::clamp(r,0.f,1.f),std::clamp(g,0.f,1.f),std::clamp(b,0.f,1.f),std::clamp(a,0.f,1.f)};});}
void ColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a){Call([&]{C().colorWrite={static_cast<GLboolean>(r!=0),static_cast<GLboolean>(g!=0),static_cast<GLboolean>(b!=0),static_cast<GLboolean>(a!=0)};});}
void CullFace(GLenum face){Call([&]{Require(face==GL_FRONT||face==GL_BACK||face==GL_FRONT_AND_BACK,GL_INVALID_ENUM);C().cull=face;});}
void FrontFace(GLenum orientation){Call([&]{Require(orientation==GL_CCW||orientation==GL_CW,GL_INVALID_ENUM);C().front=orientation;});}
void PolygonOffset(GLfloat factor,GLfloat units){Call([&]{C().polygonFactor=factor;C().polygonUnits=units;});}
void LineWidth(GLfloat width){Call([&]{Require(width>0);if(width!=1)Fail("Wide lines are unsupported by the experimental Vulkan device feature set");C().lineWidth=width;});}
void ClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){Call([&]{C().clearColor={r,g,b,a};});}
void ClearDepthf(GLfloat depth){Call([&]{C().clearDepth=std::clamp(depth,0.f,1.f);});}
void ClearStencil(GLint stencil){Call([&]{C().clearStencil=stencil;});}
void Clear(GLbitfield mask){Call([&]{Require(!(mask&~(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT)));ClearTargets(mask,C().clearColor,C().clearDepth,C().clearStencil);});}
void ClearBufferfv(GLenum buffer,GLint drawbuffer,const GLfloat* value){Call([&]{Require(value&&drawbuffer>=0&&drawbuffer<4);if(buffer==GL_COLOR){std::array<float,4> c;std::copy_n(value,4,c.begin());ClearTargets(GL_COLOR_BUFFER_BIT,c,0,0,drawbuffer,true);}else if(buffer==GL_DEPTH){Require(drawbuffer==0);ClearTargets(GL_DEPTH_BUFFER_BIT,{},std::clamp(*value,0.f,1.f),0,-1,true);}else Require(false,GL_INVALID_ENUM);});}
void ClearBufferiv(GLenum buffer,GLint drawbuffer,const GLint* value){Call([&]{Require(value&&drawbuffer==0);Require(buffer==GL_STENCIL,GL_INVALID_ENUM);ClearTargets(GL_STENCIL_BUFFER_BIT,{},0,*value,-1,true);});}
void ClearBufferfi(GLenum buffer,GLint drawbuffer,GLfloat depth,GLint stencil){Call([&]{Require(drawbuffer==0);Require(buffer==GL_DEPTH_STENCIL,GL_INVALID_ENUM);ClearTargets(GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT,{},std::clamp(depth,0.f,1.f),stencil,-1,true);});}

namespace {
VkExtent2D TransferExtent(const Targets& targets,GLbitfield mask,VkImageAspectFlags dsAspects){
    VkExtent2D extent{};auto include=[&](const Attachment& attachment){if(!attachment.texture)return;const auto& image=attachment.texture->image;IntersectExtent(extent,std::max(1u,image->width>>attachment.level),std::max(1u,image->height>>attachment.level));};
    // A framebuffer blit intersects only the buffers actually transferred.
    if(mask&GL_COLOR_BUFFER_BIT)for(const auto& color:targets.colors)include(color);
    if(((mask&GL_DEPTH_BUFFER_BIT)&&(dsAspects&VK_IMAGE_ASPECT_DEPTH_BIT))||((mask&GL_STENCIL_BUFFER_BIT)&&(dsAspects&VK_IMAGE_ASPECT_STENCIL_BIT)))include(targets.depth);
    return extent;
}
void Blit(const Attachment& source,const Attachment& destination,VkImageAspectFlagBits aspect,
          GLint sx0,GLint sy0,GLint sx1,GLint sy1,GLint dx0,GLint dy0,GLint dx1,GLint dy1,VkFilter filter){
    Require(source.texture&&source.texture->image&&destination.texture&&destination.texture->image,GL_INVALID_FRAMEBUFFER_OPERATION);auto src=source.texture->image,dst=destination.texture->image;
    Require(src!=dst||source.level!=destination.level||source.layer!=destination.layer,GL_INVALID_OPERATION);
    Require(std::min(sx0,sx1)>=0&&std::min(sy0,sy1)>=0&&std::max(sx0,sx1)<=static_cast<GLint>(std::max(1u,src->width>>source.level))&&std::max(sy0,sy1)<=static_cast<GLint>(std::max(1u,src->height>>source.level)));
    Require(std::min(dx0,dx1)>=0&&std::min(dy0,dy1)>=0&&std::max(dx0,dx1)<=static_cast<GLint>(std::max(1u,dst->width>>destination.level))&&std::max(dy0,dy1)<=static_cast<GLint>(std::max(1u,dst->height>>destination.level)));
    if(sx0==sx1||sy0==sy1||dx0==dx1||dy0==dy1)return;
    if(aspect!=VK_IMAGE_ASPECT_COLOR_BIT)Require(src->format==dst->format&&filter==VK_FILTER_NEAREST,GL_INVALID_OPERATION);
    Transition(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,source.level,1,source.layer,1);Transition(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,destination.level,1,destination.layer,1);
    VkImageBlit blit{};blit.srcSubresource={aspect,static_cast<std::uint32_t>(source.level),static_cast<std::uint32_t>(source.layer),1};blit.dstSubresource={aspect,static_cast<std::uint32_t>(destination.level),static_cast<std::uint32_t>(destination.layer),1};blit.srcOffsets[0]={sx0,sy0,0};blit.srcOffsets[1]={sx1,sy1,1};blit.dstOffsets[0]={dx0,dy0,0};blit.dstOffsets[1]={dx1,dy1,1};vkCmdBlitImage(Recording().command,src->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
    if(gpu.gfxCapture&&aspect==VK_IMAGE_ASPECT_COLOR_BIT)RecordColorWrite(dst,destination.level,destination.layer,{.operation=RecordedColorOperation::Blit,.rectangle={std::min(dx0,dx1),std::min(dy0,dy1),std::abs(dx1-dx0),std::abs(dy1-dy0)},.colorMask=15},src,source.level,source.layer,{std::min(sx0,sx1),std::min(sy0,sy1),std::abs(sx1-sx0),std::abs(sy1-sy0)});
}
std::shared_ptr<Texture> CopyObject(GLuint name,GLenum target,GLint level){
    Require(name&&level>=0,GL_INVALID_VALUE);
    if(target==GL_RENDERBUFFER){Require(level==0);auto it=C().share->renderbuffers.find(name);Require(it!=C().share->renderbuffers.end()&&it->second->image,GL_INVALID_VALUE);return it->second;}
    Require(target==GL_TEXTURE_2D||target==GL_TEXTURE_3D||target==GL_TEXTURE_2D_ARRAY||target==GL_TEXTURE_CUBE_MAP,GL_INVALID_ENUM);
    auto it=C().share->textures.find(name);Require(it!=C().share->textures.end()&&it->second->image,GL_INVALID_VALUE);Require(it->second->target==target,GL_INVALID_ENUM);return it->second;
}
bool BoxRange(std::uint32_t dimension,GLint offset,std::uint32_t extent){return offset>=0&&static_cast<std::uint32_t>(offset)<=dimension&&extent<=dimension-static_cast<std::uint32_t>(offset);}
VkBufferImageCopy CopyBox(const Image& image,GLint level,GLint x,GLint y,GLint z,std::uint32_t width,std::uint32_t height,std::uint32_t depth){
    Require(level>=0&&static_cast<std::uint32_t>(level)<image.levels,GL_INVALID_VALUE);const bool volume=image.type==VK_IMAGE_TYPE_3D;
    Require(BoxRange(std::max(1u,image.width>>level),x,width)&&BoxRange(std::max(1u,image.height>>level),y,height)&&BoxRange(volume?std::max(1u,image.depth>>level):image.layers,z,depth),GL_INVALID_VALUE);
    VkBufferImageCopy box{};box.imageSubresource={image.aspect,static_cast<std::uint32_t>(level),volume?0u:static_cast<std::uint32_t>(z),volume?1u:depth};box.imageOffset={x,y,volume?z:0};box.imageExtent={width,height,volume?depth:1u};return box;
}
bool BoxesOverlap(GLint ax,GLint ay,GLint az,GLint bx,GLint by,GLint bz,std::uint32_t width,std::uint32_t height,std::uint32_t depth){
    return std::int64_t(ax)<std::int64_t(bx)+width&&std::int64_t(bx)<std::int64_t(ax)+width&&std::int64_t(ay)<std::int64_t(by)+height&&std::int64_t(by)<std::int64_t(ay)+height&&std::int64_t(az)<std::int64_t(bz)+depth&&std::int64_t(bz)<std::int64_t(az)+depth;
}
std::uint32_t CopyClass(const Texture& texture){
    switch(texture.internal){case GL_RGB8:case GL_SRGB8:case GL_RGB8_SNORM:return 3;case 0x8F9A:return 6;case GL_RGB565:case GL_RGBA4:case GL_RGB5_A1:return 2;default:return FormatBlock(texture.image->format).bytes;}
}
void BlockBox(const Image& image,const VkBufferImageCopy& box){
    const auto block=FormatBlock(image.format);const auto width=std::max(1u,image.width>>box.imageSubresource.mipLevel),height=std::max(1u,image.height>>box.imageSubresource.mipLevel);
    Require(box.imageOffset.x%block.x==0&&box.imageOffset.y%block.y==0&&(box.imageExtent.width%block.x==0||static_cast<std::uint32_t>(box.imageOffset.x)+box.imageExtent.width==width)&&(box.imageExtent.height%block.y==0||static_cast<std::uint32_t>(box.imageOffset.y)+box.imageExtent.height==height),GL_INVALID_VALUE);
}
}
void BlitFramebuffer(GLint sx0,GLint sy0,GLint sx1,GLint sy1,GLint dx0,GLint dy0,GLint dx1,GLint dy1,GLbitfield mask,GLenum filter){Call([&]{
    Require(!(mask&~(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT))&&(filter==GL_NEAREST||filter==GL_LINEAR));
    Require(!(mask&(GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT))||filter==GL_NEAREST,GL_INVALID_OPERATION);
    auto src=Target(C().readFbo,true),dst=Target(C().drawFbo);
    const auto aspects=src.logicalDsAspects&dst.logicalDsAspects;
    if(C().enabled.contains(GL_SCISSOR_TEST)){auto rect=ClipScissor(TransferExtent(dst,mask,aspects));const auto lowx=rect.offset.x,lowy=rect.offset.y,highx=rect.offset.x+static_cast<GLint>(rect.extent.width),highy=rect.offset.y+static_cast<GLint>(rect.extent.height);auto clip=[](GLint& s0,GLint& s1,GLint& d0,GLint& d1,GLint low,GLint high){if(d0==d1)return;const auto old0=d0,old1=d1,ss0=s0,ss1=s1;d0=std::clamp(d0,low,high);d1=std::clamp(d1,low,high);s0=ss0+std::lround(double(d0-old0)*(ss1-ss0)/(old1-old0));s1=ss0+std::lround(double(d1-old0)*(ss1-ss0)/(old1-old0));};clip(sx0,sx1,dx0,dx1,lowx,highx);clip(sy0,sy1,dy0,dy1,lowy,highy);}
    if(mask&GL_COLOR_BUFFER_BIT)for(auto& a:dst.colors)if(a.texture)Blit(src.colors[0],a,VK_IMAGE_ASPECT_COLOR_BIT,sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,filter==GL_LINEAR?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    if((mask&GL_DEPTH_BUFFER_BIT)&&(aspects&VK_IMAGE_ASPECT_DEPTH_BIT))Blit(src.depth,dst.depth,VK_IMAGE_ASPECT_DEPTH_BIT,sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,VK_FILTER_NEAREST);
    if((mask&GL_STENCIL_BUFFER_BIT)&&(aspects&VK_IMAGE_ASPECT_STENCIL_BIT))Blit(src.depth,dst.depth,VK_IMAGE_ASPECT_STENCIL_BIT,sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,VK_FILTER_NEAREST);
});}
void CopyTexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLint sx,GLint sy,GLsizei w,GLsizei h){Call([&]{Require(level>=0&&x>=0&&y>=0&&sx>=0&&sy>=0&&w>=0&&h>=0);auto src=Target(C().readFbo,true);Attachment dst{0,GL_TEXTURE,target,level,static_cast<GLint>(Face(target)),BoundTexture(target)};Blit(src.colors[0],dst,VK_IMAGE_ASPECT_COLOR_BIT,sx,sy,sx+w,sy+h,x,y,x+w,y+h,VK_FILTER_NEAREST);});}
void CopyImageSubData(GLuint source,GLenum sourceTarget,GLint sourceLevel,GLint sx,GLint sy,GLint sz,GLuint destination,GLenum destinationTarget,GLint destinationLevel,GLint dx,GLint dy,GLint dz,GLsizei width,GLsizei height,GLsizei depth){Call([&]{
    Require(width>=0&&height>=0&&depth>=0);auto sourceObject=CopyObject(source,sourceTarget,sourceLevel),destinationObject=CopyObject(destination,destinationTarget,destinationLevel);auto src=sourceObject->image,dst=destinationObject->image;
    Require(src->aspect==dst->aspect,GL_INVALID_OPERATION);const auto sb=FormatBlock(src->format),db=FormatBlock(dst->format);const bool compressedSource=Compressed(src->format),compressedDestination=Compressed(dst->format);
    if(src->aspect!=VK_IMAGE_ASPECT_COLOR_BIT)Require(src->format==dst->format,GL_INVALID_OPERATION);
    else {Require(CopyClass(*sourceObject)==CopyClass(*destinationObject),GL_INVALID_OPERATION);if(sourceObject->internal!=destinationObject->internal&&(CopyClass(*sourceObject)!=sb.bytes||CopyClass(*destinationObject)!=db.bytes))Fail("Unsupported bit reinterpretation between expanded packed CopyImageSubData formats");}
    Require(sb.bytes==db.bytes,GL_INVALID_OPERATION);
    if(compressedSource&&compressedDestination){Require(sb.x==db.x&&sb.y==db.y,GL_INVALID_OPERATION);if(src->format<VK_FORMAT_ASTC_4x4_UNORM_BLOCK||dst->format<VK_FORMAT_ASTC_4x4_UNORM_BLOCK)Require(src->format==dst->format,GL_INVALID_OPERATION);}
    auto sourceBox=CopyBox(*src,sourceLevel,sx,sy,sz,width,height,depth);
    Require(destinationLevel>=0&&static_cast<std::uint32_t>(destinationLevel)<dst->levels&&dx>=0&&dy>=0&&dz>=0,GL_INVALID_VALUE);
    const bool changedBlock=sb.x!=db.x||sb.y!=db.y;const auto destinationWidth=changedBlock?static_cast<std::uint64_t>((width+sb.x-1)/sb.x)*db.x:width,destinationHeight=changedBlock?static_cast<std::uint64_t>((height+sb.y-1)/sb.y)*db.y:height;
    const auto availableWidth=std::max(1u,dst->width>>destinationLevel),availableHeight=std::max(1u,dst->height>>destinationLevel);Require(static_cast<std::uint32_t>(dx)<=availableWidth&&static_cast<std::uint32_t>(dy)<=availableHeight);
    const auto dw=std::min<std::uint64_t>(destinationWidth,availableWidth-dx),dh=std::min<std::uint64_t>(destinationHeight,availableHeight-dy);
    if(destinationWidth>dw||destinationHeight>dh)Require(changedBlock&&compressedDestination&&destinationWidth-dw<db.x&&destinationHeight-dh<db.y,GL_INVALID_VALUE);
    auto destinationBox=CopyBox(*dst,destinationLevel,dx,dy,dz,dw,dh,depth);if(!width||!height||!depth)return;BlockBox(*src,sourceBox);BlockBox(*dst,destinationBox);
    Require(src!=dst||sourceLevel!=destinationLevel||!BoxesOverlap(sx,sy,sz,dx,dy,dz,width,height,depth),GL_INVALID_OPERATION);
    const auto sourceLayout=src==dst?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destinationLayout=src==dst?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    Transition(src,sourceLayout,sourceLevel,1,sourceBox.imageSubresource.baseArrayLayer,sourceBox.imageSubresource.layerCount);Transition(dst,destinationLayout,destinationLevel,1,destinationBox.imageSubresource.baseArrayLayer,destinationBox.imageSubresource.layerCount);
    if(changedBlock){
        const auto bytes=static_cast<std::uint64_t>((width+sb.x-1)/sb.x)*((height+sb.y-1)/sb.y)*depth*sb.bytes;Require(bytes&&bytes<=kMaxBuffer,GL_OUT_OF_MEMORY);auto slice=Upload(bytes,16);sourceBox.bufferOffset=destinationBox.bufferOffset=slice.offset;
        vkCmdCopyImageToBuffer(Recording().command,src->image,sourceLayout,slice.buffer->buffer,1,&sourceBox);
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=slice.buffer->buffer;barrier.offset=slice.offset;barrier.size=bytes;
        vkCmdPipelineBarrier(Recording().command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr);vkCmdCopyBufferToImage(Recording().command,slice.buffer->buffer,dst->image,destinationLayout,1,&destinationBox);
    }else {
        std::array<VkImageCopy,2> regions{};std::uint32_t count=0;for(auto aspect:{VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_ASPECT_STENCIL_BIT})if(src->aspect&aspect){auto& copy=regions[count++];copy.srcSubresource=sourceBox.imageSubresource;copy.dstSubresource=destinationBox.imageSubresource;copy.srcSubresource.aspectMask=copy.dstSubresource.aspectMask=aspect;copy.srcOffset=sourceBox.imageOffset;copy.dstOffset=destinationBox.imageOffset;copy.extent={static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),src->type==VK_IMAGE_TYPE_3D||dst->type==VK_IMAGE_TYPE_3D?static_cast<std::uint32_t>(depth):1u};}
        vkCmdCopyImage(Recording().command,src->image,sourceLayout,dst->image,destinationLayout,count,regions.data());
    }
    if(gpu.gfxCapture)RecordColorWrite(dst,destinationLevel,destinationBox.imageSubresource.baseArrayLayer,{.operation=RecordedColorOperation::CopyImage,.rectangle={dx,dy,static_cast<GLint>(dw),static_cast<GLint>(dh)},.colorMask=15},src,sourceLevel,sourceBox.imageSubresource.baseArrayLayer,{sx,sy,width,height});
});}
void ReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void* output){Call([&]{Require(w>=0&&h>=0&&x>=0&&y>=0);if(!w||!h)return;Require(format==GL_RGBA||format==GL_RGB||format==GL_RED||format==GL_RG||format==GL_DEPTH_COMPONENT||format==GL_DEPTH_STENCIL,GL_INVALID_ENUM);auto target=Target(C().readFbo,true);
    if(format==GL_DEPTH_COMPONENT||format==GL_DEPTH_STENCIL)Require(HasDsAspect(target,VK_IMAGE_ASPECT_DEPTH_BIT),GL_INVALID_OPERATION);
    if(format==GL_DEPTH_STENCIL)Require(HasDsAspect(target,VK_IMAGE_ASPECT_STENCIL_BIT),GL_INVALID_OPERATION);
    auto attachment=format==GL_DEPTH_COMPONENT||format==GL_DEPTH_STENCIL?target.depth:target.colors[0];Require(attachment.texture&&attachment.texture->image,GL_INVALID_FRAMEBUFFER_OPERATION);auto image=attachment.texture->image;auto data=Download(attachment,format==GL_DEPTH_COMPONENT||format==GL_DEPTH_STENCIL?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,x,y,w,h);std::vector<std::uint8_t> stencil;if(format==GL_DEPTH_STENCIL)stencil=Download(attachment,VK_IMAGE_ASPECT_STENCIL_BIT,x,y,w,h);
    const std::uint64_t pixelBytes=PixelBytes(format,type),rowBytes=static_cast<std::uint64_t>(C().packRow?C().packRow:w)*pixelBytes;Require(rowBytes<=kMaxBuffer);const auto stride=(rowBytes+C().packAlignment-1)/C().packAlignment*C().packAlignment,skip=static_cast<std::uint64_t>(C().packSkipRows)*stride+static_cast<std::uint64_t>(C().packSkipPixels)*pixelBytes,span=static_cast<std::uint64_t>(h-1)*stride+w*pixelBytes;Require(skip<=kMaxBuffer&&span<=kMaxBuffer-skip);auto it=C().boundBuffers.find(GL_PIXEL_PACK_BUFFER);std::uint8_t* destination;if(it!=C().boundBuffers.end()&&it->second.second){auto b=it->second.second;auto offset=reinterpret_cast<std::uintptr_t>(output);Require(offset<=b->bytes.size()&&skip<=b->bytes.size()-offset&&span<=b->bytes.size()-offset-skip,GL_INVALID_OPERATION);destination=b->bytes.data()+offset+skip;b->dirty=true;}else{Require(output);destination=static_cast<std::uint8_t*>(output)+skip;}
    const auto block=FormatBlock(image->format);
    for(GLint row=0;row<h;++row)for(GLint col=0;col<w;++col){const auto index=static_cast<std::size_t>(row)*w+col;auto* out=destination+row*stride+col*pixelBytes;const auto* p=data.data()+index*block.bytes;std::array<float,4> values{0,0,0,1};
        if(format==GL_DEPTH_STENCIL){Require(type==GL_UNSIGNED_INT_24_8&&image->format==VK_FORMAT_D24_UNORM_S8_UINT,GL_INVALID_OPERATION);std::uint32_t v;std::memcpy(&v,p,4);v=(v<<8)|stencil[index];std::memcpy(out,&v,4);continue;}
        if(format==GL_DEPTH_COMPONENT){if(image->format==VK_FORMAT_D16_UNORM){std::uint16_t v;std::memcpy(&v,p,2);values[0]=v/65535.f;}else if(image->format==VK_FORMAT_X8_D24_UNORM_PACK32||image->format==VK_FORMAT_D24_UNORM_S8_UINT){std::uint32_t v;std::memcpy(&v,p,4);values[0]=(v&0xffffff)/16777215.f;}else std::memcpy(&values[0],p,4);}
        else {values=DecodeColor(p,image->format);if(attachment.texture->internal==GL_RGB8_SNORM||attachment.texture->internal==0x8F9A)values[3]=1;}
        for(std::size_t k=0;k<Components(format);++k){if(type==GL_FLOAT)std::memcpy(out+k*4,&values[k],4);else if(type==GL_HALF_FLOAT||type==GL_HALF_FLOAT_OES){auto v=Half(values[k]);std::memcpy(out+k*2,&v,2);}else if(type==GL_BYTE){const auto v=static_cast<std::int8_t>(std::lround(std::clamp(values[k],-1.f,1.f)*127));std::memcpy(out+k,&v,1);}else if(type==GL_SHORT){const auto v=static_cast<std::int16_t>(std::lround(std::clamp(values[k],-1.f,1.f)*32767));std::memcpy(out+k*2,&v,2);}else if(type==GL_INT){const auto v=static_cast<std::int32_t>(std::llround(std::clamp<double>(values[k],-1,1)*2147483647.));std::memcpy(out+k*4,&v,4);}else if(type==GL_UNSIGNED_BYTE)out[k]=std::clamp<int>(std::lround(std::clamp(values[k],0.f,1.f)*255),0,255);else if(type==GL_UNSIGNED_SHORT){auto v=static_cast<std::uint16_t>(std::clamp(values[k],0.f,1.f)*65535+.5f);std::memcpy(out+k*2,&v,2);}else if(type==GL_UNSIGNED_INT){auto v=static_cast<std::uint32_t>(std::clamp<double>(values[k],0,1)*4294967295.);std::memcpy(out+k*4,&v,4);}else Fail("Unsupported framebuffer readback type");}
    }
});}

void GenQueries(GLsizei count,GLuint* names){Call([&]{auto& c=C();Generate(count,names,c.nextQuery,c.queries);for(GLsizei i=0;i<count;++i)c.queries[names[i]]=std::make_shared<Query>();});}
void DeleteQueries(GLsizei count,const GLuint* names){Call([&]{Require(count>=0&&(!count||names));for(GLsizei i=0;i<count;++i){auto it=C().queries.find(names[i]);if(it!=C().queries.end()){Require(it->second!=C().activeQuery,GL_INVALID_OPERATION);C().queries.erase(it);}}});}
void BeginQuery(GLenum target,GLuint name){Call([&]{Require(target==0x8914||target==GL_ANY_SAMPLES_PASSED||target==GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_INVALID_ENUM);Require(name&&C().queries.contains(name)&&!C().activeQuery,GL_INVALID_OPERATION);EndPass();auto q=C().queries.at(name);if(!q->allocation||q->allocation.use_count()>1){auto allocation=std::make_shared<QueryAllocation>();VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_OCCLUSION;info.queryCount=256;Check(vkCreateQueryPool(gpu.device,&info,nullptr,&allocation->pool),"vkCreateQueryPool");q->allocation=allocation;}vkCmdResetQueryPool(Recording().command,q->allocation->pool,0,256);q->pieces=0;q->active=false;q->ended=false;q->target=target;C().activeQuery=q;Retain(q->allocation);});}
void EndQuery(GLenum target){Call([&]{Require(target==0x8914||target==GL_ANY_SAMPLES_PASSED||target==GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_INVALID_ENUM);Require(C().activeQuery!=nullptr&&C().activeQuery->target==target,GL_INVALID_OPERATION);auto q=C().activeQuery;if(q->active){vkCmdEndQuery(Recording().command,q->allocation->pool,q->pieces-1);q->active=false;}q->ended=true;C().activeQuery.reset();});}
void GetQueryObjectuiv(GLuint name,GLenum pname,GLuint* value){Call([&]{Require(value&&C().queries.contains(name),GL_INVALID_OPERATION);Require(pname==GL_QUERY_RESULT||pname==GL_QUERY_RESULT_AVAILABLE||pname==0x9194,GL_INVALID_ENUM);auto q=C().queries.at(name);Require(q->ended,GL_INVALID_OPERATION);if(!q->pieces){*value=pname==GL_QUERY_RESULT_AVAILABLE?1:0;return;}if(C().frames[C().frameIndex]->recording)Submit(pname==GL_QUERY_RESULT);std::vector<std::uint64_t> results(q->pieces*2);auto status=vkGetQueryPoolResults(gpu.device,q->allocation->pool,0,q->pieces,results.size()*8,results.data(),16,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT|(pname==GL_QUERY_RESULT?VK_QUERY_RESULT_WAIT_BIT:0));Require(status==VK_SUCCESS||status==VK_NOT_READY,GL_INVALID_OPERATION);bool available=true;std::uint64_t samples=0;for(std::uint32_t i=0;i<q->pieces;++i){available&=results[i*2+1]!=0;samples+=results[i*2];}if(pname==GL_QUERY_RESULT_AVAILABLE)*value=available;else if(available)*value=q->target==0x8914?std::min<std::uint64_t>(samples,UINT32_MAX-1):samples!=0;});}
GLsync FenceSync(GLenum condition,GLbitfield flags){GLsync out=nullptr;Call([&]{Require(condition==GL_SYNC_GPU_COMMANDS_COMPLETE&&flags==0,GL_INVALID_VALUE);if(C().frames[C().frameIndex]->recording)Submit();auto sync=std::make_unique<Sync>();VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};Check(vkCreateFence(gpu.device,&info,nullptr,&sync->fence),"sync fence");Check(vkQueueSubmit(gpu.queue,0,nullptr,sync->fence),"sync queue fence");auto* p=sync.release();syncs.insert(p);out=reinterpret_cast<GLsync>(p);});return out;}
GLenum ClientWaitSync(GLsync sync,GLbitfield flags,GLuint64 timeout){GLenum out=GL_WAIT_FAILED;Call([&]{Require(!(flags&~GL_SYNC_FLUSH_COMMANDS_BIT));auto* p=reinterpret_cast<Sync*>(sync);Require(syncs.contains(p),GL_INVALID_VALUE);auto status=vkGetFenceStatus(gpu.device,p->fence);if(status==VK_SUCCESS){out=GL_ALREADY_SIGNALED;return;}if(flags&GL_SYNC_FLUSH_COMMANDS_BIT)Flush();status=vkWaitForFences(gpu.device,1,&p->fence,VK_TRUE,timeout);if(status==VK_TIMEOUT)out=GL_TIMEOUT_EXPIRED;else{Check(status,"client sync wait");out=GL_CONDITION_SATISFIED;}});return out;}
void WaitSync(GLsync sync,GLbitfield flags,GLuint64 timeout){Call([&]{Require(flags==0&&timeout==GL_TIMEOUT_IGNORED);auto* p=reinterpret_cast<Sync*>(sync);Require(syncs.contains(p));Check(vkWaitForFences(gpu.device,1,&p->fence,VK_TRUE,UINT64_MAX),"sync wait");});}
void DeleteSync(GLsync sync){Call([&]{if(!sync)return;auto* p=reinterpret_cast<Sync*>(sync);Require(syncs.contains(p));Check(vkWaitForFences(gpu.device,1,&p->fence,VK_TRUE,UINT64_MAX),"sync retirement");vkDestroyFence(gpu.device,p->fence,nullptr);syncs.erase(p);delete p;});}

bool SwapBuffers(ANativeWindow* native){
    bool result=false;Call([&]{
        Require(!gpu.failed,GL_INVALID_OPERATION);auto& c=C();Require(c.window==native&&native,GL_INVALID_OPERATION);
        const auto submitOffscreen=[&]{if(c.frames[c.frameIndex]->recording)Submit(false);};
        if(presentationPaused){submitOffscreen();return;}
        try {
            auto& window=Surface(native);auto& frame=Recording();std::uint32_t index=0;
            // A disappearing Android consumer cannot promise forward progress.
            // A finite acquire also bounds contention with JNI lifecycle calls.
            constexpr std::uint64_t acquireTimeout=250000000;
            auto status=vkAcquireNextImageKHR(gpu.device,window.swapchain,acquireTimeout,frame.acquired,VK_NULL_HANDLE,&index);
            if(status==VK_ERROR_OUT_OF_DATE_KHR){Resize(window);status=vkAcquireNextImageKHR(gpu.device,window.swapchain,acquireTimeout,frame.acquired,VK_NULL_HANDLE,&index);}
            if(status==VK_ERROR_OUT_OF_DATE_KHR||status==VK_TIMEOUT||status==VK_NOT_READY){submitOffscreen();return;}
            if(status!=VK_SUCCESS&&status!=VK_SUBOPTIMAL_KHR)SurfaceCheck(window,status,"vkAcquireNextImageKHR");
            const bool acquiredSuboptimal=status==VK_SUBOPTIMAL_KHR;
            auto image=c.defaultColor->image;Transition(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=window.layouts[index];barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=window.images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[0]={0,static_cast<GLint>(image->height),0};blit.srcOffsets[1]={static_cast<GLint>(image->width),0,1};blit.dstOffsets[1]={static_cast<GLint>(window.extent.width),static_cast<GLint>(window.extent.height),1};vkCmdBlitImage(frame.command,image->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,window.images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);window.layouts[index]=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;const auto acquired=frame.acquired;const auto ready=window.ready[index];Submit(false,acquired,ready);
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&ready;present.swapchainCount=1;present.pSwapchains=&window.swapchain;present.pImageIndices=&index;status=vkQueuePresentKHR(gpu.queue,&present);
            if(status==VK_ERROR_OUT_OF_DATE_KHR)Resize(window);
            else {if(status!=VK_SUCCESS&&status!=VK_SUBOPTIMAL_KHR)SurfaceCheck(window,status,"vkQueuePresentKHR");if((acquiredSuboptimal||status==VK_SUBOPTIMAL_KHR)&&SurfaceChanged(window))Resize(window);}
            DefaultTarget(c,window.extent.width,window.extent.height);window.color=c.defaultColor;window.depth=c.defaultDepth;result=true;
        }catch(const SurfaceUnavailable&){submitOffscreen();}
    });return result;
}

namespace {
const std::vector<std::string>& Extensions(){if(!gpu.extensions.empty())return gpu.extensions;gpu.extensions={
    "GL_ARB_vertex_buffer_object","GL_ARB_map_buffer_range","GL_ARB_vertex_array_object","GL_ARB_framebuffer_object",
    "GL_ARB_depth_texture","GL_ARB_shadow","GL_ARB_texture_float","GL_ARB_half_float_pixel","GL_ARB_half_float_vertex",
    "GL_ARB_texture_non_power_of_two","GL_ARB_draw_instanced","GL_ARB_instanced_arrays",
    "GL_ARB_draw_elements_base_vertex","GL_ARB_sync","GL_ARB_texture_storage",
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_buffer_storage",
    "GL_KHR_texture_compression_astc_ldr","GL_OES_depth_texture","GL_OES_packed_depth_stencil","GL_OES_mapbuffer",
    "GL_EXT_discard_framebuffer"};if(gpu.features.samplerAnisotropy)gpu.extensions.push_back("GL_EXT_texture_filter_anisotropic");if(gpu.features.depthBounds)gpu.extensions.push_back("GL_EXT_depth_bounds_test");return gpu.extensions;}
const std::vector<GLint>& CompressedFormats(){static const std::vector<GLint> formats{0x83F1,0x83F3,GL_COMPRESSED_RGB8_ETC2,GL_COMPRESSED_RGBA8_ETC2_EAC,0x93B0,0x93D0};return formats;}
std::vector<double> State(GLenum name){
    Require(gpu.device,GL_INVALID_OPERATION);const auto& p=gpu.properties.limits;
    switch(name){
        case GL_MAX_TEXTURE_SIZE:case GL_MAX_RENDERBUFFER_SIZE:return {double(p.maxImageDimension2D)};
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE:return {double(p.maxImageDimensionCube)};case GL_MAX_3D_TEXTURE_SIZE:return {double(p.maxImageDimension3D)};
        case GL_MAX_ARRAY_TEXTURE_LAYERS:return {double(p.maxImageArrayLayers)};case GL_MAX_VERTEX_ATTRIBS:return {16};
        case GL_MAX_TEXTURE_IMAGE_UNITS:case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:return {16};case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:return {32};
        case GL_MAX_DRAW_BUFFERS:case GL_MAX_COLOR_ATTACHMENTS:return {4};case GL_MAX_SAMPLES:return {1};
        case GL_MAX_VIEWPORT_DIMS:return {double(p.maxViewportDimensions[0]),double(p.maxViewportDimensions[1])};
        case GL_MAX_VERTEX_UNIFORM_VECTORS:case GL_MAX_FRAGMENT_UNIFORM_VECTORS:return {double(p.maxUniformBufferRange/16)};
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS:case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:return {double(p.maxUniformBufferRange/4)};
        case GL_MAX_VARYING_VECTORS:return {double(std::min(p.maxVertexOutputComponents,p.maxFragmentInputComponents)/4)};
        case GL_MAX_VERTEX_OUTPUT_COMPONENTS:return {double(p.maxVertexOutputComponents)};case GL_MAX_FRAGMENT_INPUT_COMPONENTS:return {double(p.maxFragmentInputComponents)};
        case GL_MAX_UNIFORM_BLOCK_SIZE:return {double(p.maxUniformBufferRange)};case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT:return {double(p.minUniformBufferOffsetAlignment)};
        case 0x864B:return {gpu.failed?0.:-1.};case GL_NUM_EXTENSIONS:return {double(Extensions().size())};case GL_MAJOR_VERSION:return {4};case GL_MINOR_VERSION:return {5};
        case GL_NUM_COMPRESSED_TEXTURE_FORMATS:return {double(CompressedFormats().size())};case GL_COMPRESSED_TEXTURE_FORMATS:return std::vector<double>(CompressedFormats().begin(),CompressedFormats().end());
        case 0x84FF:return {gpu.features.samplerAnisotropy?p.maxSamplerAnisotropy:1};
        case GL_ALIASED_POINT_SIZE_RANGE:case GL_ALIASED_LINE_WIDTH_RANGE:return {1,1};
        case GL_SHADER_COMPILER:return {0};case GL_NUM_SHADER_BINARY_FORMATS:case GL_NUM_PROGRAM_BINARY_FORMATS:return {0};
        default:break;
    }
    auto& c=C();switch(name){
        case GL_VIEWPORT:return std::vector<double>(c.viewport.begin(),c.viewport.end());case GL_SCISSOR_BOX:return std::vector<double>(c.scissor.begin(),c.scissor.end());
        case GL_COLOR_CLEAR_VALUE:return std::vector<double>(c.clearColor.begin(),c.clearColor.end());case GL_COLOR_WRITEMASK:return std::vector<double>(c.colorWrite.begin(),c.colorWrite.end());
        case GL_BLEND_COLOR:return std::vector<double>(c.blendColor.begin(),c.blendColor.end());case GL_DEPTH_RANGE:return {c.nearDepth,c.farDepth};
        case GL_DEPTH_CLEAR_VALUE:return {c.clearDepth};case GL_STENCIL_CLEAR_VALUE:return {double(c.clearStencil)};case GL_DEPTH_WRITEMASK:return {double(c.depthWrite)};case GL_DEPTH_FUNC:return {double(c.depthFunction)};
        case GL_CULL_FACE_MODE:return {double(c.cull)};case GL_FRONT_FACE:return {double(c.front)};case GL_LINE_WIDTH:return {c.lineWidth};
        case GL_POLYGON_OFFSET_FACTOR:return {c.polygonFactor};case GL_POLYGON_OFFSET_UNITS:return {c.polygonUnits};
        case GL_BLEND_SRC_RGB:return {double(c.srcRgb)};case GL_BLEND_DST_RGB:return {double(c.dstRgb)};case GL_BLEND_SRC_ALPHA:return {double(c.srcAlpha)};case GL_BLEND_DST_ALPHA:return {double(c.dstAlpha)};
        case GL_BLEND_EQUATION_RGB:return {double(c.rgbEquation)};case GL_BLEND_EQUATION_ALPHA:return {double(c.alphaEquation)};
        case GL_STENCIL_FUNC:return {double(c.frontStencil.function)};case GL_STENCIL_REF:return {double(c.frontStencil.reference)};case GL_STENCIL_VALUE_MASK:return {double(c.frontStencil.compareMask)};case GL_STENCIL_WRITEMASK:return {double(c.frontStencil.writeMask)};
        case GL_STENCIL_FAIL:return {double(c.frontStencil.fail)};case GL_STENCIL_PASS_DEPTH_FAIL:return {double(c.frontStencil.depthFail)};case GL_STENCIL_PASS_DEPTH_PASS:return {double(c.frontStencil.pass)};
        case GL_STENCIL_BACK_FUNC:return {double(c.backStencil.function)};case GL_STENCIL_BACK_REF:return {double(c.backStencil.reference)};case GL_STENCIL_BACK_VALUE_MASK:return {double(c.backStencil.compareMask)};case GL_STENCIL_BACK_WRITEMASK:return {double(c.backStencil.writeMask)};
        case GL_STENCIL_BACK_FAIL:return {double(c.backStencil.fail)};case GL_STENCIL_BACK_PASS_DEPTH_FAIL:return {double(c.backStencil.depthFail)};case GL_STENCIL_BACK_PASS_DEPTH_PASS:return {double(c.backStencil.pass)};
        case GL_ACTIVE_TEXTURE:return {double(GL_TEXTURE0+c.activeTexture)};case GL_SAMPLER_BINDING:return {double(c.samplerUnits[c.activeTexture])};
        case GL_TEXTURE_BINDING_2D:case GL_TEXTURE_BINDING_CUBE_MAP:case GL_TEXTURE_BINDING_3D:case GL_TEXTURE_BINDING_2D_ARRAY:{auto target=name==GL_TEXTURE_BINDING_2D?GL_TEXTURE_2D:name==GL_TEXTURE_BINDING_CUBE_MAP?GL_TEXTURE_CUBE_MAP:name==GL_TEXTURE_BINDING_3D?GL_TEXTURE_3D:GL_TEXTURE_2D_ARRAY;auto it=c.textureUnits[c.activeTexture].find(target);return {double(it==c.textureUnits[c.activeTexture].end()?0:it->second.first)};}
        case GL_ARRAY_BUFFER_BINDING:case GL_PIXEL_PACK_BUFFER_BINDING:case GL_PIXEL_UNPACK_BUFFER_BINDING:case GL_COPY_READ_BUFFER_BINDING:case GL_COPY_WRITE_BUFFER_BINDING:case GL_UNIFORM_BUFFER_BINDING:{auto target=name==GL_ARRAY_BUFFER_BINDING?GL_ARRAY_BUFFER:name==GL_PIXEL_PACK_BUFFER_BINDING?GL_PIXEL_PACK_BUFFER:name==GL_PIXEL_UNPACK_BUFFER_BINDING?GL_PIXEL_UNPACK_BUFFER:name==GL_COPY_READ_BUFFER_BINDING?GL_COPY_READ_BUFFER:name==GL_COPY_WRITE_BUFFER_BINDING?GL_COPY_WRITE_BUFFER:GL_UNIFORM_BUFFER;auto it=c.boundBuffers.find(target);return {double(it==c.boundBuffers.end()?0:it->second.first)};}
        case GL_ELEMENT_ARRAY_BUFFER_BINDING:return {double(Vao().elementName)};case GL_VERTEX_ARRAY_BINDING:return {double(c.vao)};
        case GL_FRAMEBUFFER_BINDING:return {double(c.drawFbo)};case GL_READ_FRAMEBUFFER_BINDING:return {double(c.readFbo)};case GL_RENDERBUFFER_BINDING:return {double(c.boundRenderbuffer)};
        case GL_CURRENT_PROGRAM:return {0};case GL_READ_BUFFER:return {double(c.readFbo?c.fbos.at(c.readFbo).read:GL_BACK)};
        case GL_UNPACK_ALIGNMENT:return {double(c.unpackAlignment)};case GL_PACK_ALIGNMENT:return {double(c.packAlignment)};case GL_UNPACK_ROW_LENGTH:return {double(c.unpackRow)};case GL_UNPACK_IMAGE_HEIGHT:return {double(c.unpackImage)};case GL_UNPACK_SKIP_ROWS:return {double(c.unpackSkipRows)};case GL_UNPACK_SKIP_PIXELS:return {double(c.unpackSkipPixels)};case GL_UNPACK_SKIP_IMAGES:return {double(c.unpackSkipImages)};
        case GL_PACK_ROW_LENGTH:return {double(c.packRow)};case GL_PACK_SKIP_ROWS:return {double(c.packSkipRows)};case GL_PACK_SKIP_PIXELS:return {double(c.packSkipPixels)};
        case 0x0D52:case 0x0D53:case 0x0D54:case 0x0D55:return {8};
        case 0x0D56:case 0x0D57:{const bool depth=name==0x0D56;auto texture=c.drawFbo?(depth?c.fbos.at(c.drawFbo).depth:c.fbos.at(c.drawFbo).stencil).texture:c.defaultDepth;return {double(texture&&texture->image?AspectBits(texture->image->format,depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT):0)};}
        case GL_SAMPLES:case GL_SAMPLE_BUFFERS:return {0};case GL_IMPLEMENTATION_COLOR_READ_FORMAT:return {GL_RGBA};case GL_IMPLEMENTATION_COLOR_READ_TYPE:return {GL_UNSIGNED_BYTE};
        default:if(name>=GL_DRAW_BUFFER0&&name<GL_DRAW_BUFFER0+4)return {double(c.drawFbo?c.fbos.at(c.drawFbo).draw[name-GL_DRAW_BUFFER0]:name==GL_DRAW_BUFFER0?GL_BACK:GL_NONE)};if(Capability(name))return {double(c.enabled.contains(name))};Require(false,GL_INVALID_ENUM);return {};
    }
}
}
const GLubyte* GetString(GLenum name){const char* out=nullptr;Call([&]{switch(name){case 0x8874:out=gpu.failed?gpu.failure.c_str():"";break;case GL_VENDOR:out="NVIDIA Corporation";break;case GL_RENDERER:out="NVIDIA Tegra";break;case GL_VERSION:out="4.5.0 NVIDIA 361.00";break;case GL_SHADING_LANGUAGE_VERSION:out="4.50 BL2 offline Vulkan";break;case GL_EXTENSIONS:{if(gpu.extensionString.empty())for(auto& e:Extensions()){if(!gpu.extensionString.empty())gpu.extensionString+=' ';gpu.extensionString+=e;}out=gpu.extensionString.c_str();break;}default:Require(false,GL_INVALID_ENUM);}});return reinterpret_cast<const GLubyte*>(out);}
const GLubyte* GetStringi(GLenum name,GLuint index){const GLubyte* out=nullptr;Call([&]{Require(name==GL_EXTENSIONS,GL_INVALID_ENUM);Require(index<Extensions().size());out=reinterpret_cast<const GLubyte*>(Extensions()[index].c_str());});return out;}
void GetIntegerv(GLenum name,GLint* output){Call([&]{Require(output);auto values=State(name);for(std::size_t i=0;i<values.size();++i)output[i]=static_cast<GLint>(std::clamp(values[i],double(INT32_MIN),double(INT32_MAX)));});}
void GetFloatv(GLenum name,GLfloat* output){Call([&]{Require(output);auto values=State(name);for(std::size_t i=0;i<values.size();++i)output[i]=values[i];});}
void GetBooleanv(GLenum name,GLboolean* output){Call([&]{Require(output);auto values=State(name);for(std::size_t i=0;i<values.size();++i)output[i]=values[i]!=0;});}
bool SelfCheck(){
    try {
        if(!RecordedImageWriterSelfCheck()||!RecordedAoWitnessSelfCheck()||!RecordedBlurWitnessSelfCheck()||!RecordedPriorBlurWitnessSelfCheck()||!ConsumerAfterImagePairSelfCheck())return false;
        const auto sizedAttachment=[](std::uint32_t width,std::uint32_t height,VkImageAspectFlags aspects){Attachment attachment;attachment.texture=std::make_shared<Texture>();attachment.texture->image=std::make_shared<Image>();auto& image=*attachment.texture->image;image.width=width;image.height=height;image.levels=2;image.layers=1;image.aspect=aspects;return attachment;};
        Framebuffer mixed;mixed.colors[0]=sizedAttachment(1200,540,VK_IMAGE_ASPECT_COLOR_BIT);mixed.depth=sizedAttachment(1200,544,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);mixed.stencil=mixed.depth;VkExtent2D common{};
        if(FramebufferExtent(mixed,common)!=GL_FRAMEBUFFER_COMPLETE||common.width!=1200||common.height!=540)return false;
        mixed.colors[0].texture->image->height=544;mixed.depth.texture->image->height=540;
        if(FramebufferExtent(mixed,common)!=GL_FRAMEBUFFER_COMPLETE||common.width!=1200||common.height!=540)return false;
        mixed.colors[0].texture->image->width=2400;mixed.colors[0].texture->image->height=1080;mixed.colors[0].level=1;
        if(FramebufferExtent(mixed,common)!=GL_FRAMEBUFFER_COMPLETE||common.width!=1200||common.height!=540)return false;
        mixed.colors[1]=sizedAttachment(320,128,VK_IMAGE_ASPECT_COLOR_BIT);mixed.draw[1]=GL_NONE;mixed.read=GL_COLOR_ATTACHMENT0;
        if(FramebufferExtent(mixed,common)!=GL_FRAMEBUFFER_COMPLETE||common.width!=320||common.height!=128)return false;
        mixed.colors[1].level=2;if(FramebufferExtent(mixed,common)!=GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT)return false;mixed.colors[1].level=0;
        mixed.colors[1].layer=1;if(FramebufferExtent(mixed,common)!=GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT)return false;mixed.colors[1].layer=0;
        mixed.colors[1].texture->image->aspect=VK_IMAGE_ASPECT_DEPTH_BIT;if(FramebufferExtent(mixed,common)!=GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT)return false;
        if(FramebufferExtent(Framebuffer{},common)!=GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT)return false;
        Targets transfer;transfer.colors[0]=sizedAttachment(1200,544,VK_IMAGE_ASPECT_COLOR_BIT);transfer.depth=mixed.depth;
        auto transferColor=TransferExtent(transfer,GL_COLOR_BUFFER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT),transferDs=TransferExtent(transfer,GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT),transferBoth=TransferExtent(transfer,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        if(transferColor.width!=1200||transferColor.height!=544||transferDs.width!=1200||transferDs.height!=540||transferBoth.width!=1200||transferBoth.height!=540||TransferExtent(transfer,GL_STENCIL_BUFFER_BIT,0).width)return false;
        Framebuffer logical;auto packed=std::make_shared<Texture>();logical.depth.texture=packed;logical.stencil=logical.depth;
        if(LogicalDsAspects(logical)!=(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)||!SameAttachmentImage(logical.depth,logical.stencil))return false;
        ++logical.stencil.level;if(SameAttachmentImage(logical.depth,logical.stencil))return false;--logical.stencil.level;
        ++logical.stencil.layer;if(SameAttachmentImage(logical.depth,logical.stencil))return false;--logical.stencil.layer;
        logical.stencil.texture.reset();if(LogicalDsAspects(logical)!=VK_IMAGE_ASPECT_DEPTH_BIT)return false;
        logical.stencil.texture=packed;logical.depth.texture.reset();if(LogicalDsAspects(logical)!=VK_IMAGE_ASPECT_STENCIL_BIT)return false;
        logical.stencil.texture.reset();if(LogicalDsAspects(logical))return false;
        for(auto aspects:{0u,static_cast<unsigned>(VK_IMAGE_ASPECT_DEPTH_BIT),static_cast<unsigned>(VK_IMAGE_ASPECT_STENCIL_BIT),static_cast<unsigned>(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)}){
            std::array<std::uint32_t,26> effective;effective.fill(1);auto expected=effective;
            expected[0]=expected[1]=expected[24]=(aspects&VK_IMAGE_ASPECT_DEPTH_BIT)!=0;expected[3]=(aspects&VK_IMAGE_ASPECT_STENCIL_BIT)!=0;
            EffectiveDsState(effective,aspects);if(effective!=expected)return false;
        }
        if(AspectBits(VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_ASPECT_DEPTH_BIT)!=24||AspectBits(VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_ASPECT_STENCIL_BIT)!=8||AspectBits(VK_FORMAT_D16_UNORM,VK_IMAGE_ASPECT_DEPTH_BIT)!=16||AspectBits(VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_DEPTH_BIT)!=32||AspectBits(VK_FORMAT_S8_UINT,VK_IMAGE_ASPECT_DEPTH_BIT)||AspectBits(VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_STENCIL_BIT))return false;
        if(!RecoverableSurfaceStatus(VK_ERROR_SURFACE_LOST_KHR)||RecoverableSurfaceStatus(VK_ERROR_DEVICE_LOST)||RecoverableSurfaceStatus(VK_ERROR_OUT_OF_DEVICE_MEMORY)||RecoverableSurfaceStatus(VK_ERROR_OUT_OF_DATE_KHR)||RecoverableSurfaceStatus(VK_SUCCESS))return false;
        Window lost;lost.unavailable=true;lost.generation=presentationGeneration;if(RetrySurface(lost))return false;lost.generation=presentationGeneration-1;if(!RetrySurface(lost))return false;lost.unavailable=false;lost.generation=presentationGeneration;if(!RetrySurface(lost))return false;
        Window oldWindow,freshWindow;oldWindow.native=reinterpret_cast<ANativeWindow*>(1);freshWindow.native=reinterpret_cast<ANativeWindow*>(2);
        MarkPresentationSurface(oldWindow,freshWindow.native);MarkPresentationSurface(freshWindow,freshWindow.native);
        if(RetrySurface(oldWindow)||!RetrySurface(freshWindow)||freshWindow.generation==presentationGeneration)return false;
        MarkPresentationSurface(oldWindow,oldWindow.native);if(!RetrySurface(oldWindow)||oldWindow.generation==presentationGeneration)return false;
        MarkPresentationSurface(oldWindow,reinterpret_cast<ANativeWindow*>(3));if(RetrySurface(oldWindow))return false;
        MarkPresentationSurface(oldWindow,nullptr);if(RetrySurface(oldWindow))return false;
        if(ShaderLabel("//ShaderFileName: GFxVertexShaderbase_text \r\nvoid main(){}")!="GFxVertexShaderbase_text"||ShaderLabel("//ShaderFileName:\tGFxPixelShaderbase_textAlt0\t\n")!="GFxPixelShaderbase_textAlt0"||!ShaderLabel("void main(){}").empty())return false;
        const std::string spriteRaw="//ShaderFileName: BasePassVertexShader \r\n// PARTICLES_ALLOW_AXIS_ROTATION = 1 \r\n// NUM_AXIS_ROTATION_VECTORS = 2 \r\n// USE_OCCLUSION_PERCENTAGE = 1 \r\n// USE_PARTICLE_VERTEX_INDEX = 0 \r\n";
        if(!ParticleSpriteSource(spriteRaw)||!ParticleSpriteSource(spriteRaw+"// SUBUV_PARTICLES = 1\n// USE_DYNAMIC_PARAMETERS = 1\n")||ParticleSpriteSource("//ShaderFileName: BasePassVertexShader\n// USE_OCCLUSION_PERCENTAGE = 1\n")||ParticleSpriteSource("//ShaderFileName: GFxVertexShaderbase\n"+spriteRaw.substr(spriteRaw.find("\n")+1)))return false;
        if(!ShaderCommentValue("// PARTICLES_ALLOW_AXIS_ROTATION_EXTRA = 1\n","PARTICLES_ALLOW_AXIS_ROTATION").empty()||!ShaderCommentValue("// comment mentions PARTICLES_ALLOW_AXIS_ROTATION = 1\n","PARTICLES_ALLOW_AXIS_ROTATION").empty()||ShaderCommentValue("//\tPARTICLES_ALLOW_AXIS_ROTATION\t= 0\n","PARTICLES_ALLOW_AXIS_ROTATION")!="0"||!TranslucentBasePassSource("//ShaderFileName: BasePassPixelShader\n// MATERIALBLENDING_TRANSLUCENT = 1\n")||TranslucentBasePassSource("//ShaderFileName: BasePassPixelShader\n// MATERIALBLENDING_TRANSLUCENT = 0\n// MATERIALBLENDING_ADDITIVE = 1\n"))return false;
        std::unordered_map<std::string,std::uint32_t> particlePairs;std::uint32_t particleCaptures=0;
        if(!TakeParticleCapture(particlePairs,particleCaptures,"a")||TakeParticleCapture(particlePairs,particleCaptures,"a"))return false;
        for(auto pair:{"b","c","d"})if(!TakeParticleCapture(particlePairs,particleCaptures,pair))return false;
        if(TakeParticleCapture(particlePairs,particleCaptures,"e")||particlePairs.size()!=4||particleCaptures!=4)return false;
        for(auto pair:{"a","b","c","d"})if(TakeParticleCapture(particlePairs,particleCaptures,pair))return false;
        constexpr std::string_view selectedV="bb95115f9e48266950c02a252308be05e1d485cc8e78c12c0e3d6441801b04b3",selectedP="d26d89b92f67348605297147f10e67c46eb20bd95bd756cff5323ab9903ff433";
        constexpr std::string_view selectedVLabel="AmbientOcclusionShader",selectedPLabel="AmbientOcclusionShader";
        if(!SelectedDeepCapture(selectedVLabel,selectedPLabel,selectedV,selectedP)||SelectedDeepCapture("BasePassVertexShader",selectedPLabel,selectedV,selectedP)||SelectedDeepCapture(selectedVLabel,"BasePassPixelShader",selectedV,selectedP)||SelectedDeepCapture(selectedVLabel,selectedPLabel,selectedV,"9f518670bdc1c09649df103bba58fc2f5685c73856bc840bc3cccd4ad6dca433")||SelectedDeepCapture(selectedVLabel,selectedPLabel,"b3c51c333053243589a64f5cd4bf27f38950688a9aeb0c415aad0e068684b433",selectedP)||SelectedDeepCapture(selectedVLabel,selectedPLabel,selectedV,selectedP.substr(0,63)))return false;
        if(SelectedDeepCapture("BasePassVertexShader","BasePassPixelShader","b3c51c333053243589a64f5cd4bf27f38950688a9aeb0c415aad0e068684b433","9f518670bdc1c09649df103bba58fc2f5685c73856bc840bc3cccd4ad6dca433")||SelectedDeepCapture(selectedVLabel,selectedPLabel,"b3c51c333053243589a64f5cd4bf27f38950688a9aeb0c415aad0e068684b433","9f518670bdc1c09649df103bba58fc2f5685c73856bc840bc3cccd4ad6dca433"))return false;
        if(SelectedDeepCapture("HeightFogVertexShader","HeightFogPixelShader","d8a2dfd0abca7e185c0f139785b770303eaf644f5b78067d004b6b82c76fa924","3ca4f4ead86ecaf10461361354f1d4137747d1844dc2adefddbee8c36904e28c"))return false;
        if(SelectedDeepCapture("AmbientOcclusionShader","AmbientOcclusionShader","e8421fe16d91eadcc049aea07574515776b439daadef9a458d2ec2cb1cf23e7b","2df50ae2b7fe2df17ae90065d05065721e5db087387d4f4d6a7d98ba1b63e304"))return false;
        std::unordered_map<std::string,std::uint32_t> selectedPairs;std::uint32_t selectedCaptures=0;
        const auto selectedAdmission=[&](std::string_view v,std::string_view p,bool available=true){return SelectedDeepCapture(selectedVLabel,selectedPLabel,v,p)&&available&&TakeParticleCapture(selectedPairs,selectedCaptures,std::string(v)+std::string(p));};
        if(selectedAdmission("c16e19dbfc2effba73ae770fe3ee55acfd0e455dc095867e4f19d8d1cb7c6555","82b367f2779bc7296ea58d172511ebaca18bad2e86ecd2f421a4ea1324614746")||selectedAdmission(selectedV,"5ff731b8e23281b24f7a7e994c0a580e1846641454640343b94753beaca97fc0")||selectedAdmission("ad87494f6773fb1ec95434e5cfb6af53ac6618167b4b1a40714f3a6802b5c86a","1f13d3986aa584231d12885846b170d46ec2a6242dea551ca74bda6be7d831e6")||!selectedPairs.empty()||selectedCaptures)return false;
        if(selectedAdmission(selectedV,selectedP,false)||!selectedPairs.empty()||selectedCaptures)return false;
        if(!selectedAdmission(selectedV,selectedP)||selectedAdmission(selectedV,selectedP)||selectedPairs.size()!=1||selectedCaptures!=1)return false;
        std::uint32_t shadowReports=0;if(TakeShadowDescriptorReport(false,true,shadowReports)||TakeShadowDescriptorReport(true,false,shadowReports)||shadowReports)return false;
        for(unsigned i=0;i<8;++i)if(!TakeShadowDescriptorReport(true,true,shadowReports)||shadowReports!=i+1)return false;
        if(TakeShadowDescriptorReport(true,true,shadowReports)||shadowReports!=8)return false;
        ConsumerTargetPlan consumerPlan;
        if(!PlanConsumerTarget(1200,544,6276432,3,7,170,8*1024*1024,consumerPlan)||consumerPlan.rowBytes!=9600||consumerPlan.imageBytes!=5222400||consumerPlan.totalBytes!=10444800||consumerPlan.rowsPerTile!=436||consumerPlan.tilesPerImage!=2||6276432+consumerPlan.totalBytes!=16721232||16*1024*1024-6276432-consumerPlan.totalBytes!=55984)return false;
        constexpr std::size_t pairBytes=10444800;const auto exactAtlas=16*1024*1024-pairBytes,exactDiagnostic=31*1024*1024-65536-pairBytes;
        if(!PlanConsumerTarget(1200,544,exactAtlas,6,28,467,exactDiagnostic,consumerPlan)||PlanConsumerTarget(1200,544,exactAtlas+1,6,28,467,exactDiagnostic,consumerPlan)||PlanConsumerTarget(1200,544,0,7,0,0,0,consumerPlan)||PlanConsumerTarget(1200,544,0,0,29,0,0,consumerPlan)||PlanConsumerTarget(1200,544,0,0,0,468,0,consumerPlan)||PlanConsumerTarget(1200,544,0,0,0,0,exactDiagnostic+1,consumerPlan))return false;
        if(PlanConsumerTarget(0,544,0,0,0,0,0,consumerPlan)||PlanConsumerTarget(1200,0,0,0,0,0,0,consumerPlan)||PlanConsumerTarget(~0u,~0u,0,0,0,0,0,consumerPlan)||PlanConsumerTarget(524289,1,0,0,0,0,0,consumerPlan)||PlanConsumerTarget(1200,2000,0,0,0,0,0,consumerPlan)||PlanConsumerTarget(1,1,16*1024*1024+1,0,0,0,0,consumerPlan)||consumerPlan.totalBytes)return false;
        if(!PlanConsumerTarget(600,272,0,0,0,0,0,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||consumerPlan.rowBytes!=2400||consumerPlan.imageBytes!=652800||consumerPlan.totalBytes!=1305600||consumerPlan.rowsPerTile!=1747||consumerPlan.tilesPerImage!=1)return false;
        constexpr std::size_t rgPairBytes=1305600;const auto rgExactAtlas=16*1024*1024-rgPairBytes,rgExactDiagnostic=31*1024*1024-65536-rgPairBytes;
        if(!PlanConsumerTarget(600,272,rgExactAtlas,6,30,469,rgExactDiagnostic,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||PlanConsumerTarget(600,272,rgExactAtlas+1,6,30,469,rgExactDiagnostic,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||PlanConsumerTarget(600,272,0,7,0,0,0,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||PlanConsumerTarget(600,272,0,0,31,0,0,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||PlanConsumerTarget(600,272,0,0,0,470,0,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||PlanConsumerTarget(600,272,0,0,0,0,rgExactDiagnostic+1,consumerPlan,VK_FORMAT_R16G16_SFLOAT))return false;
        if(ConsumerTargetPixelBytes(VK_FORMAT_R8G8B8A8_UNORM)||PlanConsumerTarget(600,272,0,0,0,0,0,consumerPlan,VK_FORMAT_R8G8B8A8_UNORM)||PlanConsumerTarget(1048577,1,0,0,0,0,0,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||PlanConsumerTarget(0,272,0,0,0,0,0,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||PlanConsumerTarget(~0u,~0u,0,0,0,0,0,consumerPlan,VK_FORMAT_R16G16_SFLOAT)||consumerPlan.totalBytes)return false;
        std::array<std::uint8_t,4*88> consumerCpu{},consumerGpu{};for(std::size_t i=0;i<consumerCpu.size();++i)consumerCpu[i]=static_cast<std::uint8_t>(i);consumerGpu=consumerCpu;
        const std::vector<std::uint32_t> consumerRefs{1131,1133,1134,1131,1132,1133};std::vector<std::uint8_t> cpuSamples,gpuSamples;
        if(!ConsumerSamples(consumerCpu.data(),consumerCpu.size(),12,88,12,consumerRefs,1131,cpuSamples)||!ConsumerSamples(consumerGpu.data(),consumerGpu.size(),12,88,12,consumerRefs,1131,gpuSamples)||cpuSamples.size()!=72||cpuSamples!=gpuSamples||Hash(cpuSamples.data(),cpuSamples.size())!=Hash(gpuSamples.data(),gpuSamples.size()))return false;
        for(std::size_t i=0;i<consumerRefs.size();++i)if(std::memcmp(cpuSamples.data()+i*12,consumerCpu.data()+12+(consumerRefs[i]-1131)*88,12))return false;
        consumerGpu[12+3*88]^=0x80;if(!ConsumerSamples(consumerGpu.data(),consumerGpu.size(),12,88,12,consumerRefs,1131,gpuSamples)||cpuSamples==gpuSamples||Hash(cpuSamples.data(),cpuSamples.size())==Hash(gpuSamples.data(),gpuSamples.size()))return false;
        if(ConsumerSamples(nullptr,consumerCpu.size(),12,88,12,consumerRefs,1131,gpuSamples)||ConsumerSamples(consumerCpu.data(),consumerCpu.size(),~std::uint64_t{},88,12,consumerRefs,1131,gpuSamples)||ConsumerSamples(consumerCpu.data(),consumerCpu.size(),12,88,12,consumerRefs,1132,gpuSamples)||ConsumerSamples(consumerCpu.data(),consumerCpu.size(),100,88,16,consumerRefs,1131,gpuSamples)||ConsumerSamples(consumerCpu.data(),consumerCpu.size(),0,2049,12,consumerRefs,1131,gpuSamples)||!gpuSamples.empty())return false;
        auto excessRefs=consumerRefs;excessRefs.push_back(1131);if(ConsumerSamples(consumerCpu.data(),consumerCpu.size(),12,88,12,excessRefs,1131,gpuSamples)||ConsumerSamples(consumerCpu.data(),consumerCpu.size(),12,88,0,consumerRefs,1131,gpuSamples))return false;
        if(!ConsumerRange(32,20,12)||ConsumerRange(32,21,12)||ConsumerRange(32,~std::uint64_t{},12)||ConsumerRange(~std::uint64_t{},0,65537)||ConsumerRange(32,0,0))return false;
        std::size_t reportExtra=0;if(!ConsumerReportGrowth(32*1024*1024-4,100,104,reportExtra)||reportExtra!=4||ConsumerReportGrowth(32*1024*1024-3,100,104,reportExtra)||ConsumerReportGrowth(0,100,99,reportExtra)||ConsumerReportGrowth(0,0,65537,reportExtra))return false;
        std::uint32_t payloadFiles=470;if(!TakeDiagnosticPayload(payloadFiles,4*1024*1024)||payloadFiles!=471||TakeDiagnosticPayload(payloadFiles,1))return false;payloadFiles=0;if(TakeDiagnosticPayload(payloadFiles,4*1024*1024+1)||payloadFiles)return false;
        if(CaptureBytesAllowed(31*1024*1024,1,false)||!CaptureBytesAllowed(31*1024*1024,1,true)||CaptureBytesAllowed(32*1024*1024,1,true)||CaptureBytesAllowed(32*1024*1024+1,0,true)||CaptureBytesAllowed(0,4*1024*1024+1,false))return false;
        if(ParticleMipBytes({4,4,16},64,64,0,0)!=4096||ParticleMipBytes({4,4,16},10,7,0,0)!=96||ParticleMipBytes({1,1,8},1200,544,0,0)!=5222400||ParticleMipBytes({1,1,8},1200,544,0,31)||ParticleMipBytes({1,1,1},1,1,16*1024*1024-1,31)!=1||ParticleMipBytes({1,1,1},1,1,16*1024*1024,0)||ParticleMipBytes({1,1,4},8192,8192,0,0)||ParticleMipBytes({4,4,16},~0u,~0u,0,0)||ParticleMipBytes({0,4,16},4,4,0,0)||ParticleMipBytes({1,1,1},1,1,0,32))return false;
        if(!Range(64,16,48)||Range(64,-1,1)||Range(64,63,2)||!Range(0,0,0))return false;
        if(PresentationTransform(VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR|VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)!=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR||PresentationTransform(VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR|VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)!=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR||PresentationTransform(VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)!=0||PresentationTransform(0)!=0)return false;
        VkSurfaceCapabilitiesKHR caps{};caps.currentExtent={1200,540};caps.minImageExtent={100,100};caps.maxImageExtent={2400,1080};if(ExtentChanged({1200,540},RequestedExtent(caps,2400,1080))||!ExtentChanged({1200,540},{540,1200}))return false;caps.currentExtent={~0u,~0u};auto clampedExtent=RequestedExtent(caps,40,2400);if(clampedExtent.width!=100||clampedExtent.height!=1080)return false;
        if(Half(1)!=0x3c00||Half(-2)!=0xc000||Unhalf(0x3c00)!=1||Unhalf(1)!=std::ldexp(1.f,-24))return false;
        if(BaseDepth(GL_TEXTURE_2D,3,1)!=1||BaseDepth(GL_TEXTURE_CUBE_MAP_POSITIVE_X,5,1)!=1||BaseDepth(GL_TEXTURE_3D,2,8)!=32)return false;Image image;image.format=VK_FORMAT_R8G8B8A8_UNORM;image.width=301;image.height=149;image.levels=4;if(!MatchesMip(image,GL_TEXTURE_2D,1,image.format,150,74,1,0)||MatchesMip(image,GL_TEXTURE_2D,1,image.format,151,74,1,0))return false;
        if(FormatBlock(VK_FORMAT_ASTC_6x6_UNORM_BLOCK).x!=6||FormatBlock(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK).bytes!=8)return false;
        if(Format(GL_RG8_SNORM)!=VK_FORMAT_R8G8_SNORM||Format(GL_R8_SNORM)!=VK_FORMAT_R8_SNORM||Format(0x8F99)!=VK_FORMAT_R16G16_SNORM||Format(0x8F9B)!=VK_FORMAT_R16G16B16A16_SNORM||FormatBlock(VK_FORMAT_R8G8_SNORM).bytes!=2||FormatBlock(VK_FORMAT_R16G16_SNORM).bytes!=4||FormatBlock(VK_FORMAT_R16G16B16A16_SNORM).bytes!=8||PixelBytes(GL_RG,GL_BYTE)!=2||PixelBytes(GL_RG,GL_SHORT)!=4)return false;
        const std::array<std::int8_t,6> signed8{-128,-127,-64,0,64,127};std::array<std::uint8_t,10> snorm8;snorm8.fill(0xa5);
        for(std::size_t i=0;i<3;++i)StoreColor(snorm8.data()+2+i*2,VK_FORMAT_R8G8_SNORM,reinterpret_cast<const std::uint8_t*>(signed8.data()+i*2),GL_RG,GL_BYTE);
        if(snorm8[0]!=0xa5||snorm8[1]!=0xa5||snorm8[8]!=0xa5||snorm8[9]!=0xa5||std::memcmp(snorm8.data()+2,signed8.data(),6))return false;
        for(std::size_t i=0;i<3;++i){auto pixel=DecodeColor(snorm8.data()+2+i*2,VK_FORMAT_R8G8_SNORM);if(std::abs(pixel[0]-std::max(-1.f,signed8[i*2]/127.f))>1e-6f||std::abs(pixel[1]-std::max(-1.f,signed8[i*2+1]/127.f))>1e-6f||pixel[2]!=0||pixel[3]!=1)return false;}
        const std::array<std::int16_t,4> signed16{-32768,-16384,0,32767};std::array<std::uint8_t,12> snorm16;snorm16.fill(0x5a);
        StoreColor(snorm16.data()+2,VK_FORMAT_R16G16B16A16_SNORM,reinterpret_cast<const std::uint8_t*>(signed16.data()),GL_RGBA,GL_SHORT);const auto decoded16=DecodeColor(snorm16.data()+2,VK_FORMAT_R16G16B16A16_SNORM);
        if(snorm16[0]!=0x5a||snorm16[1]!=0x5a||snorm16[10]!=0x5a||snorm16[11]!=0x5a||std::memcmp(snorm16.data()+2,signed16.data(),8)||decoded16[0]!=-1||std::abs(decoded16[1]+16384.f/32767)>1e-6f||decoded16[2]!=0||decoded16[3]!=1)return false;
        EncodeColor(snorm8.data()+2,VK_FORMAT_R8G8_SNORM,{-2,2,0,1});const std::array<std::int8_t,2> clamped{-127,127};if(std::memcmp(snorm8.data()+2,clamped.data(),2))return false;
        StoreColor(snorm8.data()+2,VK_FORMAT_R8G8B8A8_SNORM,reinterpret_cast<const std::uint8_t*>(signed8.data()),GL_RGB,GL_BYTE);if(std::memcmp(snorm8.data()+2,signed8.data(),3)||snorm8[5]!=127||snorm8[6]!=64||snorm8[7]!=127)return false;
        Program one,two;one.shader=std::make_shared<Shader>();one.shader->stage.fields={{1,bl2::vk::FieldType::Vec4,2,0},{4,bl2::vk::FieldType::Bool,2,32}};two.shader=one.shader;one.groups[1]={2,32};one.groups[4]={8,32};two.groups[1]={4,16};two.groups[4]={9,16};one.locals.resize(40);two.locals.resize(40);
        for(std::size_t i=0;i<one.locals.size();++i){one.locals[i]=100+i;two.locals[i]=200+i;}one.locals[32]=0;one.locals[33]=1;one.locals[36]=5;
        std::array<std::uint32_t,16> a{},b{};Parameters(one,a.data(),64);Parameters(two,b.data(),64);
        if(a[0]!=108||a[4]!=112||a[8]!=0||a[12]!=1||b[0]!=216||b[4]!=0||b[8]!=1||b[12]!=0)return false;
        Context bindings;bindings.share=std::make_shared<ShareGroup>();auto prior=std::make_shared<Texture>(),next=std::make_shared<Texture>();prior->target=next->target=GL_TEXTURE_2D;bindings.share->textures[9]=prior;bindings.share->textures[11]=next;bindings.share->samplers[3]=Sampling{};bindings.share->samplers[5]=Sampling{};bindings.textureUnits[1][GL_TEXTURE_2D]={9,prior};bindings.samplerUnits[1]=3;
        Program textureProgram;textureProgram.shader=std::make_shared<Shader>();textureProgram.shader->stage.samplers={{1,19,bl2::vk::SamplerType::Sampler2D}};textureProgram.samplerRegisters[1]=7;textureProgram.locals.resize(32);ApplyLegacyBindings(bindings,textureProgram);if(bindings.textureUnits[1][GL_TEXTURE_2D].first!=9||bindings.samplerUnits[1]!=3)return false;
        textureProgram.locals[28]=11;textureProgram.locals[29]=5;ApplyLegacyBindings(bindings,textureProgram);if(bindings.textureUnits[1][GL_TEXTURE_2D].first!=11||bindings.textureUnits[1][GL_TEXTURE_2D].second!=next||bindings.samplerUnits[1]!=5)return false;
        textureProgram.locals[28]=textureProgram.locals[29]=0;ApplyLegacyBindings(bindings,textureProgram);if(bindings.textureUnits[1][GL_TEXTURE_2D].first!=11||bindings.samplerUnits[1]!=5)return false;textureProgram.locals.resize(29);textureProgram.locals[28]=9;ApplyLegacyBindings(bindings,textureProgram);if(bindings.textureUnits[1][GL_TEXTURE_2D].first!=11||bindings.samplerUnits[1]!=5)return false;
        next->image=std::make_shared<Image>();next->image->aspect=VK_IMAGE_ASPECT_COLOR_BIT;
        if(HasShadowWithoutDepth(bindings,textureProgram))return false;
        textureProgram.shader->stage.samplers[0].type=bl2::vk::SamplerType::Sampler2DShadow;
        next->image->aspect=VK_IMAGE_ASPECT_DEPTH_BIT;if(HasShadowWithoutDepth(bindings,textureProgram))return false;
        next->image->aspect=VK_IMAGE_ASPECT_COLOR_BIT;if(!HasShadowWithoutDepth(bindings,textureProgram))return false;
        next->image.reset();if(HasShadowWithoutDepth(bindings,textureProgram))return false;
        bindings.textureUnits[1].erase(GL_TEXTURE_2D);if(HasShadowWithoutDepth(bindings,textureProgram))return false;
        Program inactive;inactive.shader=one.shader;inactive.groups.fill({-1,0xffff});inactive.locals.assign(40,~0u);std::array<std::uint32_t,16> ignored{};Parameters(inactive,ignored.data(),64);if(std::any_of(ignored.begin(),ignored.end(),[](auto value){return value!=0;}))return false;
        std::array<std::uint8_t,18> raw{};std::array<float,4> expected{-.5f,.25f,1,2};std::memcpy(raw.data()+2,expected.data(),16);Attribute attr;attr.type=GL_FLOAT;attr.size=4;if(AttributeValue(attr,raw.data()+2)!=expected)return false;
        if(!ValidConstantGroup({-1,0xffff})||!ValidConstantGroup({4095,16})||ValidConstantGroup({4095,32})||ValidConstantGroup({1,0xffff})||ValidConstantGroup({-2,0}))return false;
        if(!BoxRange(32,24,8)||BoxRange(32,24,9)||BoxRange(32,-1,1)||!BoxRange(0,0,0))return false;
        Image volume;volume.width=16;volume.height=8;volume.depth=4;volume.levels=3;volume.type=VK_IMAGE_TYPE_3D;volume.aspect=VK_IMAGE_ASPECT_COLOR_BIT;auto box3d=CopyBox(volume,1,1,0,1,2,2,1);if(box3d.imageOffset.z!=1||box3d.imageSubresource.baseArrayLayer||box3d.imageSubresource.layerCount!=1)return false;
        Image layers;layers.width=16;layers.height=8;layers.layers=6;layers.levels=3;layers.aspect=VK_IMAGE_ASPECT_COLOR_BIT;auto box2d=CopyBox(layers,1,1,0,2,2,2,3);if(box2d.imageOffset.z||box2d.imageSubresource.baseArrayLayer!=2||box2d.imageSubresource.layerCount!=3||box2d.imageExtent.depth!=1)return false;
        if(BoxesOverlap(0,0,0,8,0,0,8,8,1)||!BoxesOverlap(0,0,0,7,0,0,8,8,1))return false;
        VkPhysicalDeviceProperties properties{};properties.vendorID=0x5143;properties.deviceID=0x1234;for(std::size_t i=0;i<16;++i)properties.pipelineCacheUUID[i]=i;
        std::vector<std::uint8_t> cache(48+40);std::memcpy(cache.data(),"BL2VKC01",8);const std::uint64_t payloadBytes=40;std::memcpy(cache.data()+8,&payloadBytes,8);const std::uint32_t headerBytes=32,version=VK_PIPELINE_CACHE_HEADER_VERSION_ONE;std::memcpy(cache.data()+48,&headerBytes,4);std::memcpy(cache.data()+52,&version,4);std::memcpy(cache.data()+56,&properties.vendorID,4);std::memcpy(cache.data()+60,&properties.deviceID,4);std::memcpy(cache.data()+64,properties.pipelineCacheUUID,16);auto digest=Hash(cache.data()+48,40);std::memcpy(cache.data()+16,digest.data(),32);std::uint64_t length=0;
        if(ValidateDriverCache(cache,properties,length)!=DriverCacheError::None||length!=40)return false;
        auto invalid=cache;invalid.back()^=1;if(ValidateDriverCache(invalid,properties,length)!=DriverCacheError::Envelope)return false;invalid=cache;invalid.pop_back();if(ValidateDriverCache(invalid,properties,length)!=DriverCacheError::Envelope)return false;invalid=cache;invalid.push_back(0);if(ValidateDriverCache(invalid,properties,length)!=DriverCacheError::Envelope)return false;
        auto other=properties;other.pipelineCacheUUID[0]^=1;if(ValidateDriverCache(cache,other,length)!=DriverCacheError::Identity)return false;other=properties;other.vendorID^=1;if(ValidateDriverCache(cache,other,length)!=DriverCacheError::Identity)return false;other=properties;other.deviceID^=1;if(ValidateDriverCache(cache,other,length)!=DriverCacheError::Identity)return false;
        Recipe r{};const auto checksum=Hash(&r,sizeof(r));r.state[1]=1;if(Hash(&r,sizeof(r))==checksum)return false;
        return true;
    }catch(...){return false;}
}
}
