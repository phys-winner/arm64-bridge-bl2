#pragma once

#include <jni.h>
#include <memory>
#include <string>
#include <vector>
#include "native_vulkan_seed.h"

class ThunkManager;
class Bl2ShaderStageCache;
namespace bl2::vk { class ShaderPack; }

bool InitializeNativeVfs(JNIEnv* env, jobject context, const char* base_apk_path);
void RegisterNativeVfsThunks(ThunkManager& manager);
bool NativeVfsSelfCheck();
// Current GL context; reader owns its compressed APK asset for this call only.
bool PrepareBl2ShaderStageCache(Bl2ShaderStageCache& cache);
// Header-only open on the renderer; default formation uses independent worker contexts.
bool OpenBl2ShaderStageCache(Bl2ShaderStageCache& cache);
std::shared_ptr<bl2::vk::ShaderPack> NativeOpenVulkanShaderPack(std::string& error);
const std::string& NativeSavedFilesRoot();
const std::string& NativeShaderCacheRoot();

bool NativeVulkanSeedRequired();
bool NativeReadVulkanMandatorySeed(std::vector<std::uint8_t>& bytes, bl2vk::seed::Request& request, std::string& error);
