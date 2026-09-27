#include "bl2_native_resolution.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "obb_sha256.h"
#include "thunk_manager.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>

#define LOG_TAG "Arm32Bridge_Resolution"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {

constexpr char kPolicy[] = "bl2-native-surface-v1";
constexpr char kGuestSha256[] =
    "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
constexpr size_t kGuestFileBytes = 49885152;
constexpr uint32_t kImageSize = 0x03089000;
constexpr uint32_t kLaunchRva = 0x01E2EEE8;
constexpr uint32_t kCommitRva = 0x01E2EFA0;
constexpr uint32_t kSystemSettingsRva = 0x02FFEE6C;
constexpr uint32_t kSceneTargetsRva = 0x03013560;

// An arithmetic bound for this 32-bit renderer, not a claim about GPU support.
// A 16384^2 RGBA buffer still fits a signed 32-bit byte count. Actual texture,
// renderbuffer and viewport limits are queried from each real GL context.
constexpr int32_t kMaximumDimension = 16384;

struct Patch {
    uint32_t rva;
    uint32_t original;
    uint32_t replacement;
};
constexpr Patch kPatches[] = {
    {0x01E2EFA0, 0xEE053A90, 0xE1A0B003},  // mov r11,r3 (incoming width)
    {0x01E2EFB4, 0xE344E2C8, 0xE344E120},  // movt lr,#0x4120 (float 10%)
    {0x01E2EFC4, 0xEEB87AE5, 0xE59DA120},  // ldr r10,[sp,#0x120] (height)
    {0x01E2EFF4, 0xEE867A07, 0xEEB77A00},  // vmov.f32 s14,#1.0
    {0x01E2F008, 0xEEC67AA7, 0xEEF77A00},  // vmov.f32 s15,#1.0
};

// Exact common-commit preimage through the conditional branch. Besides the
// four edited words, guard all stores, source registers and the cmp whose
// flags must survive the new VFP moves. SHA below pins the full input too.
constexpr uint32_t kCommitPreimage[] = {
    0xEE053A90, 0xE59F020C, 0xE59F520C, 0xE3A0E000,
    0xE59F1208, 0xE344E2C8, 0xE59F2204, 0xE7963000,
    0xE59F0200, 0xEEB87AE5, 0xEDDD5A48, 0xE08F1001,
    0xE5914040, 0xE583B408, 0xE283CB01, 0xE583A40C,
    0xE7963005, 0xE08F0000, 0xEEF87AE5, 0xE581B038,
    0xE3540000, 0xEE867A07, 0xE583B000, 0xE7962002,
    0xE58CE000, 0xE581A03C, 0xEEC67AA7, 0xE582A000,
    0xED807A01, 0xEDC07A02, 0x0A00008F,
};

struct Global {
    const char* name;
    uint32_t rva;
};
constexpr Global kGlobals[] = {
    {"GSystemSettings", kSystemSettingsRva},
    {"GScreenWidth", 0x02F3C2D4}, {"GScreenHeight", 0x02F3C2D0},
    {"GRealScreenWidth", 0x03058B38}, {"GRealScreenHeight", 0x03058B3C},
    {"GScaleformMouseScaleX", 0x02F63EA0},
    {"GScaleformMouseScaleY", 0x02F63EA4},
    {"GSceneRenderTargets", kSceneTargetsRva},
};

bool s_input_verified = false;  // Written/read only during serialized loading.
std::atomic<bool> s_installed{false};
std::atomic<uint64_t> s_surface_dimensions{0};
std::atomic<uint32_t> s_surface_generation{0};
std::atomic<uint64_t> s_gl_limits{0};
bool s_ten_percent_diagnostic = false;  // Set before guest execution starts.

uint64_t PackDimensions(uint32_t width, uint32_t height) {
    return (static_cast<uint64_t>(width) << 32) | height;
}

bool DimensionsValid(int32_t width, int32_t height) {
    return width > 0 && height > 0 && width <= kMaximumDimension &&
           height <= kMaximumDimension;
}

bool FitsKnownLimits(int32_t width, int32_t height) {
    const uint64_t limits = s_gl_limits.load(std::memory_order_acquire);
    // FSceneRenderTargets::Allocate rounds each buffer dimension up to eight.
    const uint32_t target_width = (static_cast<uint32_t>(width) + 7u) & ~7u;
    const uint32_t target_height = (static_cast<uint32_t>(height) + 7u) & ~7u;
    return limits == 0 ||
        (target_width <= static_cast<uint32_t>(limits >> 32) &&
         target_height <= static_cast<uint32_t>(limits));
}

uint32_t PatchedWord(uint32_t rva, uint32_t original) {
    for (const auto& patch : kPatches) {
        if (patch.rva == rva) {
            if (rva == 0x01E2EFB4 && !s_ten_percent_diagnostic) return original;
            return patch.replacement;
        }
    }
    return original;
}

float FloatFromBits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

bool VerifyBl2NativeResolutionInput(const uint8_t* data, size_t size) {
    s_input_verified = false;
    if (!data || size != kGuestFileBytes) {
        LOGE("%s input refused: bytes=%zu expected=%zu", kPolicy, size,
             kGuestFileBytes);
        return false;
    }
    ObbSha256 digest;
    if (!digest.Update(data, size)) return false;
    const auto hex = ObbSha256::Hex(digest.Finalize());
    s_input_verified = std::strcmp(hex.data(), kGuestSha256) == 0;
    if (!s_input_verified) {
        LOGE("%s input refused: sha256=%s", kPolicy, hex.data());
        return false;
    }
    LOGI("%s input verified: bytes=%zu sha256=%s", kPolicy, size, hex.data());
    return true;
}

bool InstallBl2NativeResolutionPatch(DynarmicHost& host) {
    char render_scale[PROP_VALUE_MAX] = {};
    s_ten_percent_diagnostic =
        __system_property_get("debug.bl2.render_scale", render_scale) > 0 &&
        std::strcmp(render_scale, "10") == 0;
    auto& loader = host.GetElfLoader();
    const uint32_t base = loader.GetBaseAddress();
    const uint32_t launch = loader.GetSymbolAddress(
        "_Z22PlatformLaunchRendererP7_JNIEnvP8_jobjectS2_ii");
    if (!s_input_verified || loader.GetImageSize() != kImageSize ||
        static_cast<uint64_t>(base) + kImageSize > host.GetGuestMemorySize() ||
        launch != base + kLaunchRva ||
        host.MemoryRead32(launch) != 0xE92D4FF0 ||
        host.MemoryRead32(launch + 4) != 0xE24DD0FC) {
        LOGE("%s patch refused: image/symbol/prologue signature mismatch", kPolicy);
        return false;
    }
    for (const auto& global : kGlobals) {
        if (loader.GetSymbolAddress(global.name) != base + global.rva) {
            LOGE("%s patch refused: global %s RVA mismatch", kPolicy, global.name);
            return false;
        }
    }
    bool original = true;
    bool active = true;
    for (size_t i = 0; i < std::size(kCommitPreimage); ++i) {
        const uint32_t rva = kCommitRva + static_cast<uint32_t>(i * 4);
        const uint32_t word = host.MemoryRead32(base + rva);
        original &= word == kCommitPreimage[i];
        active &= word == PatchedWord(rva, kCommitPreimage[i]);
    }
    if (!original && !active) {
        LOGE("%s patch refused: common-commit preimage mismatch", kPolicy);
        return false;
    }
    // Startup-only escape hatch for a controlled comparison of an unrelated
    // renderer fix. Native 100% is the default. Never rewrite live guest code
    // back to legacy dimensions after renderer threads have started.
    char legacy[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.legacy_resolution", legacy) > 0 &&
        std::strcmp(legacy, "1") == 0) {
        if (!original) {
            LOGE("%s legacy diagnostic refused: patch already active", kPolicy);
            return false;
        }
        LOGI("%s DIAGNOSTIC legacy resolution retained by "
             "debug.bl2.legacy_resolution=1; native policy disabled this process",
             kPolicy);
        return true;
    }
    if (!active) {
        // Called before JNI_OnLoad and before the Java renderer thread starts.
        // All signatures passed before any of the four code words is changed.
        for (const auto& patch : kPatches) {
            const uint32_t desired = PatchedWord(patch.rva, patch.original);
            if (desired != patch.original) host.MemoryWrite32(base + patch.rva, desired);
        }
        host.InvalidateGuestCode(base + kCommitRva, sizeof(kCommitPreimage));
    }
    for (size_t i = 0; i < std::size(kCommitPreimage); ++i) {
        const uint32_t rva = kCommitRva + static_cast<uint32_t>(i * 4);
        if (host.MemoryRead32(base + rva) !=
            PatchedWord(rva, kCommitPreimage[i])) {
            LOGE("%s patch write verification failed at RVA=0x%08X", kPolicy, rva);
            return false;
        }
    }
    s_installed.store(true, std::memory_order_release);
    LOGI("%s active: launch=0x%08X commit=0x%08X words=%u "
         "screen_percentage=%u%% source=SurfaceHolder gfXScale=1,1",
         kPolicy, launch, base + kCommitRva,
         s_ten_percent_diagnostic ? 5u : 4u,
         s_ten_percent_diagnostic ? 10u : 100u);
    return true;
}

bool PrepareBl2NativeResolutionSurface(JNIEnv* env, jobject surface,
                                       jint& width, jint& height) {
    if (!surface || env->ExceptionCheck()) {
        LOGE("%s surface deferred: null Surface or pending JNI exception", kPolicy);
        return false;
    }
    // This callback is typed Surface. NativeCallback_Init's generic jobject is
    // an AssetManager and must never be passed to ANativeWindow_fromSurface.
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    const int32_t native_width = window ? ANativeWindow_getWidth(window) : 0;
    const int32_t native_height = window ? ANativeWindow_getHeight(window) : 0;
    if (window) ANativeWindow_release(window);
    if (!window || env->ExceptionCheck()) {
        LOGE("%s surface deferred: no usable native window", kPolicy);
        return false;
    }
    const jint callback_width = width;
    const jint callback_height = height;
    const char* source = "SurfaceHolder";
    if (!DimensionsValid(width, height)) {
        // UE3JavaApp is sensorLandscape and normalizes the callback by max/min.
        // Use the same orientation when recovering an invalid callback pair.
        width = std::max(native_width, native_height);
        height = std::min(native_width, native_height);
        source = "ANativeWindow-recovery";
    }
    if (!DimensionsValid(width, height) || !FitsKnownLimits(width, height)) {
        const uint64_t limits = s_gl_limits.load(std::memory_order_acquire);
        LOGE("%s surface deferred: callback=%dx%d native=%dx%d selected=%dx%d "
             "known_gl_limit=%ux%u; waiting for valid SurfaceChanged",
             kPolicy, callback_width, callback_height, native_width, native_height,
             width, height, static_cast<uint32_t>(limits >> 32),
             static_cast<uint32_t>(limits));
        return false;
    }
    s_surface_dimensions.store(PackDimensions(width, height),
                               std::memory_order_release);
    const uint32_t generation =
        s_surface_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    LOGI("%s surface: callback=%dx%d native=%dx%d selected=%dx%d source=%s "
         "generation=%u",
         kPolicy, callback_width, callback_height, native_width, native_height,
         width, height, source, generation);
    return true;
}

void LogBl2NativeResolutionState(DynarmicHost& host, const char* event) {
    if (!s_installed.load(std::memory_order_acquire)) return;
    const uint32_t base = host.GetElfLoader().GetBaseAddress();
    const uint64_t dimensions = s_surface_dimensions.load(std::memory_order_acquire);
    LOGI("%s state event=%s expected=%ux%u settings=%ux%u percentage=%.3f "
         "screen=%ux%u real=%ux%u gfx_mouse=%.6f,%.6f virtual=%ux%u "
         "scene_buffer=%ux%u",
         kPolicy, event, static_cast<uint32_t>(dimensions >> 32),
         static_cast<uint32_t>(dimensions),
         host.MemoryRead32(base + kSystemSettingsRva + 0x408),
         host.MemoryRead32(base + kSystemSettingsRva + 0x40C),
         FloatFromBits(host.MemoryRead32(base + kSystemSettingsRva + 0x400)),
         host.MemoryRead32(base + 0x02F3C2D4), host.MemoryRead32(base + 0x02F3C2D0),
         host.MemoryRead32(base + 0x03058B38), host.MemoryRead32(base + 0x03058B3C),
         FloatFromBits(host.MemoryRead32(base + 0x02F63EA0)),
         FloatFromBits(host.MemoryRead32(base + 0x02F63EA4)),
         ThunkManager::s_virtualWidth, ThunkManager::s_virtualHeight,
         host.MemoryRead32(base + kSceneTargetsRva + 0xE0),
         host.MemoryRead32(base + kSceneTargetsRva + 0xE4));
}

void CheckBl2NativeResolutionLimits(DynarmicHost& host) {
    if (!s_installed.load(std::memory_order_acquire)) return;
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return;
    const uint64_t dimensions = s_surface_dimensions.load(std::memory_order_acquire);
    const uint32_t generation = s_surface_generation.load(std::memory_order_acquire);
    thread_local EGLContext last_context = EGL_NO_CONTEXT;
    thread_local uint64_t last_dimensions = 0;
    thread_local uint32_t last_generation = 0;
    if (context == last_context && dimensions == last_dimensions &&
        generation == last_generation) return;
    const uint32_t width = static_cast<uint32_t>(dimensions >> 32);
    const uint32_t height = static_cast<uint32_t>(dimensions);
    const uint32_t target_width = (width + 7u) & ~7u;
    const uint32_t target_height = (height + 7u) & ~7u;
    GLint texture = 0, renderbuffer = 0, viewport[2] = {};
    // All three are core queries in the real GLES 3.2 context. Do not consume
    // glGetError here: a pending guest error must remain visible to its caller.
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &renderbuffer);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport);
    const GLint max_width = std::min({texture, renderbuffer, viewport[0]});
    const GLint max_height = std::min({texture, renderbuffer, viewport[1]});
    const bool supported = width > 0 && height > 0 && max_width > 0 &&
        max_height > 0 && target_width <= static_cast<uint32_t>(max_width) &&
        target_height <= static_cast<uint32_t>(max_height);
    LOGI("%s GL limits context=%p requested=%ux%u allocation=%ux%u "
         "texture=%d renderbuffer=%d "
         "viewport=%dx%d supported=%d", kPolicy, context, width, height,
         target_width, target_height, texture, renderbuffer, viewport[0],
         viewport[1], supported);
    if (max_width > 0 && max_height > 0) {
        s_gl_limits.store(PackDimensions(max_width, max_height),
                          std::memory_order_release);
    }
    if (!supported) {
        LOGE("%s LIMIT_GUARD: native resolution exceeds real GL limits; "
             "stopping guest renderer before target allocation", kPolicy);
        host.HaltCurrentThreadExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    last_context = context;
    last_dimensions = dimensions;
    last_generation = generation;
    LogBl2NativeResolutionState(host, "context-ready");
}

void AuditBl2NativeResolutionPresent(DynarmicHost& host) {
    if (!s_installed.load(std::memory_order_acquire)) return;
    const uint64_t dimensions = s_surface_dimensions.load(std::memory_order_acquire);
    const uint32_t generation = s_surface_generation.load(std::memory_order_acquire);
    thread_local uint32_t last_generation = 0;
    thread_local uint32_t presents = 0;
    if (generation != last_generation) {
        last_generation = generation;
        presents = 0;
    }
    if (presents >= 60) return;
    ++presents;
    if (presents != 1 && presents != 60) return;
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    EGLint width = 0, height = 0;
    const bool queried = display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE &&
        eglQuerySurface(display, surface, EGL_WIDTH, &width) &&
        eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    GLint viewport[4] = {}, framebuffer = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    LOGI("%s present=%u generation=%u expected=%ux%u egl=%dx%d queried=%d "
         "viewport=%d,%d,%d,%d draw_fbo=%d", kPolicy, presents,
         generation,
         static_cast<uint32_t>(dimensions >> 32), static_cast<uint32_t>(dimensions),
         width, height, queried, viewport[0], viewport[1], viewport[2], viewport[3],
         framebuffer);
    LogBl2NativeResolutionState(host, "present");
}
