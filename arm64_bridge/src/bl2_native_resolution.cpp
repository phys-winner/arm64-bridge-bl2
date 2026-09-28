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
#include <cmath>
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


// Authored UI_HUD.HUD is 1280x720. Its definition chooses ExactFit (2),
// which scales the same rotated crosshair bar differently on wide screens.
// ShowAll (1) is the native uniform fit for the whole HUD stage/safe area.
// Confirmed screen menus use the same native fit at arbitrary aspect ratios.
// Other modes, render targets, world signs and weapon scopes keep their policy.
constexpr uint32_t kHudScaleSetterRva = 0x01CB4BC4;
constexpr uint32_t kHudClassRva = 0x0306AF70;
constexpr uint32_t kHudScaleSetterWords[] = {
    0xE92D4008, 0xE590303C, 0xE3530000, 0x08BD8008,
    0xE5930034, 0xE5903000, 0xE593303C, 0xE12FFF33, 0xE8BD8008,
};
bool s_hud_scale_installed = false;  // Before any guest thread starts.
std::atomic<uint32_t> s_hud_scale_logs{0};

// Complete supplied definitions plus direct CDO player: 35 screen players.
// Native ShowAll uses each movie's own RECT and its real viewport. World mesh
// signs, ItemPickup render textures and the optical WeaponScope are excluded.
// PCContextMenuObject autoplays in Init; ShowFor remaps both Stage.visibleRect
// values and mouse positions. Frontend keeps its 4x3/16x9 frame and 3D matrices.
// Only known ExactFit(2), NoBorder(3), FitVertical(4) screen calls become ShowAll;
// NoScale(0), ShowAll(1), FitHorizontal(5) and unknown modes remain unchanged.
constexpr const char* kScreenFitClasses[] = {
    "BankGFxMovie",
    "CharacterSelectionReduxGFxMovie",
    "ConfirmCharacterLobbyGFxMovie",
    "CustomizationGFxMovie",
    "FastTravelStationGFxMovie",
    "FrontendGFxMovie",
    "GearboxAccountGFxMovie",
    "GearboxEULAGFxMovie",
    "HUDScaleGuideGFxMovie",
    "ItemInspectionGFxMovie",
    "LANServerBrowserGFxMovie",
    "LatentRewardGFxMovie",
    "LobbyGFxMovie",
    "MarketplaceGFxMovie",
    "MatchmakingLobbyGFxMovie",
    "NetworkOptionsGFxMovie",
    "OnlineMessageGFxMovie",
    "OptionsGFxMovie",
    "PauseGFxMovie",
    "PlayerDetailsGFxMovie",
    "QuestAcceptGFxMovie",
    "StashGFxMovie",
    "StatusMenuExGFxMovie",
    "TextChatGFxMovie",
    "TradingGFxMovie",
    "VehicleSpawnStationGFxMovie",
    "VendingMachineExGFxMovie",
    "WillowGFxDialogBox",
    "WillowGFxLobbyLoadCharacter",
    "WillowGFxMovieCredits",
    "WillowGFxMoviePressStart",
    "WillowGFxTrainingDialogBox",
    "WillowGFxTrainingDialogBox_NoPause",
    "WillowHUDGFxMovie",
    "PCContextMenuObject",
};
constexpr uint32_t kScreenNamesRva = 0x02FB00C8;
constexpr uint32_t kMoviePlayerClassRva = 0x0302F930;

const char* ScreenFitClassName(DynarmicHost& host, uint32_t cls) {
    const auto valid = [&host](uint32_t p, size_t size) {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() &&
            size <= host.GetGuestMemorySize() - p;
    };
    if (!valid(cls, 0x38) || host.MemoryRead32(cls + 0x30) != 0) return nullptr;
    const auto names = host.GetElfLoader().GetBaseAddress() + kScreenNamesRva;
    const auto table = host.MemoryRead32(names);
    const auto count = host.MemoryRead32(names + 4);
    const auto capacity = host.MemoryRead32(names + 8);
    const auto index = host.MemoryRead32(cls + 0x2C);
    if (!count || count > capacity || capacity > 2000000 || index >= count ||
        !valid(table, size_t(count) * 4)) return nullptr;
    const auto entry = host.MemoryRead32(table + index * 4);
    if (!valid(entry, 72)) return nullptr;
    char name[64] = {};
    for (size_t i = 0; i < sizeof(name); ++i) {
        const auto ch = host.MemoryRead8(entry + 8 + uint32_t(i));
        if (!ch) {
            for (const auto* wanted : kScreenFitClasses)
                if (std::strcmp(name, wanted) == 0) return wanted;
            return nullptr;
        }
        if (ch < 32 || ch > 126) return nullptr;
        name[i] = static_cast<char>(ch);
    }
    return nullptr;
}

const char* ScreenFitMovie(DynarmicHost& host, uint32_t object, uint32_t& cls) {
    const auto valid = [&host](uint32_t p, size_t size) {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() &&
            size <= host.GetGuestMemorySize() - p;
    };
    cls = 0;
    if (!valid(object, 0xD0)) return nullptr;
    const auto movie = host.MemoryRead32(object + 0x3C);
    // FGFxEngine::StartScene stores the real render target at movie+104.
    // PostMovieStart calls this setter after StartScene. Refuse unloaded/RT
    // instances, including preview clones, instead of fitting their textures.
    if (!valid(movie, 112) || !host.MemoryRead32(movie + 72) ||
        host.MemoryRead32(movie + 104) != 0 ||
        host.MemoryRead32(object + 204) != 0) return nullptr;
    cls = host.MemoryRead32(object + 0x34);
    const auto wanted = host.MemoryRead32(
        host.GetElfLoader().GetBaseAddress() + kMoviePlayerClassRva);
    if (!wanted || !valid(wanted, 0x44)) return nullptr;
    const char* screen = nullptr;
    auto current = cls;
    for (unsigned depth = 0; depth < 48 && current; ++depth) {
        if (!valid(current, 0x44)) return nullptr;
        if (current == wanted) return screen;
        if (!screen) screen = ScreenFitClassName(host, current);
        const auto parent = host.MemoryRead32(current + 0x40);
        if (parent == current) return nullptr;
        current = parent;
    }
    return nullptr;
}


// Set3DMatrices fits its UI plane to height independently of the 2D ScaleMode.
// Before the native center/half-size VMUL, S22/S23 are the movie's own RECT,
// S28/S29 are its actual viewport and S16=movieHeight*fullRegionHeight.
// S16 is read only by the distance divide at +0x25C, then reset to 1.0.
// A larger UI-only distance uniformly fits narrow screens. Native camera FOV,
// view offsets, projection generation and the matching hit matrices continue.
constexpr uint32_t kScreen3DClassRva = 0x0306AB2C;
constexpr uint32_t kScreen3DPointRva = 0x02230900;
constexpr uint32_t kScreen3DWords[] = {
    0xEE2BBA27, 0xEE736AA3, 0xE59280D0, 0xEE6BBAA7,
    0xE50BC0AC, 0xE592C0C4, 0xEEB17A47, 0xEEC86A26,
};
std::atomic<uint32_t> s_screen_3d_logs{0};

bool ApplyScreen3DFit(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    const auto object = jit.Regs()[6];
    uint32_t cls = 0;
    const auto* screen = ScreenFitMovie(host, object, cls);
    if (!screen || std::strcmp(screen, "WillowHUDGFxMovie") == 0 ||
        std::strcmp(screen, "WillowGFxMovieCredits") == 0 ||
        std::strcmp(screen, "LatentRewardGFxMovie") == 0 ||
        std::strcmp(screen, "MatchmakingLobbyGFxMovie") == 0) return false;
    const auto valid = [&host](uint32_t p, size_t size) {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() &&
            size <= host.GetGuestMemorySize() - p;
    };
    if (!valid(object, 636)) return false;
    const auto wanted = host.MemoryRead32(
        host.GetElfLoader().GetBaseAddress() + kScreen3DClassRva);
    if (!wanted || !valid(wanted, 0x44)) return false;
    auto current = cls;
    for (unsigned depth = 0; depth < 48 && current != wanted; ++depth) {
        if (!valid(current, 0x44)) return false;
        const auto parent = host.MemoryRead32(current + 0x40);
        if (parent == current) return false;
        current = parent;
    }
    if (current != wanted) return false;
    const auto definition = host.MemoryRead32(object + 612);
    if (!valid(definition, 240) || jit.Regs()[2] != definition ||
        host.MemoryRead32(object + 620) != 0 ||
        host.MemoryRead32(object + 624) != 0 ||
        host.MemoryRead32(object + 628) != 0x3F800000 ||
        host.MemoryRead32(object + 632) != 0x3F800000) return false;
    auto& ext = jit.ExtRegs();
    const float stage_w = FloatFromBits(ext[22]);
    const float stage_h = FloatFromBits(ext[23]);
    const float viewport_w = FloatFromBits(ext[28]);
    const float viewport_h = FloatFromBits(ext[29]);
    const float aspect = FloatFromBits(ext[30]);
    const float height = FloatFromBits(ext[16]);
    const auto dimension = [](float v) {
        return std::isfinite(v) && v >= 1.0f && v <= 16384.0f;
    };
    if (!dimension(stage_w) || !dimension(stage_h) ||
        !dimension(viewport_w) || !dimension(viewport_h) ||
        !dimension(height) || !std::isfinite(aspect) || aspect <= 0.0f ||
        std::fabs(aspect - viewport_w / viewport_h) > aspect * 0.0001f ||
        std::fabs(height - stage_h) > stage_h * 0.0001f) return false;
    const float factor = (stage_w * viewport_h) / (stage_h * viewport_w);
    if (!std::isfinite(factor) || factor <= 1.0f || factor > 32.0f) return false;
    const float fitted_height = height * factor;
    if (!std::isfinite(fitted_height) || fitted_height > 524288.0f) return false;
    std::memcpy(&ext[16], &fitted_height, sizeof(fitted_height));
    if (s_screen_3d_logs.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOGI("UI3D fit class=%s object=%08X stage=%.3fx%.3f "
             "viewport=%.3fx%.3f distance_factor=%.6f height=%.3f->%.3f",
             screen, object, stage_w, stage_h, viewport_w, viewport_h,
             factor, height, fitted_height);
    }
    return true;
}

bool InstallHudScalePolicy(DynarmicHost& host) {
    if (s_hud_scale_installed) return true;
    auto& loader = host.GetElfLoader();
    const auto base = loader.GetBaseAddress();
    const auto setter = base + kHudScaleSetterRva;
    if (!s_input_verified || loader.GetImageSize() != kImageSize ||
        uint64_t(base) + kImageSize > host.GetGuestMemorySize() ||
        loader.GetSymbolAddress("_ZN15UGFxMoviePlayer16SetViewScaleModeEh") != setter ||
        loader.GetSymbolAddress("_ZN18UWillowHUDGFxMovie18PrivateStaticClassE") != base + kHudClassRva ||
        loader.GetSymbolAddress("_ZN15UGFxMoviePlayer18PrivateStaticClassE") != base + kMoviePlayerClassRva ||
        loader.GetSymbolAddress("_ZN5FName5NamesE") != base + kScreenNamesRva ||
        loader.GetSymbolAddress("_ZN17UWillowGFxMovie3D13Set3DMatricesEv") != base + 0x022306C0 ||
        loader.GetSymbolAddress("_ZN17UWillowGFxMovie3D18PrivateStaticClassE") != base + kScreen3DClassRva) {
        LOGE("UI fit refused: image/symbol guard mismatch");
        return false;
    }
    for (size_t i = 0; i < std::size(kHudScaleSetterWords); ++i) {
        if (host.MemoryRead32(setter + uint32_t(i * 4)) != kHudScaleSetterWords[i]) {
            LOGE("UI fit refused: setter preimage mismatch at +%zu", i * 4);
            return false;
        }
    }
    const auto point3d = base + kScreen3DPointRva;
    for (size_t i = 0; i < std::size(kScreen3DWords); ++i) {
        if (host.MemoryRead32(point3d + uint32_t(i * 4)) != kScreen3DWords[i]) {
            LOGE("UI3D fit refused: live-point preimage mismatch at +%zu", i * 4);
            return false;
        }
    }
    const auto veneer = host.GuestAlloc(20, 4);
    if (!veneer) return false;
    const auto thunk = host.GetThunkManager().RegisterThunk("bl2_hud_uniform_fit",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& currentHost) {
            uint32_t cls = 0;
            const auto incoming = jit.Regs()[1];
            if (incoming != 2 && incoming != 3 && incoming != 4) return;
            const auto* screen = ScreenFitMovie(currentHost, jit.Regs()[0], cls);
            if (!screen) return;
            jit.Regs()[1] = 1;
            if (s_hud_scale_logs.fetch_add(1, std::memory_order_relaxed) < 32) {
                LOGI("UI fit setter class=%s object=%08X uclass=%08X "
                     "incoming=%u effective=%u safe_area=uniform-authored-stage",
                     screen, jit.Regs()[0], cls, incoming, jit.Regs()[1]);
            }
        });
    if (!thunk) return false;
    const auto svc = host.MemoryRead32(thunk);
    if ((svc & 0xFF000000u) != 0xEF000000u) return false;
    // Preserve both PC-independent displaced instructions, then resume the
    // original setter. This is its normal startup engine-thread call boundary.
    host.MemoryWrite32(veneer, svc);
    host.MemoryWrite32(veneer + 4, kHudScaleSetterWords[0]);
    host.MemoryWrite32(veneer + 8, kHudScaleSetterWords[1]);
    host.MemoryWrite32(veneer + 12, 0xE51FF004);
    host.MemoryWrite32(veneer + 16, setter + 8);
    host.AddExecutableRange(veneer, 20);
    host.InvalidateGuestCode(veneer, 20);
    const auto veneer3d = host.GuestAlloc(20, 4);
    if (!veneer3d) return false;
    const auto thunk3d = host.GetThunkManager().RegisterThunk("bl2_screen_3d_uniform_fit",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& currentHost) {
            ApplyScreen3DFit(jit, currentHost);
        });
    if (!thunk3d) return false;
    const auto svc3d = host.MemoryRead32(thunk3d);
    if ((svc3d & 0xFF000000u) != 0xEF000000u) return false;
    host.MemoryWrite32(veneer3d, svc3d);
    host.MemoryWrite32(veneer3d + 4, kScreen3DWords[0]);
    host.MemoryWrite32(veneer3d + 8, kScreen3DWords[1]);
    host.MemoryWrite32(veneer3d + 12, 0xE51FF004);
    host.MemoryWrite32(veneer3d + 16, point3d + 8);
    host.AddExecutableRange(veneer3d, 20);
    host.InvalidateGuestCode(veneer3d, 20);
    // Both veneers, thunks and SVCs are prepared before either live point changes.
    host.MemoryWrite32(setter, 0xE51FF004);
    host.MemoryWrite32(setter + 4, veneer);
    host.InvalidateGuestCode(setter, 8);
    if (host.MemoryRead32(setter) != 0xE51FF004 ||
        host.MemoryRead32(setter + 4) != veneer) return false;
    host.MemoryWrite32(point3d, 0xE51FF004);
    host.MemoryWrite32(point3d + 4, veneer3d);
    host.InvalidateGuestCode(point3d, 8);
    if (host.MemoryRead32(point3d) != 0xE51FF004 ||
        host.MemoryRead32(point3d + 4) != veneer3d) return false;
    LOGI("UI3D fit installed: point=%08X full-region screen menus only logs<=32", point3d);
    s_hud_scale_installed = true;
    LOGI("UI fit policy installed: setter=%08X screen modes=2,3,4 ShowAll=1 logs<=32", setter);
    return true;
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
    if (!InstallHudScalePolicy(host)) return false;
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
