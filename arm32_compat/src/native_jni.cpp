#include "native_jni.h"
#include "native_host.h"
#include "native_vfs.h"
#include "native_vulkan_renderer.h"
#include "bl2_native_resolution.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_settings.h"
#include "bl2_shader_former.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include "obb_pair_selection.h"
#include "thunk_manager.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <iterator>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <memory>
#include <thread>
#include <pthread.h>
#include <sys/system_properties.h>

extern JavaVM* GetHostJavaVM();

namespace {
constexpr const char* kTag = "BL2Native_JNI";
constexpr uint32_t kImageBytes = 0x03089000;
std::mutex patchMutex;
uint32_t activeTickLimit = 0;
std::atomic<uint32_t> displayTickLimit{0};
std::atomic<uint32_t> publishedTickLimit{0};
std::atomic<bool> localIdentityPrepared{false};
jclass shaderProgressClass = nullptr;
jmethodID shaderProgressMethod = nullptr;

template<class F> F engine(const char* symbol) {
    const uint32_t address = GetNativeHost().FindSymbol(symbol);
    if (!address) __android_log_print(ANDROID_LOG_ERROR, kTag, "Missing engine JNI export %s", symbol);
    return reinterpret_cast<F>(static_cast<uintptr_t>(address));
}

// The JNI calls and the original ARM32 ELF use the same Android base PCS.
// Typed calls retain the float arguments and compiler-provided jlong padding.
using InitFn = jboolean (*)(JNIEnv*, jobject, jint, jint, jfloat, jboolean,
                           jobject, jboolean, jint, jboolean);
using DeviceFn = jboolean (*)(JNIEnv*, jobject, jint, jint, jboolean);
using KeyFn = jboolean (*)(JNIEnv*, jobject, jint, jint, jint, jint, jlong);
using MotionFn = jboolean (*)(JNIEnv*, jobject, jint, jint, jfloat, jfloat,
    jfloat, jfloat, jfloat, jfloat, jfloat, jfloat, jint, jlong);
using MouseFn = jboolean (*)(JNIEnv*, jobject, jint, jfloat, jfloat, jfloat,
                           jfloat, jint, jint, jint);
using SurfaceFn = void (*)(JNIEnv*, jobject, jobject, jint, jint);
using LanguageFn = void (*)(JNIEnv*, jobject, jstring);
using BoolFn = jboolean (*)(JNIEnv*, jobject);
using InterruptFn = jboolean (*)(JNIEnv*, jobject, jboolean);
using CleanupFn = void (*)(JNIEnv*, jobject);
InitFn originalInit = nullptr;
DeviceFn originalDevice = nullptr;
KeyFn originalKey = nullptr;
MotionFn originalMotion = nullptr;
MouseFn originalMouse = nullptr;
SurfaceFn originalSurface = nullptr;
LanguageFn originalLanguage = nullptr;
BoolFn originalPause = nullptr, originalResume = nullptr;
InterruptFn originalInterrupt = nullptr;
CleanupFn originalCleanup = nullptr;

namespace native_lifecycle {
constexpr auto kUiWait = std::chrono::milliseconds(2000);
struct Request {
    std::uint64_t serial;
    jobject receiver;
    jboolean interrupted;
    bool done = false;
    jboolean result = JNI_TRUE;
};
std::mutex mutex;
std::condition_variable changed;
std::deque<std::shared_ptr<Request>> queue;
std::uint64_t next = 0, completed = 0;
bool started = false, attached = false, failed = false;

void Worker(JavaVM* vm) {
    pthread_setname_np(pthread_self(), "BL2Lifecycle");
    JNIEnv* env = nullptr;
    const bool ready = vm && vm->AttachCurrentThread(&env, nullptr) == JNI_OK && env;
    std::unique_lock lock(mutex);
    attached = ready;
    failed = !ready;
    changed.notify_all();
    if (!ready) return;
    for (;;) {
        changed.wait(lock, [] { return !queue.empty(); });
        const auto request = queue.front();
        queue.pop_front();
        lock.unlock();
        jboolean result = originalInterrupt(env, request->receiver, request->interrupted);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe(); env->ExceptionClear();
            result = JNI_FALSE;
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Native lifecycle callback raised a Java exception");
        }
        env->DeleteGlobalRef(request->receiver);
        lock.lock();
        request->receiver = nullptr;
        request->result = result;
        request->done = true;
        completed = request->serial;
        changed.notify_all();
    }
}

jboolean Call(JNIEnv* env, jobject self, jboolean interrupted) {
    if (!env || !self || env->ExceptionCheck() || !originalInterrupt) return JNI_FALSE;
    const auto deadline = std::chrono::steady_clock::now() + kUiWait;
    std::unique_lock lock(mutex);
    if (!started && !failed) {
        try { std::thread(Worker, GetHostJavaVM()).detach(); started = true; }
        catch (...) { failed = true; }
    }
    changed.wait_until(lock, deadline, [] { return attached || failed; });
    if (!attached || failed) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Native lifecycle worker unavailable");
        return JNI_FALSE;
    }
    jobject receiver = env->NewGlobalRef(self);
    if (!receiver || env->ExceptionCheck()) return JNI_FALSE;
    std::shared_ptr<Request> request;
    try {
        request = std::make_shared<Request>(Request{next + 1, receiver, interrupted});
        queue.push_back(request);
    } catch (...) {
        env->DeleteGlobalRef(receiver);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Native lifecycle request allocation failed");
        return JNI_FALSE;
    }
    ++next;
    changed.notify_all();
    changed.wait_until(lock, deadline, [&] { return request->done; });
    if (request->done) return request->result;
    // Keep the original transition in FIFO order without holding Android's UI thread.
    __android_log_print(ANDROID_LOG_WARN, kTag,
        "Native lifecycle interrupted=%u still waiting after 2000 ms; UI continues",
        unsigned(interrupted));
    return JNI_TRUE;
}

bool Drain() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kUiWait, [] { return completed == next; });
}
} // namespace native_lifecycle

void resetInput() {
    bl2::touch::sharedMailbox().reset(bl2::touch::ContextChanged);
    ResetBl2TouchPhysicalOwnership();
}

bool prepareLocalAndroidOwners(JNIEnv* env, jobject self) {
    if (localIdentityPrepared.load(std::memory_order_acquire)) return true;
    if (!env || !self || env->ExceptionCheck() || env->PushLocalFrame(8) != JNI_OK) return false;
    // Reuse 425's real Java sign-in worker before the original engine Init.
    // Its disconnected identity initializes actual local save/achievement owners.
    jclass activity = env->GetObjectClass(self);
    jfieldID identityField = activity ? env->GetFieldID(activity, "mLocalIdentity",
        "Lcom/tt2kgames/Borderlands2/LocalIdentity;") : nullptr;
    jfieldID workerField = identityField ? env->GetFieldID(activity, "mLocalSignInThread",
        "Ljava/lang/Thread;") : nullptr;
    jobject identity = workerField ? env->GetObjectField(self, identityField) : nullptr;
    jclass identityClass = identity ? env->GetObjectClass(identity) : nullptr;
    jmethodID prepare = identityClass ? env->GetMethodID(identityClass, "prepareStartup", "()V") : nullptr;
    if (prepare && !env->ExceptionCheck()) env->CallVoidMethod(identity, prepare);
    jobject worker = prepare && !env->ExceptionCheck() ? env->GetObjectField(self, workerField) : nullptr;
    jclass workerClass = worker ? env->GetObjectClass(worker) : nullptr;
    jmethodID join = workerClass ? env->GetMethodID(workerClass, "join", "(J)V") : nullptr;
    jmethodID alive = join ? env->GetMethodID(workerClass, "isAlive", "()Z") : nullptr;
    if (alive && !env->ExceptionCheck()) env->CallVoidMethod(worker, join, jlong{1000});
    const bool completed = alive && !env->ExceptionCheck() && !env->CallBooleanMethod(worker, alive);
    env->PopLocalFrame(nullptr);
    if (!completed || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Local identity preparation failed");
        return false;
    }
    auto& host = GetNativeHost();
    const auto tick = host.FindSymbol("_Z23AndroidSteam_TickSignInv");
    const auto signedIn = host.FindSymbol("_Z23AndroidSteam_IsSignedInv");
    const auto connected = host.FindSymbol("_Z24AndroidSteam_IsConnectedv");
    if (!tick || !signedIn || !connected) return false;
    host.RunGuestCallback(tick, {});
    const bool ready = host.RunGuestCallback(signedIn, {}) && !host.RunGuestCallback(connected, {});
    localIdentityPrepared.store(ready, std::memory_order_release);
    if (ready) __android_log_print(ANDROID_LOG_INFO, kTag,
        "Real local save/achievement owners prepared before native Init; remote=0");
    return ready;
}

bool installLocalPlatformMappings() {
    auto& host = GetNativeHost();
    const uint32_t base = host.GetElfLoader().GetBaseAddress();
    if (host.GetElfLoader().GetImageSize() != kImageBytes ||
        host.FindSymbol("nvsiInitJNI") != base + 0x02BA51C0 ||
        host.FindSymbol("nvsiInitJNINative") != base + 0x02BA4CE8) return false;
    struct Mapping {
        uint32_t literal, load, add, loadWord, addWord, relative;
        const char* original;
        const char* replacement;
    };
    // Match the arm64 FakeJni mappings while retaining the original real-JNI
    // initializer, its 23 method/field caches and its three native bindings.
    // These literals are PC-relative addresses, not persisted ELF strings.
    const Mapping mappings[] = {
        {0x02BA5440, 0x02BA5288, 0x02BA5290, 0xE59F11B0, 0xE08F1001, 0x00110278,
            "com/nvidia/nvsi/NvSecureInterfaceUtil", "com/tt2kgames/Borderlands2/LocalPlatform"},
        {0x02BA4CDC, 0x02BA4AD8, 0x02BA4AE0, 0xE59F31FC, 0xE08F3003, 0x001107D4,
            "()Lcom/nvidia/nvsi/NvSecureInterfaceUtil;", "()Lcom/tt2kgames/Borderlands2/LocalPlatform;"},
        {0x02BA50FC, 0x02BA4D60, 0x02BA4D68, 0xE59F2394, 0xE08F2002, 0x001107A0,
            "com/nvidia/nvsi/NvSecureInterfaceUtil", "com/tt2kgames/Borderlands2/LocalPlatform"},
    };
    const auto relative = [base](const Mapping& mapping) {
        return uint32_t(reinterpret_cast<uintptr_t>(mapping.replacement)) - (base + mapping.add + 8);
    };
    for (const auto& mapping : mappings) {
        const uint32_t word = host.MemoryRead32(base + mapping.literal);
        const uint32_t stringRva = mapping.add + 8 + mapping.relative;
        const size_t bytes = std::strlen(mapping.original) + 1;
        if (host.MemoryRead32(base + mapping.load) != mapping.loadWord ||
            host.MemoryRead32(base + mapping.add) != mapping.addWord ||
            (word != mapping.relative && word != relative(mapping)) ||
            stringRva >= kImageBytes || bytes > kImageBytes - stringRva ||
            std::memcmp(reinterpret_cast<const void*>(uintptr_t(base + stringRva)), mapping.original, bytes) != 0)
            return false;
    }
    for (const auto& mapping : mappings) {
        host.MemoryWrite32(base + mapping.literal, relative(mapping));
        host.InvalidateGuestCode(base + mapping.literal, 4);
        if (host.MemoryRead32(base + mapping.literal) != relative(mapping)) return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "Native NVSI JNI mapped to existing LocalPlatform; original caches retained");
    return true;
}

float displayRefreshRate(JNIEnv* env, jobject activity) {
    if (env->ExceptionCheck() || env->PushLocalFrame(8) != JNI_OK) return 0;
    float result = 0;
    do {
        jclass activityClass = env->GetObjectClass(activity);
        if (!activityClass) break;
        jmethodID getManager = env->GetMethodID(activityClass, "getWindowManager", "()Landroid/view/WindowManager;");
        if (!getManager) break;
        jobject manager = env->CallObjectMethod(activity, getManager);
        if (!manager || env->ExceptionCheck()) break;
        jclass managerClass = env->GetObjectClass(manager);
        if (!managerClass) break;
        jmethodID getDisplay = env->GetMethodID(managerClass, "getDefaultDisplay", "()Landroid/view/Display;");
        if (!getDisplay) break;
        jobject display = env->CallObjectMethod(manager, getDisplay);
        if (!display || env->ExceptionCheck()) break;
        jclass displayClass = env->GetObjectClass(display);
        if (!displayClass) break;
        jmethodID getRate = env->GetMethodID(displayClass, "getRefreshRate", "()F");
        if (getRate) result = env->CallFloatMethod(display, getRate);
    } while (false);
    if (env->ExceptionCheck()) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Display refresh query failed");
        env->ExceptionClear();
        result = 0;
    }
    env->PopLocalFrame(nullptr);
    return result;
}

bool installTickLimit(float refresh) {
    if (!std::isfinite(refresh) || refresh <= 0 || refresh > 1000) return false;
    std::lock_guard<std::mutex> lock(patchMutex);
    auto& host = GetNativeHost();
    const uint32_t site = host.FindSymbol("_ZN11UGameEngine14GetMaxTickRateEfj");
    if (host.GetElfLoader().GetImageSize() != kImageBytes ||
        site != host.GetElfLoader().GetBaseAddress() + 0x01460570) return false;
    constexpr uint32_t original[] = {0xE92D4070, 0xE1A06000, 0xE59F50E0};
    uint32_t bits;
    std::memcpy(&bits, &refresh, sizeof(bits));
    if (activeTickLimit == bits) return true;
    const uint32_t replacement[] = {0xE59F0000, 0xE12FFF1E, bits};
    bool pristine = true, active = activeTickLimit != 0;
    for (size_t i = 0; i < std::size(original); ++i) {
        const auto word = host.MemoryRead32(site + uint32_t(i * 4));
        pristine &= word == original[i];
        active &= word == (i == 2 ? activeTickLimit : replacement[i]);
    }
    if (!pristine && !active) return false;
    // Publish the literal before enabling the load/return instructions.
    host.MemoryWrite32(site + 8, bits);
    if (pristine) {
        host.MemoryWrite32(site + 4, replacement[1]);
        host.MemoryWrite32(site, replacement[0]);
    }
    host.InvalidateGuestCode(site, sizeof(replacement));
    for (size_t i = 0; i < std::size(replacement); ++i)
        if (host.MemoryRead32(site + uint32_t(i * 4)) != replacement[i]) return false;
    activeTickLimit = bits;
    publishedTickLimit.store(bits,std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "Native max tick limit %.3f FPS; original owned GetMaxTickRate literal", refresh);
    return true;
}

bool setDisplayRefreshRate(float refresh) {
    if (!std::isfinite(refresh) || refresh<=0 || refresh>1000) return false;
    uint32_t bits;
    std::memcpy(&bits,&refresh,sizeof(bits));
    displayTickLimit.store(bits,std::memory_order_release);
    return NativeApplyFrameRateLimit();
}

bool refreshVfs(JNIEnv* env, jobject activity) {
    if (env->ExceptionCheck() || env->PushLocalFrame(4) != JNI_OK) return false;
    bool result = false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID getPath = cls ? env->GetMethodID(cls, "getPackageCodePath", "()Ljava/lang/String;") : nullptr;
    jstring path = getPath ? static_cast<jstring>(env->CallObjectMethod(activity, getPath)) : nullptr;
    if (path && !env->ExceptionCheck()) {
        const char* chars = env->GetStringUTFChars(path, nullptr);
        if (chars) {
            result = InitializeNativeVfs(env, activity, chars);
            env->ReleaseStringUTFChars(path, chars);
        }
    }
    env->PopLocalFrame(nullptr);
    return result && !env->ExceptionCheck();
}

bool checkDiagnosticCommandLineParser() {
    char setting[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.bl2.no_sound", setting) <= 0 ||
        setting[0] != '1' || setting[1] != 0) return true;
    auto& host = GetNativeHost();
    const uint32_t base = host.GetElfLoader().GetBaseAddress();
    const uint32_t parser = host.FindSymbol("_Z10ParseParamPKcS0_");
    if (host.GetElfLoader().GetImageSize() != kImageBytes ||
        parser != base + 0x00FDD9E8) return false;
    const auto parse = reinterpret_cast<uint32_t (*)(const char*, const char*)>(uintptr_t(parser));
    const auto line_feed = parse("-nosound\n", "nosound");
    const auto space = parse("-nosound \n", "nosound");
    __android_log_print(ANDROID_LOG_INFO, kTag,
                       "Original ParseParam delimiter check: LF=%u space_LF=%u", line_feed, space);
    return line_feed == 0 && space == 1;
}

// Read the existing profile before originalInit can start RHI. Reuse the
// shipped Java preference parser without its Activity constructor/migration.
// shortcut: file-only startup; legacy preferences migrate at the existing Java boundary.
bool prepareInitialTouchPreferences(JNIEnv* env) {
    {
        auto& settings=bl2::touch::touchSettingsState;
        std::lock_guard lock(settings.mutex);
        if(settings.initialized)return true;
    }
    const auto& root=NativeSavedFilesRoot();if(root.empty())return false;
    const auto file=root+"/touch-controls.properties";
    const int fd=::open(file.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if(fd<0) {
        if(errno==ENOENT)return true; // Missing preferences inherit loaded engine settings.
        __android_log_print(ANDROID_LOG_ERROR,kTag,"Initial touch preference read failed: errno=%d",errno);
        return true;
    }
    struct Close { int fd;~Close(){::close(fd);} } close{fd};
    struct stat info{};
    if(::fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size>0x10000)return true;
    std::vector<std::uint8_t> bytes(0x10001);std::size_t size=0;
    while(size<bytes.size()) {
        const auto count=::read(fd,bytes.data()+size,bytes.size()-size);
        if(count<0 && errno==EINTR)continue;
        if(count<0)return true;
        if(!count)break;
        size+=std::size_t(count);
    }
    if(size>0x10000)return true;
    if(!env || env->ExceptionCheck() || env->PushLocalFrame(10)!=JNI_OK)return false;
    struct Pop { JNIEnv* env;~Pop(){env->PopLocalFrame(nullptr);} } pop{env};
    const auto preferencesClass=env->FindClass("com/tt2kgames/Borderlands2/touch/TouchPreferences");
    if(!preferencesClass || env->ExceptionCheck())return false;
    const auto propertiesClass=env->FindClass("java/util/Properties");
    if(!propertiesClass || env->ExceptionCheck())return false;
    const auto inputClass=env->FindClass("java/io/ByteArrayInputStream");
    if(!inputClass || env->ExceptionCheck())return false;
    const auto constructor=env->GetMethodID(preferencesClass,"<init>","()V");
    if(!constructor || env->ExceptionCheck())return false;
    const auto storeField=env->GetFieldID(preferencesClass,"store","Ljava/util/Properties;");
    if(!storeField || env->ExceptionCheck())return false;
    const auto reload=env->GetMethodID(preferencesClass,"reload","()V");
    if(!reload || env->ExceptionCheck())return false;
    const auto wireMethod=env->GetMethodID(preferencesClass,"settingsWire","(I)[I");
    if(!wireMethod || env->ExceptionCheck())return false;
    const auto load=env->GetMethodID(propertiesClass,"load","(Ljava/io/InputStream;)V");
    if(!load || env->ExceptionCheck())return false;
    const auto inputConstructor=env->GetMethodID(inputClass,"<init>","([B)V");
    if(!inputConstructor || env->ExceptionCheck())return false;
    const auto preferences=env->NewObject(preferencesClass,constructor);
    if(!preferences || env->ExceptionCheck())return false;
    const auto properties=env->GetObjectField(preferences,storeField);
    const auto data=env->NewByteArray(jsize(size));
    if(!properties || !data || env->ExceptionCheck())return false;
    env->SetByteArrayRegion(data,0,jsize(size),reinterpret_cast<const jbyte*>(bytes.data()));
    if(env->ExceptionCheck())return false;
    const auto input=env->NewObject(inputClass,inputConstructor,data);
    if(!input || env->ExceptionCheck())return false;
    env->CallVoidMethod(properties,load,input);
    if(env->ExceptionCheck()) {
        const auto error=env->ExceptionOccurred();env->ExceptionClear();
        const auto invalidClass=env->FindClass("java/lang/IllegalArgumentException");
        if(!invalidClass || !env->IsInstanceOf(error,invalidClass)) {
            if(!env->ExceptionCheck())env->Throw(error);
            return false;
        }
        const auto clear=env->GetMethodID(propertiesClass,"clear","()V");
        if(!clear || env->ExceptionCheck())return false;
        env->CallVoidMethod(properties,clear);
        if(env->ExceptionCheck())return false;
        __android_log_print(ANDROID_LOG_ERROR,kTag,"Cannot read touch settings; retaining file");
    }
    env->CallVoidMethod(preferences,reload);
    if(env->ExceptionCheck())return false;
    const auto wire=static_cast<jintArray>(env->CallObjectMethod(preferences,wireMethod,0));
    if(!wire || env->ExceptionCheck() || env->GetArrayLength(wire)!=jint(bl2::touch::kTouchSettingCount+2))return false;
    std::vector<std::uint32_t> words(bl2::touch::kTouchSettingCount+2);
    env->GetIntArrayRegion(wire,0,jsize(words.size()),reinterpret_cast<jint*>(words.data()));
    if(env->ExceptionCheck())return false;
    bl2::touch::ExchangeBl2TouchSettings(words);
    auto& settings=bl2::touch::touchSettingsState;
    std::lock_guard lock(settings.mutex);
    return settings.initialized;
}

jboolean JNICALL nativeInit(JNIEnv* env, jobject self, jint width, jint height,
    jfloat scale, jboolean useSurface, jobject assets, jboolean fullscreen,
    jint preferredFormat, jboolean useDepth) {
    if (!native_lifecycle::Drain() || !refreshVfs(env, self) || !prepareLocalAndroidOwners(env, self) ||
        !setDisplayRefreshRate(displayRefreshRate(env, self)) || !prepareInitialTouchPreferences(env)) return JNI_FALSE;
    ThunkManager::s_screenWidth = uint32_t(std::max(width, 0));
    ThunkManager::s_screenHeight = uint32_t(std::max(height, 0));
    return originalInit(env, self, width, height, scale, useSurface, assets,
                        fullscreen, preferredFormat, useDepth);
}
jboolean JNICALL nativeDevice(JNIEnv* env, jobject self, jint device, jint event, jboolean connected) {
    NotifyBl2TouchPhysicalDevice(device, connected != JNI_FALSE);
    return originalDevice(env, self, device, event, connected);
}
jboolean JNICALL nativeKey(JNIEnv* env, jobject self, jint device, jint action,
                           jint key, jint meta, jlong timestamp) {
    NotifyBl2TouchPhysicalKey(device, action, key);
    return originalKey(env, self, device, action, key, meta, timestamp);
}
jboolean JNICALL nativeMotion(JNIEnv* env, jobject self, jint device, jint action,
    jfloat x0, jfloat x1, jfloat x2, jfloat x3, jfloat x4, jfloat x5,
    jfloat x6, jfloat x7, jint meta, jlong timestamp) {
    NotifyBl2TouchPhysicalMotion(device, {x0, x1, x2, x3, x4, x5, x6, x7});
    return originalMotion(env, self, device, action, x0, x1, x2, x3, x4, x5, x6, x7, meta, timestamp);
}
jboolean JNICALL nativeMouse(JNIEnv* env, jobject self, jint device, jfloat x,
    jfloat y, jfloat dx, jfloat dy, jint left, jint right, jint wheel) {
    NotifyBl2TouchPhysicalMouse(device, left, right, wheel);
    return originalMouse(env, self, device, x, y, dx, dy, left, right, wheel);
}
void JNICALL nativeSurface(JNIEnv* env, jobject self, jobject surface, jint width, jint height) {
    bl2::touch::sharedMailbox().reset(bl2::touch::ContextChanged);
    if (!PrepareBl2NativeResolutionSurface(env, surface, width, height)) return;
    if (bl2vk::IsInitialized()) {
        auto* fresh = ANativeWindow_fromSurface(env, surface);
        if (!fresh || env->ExceptionCheck()) { if (fresh) ANativeWindow_release(fresh); return; }
        bl2vk::NotifyPresentationSurfaceChanged(fresh);
        ANativeWindow_release(fresh);
    }
    ThunkManager::s_screenWidth = uint32_t(width);
    ThunkManager::s_screenHeight = uint32_t(height);
    originalSurface(env, self, surface, width, height);
    LogBl2NativeResolutionState(GetNativeHost(), "native-surface-changed");
}
void JNICALL nativeLanguage(JNIEnv* env, jobject self, jstring) {
    // Verified locale splits replace INT assets; the guest resolver stays INT.
    jstring effective = env->NewStringUTF("en");
    if (!effective) return;
    originalLanguage(env, self, effective);
    env->DeleteLocalRef(effective);
}
jboolean JNICALL nativePause(JNIEnv* env, jobject self) {
    resetInput();
    const jboolean paused = originalPause(env, self);
    Bl2ShaderFormerPause(true);
    if (bl2vk::IsInitialized()) {
        bl2vk::SetPresentationPaused(true);
        bl2vk::PersistCaches();
    }
    return paused;
}
jboolean JNICALL nativeResume(JNIEnv* env, jobject self) {
    resetInput();
    Bl2ShaderFormerPause(false);
    if (bl2vk::IsInitialized()) bl2vk::SetPresentationPaused(false);
    return originalResume(env, self);
}
jboolean JNICALL nativeInterrupt(JNIEnv* env, jobject self, jboolean interrupted) {
    resetInput();
    return native_lifecycle::Call(env, self, interrupted);
}
void JNICALL nativeCleanup(JNIEnv* env, jobject self) {
    resetInput(); localIdentityPrepared.store(false, std::memory_order_release);
    Bl2ShaderFormerPause(true);
    if (!native_lifecycle::Drain())
        __android_log_print(ANDROID_LOG_WARN, kTag, "Pending interruption at native cleanup; retaining caller thread");
    // The game thread re-enters here through Java; a worker would join that waiting caller.
    originalCleanup(env, self);
}
void JNICALL contentPurchased(JNIEnv*, jobject, jstring) {}
void JNICALL platformNoop(JNIEnv*, jobject) {}
void JNICALL platformScreenOff(JNIEnv*, jobject) { resetInput(); }
void JNICALL platformRecheckNoop(JNIEnv*, jobject, jint) {}
} // namespace

bool NativeLocalIdentityPrepared() {
    return localIdentityPrepared.load(std::memory_order_acquire);
}

void NativeShaderPreparationProgress(std::uint32_t completed, std::uint32_t total) {
    extern JavaVM* GetHostJavaVM();
    JavaVM* vm = GetHostJavaVM();
    if (!vm || !shaderProgressClass || !shaderProgressMethod || total > 1000000) return;
    JNIEnv* env = nullptr;
    const bool attach = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attach && vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    if (!env) return;
    if (!env->ExceptionCheck()) {
        env->CallStaticVoidMethod(shaderProgressClass, shaderProgressMethod,
                                  jint(std::min(completed, total)), jint(total));
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            __android_log_print(ANDROID_LOG_ERROR, kTag, "Shader preparation progress failed");
        }
    }
    if (attach) vm->DetachCurrentThread();
}

float NativeCurrentFrameRateLimit() {
    const auto bits=displayTickLimit.load(std::memory_order_acquire);
    float display=0;
    std::memcpy(&display,&bits,sizeof(display));
    return NativeFrameRateFor(display,bl2::touch::Bl2FrameRateLimit());
}

bool NativeApplyFrameRateLimit() {
    const auto limit=NativeCurrentFrameRateLimit();
    if (limit==0) return false;
    uint32_t requestedBits;
    std::memcpy(&requestedBits,&limit,sizeof(requestedBits));
    if (publishedTickLimit.load(std::memory_order_acquire)==requestedBits) return true;
    return installTickLimit(limit);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_tt2kgames_Borderlands2_DisplayModePolicy_nativeRefreshRate(JNIEnv*, jclass, jfloat refresh) {
    return setDisplayRefreshRate(refresh) ? JNI_TRUE : JNI_FALSE;
}

bool InstallNativeJniRuntimePatches() {
    auto& host = GetNativeHost();
    const uint32_t base = host.GetElfLoader().GetBaseAddress();
    if (host.GetElfLoader().GetImageSize() != kImageBytes) return false;
    if (!installLocalPlatformMappings() || !checkDiagnosticCommandLineParser()) return false;
    if (kBl2Astc4Textures) {
        const uint32_t formats = host.FindSymbol("GPixelFormats");
        if (formats != base + 0x02F468EC) return false;
        constexpr uint32_t rows[2][9] = {
            {4, 4, 1, 8, 3, 0, 0, 1, 5}, {4, 4, 1, 16, 4, 0, 0, 1, 7}};
        for (unsigned row = 0; row < 2; ++row) {
            const uint32_t entry = formats + (row == 0 ? 5 : 7) * 40;
            for (unsigned word = 0; word < 9; ++word) {
                const uint32_t value = host.MemoryRead32(entry + (word + 1) * 4);
                if (value != rows[row][word] && !(row == 0 && word == 3 && value == 16)) return false;
            }
        }
        host.MemoryWrite32(formats + 5 * 40 + 16, 16);
        if (host.MemoryRead32(formats + 5 * 40 + 16) != 16) return false;
        __android_log_print(ANDROID_LOG_INFO, kTag, "Native ASTC allocation PF_DXT1 BlockBytes=16");
    }
    // Retain UE3 task completion/delegates while removing its remote DLC query.
    const uint32_t worker = host.FindSymbol("_ZN39FSteamworksReadDownloadableContentAsync6DoWorkEv");
    if (worker != base + 0x01F5FE94) return false;
    constexpr uint32_t original[] = {0xE92D4FF0, 0xE24DDF4F, 0xE59F52A8, 0xE58D0018, 0xE59F62A4, 0xE08F5005};
    constexpr uint32_t replacement[] = {0xE5901000, 0xE3A02000, 0xE5812018, 0xF57FF05F, 0xE1A00001, 0xE12FFF1E};
    bool pristine = true, active = true;
    for (size_t i = 0; i < std::size(original); ++i) {
        const uint32_t word = host.MemoryRead32(worker + uint32_t(i * 4));
        pristine &= word == original[i];
        active &= word == replacement[i];
    }
    if (!pristine && !active) return false;
    if (pristine) {
        for (size_t i = 0; i < std::size(replacement); ++i)
            host.MemoryWrite32(worker + uint32_t(i * 4), replacement[i]);
        host.InvalidateGuestCode(worker, sizeof(replacement));
    }
    for (size_t i = 0; i < std::size(replacement); ++i)
        if (host.MemoryRead32(worker + uint32_t(i * 4)) != replacement[i]) return false;
    __android_log_print(ANDROID_LOG_INFO, kTag, "Native offline DLC worker verified");
    return true;
}

bool RegisterNativeActivityHooks(JNIEnv* env) {
    if (!env || env->ExceptionCheck()) return false;
    if (!shaderProgressClass) {
        jclass controls = env->FindClass("com/tt2kgames/Borderlands2/LaunchControls");
        if (!controls) return false;
        shaderProgressMethod = env->GetStaticMethodID(controls, "showShaderPreparation", "(II)V");
        if (shaderProgressMethod && !env->ExceptionCheck())
            shaderProgressClass = static_cast<jclass>(env->NewGlobalRef(controls));
        env->DeleteLocalRef(controls);
        if (!shaderProgressClass || env->ExceptionCheck()) return false;
    }
    originalInit = engine<InitFn>("_Z19NativeCallback_InitP7_JNIEnvP8_jobjectiifhS2_hih");
    originalDevice = engine<DeviceFn>("_Z26NativeCallback_DeviceEventP7_JNIEnvP8_jobjectiih");
    originalKey = engine<KeyFn>("_Z23NativeCallback_KeyEventP7_JNIEnvP8_jobjectiiiix");
    originalMotion = engine<MotionFn>("_Z26NativeCallback_MotionEventP7_JNIEnvP8_jobjectiiffffffffix");
    originalMouse = engine<MouseFn>("_Z25NativeCallback_MouseEventP7_JNIEnvP8_jobjectiffffiii");
    originalSurface = engine<SurfaceFn>("_Z29NativeCallback_PostInitUpdateP7_JNIEnvP8_jobjectS2_ii");
    originalLanguage = engine<LanguageFn>("_Z26NativeCallback_LanguageSetP7_JNIEnvP8_jobjectP8_jstring");
    originalPause = engine<BoolFn>("_Z22NativeCallback_onPauseP7_JNIEnvP8_jobject");
    originalResume = engine<BoolFn>("_Z23NativeCallback_onResumeP7_JNIEnvP8_jobject");
    originalInterrupt = engine<InterruptFn>("_Z34NativeCallback_InterruptionChangedP7_JNIEnvP8_jobjecth");
    originalCleanup = engine<CleanupFn>("_Z22NativeCallback_CleanupP7_JNIEnvP8_jobject");
    if (!originalInit || !originalDevice || !originalKey || !originalMotion || !originalMouse ||
        !originalSurface || !originalLanguage || !originalPause || !originalResume ||
        !originalInterrupt || !originalCleanup) return false;
    jclass cls = env->FindClass("com/tt2kgames/Borderlands2/UE3JavaApp");
    if (!cls) return false;
#define JNI_ROW(name, signature, callback) {const_cast<char*>(name), const_cast<char*>(signature), reinterpret_cast<void*>(callback)}
    const JNINativeMethod methods[] = {
        JNI_ROW("NativeCallback_Init", "(IIFZLjava/lang/Object;ZIZ)Z", nativeInit),
        JNI_ROW("NativeCallback_DeviceEvent", "(IIZ)Z", nativeDevice),
        JNI_ROW("NativeCallback_KeyEvent", "(IIIIJ)Z", nativeKey),
        JNI_ROW("NativeCallback_MotionEvent", "(IIFFFFFFFFIJ)Z", nativeMotion),
        JNI_ROW("NativeCallback_MouseEvent", "(IFFFFIII)Z", nativeMouse),
        JNI_ROW("NativeCallback_PostInitUpdate", "(Landroid/view/Surface;II)V", nativeSurface),
        JNI_ROW("NativeCallback_LanguageSet", "(Ljava/lang/String;)V", nativeLanguage),
        JNI_ROW("NativeCallback_onPause", "()Z", nativePause),
        JNI_ROW("NativeCallback_onResume", "()Z", nativeResume),
        JNI_ROW("NativeCallback_InterruptionChanged", "(Z)Z", nativeInterrupt),
        JNI_ROW("NativeCallback_Cleanup", "()V", nativeCleanup),
        JNI_ROW("NativeCallback_ContentPurchased", "(Ljava/lang/String;)V", contentPurchased),
    };
    const jint result = env->RegisterNatives(cls, methods, jint(std::size(methods)));
    env->DeleteLocalRef(cls);
    if (result != JNI_OK || env->ExceptionCheck()) return false;
    cls = env->FindClass("com/tt2kgames/Borderlands2/LocalPlatform");
    if (!cls) return false;
    const JNINativeMethod platformMethods[] = {
        JNI_ROW("native_nvsiCancelVerification", "()V", platformNoop),
        JNI_ROW("native_nvsiForceRecheck", "(I)V", platformRecheckNoop),
        JNI_ROW("native_nvsiOnScreenOff", "()V", platformScreenOff),
    };
#undef JNI_ROW
    const jint platformResult = env->RegisterNatives(cls, platformMethods, jint(std::size(platformMethods)));
    env->DeleteLocalRef(cls);
    if (platformResult != JNI_OK || env->ExceptionCheck()) return false;
    __android_log_print(ANDROID_LOG_INFO, kTag, "Registered %zu native ARM32 activity adapters", std::size(methods));
    return true;
}
