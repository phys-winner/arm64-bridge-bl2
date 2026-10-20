#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

static_assert(sizeof(void*) == 4, "The driver must be tested in an ARM32 process");

static void String(std::string_view value) {
    std::putchar('"');
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') std::putchar('\\');
        if (c < 32) std::printf("\\u%04x", c);
        else std::putchar(c);
    }
    std::putchar('"');
}

int main() {
    void* loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!loader) { std::fputs(dlerror(), stderr); return 1; }
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (!get) return 2;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get(nullptr, "vkEnumerateInstanceVersion"));
    uint32_t version = VK_API_VERSION_1_0;
    if (enumerateVersion && enumerateVersion(&version) != VK_SUCCESS) return 3;
    auto enumerateExtensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(get(nullptr, "vkEnumerateInstanceExtensionProperties"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    if (!enumerateExtensions || !createInstance) return 4;
    uint32_t extensionCount = 0;
    if (enumerateExtensions(nullptr, &extensionCount, nullptr) != VK_SUCCESS || extensionCount > 4096) return 5;
    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (enumerateExtensions(nullptr, &extensionCount, extensions.data()) != VK_SUCCESS) return 6;
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "BL2 ARM32 capability check", 1,
                               "BL2 native experiment", 1, std::min(version, VK_API_VERSION_1_1)};
    const VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkInstance instance = VK_NULL_HANDLE;
    const auto created = createInstance(&info, nullptr, &instance);
    if (created != VK_SUCCESS) { std::fprintf(stderr, "vkCreateInstance=%d\n", created); return 7; }
#define INSTANCE(name) auto name = reinterpret_cast<PFN_##name>(get(instance, #name)); if (!name) return 8
    INSTANCE(vkDestroyInstance);
    INSTANCE(vkEnumeratePhysicalDevices);
    INSTANCE(vkGetPhysicalDeviceProperties);
    INSTANCE(vkGetPhysicalDeviceFeatures);
    INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(vkGetPhysicalDeviceFormatProperties);
    INSTANCE(vkEnumerateDeviceExtensionProperties);
    INSTANCE(vkCreateDevice);
    INSTANCE(vkGetDeviceProcAddr);
#undef INSTANCE
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || !count || count > 32) return 9;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) return 10;
    std::printf("{\"abi\":\"armeabi-v7a\",\"pointer_bytes\":%zu,\"loader_version\":%u,\"instance_extensions\":[", sizeof(void*), version);
    for (uint32_t i = 0; i < extensionCount; ++i) { if (i) std::putchar(','); String(extensions[i].extensionName); }
    std::fputs("],\"devices\":[", stdout);
    for (uint32_t i = 0; i < count; ++i) {
        if (i) std::putchar(',');
        const auto physical = devices[i];
        VkPhysicalDeviceProperties props{};
        VkPhysicalDeviceFeatures features{};
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceProperties(physical, &props);
        vkGetPhysicalDeviceFeatures(physical, &features);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        std::fputs("{\"name\":", stdout); String(props.deviceName);
        std::printf(",\"api_version\":%u,\"driver_version\":%u,\"vendor_id\":%u,\"device_id\":%u,\"pipeline_cache_uuid\":\"", props.apiVersion, props.driverVersion, props.vendorID, props.deviceID);
        for (const auto b : props.pipelineCacheUUID) std::printf("%02x", b);
        std::fputs("\",\"features\":{", stdout);
#define FEATURE(name) std::printf("\"" #name "\":%u,", features.name)
        FEATURE(robustBufferAccess); FEATURE(fullDrawIndexUint32); FEATURE(independentBlend);
        FEATURE(geometryShader); FEATURE(tessellationShader); FEATURE(sampleRateShading);
        FEATURE(dualSrcBlend); FEATURE(logicOp); FEATURE(multiDrawIndirect); FEATURE(drawIndirectFirstInstance);
        FEATURE(depthClamp); FEATURE(depthBiasClamp); FEATURE(fillModeNonSolid); FEATURE(depthBounds);
        FEATURE(wideLines); FEATURE(largePoints); FEATURE(alphaToOne); FEATURE(multiViewport);
        FEATURE(samplerAnisotropy); FEATURE(textureCompressionETC2); FEATURE(textureCompressionASTC_LDR);
        FEATURE(textureCompressionBC); FEATURE(occlusionQueryPrecise); FEATURE(pipelineStatisticsQuery);
        FEATURE(vertexPipelineStoresAndAtomics); FEATURE(fragmentStoresAndAtomics);
        FEATURE(shaderImageGatherExtended); FEATURE(shaderUniformBufferArrayDynamicIndexing);
        FEATURE(shaderSampledImageArrayDynamicIndexing); FEATURE(shaderStorageBufferArrayDynamicIndexing);
        FEATURE(shaderStorageImageArrayDynamicIndexing); FEATURE(shaderClipDistance); FEATURE(shaderCullDistance);
        FEATURE(shaderFloat64); FEATURE(shaderInt64); FEATURE(shaderInt16);
        std::printf("\"inheritedQueries\":%u},\"limits\":{", features.inheritedQueries);
#undef FEATURE
#define LIMIT(name) std::printf("\"" #name "\":%llu,", static_cast<unsigned long long>(props.limits.name))
        LIMIT(maxImageDimension2D); LIMIT(maxImageDimensionCube); LIMIT(maxImageArrayLayers);
        LIMIT(maxUniformBufferRange); LIMIT(maxPushConstantsSize); LIMIT(maxMemoryAllocationCount);
        LIMIT(maxBoundDescriptorSets); LIMIT(maxPerStageDescriptorSamplers); LIMIT(maxPerStageDescriptorUniformBuffers);
        LIMIT(maxDescriptorSetSamplers); LIMIT(maxVertexInputAttributes); LIMIT(maxVertexInputBindings);
        LIMIT(maxVertexInputAttributeOffset); LIMIT(maxVertexInputBindingStride); LIMIT(maxVertexOutputComponents);
        LIMIT(maxFragmentInputComponents); LIMIT(maxFragmentOutputAttachments); LIMIT(minUniformBufferOffsetAlignment);
        LIMIT(nonCoherentAtomSize);
        std::printf("\"framebufferColorSampleCounts\":%u,\"framebufferDepthSampleCounts\":%u},\"memory_heaps\":[", props.limits.framebufferColorSampleCounts, props.limits.framebufferDepthSampleCounts);
#undef LIMIT
        for (uint32_t j = 0; j < memory.memoryHeapCount; ++j) {
            if (j) std::putchar(',');
            std::printf("{\"bytes\":%llu,\"flags\":%u}", static_cast<unsigned long long>(memory.memoryHeaps[j].size), memory.memoryHeaps[j].flags);
        }
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues.data());
        uint32_t graphics = UINT32_MAX;
        std::fputs("],\"queue_families\":[", stdout);
        for (uint32_t j = 0; j < queueCount; ++j) {
            if (j) std::putchar(',');
            std::printf("{\"flags\":%u,\"count\":%u,\"timestamp_bits\":%u}", queues[j].queueFlags, queues[j].queueCount, queues[j].timestampValidBits);
            if (graphics == UINT32_MAX && queues[j].queueCount && (queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) graphics = j;
        }
        if (graphics == UINT32_MAX) return 11;
        uint32_t deviceExtensionCount = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtensionCount, nullptr) != VK_SUCCESS || deviceExtensionCount > 4096) return 12;
        std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtensionCount, deviceExtensions.data()) != VK_SUCCESS) return 13;
        std::fputs("],\"extensions\":[", stdout);
        for (uint32_t j = 0; j < deviceExtensionCount; ++j) { if (j) std::putchar(','); String(deviceExtensions[j].extensionName); }
        std::fputs("],\"formats\":[", stdout);
        constexpr VkFormat formats[]{VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
            VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK};
        for (size_t j = 0; j < std::size(formats); ++j) {
            if (j) std::putchar(',');
            VkFormatProperties format{}; vkGetPhysicalDeviceFormatProperties(physical, formats[j], &format);
            std::printf("{\"format\":%d,\"linear\":%u,\"optimal\":%u,\"buffer\":%u}", formats[j], format.linearTilingFeatures, format.optimalTilingFeatures, format.bufferFeatures);
        }
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, graphics, 1, &priority};
        const VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queueInfo, 0, nullptr, 0, nullptr, nullptr};
        VkDevice device = VK_NULL_HANDLE;
        const auto deviceResult = vkCreateDevice(physical, &deviceInfo, nullptr, &device);
        VkResult cacheResult = VK_ERROR_INITIALIZATION_FAILED;
        if (deviceResult == VK_SUCCESS) {
            auto createCache = reinterpret_cast<PFN_vkCreatePipelineCache>(vkGetDeviceProcAddr(device, "vkCreatePipelineCache"));
            auto destroyCache = reinterpret_cast<PFN_vkDestroyPipelineCache>(vkGetDeviceProcAddr(device, "vkDestroyPipelineCache"));
            auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(device, "vkDestroyDevice"));
            if (!createCache || !destroyCache || !destroyDevice) return 14;
            const VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
            VkPipelineCache cache = VK_NULL_HANDLE;
            cacheResult = createCache(device, &cacheInfo, nullptr, &cache);
            if (cacheResult == VK_SUCCESS) destroyCache(device, cache, nullptr);
            destroyDevice(device, nullptr);
        }
        std::printf("],\"create_device_result\":%d,\"create_pipeline_cache_result\":%d,\"android_surface_present_tested\":false}", deviceResult, cacheResult);
        if (deviceResult != VK_SUCCESS || cacheResult != VK_SUCCESS) return 15;
    }
    std::fputs("],\"status\":\"CAPABILITIES_PASS\"}\n", stdout);
    vkDestroyInstance(instance, nullptr);
    dlclose(loader);
}
