#pragma once

class ThunkManager;
class DynarmicHost;

// Register after successful process-start Vulkan initialization, instead of GLES/EGL.
void RegisterNativeVulkanThunks(ThunkManager& manager, DynarmicHost& host);
bool NativeVulkanCurrentSurfaceSize(int* width, int* height);
bool NativeVulkanThunkSelfCheck();
