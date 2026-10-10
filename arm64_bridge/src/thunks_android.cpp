#include "thunks_android.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include "dynarmic_host.h"
#include "jni_fake_env.h"
#include "perf_probe.h"
#include "bl2_native_resolution.h"
#include "bl2_hitch_report.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <elf.h>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <time.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/configuration.h>
#include <android/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "Arm32Bridge_Android"
#include "bridge_log.h"
#include "bl2_fog_audit.h"
#include "bl2_directory_cache.h"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {
template <typename T>
class HandleTable {
public:
    uint32_t Add(T value, bool retain = false) {
        if (!value) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [handle, existing] : values_) if (existing == value) { if (retain) ++refs_[handle]; return handle; }
        const uint32_t handle = next_++;
        values_[handle] = value; refs_[handle] = 1;
        return handle;
    }
    T Get(uint32_t handle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(handle);
        return it == values_.end() ? T{} : it->second;
    }
    void Remove(uint32_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        values_.erase(handle); refs_.erase(handle);
    }
    T Retain(uint32_t handle) {
        std::lock_guard lock(mutex_); auto it=values_.find(handle);
        if(it==values_.end())return T{}; ++refs_[handle];return it->second;
    }
    T Release(uint32_t handle) {
        std::lock_guard lock(mutex_); auto it=values_.find(handle);
        if(it==values_.end())return T{}; T value=it->second;
        if(--refs_[handle]==0) { values_.erase(it);refs_.erase(handle); }return value;
    }
private:
    mutable std::mutex mutex_;
    uint32_t next_ = 0x60000000;
    std::unordered_map<uint32_t, T> values_;
    std::unordered_map<uint32_t, size_t> refs_;
};

HandleTable<EGLDisplay> displays;
HandleTable<EGLConfig> configs;
HandleTable<EGLContext> contexts;
HandleTable<EGLSurface> surfaces;
HandleTable<ANativeWindow*> windows;
HandleTable<AAssetManager*> asset_managers;
HandleTable<AAsset*> assets;
HandleTable<AAssetDir*> asset_dirs;
HandleTable<AConfiguration*> configurations;

struct WindowSurfaceEntry {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    uint32_t surface_handle = 0;
};

std::mutex window_surfaces_mutex;
std::unordered_map<ANativeWindow*, WindowSurfaceEntry> window_surfaces;

const char* GuestString(DynarmicHost& host, uint32_t address) {
    if (!GuestRangeValid(host.GetGuestMemorySize(), address, 1)) return nullptr;
    const char* value = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + address);
    return memchr(value, 0, std::min<size_t>(1024 * 1024, host.GetGuestMemorySize() - address)) ? value : nullptr;
}

const EGLint* GuestEglInts(DynarmicHost& host, uint32_t address) {
    if (!address || address % alignof(EGLint)) return nullptr;
    const size_t size = host.GetGuestMemorySize();
    for (uint64_t cursor = address; cursor + sizeof(EGLint) <= size && cursor - address < 2048; cursor += 2 * sizeof(EGLint)) {
        if (host.MemoryRead32(static_cast<uint32_t>(cursor)) == EGL_NONE)
            return reinterpret_cast<const EGLint*>(host.GetGuestMemoryBase() + address);
        if (cursor + 2 * sizeof(EGLint) > size) break;
    }
    LOGE("EGL rejected unterminated guest attributes at 0x%08X", address);
    return nullptr;
}

std::string DescribeEglAttributes(const EGLint* attributes) {
    if (!attributes) return "(null)";

    std::string result = "[";
    for (size_t pair = 0; pair < 256 && attributes[pair * 2] != EGL_NONE; ++pair) {
        char item[64];
        snprintf(item, sizeof(item), "%s0x%04X=%d",
                 pair ? "," : "",
                 static_cast<unsigned>(attributes[pair * 2]),
                 static_cast<int>(attributes[pair * 2 + 1]));
        result += item;
    }
    result += "]";
    return result;
}

bool RequestsDesktopOpenGlContext(const EGLint* attributes) {
    if (!attributes) return false;

    for (size_t pair = 0; pair < 256 && attributes[pair * 2] != EGL_NONE; ++pair) {
        const EGLint name = attributes[pair * 2];
        const EGLint value = attributes[pair * 2 + 1];
        if (name == EGL_CONTEXT_OPENGL_PROFILE_MASK ||
            (name == EGL_CONTEXT_MAJOR_VERSION && value > 3)) {
            return true;
        }
    }
    return false;
}

uint32_t CopyHostString(DynarmicHost& host, const char* value) {
    if (!value) return 0;
    const size_t size = strlen(value) + 1;
    const uint32_t guest = host.GuestAlloc(size, 4);
    if (guest) memcpy(host.GetGuestMemoryBase() + guest, value, size);
    return guest;
}
}

struct AndroidStrings {
    std::mutex mutex;
    std::unordered_map<std::string, uint32_t> egl;
    std::unordered_map<uint32_t, uint32_t> dirs;
    std::unordered_map<uint32_t, uint32_t> buffers;
};

static void RegisterAndroidExtraThunks(ThunkManager& manager);

void RegisterAndroidThunks(ThunkManager& manager, DynarmicHost& host) {
    auto strings = std::make_shared<AndroidStrings>();
    manager.RegisterThunkIfAbsent("ANativeWindow_fromSurface", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        JNIEnv* env = FakeJniEnv::Instance().GetEnv();
        const uint32_t guest_surface = jit.Regs()[1];
        jobject surface = FakeJniEnv::Instance().ResolveObject(guest_surface);
        ANativeWindow* window = (env && surface)
            ? ANativeWindow_fromSurface(env, surface)
            : nullptr;
        const uint32_t handle = windows.Add(window, true);
        LOGI("ANativeWindow_fromSurface guest=0x%08X resolved=%p window=%p "
             "handle=0x%08X size=%dx%d format=%d",
             guest_surface, surface, window, handle,
             window ? ANativeWindow_getWidth(window) : 0,
             window ? ANativeWindow_getHeight(window) : 0,
             window ? ANativeWindow_getFormat(window) : 0);
        jit.Regs()[0] = handle;
        if (env && surface) env->DeleteLocalRef(surface);
    });
    manager.RegisterThunkIfAbsent("ANativeWindow_release", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        ANativeWindow* window = windows.Release(jit.Regs()[0]);
        if (window) ANativeWindow_release(window);
    });

    manager.RegisterThunkIfAbsent("AAssetManager_fromJava", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        JNIEnv* env = FakeJniEnv::Instance().GetEnv();
        jobject object = FakeJniEnv::Instance().ResolveObject(jit.Regs()[1]);
        jit.Regs()[0] = (env && object) ? asset_managers.Add(AAssetManager_fromJava(env, object)) : 0;
        if (env && object) env->DeleteLocalRef(object);
    });
    manager.RegisterThunkIfAbsent("AAssetManager_open", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAssetManager* manager_ptr = asset_managers.Get(jit.Regs()[0]);
        const char* name = GuestString(host, jit.Regs()[1]);
        AAsset* asset = (manager_ptr && name) ? AAssetManager_open(manager_ptr, name, jit.Regs()[2]) : nullptr;
        jit.Regs()[0] = assets.Add(asset);
        LOGI("AAssetManager_open '%s' -> 0x%08X", name ? name : "(null)", jit.Regs()[0]);
    });
    manager.RegisterThunkIfAbsent("AAssetManager_openDir", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAssetManager* manager_ptr = asset_managers.Get(jit.Regs()[0]);
        const char* name = GuestString(host, jit.Regs()[1]);
        AAssetDir* dir = (manager_ptr && name) ? AAssetManager_openDir(manager_ptr, name) : nullptr;
        jit.Regs()[0] = asset_dirs.Add(dir);
        LOGI("AAssetManager_openDir '%s' -> 0x%08X", name ? name : "(null)", jit.Regs()[0]);
    });
    manager.RegisterThunkIfAbsent("AAsset_read", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        void* buffer = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], jit.Regs()[2]);
        jit.Regs()[0] = (asset && buffer) ? static_cast<uint32_t>(AAsset_read(asset, buffer, jit.Regs()[2])) : static_cast<uint32_t>(-1);
    });
    manager.RegisterThunkIfAbsent("AAsset_seek", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        jit.Regs()[0] = asset ? static_cast<uint32_t>(AAsset_seek(asset, static_cast<off_t>(static_cast<int32_t>(jit.Regs()[1])), jit.Regs()[2])) : static_cast<uint32_t>(-1);
    });
    manager.RegisterThunkIfAbsent("AAsset_close", [strings](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        if (asset) AAsset_close(asset);
        assets.Remove(jit.Regs()[0]);
        std::lock_guard lock(strings->mutex);
        auto found=strings->buffers.find(jit.Regs()[0]);
        if(found!=strings->buffers.end()) { host.GuestFree(found->second);strings->buffers.erase(found); }
    });
    manager.RegisterThunkIfAbsent("AAsset_getBuffer", [strings](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t handle=jit.Regs()[0];AAsset* asset=assets.Get(handle);
        if(!asset) { jit.Regs()[0]=0;return; }
        std::lock_guard lock(strings->mutex);
        auto found=strings->buffers.find(handle);
        if(found!=strings->buffers.end()) { jit.Regs()[0]=found->second;return; }
        const int64_t length=AAsset_getLength64(asset);const void* buffer=AAsset_getBuffer(asset);
        uint32_t guest=buffer&&length>=0&&uint64_t(length)<=UINT32_MAX?host.GuestAlloc(std::max<int64_t>(length,1)):0;
        if(guest) { memcpy(host.GetGuestMemoryBase()+guest,buffer,length);strings->buffers.emplace(handle,guest); }
        jit.Regs()[0]=guest;
    });
    manager.RegisterThunkIfAbsent("AAsset_isAllocated", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        AAsset* asset=assets.Get(jit.Regs()[0]);jit.Regs()[0]=asset?AAsset_isAllocated(asset):0;
    });
    manager.RegisterThunkIfAbsent("AAsset_getLength", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        jit.Regs()[0] = asset ? static_cast<uint32_t>(AAsset_getLength(asset)) : 0;
    });
    manager.RegisterThunkIfAbsent("AAsset_openFileDescriptor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAsset* asset = assets.Get(jit.Regs()[0]);
        if ((jit.Regs()[1] && !GuestRangeValid(host.GetGuestMemorySize(), jit.Regs()[1], sizeof(uint32_t))) ||
            (jit.Regs()[2] && !GuestRangeValid(host.GetGuestMemorySize(), jit.Regs()[2], sizeof(uint32_t)))) {
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        off_t start = 0, length = 0;
        int fd = asset ? AAsset_openFileDescriptor(asset, &start, &length) : -1;
        if (fd >= 0 && (start > INT32_MAX || length > INT32_MAX)) {
            close(fd); jit.Regs()[0] = static_cast<uint32_t>(-1); return;
        }
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(start));
        if (jit.Regs()[2]) host.MemoryWrite32(jit.Regs()[2], static_cast<uint32_t>(length));
        jit.Regs()[0] = static_cast<uint32_t>(fd);
    });
    manager.RegisterThunkIfAbsent("AAssetDir_getNextFileName", [strings](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAssetDir* dir = asset_dirs.Get(jit.Regs()[0]);
        std::lock_guard lock(strings->mutex);
        uint32_t& previous = strings->dirs[jit.Regs()[0]];
        if (previous) host.GuestFree(previous);
        previous = dir ? CopyHostString(host, AAssetDir_getNextFileName(dir)) : 0;
        jit.Regs()[0] = previous;
    });
    manager.RegisterThunkIfAbsent("AAssetDir_rewind", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        AAssetDir* dir = asset_dirs.Get(jit.Regs()[0]);
        if (dir) AAssetDir_rewind(dir);
    });
    manager.RegisterThunkIfAbsent("AAssetDir_close", [strings](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        AAssetDir* dir = asset_dirs.Get(jit.Regs()[0]);
        if (dir) AAssetDir_close(dir);
        asset_dirs.Remove(jit.Regs()[0]);
        std::lock_guard lock(strings->mutex);
        auto found = strings->dirs.find(jit.Regs()[0]);
        if(found != strings->dirs.end()) { host.GuestFree(found->second);strings->dirs.erase(found); }
    });

    manager.RegisterThunkIfAbsent("eglBindAPI", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const EGLenum guest_api = static_cast<EGLenum>(jit.Regs()[0]);
        // NVIDIA's Android EGL accepted EGL_OPENGL_API for this game even
        // though the guest links libGLESv3.so and issues GLES calls. Android's
        // modern EGL implementation correctly rejects that desktop-GL enum.
        // Preserve the Tegra-era behavior by binding the equivalent GLES API.
        const EGLenum host_api = guest_api == EGL_OPENGL_API
            ? EGL_OPENGL_ES_API
            : guest_api;
        const EGLenum before = eglQueryAPI();
        const EGLBoolean ok = eglBindAPI(host_api);
        const EGLint error = ok ? EGL_SUCCESS : eglGetError();
        LOGI("eglBindAPI guest=0x%04X host=0x%04X before=0x%04X "
             "result=%u error=0x%04X tid=%d",
             guest_api, host_api, before, ok, error, gettid());
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglQueryAPI", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglQueryAPI(); });
    manager.RegisterThunkIfAbsent("eglGetDisplay", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        const EGLint error = display != EGL_NO_DISPLAY ? EGL_SUCCESS : eglGetError();
        const uint32_t handle = displays.Add(display);
        LOGI("eglGetDisplay native=%p handle=0x%08X error=0x%04X tid=%d",
             display, handle, error, gettid());
        jit.Regs()[0] = handle;
    });
    manager.RegisterThunkIfAbsent("eglInitialize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        EGLDisplay display = displays.Get(jit.Regs()[0]);
        EGLint* major = jit.Regs()[1] ? static_cast<EGLint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[1], sizeof(EGLint))) : nullptr;
        EGLint* minor = jit.Regs()[2] ? static_cast<EGLint*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[2], sizeof(EGLint))) : nullptr;
        if ((jit.Regs()[1] && !major) || (jit.Regs()[2] && !minor)) { jit.Regs()[0] = EGL_FALSE; return; }
        const EGLBoolean ok = eglInitialize(display, major, minor);
        const EGLint error = ok ? EGL_SUCCESS : eglGetError();
        LOGI("eglInitialize guest_display=0x%08X native=%p result=%u "
             "version=%d.%d error=0x%04X tid=%d",
             jit.Regs()[0], display, ok, major ? *major : -1,
             minor ? *minor : -1, error, gettid());
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglChooseConfig", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        EGLDisplay display = displays.Get(jit.Regs()[0]);
        const EGLint* attributes = GuestEglInts(host, jit.Regs()[1]);
        const uint32_t out = jit.Regs()[2];
        const EGLint capacity = static_cast<EGLint>(jit.Regs()[3]);
        const uint32_t count_out = host.MemoryRead32(jit.Regs()[13]);
        if ((jit.Regs()[1] && !attributes) || capacity < 0 ||
            !GuestRangeValid(host.GetGuestMemorySize(), count_out, sizeof(EGLint)) ||
            (out && !GuestRangeValid(host.GetGuestMemorySize(), out, static_cast<size_t>(capacity) * sizeof(uint32_t)))) {
            jit.Regs()[0] = EGL_FALSE;
            return;
        }
        EGLint count = 0;
        EGLBoolean ok = eglChooseConfig(display, attributes, nullptr, 0, &count);
        if (ok && out && capacity > 0 && count > 0) {
            const EGLint limit = std::min(count, capacity);
            std::vector<EGLConfig> found(limit);
            ok = eglChooseConfig(display, attributes, found.data(), limit, &count);
            if (ok) for (EGLint i = 0; i < std::min(count, limit); ++i) host.MemoryWrite32(out + i * 4, configs.Add(found[i]));
        } else if (out && capacity == 0) {
            count = 0;
        }
        host.MemoryWrite32(count_out, static_cast<uint32_t>(count));
        LOGI("eglChooseConfig display=0x%08X out=0x%08X capacity=%d "
             "count=%d result=%u first_config=0x%08X tid=%d",
             jit.Regs()[0], out, capacity, count, ok,
             (ok && out && capacity > 0 && count > 0) ? host.MemoryRead32(out) : 0,
             gettid());
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglGetConfigs", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        EGLDisplay display = displays.Get(jit.Regs()[0]);
        const uint32_t out = jit.Regs()[1];
        const EGLint capacity = static_cast<EGLint>(jit.Regs()[2]);
        const uint32_t count_out = jit.Regs()[3];
        if (capacity < 0 || !GuestRangeValid(host.GetGuestMemorySize(), count_out, sizeof(EGLint)) ||
            (out && !GuestRangeValid(host.GetGuestMemorySize(), out, static_cast<size_t>(capacity) * sizeof(uint32_t)))) {
            jit.Regs()[0] = EGL_FALSE;
            return;
        }
        EGLint count = 0;
        EGLBoolean ok = eglGetConfigs(display, nullptr, 0, &count);
        if (ok && out && capacity > 0 && count > 0) {
            const EGLint limit = std::min(count, capacity);
            std::vector<EGLConfig> found(limit);
            ok = eglGetConfigs(display, found.data(), limit, &count);
            if (ok) for (EGLint i = 0; i < std::min(count, limit); ++i) host.MemoryWrite32(out + i * 4, configs.Add(found[i]));
        } else if (out && capacity == 0) {
            count = 0;
        }
        host.MemoryWrite32(count_out, static_cast<uint32_t>(count));
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglGetConfigAttrib", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!GuestRangeValid(host.GetGuestMemorySize(), jit.Regs()[3], sizeof(EGLint))) { jit.Regs()[0] = EGL_FALSE; return; }
        const EGLint attribute = static_cast<EGLint>(jit.Regs()[2]);
        EGLint value = 0;
        const EGLBoolean ok = eglGetConfigAttrib(displays.Get(jit.Regs()[0]),
                                                  configs.Get(jit.Regs()[1]),
                                                  attribute, &value);
        if (ok && attribute == EGL_RENDERABLE_TYPE &&
            (value & EGL_OPENGL_ES3_BIT)) {
            // Match the Tegra EGL facade paired with the eglBindAPI remap: the
            // guest filters otherwise valid GLES configs for EGL_OPENGL_BIT.
            // The underlying config and context remain native GLES objects.
            const EGLint host_value = value;
            value |= EGL_OPENGL_BIT;
            static std::atomic<uint32_t> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOGI("eglGetConfigAttrib EGL_RENDERABLE_TYPE host=0x%04X "
                     "guest=0x%04X config=0x%08X tid=%d",
                     host_value, value, jit.Regs()[1], gettid());
            }
        }
        if (ok) host.MemoryWrite32(jit.Regs()[3], static_cast<uint32_t>(value));
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglCreateWindowSurface", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLint* attributes = GuestEglInts(host, jit.Regs()[3]);
        if (jit.Regs()[3] && !attributes) { jit.Regs()[0] = 0; return; }
        const uint32_t display_handle = jit.Regs()[0];
        const uint32_t config_handle = jit.Regs()[1];
        const uint32_t window_handle = jit.Regs()[2];
        EGLDisplay display = displays.Get(display_handle);
        EGLConfig config = configs.Get(config_handle);
        ANativeWindow* window = windows.Get(window_handle);

        // SurfaceView commonly delivers surfaceCreated followed immediately by
        // surfaceChanged for the same native window.  This Tegra-era renderer
        // creates its EGL window surface in both callbacks without destroying
        // the first one.  Modern Android rejects the second connection with
        // EGL_BAD_ALLOC, after which the guest stores EGL_NO_SURFACE and all
        // swaps fail.  Reuse the still-live, configuration-compatible surface.
        if (window) {
            std::lock_guard lock(window_surfaces_mutex);
            auto cached = window_surfaces.find(window);
            if (cached != window_surfaces.end()) {
                EGLSurface surface = surfaces.Get(cached->second.surface_handle);
                if (surface != EGL_NO_SURFACE && cached->second.display == display &&
                    cached->second.config == config) {
                    LOGI("eglCreateWindowSurface reused window=0x%08X/%p "
                         "native=%p handle=0x%08X tid=%d",
                         window_handle, window, surface,
                         cached->second.surface_handle, gettid());
                    jit.Regs()[0] = cached->second.surface_handle;
                    return;
                }
                if (surface == EGL_NO_SURFACE) {
                    window_surfaces.erase(cached);
                }
            }
        }

        EGLSurface surface = eglCreateWindowSurface(display, config, window, attributes);
        const EGLint error = surface != EGL_NO_SURFACE ? EGL_SUCCESS : eglGetError();
        EGLint surface_width = 0, surface_height = 0;
        if (surface != EGL_NO_SURFACE) {
            eglQuerySurface(display, surface, EGL_WIDTH, &surface_width);
            eglQuerySurface(display, surface, EGL_HEIGHT, &surface_height);
        }
        const uint32_t surface_handle = surfaces.Add(surface);
        if (window && surface != EGL_NO_SURFACE) {
            std::lock_guard lock(window_surfaces_mutex);
            window_surfaces[window] = {display, config, surface_handle};
        }
        LOGI("eglCreateWindowSurface display=0x%08X/%p config=0x%08X/%p "
             "window=0x%08X/%p attrs=0x%08X native=%p handle=0x%08X "
             "error=0x%04X size=%dx%d window=%dx%d tid=%d",
             display_handle, display, config_handle, config, window_handle, window,
             jit.Regs()[3], surface, surface_handle, error,
             surface_width, surface_height,
             window ? ANativeWindow_getWidth(window) : 0,
             window ? ANativeWindow_getHeight(window) : 0, gettid());
        jit.Regs()[0] = surface_handle;
    });
    manager.RegisterThunkIfAbsent("eglCreateContext", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLint* attributes = GuestEglInts(host, jit.Regs()[3]);
        if (jit.Regs()[3] && !attributes) { jit.Regs()[0] = 0; return; }
        const uint32_t display_handle = jit.Regs()[0];
        const uint32_t config_handle = jit.Regs()[1];
        const uint32_t share_handle = jit.Regs()[2];
        const uint32_t attributes_address = jit.Regs()[3];
        EGLDisplay display = displays.Get(display_handle);
        EGLConfig config = configs.Get(config_handle);
        EGLContext share = contexts.Get(share_handle);
        const EGLint gles32_attributes[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 2,
            EGL_NONE,
        };
        const bool translated_desktop_request = RequestsDesktopOpenGlContext(attributes);
        const EGLint* host_attributes = translated_desktop_request
            ? gles32_attributes : attributes;
        const std::string guest_attributes_text = DescribeEglAttributes(attributes);
        const std::string host_attributes_text = DescribeEglAttributes(host_attributes);
        EGLContext context = eglCreateContext(display, config, share, host_attributes);
        const EGLint error = eglGetError();
        extern void Bl2ShaderWarmupContextCreated(EGLDisplay, EGLContext, EGLContext);
        Bl2ShaderWarmupContextCreated(display, context, share);
        const uint32_t context_handle = contexts.Add(context);
        LOGI("eglCreateContext display=0x%08X/%p config=0x%08X/%p "
             "share=0x%08X/%p guest_attrs=0x%08X%s host_attrs=%s "
             "translated_desktop=%d native=%p handle=0x%08X error=0x%04X tid=%d",
             display_handle, display, config_handle, config, share_handle, share,
             attributes_address, guest_attributes_text.c_str(),
             host_attributes_text.c_str(), translated_desktop_request,
             context, context_handle, error, gettid());
        jit.Regs()[0] = context_handle;
    });
    manager.RegisterThunkIfAbsent("eglMakeCurrent", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t display_handle = jit.Regs()[0];
        const uint32_t draw_handle = jit.Regs()[1];
        const uint32_t read_handle = jit.Regs()[2];
        const uint32_t context_handle = jit.Regs()[3];
        EGLDisplay display = displays.Get(display_handle);
        EGLSurface draw = surfaces.Get(draw_handle);
        EGLSurface read = surfaces.Get(read_handle);
        EGLContext context = contexts.Get(context_handle);
        const EGLBoolean result = eglMakeCurrent(display, draw, read, context);
        const EGLint error = eglGetError();
        extern void RefreshGlBindings();
        if (result) RefreshGlBindings();
        if (result && context != EGL_NO_CONTEXT) CheckBl2NativeResolutionLimits(host);

        if (result && context != EGL_NO_CONTEXT) {
            static std::mutex identity_mutex;
            static std::array<EGLContext, 4> reported_contexts{};
            std::lock_guard lock(identity_mutex);
            const auto found = std::find(reported_contexts.begin(), reported_contexts.end(), context);
            const auto empty = std::find(reported_contexts.begin(), reported_contexts.end(), EGL_NO_CONTEXT);
            if (found == reported_contexts.end() && empty != reported_contexts.end()) {
                *empty = context;
                // These are actual host GL calls after a successful binding,
                // before the guest-facing NVIDIA/version facade is consulted.
                const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
                const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
                const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
                const auto* shading = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
                __android_log_print(ANDROID_LOG_INFO, "BL2Arm64GL",
                    "host display=%p context=%p guest_context=0x%08X vendor='%s' renderer='%s' version='%s' GLSL='%s' tid=%d",
                    display, context, context_handle, vendor ? vendor : "(null)",
                    renderer ? renderer : "(null)", version ? version : "(null)",
                    shading ? shading : "(null)", gettid());
            }
        }
#if HORN_ENABLE_LOGGING
        const char* version = result && context != EGL_NO_CONTEXT
            ? reinterpret_cast<const char*>(glGetString(GL_VERSION)) : nullptr;
        const char* shading_language = result && context != EGL_NO_CONTEXT
            ? reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)) : nullptr;
        LOGI("eglMakeCurrent display=0x%08X/%p draw=0x%08X/%p "
             "read=0x%08X/%p context=0x%08X/%p result=%u error=0x%04X "
             "GL_VERSION='%s' GLSL='%s' tid=%d",
             display_handle, display, draw_handle, draw, read_handle, read,
             context_handle, context, result, error,
              version ? version : "(unavailable)",
              shading_language ? shading_language : "(unavailable)", gettid());
#endif
        jit.Regs()[0] = result;
    });
    manager.RegisterThunkIfAbsent("eglSwapBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t display_handle = jit.Regs()[0];
        const uint32_t surface_handle = jit.Regs()[1];
        EGLDisplay display = displays.Get(display_handle);
        EGLSurface surface = surfaces.Get(surface_handle);
#if !defined(BL2_NATIVE_ARM32)
        extern bool ApplyBl2FrameRateLimit(DynarmicHost&);
        ApplyBl2FrameRateLimit(host);
#endif
        AuditBl2NativeResolutionPresent(host);
        // Bl2FogAuditPresent();  // Pixel readback diagnosis paused.
        Bl2DirectoryCacheReportStartup();
        EGLBoolean result;
        {
            HornPerfScope perf(HornPerfCategory::Swap);
            result = eglSwapBuffers(display, surface);
        }
        const EGLint error = eglGetError();
        if (result == EGL_TRUE) {
            HornPerfFrameBoundary();
            extern void GlStateProbePresent();
            GlStateProbePresent();
            extern void RefreshSceneAuditMode();
            RefreshSceneAuditMode();
#if !defined(BL2_NATIVE_ARM32)
            // Renderer idle slice for learned JIT warmup, only after a frame
            // that left the thread mostly idle (frame-limited, not render-bound).
            static thread_local uint64_t last_wall = 0, last_cpu = 0;
            const auto clock_ns = [](clockid_t clock) {
                timespec value{};
                clock_gettime(clock, &value);
                return static_cast<uint64_t>(value.tv_sec) * 1000000000ull +
                       static_cast<uint64_t>(value.tv_nsec);
            };
            const uint64_t wall = clock_ns(CLOCK_MONOTONIC);
            const uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
            if (last_wall && wall > last_wall && (cpu - last_cpu) * 10 < (wall - last_wall) * 7)
                host.WarmLearnedBlocksInCallback(jit, 1000000);
            last_wall = clock_ns(CLOCK_MONOTONIC);
            last_cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
            Bl2HitchFrame();
#endif
        }
        static std::atomic<uint32_t> calls{0};
        const uint32_t index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (index <= 16 || !result) {
            LOGI("eglSwapBuffers #%u display=0x%08X/%p surface=0x%08X/%p "
                 "result=%u error=0x%04X tid=%d",
                 index, display_handle, display, surface_handle, surface,
                 result, error, gettid());
        } else if (index == 17) {
            LOGI("eglSwapBuffers: suppressing further successful calls after 16");
        }
        jit.Regs()[0] = result;
    });
    manager.RegisterThunkIfAbsent("eglSurfaceAttrib", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const EGLBoolean result = eglSurfaceAttrib(
            displays.Get(jit.Regs()[0]), surfaces.Get(jit.Regs()[1]),
            static_cast<EGLint>(jit.Regs()[2]), static_cast<EGLint>(jit.Regs()[3]));
        jit.Regs()[0] = result;
    });
    manager.RegisterThunkIfAbsent("eglSwapInterval", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = eglSwapInterval(displays.Get(jit.Regs()[0]), static_cast<EGLint>(jit.Regs()[1]));
    });
    manager.RegisterThunkIfAbsent("eglDestroySurface", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const uint32_t surface_handle = jit.Regs()[1];
        const EGLBoolean ok = eglDestroySurface(displays.Get(jit.Regs()[0]),
                                                surfaces.Get(surface_handle));
        if (ok) {
            surfaces.Remove(surface_handle);
            std::lock_guard lock(window_surfaces_mutex);
            for (auto entry = window_surfaces.begin(); entry != window_surfaces.end();) {
                if (entry->second.surface_handle == surface_handle) {
                    entry = window_surfaces.erase(entry);
                } else {
                    ++entry;
                }
            }
        }
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglDestroyContext", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const EGLContext context = contexts.Get(jit.Regs()[1]);
        const EGLBoolean ok = eglDestroyContext(displays.Get(jit.Regs()[0]), context);
        if (ok) {
            extern void Bl2ShaderWarmupContextDestroyed(EGLContext);
            Bl2ShaderWarmupContextDestroyed(context);
            contexts.Remove(jit.Regs()[1]);
        }
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglTerminate", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const EGLDisplay display = displays.Get(jit.Regs()[0]);
        const EGLBoolean ok = eglTerminate(display);
        if (ok) {
            extern void Bl2ShaderWarmupDisplayTerminated(EGLDisplay);
            Bl2ShaderWarmupDisplayTerminated(display);
        }
        jit.Regs()[0] = ok;
    });
    manager.RegisterThunkIfAbsent("eglWaitNative", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglWaitNative(jit.Regs()[0]); });
    manager.RegisterThunkIfAbsent("eglWaitGL", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglWaitGL(); });
    manager.RegisterThunkIfAbsent("eglReleaseThread", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglReleaseThread(); });
    manager.RegisterThunkIfAbsent("eglGetError", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = eglGetError(); });
    manager.RegisterThunkIfAbsent("eglQueryString", [strings](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const EGLint name = static_cast<EGLint>(jit.Regs()[1]);
        const char* value = eglQueryString(displays.Get(jit.Regs()[0]), name);
        if (!value) {
            LOGE("eglQueryString display=0x%08X name=0x%04X returned null error=0x%04X",
                 jit.Regs()[0], name, eglGetError());
            jit.Regs()[0] = 0;
            return;
        }
        LOGI("eglQueryString display=0x%08X name=0x%04X len=%zu prefix='%.192s'",
             jit.Regs()[0], name, strlen(value), value);
        std::lock_guard lock(strings->mutex);
        auto [it, inserted] = strings->egl.try_emplace(value, 0);
        if (inserted) it->second = CopyHostString(host, value);
        jit.Regs()[0] = it->second;
    });
    manager.RegisterThunkIfAbsent("eglGetCurrentDisplay", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = displays.Add(eglGetCurrentDisplay()); });
    manager.RegisterThunkIfAbsent("eglGetCurrentContext", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = contexts.Add(eglGetCurrentContext()); });
    manager.RegisterThunkIfAbsent("eglGetCurrentSurface", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = surfaces.Add(eglGetCurrentSurface(jit.Regs()[0])); });
    manager.RegisterThunkIfAbsent("eglGetProcAddress", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = GuestString(host, jit.Regs()[0]);
        jit.Regs()[0] = name ? host.GetThunkManager().FindRegisteredSymbol(name) : 0;
    });

    RegisterAndroidExtraThunks(manager);
    LOGI("Registered Android asset/window/EGL thunks");
}

static void RegisterAndroidExtraThunks(ThunkManager& manager) {
    using Jit=Dynarmic::A32::Jit;
    using Host=DynarmicHost;
    manager.RegisterThunkIfAbsent("ANativeWindow_acquire",[](Jit& j,Host&) { if(auto* w=windows.Retain(j.Regs()[0]))ANativeWindow_acquire(w); });
    manager.RegisterThunkIfAbsent("ANativeWindow_getWidth",[](Jit& j,Host&) { auto* w=windows.Get(j.Regs()[0]);j.Regs()[0]=w?ANativeWindow_getWidth(w):0; });
    manager.RegisterThunkIfAbsent("ANativeWindow_getHeight",[](Jit& j,Host&) { auto* w=windows.Get(j.Regs()[0]);j.Regs()[0]=w?ANativeWindow_getHeight(w):0; });
    manager.RegisterThunkIfAbsent("ANativeWindow_getFormat",[](Jit& j,Host&) { auto* w=windows.Get(j.Regs()[0]);j.Regs()[0]=w?ANativeWindow_getFormat(w):0; });
    manager.RegisterThunkIfAbsent("ANativeWindow_setBuffersGeometry", [](Jit& j, Host&) {
        ANativeWindow* window = windows.Get(j.Regs()[0]);
        const int32_t width = static_cast<int32_t>(j.Regs()[1]);
        const int32_t height = static_cast<int32_t>(j.Regs()[2]);
        const int32_t format = static_cast<int32_t>(j.Regs()[3]);
        const int32_t result = window ?
            ANativeWindow_setBuffersGeometry(window, width, height, format) : -EINVAL;
        static std::atomic<uint32_t> calls{0};
        if (calls.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOGI("ANativeWindow_setBuffersGeometry requested=%dx%d format=%d "
                 "result=%d actual=%dx%d", width, height, format, result,
                 window ? ANativeWindow_getWidth(window) : 0,
                 window ? ANativeWindow_getHeight(window) : 0);
        }
        j.Regs()[0] = static_cast<uint32_t>(result);
    });
    manager.RegisterThunkIfAbsent("AAsset_getLength64",[](Jit& j,Host&) { auto* a=assets.Get(j.Regs()[0]);uint64_t n=a?AAsset_getLength64(a):0;j.Regs()[0]=n;j.Regs()[1]=n>>32; });
    manager.RegisterThunkIfAbsent("AAsset_getRemainingLength",[](Jit& j,Host&) { auto* a=assets.Get(j.Regs()[0]);j.Regs()[0]=a?AAsset_getRemainingLength(a):0; });
    manager.RegisterThunkIfAbsent("AAsset_getRemainingLength64",[](Jit& j,Host&) { auto* a=assets.Get(j.Regs()[0]);uint64_t n=a?AAsset_getRemainingLength64(a):0;j.Regs()[0]=n;j.Regs()[1]=n>>32; });
    manager.RegisterThunkIfAbsent("AAsset_seek64",[](Jit& j,Host& h) {
        Arm32CallArgs args(j,h);auto* a=assets.Get(args.Ptr());int64_t offset=args.I64();int whence=args.I32();
        uint64_t result=a?AAsset_seek64(a,offset,whence):-1;j.Regs()[0]=result;j.Regs()[1]=result>>32;
    });
    manager.RegisterThunkIfAbsent("AAsset_openFileDescriptor64",[](Jit& j,Host& h) {
        const uint32_t start=j.Regs()[1],length=j.Regs()[2];
        if(!GuestRangeValid(h.GetGuestMemorySize(),start,8)||!GuestRangeValid(h.GetGuestMemorySize(),length,8)) { j.Regs()[0]=-1;return; }
        auto* a=assets.Get(j.Regs()[0]);off64_t begin=0,size=0;int fd=a?AAsset_openFileDescriptor64(a,&begin,&size):-1;
        if(fd>=0) { h.MemoryWrite64(start,begin);h.MemoryWrite64(length,size); }j.Regs()[0]=fd;
    });
    manager.RegisterThunkIfAbsent("AConfiguration_new",[](Jit& j,Host&) { j.Regs()[0]=configurations.Add(AConfiguration_new()); });
    manager.RegisterThunkIfAbsent("AConfiguration_delete",[](Jit& j,Host&) { auto* c=configurations.Get(j.Regs()[0]);if(c)AConfiguration_delete(c);configurations.Remove(j.Regs()[0]); });
    manager.RegisterThunkIfAbsent("AConfiguration_fromAssetManager",[](Jit& j,Host&) { auto* c=configurations.Get(j.Regs()[0]);auto* a=asset_managers.Get(j.Regs()[1]);if(c&&a)AConfiguration_fromAssetManager(c,a); });
    auto getter=[&](const char* name,int32_t(*fn)(AConfiguration*)) { manager.RegisterThunkIfAbsent(name,[fn](Jit& j,Host&) { auto* c=configurations.Get(j.Regs()[0]);j.Regs()[0]=c?fn(c):0; }); };
    getter("AConfiguration_getDensity",AConfiguration_getDensity);getter("AConfiguration_getOrientation",AConfiguration_getOrientation);
    getter("AConfiguration_getMcc",AConfiguration_getMcc);getter("AConfiguration_getMnc",AConfiguration_getMnc);
    getter("AConfiguration_getTouchscreen",AConfiguration_getTouchscreen);getter("AConfiguration_getKeyboard",AConfiguration_getKeyboard);
    getter("AConfiguration_getKeysHidden",AConfiguration_getKeysHidden);getter("AConfiguration_getNavigation",AConfiguration_getNavigation);
    getter("AConfiguration_getNavHidden",AConfiguration_getNavHidden);getter("AConfiguration_getSdkVersion",AConfiguration_getSdkVersion);
    getter("AConfiguration_getScreenSize",AConfiguration_getScreenSize);getter("AConfiguration_getScreenLong",AConfiguration_getScreenLong);
    getter("AConfiguration_getUiModeType",AConfiguration_getUiModeType);getter("AConfiguration_getUiModeNight",AConfiguration_getUiModeNight);
    manager.RegisterThunkIfAbsent("AConfiguration_getLanguage",[](Jit& j,Host& h) { auto* c=configurations.Get(j.Regs()[0]);auto* out=static_cast<char*>(GuestSpan(h.GetGuestMemoryBase(),h.GetGuestMemorySize(),j.Regs()[1],2));if(c&&out)AConfiguration_getLanguage(c,out); });
    manager.RegisterThunkIfAbsent("AConfiguration_getCountry",[](Jit& j,Host& h) { auto* c=configurations.Get(j.Regs()[0]);auto* out=static_cast<char*>(GuestSpan(h.GetGuestMemoryBase(),h.GetGuestMemorySize(),j.Regs()[1],2));if(c&&out)AConfiguration_getCountry(c,out); });
    manager.RegisterThunkIfAbsent("eglCreatePbufferSurface",[](Jit& j,Host& h) { const EGLint* a=GuestEglInts(h,j.Regs()[2]);if(j.Regs()[2]&&!a) { j.Regs()[0]=0;return; }j.Regs()[0]=surfaces.Add(eglCreatePbufferSurface(displays.Get(j.Regs()[0]),configs.Get(j.Regs()[1]),a)); });
    manager.RegisterThunkIfAbsent("eglQuerySurface",[](Jit& j,Host& h) { auto* out=static_cast<EGLint*>(GuestSpan(h.GetGuestMemoryBase(),h.GetGuestMemorySize(),j.Regs()[3],4));j.Regs()[0]=out?eglQuerySurface(displays.Get(j.Regs()[0]),surfaces.Get(j.Regs()[1]),j.Regs()[2],out):EGL_FALSE; });
    manager.RegisterThunkIfAbsent("eglQueryContext",[](Jit& j,Host& h) { auto* out=static_cast<EGLint*>(GuestSpan(h.GetGuestMemoryBase(),h.GetGuestMemorySize(),j.Regs()[3],4));j.Regs()[0]=out?eglQueryContext(displays.Get(j.Regs()[0]),contexts.Get(j.Regs()[1]),j.Regs()[2],out):EGL_FALSE; });
}
