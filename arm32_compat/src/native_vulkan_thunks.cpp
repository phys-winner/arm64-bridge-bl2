#include "native_vulkan_thunks.h"
#include "native_vulkan_renderer.h"
#include "native_perf.h"
#include "native_jni.h"
#include "native_host.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include "perf_probe.h"
#include "bl2_graphics_settings.h"
#include "bl2_native_resolution.h"
#include "obb_pair_selection.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
static_assert(sizeof(void*) == 4, "The graphics facade targets native ARM32");
using Jit = Dynarmic::A32::Jit;
using Host = DynarmicHost;
constexpr GLenum kVertexProgram = 0x8620, kFragmentProgram = 0x8804;
constexpr GLenum kProgramAscii = 0x8875;
constexpr std::size_t kMaxShaderSource = 1024 * 1024;
constexpr std::uint32_t kDisplay = 0xB1200001, kConfig = 0xB1200002;
thread_local EGLint eglError = EGL_SUCCESS;
thread_local EGLenum eglApi = EGL_OPENGL_ES_API;
thread_local std::uint32_t currentContext = 0, currentDraw = 0, currentRead = 0;

void GlError(GLenum value) { bl2vk::RecordError(value); }
EGLBoolean EglError(EGLint value) { eglError = value; return EGL_FALSE; }
bool Current() { if (currentContext) return true; GlError(GL_INVALID_OPERATION); return false; }

template<class T> T* Span(Host& host, std::uint32_t address, std::size_t count,
                        bool optional = false) {
    if (!address && (optional || !count)) return nullptr;
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T) ||
        !address || !GuestRangeValid(host.GetGuestMemorySize(), address, count * sizeof(T))) {
        GlError(GL_INVALID_VALUE); return nullptr;
    }
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address));
}
template<class T> T* EglSpan(Host& host, std::uint32_t address, std::size_t count,
                           bool optional = false) {
    if (!address && optional) return nullptr;
    if (!address || count > std::numeric_limits<std::size_t>::max() / sizeof(T) ||
        !GuestRangeValid(host.GetGuestMemorySize(), address, count * sizeof(T))) {
        EglError(EGL_BAD_PARAMETER); return nullptr;
    }
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address));
}

template<class T> T ReadArg(Arm32CallArgs& args) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(static_cast<std::uintptr_t>(args.Ptr()));
    else if constexpr (std::is_same_v<T, float>) return args.F32();
    else if constexpr (std::is_same_v<T, double>) return args.F64();
    else if constexpr (sizeof(T) == 8) return static_cast<T>(args.U64());
    else return static_cast<T>(args.U32());
}
template<class T> void Return(Jit& jit, T value) {
    if constexpr (std::is_pointer_v<T>) jit.Regs()[0] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(value));
    else if constexpr (sizeof(T) == 8) {
        jit.Regs()[0] = static_cast<std::uint32_t>(value);
        jit.Regs()[1] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> 32);
    } else jit.Regs()[0] = static_cast<std::uint32_t>(value);
}
template<class R, class... A> void Register(ThunkManager& manager, const char* name, R (*function)(A...)) {
    manager.RegisterThunk(name, [function](Jit& jit, Host& host) {
        if (!Current()) { if constexpr (!std::is_void_v<R>) Return(jit, R{}); return; }
        Arm32CallArgs args(jit, host);
        // Braced tuple initialization preserves AAPCS argument order, including stack doubles.
        auto values = std::tuple<A...>{ReadArg<A>(args)...};
        if constexpr (std::is_void_v<R>) std::apply(function, values);
        else Return(jit, std::apply(function, values));
    });
}
void Names(ThunkManager& manager, const char* name, void (*function)(GLsizei, GLuint*)) {
    manager.RegisterThunk(name, [function](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto count = static_cast<GLsizei>(jit.Regs()[0]);
        if (count < 0) { GlError(GL_INVALID_VALUE); return; }
        auto* names = Span<GLuint>(host, jit.Regs()[1], static_cast<std::size_t>(count));
        if (!count || names) function(count, names);
    });
}
void Names(ThunkManager& manager, const char* name, void (*function)(GLsizei, const GLuint*)) {
    manager.RegisterThunk(name, [function](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto count = static_cast<GLsizei>(jit.Regs()[0]);
        if (count < 0) { GlError(GL_INVALID_VALUE); return; }
        const auto* names = Span<GLuint>(host, jit.Regs()[1], static_cast<std::size_t>(count));
        if (!count || names) function(count, names);
    });
}

std::size_t QueryWidth(GLenum name) {
    switch (name) {
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_CLEAR_VALUE:
    case GL_COLOR_WRITEMASK: case GL_BLEND_COLOR: return 4;
    case GL_MAX_VIEWPORT_DIMS: case GL_DEPTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE:
    case GL_ALIASED_LINE_WIDTH_RANGE: return 2;
    case GL_COMPRESSED_TEXTURE_FORMATS: {
        GLint count = 0; bl2vk::GetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    }
    default: return 1;
    }
}

bool RecoverSource(std::span<const std::uint8_t> memory, std::size_t nv,
                   std::size_t length, std::string_view& source) {
    if (nv < 2 || length == 0 || length > kMaxShaderSource || nv >= memory.size() ||
        length > memory.size() - nv || memory[nv - 1] != 0) return false;
    const auto lower = nv > kMaxShaderSource ? nv - kMaxShaderSource : 0;
    auto begin = nv - 1;
    while (begin > lower && memory[begin - 1] != 0) --begin;
    if (begin == nv - 1) return false;
    const std::string_view recovered(reinterpret_cast<const char*>(memory.data() + begin), nv - 1 - begin);
    const auto marker = recovered.rfind("//ShaderFileName:");
    if (marker == std::string_view::npos) return false;
    source = recovered.substr(marker);
    return source.find("void main") != std::string_view::npos;
}

bool ConstantGroupValid(const bl2vk::ConstantGroup& group) {
    if (group.first_register < -1) return false;
    // Original unused banks retain {-1, 0xFFFF}; only active banks have byte ranges.
    return group.first_register == -1 ||
        (group.byte_count % 16 == 0 &&
         static_cast<std::uint32_t>(group.first_register) + group.byte_count / 16 <= 4096);
}

bool PixelExtent(GLsizei width, GLsizei height, GLsizei depth, std::size_t pixel,
                 GLint alignment, GLint rowLength, GLint imageHeight,
                 GLint skipPixels, GLint skipRows, GLint skipImages, std::size_t& result) {
    if (width < 0 || height < 0 || depth < 0 || !pixel ||
        (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) ||
        rowLength < 0 || imageHeight < 0 || skipPixels < 0 || skipRows < 0 || skipImages < 0) return false;
    if (!width || !height || !depth) { result = 0; return true; }
    const std::uint64_t row = static_cast<std::uint64_t>(rowLength ? rowLength : width) * pixel;
    const std::uint64_t stride = (row + alignment - 1) & ~static_cast<std::uint64_t>(alignment - 1);
    const std::uint64_t rows = imageHeight ? imageHeight : height;
    const std::uint64_t images = static_cast<std::uint64_t>(skipImages) + depth - 1;
    if (stride > UINT32_MAX || rows > UINT32_MAX ||
        images > UINT32_MAX || images * rows > UINT32_MAX) return false;
    const std::uint64_t rowCount = images * rows + static_cast<std::uint64_t>(skipRows) + height - 1;
    if (rowCount > UINT32_MAX || (stride && rowCount > UINT32_MAX / stride)) return false;
    const std::uint64_t bytes = rowCount * stride +
        (static_cast<std::uint64_t>(skipPixels) + width) * pixel;
    if (bytes > UINT32_MAX) return false;
    result = static_cast<std::size_t>(bytes); return true;
}

std::size_t PixelSize(GLenum format, GLenum type) {
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8: return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: break;
    }
    std::size_t components = 0, bytes = 0;
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: case GL_RED: case GL_RED_INTEGER: case GL_DEPTH_COMPONENT: components = 1; break;
    case GL_LUMINANCE_ALPHA: case GL_RG: case GL_RG_INTEGER: components = 2; break;
    case GL_RGB: case GL_RGB_INTEGER: components = 3; break;
    case GL_RGBA: case GL_RGBA_INTEGER: case 0x80E1: components = 4; break;
    default: return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: bytes = 1; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case 0x8D61: bytes = 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: bytes = 4; break;
    default: return 0;
    }
    return components * bytes;
}
bool Pixels(Host& host, std::uint32_t address, GLsizei width, GLsizei height,
            GLsizei depth, GLenum format, GLenum type, bool pack, bool optional,
            bool volume = false) {
    GLint pbo = 0;
    bl2vk::GetIntegerv(pack ? GL_PIXEL_PACK_BUFFER_BINDING : GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    GLint alignment = 0, row = 0, image = 0, sx = 0, sy = 0, sz = 0;
    bl2vk::GetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &alignment);
    bl2vk::GetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &row);
    bl2vk::GetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &sx);
    bl2vk::GetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &sy);
    if (!pack && volume) {
        bl2vk::GetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image);
        bl2vk::GetIntegerv(GL_UNPACK_SKIP_IMAGES, &sz);
    }
    std::size_t size = 0;
    const auto pixel = PixelSize(format, type);
    if (!pixel) { GlError(GL_INVALID_ENUM); return false; }
    if (!PixelExtent(width, height, depth, pixel, alignment, row, image, sx, sy, sz, size)) {
        GlError(GL_INVALID_VALUE); return false;
    }
    if (pbo) {
        GLint capacity = 0;
        bl2vk::GetBufferParameteriv(pack ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &capacity);
        if (capacity < 0 || address > static_cast<std::uint32_t>(capacity) ||
            size > static_cast<std::size_t>(capacity) - address) { GlError(GL_INVALID_OPERATION); return false; }
        return true;
    }
    return size == 0 || (!address && optional) || Span<std::uint8_t>(host, address, size) != nullptr;
}
bool CompressedPixels(Host& host, std::uint32_t address, GLsizei count, bool optional) {
    if (count < 0) { GlError(GL_INVALID_VALUE); return false; }
    GLint pbo = 0; bl2vk::GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (pbo) {
        GLint capacity = 0; bl2vk::GetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &capacity);
        if (capacity < 0 || address > static_cast<std::uint32_t>(capacity) ||
            static_cast<std::size_t>(count) > static_cast<std::size_t>(capacity) - address) {
            GlError(GL_INVALID_OPERATION); return false;
        }
        return true;
    }
    return !count || (!address && optional) || Span<std::uint8_t>(host, address, count) != nullptr;
}

struct Context {
    std::uintptr_t native = 0, owner = 0;
    std::uint32_t group = 0;
    EGLenum api = EGL_OPENGL_ES_API;
    bool deleted = false;
    EGLint requestedMajor = 1;
};
struct Surface {
    ANativeWindow* window = nullptr;
    EGLint width = 0, height = 0;
    std::uintptr_t owner = 0;
    bool deleted = false;
    EGLBoolean largest = EGL_FALSE;
};
auto FindWindowSurface(const std::unordered_map<std::uint32_t, Surface>& state,
                       ANativeWindow* window) {
    return std::find_if(state.begin(), state.end(), [window](const auto& entry) {
        return entry.second.window == window && !entry.second.deleted;
    });
}
template<class RetireWindow, class RetirePbuffer>
void RetireSurfaceState(std::unordered_map<std::uint32_t, Surface>& state,
                        std::uint32_t id, RetireWindow retireWindow,
                        RetirePbuffer retirePbuffer) {
    const auto found = state.find(id);
    if (found == state.end()) return;
    auto* window = found->second.window;
    if (window) {
        // A replacement handle may already own this window when the old one retires.
        const bool lastReference = std::none_of(state.begin(), state.end(), [id, window](const auto& entry) {
            return entry.first != id && entry.second.window == window;
        });
        retireWindow(window, lastReference);
    } else retirePbuffer(id);
    state.erase(found);
}
std::mutex eglMutex;
std::unordered_map<std::uint32_t, Context> contexts;
std::unordered_map<std::uint32_t, Surface> surfaces;
std::uint32_t nextContext = 0xB1300000, nextSurface = 0xB1400000;
bool displayInitialized = false;
std::mutex handleMutex;
std::unordered_map<std::uint32_t, std::unordered_map<GLuint64, bool>> handles;
thread_local std::uint32_t currentGroup = 0;
bool Display(std::uint32_t display) { return display == kDisplay || EglError(EGL_BAD_DISPLAY); }
bool Initialized(std::uint32_t display) {
    return Display(display) && (displayInitialized || EglError(EGL_NOT_INITIALIZED));
}

const EGLint* Attributes(Host& host, std::uint32_t address) {
    if (!address) return nullptr;
    for (std::size_t pair = 0; pair < 128; ++pair) {
        const std::uint64_t offset = static_cast<std::uint64_t>(address) + pair * 8;
        if (offset > UINT32_MAX || !GuestRangeValid(host.GetGuestMemorySize(), offset, 4)) break;
        const auto* item = reinterpret_cast<const EGLint*>(static_cast<std::uintptr_t>(offset));
        if (*item == EGL_NONE) return reinterpret_cast<const EGLint*>(static_cast<std::uintptr_t>(address));
        if (!GuestRangeValid(host.GetGuestMemorySize(), offset, 8)) break;
    }
    EglError(EGL_BAD_ATTRIBUTE); return nullptr;
}
bool AttributeList(Host& host, std::uint32_t address, const EGLint*& output) {
    output = Attributes(host, address); return !address || output;
}
EGLint ConfigValue(EGLint attribute, bool& valid) {
    valid = true;
    switch (attribute) {
    case EGL_CONFIG_ID: return 1;
    case EGL_BUFFER_SIZE: return 32;
    case EGL_COLOR_BUFFER_TYPE: return EGL_RGB_BUFFER;
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: return 8;
    case EGL_DEPTH_SIZE: return 24;
    case EGL_STENCIL_SIZE: return 8;
    case EGL_SURFACE_TYPE: return EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: return EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
    case EGL_CONFIG_CAVEAT: case EGL_TRANSPARENT_TYPE: return EGL_NONE;
    case EGL_NATIVE_VISUAL_ID: return WINDOW_FORMAT_RGBA_8888;
    case EGL_NATIVE_VISUAL_TYPE: return 0;
    case EGL_NATIVE_RENDERABLE: return EGL_FALSE;
    case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: case EGL_LEVEL: case EGL_ALPHA_MASK_SIZE: return 0;
    case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: {
        GLint value = 0; bl2vk::GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &value); return value;
    }
    case EGL_MAX_PBUFFER_PIXELS: {
        GLint value = 0; bl2vk::GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &value);
        return static_cast<EGLint>(std::min<std::int64_t>(INT32_MAX, static_cast<std::int64_t>(value) * value));
    }
    case EGL_MIN_SWAP_INTERVAL: return 0;
    case EGL_MAX_SWAP_INTERVAL: return 1;
    case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA: return EGL_FALSE;
    default: valid = false; return 0;
    }
}
bool ConfigMatches(const EGLint* attributes, bool& match) {
    match = true;
    if (!attributes) return true;
    for (std::size_t pair = 0; attributes[pair * 2] != EGL_NONE; ++pair) {
        const auto attribute = attributes[pair * 2], expected = attributes[pair * 2 + 1];
        bool valid = false; const auto actual = ConfigValue(attribute, valid);
        if (!valid) { EglError(EGL_BAD_ATTRIBUTE); return false; }
        if (expected == EGL_DONT_CARE) continue;
        switch (attribute) {
        case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE:
        case EGL_DEPTH_SIZE: case EGL_STENCIL_SIZE: case EGL_BUFFER_SIZE:
        case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: case EGL_ALPHA_MASK_SIZE:
            if (expected < 0) { EglError(EGL_BAD_ATTRIBUTE); return false; }
            match = match && actual >= expected; break;
        case EGL_SURFACE_TYPE: case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT:
            match = match && (actual & expected) == expected; break;
        default: match = match && actual == expected; break;
        }
    }
    return true;
}
void RetireSurface(std::uint32_t id) {
    RetireSurfaceState(surfaces, id, [](ANativeWindow* window, bool lastReference) {
        if (lastReference) bl2vk::ReleaseWindow(window);
        ANativeWindow_release(window);
    }, bl2vk::ReleasePbuffer);
}
void ReleaseCurrentLocked() {
    if (const auto found = contexts.find(currentContext); found != contexts.end()) {
        found->second.owner = 0;
        if (found->second.deleted) { bl2vk::DestroyContext(found->second.native); contexts.erase(found); }
    }
    for (const auto id : {currentDraw, currentRead}) {
        if (const auto found = surfaces.find(id); found != surfaces.end()) {
            found->second.owner = 0;
            if (found->second.deleted) RetireSurface(id);
        }
    }
    currentContext = currentDraw = currentRead = currentGroup = 0;
}
struct ThreadRelease {
    ~ThreadRelease() {
        if (!currentContext) return;
        std::lock_guard lock(eglMutex);
        bl2vk::MakeCurrent(0, nullptr);
        ReleaseCurrentLocked();
    }
};
void EnsureThreadRelease() { thread_local ThreadRelease release; (void)release; }

void RegisterEgl(ThunkManager& manager) {
    manager.RegisterThunk("eglGetDisplay", [](Jit& jit, Host&) {
        jit.Regs()[0] = jit.Regs()[0] == 0 ? kDisplay : EglError(EGL_BAD_PARAMETER);
    });
    manager.RegisterThunk("eglInitialize", [](Jit& jit, Host& host) {
        std::lock_guard lock(eglMutex);
        if (!Display(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        if (!bl2vk::IsInitialized()) { jit.Regs()[0] = EglError(EGL_NOT_INITIALIZED); return; }
        auto* major = EglSpan<EGLint>(host, jit.Regs()[1], 1, true);
        auto* minor = EglSpan<EGLint>(host, jit.Regs()[2], 1, true);
        if ((jit.Regs()[1] && !major) || (jit.Regs()[2] && !minor)) { jit.Regs()[0] = EglError(EGL_BAD_PARAMETER); return; }
        if (major) *major = 1; if (minor) *minor = 5;
        displayInitialized = true; jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglBindAPI", [](Jit& jit, Host&) {
        if (jit.Regs()[0] != EGL_OPENGL_API && jit.Regs()[0] != EGL_OPENGL_ES_API) { jit.Regs()[0] = EglError(EGL_BAD_PARAMETER); return; }
        eglApi = jit.Regs()[0]; jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglQueryAPI", [](Jit& jit, Host&) { jit.Regs()[0] = eglApi; });
    auto configurations = [](Jit& jit, Host& host, bool choose) {
        std::lock_guard lock(eglMutex);
        Arm32CallArgs args(jit, host); const auto display = args.U32();
        const auto attributesAddress = choose ? args.Ptr() : 0;
        const auto configAddress = args.Ptr(); const auto count = args.I32(); const auto numberAddress = args.Ptr();
        if (!Initialized(display)) { jit.Regs()[0] = EGL_FALSE; return; }
        const EGLint* attributes = nullptr; bool match = true;
        if (!AttributeList(host, attributesAddress, attributes) || !ConfigMatches(attributes, match)) { jit.Regs()[0] = EGL_FALSE; return; }
        auto* number = EglSpan<EGLint>(host, numberAddress, 1);
        if (!number || count < 0) { jit.Regs()[0] = EglError(EGL_BAD_PARAMETER); return; }
        if (configAddress && count && !EglSpan<std::uint32_t>(host, configAddress, count)) { jit.Regs()[0] = EglError(EGL_BAD_PARAMETER); return; }
        *number = match && (!configAddress || count > 0) ? 1 : 0;
        if (configAddress && count && match) *reinterpret_cast<std::uint32_t*>(configAddress) = kConfig;
        jit.Regs()[0] = EGL_TRUE;
    };
    manager.RegisterThunk("eglChooseConfig", [configurations](Jit& jit, Host& host) { configurations(jit, host, true); });
    manager.RegisterThunk("eglGetConfigs", [configurations](Jit& jit, Host& host) { configurations(jit, host, false); });
    manager.RegisterThunk("eglGetConfigAttrib", [](Jit& jit, Host& host) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        if (jit.Regs()[1] != kConfig) { jit.Regs()[0] = EglError(EGL_BAD_CONFIG); return; }
        bool valid = false; const auto value = ConfigValue(jit.Regs()[2], valid);
        auto* output = EglSpan<EGLint>(host, jit.Regs()[3], 1);
        if (!valid || !output) { jit.Regs()[0] = EglError(valid ? EGL_BAD_PARAMETER : EGL_BAD_ATTRIBUTE); return; }
        *output = value; jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglCreateContext", [](Jit& jit, Host& host) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = 0; return; }
        if (jit.Regs()[1] != kConfig) { jit.Regs()[0] = EglError(EGL_BAD_CONFIG); return; }
        const auto shareId = jit.Regs()[2]; auto share = contexts.find(shareId);
        if (shareId && (share == contexts.end() || share->second.deleted || share->second.api != eglApi)) { jit.Regs()[0] = EglError(EGL_BAD_CONTEXT); return; }
        const EGLint* attributes = nullptr;
        if (!AttributeList(host, jit.Regs()[3], attributes)) { jit.Regs()[0] = 0; return; }
        EGLint major = 1, minor = 0, flags = 0, profile = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
        if (attributes) for (std::size_t pair = 0; attributes[pair * 2] != EGL_NONE; ++pair) {
            const auto attribute = attributes[pair * 2], value = attributes[pair * 2 + 1];
            if (attribute == EGL_CONTEXT_MAJOR_VERSION) major = value;
            else if (attribute == EGL_CONTEXT_MINOR_VERSION) minor = value;
            else if (attribute == EGL_CONTEXT_OPENGL_PROFILE_MASK) {
                if (eglApi != EGL_OPENGL_API) { jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE); return; }
                profile = value;
            } else if (attribute == EGL_CONTEXT_FLAGS_KHR) flags = value;
            else { jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE); return; }
        }
        constexpr EGLint knownFlags = EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR |
            EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR | EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR;
        if (flags & ~knownFlags) { jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE); return; }
        if (flags & (EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR | EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)) {
            jit.Regs()[0] = EglError(eglApi == EGL_OPENGL_API ? EGL_BAD_MATCH : EGL_BAD_ATTRIBUTE); return;
        }
        const bool validVersion = eglApi == EGL_OPENGL_API ?
            ((major == 1 && minor >= 0 && minor <= 5) || (major == 2 && minor >= 0 && minor <= 1) ||
             (major == 3 && minor >= 0 && minor <= 3) || (major == 4 && minor >= 0 && minor <= 5)) :
            ((major == 2 && minor == 0) || (major == 3 && minor >= 0 && minor <= 2));
        if (!validVersion) { jit.Regs()[0] = EglError(EGL_BAD_MATCH); return; }
        if (eglApi == EGL_OPENGL_API && (major > 3 || (major == 3 && minor >= 2)) &&
            profile != EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR &&
            profile != EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR) {
            jit.Regs()[0] = EglError(EGL_BAD_MATCH); return;
        }
        const auto native = bl2vk::CreateContext(shareId ? share->second.native : 0);
        if (!native) { jit.Regs()[0] = EglError(EGL_BAD_ALLOC); return; }
        const auto id = nextContext++;
        contexts.emplace(id, Context{native, 0, shareId ? share->second.group : id, eglApi, false, major});
        jit.Regs()[0] = id;
    });
    manager.RegisterThunk("eglCreateWindowSurface", [](Jit& jit, Host& host) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = 0; return; }
        if (jit.Regs()[1] != kConfig) { jit.Regs()[0] = EglError(EGL_BAD_CONFIG); return; }
        auto* window = reinterpret_cast<ANativeWindow*>(jit.Regs()[2]);
        if (!window) { jit.Regs()[0] = EglError(EGL_BAD_NATIVE_WINDOW); return; }
        const EGLint* attributes = nullptr;
        if (!AttributeList(host, jit.Regs()[3], attributes)) { jit.Regs()[0] = 0; return; }
        if (attributes) for (std::size_t pair = 0; attributes[pair * 2] != EGL_NONE; ++pair) {
            if (attributes[pair * 2] != EGL_RENDER_BUFFER || attributes[pair * 2 + 1] != EGL_BACK_BUFFER) {
                jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE); return;
            }
        }
        if (const auto prior = FindWindowSurface(surfaces, window); prior != surfaces.end()) {
            // The engine repeats this request; preserve NativePlatform's surface reuse.
            jit.Regs()[0] = prior->first;
            return;
        }
        ANativeWindow_acquire(window);
        const auto id = nextSurface++;
        surfaces.emplace(id, Surface{window, ANativeWindow_getWidth(window), ANativeWindow_getHeight(window)});
        jit.Regs()[0] = id;
    });
    manager.RegisterThunk("eglCreatePbufferSurface", [](Jit& jit, Host& host) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = 0; return; }
        if (jit.Regs()[1] != kConfig) { jit.Regs()[0] = EglError(EGL_BAD_CONFIG); return; }
        const EGLint* attributes = nullptr;
        if (!AttributeList(host, jit.Regs()[2], attributes)) { jit.Regs()[0] = 0; return; }
        Surface surface;
        if (attributes) for (std::size_t pair = 0; attributes[pair * 2] != EGL_NONE; ++pair) {
            const auto attribute = attributes[pair * 2], value = attributes[pair * 2 + 1];
            if (attribute == EGL_WIDTH) surface.width = value;
            else if (attribute == EGL_HEIGHT) surface.height = value;
            else if (attribute == EGL_LARGEST_PBUFFER && (value == EGL_FALSE || value == EGL_TRUE)) surface.largest = value;
            else if ((attribute == EGL_TEXTURE_FORMAT || attribute == EGL_TEXTURE_TARGET) && value == EGL_NO_TEXTURE) {}
            else if (attribute == EGL_MIPMAP_TEXTURE && value == EGL_FALSE) {}
            else { jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE); return; }
        }
        if (surface.width < 0 || surface.height < 0) { jit.Regs()[0] = EglError(EGL_BAD_PARAMETER); return; }
        GLint maximum = 0; bl2vk::GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maximum);
        if (maximum <= 0) { jit.Regs()[0] = EglError(EGL_BAD_ALLOC); return; }
        if (surface.width > maximum || surface.height > maximum) {
            if (!surface.largest) { jit.Regs()[0] = EglError(EGL_BAD_ALLOC); return; }
            surface.width = std::min(surface.width, maximum); surface.height = std::min(surface.height, maximum);
        }
        const auto id = nextSurface++; surfaces.emplace(id, surface); jit.Regs()[0] = id;
    });
    manager.RegisterThunk("eglMakeCurrent", [](Jit& jit, Host& host) {
        std::unique_lock lock(eglMutex);
        const auto draw = jit.Regs()[1], read = jit.Regs()[2], contextId = jit.Regs()[3];
        if (!Display(jit.Regs()[0]) || (contextId && !Initialized(jit.Regs()[0]))) { jit.Regs()[0] = EGL_FALSE; return; }
        if (!contextId) {
            if (draw || read) { jit.Regs()[0] = EglError(EGL_BAD_MATCH); return; }
            if (!bl2vk::MakeCurrent(0, nullptr)) { jit.Regs()[0] = EglError(EGL_BAD_ACCESS); return; }
            ReleaseCurrentLocked(); jit.Regs()[0] = EGL_TRUE; return;
        }
        auto context = contexts.find(contextId);
        if (context == contexts.end() || context->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_CONTEXT); return; }
        const auto owner = Jit::NativeCoreIdentity();
        if (context->second.owner && context->second.owner != owner) { jit.Regs()[0] = EglError(EGL_BAD_ACCESS); return; }
        if (draw != read || !draw) { jit.Regs()[0] = EglError(EGL_BAD_MATCH); return; }
        auto surface = surfaces.find(draw);
        if (surface == surfaces.end() || surface->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_SURFACE); return; }
        if (surface->second.owner && surface->second.owner != owner) { jit.Regs()[0] = EglError(EGL_BAD_ACCESS); return; }
        if (!bl2vk::MakeCurrent(context->second.native, surface->second.window,
                               surface->second.width, surface->second.height,
                               surface->second.window ? 0 : draw)) { jit.Regs()[0] = EglError(EGL_BAD_ACCESS); return; }
        ReleaseCurrentLocked();
        currentContext = contextId; currentDraw = draw; currentRead = read; currentGroup = context->second.group;
        context->second.owner = owner; surface->second.owner = owner;
        EnsureThreadRelease();
        jit.Regs()[0] = EGL_TRUE;
        lock.unlock();
        CheckBl2NativeResolutionLimits(host);
    });
    manager.RegisterThunk("eglDestroyContext", [](Jit& jit, Host&) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        auto found = contexts.find(jit.Regs()[1]);
        if (found == contexts.end() || found->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_CONTEXT); return; }
        if (found->second.owner) found->second.deleted = true;
        else { if (!bl2vk::DestroyContext(found->second.native)) { jit.Regs()[0] = EglError(EGL_BAD_ACCESS); return; } contexts.erase(found); }
        jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglDestroySurface", [](Jit& jit, Host&) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        auto found = surfaces.find(jit.Regs()[1]);
        if (found == surfaces.end() || found->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_SURFACE); return; }
        if (found->second.owner) found->second.deleted = true; else RetireSurface(found->first);
        jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglQuerySurface", [](Jit& jit, Host& host) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        const auto found = surfaces.find(jit.Regs()[1]);
        if (found == surfaces.end() || found->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_SURFACE); return; }
        auto* output = EglSpan<EGLint>(host, jit.Regs()[3], 1);
        if (!output) { jit.Regs()[0] = EglError(EGL_BAD_PARAMETER); return; }
        const auto& surface = found->second;
        switch (jit.Regs()[2]) {
        case EGL_WIDTH: *output = surface.window ? ANativeWindow_getWidth(surface.window) : surface.width; break;
        case EGL_HEIGHT: *output = surface.window ? ANativeWindow_getHeight(surface.window) : surface.height; break;
        case EGL_CONFIG_ID: *output = 1; break;
        case EGL_RENDER_BUFFER: *output = EGL_BACK_BUFFER; break;
        case EGL_SWAP_BEHAVIOR: *output = EGL_BUFFER_DESTROYED; break;
        case EGL_TEXTURE_FORMAT: case EGL_TEXTURE_TARGET: if (!surface.window) *output = EGL_NO_TEXTURE; break;
        case EGL_MIPMAP_TEXTURE: if (!surface.window) *output = EGL_FALSE; break;
        case EGL_LARGEST_PBUFFER: if (!surface.window) *output = surface.largest; break;
        case EGL_MIPMAP_LEVEL: if (!surface.window) *output = 0; break;
        case EGL_MULTISAMPLE_RESOLVE: *output = EGL_MULTISAMPLE_RESOLVE_DEFAULT; break;
        case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: case EGL_PIXEL_ASPECT_RATIO: *output = EGL_UNKNOWN; break;
        default: jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE); return;
        }
        jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglQueryContext", [](Jit& jit, Host& host) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        const auto found = contexts.find(jit.Regs()[1]);
        if (found == contexts.end() || found->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_CONTEXT); return; }
        auto* output = EglSpan<EGLint>(host, jit.Regs()[3], 1);
        if (!output) { jit.Regs()[0] = EglError(EGL_BAD_PARAMETER); return; }
        switch (jit.Regs()[2]) {
        case EGL_CONFIG_ID: *output = 1; break;
        case EGL_CONTEXT_CLIENT_TYPE: *output = found->second.api; break;
        case EGL_CONTEXT_CLIENT_VERSION: *output = found->second.requestedMajor; break;
        case EGL_RENDER_BUFFER: *output = found->second.owner ? EGL_BACK_BUFFER : EGL_NONE; break;
        default: jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE); return;
        }
        jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglSurfaceAttrib", [](Jit& jit, Host&) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        const auto found = surfaces.find(jit.Regs()[1]);
        if (found == surfaces.end() || found->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_SURFACE); return; }
        if (jit.Regs()[2] == EGL_SWAP_BEHAVIOR) jit.Regs()[0] = jit.Regs()[3] == EGL_BUFFER_DESTROYED ? EGL_TRUE : EglError(EGL_BAD_MATCH);
        else if (jit.Regs()[2] == EGL_MULTISAMPLE_RESOLVE) jit.Regs()[0] = jit.Regs()[3] == EGL_MULTISAMPLE_RESOLVE_DEFAULT ? EGL_TRUE : EglError(EGL_BAD_MATCH);
        else jit.Regs()[0] = EglError(EGL_BAD_ATTRIBUTE);
    });
    manager.RegisterThunk("eglSwapBuffers", [](Jit& jit, Host& host) {
        ANativeWindow* window = nullptr;
        {
            std::lock_guard lock(eglMutex);
            if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
            const auto found = surfaces.find(jit.Regs()[1]);
            if (found == surfaces.end() || found->second.deleted) { jit.Regs()[0] = EglError(EGL_BAD_SURFACE); return; }
            if (!currentContext || currentDraw != found->first) { jit.Regs()[0] = EglError(EGL_BAD_CURRENT_SURFACE); return; }
            window = found->second.window;
        }
        if (!window) { jit.Regs()[0] = EGL_TRUE; return; }
        AuditBl2NativeResolutionPresent(host);
        NativeApplyFrameRateLimit();
        const auto start = NativePerfSwapStart();
        { HornPerfScope scope(HornPerfCategory::Swap); jit.Regs()[0] = bl2vk::SwapBuffers(window) ? EGL_TRUE : EglError(EGL_BAD_SURFACE); }
        NativePerfPresent(host, start, jit.Regs()[0] != 0);
        if (jit.Regs()[0]) HornPerfFrameBoundary();
    });
    manager.RegisterThunk("eglSwapInterval", [](Jit& jit, Host&) {
        std::lock_guard lock(eglMutex);
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        if (!currentContext) { jit.Regs()[0] = EglError(EGL_BAD_CONTEXT); return; }
        bl2vk::SwapInterval(std::clamp(static_cast<EGLint>(jit.Regs()[1]), 0, 1)); jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglGetCurrentContext", [](Jit& jit, Host&) { jit.Regs()[0] = currentContext; });
    manager.RegisterThunk("eglGetCurrentDisplay", [](Jit& jit, Host&) { jit.Regs()[0] = currentContext ? kDisplay : 0; });
    manager.RegisterThunk("eglGetCurrentSurface", [](Jit& jit, Host&) {
        const auto selector = jit.Regs()[0];
        jit.Regs()[0] = selector == EGL_DRAW ? currentDraw : selector == EGL_READ ? currentRead : EglError(EGL_BAD_PARAMETER);
    });
    manager.RegisterThunk("eglGetError", [](Jit& jit, Host&) { jit.Regs()[0] = eglError; eglError = EGL_SUCCESS; });
    manager.RegisterThunk("eglQueryString", [](Jit& jit, Host&) {
        std::lock_guard lock(eglMutex);
        if (!jit.Regs()[0] && jit.Regs()[1] == EGL_EXTENSIONS) { Return(jit, ""); return; }
        if (!Initialized(jit.Regs()[0])) { jit.Regs()[0] = 0; return; }
        const char* value = nullptr;
        switch (jit.Regs()[1]) {
        case EGL_VENDOR: value = "Borderlands 2 native Vulkan"; break;
        case EGL_VERSION: value = "1.5"; break;
        case EGL_CLIENT_APIS: value = "OpenGL OpenGL_ES"; break;
        case EGL_EXTENSIONS: value = "EGL_KHR_create_context"; break;
        default: EglError(EGL_BAD_PARAMETER); break;
        }
        Return(jit, value);
    });
    manager.RegisterThunk("eglWaitGL", [](Jit& jit, Host&) { if (currentContext) bl2vk::Finish(); jit.Regs()[0] = EGL_TRUE; });
    manager.RegisterThunk("eglWaitClient", [](Jit& jit, Host&) { if (currentContext) bl2vk::Finish(); jit.Regs()[0] = EGL_TRUE; });
    manager.RegisterThunk("eglWaitNative", [](Jit& jit, Host&) {
        jit.Regs()[0] = jit.Regs()[0] == EGL_CORE_NATIVE_ENGINE ? EGL_TRUE : EglError(EGL_BAD_PARAMETER);
    });
    manager.RegisterThunk("eglReleaseThread", [](Jit& jit, Host&) {
        std::lock_guard lock(eglMutex);
        if (currentContext && !bl2vk::MakeCurrent(0, nullptr)) { jit.Regs()[0] = EglError(EGL_BAD_ACCESS); return; }
        ReleaseCurrentLocked(); eglError = EGL_SUCCESS; eglApi = EGL_OPENGL_ES_API; jit.Regs()[0] = EGL_TRUE;
    });
    manager.RegisterThunk("eglTerminate", [](Jit& jit, Host&) {
        std::lock_guard lock(eglMutex);
        if (!Display(jit.Regs()[0])) { jit.Regs()[0] = EGL_FALSE; return; }
        for (auto found = contexts.begin(); found != contexts.end();) {
            if (found->second.owner) { found->second.deleted = true; ++found; }
            else { bl2vk::DestroyContext(found->second.native); found = contexts.erase(found); }
        }
        for (auto found = surfaces.begin(); found != surfaces.end();) {
            if (found->second.owner) { found->second.deleted = true; ++found; }
            else { const auto id = found->first; ++found; RetireSurface(id); }
        }
        displayInitialized = false; jit.Regs()[0] = EGL_TRUE;
    });
    auto resolve = [&manager](Jit& jit, Host& host, bool dynamic) {
        const auto address = jit.Regs()[dynamic ? 1 : 0];
        const auto* name = GuestCString(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address);
        if (!name) { jit.Regs()[0] = 0; return; }
        const auto registered = manager.FindRegisteredSymbol(name);
        if (registered) { jit.Regs()[0] = registered; return; }
        const bool graphics = (!std::strncmp(name, "gl", 2) && name[2] >= 'A' && name[2] <= 'Z') ||
            (!std::strncmp(name, "egl", 3) && name[3] >= 'A' && name[3] <= 'Z');
        if (graphics) { jit.Regs()[0] = 0; return; }
        jit.Regs()[0] = dynamic ? static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(dlsym(reinterpret_cast<void*>(jit.Regs()[0]), name))) : 0;
    };
    manager.RegisterThunk("eglGetProcAddress", [resolve](Jit& jit, Host& host) { resolve(jit, host, false); });
    manager.RegisterThunk("dlsym", [resolve](Jit& jit, Host& host) { resolve(jit, host, true); });
}

GLenum ConvertedFormat(GLenum format) {
    switch (format) {
    case 0x83F0: case 0x83F1: return kBl2Astc4Textures ? 0x93B0 : GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2;
    case 0x8C4C: case 0x8C4D: return kBl2Astc4Textures ? 0x93D0 : GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2;
    case 0x83F3: return kBl2Astc4Textures ? 0x93B0 : GL_COMPRESSED_RGBA8_ETC2_EAC;
    case 0x8C4F: return kBl2Astc4Textures ? 0x93D0 : GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC;
    case 0x83F2: case 0x8C4E: GlError(GL_INVALID_ENUM); return 0;
    default: return format;
    }
}
bool ConvertedSize(GLenum format, GLsizei width, GLsizei height, GLsizei depth, GLsizei size) {
    const bool dxt1 = format == 0x83F0 || format == 0x83F1 || format == 0x8C4C || format == 0x8C4D;
    const bool dxt5 = format == 0x83F3 || format == 0x8C4F;
    if (!dxt1 && !dxt5) return true;
    if (width <= 0 || height <= 0 || depth <= 0 || size < 0) { GlError(GL_INVALID_VALUE); return false; }
    const std::uint64_t blocks = ((static_cast<std::uint64_t>(width) + 3) / 4) *
        ((static_cast<std::uint64_t>(height) + 3) / 4);
    const std::uint64_t blockSize = dxt1 && !kBl2Astc4Textures ? 8 : 16;
    if (blocks > UINT32_MAX / blockSize || blocks * blockSize > UINT32_MAX / static_cast<std::uint32_t>(depth) ||
        blocks * blockSize * depth != static_cast<std::uint64_t>(size)) { GlError(GL_INVALID_VALUE); return false; }
    return true;
}
struct Filter { GLint original = 0; std::uint32_t control = UINT32_MAX; };
thread_local std::unordered_map<std::uint64_t, Filter> filters;
void ApplyFiltering(GLenum target, GLuint texture) {
    if (target != GL_TEXTURE_2D || !texture) return;
    auto& filter = filters[(static_cast<std::uint64_t>(currentGroup) << 32) | texture];
    const auto control = Bl2GraphicsFiltering();
    if (filter.control == control) return;
    GLint maximum = 1; bl2vk::GetIntegerv(0x84FF, &maximum);
    maximum = std::max(maximum, 1); Bl2GraphicsHostAnisotropy(maximum);
    const auto selected = static_cast<GLint>(control & 0xFFu);
    bl2vk::TexParameteri(target, 0x84FE, std::clamp(selected ? selected : maximum, 1, maximum));
    if (filter.original) bl2vk::TexParameteri(target, GL_TEXTURE_MIN_FILTER,
        bl2::graphics::MinificationFilter(filter.original, control));
    filter.control = control;
}
void TextureParameter(GLenum target, GLenum pname, GLint value) {
    if (target == GL_TEXTURE_2D && pname == GL_TEXTURE_MIN_FILTER) {
        GLint texture = 0; bl2vk::GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        if (texture) {
            auto& filter = filters[(static_cast<std::uint64_t>(currentGroup) << 32) | static_cast<GLuint>(texture)];
            filter.original = value; filter.control = UINT32_MAX;
            ApplyFiltering(target, texture); return;
        }
    }
    bl2vk::TexParameteri(target, pname, value);
}

void RegisterLegacyPrograms(ThunkManager& manager) {
    Names(manager, "glGenProgramsARB", bl2vk::GenPrograms);
    Names(manager, "glDeleteProgramsARB", bl2vk::DeletePrograms);
    Register(manager, "glBindProgramARB", bl2vk::BindProgram);
    manager.RegisterThunk("glNamedProgramStringEXT", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs args(jit, host);
        const auto name = args.U32(), target = args.U32(), format = args.U32();
        const auto length = args.I32(); const auto address = args.Ptr();
        if ((target != kVertexProgram && target != kFragmentProgram) || format != kProgramAscii) { GlError(GL_INVALID_ENUM); return; }
        if (length < 9 || static_cast<std::size_t>(length) > kMaxShaderSource ||
            !Span<std::uint8_t>(host, address, length)) { GlError(GL_INVALID_VALUE); return; }
        const auto* nv = reinterpret_cast<const char*>(address);
        const bool expected = std::memcmp(nv, target == kVertexProgram ? "!!NVvp5.0" : "!!NVfp5.0", 9) == 0;
        std::string_view source;
        if (!expected || !RecoverSource({host.GetGuestMemoryBase(), host.GetGuestMemorySize()}, address, length, source)) {
            __android_log_print(ANDROID_LOG_ERROR, "BL2Vulkan", "GLASM source recovery failed name=%u target=%04x source=%08x length=%d", name, target, address, length);
            GlError(GL_INVALID_OPERATION); return;
        }
        const std::uint64_t metadataAddress = static_cast<std::uint64_t>(address) + length + 1;
        if (metadataAddress > UINT32_MAX || !GuestRangeValid(host.GetGuestMemorySize(), metadataAddress, 0x48) || nv[length] != 0) {
            GlError(GL_INVALID_VALUE); return;
        }
        const auto* metadata = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(metadataAddress));
        std::array<bl2vk::ConstantGroup, 9> groups;
        std::array<std::int16_t, 16> registers;
        for (std::size_t group = 0; group < groups.size(); ++group) {
            std::memcpy(&groups[group].first_register, metadata + group * 4, 2);
            std::memcpy(&groups[group].byte_count, metadata + group * 4 + 2, 2);
            if (!ConstantGroupValid(groups[group])) {
                GlError(GL_INVALID_VALUE); return;
            }
        }
        for (std::size_t unit = 0; unit < registers.size(); ++unit) {
            std::memcpy(&registers[unit], metadata + 0x24 + unit * 2, 2);
            if (registers[unit] < -1 || registers[unit] >= 4096) { GlError(GL_INVALID_VALUE); return; }
        }
        HornPerfScope scope(HornPerfCategory::ShaderCompile);
        if (!bl2vk::NamedProgramString(name, target, source, groups.data(), registers.data(), length)) GlError(GL_INVALID_OPERATION);
    });
    manager.RegisterThunk("glNamedProgramLocalParametersI4uivEXT", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs args(jit, host);
        const auto name = args.U32(), target = args.U32(), first = args.U32();
        const auto count = args.I32(); const auto address = args.Ptr();
        if (target != kVertexProgram && target != kFragmentProgram) { GlError(GL_INVALID_ENUM); return; }
        if (count < 0 || static_cast<std::uint64_t>(first) + static_cast<std::uint32_t>(count) > 4096) { GlError(GL_INVALID_VALUE); return; }
        const auto* words = Span<GLuint>(host, address, static_cast<std::size_t>(count) * 4);
        if (!count || words) bl2vk::NamedProgramLocalParametersI4uiv(name, target, first, count, words);
    });
    manager.RegisterThunk("glGetProgramivARB", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* value = Span<GLint>(host, jit.Regs()[2], 1);
        if (value) bl2vk::GetProgramiv(jit.Regs()[0], jit.Regs()[1], value);
    });
    auto getHandle = [](Jit& jit, Host&, bool withSampler) {
        const auto texture = jit.Regs()[0], sampler = withSampler ? jit.Regs()[1] : 0;
        auto failed = [&](GLenum requestedError, bool queried, GLboolean isTexture) {
            static std::atomic<std::uint32_t> reports{0};
            auto report = reports.load(std::memory_order_relaxed);
            while (report < 8 && !reports.compare_exchange_weak(report, report + 1, std::memory_order_relaxed)) {}
            if (report < 8) __android_log_print(ANDROID_LOG_ERROR, "BL2Vulkan",
                "Invalid bindless handle: report=%u operation=%s texture=%u sampler=%u isTextureQueried=%u isTexture=%u requestedError=0x%04x guestLR=0x%08x context=0x%08x group=%u returned={00000000,00000000}",
                report + 1, withSampler ? "glGetTextureSamplerHandleNV" : "glGetTextureHandleNV",
                texture, sampler, static_cast<unsigned>(queried), static_cast<unsigned>(isTexture), requestedError,
                jit.Regs()[14], currentContext, currentGroup);
            Return(jit, GLuint64{0});
        };
        if (!Current()) { failed(GL_INVALID_OPERATION, false, GL_FALSE); return; }
        const auto isTexture = texture ? bl2vk::IsTexture(texture) : GL_FALSE;
        if (!isTexture) { GlError(GL_INVALID_VALUE); failed(GL_INVALID_VALUE, texture != 0, isTexture); return; }
        const auto handle = static_cast<GLuint64>(texture) | (static_cast<GLuint64>(sampler) << 32);
        std::lock_guard lock(handleMutex);
        handles[currentGroup].try_emplace(handle, false); Return(jit, handle);
    };
    manager.RegisterThunk("glGetTextureHandleNV", [getHandle](Jit& jit, Host& host) { getHandle(jit, host, false); });
    manager.RegisterThunk("glGetTextureSamplerHandleNV", [getHandle](Jit& jit, Host& host) { getHandle(jit, host, true); });
    auto residency = [](Jit& jit, Host& host, int action) {
        if (!Current()) { if (!action) jit.Regs()[0] = GL_FALSE; return; }
        Arm32CallArgs args(jit, host); const auto handle = args.U64();
        std::lock_guard lock(handleMutex);
        const auto group = handles.find(currentGroup);
        const auto found = group == handles.end() ? std::unordered_map<GLuint64, bool>::iterator{} : group->second.find(handle);
        if (group == handles.end() || found == group->second.end()) {
            if (!action) jit.Regs()[0] = GL_FALSE; else GlError(GL_INVALID_OPERATION);
            return;
        }
        if (action) found->second = action > 0;
        else jit.Regs()[0] = found->second ? GL_TRUE : GL_FALSE;
    };
    manager.RegisterThunk("glMakeTextureHandleResidentNV", [residency](Jit& jit, Host& host) { residency(jit, host, 1); });
    manager.RegisterThunk("glMakeTextureHandleNonResidentNV", [residency](Jit& jit, Host& host) { residency(jit, host, -1); });
    manager.RegisterThunk("glIsTextureHandleResidentNV", [residency](Jit& jit, Host& host) { residency(jit, host, 0); });
}

void RegisterTextures(ThunkManager& manager) {
    Names(manager, "glGenTextures", bl2vk::GenTextures);
    manager.RegisterThunk("glDeleteTextures", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto count = static_cast<GLsizei>(jit.Regs()[0]);
        if (count < 0) { GlError(GL_INVALID_VALUE); return; }
        const auto* names = Span<GLuint>(host, jit.Regs()[1], count);
        if (count && !names) return;
        {
            std::lock_guard lock(handleMutex);
            for (GLsizei index = 0; index < count; ++index) {
                filters.erase((static_cast<std::uint64_t>(currentGroup) << 32) | names[index]);
                if (const auto group = handles.find(currentGroup); group != handles.end()) {
                    for (auto found = group->second.begin(); found != group->second.end();) {
                        found = static_cast<GLuint>(found->first) == names[index] ? group->second.erase(found) : std::next(found);
                    }
                }
            }
        }
        bl2vk::DeleteTextures(count, names);
    });
    manager.RegisterThunk("glBindTexture", [](Jit& jit, Host&) {
        if (!Current()) return; bl2vk::BindTexture(jit.Regs()[0], jit.Regs()[1]); ApplyFiltering(jit.Regs()[0], jit.Regs()[1]);
    });
    Register(manager, "glBindMultiTextureEXT", bl2vk::BindMultiTexture);
    Register(manager, "glActiveTexture", bl2vk::ActiveTexture);
    Register(manager, "glIsTexture", bl2vk::IsTexture);
    auto storage = [](Jit& jit, Host& host, bool volume) {
        if (!Current()) return;
        Arm32CallArgs args(jit, host);
        const auto target = args.U32(); const auto levels = args.I32();
        const auto format = ConvertedFormat(args.U32());
        const auto width = args.I32(), height = args.I32(), depth = volume ? args.I32() : 1;
        if (!format) return;
        if (volume) bl2vk::TexStorage3D(target, levels, format, width, height, depth);
        else bl2vk::TexStorage2D(target, levels, format, width, height);
    };
    manager.RegisterThunk("glTexStorage2D", [storage](Jit& jit, Host& host) { storage(jit, host, false); });
    manager.RegisterThunk("glTexStorage3D", [storage](Jit& jit, Host& host) { storage(jit, host, true); });
    Register(manager, "glGenerateMipmap", bl2vk::GenerateMipmap);
    Register(manager, "glPixelStorei", bl2vk::PixelStorei);
    Register(manager, "glTexParameteri", TextureParameter);
    manager.RegisterThunk("glTexParameteriv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto pname = jit.Regs()[1]; const auto count = pname == 0x8E46 || pname == 0x1004 ? 4u : 1u;
        const auto* value = Span<GLint>(host, jit.Regs()[2], count);
        if (!value) return;
        if (pname == 0x8E46) for (std::uint32_t index = 0; index < 4; ++index) bl2vk::TexParameteri(jit.Regs()[0], GL_TEXTURE_SWIZZLE_R + index, value[index]);
        else if (count == 1) TextureParameter(jit.Regs()[0], pname, value[0]);
        else { std::array<GLfloat, 4> floats; std::copy_n(value, 4, floats.data()); bl2vk::TexParameterfv(jit.Regs()[0], pname, floats.data()); }
    });
    Register(manager, "glTexParameterf", bl2vk::TexParameterf);
    manager.RegisterThunk("glTexParameterfv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto pname = jit.Regs()[1]; const auto count = pname == 0x8E46 || pname == 0x1004 ? 4u : 1u;
        const auto* value = Span<GLfloat>(host, jit.Regs()[2], count);
        if (value) bl2vk::TexParameterfv(jit.Regs()[0], pname, value);
    });
    manager.RegisterThunk("glGetTexParameteriv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto* value = Span<GLint>(host, jit.Regs()[2], jit.Regs()[1] == 0x8E46 || jit.Regs()[1] == 0x1004 ? 4 : 1);
        if (value) bl2vk::GetTexParameteriv(jit.Regs()[0], jit.Regs()[1], const_cast<GLint*>(value));
    });
    manager.RegisterThunk("glGetTexParameterfv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto count = jit.Regs()[1] == 0x8E46 || jit.Regs()[1] == 0x1004 ? 4u : 1u;
        auto* value = Span<GLfloat>(host, jit.Regs()[2], count);
        if (!value) return;
        std::array<GLint, 4> integers{}; bl2vk::GetTexParameteriv(jit.Regs()[0], jit.Regs()[1], integers.data());
        std::copy_n(integers.data(), count, value);
    });
    manager.RegisterThunk("glGetTexLevelParameteriv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* value = Span<GLint>(host, jit.Regs()[3], 1);
        if (value) bl2vk::GetTexLevelParameteriv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], value);
    });
    manager.RegisterThunk("glTexImage2D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(), internal = a.I32(), width = a.I32(), height = a.I32(), border = a.I32();
        const auto format = a.U32(), type = a.U32(), address = a.Ptr();
        if (Pixels(host, address, width, height, 1, format, type, false, true)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload); bl2vk::TexImage2D(target, level, internal, width, height, border, format, type, reinterpret_cast<const void*>(address));
        }
    });
    manager.RegisterThunk("glTexSubImage2D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(), x = a.I32(), y = a.I32(), width = a.I32(), height = a.I32();
        const auto format = a.U32(), type = a.U32(), address = a.Ptr();
        if (Pixels(host, address, width, height, 1, format, type, false, false)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload); bl2vk::TexSubImage2D(target, level, x, y, width, height, format, type, reinterpret_cast<const void*>(address));
        }
    });
    manager.RegisterThunk("glTexImage3D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(), internal = a.I32(), width = a.I32(), height = a.I32(), depth = a.I32(), border = a.I32();
        const auto format = a.U32(), type = a.U32(), address = a.Ptr();
        if (Pixels(host, address, width, height, depth, format, type, false, true, true)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload); bl2vk::TexImage3D(target, level, internal, width, height, depth, border, format, type, reinterpret_cast<const void*>(address));
        }
    });
    manager.RegisterThunk("glTexSubImage3D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(), x = a.I32(), y = a.I32(), z = a.I32(), width = a.I32(), height = a.I32(), depth = a.I32();
        const auto format = a.U32(), type = a.U32(), address = a.Ptr();
        if (Pixels(host, address, width, height, depth, format, type, false, false, true)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload); bl2vk::TexSubImage3D(target, level, x, y, z, width, height, depth, format, type, reinterpret_cast<const void*>(address));
        }
    });
    manager.RegisterThunk("glCompressedTexImage2D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(); const auto format = a.U32();
        const auto width = a.I32(), height = a.I32(), border = a.I32(), size = a.I32(); const auto address = a.Ptr();
        const auto converted = ConvertedFormat(format);
        if (converted && ConvertedSize(format, width, height, 1, size) && CompressedPixels(host, address, size, true)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload, std::max(size, 0)); bl2vk::CompressedTexImage2D(target, level, converted, width, height, border, size, reinterpret_cast<const void*>(address));
        }
    });
    manager.RegisterThunk("glCompressedTexSubImage2D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(), x = a.I32(), y = a.I32(), width = a.I32(), height = a.I32();
        const auto format = a.U32(); const auto size = a.I32(); const auto address = a.Ptr(); const auto converted = ConvertedFormat(format);
        if (converted && ConvertedSize(format, width, height, 1, size) && CompressedPixels(host, address, size, false)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload, std::max(size, 0)); bl2vk::CompressedTexSubImage2D(target, level, x, y, width, height, converted, size, reinterpret_cast<const void*>(address));
        }
    });
    manager.RegisterThunk("glCompressedTexImage3D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(); const auto format = a.U32();
        const auto width = a.I32(), height = a.I32(), depth = a.I32(), border = a.I32(), size = a.I32(); const auto address = a.Ptr(); const auto converted = ConvertedFormat(format);
        if (converted && ConvertedSize(format, width, height, depth, size) && CompressedPixels(host, address, size, true)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload, std::max(size, 0)); bl2vk::CompressedTexImage3D(target, level, converted, width, height, depth, border, size, reinterpret_cast<const void*>(address));
        }
    });
    manager.RegisterThunk("glCompressedTexSubImage3D", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto target = a.U32(); const auto level = a.I32(), x = a.I32(), y = a.I32(), z = a.I32(), width = a.I32(), height = a.I32(), depth = a.I32();
        const auto format = a.U32(); const auto size = a.I32(); const auto address = a.Ptr(); const auto converted = ConvertedFormat(format);
        if (converted && ConvertedSize(format, width, height, depth, size) && CompressedPixels(host, address, size, false)) {
            HornPerfScope scope(HornPerfCategory::TextureUpload, std::max(size, 0)); bl2vk::CompressedTexSubImage3D(target, level, x, y, z, width, height, depth, converted, size, reinterpret_cast<const void*>(address));
        }
    });
    Register(manager, "glCopyTexSubImage2D", bl2vk::CopyTexSubImage2D);
    for (const auto* name : {"glCopyImageSubData", "glCopyImageSubDataEXT", "glCopyImageSubDataOES", "glCopyImageSubDataNV"})
        Register(manager, name, bl2vk::CopyImageSubData);
    manager.RegisterThunk("glReadPixels", [](Jit& jit, Host& host) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto x = a.I32(), y = a.I32(), width = a.I32(), height = a.I32();
        const auto format = a.U32(), type = a.U32(), address = a.Ptr();
        if (Pixels(host, address, width, height, 1, format, type, true, false)) bl2vk::ReadPixels(x, y, width, height, format, type, reinterpret_cast<void*>(address));
    });
    Names(manager, "glGenSamplers", bl2vk::GenSamplers);
    Names(manager, "glDeleteSamplers", bl2vk::DeleteSamplers);
    Register(manager, "glBindSampler", bl2vk::BindSampler);
    Register(manager, "glSamplerParameteri", bl2vk::SamplerParameteri);
    Register(manager, "glSamplerParameterf", bl2vk::SamplerParameterf);
}

void RegisterBuffers(ThunkManager& manager) {
    Names(manager, "glGenBuffers", bl2vk::GenBuffers);
    Names(manager, "glDeleteBuffers", bl2vk::DeleteBuffers);
    Register(manager, "glBindBuffer", bl2vk::BindBuffer);
    Register(manager, "glIsBuffer", bl2vk::IsBuffer);
    auto store = [](Jit& jit, Host& host, bool immutable) {
        if (!Current()) return;
        const auto size = static_cast<std::int32_t>(jit.Regs()[1]);
        if (size < 0) { GlError(GL_INVALID_VALUE); return; }
        const auto* data = Span<std::uint8_t>(host, jit.Regs()[2], size, true);
        if (jit.Regs()[2] && !data) return;
        HornPerfScope scope(HornPerfCategory::BufferUpload, size);
        if (immutable) bl2vk::BufferStorage(jit.Regs()[0], size, data, jit.Regs()[3]);
        else bl2vk::BufferData(jit.Regs()[0], size, data, jit.Regs()[3]);
    };
    manager.RegisterThunk("glBufferData", [store](Jit& jit, Host& host) { store(jit, host, false); });
    manager.RegisterThunk("glBufferStorage", [store](Jit& jit, Host& host) { store(jit, host, true); });
    manager.RegisterThunk("glBufferStorageEXT", [store](Jit& jit, Host& host) { store(jit, host, true); });
    manager.RegisterThunk("glBufferSubData", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto offset = static_cast<std::int32_t>(jit.Regs()[1]), size = static_cast<std::int32_t>(jit.Regs()[2]);
        if (offset < 0 || size < 0) { GlError(GL_INVALID_VALUE); return; }
        const auto* data = Span<std::uint8_t>(host, jit.Regs()[3], size);
        if (!size || data) { HornPerfScope scope(HornPerfCategory::BufferUpload, size); bl2vk::BufferSubData(jit.Regs()[0], offset, size, data); }
    });
    Register(manager, "glCopyBufferSubData", bl2vk::CopyBufferSubData);
    Register(manager, "glMapBufferRange", bl2vk::MapBufferRange);
    Register(manager, "glMapBuffer", bl2vk::MapBuffer);
    Register(manager, "glMapBufferOES", bl2vk::MapBuffer);
    Register(manager, "glUnmapBuffer", bl2vk::UnmapBuffer);
    Register(manager, "glUnmapBufferOES", bl2vk::UnmapBuffer);
    Register(manager, "glFlushMappedBufferRange", bl2vk::FlushMappedBufferRange);
    manager.RegisterThunk("glGetBufferParameteriv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* output = Span<GLint>(host, jit.Regs()[2], 1);
        if (output) bl2vk::GetBufferParameteriv(jit.Regs()[0], jit.Regs()[1], output);
    });
    manager.RegisterThunk("glGetBufferPointerv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* output = Span<void*>(host, jit.Regs()[2], 1);
        if (output) bl2vk::GetBufferPointerv(jit.Regs()[0], jit.Regs()[1], output);
    });
    manager.RegisterThunk("glGetBufferSubData", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto offset = static_cast<std::int32_t>(jit.Regs()[1]), size = static_cast<std::int32_t>(jit.Regs()[2]);
        if (offset < 0 || size < 0) { GlError(GL_INVALID_VALUE); return; }
        auto* output = Span<std::uint8_t>(host, jit.Regs()[3], size);
        if (!size || output) bl2vk::GetBufferSubData(jit.Regs()[0], offset, size, output);
    });
    manager.RegisterThunk("glInvalidateBufferData", [](Jit& jit, Host&) {
        if (Current() && !bl2vk::IsBuffer(jit.Regs()[0])) GlError(GL_INVALID_VALUE);
        // Invalidation is a storage-discard hint; retaining bytes is valid GL behavior.
    });
}

void RegisterDraws(ThunkManager& manager) {
    Names(manager, "glGenVertexArrays", bl2vk::GenVertexArrays);
    Names(manager, "glDeleteVertexArrays", bl2vk::DeleteVertexArrays);
    Register(manager, "glBindVertexArray", bl2vk::BindVertexArray);
    Register(manager, "glEnableVertexAttribArray", bl2vk::EnableVertexAttribArray);
    Register(manager, "glDisableVertexAttribArray", bl2vk::DisableVertexAttribArray);
    Register(manager, "glVertexAttribDivisor", bl2vk::VertexAttribDivisor);
    auto pointer = [](Jit& jit, Host& host, bool integer) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto index = a.U32(); const auto size = a.I32(); const auto type = a.U32();
        const auto normalized = integer ? GL_FALSE : static_cast<GLboolean>(a.U32());
        const auto stride = a.I32(); const auto address = a.Ptr();
        GLint bound = 0; bl2vk::GetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound);
        if (!bound && address && !Span<std::uint8_t>(host, address, 1)) return;
        if (integer) bl2vk::VertexAttribIPointer(index, size, type, stride, reinterpret_cast<const void*>(address));
        else bl2vk::VertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<const void*>(address));
    };
    manager.RegisterThunk("glVertexAttribPointer", [pointer](Jit& jit, Host& host) { pointer(jit, host, false); });
    manager.RegisterThunk("glVertexAttribIPointer", [pointer](Jit& jit, Host& host) { pointer(jit, host, true); });
    manager.RegisterThunk("glVertexAttrib4fv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto* value = Span<GLfloat>(host, jit.Regs()[1], 4);
        if (value) bl2vk::VertexAttrib4fv(jit.Regs()[0], value);
    });
    manager.RegisterThunk("glGetVertexAttribiv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* value = Span<GLint>(host, jit.Regs()[2], jit.Regs()[1] == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1);
        if (value) bl2vk::GetVertexAttribiv(jit.Regs()[0], jit.Regs()[1], value);
    });
    manager.RegisterThunk("glGetVertexAttribPointerv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* value = Span<void*>(host, jit.Regs()[2], 1);
        if (value) bl2vk::GetVertexAttribPointerv(jit.Regs()[0], jit.Regs()[1], value);
    });
    manager.RegisterThunk("glDrawArrays", [](Jit& jit, Host&) {
        if (!Current()) return; HornPerfScope scope(HornPerfCategory::Draw);
        bl2vk::DrawArrays(jit.Regs()[0], static_cast<GLint>(jit.Regs()[1]), static_cast<GLsizei>(jit.Regs()[2]));
    });
    manager.RegisterThunk("glDrawArraysInstanced", [](Jit& jit, Host&) {
        if (!Current()) return; HornPerfScope scope(HornPerfCategory::Draw);
        bl2vk::DrawArraysInstanced(jit.Regs()[0], static_cast<GLint>(jit.Regs()[1]), static_cast<GLsizei>(jit.Regs()[2]), static_cast<GLsizei>(jit.Regs()[3]));
    });
    auto indexed = [](Jit& jit, Host& host, bool instanced, bool base) {
        if (!Current()) return;
        Arm32CallArgs a(jit, host); const auto mode = a.U32(); const auto count = a.I32(); const auto type = a.U32(), address = a.Ptr();
        const auto instances = instanced ? a.I32() : 1; const auto baseVertex = base ? a.I32() : 0;
        if (count < 0 || instances < 0) { GlError(GL_INVALID_VALUE); return; }
        const auto width = type == GL_UNSIGNED_BYTE ? 1u : type == GL_UNSIGNED_SHORT ? 2u : type == GL_UNSIGNED_INT ? 4u : 0u;
        if (!width) { GlError(GL_INVALID_ENUM); return; }
        const auto bytes = static_cast<std::uint64_t>(count) * width;
        if (bytes > UINT32_MAX) { GlError(GL_INVALID_VALUE); return; }
        GLint buffer = 0; bl2vk::GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buffer);
        if (!buffer && count && !Span<std::uint8_t>(host, address, bytes)) return;
        if (buffer) {
            GLint size = 0; bl2vk::GetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
            if (size < 0 || address > static_cast<std::uint32_t>(size) || bytes > static_cast<std::uint32_t>(size) - address) { GlError(GL_INVALID_OPERATION); return; }
        }
        HornPerfScope scope(HornPerfCategory::Draw);
        if (instanced) bl2vk::DrawElementsInstancedBaseVertex(mode, count, type, reinterpret_cast<const void*>(address), instances, baseVertex);
        else if (base) bl2vk::DrawElementsBaseVertex(mode, count, type, reinterpret_cast<const void*>(address), baseVertex);
        else bl2vk::DrawElements(mode, count, type, reinterpret_cast<const void*>(address));
    };
    manager.RegisterThunk("glDrawElements", [indexed](Jit& jit, Host& host) { indexed(jit, host, false, false); });
    manager.RegisterThunk("glDrawElementsInstanced", [indexed](Jit& jit, Host& host) { indexed(jit, host, true, false); });
    for (const auto* name : {"glDrawElementsBaseVertex", "glDrawElementsBaseVertexEXT", "glDrawElementsBaseVertexOES"})
        manager.RegisterThunk(name, [indexed](Jit& jit, Host& host) { indexed(jit, host, false, true); });
    for (const auto* name : {"glDrawElementsInstancedBaseVertex", "glDrawElementsInstancedBaseVertexEXT", "glDrawElementsInstancedBaseVertexOES"})
        manager.RegisterThunk(name, [indexed](Jit& jit, Host& host) { indexed(jit, host, true, true); });
}

void RegisterFramebuffers(ThunkManager& manager) {
    Names(manager, "glGenFramebuffers", bl2vk::GenFramebuffers);
    Names(manager, "glDeleteFramebuffers", bl2vk::DeleteFramebuffers);
    Register(manager, "glBindFramebuffer", bl2vk::BindFramebuffer);
    Register(manager, "glFramebufferTexture2D", bl2vk::FramebufferTexture2D);
    Register(manager, "glFramebufferTextureLayer", bl2vk::FramebufferTextureLayer);
    Register(manager, "glCheckFramebufferStatus", bl2vk::CheckFramebufferStatus);
    Register(manager, "glBlitFramebuffer", bl2vk::BlitFramebuffer);
    Register(manager, "glReadBuffer", bl2vk::ReadBuffer);
    Names(manager, "glGenRenderbuffers", bl2vk::GenRenderbuffers);
    Names(manager, "glDeleteRenderbuffers", bl2vk::DeleteRenderbuffers);
    Register(manager, "glBindRenderbuffer", bl2vk::BindRenderbuffer);
    Register(manager, "glRenderbufferStorage", bl2vk::RenderbufferStorage);
    Register(manager, "glRenderbufferStorageMultisample", bl2vk::RenderbufferStorageMultisample);
    Register(manager, "glRenderbufferStorageMultisampleEXT", bl2vk::RenderbufferStorageMultisample);
    Register(manager, "glFramebufferRenderbuffer", bl2vk::FramebufferRenderbuffer);
    manager.RegisterThunk("glGetFramebufferAttachmentParameteriv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* output = Span<GLint>(host, jit.Regs()[3], 1);
        if (output) bl2vk::GetFramebufferAttachmentParameteriv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], output);
    });
    manager.RegisterThunk("glGetRenderbufferParameteriv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* output = Span<GLint>(host, jit.Regs()[2], 1);
        if (output) bl2vk::GetRenderbufferParameteriv(jit.Regs()[0], jit.Regs()[1], output);
    });
    manager.RegisterThunk("glDrawBuffers", [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto count = static_cast<GLsizei>(jit.Regs()[0]);
        if (count < 0) { GlError(GL_INVALID_VALUE); return; }
        const auto* buffers = Span<GLenum>(host, jit.Regs()[1], count);
        if (!count || buffers) bl2vk::DrawBuffers(count, buffers);
    });
    auto discard = [](Jit& jit, Host& host) {
        if (!Current()) return;
        const auto count = static_cast<GLsizei>(jit.Regs()[1]);
        if (count < 0) { GlError(GL_INVALID_VALUE); return; }
        const auto* attachments = Span<GLenum>(host, jit.Regs()[2], count);
        if (!count || attachments) bl2vk::InvalidateFramebuffer(jit.Regs()[0], count, attachments);
    };
    manager.RegisterThunk("glInvalidateFramebuffer", discard);
    manager.RegisterThunk("glDiscardFramebufferEXT", discard);
}

void RegisterState(ThunkManager& manager) {
#define STATE(name) Register(manager, "gl" #name, bl2vk::name)
    STATE(Enable); STATE(Disable); STATE(IsEnabled); STATE(Viewport); STATE(Scissor);
    STATE(DepthRangef); STATE(DepthFunc); STATE(DepthMask); STATE(StencilFuncSeparate);
    STATE(StencilOpSeparate); STATE(StencilMaskSeparate); STATE(BlendFuncSeparate);
    STATE(BlendEquationSeparate); STATE(BlendColor); STATE(ColorMask); STATE(CullFace);
    STATE(FrontFace); STATE(PolygonOffset); STATE(LineWidth); STATE(ClearColor);
    STATE(ClearDepthf); STATE(ClearStencil); STATE(Clear); STATE(ClearBufferfi);
    STATE(Flush); STATE(Finish); STATE(FenceSync); STATE(ClientWaitSync); STATE(WaitSync); STATE(DeleteSync);
    STATE(BeginQuery); STATE(EndQuery);
#undef STATE
    Register(manager, "glDepthBoundsEXT", bl2vk::DepthBounds);
    manager.RegisterThunk("glBlendFunc", [](Jit& jit, Host&) { if (Current()) bl2vk::BlendFuncSeparate(jit.Regs()[0], jit.Regs()[1], jit.Regs()[0], jit.Regs()[1]); });
    manager.RegisterThunk("glBlendEquation", [](Jit& jit, Host&) { if (Current()) bl2vk::BlendEquationSeparate(jit.Regs()[0], jit.Regs()[0]); });
    manager.RegisterThunk("glStencilFunc", [](Jit& jit, Host&) { if (Current()) bl2vk::StencilFuncSeparate(GL_FRONT_AND_BACK, jit.Regs()[0], static_cast<GLint>(jit.Regs()[1]), jit.Regs()[2]); });
    manager.RegisterThunk("glStencilOp", [](Jit& jit, Host&) { if (Current()) bl2vk::StencilOpSeparate(GL_FRONT_AND_BACK, jit.Regs()[0], jit.Regs()[1], jit.Regs()[2]); });
    manager.RegisterThunk("glStencilMask", [](Jit& jit, Host&) { if (Current()) bl2vk::StencilMaskSeparate(GL_FRONT_AND_BACK, jit.Regs()[0]); });
    manager.RegisterThunk("glClearBufferfv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        if (jit.Regs()[0] != GL_COLOR && jit.Regs()[0] != GL_DEPTH) { GlError(GL_INVALID_ENUM); return; }
        const auto* value = Span<GLfloat>(host, jit.Regs()[2], jit.Regs()[0] == GL_COLOR ? 4 : 1);
        if (value) bl2vk::ClearBufferfv(jit.Regs()[0], static_cast<GLint>(jit.Regs()[1]), value);
    });
    manager.RegisterThunk("glClearBufferiv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        if (jit.Regs()[0] != GL_COLOR && jit.Regs()[0] != GL_STENCIL) { GlError(GL_INVALID_ENUM); return; }
        const auto* value = Span<GLint>(host, jit.Regs()[2], jit.Regs()[0] == GL_COLOR ? 4 : 1);
        if (value) bl2vk::ClearBufferiv(jit.Regs()[0], static_cast<GLint>(jit.Regs()[1]), value);
    });
    Names(manager, "glGenQueries", bl2vk::GenQueries);
    Names(manager, "glDeleteQueries", bl2vk::DeleteQueries);
    manager.RegisterThunk("glGetQueryObjectuiv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* value = Span<GLuint>(host, jit.Regs()[2], 1);
        if (value) bl2vk::GetQueryObjectuiv(jit.Regs()[0], jit.Regs()[1], value);
    });
    manager.RegisterThunk("glGetError", [](Jit& jit, Host&) { jit.Regs()[0] = bl2vk::GetError(); });
    manager.RegisterThunk("glGetString", [](Jit& jit, Host&) { Return(jit, Current() ? bl2vk::GetString(jit.Regs()[0]) : nullptr); });
    manager.RegisterThunk("glGetStringi", [](Jit& jit, Host&) { Return(jit, Current() ? bl2vk::GetStringi(jit.Regs()[0], jit.Regs()[1]) : nullptr); });
    manager.RegisterThunk("glGetIntegerv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* output = Span<GLint>(host, jit.Regs()[1], QueryWidth(jit.Regs()[0]));
        if (output) bl2vk::GetIntegerv(jit.Regs()[0], output);
    });
    manager.RegisterThunk("glGetFloatv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* output = Span<GLfloat>(host, jit.Regs()[1], QueryWidth(jit.Regs()[0]));
        if (output) bl2vk::GetFloatv(jit.Regs()[0], output);
    });
    manager.RegisterThunk("glGetBooleanv", [](Jit& jit, Host& host) {
        if (!Current()) return;
        auto* output = Span<GLboolean>(host, jit.Regs()[1], QueryWidth(jit.Regs()[0]));
        if (output) bl2vk::GetBooleanv(jit.Regs()[0], output);
    });
}

} // namespace

bool NativeVulkanCurrentSurfaceSize(int* width, int* height) {
    if (!width || !height) return false;
    std::lock_guard lock(eglMutex);
    const auto found = surfaces.find(currentDraw);
    if (!currentContext || found == surfaces.end()) return false;
    const auto& surface = found->second;
    *width = surface.window ? ANativeWindow_getWidth(surface.window) : surface.width;
    *height = surface.window ? ANativeWindow_getHeight(surface.window) : surface.height;
    return *width > 0 && *height > 0;
}

void RegisterNativeVulkanThunks(ThunkManager& manager, DynarmicHost&) {
    if (bl2vk::IsInitialized()) bl2vk::EnableFatalDispatch();
    RegisterEgl(manager);
    RegisterLegacyPrograms(manager);
    RegisterBuffers(manager);
    RegisterTextures(manager);
    RegisterDraws(manager);
    RegisterFramebuffers(manager);
    RegisterState(manager);
    assert(NativeVulkanThunkSelfCheck());
}

bool NativeVulkanThunkSelfCheck() {
    Host host;
    Jit jit;
    std::array<std::uint32_t, 4> stack{0x89ABCDEF, 0x01234567, 0x40400000, 0};
    jit.Regs()[13] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(stack.data()));
    jit.Regs()[0] = 0x3F800000; jit.Regs()[1] = 0xDEADBEEF;
    jit.Regs()[2] = 0x76543210; jit.Regs()[3] = 0xFEDCBA98;
    jit.ExtRegs()[0] = 0x40800000;
    Arm32CallArgs args(jit, host);
    if (ReadArg<GLfloat>(args) != 1.0f || ReadArg<GLuint64>(args) != 0xFEDCBA9876543210ULL ||
        ReadArg<GLuint64>(args) != 0x0123456789ABCDEFULL || ReadArg<GLfloat>(args) != 3.0f) return false;
    jit.Regs()[0] = 1; jit.Regs()[1] = 2; jit.Regs()[2] = 3;
    Arm32CallArgs mixed(jit, host);
    const auto decoded = std::tuple<GLuint, GLuint, GLuint, GLuint64>{
        ReadArg<GLuint>(mixed), ReadArg<GLuint>(mixed), ReadArg<GLuint>(mixed), ReadArg<GLuint64>(mixed)};
    if (decoded != std::tuple<GLuint, GLuint, GLuint, GLuint64>{1, 2, 3, 0x0123456789ABCDEFULL}) return false;
    Return(jit, GLuint64{0x0123456789ABCDEFULL});
    if (jit.Regs()[0] != 0x89ABCDEF || jit.Regs()[1] != 0x01234567) return false;
    constexpr char fixture[] = "\0//ShaderFileName:Test\nvoid main() {}\0!!NVvp5.0\0";
    const auto memory = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(fixture), sizeof(fixture));
    const auto nv = std::string_view(fixture + 1, sizeof(fixture) - 2).find("!!NVvp5.0") + 1;
    std::string_view source;
    if (!RecoverSource(memory, nv, 9, source) || source != "//ShaderFileName:Test\nvoid main() {}") return false;
    if (RecoverSource(memory, 0, 9, source) || RecoverSource(memory, nv, kMaxShaderSource + 1, source) ||
        RecoverSource(memory, nv, memory.size(), source) || RecoverSource(memory, nv - 1, 9, source)) return false;
    if (!ConstantGroupValid({-1, 0xFFFF}) || !ConstantGroupValid({4095, 16}) ||
        ConstantGroupValid({-2, 0xFFFF}) || ConstantGroupValid({0, 0xFFFF}) ||
        ConstantGroupValid({4095, 32})) return false;
    // Opaque fake windows are only compared here, never acquired or dereferenced.
    auto* window = reinterpret_cast<ANativeWindow*>(std::uintptr_t{1});
    auto* otherWindow = reinterpret_cast<ANativeWindow*>(std::uintptr_t{2});
    std::unordered_map<std::uint32_t, Surface> windowState{{7, Surface{window}}};
    const auto prior = FindWindowSurface(windowState, window);
    if (prior == windowState.end() || prior->first != 7 ||
        FindWindowSurface(windowState, otherWindow) != windowState.end()) return false;
    windowState.at(7).owner = 17;
    const auto owned = FindWindowSurface(windowState, window);
    if (owned == windowState.end() || owned->first != 7 || owned->second.deleted ||
        owned->second.owner != 17) return false;
    windowState.at(7).deleted = true;
    if (FindWindowSurface(windowState, window) != windowState.end()) return false;
    windowState.emplace(9, Surface{window, 640, 480, 17});
    windowState.emplace(11, Surface{otherWindow, 320, 240, 29});
    const auto replacement = FindWindowSurface(windowState, window);
    if (replacement == windowState.end() || replacement->first != 9) return false;
    windowState.at(7).owner = 0;
    std::array<unsigned, 2> nativeReleases{}, backendReleases{};
    auto* boundWindow = window;
    bool unexpectedPbuffer = false;
    const auto retireWindow = [&](ANativeWindow* retired, bool lastReference) {
        const auto index = retired == window ? 0u : 1u;
        ++nativeReleases[index];
        if (lastReference) {
            ++backendReleases[index];
            if (boundWindow == retired) boundWindow = nullptr;
        }
    };
    const auto retirePbuffer = [&](std::uint32_t) { unexpectedPbuffer = true; };
    RetireSurfaceState(windowState, 7, retireWindow, retirePbuffer);
    if (windowState.contains(7) || windowState.at(9).owner != 17 ||
        windowState.at(11).owner != 29 || boundWindow != window ||
        nativeReleases != std::array<unsigned, 2>{1, 0} ||
        backendReleases != std::array<unsigned, 2>{0, 0}) return false;
    RetireSurfaceState(windowState, 11, retireWindow, retirePbuffer);
    if (windowState.at(9).owner != 17 || boundWindow != window ||
        nativeReleases != std::array<unsigned, 2>{1, 1} ||
        backendReleases != std::array<unsigned, 2>{0, 1}) return false;
    RetireSurfaceState(windowState, 9, retireWindow, retirePbuffer);
    RetireSurfaceState(windowState, 9, retireWindow, retirePbuffer);
    if (!windowState.empty() || boundWindow || unexpectedPbuffer ||
        nativeReleases != std::array<unsigned, 2>{2, 1} ||
        backendReleases != std::array<unsigned, 2>{1, 1}) return false;
    std::size_t bytes = 0;
    if (!PixelExtent(3, 2, 1, 1, 4, 0, 0, 0, 0, 0, bytes) || bytes != 7) return false;
    if (!PixelExtent(3, 2, 2, 4, 4, 8, 4, 1, 2, 1, bytes) || bytes != 368) return false;
    if (!PixelExtent(0, 2, 1, 4, 4, 0, 0, 0, 0, 0, bytes) || bytes != 0) return false;
    if (PixelExtent(-1, 2, 1, 4, 4, 0, 0, 0, 0, 0, bytes) ||
        PixelExtent(1, 1, 1, 4, 3, 0, 0, 0, 0, 0, bytes) ||
        PixelExtent(INT32_MAX, INT32_MAX, INT32_MAX, 16, 8, 0, 0, 0, 0, 0, bytes)) return false;
    return true;
}
