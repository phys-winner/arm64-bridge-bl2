#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <cerrno>
#include <dirent.h>
#include <ftw.h>
#include <string_view>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <cstdlib>
#include <cstring>
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "horn_fastpaths.h"
#include "thunk_manager.h"
#include "jni_fake_env.h"
#include "thunks_opensles.h"
#include "thunks_tegra.h"
#include "bl2_native_resolution.h"
#include "bl2_graphics_settings.h"
#include "bl2_fresh_buffer_names.h"
#include "bl2_pause_debug_menu.h"
#include "bl2_vertex_element_fastpath.h"
#include "bl2_convex_fastpath.h"
#include "bl2_limiter_peak_fastpath.h"
#include "bl2_program_locals_fastpath.h"
#include "bl2_perf_hint.h"
#include "bl2_hitch_report.h"
#include "bl2_frame_pacer.h"
#include "bl2_performance_settings.h"
#include "bl2_audio_decode_fastpath.h"
#include "bl2_guest_integrity.h"
#include "bl2_touch_runtime.h"
#include "bl2_io_failure_probe.h"
#include "bl2_movie_skip.h"
#include "bl2_startup_screens.h"
#if HORN_SILENT_LOGCAT
#include "bl2_silent_log.h"
#endif
#include "bl2_touch_physical.h"
#include "bl2_locale.h"
#include "bl2_shader_stage_cache.h"
#include "bl2_shader_former.h"
#include "bl2_touch_settings.h"
#include "engine_hooks.h"
#include "obb_pair_selection.h"

#define LOG_TAG "Arm32Bridge_JNI"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

// ===========================================================================
// PER-GAME CONFIGURATION -- this block and the JNINativeMethod tables at the
// bottom are the only parts of this file that a new port has to rewrite.
//
// Fill it during the initial analysis of the guest APK:
//   unzip -l <apk>                          -> which .so files exist
//   readelf -d <guest.so> | grep NEEDED     -> which of them must be loaded too
//   strings/IDA on the guest                -> the Java classes it calls up into
// ===========================================================================
namespace cfg {

// Main guest library, as named inside the APK.
constexpr const char* kGuestLib = "libUnrealEngine3.so";

// Extra guest libraries loaded next to it (FMOD Ex ships as a pair). Leave the
// entries empty when the game has none; the loader then takes the single-lib path.
constexpr const char* kGuestLibAux1 = "";
constexpr const char* kGuestLibAux2 = "";

// Java classes carrying native methods. Every name here must exist in the APK
// or RegisterNatives silently registers nothing (see LESSONS 6.7).
constexpr const char* kActivityClass = "com/tt2kgames/Borderlands2/UE3JavaApp";
constexpr const char* kNvsiClass = "com/tt2kgames/Borderlands2/LocalPlatform";
constexpr const char* kZipClass =
    "com/android/vending/expansion/zipfile/ZipResourceFile";
constexpr const char* kGuestJniOnLoad = "JNI_OnLoad";
constexpr const char* kCalcSceneView =
    "_ZN12ULocalPlayer13CalcSceneViewEP16FSceneViewFamilyR7FVectorR8FRotatorP9FViewportP18FViewElementDrawer";
constexpr const char* kEngineGetMaxTickRate =
    "_ZN7UEngine14GetMaxTickRateEfj";
constexpr const char* kBl2GameGetMaxTickRate =
    "_ZN11UGameEngine14GetMaxTickRateEfj";
constexpr const char* kMobileSystemSettingsSelector =
    "_Z37appGetMobileSystemSettingsSectionNamev";
constexpr const char* kGameGetCurrentPlatform =
    "_ZN19APhosphorMobileBase18GetCurrentPlatformEv";
constexpr const char* kSystemSettingsGlobal = "GSystemSettings";

// Every shipped Coalesced_*.bin selects AspectRatio_MajorAxisFOV. In landscape
// that keeps horizontal FOV and crops the top and bottom (Vert-). Horn's camera
// values are authored as horizontal FOV at 16:9, so raw MaintainYFOV is also
// wrong: it interprets the same number as vertical FOV and exposes unrendered
// scene edges. The runtime patch below selects UE3's MaintainY path and scales
// both projection multipliers by 16/9. This preserves the authored 16:9
// vertical framing and expands only the sides (reference-aspect Hor+).
//
// Only loaded guest memory is changed; the ARM32 ELF and OBBs on disk remain
// byte-for-byte identical.
struct GuestInstructionPatch {
    uint32_t offset;
    uint32_t expected;
    uint32_t replacement;
};

constexpr GuestInstructionPatch kMaintainYFovPatches[] = {
    {0x6FC, 0x95D53378, 0x93A03000},  // ldrbls r3,[r5,#0x378] -> movls r3,#0
    {0x708, 0xE5D53378, 0xE3A03000},  // ldrb   r3,[r5,#0x378] -> mov   r3,#0
};

constexpr uint32_t kProjectionScaleOffset = 0x728;
constexpr uint32_t kProjectionScaleExpectedInstruction = 0xEEC76A27;  // vdiv.f32 s13,s14,s15
constexpr uint32_t kGuestImageSize = 0x01848000;
constexpr uint32_t kGuestBssEndOffset = 0x01847AD0;
constexpr uint32_t kAndroidLocaleSelectionFlagOffset = 0x0183B7F4;
constexpr uint32_t kWidescreenCaveSize = 0x20;
constexpr uint32_t kReferenceAspect16By9 = 0x3FE38E39;  // 1.7777778f
constexpr uint32_t kAspectCaveWordCount = 5;

// The cave replays the displaced division, loads 16/9, scales MultFOVX, then
// returns. s12 keeps 16/9 and is subsequently passed as MultFOVY.
constexpr uint32_t kCaveDivide = 0xEEC76A27;       // vdiv.f32 s13,s14,s15
constexpr uint32_t kCaveLoadAspect = 0xED9F6A01;   // vldr s12,[pc,#4]
constexpr uint32_t kCaveScaleX = 0xEE666A86;       // vmul.f32 s13,s13,s12

// Horn's runtime config enables UE3 frame smoothing at 22..62 FPS. Android's
// window vote already selects the panel's highest supported refresh rate, so
// retain smoothing but raise its upper bound to panelHz+2 on high-refresh
// devices. EGL/SF remains the final device-specific pacing boundary. The last
// 12 bytes of the existing zero-filled runtime cave hold a load/return pair and
// the dynamic IEEE-754 limit; the guest ELF on disk is never modified.
constexpr uint32_t kMaxSmoothedRateLoadOffset = 0x90;
constexpr uint32_t kMaxSmoothedRateExpectedInstruction = 0xEDD46A00;  // vldr s13,[r4]
constexpr uint32_t kRefreshCaveSize = 0x0C;
constexpr uint32_t kRefreshCaveLoad = 0xEDDF6A00;  // vldr s13,[pc,#0]
constexpr float kShippedMaxSmoothedRate = 62.0f;
constexpr float kSmoothedRateHeadroom = 2.0f;
constexpr float kMaximumSaneDisplayRate = 1000.0f;

// BL2's UGameEngine override is reached by appUpdateTimeAndHandleMaxTickRate.
// This replaces only its returned soft-float value. The embedded literal is
// written before the two instructions, while guest execution is not running.
constexpr uint32_t kBl2MaxTickRateRva = 0x01460570;
constexpr uint32_t kBl2MaxTickRateExpected[] = {
    0xE92D4070,  // push {r4,r5,r6,lr}
    0xE1A06000,  // mov r6,r0
    0xE59F50E0,  // ldr r5,[pc,#0xe0]
};
constexpr uint32_t kBl2MaxTickRateLoad = 0xE59F0000;  // ldr r0,[pc,#0]
constexpr uint32_t kBl2MaxTickRateReturn = 0xE12FFF1E;  // bx lr
constexpr float kBl2DiagnosticFpsLimit = 900.0f;
// While presents are paced, the user's limit is enforced as whole display
// refreshes on the render thread. The game thread's own sleep limit then sits
// this much above it, so the two clocks never beat against each other.
constexpr float kBl2PacedTickHeadroom = 1.1f;

// Horn ships the full Tegra texture/content set, but its 2013 device whitelist
// sends every unknown modern Android device to SystemSettingsAndroid. That
// profile inherits SystemSettingsMobileTextureBias and discards 3 world mips
// plus 7 normal/specular mips. Tegra 3/4 instead uses LODBias=0 throughout.
// Keep the real GL/Java device identity so no Tegra-only driver assumptions are
// enabled; patch only the two high-level policy selectors used by UE3 config and
// Horn's UnrealScript content branches.
constexpr uint32_t kTegra4ProfileNameRva = 0x015EF7AC;
constexpr char kTegra4ProfileName[] = "SystemSettingsAndroid_Tegra4";
constexpr uint32_t kTegra4PlatformEnum = 5;
constexpr uint32_t kArmLdrR0FromPc = 0xE59F0000;  // ldr r0,[pc,#0]
constexpr uint32_t kArmBxLr = 0xE12FFF1E;         // bx lr
constexpr uint32_t kArmMovR0Tegra4 = 0xE3A00005;  // mov r0,#5
constexpr uint32_t kSettingsSelectorExpected[] = {
    0xE92D4010,  // push {r4,lr}
    0xE59F40EC,  // ldr r4,[pc,#0xec]
    0xE59F30EC,  // ldr r3,[pc,#0xec]
};
constexpr uint32_t kPlatformSelectorExpected[] = {
    0xE92D4010,  // push {r4,lr}
    0xE59F4064,  // ldr r4,[pc,#0x64]
};


}  // namespace cfg

std::unique_ptr<DynarmicHost> g_host;
JavaVM* g_jvm = nullptr;
JavaVM* GetHostJavaVM() { return g_jvm; }
std::string g_saved_apk_path;
// Inventory only until IndexApk can validate and select a whole main+patch pair.
std::vector<std::string> g_saved_split_apk_paths;
std::string g_saved_files_dir;
// Persisted GL binaries use internal app storage, independently of save paths.
std::string g_bl2_shader_cache_dir;
static bool g_split_source_dirs_captured = false;
static bool g_shader_cache_dir_capture_attempted = false;
static bool g_shader_assets_capture_attempted = false;
static AAssetManager* g_shader_assets = nullptr;
// AAssetManager_fromJava remains valid while its Java manager is retained.
static jobject g_shader_assets_owner = nullptr;
static jclass g_shader_progress_class = nullptr;
static jmethodID g_shader_progress_method = nullptr;
static uint32_t g_language_ext_result = 0;

static std::recursive_mutex g_load_mutex;

enum class GuestLoadState {
    Cold,
    Loading,
    Ready,
    Fatal,
};

// Protected by g_load_mutex. In particular, never retry after FakeJniEnv has
// been attached to a host that failed during guest JNI_OnLoad: allocating a new
// DynarmicHost at the same host address could make FakeJniEnv::Init mistake its
// stale guest tables for current ones.
static GuestLoadState g_guest_load_state = GuestLoadState::Cold;
// Display callbacks can arrive before the guest has reached B_Init.
// The host mutex protects the verified guest patch and its published literal.
// Never hold the load mutex while waiting for a running guest's host mutex:
// synchronous guest-to-Java callbacks can re-enter EnsureGameLoaded.
static std::atomic<float> g_pending_display_refresh_rate{0.0f};
static std::atomic<bool> g_bl2_tick_patch_ready{false};
static uint32_t g_bl2_tick_limit_bits = 0;
static std::atomic<bool> g_local_identity_prepared{false};

// Guest pthreads are represented by detached host threads and may still be in
// Dynarmic when the shared library's C++ globals are torn down. Android reclaims
// the entire address space at process exit, so once FakeJniEnv has published
// pointers into this host, deliberately release ownership before global
// destructors run. This prevents a late unique_ptr destructor from unmapping the
// 4 GiB arena underneath a worker; early load failures still use normal reset().
static void PinHostForProcessLifetime() {
    struct HostLifetimePin {
        ~HostLifetimePin() { (void)g_host.release(); }
    };
    static HostLifetimePin pin;
    (void)pin;
}

extern "C" int IndexApk(const char* apk_path);
extern "C" int IndexObbSplitApks(const char* const* paths, size_t count);

struct GuestMmapView {
    int fd = -1;
    void* map_base = MAP_FAILED;
    size_t map_length = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::string entry_name;
};

static uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static bool ReadExactAt(int fd, uint64_t offset, void* output, size_t length) {
    auto* bytes = static_cast<uint8_t*>(output);
    size_t done = 0;
    while (done < length) {
        const ssize_t count = pread(fd, bytes + done, length - done,
                                    static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<size_t>(count);
    }
    return true;
}

static void CloseGuestMmap(GuestMmapView& view) {
    if (view.map_base != MAP_FAILED) munmap(view.map_base, view.map_length);
    if (view.fd >= 0) close(view.fd);
    view = {};
}

// Map an uncompressed APK entry directly. The view remains valid only until
// CloseGuestMmap; ELF loading copies its PT_LOAD ranges before the mapping ends.
static bool MmapGuestFromApk(const std::string& apk_path,
                             const std::vector<std::string>& candidate_names,
                             GuestMmapView& view) {
    if (apk_path.empty()) return false;
    const int fd = open(apk_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("MmapGuestFromApk: open failed for %s (errno=%d)",
             apk_path.c_str(), errno);
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < 22) {
        close(fd);
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);
    const size_t tail_size = static_cast<size_t>(
        std::min<uint64_t>(file_size, 65535u + 22u));
    std::vector<uint8_t> tail(tail_size);
    if (!ReadExactAt(fd, file_size - tail_size, tail.data(), tail.size())) {
        close(fd);
        return false;
    }

    size_t eocd = std::string::npos;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (ReadLe32(tail.data() + i) == 0x06054B50u &&
            i + 22 <= tail.size() &&
            i + 22u + ReadLe16(tail.data() + i + 20) == tail.size()) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        close(fd);
        LOGE("MmapGuestFromApk: ZIP EOCD not found in %s", apk_path.c_str());
        return false;
    }

    const uint8_t* end = tail.data() + eocd;
    const uint16_t disk = ReadLe16(end + 4);
    const uint16_t central_disk = ReadLe16(end + 6);
    const uint16_t entries_disk = ReadLe16(end + 8);
    const uint16_t entries_total = ReadLe16(end + 10);
    const uint32_t central_size = ReadLe32(end + 12);
    const uint32_t central_offset = ReadLe32(end + 16);
    if (disk != 0 || central_disk != 0 || entries_disk != entries_total ||
        entries_total == 0xFFFFu || central_size == 0xFFFFFFFFu ||
        central_offset == 0xFFFFFFFFu ||
        static_cast<uint64_t>(central_offset) + central_size > file_size) {
        close(fd);
        LOGE("MmapGuestFromApk: unsupported/malformed ZIP directory");
        return false;
    }

    std::vector<uint8_t> central(central_size);
    if (!ReadExactAt(fd, central_offset, central.data(), central.size())) {
        close(fd);
        return false;
    }
    size_t pos = 0;
    for (uint32_t i = 0; i < entries_total; ++i) {
        if (pos > central.size() || central.size() - pos < 46 ||
            ReadLe32(central.data() + pos) != 0x02014B50u) {
            close(fd);
            LOGE("MmapGuestFromApk: malformed ZIP central directory entry %u", i);
            return false;
        }
        const uint16_t flags = ReadLe16(central.data() + pos + 8);
        const uint16_t method = ReadLe16(central.data() + pos + 10);
        const uint32_t compressed_size = ReadLe32(central.data() + pos + 20);
        const uint32_t uncompressed_size = ReadLe32(central.data() + pos + 24);
        const uint16_t name_length = ReadLe16(central.data() + pos + 28);
        const uint16_t extra_length = ReadLe16(central.data() + pos + 30);
        const uint16_t comment_length = ReadLe16(central.data() + pos + 32);
        const uint32_t local_offset = ReadLe32(central.data() + pos + 42);
        const size_t record_size = 46u + name_length + extra_length + comment_length;
        if (record_size > central.size() - pos) {
            close(fd);
            LOGE("MmapGuestFromApk: truncated ZIP central directory");
            return false;
        }

        const std::string name(
            reinterpret_cast<const char*>(central.data() + pos + 46), name_length);
        const bool wanted = std::find(candidate_names.begin(), candidate_names.end(),
                                      name) != candidate_names.end();
        if (wanted) {
            if ((flags & 1u) != 0 || method != 0 ||
                compressed_size != uncompressed_size || uncompressed_size == 0) {
                close(fd);
                LOGE("MmapGuestFromApk: %s must be a non-empty, unencrypted STORED entry",
                     name.c_str());
                return false;
            }
            uint8_t local[30];
            if (!ReadExactAt(fd, local_offset, local, sizeof(local)) ||
                ReadLe32(local) != 0x04034B50u) {
                close(fd);
                LOGE("MmapGuestFromApk: invalid local header for %s", name.c_str());
                return false;
            }
            const uint16_t local_name_length = ReadLe16(local + 26);
            const uint16_t local_extra_length = ReadLe16(local + 28);
            const uint64_t data_offset = static_cast<uint64_t>(local_offset) + 30u +
                                         local_name_length + local_extra_length;
            if (data_offset + uncompressed_size > file_size) {
                close(fd);
                LOGE("MmapGuestFromApk: data range is outside APK for %s", name.c_str());
                return false;
            }
            std::vector<uint8_t> local_name(local_name_length);
            if (!ReadExactAt(fd, static_cast<uint64_t>(local_offset) + 30u,
                             local_name.data(), local_name.size()) ||
                std::string(reinterpret_cast<const char*>(local_name.data()),
                            local_name.size()) != name) {
                close(fd);
                LOGE("MmapGuestFromApk: local/central name mismatch for %s", name.c_str());
                return false;
            }
            const long page_size = sysconf(_SC_PAGESIZE);
            if (page_size <= 0 ||
                (static_cast<unsigned long>(page_size) &
                 (static_cast<unsigned long>(page_size) - 1u)) != 0) {
                close(fd);
                LOGE("MmapGuestFromApk: invalid host page size %ld", page_size);
                return false;
            }
            const uint64_t aligned_offset = data_offset &
                ~static_cast<uint64_t>(page_size - 1);
            const size_t delta = static_cast<size_t>(data_offset - aligned_offset);
            if (uncompressed_size > SIZE_MAX - delta) {
                close(fd);
                return false;
            }
            const size_t map_length = delta + uncompressed_size;
            void* mapped = mmap(nullptr, map_length, PROT_READ, MAP_PRIVATE, fd,
                                static_cast<off_t>(aligned_offset));
            if (mapped == MAP_FAILED) {
                LOGE("MmapGuestFromApk: mmap failed for %s (errno=%d)",
                     name.c_str(), errno);
                close(fd);
                return false;
            }
            view.fd = fd;
            view.map_base = mapped;
            view.map_length = map_length;
            view.data = static_cast<const uint8_t*>(mapped) + delta;
            view.size = uncompressed_size;
            view.entry_name = name;
            LOGI("Mapped stored APK entry: %s (%u bytes)",
                 name.c_str(), uncompressed_size);
            return true;
        }
        pos += record_size;
    }
    close(fd);
    LOGE("MmapGuestFromApk: guest ELF candidates not found in %s", apk_path.c_str());
    return false;
}

std::vector<uint8_t> LoadBl2ShaderWarmupTable() {
    GuestMmapView view;
    if (!MmapGuestFromApk(g_saved_apk_path,
                         {"assets/bl2_shader_warmup.bin"}, view)) return {};
    std::vector<uint8_t> bytes;
    if (view.size <= 8u * 1024u * 1024u) {
        bytes.assign(view.data, view.data + view.size);
    }
    CloseGuestMmap(view);
    return bytes;
}

static std::vector<uint8_t> LoadBl2JitWarmupTable() {
    GuestMmapView view;
    if (!MmapGuestFromApk(g_saved_apk_path, {"assets/bl2_jit_warmup.bin"}, view)) return {};
    std::vector<uint8_t> bytes;
    if (view.size <= Bl2JitWarmup::kMaxBytes)
        bytes.assign(view.data, view.data + view.size);
    CloseGuestMmap(view);
    return bytes;
}

// A fresh install has no learned JIT profile: start from the shipped one. The
// guest is one ELF at one base on every device, so the blocks earlier sessions
// reached are this device's warm-up list too; play then extends this copy.
// LoadLearnedJitProfile still checks its guest identity and every descriptor.
static void SeedBl2LearnedJitProfile(const std::string& directory) {
    if (directory.empty()) return;
    const std::string path = directory + "/bl2_jit_learned.bin";
    struct stat info{};
    if (::stat(path.c_str(), &info) == 0) return;  // The device's own profile wins.
    GuestMmapView view;
    if (!MmapGuestFromApk(g_saved_apk_path, {"assets/bl2_jit_learned_seed.bin"}, view)) return;
    std::string temporary = path + ".seed.XXXXXX";
    const int fd = view.size <= 16u * 1024u * 1024u ? ::mkstemp(temporary.data()) : -1;
    bool good = fd >= 0;
    for (size_t written = 0; good && written < view.size;) {
        const ssize_t count = ::write(fd, view.data + written, view.size - written);
        good = count > 0;
        if (good) written += static_cast<size_t>(count);
    }
    if (fd >= 0) good = ::close(fd) == 0 && good;
    if (fd >= 0 && (!good || std::rename(temporary.c_str(), path.c_str()) != 0)) {
        std::remove(temporary.c_str());
        good = false;
    }
    __android_log_print(ANDROID_LOG_INFO, "BL2JitLearn", "seed profile installed=%d bytes=%zu",
                        good, view.size);
    CloseGuestMmap(view);
}

// ---------------------------------------------------------------------------
// Loading the guest
// ---------------------------------------------------------------------------

// Candidate paths inside the APK for one library name.
static std::vector<std::string> ApkCandidates(const char* lib) {
    return {std::string("assets/guest_libs/") + lib,
            std::string("lib/armeabi/") + lib,
            std::string("lib/armeabi-v7a/") + lib,
            std::string("assets/guestlib/") + lib,
            std::string("assets/") + lib,
            std::string("assets/lib/") + lib};
}

static uint32_t Sym(const char* name) {
    uint32_t a = g_host->GetElfLoader().GetSymbolAddress(name);
    if (!a) LOGE("Guest symbol not found: %s", name);
    return a;
}

static bool EncodeArmBranch(uint32_t from, uint32_t to, uint32_t& instruction) {
    if ((from & 3u) != 0 || (to & 3u) != 0) return false;
    const int64_t delta = static_cast<int64_t>(to) -
                          (static_cast<int64_t>(from) + 8);
    if ((delta & 3) != 0) return false;
    const int64_t words = delta / 4;
    if (words < -0x800000LL || words > 0x7FFFFFLL) return false;
    instruction = 0xEA000000u |
                  (static_cast<uint32_t>(words) & 0x00FFFFFFu);
    return true;
}

static thread_local uint32_t g_gfx_movie_def_depth = 0;

static bool InstallDiagnosticGFxEntryTrace() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.horn.gfx_entry_trace", value) <= 0 ||
        value[0] != '1') return true;

    ElfLoader& loader = g_host->GetElfLoader();
    constexpr uint32_t expected_image_size = 0x03089000u;
    constexpr uint32_t cave_bytes = 224;
    if (loader.GetImageSize() != expected_image_size) {
        LOGE("GFx entry trace refused: unexpected guest image size 0x%08X",
             loader.GetImageSize());
        return false;
    }
    const uint32_t start = Sym("_ZN15UGFxMoviePlayer5StartEj");
    const uint32_t load = Sym("_ZN10FGFxEngine9LoadMovieEPKcj");
    const uint32_t load_def = Sym("_ZN10FGFxEngine12LoadMovieDefEPKcRN9Scaleform3GFx9MovieInfoE");
    const uint32_t load_success = load + 0x1f4u;
    const uint32_t load_failure = load + 0x268u;
    const uint32_t movie_def_result = load + 0xc8u;
    const uint32_t instance_result = load + 0x164u;
    const uint32_t init_result = load + 0x198u;
    const uint32_t static_load_result = load_def + 0x12cu;
    const uint32_t def_return = load_def + 0xe0u;
    const uint32_t loader_result = load_def + 0xc4u;
    const uint32_t fallback_loader_result = load_def + 0x260u;
    const uint32_t scaleform_loader_state = loader.GetBaseAddress() + 0x0274ff28u;
    const uint32_t input_drain_entry = Sym("_ZN20FAndroidInputManager15GetAllKeyEventsER6TArrayI16FAndroidKeyEvent21TAlignedHeapAllocatorILm8EEE");
    const uint32_t viewport_key_entry = Sym("_ZN11FKdViewport28PassKeyStateToViewportClientEii5FNameid");
    const uint32_t scaleform_movie_result = loader.GetBaseAddress() + 0x0275023cu;
    const uint32_t input_keymap_result = loader.GetBaseAddress() + 0x01e13180u;
    const uint32_t input_queue_return = loader.GetBaseAddress() + 0x01e1325cu;
    const uint32_t input_early_return = loader.GetBaseAddress() + 0x01e13108u;
    const uint32_t cave = loader.GetBaseAddress() + expected_image_size - cave_bytes;
    constexpr uint32_t start_push = 0xE92D40F0u;
    constexpr uint32_t load_push = 0xE92D4FF0u;
    constexpr uint32_t load_return = 0xE8BD8FF0u;
    constexpr uint32_t mov_r4_r0 = 0xE1A04000u;
    constexpr uint32_t ldr_r0_r6_34 = 0xE5960034u;
    constexpr uint32_t ldr_r3_r6_2e8 = 0xE59632E8u;
    constexpr uint32_t mov_r0_r5 = 0xE1A00005u;
    constexpr uint32_t cmp_r0_zero = 0xE3500000u;
    constexpr uint32_t mov_r5_r0 = 0xE1A05000u;
    constexpr uint32_t ldr_r3_r6_30 = 0xE5963030u;
    constexpr uint32_t input_drain_push = 0xE92D41F0u;
    constexpr uint32_t viewport_key_sub_sp = 0xE24DD008u;
    constexpr uint32_t subs_r5_r0_zero = 0xE2505000u;
    constexpr uint32_t cmn_r0_negative_one = 0xE3700001u;
    constexpr uint32_t mov_r0_one = 0xE3A00001u;
    if (!start || !load || !load_def || !input_drain_entry || !viewport_key_entry ||
        g_host->MemoryRead32(start) != start_push ||
        g_host->MemoryRead32(load) != load_push ||
        g_host->MemoryRead32(load_def) != load_push ||
        g_host->MemoryRead32(load_success) != load_return ||
        g_host->MemoryRead32(load_failure) != load_return ||
        g_host->MemoryRead32(movie_def_result) != mov_r4_r0 ||
        g_host->MemoryRead32(instance_result) != mov_r4_r0 ||
        g_host->MemoryRead32(init_result) != ldr_r0_r6_34 ||
        g_host->MemoryRead32(static_load_result) != ldr_r3_r6_2e8 ||
        g_host->MemoryRead32(def_return) != mov_r0_r5 ||
        g_host->MemoryRead32(loader_result) != cmp_r0_zero ||
        g_host->MemoryRead32(fallback_loader_result) != mov_r5_r0 ||
        g_host->MemoryRead32(scaleform_loader_state) != ldr_r3_r6_30 ||
        g_host->MemoryRead32(input_drain_entry) != input_drain_push ||
        g_host->MemoryRead32(viewport_key_entry) != viewport_key_sub_sp ||
        g_host->MemoryRead32(scaleform_movie_result) != subs_r5_r0_zero ||
        g_host->MemoryRead32(input_keymap_result) != cmn_r0_negative_one ||
        g_host->MemoryRead32(input_queue_return) != mov_r0_one ||
        g_host->MemoryRead32(input_early_return) != mov_r0_one) {
        LOGE("GFx entry trace refused: function signature mismatch");
        return false;
    }
    for (uint32_t offset = 0; offset < cave_bytes; offset += 4) {
        if (g_host->MemoryRead32(cave + offset) != 0) {
            LOGE("GFx entry trace refused: nonzero code cave 0x%08X", cave);
            return false;
        }
    }
    uint32_t to_start_cave = 0, back_from_start = 0;
    uint32_t to_load_cave = 0, back_from_load = 0;
    uint32_t to_success_cave = 0, to_failure_cave = 0;
    uint32_t to_def_cave = 0, back_from_def = 0;
    uint32_t to_instance_cave = 0, back_from_instance = 0;
    uint32_t to_init_cave = 0, back_from_init = 0;
    uint32_t to_static_cave = 0, back_from_static = 0;
    uint32_t to_def_return_cave = 0, back_from_def_return = 0;
    uint32_t to_def_entry_cave = 0, back_from_def_entry = 0;
    uint32_t to_loader_cave = 0, back_from_loader = 0;
    uint32_t to_fallback_cave = 0, back_from_fallback = 0;
    uint32_t to_scaleform_cave = 0, back_from_scaleform = 0;
    uint32_t to_cache_cave = 0, back_from_cache = 0;
    uint32_t to_resource_cave = 0, back_from_resource = 0;
    uint32_t to_movie_cave = 0, back_from_movie = 0;
    uint32_t to_map_cave = 0, back_from_map = 0;
    uint32_t to_queue_cave = 0, back_from_queue = 0;
    uint32_t to_early_return_cave = 0, back_from_early_return = 0;
    if (!EncodeArmBranch(start, cave, to_start_cave) ||
        !EncodeArmBranch(cave + 8, start + 4, back_from_start) ||
        !EncodeArmBranch(load, cave + 12, to_load_cave) ||
        !EncodeArmBranch(cave + 20, load + 4, back_from_load) ||
        !EncodeArmBranch(load_success, cave + 24, to_success_cave) ||
        !EncodeArmBranch(load_failure, cave + 32, to_failure_cave) ||
        !EncodeArmBranch(movie_def_result, cave + 40, to_def_cave) ||
        !EncodeArmBranch(cave + 48, movie_def_result + 4, back_from_def) ||
        !EncodeArmBranch(instance_result, cave + 52, to_instance_cave) ||
        !EncodeArmBranch(cave + 60, instance_result + 4, back_from_instance) ||
        !EncodeArmBranch(init_result, cave + 64, to_init_cave) ||
        !EncodeArmBranch(cave + 72, init_result + 4, back_from_init) ||
        !EncodeArmBranch(static_load_result, cave + 76, to_static_cave) ||
        !EncodeArmBranch(cave + 84, static_load_result + 4, back_from_static) ||
        !EncodeArmBranch(def_return, cave + 88, to_def_return_cave) ||
        !EncodeArmBranch(cave + 96, def_return + 4, back_from_def_return) ||
        !EncodeArmBranch(load_def, cave + 100, to_def_entry_cave) ||
        !EncodeArmBranch(cave + 108, load_def + 4, back_from_def_entry) ||
        !EncodeArmBranch(loader_result, cave + 112, to_loader_cave) ||
        !EncodeArmBranch(cave + 120, loader_result + 4, back_from_loader) ||
        !EncodeArmBranch(fallback_loader_result, cave + 124, to_fallback_cave) ||
        !EncodeArmBranch(cave + 132, fallback_loader_result + 4, back_from_fallback) ||
        !EncodeArmBranch(scaleform_loader_state, cave + 136, to_scaleform_cave) ||
        !EncodeArmBranch(cave + 144, scaleform_loader_state + 4, back_from_scaleform) ||
        !EncodeArmBranch(input_drain_entry, cave + 148, to_cache_cave) ||
        !EncodeArmBranch(cave + 156, input_drain_entry + 4, back_from_cache) ||
        !EncodeArmBranch(viewport_key_entry, cave + 160, to_resource_cave) ||
        !EncodeArmBranch(cave + 168, viewport_key_entry + 4, back_from_resource) ||
        !EncodeArmBranch(scaleform_movie_result, cave + 172, to_movie_cave) ||
        !EncodeArmBranch(cave + 180, scaleform_movie_result + 4, back_from_movie) ||
        !EncodeArmBranch(input_keymap_result, cave + 184, to_map_cave) ||
        !EncodeArmBranch(cave + 192, input_keymap_result + 4, back_from_map) ||
        !EncodeArmBranch(input_queue_return, cave + 196, to_queue_cave) ||
        !EncodeArmBranch(cave + 204, input_queue_return + 4,
                         back_from_queue) ||
        !EncodeArmBranch(input_early_return, cave + 208,
                         to_early_return_cave) ||
        !EncodeArmBranch(cave + 216, input_early_return + 4,
                         back_from_early_return)) {
        LOGE("GFx entry trace refused: ARM branch out of range");
        return false;
    }
    auto& thunks = g_host->GetThunkManager();
    const uint32_t start_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_movie_start",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            static std::atomic<uint32_t> calls{0};
            const uint32_t n = calls.fetch_add(1);
            if (n >= 24) return;
            const uint32_t object = jit.Regs()[0];
            const uint32_t state = object &&
                static_cast<uint64_t>(object) + 0x40u <= host.GetGuestMemorySize()
                ? host.MemoryRead32(object + 0x3cu) : 0;
            LOGI("GFxMoviePlayer::Start call=%u this=0x%08X arg=%u state=0x%08X",
                 n + 1, object, jit.Regs()[1], state);
        });
    const uint32_t load_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_engine_load_movie",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            static std::atomic<uint32_t> calls{0};
            const uint32_t n = calls.fetch_add(1);
            if (n >= 24) return;
            const uint32_t path_address = jit.Regs()[1];
            std::string path;
            for (uint32_t i = 0; i < 80 &&
                    static_cast<uint64_t>(path_address) + i < host.GetGuestMemorySize();
                    ++i) {
                const uint8_t ch = host.MemoryRead8(path_address + i);
                if (!ch) break;
                path.push_back(ch >= 32 && ch < 127 ? static_cast<char>(ch) : '?');
            }
            LOGI("FGFxEngine::LoadMovie call=%u this=0x%08X path='%s' arg2=%u",
                 n + 1, jit.Regs()[0], path.c_str(), jit.Regs()[2]);
        });
    const uint32_t success_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_movie_load_return_success",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            static std::atomic<uint32_t> calls{0};
            const uint32_t n = calls.fetch_add(1);
            if (n < 24) LOGI("FGFxEngine::LoadMovie success-return call=%u result=0x%08X",
                             n + 1, jit.Regs()[0]);
        });
    const uint32_t failure_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_movie_load_return_failure",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            static std::atomic<uint32_t> calls{0};
            const uint32_t n = calls.fetch_add(1);
            if (n < 24) LOGI("FGFxEngine::LoadMovie failure-return call=%u result=0x%08X",
                             n + 1, jit.Regs()[0]);
        });
    const auto stage_trace = [](const char* name) {
        return [name](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            static std::atomic<uint32_t> calls{0};
            const uint32_t n = calls.fetch_add(1);
            if (n >= 72) return;
            const uint32_t work = jit.Regs()[6];
            const bool valid = work &&
                static_cast<uint64_t>(work) + 0x38u <= host.GetGuestMemorySize();
            LOGI("FGFxEngine::LoadMovie stage=%s call=%u r0=0x%08X work=0x%08X "
                 "definition=0x%08X instance=0x%08X",
                 name, n + 1, jit.Regs()[0], work,
                 valid ? host.MemoryRead32(work + 0x30u) : 0,
                 valid ? host.MemoryRead32(work + 0x34u) : 0);
        };
    };
    const uint32_t def_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_movie_definition_result", stage_trace("definition"));
    const uint32_t instance_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_movie_instance_result", stage_trace("instance"));
    const uint32_t init_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_movie_init_result", stage_trace("init"));
    const uint32_t static_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_static_load_result",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            static std::atomic<uint32_t> calls{0};
            const uint32_t n = calls.fetch_add(1);
            if (g_gfx_movie_def_depth != 1) return;
            const uint32_t engine = jit.Regs()[6];
            const bool valid = engine &&
                static_cast<uint64_t>(engine) + 0x2ecu <= host.GetGuestMemorySize();
            LOGI("FGFxEngine::LoadMovieDef StaticLoadObject call=%u result=0x%08X "
                 "engine=0x%08X field2e8=0x%08X",
                 n + 1, jit.Regs()[0], engine,
                 valid ? host.MemoryRead32(engine + 0x2e8u) : 0);
        });
    const uint32_t def_return_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_load_movie_def_return",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            static std::atomic<uint32_t> calls{0};
            const uint32_t n = calls.fetch_add(1);
            if (g_gfx_movie_def_depth == 1)
                LOGI("FGFxEngine::LoadMovieDef outer-return call=%u result=0x%08X "
                     "staticCandidate=0x%08X",
                     n + 1, jit.Regs()[5], jit.Regs()[8]);
            if (g_gfx_movie_def_depth > 0) --g_gfx_movie_def_depth;
        });
    const uint32_t def_entry_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_load_movie_def_entry",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            ++g_gfx_movie_def_depth;
            if (g_gfx_movie_def_depth == 1)
                LOGI("FGFxEngine::LoadMovieDef outer-entry engine=0x%08X path=0x%08X",
                     jit.Regs()[0], jit.Regs()[1]);
        });
    const auto loader_stage = [](const char* stage) {
        return [stage](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            if (g_gfx_movie_def_depth == 1)
                LOGI("FGFxEngine::LoadMovieDef outer-%s result=0x%08X",
                     stage, jit.Regs()[0]);
        };
    };
    const uint32_t loader_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_loader_result", loader_stage("loader"));
    const uint32_t fallback_thunk = thunks.RegisterThunk(
        "diagnostic_gfx_fallback_loader_result", loader_stage("fallback-loader"));
    const uint32_t scaleform_thunk = thunks.RegisterThunk(
        "diagnostic_scaleform_loader_state",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            if (g_gfx_movie_def_depth != 1) return;
            const uint32_t object = jit.Regs()[6];
            const bool valid = object &&
                static_cast<uint64_t>(object) + 0x34u <= host.GetGuestMemorySize();
            LOGI("Scaleform loader outer state object=0x%08X field30=0x%08X",
                 object, valid ? host.MemoryRead32(object + 0x30u) : 0);
        });
    const auto scaleform_stage = [](const char* stage) {
        return [stage](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            if (g_gfx_movie_def_depth == 1)
                LOGI("Scaleform loader outer stage=%s result=0x%08X",
                     stage, jit.Regs()[0]);
        };
    };
    const uint32_t cache_thunk = thunks.RegisterThunk(
        "diagnostic_input_queue_drain",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            const uint32_t manager = jit.Regs()[0];
            if (static_cast<uint64_t>(manager) + 0x30u > host.GetGuestMemorySize())
                return;
            const uint32_t count = host.MemoryRead32(manager + 0x2cu);
            if (!count) return;
            static std::atomic<uint32_t> calls{0};
            if (calls.fetch_add(1) >= 48) return;
            LOGI("FAndroidInputManager::GetAllKeyEvents queued=%u output=0x%08X",
                 count, jit.Regs()[1]);
        });
    const uint32_t resource_thunk = thunks.RegisterThunk(
        "diagnostic_viewport_key_dispatch",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            static std::atomic<uint32_t> calls{0};
            if (calls.fetch_add(1) >= 48) return;
            LOGI("FKdViewport::PassKeyState controller=%u state=%u name=0x%08X",
                 jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
        });
    const uint32_t movie_thunk = thunks.RegisterThunk(
        "diagnostic_scaleform_movie_result", scaleform_stage("movie-build"));
    const uint32_t map_thunk = thunks.RegisterThunk(
        "diagnostic_input_keymap_result",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            static std::atomic<uint32_t> calls{0};
            if (calls.fetch_add(1) >= 48) return;
            LOGI("NativeCallback_KeyEvent keymap controller=%u action=%u rawKey=%u "
                 "mappedIndex=0x%08X",
                 jit.Regs()[5], jit.Regs()[7], jit.Regs()[8], jit.Regs()[0]);
        });
    const uint32_t queue_thunk = thunks.RegisterThunk(
        "diagnostic_input_queue_return",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            static std::atomic<uint32_t> calls{0};
            if (calls.fetch_add(1) >= 48) return;
            LOGI("NativeCallback_KeyEvent queued controller=%u actionChanged=%u "
                 "mappedData0=0x%08X mappedData1=0x%08X",
                 jit.Regs()[5], jit.Regs()[7], jit.Regs()[8], jit.Regs()[9]);
        });
    const uint32_t early_return_thunk = thunks.RegisterThunk(
        "diagnostic_input_early_return",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            static std::atomic<uint32_t> calls{0};
            if (calls.fetch_add(1) >= 48) return;
            LOGI("NativeCallback_KeyEvent early-return controller=%u action=%u rawKey=%u",
                 jit.Regs()[5], jit.Regs()[7], jit.Regs()[8]);
        });
    if (!start_thunk || !load_thunk || !success_thunk || !failure_thunk ||
        !def_thunk || !instance_thunk || !init_thunk ||
        !static_thunk || !def_return_thunk || !def_entry_thunk ||
        !loader_thunk || !fallback_thunk || !scaleform_thunk ||
        !cache_thunk || !resource_thunk || !movie_thunk || !map_thunk ||
        !queue_thunk || !early_return_thunk)
        return false;
    const uint32_t start_svc = g_host->MemoryRead32(start_thunk);
    const uint32_t load_svc = g_host->MemoryRead32(load_thunk);
    const uint32_t success_svc = g_host->MemoryRead32(success_thunk);
    const uint32_t failure_svc = g_host->MemoryRead32(failure_thunk);
    const uint32_t def_svc = g_host->MemoryRead32(def_thunk);
    const uint32_t instance_svc = g_host->MemoryRead32(instance_thunk);
    const uint32_t init_svc = g_host->MemoryRead32(init_thunk);
    const uint32_t static_svc = g_host->MemoryRead32(static_thunk);
    const uint32_t def_return_svc = g_host->MemoryRead32(def_return_thunk);
    const uint32_t def_entry_svc = g_host->MemoryRead32(def_entry_thunk);
    const uint32_t loader_svc = g_host->MemoryRead32(loader_thunk);
    const uint32_t fallback_svc = g_host->MemoryRead32(fallback_thunk);
    const uint32_t scaleform_svc = g_host->MemoryRead32(scaleform_thunk);
    const uint32_t cache_svc = g_host->MemoryRead32(cache_thunk);
    const uint32_t resource_svc = g_host->MemoryRead32(resource_thunk);
    const uint32_t movie_svc = g_host->MemoryRead32(movie_thunk);
    const uint32_t map_svc = g_host->MemoryRead32(map_thunk);
    const uint32_t queue_svc = g_host->MemoryRead32(queue_thunk);
    const uint32_t early_return_svc = g_host->MemoryRead32(early_return_thunk);
    if ((start_svc & 0xFF000000u) != 0xEF000000u ||
        (load_svc & 0xFF000000u) != 0xEF000000u ||
        (success_svc & 0xFF000000u) != 0xEF000000u ||
        (failure_svc & 0xFF000000u) != 0xEF000000u ||
        (def_svc & 0xFF000000u) != 0xEF000000u ||
        (instance_svc & 0xFF000000u) != 0xEF000000u ||
        (init_svc & 0xFF000000u) != 0xEF000000u ||
        (static_svc & 0xFF000000u) != 0xEF000000u ||
        (def_return_svc & 0xFF000000u) != 0xEF000000u ||
        (def_entry_svc & 0xFF000000u) != 0xEF000000u ||
        (loader_svc & 0xFF000000u) != 0xEF000000u ||
        (fallback_svc & 0xFF000000u) != 0xEF000000u ||
        (scaleform_svc & 0xFF000000u) != 0xEF000000u ||
        (cache_svc & 0xFF000000u) != 0xEF000000u ||
        (resource_svc & 0xFF000000u) != 0xEF000000u ||
        (movie_svc & 0xFF000000u) != 0xEF000000u ||
        (map_svc & 0xFF000000u) != 0xEF000000u ||
        (queue_svc & 0xFF000000u) != 0xEF000000u ||
        (early_return_svc & 0xFF000000u) != 0xEF000000u) return false;
    const uint32_t instructions[] = {
        start_svc, start_push, back_from_start,
        load_svc, load_push, back_from_load,
        success_svc, load_return,
        failure_svc, load_return,
        def_svc, mov_r4_r0, back_from_def,
        instance_svc, mov_r4_r0, back_from_instance,
        init_svc, ldr_r0_r6_34, back_from_init,
        static_svc, ldr_r3_r6_2e8, back_from_static,
        def_return_svc, mov_r0_r5, back_from_def_return,
        def_entry_svc, load_push, back_from_def_entry,
        loader_svc, cmp_r0_zero, back_from_loader,
        fallback_svc, mov_r5_r0, back_from_fallback,
        scaleform_svc, ldr_r3_r6_30, back_from_scaleform,
        cache_svc, input_drain_push, back_from_cache,
        resource_svc, viewport_key_sub_sp, back_from_resource,
        movie_svc, subs_r5_r0_zero, back_from_movie,
        map_svc, cmn_r0_negative_one, back_from_map,
        queue_svc, mov_r0_one, back_from_queue,
        early_return_svc, mov_r0_one, back_from_early_return};
    for (uint32_t i = 0; i < sizeof(instructions) / sizeof(instructions[0]); ++i) {
        g_host->MemoryWrite32(cave + i * 4, instructions[i]);
    }
    g_host->InvalidateGuestCode(cave, cave_bytes);
    g_host->MemoryWrite32(start, to_start_cave);
    g_host->InvalidateGuestCode(start, 4);
    g_host->MemoryWrite32(load, to_load_cave);
    g_host->InvalidateGuestCode(load, 4);
    g_host->MemoryWrite32(load_success, to_success_cave);
    g_host->InvalidateGuestCode(load_success, 4);
    g_host->MemoryWrite32(load_failure, to_failure_cave);
    g_host->InvalidateGuestCode(load_failure, 4);
    g_host->MemoryWrite32(movie_def_result, to_def_cave);
    g_host->InvalidateGuestCode(movie_def_result, 4);
    g_host->MemoryWrite32(instance_result, to_instance_cave);
    g_host->InvalidateGuestCode(instance_result, 4);
    g_host->MemoryWrite32(init_result, to_init_cave);
    g_host->InvalidateGuestCode(init_result, 4);
    g_host->MemoryWrite32(static_load_result, to_static_cave);
    g_host->InvalidateGuestCode(static_load_result, 4);
    g_host->MemoryWrite32(def_return, to_def_return_cave);
    g_host->InvalidateGuestCode(def_return, 4);
    g_host->MemoryWrite32(load_def, to_def_entry_cave);
    g_host->InvalidateGuestCode(load_def, 4);
    g_host->MemoryWrite32(loader_result, to_loader_cave);
    g_host->InvalidateGuestCode(loader_result, 4);
    g_host->MemoryWrite32(fallback_loader_result, to_fallback_cave);
    g_host->InvalidateGuestCode(fallback_loader_result, 4);
    g_host->MemoryWrite32(scaleform_loader_state, to_scaleform_cave);
    g_host->InvalidateGuestCode(scaleform_loader_state, 4);
    g_host->MemoryWrite32(input_drain_entry, to_cache_cave);
    g_host->InvalidateGuestCode(input_drain_entry, 4);
    g_host->MemoryWrite32(viewport_key_entry, to_resource_cave);
    g_host->InvalidateGuestCode(viewport_key_entry, 4);
    g_host->MemoryWrite32(scaleform_movie_result, to_movie_cave);
    g_host->InvalidateGuestCode(scaleform_movie_result, 4);
    g_host->MemoryWrite32(input_keymap_result, to_map_cave);
    g_host->InvalidateGuestCode(input_keymap_result, 4);
    g_host->MemoryWrite32(input_queue_return, to_queue_cave);
    g_host->InvalidateGuestCode(input_queue_return, 4);
    g_host->MemoryWrite32(input_early_return, to_early_return_cave);
    g_host->InvalidateGuestCode(input_early_return, 4);
    if (g_host->MemoryRead32(start) != to_start_cave ||
        g_host->MemoryRead32(load) != to_load_cave ||
        g_host->MemoryRead32(load_success) != to_success_cave ||
        g_host->MemoryRead32(load_failure) != to_failure_cave ||
        g_host->MemoryRead32(movie_def_result) != to_def_cave ||
        g_host->MemoryRead32(instance_result) != to_instance_cave ||
        g_host->MemoryRead32(init_result) != to_init_cave ||
        g_host->MemoryRead32(static_load_result) != to_static_cave ||
        g_host->MemoryRead32(def_return) != to_def_return_cave ||
        g_host->MemoryRead32(load_def) != to_def_entry_cave ||
        g_host->MemoryRead32(loader_result) != to_loader_cave ||
        g_host->MemoryRead32(fallback_loader_result) != to_fallback_cave ||
        g_host->MemoryRead32(scaleform_loader_state) != to_scaleform_cave ||
        g_host->MemoryRead32(input_drain_entry) != to_cache_cave ||
        g_host->MemoryRead32(viewport_key_entry) != to_resource_cave ||
        g_host->MemoryRead32(scaleform_movie_result) != to_movie_cave ||
        g_host->MemoryRead32(input_keymap_result) != to_map_cave ||
        g_host->MemoryRead32(input_queue_return) != to_queue_cave ||
        g_host->MemoryRead32(input_early_return) != to_early_return_cave)
        return false;
    LOGI("Diagnostic GFx entry/return trace active start=0x%08X load=0x%08X "
         "success=0x%08X failure=0x%08X cave=0x%08X",
         start, load, load_success, load_failure, cave);
    return true;
}

static bool InstallHornReferenceAspectPatch() {
    ElfLoader& loader = g_host->GetElfLoader();
    if (loader.GetImageSize() != cfg::kGuestImageSize) {
        LOGI("Skipping inherited Horn reference-aspect patch for this guest: "
             "image size 0x%08X (Horn signature expects 0x%08X)",
             loader.GetImageSize(), cfg::kGuestImageSize);
        return true;
    }
    const uint32_t calc_scene_view = Sym(cfg::kCalcSceneView);
    if (!calc_scene_view) return false;
    if (cfg::kGuestBssEndOffset >
        cfg::kGuestImageSize - cfg::kWidescreenCaveSize) {
        LOGE("Reference-aspect patch refused: invalid configured cave bounds");
        return false;
    }

    const uint32_t projection_site =
        calc_scene_view + cfg::kProjectionScaleOffset;
    const uint32_t cave_address = loader.GetBaseAddress() +
        loader.GetImageSize() - cfg::kWidescreenCaveSize;
    const uint32_t return_address = projection_site + sizeof(uint32_t);
    uint32_t branch_to_cave = 0;
    uint32_t branch_from_cave = 0;
    if (!EncodeArmBranch(projection_site, cave_address, branch_to_cave) ||
        !EncodeArmBranch(cave_address + 3 * sizeof(uint32_t),
                         return_address, branch_from_cave)) {
        LOGE("Reference-aspect patch refused: ARM branch out of range (site=0x%08X cave=0x%08X)",
             projection_site, cave_address);
        return false;
    }

    const uint32_t expected_cave[cfg::kAspectCaveWordCount] = {
        cfg::kCaveDivide,
        cfg::kCaveLoadAspect,
        cfg::kCaveScaleX,
        branch_from_cave,
        cfg::kReferenceAspect16By9,
    };

    bool original =
        g_host->MemoryRead32(projection_site) ==
            cfg::kProjectionScaleExpectedInstruction;
    bool active = g_host->MemoryRead32(projection_site) == branch_to_cave;
    for (const auto& patch : cfg::kMaintainYFovPatches) {
        const uint32_t patch_address = calc_scene_view + patch.offset;
        const uint32_t instruction = g_host->MemoryRead32(patch_address);
        original = original && instruction == patch.expected;
        active = active && instruction == patch.replacement;
    }
    for (size_t i = 0; i < cfg::kAspectCaveWordCount; ++i) {
        const uint32_t word = g_host->MemoryRead32(
            cave_address + static_cast<uint32_t>(i * sizeof(uint32_t)));
        original = original && word == 0;
        active = active && word == expected_cave[i];
    }

    if (active) {
        LOGI("Reference-aspect Hor+ patch already active at 0x%08X (cave 0x%08X)",
             projection_site, cave_address);
        return true;
    }
    if (!original) {
        LOGE("Reference-aspect patch refused: guest signature or cave padding mismatch "
             "(site=0x%08X word=0x%08X cave=0x%08X)",
             projection_site, g_host->MemoryRead32(projection_site), cave_address);
        return false;
    }

    // Publish the complete cave first and redirect execution only after it is
    // ready. The cave is the final zero-filled 32 bytes of the loader's rounded
    // image range, 0x510 bytes after the shipped BSS end.
    for (size_t i = 0; i < cfg::kAspectCaveWordCount; ++i) {
        g_host->MemoryWrite32(
            cave_address + static_cast<uint32_t>(i * sizeof(uint32_t)),
            expected_cave[i]);
    }
    g_host->InvalidateGuestCode(cave_address, cfg::kWidescreenCaveSize);

    for (const auto& patch : cfg::kMaintainYFovPatches) {
        const uint32_t patch_address = calc_scene_view + patch.offset;
        g_host->MemoryWrite32(patch_address, patch.replacement);
        g_host->InvalidateGuestCode(patch_address, sizeof(uint32_t));
    }
    g_host->MemoryWrite32(projection_site, branch_to_cave);
    g_host->InvalidateGuestCode(projection_site, sizeof(uint32_t));

    bool verified = g_host->MemoryRead32(projection_site) == branch_to_cave;
    for (const auto& patch : cfg::kMaintainYFovPatches) {
        verified = verified &&
            g_host->MemoryRead32(calc_scene_view + patch.offset) ==
                patch.replacement;
    }
    for (size_t i = 0; i < cfg::kAspectCaveWordCount; ++i) {
        verified = verified &&
            g_host->MemoryRead32(
                cave_address + static_cast<uint32_t>(i * sizeof(uint32_t))) ==
                expected_cave[i];
    }
    if (!verified) {
        LOGE("Reference-aspect patch write verification failed");
        return false;
    }

    LOGI("Reference-aspect Hor+ patch active: enum at 0x%08X/0x%08X, "
         "scale at 0x%08X, cave 0x%08X, reference 16:9",
         calc_scene_view + cfg::kMaintainYFovPatches[0].offset,
         calc_scene_view + cfg::kMaintainYFovPatches[1].offset,
         projection_site, cave_address);
    return true;
}

static bool InstallTegra4GraphicsSelectionPatch() {
    bool patch_settings = true;
    bool patch_platform = true;
    const char* selection_mode = "full";
    char mode_property[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.horn.graphics_mode", mode_property) > 0) {
        if (strcmp(mode_property, "generic") == 0) {
            patch_settings = false;
            patch_platform = false;
            selection_mode = "generic";
        } else if (strcmp(mode_property, "profile") == 0) {
            patch_platform = false;
            selection_mode = "profile-only";
        } else if (strcmp(mode_property, "platform") == 0) {
            patch_settings = false;
            selection_mode = "platform-only";
        } else if (strcmp(mode_property, "full") != 0) {
            LOGI("Ignoring unknown debug.horn.graphics_mode='%s'", mode_property);
        }
    }
    ElfLoader& loader = g_host->GetElfLoader();
    if (loader.GetImageSize() != cfg::kGuestImageSize) {
        LOGI("Skipping inherited Horn Tegra 4 profile patch for this guest: "
             "image size 0x%08X (Horn signature expects 0x%08X)",
             loader.GetImageSize(), cfg::kGuestImageSize);
        return true;
    }

    const uint32_t settings_selector = Sym(cfg::kMobileSystemSettingsSelector);
    const uint32_t platform_selector = Sym(cfg::kGameGetCurrentPlatform);
    if (!settings_selector || !platform_selector) return false;

    if (cfg::kTegra4ProfileNameRva >= loader.GetImageSize()) {
        LOGE("Tegra 4 graphics patch refused: profile-name RVA is outside guest image");
        return false;
    }

    const uint32_t profile_name =
        loader.GetBaseAddress() + cfg::kTegra4ProfileNameRva;
    for (size_t i = 0; i < std::size(cfg::kTegra4ProfileName); ++i) {
        if (g_host->MemoryRead32(profile_name + static_cast<uint32_t>(i * 4)) !=
            static_cast<uint8_t>(cfg::kTegra4ProfileName[i])) {
            LOGE("Tegra 4 graphics patch refused: profile-name signature mismatch at 0x%08X",
                 profile_name + static_cast<uint32_t>(i * 4));
            return false;
        }
    }

    const uint32_t settings_replacement[] = {
        cfg::kArmLdrR0FromPc,
        cfg::kArmBxLr,
        profile_name,
    };
    const uint32_t platform_replacement[] = {
        cfg::kArmMovR0Tegra4,
        cfg::kArmBxLr,
    };

    bool settings_original = true;
    bool settings_active = true;
    for (size_t i = 0; i < std::size(settings_replacement); ++i) {
        const uint32_t word = g_host->MemoryRead32(
            settings_selector + static_cast<uint32_t>(i * 4));
        settings_original = settings_original &&
            word == cfg::kSettingsSelectorExpected[i];
        settings_active = settings_active && word == settings_replacement[i];
    }

    bool platform_original = true;
    bool platform_active = true;
    for (size_t i = 0; i < std::size(platform_replacement); ++i) {
        const uint32_t word = g_host->MemoryRead32(
            platform_selector + static_cast<uint32_t>(i * 4));
        platform_original = platform_original &&
            word == cfg::kPlatformSelectorExpected[i];
        platform_active = platform_active && word == platform_replacement[i];
    }

    const bool settings_expected = patch_settings ? settings_active : settings_original;
    const bool platform_expected = patch_platform ? platform_active : platform_original;
    if (settings_expected && platform_expected) {
        LOGI("Graphics selection already active: mode=%s profile=%s platform=%s",
             selection_mode, patch_settings ? cfg::kTegra4ProfileName : "device-default",
             patch_platform ? "Tegra4" : "device-default");
        return true;
    }
    if ((!settings_original && !settings_active) ||
        (!platform_original && !platform_active) ||
        (!patch_settings && !settings_original) ||
        (!patch_platform && !platform_original)) {
        LOGE("Tegra 4 graphics patch refused: guest selector signature mismatch "
             "(settings=0x%08X platform=0x%08X)",
             settings_selector, platform_selector);
        return false;
    }

    // Publish tail words first so neither entry point can observe an incomplete
    // replacement if startup behavior changes and guest initialization becomes
    // concurrent in a future bridge revision.
    if (patch_settings) {
        for (size_t i = std::size(settings_replacement); i-- > 0;) {
            g_host->MemoryWrite32(
                settings_selector + static_cast<uint32_t>(i * 4),
                settings_replacement[i]);
        }
    }
    if (patch_platform) {
        for (size_t i = std::size(platform_replacement); i-- > 0;) {
            g_host->MemoryWrite32(
                platform_selector + static_cast<uint32_t>(i * 4),
                platform_replacement[i]);
        }
    }
    if (patch_settings) {
        g_host->InvalidateGuestCode(
            settings_selector,
            static_cast<uint32_t>(sizeof(settings_replacement)));
    }
    if (patch_platform) {
        g_host->InvalidateGuestCode(
            platform_selector,
            static_cast<uint32_t>(sizeof(platform_replacement)));
    }

    bool verified = true;
    for (size_t i = 0; i < std::size(settings_replacement); ++i) {
        verified = verified &&
            g_host->MemoryRead32(
                settings_selector + static_cast<uint32_t>(i * 4)) ==
            (patch_settings ? settings_replacement[i] : cfg::kSettingsSelectorExpected[i]);
    }
    for (size_t i = 0; i < std::size(platform_replacement); ++i) {
        verified = verified &&
            g_host->MemoryRead32(
                platform_selector + static_cast<uint32_t>(i * 4)) ==
            (patch_platform ? platform_replacement[i] : cfg::kPlatformSelectorExpected[i]);
    }
    if (!verified) {
        LOGE("Tegra 4 graphics patch write verification failed");
        return false;
    }

    LOGI("Graphics selection active: mode=%s profile=%s platform=%s",
         selection_mode, patch_settings ? cfg::kTegra4ProfileName : "device-default",
         patch_platform ? "Tegra4" : "device-default");
    return true;
}

static bool InstallAdaptiveMaxRefreshPatch(float preferred_refresh_rate) {
    if (!std::isfinite(preferred_refresh_rate) || preferred_refresh_rate <= 0.0f) {
        LOGE("Adaptive max-refresh patch skipped: invalid preferredRefreshRate %.3f",
             preferred_refresh_rate);
        return true;
    }
    if (preferred_refresh_rate <= cfg::kShippedMaxSmoothedRate) {
        LOGI("Adaptive max-refresh patch not needed: preferred %.3f Hz, shipped cap %.1f FPS",
             preferred_refresh_rate, cfg::kShippedMaxSmoothedRate);
        return true;
    }
    if (preferred_refresh_rate > cfg::kMaximumSaneDisplayRate) {
        LOGE("Adaptive max-refresh patch refused: implausible preferredRefreshRate %.3f",
             preferred_refresh_rate);
        return false;
    }

    ElfLoader& loader = g_host->GetElfLoader();
    if (loader.GetImageSize() != cfg::kGuestImageSize) {
        LOGI("Skipping inherited Horn max-refresh patch for this guest: "
             "image size 0x%08X (Horn signature expects 0x%08X)",
             loader.GetImageSize(), cfg::kGuestImageSize);
        return true;
    }
    const uint32_t get_max_tick_rate = Sym(cfg::kEngineGetMaxTickRate);
    if (!get_max_tick_rate) return false;

    const uint32_t patch_site =
        get_max_tick_rate + cfg::kMaxSmoothedRateLoadOffset;
    const uint32_t cave_address = loader.GetBaseAddress() +
        loader.GetImageSize() - cfg::kRefreshCaveSize;
    const uint32_t return_address = patch_site + sizeof(uint32_t);
    uint32_t branch_to_cave = 0;
    uint32_t branch_from_cave = 0;
    if (!EncodeArmBranch(patch_site, cave_address, branch_to_cave) ||
        !EncodeArmBranch(cave_address + sizeof(uint32_t),
                         return_address, branch_from_cave)) {
        LOGE("Adaptive max-refresh patch refused: ARM branch out of range "
             "(site=0x%08X cave=0x%08X)", patch_site, cave_address);
        return false;
    }

    const float smoothed_limit =
        preferred_refresh_rate + cfg::kSmoothedRateHeadroom;
    uint32_t limit_bits = 0;
    static_assert(sizeof(limit_bits) == sizeof(smoothed_limit));
    std::memcpy(&limit_bits, &smoothed_limit, sizeof(limit_bits));

    const uint32_t expected_cave[] = {
        cfg::kRefreshCaveLoad,
        branch_from_cave,
        limit_bits,
    };
    const uint32_t site_word = g_host->MemoryRead32(patch_site);
    bool original = site_word == cfg::kMaxSmoothedRateExpectedInstruction;
    bool active = site_word == branch_to_cave;
    for (size_t i = 0; i < std::size(expected_cave); ++i) {
        const uint32_t word = g_host->MemoryRead32(
            cave_address + static_cast<uint32_t>(i * sizeof(uint32_t)));
        original = original && word == 0;
        active = active && word == expected_cave[i];
    }

    if (active) {
        LOGI("Adaptive max-refresh patch already active: preferred %.3f Hz, "
             "UE3 smoothing cap %.3f FPS", preferred_refresh_rate, smoothed_limit);
        return true;
    }
    if (!original) {
        LOGE("Adaptive max-refresh patch refused: guest signature or cave padding "
             "mismatch (site=0x%08X word=0x%08X cave=0x%08X)",
             patch_site, site_word, cave_address);
        return false;
    }

    // Publish the complete cave first; redirect guest execution only after the
    // literal and return branch are visible and their JIT range is invalidated.
    for (size_t i = 0; i < std::size(expected_cave); ++i) {
        g_host->MemoryWrite32(
            cave_address + static_cast<uint32_t>(i * sizeof(uint32_t)),
            expected_cave[i]);
    }
    g_host->InvalidateGuestCode(cave_address, cfg::kRefreshCaveSize);
    g_host->MemoryWrite32(patch_site, branch_to_cave);
    g_host->InvalidateGuestCode(patch_site, sizeof(uint32_t));

    bool verified = g_host->MemoryRead32(patch_site) == branch_to_cave;
    for (size_t i = 0; i < std::size(expected_cave); ++i) {
        verified = verified &&
            g_host->MemoryRead32(
                cave_address + static_cast<uint32_t>(i * sizeof(uint32_t))) ==
                expected_cave[i];
    }
    if (!verified) {
        LOGE("Adaptive max-refresh patch write verification failed");
        return false;
    }

    LOGI("Adaptive max-refresh patch active: preferred %.3f Hz, "
         "UE3 smoothing cap %.3f FPS, site 0x%08X, cave 0x%08X",
         preferred_refresh_rate, smoothed_limit, patch_site, cave_address);
    return true;
}

static bool CopyJavaString(JNIEnv* env, jstring value, std::string& out) {
    if (!value) return false;
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return false;
    out.assign(chars);
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

static void ClearPathLookupException(JNIEnv* env, const char* operation) {
    if (!env->ExceptionCheck()) return;
    LOGE("Path lookup failed at %s", operation);
    env->ExceptionClear();
}

// A ZipResourceFile static native can be the first bridge call, in which case
// its `jclass` receiver is not an Android Context. Resolve the process
// Application without retaining it globally so path discovery is independent
// of which of the 39+7 methods happens to run first.
static jobject CurrentApplication(JNIEnv* env) {
    struct Candidate {
        const char* class_name;
        const char* method_name;
    };
    constexpr Candidate candidates[] = {
        {"android/app/ActivityThread", "currentApplication"},
        {"android/app/AppGlobals", "getInitialApplication"},
    };

    for (const Candidate& candidate : candidates) {
        jclass owner = env->FindClass(candidate.class_name);
        if (!owner) {
            ClearPathLookupException(env, candidate.class_name);
            continue;
        }
        jmethodID method = env->GetStaticMethodID(
            owner, candidate.method_name, "()Landroid/app/Application;");
        if (!method) {
            ClearPathLookupException(env, candidate.method_name);
            env->DeleteLocalRef(owner);
            continue;
        }
        jobject application = env->CallStaticObjectMethod(owner, method);
        env->DeleteLocalRef(owner);
        if (env->ExceptionCheck()) {
            ClearPathLookupException(env, candidate.method_name);
            if (application) env->DeleteLocalRef(application);
            continue;
        }
        if (application) return application; // caller owns this local ref
    }
    return nullptr;
}

// QA: debug.bl2.shader_cache_reset=1 before launch deletes every shader stage
// and pair cache (all driver identities) and the legacy program cache, so a
// first launch can be measured on a device without root access to the cache.
static void ResetBl2ShaderCachesForQa(const std::string& directory) {
    char value[PROP_VALUE_MAX] = {};
    if (directory.empty() || __system_property_get("debug.bl2.shader_cache_reset", value) <= 0 ||
        value[0] != '1') return;
    size_t removed = 0;
    if (DIR* listing = opendir(directory.c_str())) {
        std::vector<std::string> targets;
        while (const dirent* entry = readdir(listing)) {
            const std::string_view name(entry->d_name);
            if (name.starts_with("bl2-stage-cache-") || name == "bl2-program-cache.bin")
                targets.emplace_back(directory + "/" + entry->d_name);
        }
        closedir(listing);
        for (const auto& target : targets) {
            // Depth-first: files, then their directory. A failed entry is skipped.
            const int walked = nftw(target.c_str(), [](const char* path, const struct stat*, int, FTW*) {
                ::remove(path);
                return 0;
            }, 16, FTW_DEPTH | FTW_PHYS);
            if (walked == 0 && access(target.c_str(), F_OK) != 0) ++removed;
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "BL2ShaderCache", "QA cache reset removed=%zu", removed);
}

static void CaptureHostPaths(JNIEnv* env, jobject candidate_context) {
    if (!env || (!g_saved_apk_path.empty() && !g_saved_files_dir.empty() &&
                 g_split_source_dirs_captured && g_shader_cache_dir_capture_attempted &&
                 g_shader_assets_capture_attempted)) return;

    jobject context = candidate_context;
    bool owns_context = false;
    jclass context_base = env->FindClass("android/content/Context");
    const bool candidate_is_context = context_base && candidate_context &&
                                      env->IsInstanceOf(candidate_context, context_base);
    if (context_base) env->DeleteLocalRef(context_base);
    else ClearPathLookupException(env, "android/content/Context");
    if (!candidate_is_context) {
        context = CurrentApplication(env);
        owns_context = context != nullptr;
    }
    if (!context) {
        LOGE("Could not obtain an Android Context for APK path capture");
        return;
    }

    jclass context_class = env->GetObjectClass(context);
    if (!context_class) {
        ClearPathLookupException(env, "GetObjectClass(Context)");
        if (owns_context) env->DeleteLocalRef(context);
        return;
    }

    // Resolve the selected app language before any signed base/split indexing.
    // The stock LanguageSet callback keeps INT/en for the mapped resource ABI.
    jclass storage_class = env->FindClass("com/tt2kgames/Borderlands2/GameStorage");
    jmethodID app_language = storage_class ? env->GetStaticMethodID(storage_class,
        "appLanguage", "(Landroid/content/Context;)Ljava/lang/String;") : nullptr;
    jstring language = app_language ? static_cast<jstring>(
        env->CallStaticObjectMethod(storage_class, app_language, context)) : nullptr;
    std::string selected_language = "en";
    if (!env->ExceptionCheck()) CopyJavaString(env, language, selected_language);
    else ClearPathLookupException(env, "appLanguage");
    SetBl2LocaleLanguage(selected_language.c_str());
    if (language) env->DeleteLocalRef(language);
    if (storage_class) env->DeleteLocalRef(storage_class);

    if (g_saved_apk_path.empty() || !g_split_source_dirs_captured) {
        jmethodID get_application_info = env->GetMethodID(
            context_class, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
        if (get_application_info) {
            jobject application_info = env->CallObjectMethod(context, get_application_info);
            if (application_info && !env->ExceptionCheck()) {
                jclass info_class = env->GetObjectClass(application_info);
                if (info_class && g_saved_apk_path.empty()) {
                    jfieldID source_dir = env->GetFieldID(
                        info_class, "sourceDir", "Ljava/lang/String;");
                    jstring source = source_dir
                        ? static_cast<jstring>(env->GetObjectField(application_info, source_dir))
                        : nullptr;
                    if (!env->ExceptionCheck()) CopyJavaString(env, source, g_saved_apk_path);
                    if (source) env->DeleteLocalRef(source);
                }
                if (env->ExceptionCheck()) ClearPathLookupException(env, "sourceDir");
                if (info_class && !g_split_source_dirs_captured) {
                    jfieldID split_dirs = env->GetFieldID(
                        info_class, "splitSourceDirs", "[Ljava/lang/String;");
                    if (!split_dirs) {
                        ClearPathLookupException(env, "splitSourceDirs");
                    } else {
                        jobjectArray splits = static_cast<jobjectArray>(
                            env->GetObjectField(application_info, split_dirs));
                        bool complete = !env->ExceptionCheck();
                        jsize count = 0;
                        if (complete && splits) {
                            count = env->GetArrayLength(splits);
                            complete = !env->ExceptionCheck() && count <= 64;
                        }
                        std::vector<std::string> paths;
                        if (complete) {
                            paths.reserve(static_cast<size_t>(count));
                            for (jsize i = 0; i < count; ++i) {
                                jstring item = static_cast<jstring>(
                                    env->GetObjectArrayElement(splits, i));
                                if (env->ExceptionCheck()) {
                                    complete = false;
                                    break;
                                }
                                std::string path;
                                const bool copied = CopyJavaString(env, item, path);
                                if (item) env->DeleteLocalRef(item);
                                if (env->ExceptionCheck()) {
                                    complete = false;
                                    break;
                                }
                                if (copied && !path.empty() && path != g_saved_apk_path &&
                                    std::find(paths.begin(), paths.end(), path) == paths.end()) {
                                    paths.push_back(std::move(path));
                                }
                            }
                        }
                        if (splits) env->DeleteLocalRef(splits);
                        if (complete) {
                            g_saved_split_apk_paths = std::move(paths);
                            g_split_source_dirs_captured = true;
                        } else {
                            ClearPathLookupException(env, "splitSourceDirs contents");
                            LOGE("Could not capture complete splitSourceDirs (count=%d)",
                                 static_cast<int>(count));
                        }
                    }
                }
                if (info_class) env->DeleteLocalRef(info_class);
            }
            if (application_info) env->DeleteLocalRef(application_info);
        }
    }

    if (g_saved_files_dir.empty() && !env->ExceptionCheck()) {
        jmethodID get_files_dir = env->GetMethodID(
            context_class, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
        if (get_files_dir) {
            jobject files_dir = env->CallObjectMethod(context, get_files_dir, nullptr);
            if (files_dir && !env->ExceptionCheck()) {
                jclass file_class = env->GetObjectClass(files_dir);
                jmethodID absolute_path = file_class
                    ? env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;")
                    : nullptr;
                jstring path = absolute_path
                    ? static_cast<jstring>(env->CallObjectMethod(files_dir, absolute_path))
                    : nullptr;
                if (!env->ExceptionCheck()) CopyJavaString(env, path, g_saved_files_dir);
                if (path) env->DeleteLocalRef(path);
                if (file_class) env->DeleteLocalRef(file_class);
            }
            if (files_dir) env->DeleteLocalRef(files_dir);
        }
    }

    // Keep disposable binaries outside the legacy player files scanned by
    // GameStorage. Internal files still provide the existing save fallback.
    const auto capture_directory = [&](const char* method, std::string& destination) {
        if (env->ExceptionCheck()) return;
        jmethodID get_directory = env->GetMethodID(
            context_class, method, "()Ljava/io/File;");
        if (get_directory) {
            jobject files_dir = env->CallObjectMethod(context, get_directory);
            if (files_dir && !env->ExceptionCheck()) {
                jclass file_class = env->GetObjectClass(files_dir);
                jmethodID absolute_path = file_class
                    ? env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;")
                    : nullptr;
                jstring path = absolute_path
                    ? static_cast<jstring>(env->CallObjectMethod(files_dir, absolute_path))
                    : nullptr;
                if (!env->ExceptionCheck()) CopyJavaString(env, path, destination);
                if (path) env->DeleteLocalRef(path);
                if (file_class) env->DeleteLocalRef(file_class);
            }
            if (files_dir) env->DeleteLocalRef(files_dir);
        }
        if (env->ExceptionCheck()) ClearPathLookupException(env, method);
    };
    if (g_saved_files_dir.empty()) capture_directory("getFilesDir", g_saved_files_dir);
    if (!g_shader_cache_dir_capture_attempted) {
        capture_directory("getCacheDir", g_bl2_shader_cache_dir);
        ResetBl2ShaderCachesForQa(g_bl2_shader_cache_dir);
    }
    // Cache storage is optional; a failed capture must not add JNI work to
    // every later guest entry. An empty directory retains source-only warmup.
    g_shader_cache_dir_capture_attempted = true;

    if (!g_shader_assets_capture_attempted && !env->ExceptionCheck()) {
        const jmethodID get_assets = env->GetMethodID(
            context_class, "getAssets", "()Landroid/content/res/AssetManager;");
        jobject assets = get_assets ? env->CallObjectMethod(context, get_assets) : nullptr;
        if (assets && !env->ExceptionCheck()) {
            jobject retained = env->NewGlobalRef(assets);
            AAssetManager* manager = retained ? AAssetManager_fromJava(env, retained) : nullptr;
            if (manager && !env->ExceptionCheck()) {
                g_shader_assets_owner = retained;
                g_shader_assets = manager;
            } else if (retained) env->DeleteGlobalRef(retained);
        }
        if (assets) env->DeleteLocalRef(assets);
        if (env->ExceptionCheck()) ClearPathLookupException(env, "getAssets");
        // Cache preparation is optional; missing assets retain genuine lazy
        // monolithic compilation without repeated Context work on guest entry.
        g_shader_assets_capture_attempted = true;
    }

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(context_class);
    if (owns_context) env->DeleteLocalRef(context);
    LOGI("Captured paths: apk='%s' files='%s' split_capture=%d split_count=%zu",
         g_saved_apk_path.c_str(), g_saved_files_dir.c_str(),
         g_split_source_dirs_captured ? 1 : 0, g_saved_split_apk_paths.size());
    for (size_t i = 0; i < g_saved_split_apk_paths.size(); ++i) {
        LOGI("Captured splitSourceDirs[%zu]: '%s'", i,
             g_saved_split_apk_paths[i].c_str());
    }
}

static bool IsSupportedGuestJniVersion(jint version) {
    return version == JNI_VERSION_1_1 || version == JNI_VERSION_1_2 ||
           version == JNI_VERSION_1_4 || version == JNI_VERSION_1_6;
}

static void ShaderPreparationProgress(uint32_t completed, uint32_t total) {
    if (!g_jvm || !g_shader_progress_class || !g_shader_progress_method || total > 1000000) return;
    JNIEnv* env = nullptr;
    const bool attach = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attach && g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    if (!env) return;
    if (!env->ExceptionCheck()) {
        // LaunchControls posts to the UI Handler; it never waits for the
        // renderer or calls the guest while the shader mutex is held.
        env->CallStaticVoidMethod(g_shader_progress_class, g_shader_progress_method,
                                  jint(std::min(completed, total)), jint(total));
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("Shader preparation progress failed");
        }
    }
    if (attach) g_jvm->DetachCurrentThread();
}

bool PrepareBl2ShaderStageCache(Bl2ShaderStageCache& cache) {
    if (!g_shader_assets) return false;
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(g_shader_assets, "bl2_shader_stages.bin", AASSET_MODE_STREAMING), AAsset_close);
    if (!asset) return false;
    const auto length = AAsset_getLength64(asset.get());
    if (length <= 0 || length > 256LL * 1024 * 1024) return false;
    const Bl2ShaderStageCache::Reader reader{
        asset.get(), static_cast<uint64_t>(length),
        [](void* opaque, uint8_t* bytes, size_t wanted) -> size_t {
            const int read = AAsset_read(static_cast<AAsset*>(opaque), bytes,
                                        std::min<size_t>(wanted, INT_MAX));
            return read > 0 ? static_cast<size_t>(read) : 0;
        },
        [](void* opaque) { return AAsset_seek64(static_cast<AAsset*>(opaque), 0, SEEK_SET) == 0; }
    };
    const bool prepared = cache.PrepareStream(reader, g_bl2_shader_cache_dir,
        [](void*, uint32_t completed, uint32_t total) {
            ShaderPreparationProgress(completed, total);
        });
    ShaderPreparationProgress(0, 0);
    return prepared;
}

// Each call opens its own streaming reader (the former's producer thread).
static bool OpenShaderFormerAsset(const char* name, Bl2ShaderFormerAsset* asset) {
    if (!g_shader_assets || !asset) return false;
    AAsset* opened = AAssetManager_open(g_shader_assets, name, AASSET_MODE_STREAMING);
    if (!opened) return false;
    const auto length = AAsset_getLength64(opened);
    if (length <= 0) {
        AAsset_close(opened);
        return false;
    }
    asset->opaque = opened;
    asset->bytes = static_cast<uint64_t>(length);
    asset->read = [](void* opaque, uint8_t* bytes, size_t wanted) -> size_t {
        const int read = AAsset_read(static_cast<AAsset*>(opaque), bytes,
                                     std::min<size_t>(wanted, INT_MAX));
        return read > 0 ? static_cast<size_t>(read) : 0;
    };
    asset->close = [](void* opaque) { AAsset_close(static_cast<AAsset*>(opaque)); };
    return true;
}

// The touch overlay's main-menu progress line: per mille of the catalogue with
// a binary, -1 once formation has stopped.
static std::int32_t ShaderFormationPermille() {
    uint32_t ready = 0, total = 0;
    if (!Bl2ShaderFormerProgress(&ready, &total)) return -1;
    return static_cast<std::int32_t>(uint64_t{ready} * 1000 / total);
}

// Renderer thread, first guest shader stage. The default opens the cache from
// the catalogue header alone and leaves any formation to background workers:
// the game never waits for it. debug.bl2.shader_former=full keeps the old pass.
bool OpenBl2ShaderStageCache(Bl2ShaderStageCache& cache) {
    const auto mode = Bl2ShaderFormerConfiguredMode();
    if (mode == Bl2ShaderFormerMode::Blocking) return PrepareBl2ShaderStageCache(cache);
    if (!g_shader_assets) return false;
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(g_shader_assets, "bl2_shader_stages.bin", AASSET_MODE_STREAMING), AAsset_close);
    if (!asset) return false;
    const auto length = AAsset_getLength64(asset.get());
    if (length <= 0 || length > 256LL * 1024 * 1024) return false;
    const Bl2ShaderStageCache::Reader reader{
        asset.get(), static_cast<uint64_t>(length),
        [](void* opaque, uint8_t* bytes, size_t wanted) -> size_t {
            const int read = AAsset_read(static_cast<AAsset*>(opaque), bytes,
                                        std::min<size_t>(wanted, INT_MAX));
            return read > 0 ? static_cast<size_t>(read) : 0;
        },
        [](void* opaque) { return AAsset_seek64(static_cast<AAsset*>(opaque), 0, SEEK_SET) == 0; }
    };
    if (!cache.OpenStream(reader, g_bl2_shader_cache_dir)) return false;
    if (!cache.Complete() && mode == Bl2ShaderFormerMode::Background) {
        Bl2ShaderFormerSetGameplayProbe(Bl2TouchPlayerInWorld);
        if (Bl2ShaderFormerStart(&cache, eglGetCurrentDisplay(), OpenShaderFormerAsset,
                                 Bl2ShaderFormerConfiguredThreads()))
            bl2::touch::setShaderFormationProgress(ShaderFormationPermille);
    }
    return true;
}

// DISPLAY limit plus debug.bl2.fps_limit=900: uncapped and unpaced, for
// throughput measurements.
static bool Bl2UncappedDiagnostic(uint32_t requested) {
    char diagnostic[PROP_VALUE_MAX] = {};
    return requested == 0 &&
        __system_property_get("debug.bl2.fps_limit", diagnostic) > 0 &&
        std::strcmp(diagnostic, "900") == 0;
}

static bool InstallBl2MaxTickRatePatch(float display_refresh_rate) {
    ElfLoader& loader = g_host->GetElfLoader();
    if (loader.GetImageSize() != 0x03089000) return true;
    if (!std::isfinite(display_refresh_rate) ||
        display_refresh_rate <= 0.0f ||
        display_refresh_rate > cfg::kMaximumSaneDisplayRate) {
        LOGE("BL2 max tick patch refused: invalid display refresh %.3f Hz",
             display_refresh_rate);
        return false;
    }

    const uint32_t requested = bl2::touch::Bl2FrameRateLimit();
    if (requested != 0 && requested != 30 && requested != 60 && requested != 90) return false;
    const bool uncapped_diagnostic = Bl2UncappedDiagnostic(requested);
    const bool paced = requested != 0 && Bl2FramePacerActive();
    const float limit = requested ?
        static_cast<float>(requested) * (paced ? cfg::kBl2PacedTickHeadroom : 1.0f) :
        uncapped_diagnostic ? cfg::kBl2DiagnosticFpsLimit : display_refresh_rate;
    uint32_t limit_bits = 0;
    std::memcpy(&limit_bits, &limit, sizeof(limit_bits));
    const uint32_t address = Sym(cfg::kBl2GameGetMaxTickRate);
    if (address != loader.GetBaseAddress() + cfg::kBl2MaxTickRateRva) {
        LOGE("BL2 max tick patch refused: symbol RVA mismatch 0x%08X", address);
        return false;
    }
    const uint32_t replacement[] = {
        cfg::kBl2MaxTickRateLoad, cfg::kBl2MaxTickRateReturn, limit_bits,
    };
    bool original = true;
    bool active = g_bl2_tick_limit_bits != 0;
    for (size_t i = 0; i < std::size(replacement); ++i) {
        const uint32_t word = g_host->MemoryRead32(address + i * 4);
        original &= word == cfg::kBl2MaxTickRateExpected[i];
        active &= word == (i == 2 ? g_bl2_tick_limit_bits : replacement[i]);
    }
    if (!original && !active) {
        LOGE("BL2 max tick patch refused: guest prologue mismatch");
        return false;
    }
    if (original) {
        // Publish the literal first, then the return and entry instruction.
        g_host->MemoryWrite32(address + 8, replacement[2]);
        g_host->MemoryWrite32(address + 4, replacement[1]);
        g_host->MemoryWrite32(address, replacement[0]);
        g_host->InvalidateGuestCode(address, sizeof(replacement));
    } else if (g_bl2_tick_limit_bits != limit_bits) {
        g_host->MemoryWrite32(address + 8, replacement[2]);
        g_host->InvalidateGuestCode(address, sizeof(replacement));
    }
    for (size_t i = 0; i < std::size(replacement); ++i) {
        if (g_host->MemoryRead32(address + i * 4) != replacement[i]) {
            LOGE("BL2 max tick patch write verification failed at word %zu", i);
            return false;
        }
    }
    if (original || g_bl2_tick_limit_bits != limit_bits) {
        __android_log_print(ANDROID_LOG_INFO, "BL2_Parity",
            "ARM64 guest max tick limit %.3f FPS display=%.3f Hz requested=%u diagnostic=%d "
            "paced=%d",
            limit, display_refresh_rate, requested, uncapped_diagnostic, paced);
    }
    g_bl2_tick_limit_bits = limit_bits;
    return true;
}

bool ApplyBl2FrameRateLimit(DynarmicHost& host) {
    if (!g_bl2_tick_patch_ready.load(std::memory_order_acquire)) return true;
    if (g_host.get() != &host) return false;
    // A ready host is pinned for process lifetime. The present boundary already
    // runs on its guest core; only this recursive host lock is needed here.
    std::lock_guard<std::recursive_mutex> host_lock(host.GetMutex());
    const float refresh = g_pending_display_refresh_rate.load(std::memory_order_acquire);
    const bool applied = InstallBl2MaxTickRatePatch(refresh);
    if (applied && g_bl2_tick_limit_bits) {
        float limit = 0.0f;
        std::memcpy(&limit, &g_bl2_tick_limit_bits, sizeof(limit));
        const uint32_t requested = bl2::touch::Bl2FrameRateLimit();
        const Bl2FramePace pace = Bl2FramePacerConfigure(
            refresh, requested,
            bl2::touch::Bl2FramePacingEnabled() && !Bl2UncappedDiagnostic(requested));
        // Paced: the CPU hint aims at the next faster pace, so that pace can
        // still be measured as reachable; hitches are judged by the shown one.
        const bool paced = pace.paced_fps > 0.0f;
        Bl2PerfHintFrame(paced ? pace.aim_fps : limit);
        Bl2HitchSetFrameLimit(paced ? pace.paced_fps : limit);
    }
    return applied;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_tt2kgames_Borderlands2_DisplayModePolicy_nativeRefreshRate(
    JNIEnv*, jclass, jfloat refresh_rate) {
    if (!std::isfinite(refresh_rate) || refresh_rate <= 0.0f ||
        refresh_rate > cfg::kMaximumSaneDisplayRate) return JNI_FALSE;
    DynarmicHost* host = nullptr;
    {
        std::lock_guard<std::recursive_mutex> load_lock(g_load_mutex);
        if (g_guest_load_state == GuestLoadState::Fatal) return JNI_FALSE;
        g_pending_display_refresh_rate.store(refresh_rate, std::memory_order_release);
        if (!g_bl2_tick_patch_ready.load(std::memory_order_acquire)) return JNI_TRUE;
        if (g_guest_load_state != GuestLoadState::Ready || !g_host) return JNI_FALSE;
        // A Ready host is pinned for the process lifetime; it cannot be reset.
        host = g_host.get();
    }
    std::lock_guard<std::recursive_mutex> host_lock(host->GetMutex());
    return InstallBl2MaxTickRatePatch(
        g_pending_display_refresh_rate.load(std::memory_order_acquire))
        ? JNI_TRUE : JNI_FALSE;
}

static bool InstallOfflineDlcEnumerationPatch(DynarmicHost& host) {
    // This port has no downloadable add-on payload or store. Keep the UE3
    // asynchronous task and its completion delegate, but finish its worker
    // with an empty successful result instead of querying Steam/GPGS DLC.
    constexpr char kSymbol[] =
        "_ZN39FSteamworksReadDownloadableContentAsync6DoWorkEv";
    constexpr uint32_t kRva = 0x01F5FE94;
    constexpr uint32_t kImageSize = 0x03089000;
    constexpr uint32_t kOriginal[] = {
        0xE92D4FF0, 0xE24DDF4F, 0xE59F52A8,
        0xE58D0018, 0xE59F62A4, 0xE08F5005,
    };
    // r0 is the worker object; [r0] owns the async completion word at +24.
    // Preserve the original return value (the owner pointer). The thread-pool
    // path still invokes ProcessAsyncResults and its success delegates.
    constexpr uint32_t kEmptySuccess[] = {
        0xE5901000, // ldr r1, [r0]
        0xE3A02000, // mov r2, #0 (success)
        0xE5812018, // str r2, [r1, #24]
        0xF57FF05F, // dmb sy
        0xE1A00001, // mov r0, r1
        0xE12FFF1E, // bx lr
    };
    const auto& elf = host.GetElfLoader();
    const uint32_t site = elf.GetBaseAddress() + kRva;
    if (elf.GetImageSize() != kImageSize ||
        elf.GetSymbolAddress(kSymbol) != site) {
        LOGE("Offline DLC enumeration patch skipped: guest identity mismatch");
        return false;
    }
    bool original = true;
    bool active = true;
    for (size_t i = 0; i < std::size(kOriginal); ++i) {
        const uint32_t current = host.MemoryRead32(site + uint32_t(i * 4));
        original &= current == kOriginal[i];
        active &= current == kEmptySuccess[i];
    }
    if (!original && !active) {
        LOGE("Offline DLC enumeration patch skipped: instruction mismatch at 0x%08X", site);
        return false;
    }
    if (original) {
        for (size_t i = 0; i < std::size(kEmptySuccess); ++i)
            host.MemoryWrite32(site + uint32_t(i * 4), kEmptySuccess[i]);
        host.InvalidateGuestCode(site, sizeof(kEmptySuccess));
    }
    for (size_t i = 0; i < std::size(kEmptySuccess); ++i) {
        if (host.MemoryRead32(site + uint32_t(i * 4)) != kEmptySuccess[i]) {
            LOGE("Offline DLC enumeration patch verification failed at 0x%08X", site);
            return false;
        }
    }
    LOGI("Offline DLC enumeration patch active at 0x%08X", site);
    return true;
}

static bool InstallBl2AstcBlockBytes(DynarmicHost& host) {
    if (!kBl2Astc4Textures) return true;
    auto& loader = host.GetElfLoader();
    constexpr uint32_t table_rva = 0x02F468EC;
    constexpr uint32_t table_bytes = 28 * 40;
    const uint32_t table = loader.GetSymbolAddress("GPixelFormats");
    if (loader.GetImageSize() != 0x03089000u ||
        uint64_t(loader.GetBaseAddress()) + loader.GetImageSize() > host.GetGuestMemorySize() ||
        table_rva + table_bytes > loader.GetImageSize() ||
        table != loader.GetBaseAddress() + table_rva) {
        LOGE("ASTC4 texture allocation refused: pinned GPixelFormats mismatch");
        return false;
    }
    const uint32_t dxt1 = table + 5 * 40;
    const uint32_t dxt5 = table + 7 * 40;
    constexpr uint32_t expected[2][9] = {
        {4, 4, 1, 8, 3, 0, 0, 1, 5},
        {4, 4, 1, 16, 4, 0, 0, 1, 7},
    };
    unsigned row = 0;
    for (const uint32_t entry : {dxt1, dxt5}) {
        for (unsigned word = 0; word < 9; ++word) {
            const uint32_t value = host.MemoryRead32(entry + (word + 1) * 4);
            if (value != expected[row][word] && !(row == 0 && word == 3 && value == 16)) {
                LOGE("ASTC4 texture allocation refused: record%u word%u value=%u",
                     row, word, value);
                return false;
            }
        }
        ++row;
    }
    const uint32_t original = host.MemoryRead32(dxt1 + 16);
    if ((original != 8 && original != 16) || host.MemoryRead32(dxt5 + 16) != 16) {
        LOGE("ASTC4 texture allocation refused: block-byte preimage mismatch");
        return false;
    }
    host.MemoryWrite32(dxt1 + 16, 16);
    if (host.MemoryRead32(dxt1 + 16) != 16) return false;
    LOGI("ASTC4 allocation: GPixelFormats PF_DXT1 bytes=%u->16 PF_DXT5=16; IDs/channels unchanged",
         original);
    return true;
}

// The local profile must exist before GameViewport saves its initial login
// state. A late first sign-in marks that baseline dirty and WillowPlayerController
// treats ordinary local initialization as a loss of online login.
static bool InstallBl2LocalSessionPatch(DynarmicHost& host) {
    auto& loader = host.GetElfLoader();
    const auto base = loader.GetBaseAddress();
    const auto sign_in = loader.GetSymbolAddress("_ZN26UOnlineSubsystemSteamworks13SignInLocallyEv");
    const auto signed_in = loader.GetSymbolAddress("_Z23AndroidSteam_IsSignedInv");
    const auto connected = loader.GetSymbolAddress("_Z24AndroidSteam_IsConnectedv");
    if (loader.GetImageSize() != 0x03089000 ||
        loader.GetSymbolAddress("_ZN26UOnlineSubsystemSteamworks4InitEv") != base + 0x01F5AEE4 ||
        loader.GetSymbolAddress("_ZN26UOnlineSubsystemSteamworks26OnSteamServersDisconnectedEP26SteamServersDisconnected_t") != base + 0x01F55990 ||
        sign_in != base + 0x01F55934 || signed_in != base + 0x01F529AC || connected != base + 0x01F52948)
        return false;
    constexpr std::pair<uint32_t,uint32_t> preimage[] = {
        {0x01F5B4E4,0xE59440F0}, {0x01F5B4E8,0xE59D0040},
        {0x01F5B4EC,0xE2944000}, {0x01F5B4F0,0x13A04001},
        {0x01F5B500,0xE1A00004}, {0x01F5B504,0xE28DD084}, {0x01F5B508,0xE8BD8FF0},
        {0x01F55A94,0xE2509000}, {0x01F55A98,0x1A000039},
        {0x01F55A9C,0xE5D83111}, {0x01F55AA0,0xE3530000}, {0x01F55AA4,0x0A000038},
        {0x01F55AA8,0xE5941064}, {0x01F55B84,0xE28DD040}, {0x01F55B88,0xE8BD87F0},
        {0x01F55B8C,0xE1A00008}, {0x01F55B90,0xEBFFFF67},
    };
    for (const auto& [rva, word] : preimage)
        if (!loader.IsReadOnlyRange(base + rva,4) || host.MemoryRead32(base + rva) != word) return false;
    const auto initialize = host.GetThunkManager().RegisterThunk("bl2_initialize_local_session",
        [sign_in,signed_in,connected](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
            const auto owner = jit.Regs()[4];
            if (!g_local_identity_prepared.load(std::memory_order_acquire) || owner < 0x10000 ||
                uint64_t{owner} + 0x4D8 > current.GetGuestMemorySize() ||
                current.MemoryRead8(owner + 0x111) != 0 ||
                !current.RunFunction(signed_in,{}) || current.RunFunction(connected,{})) return;
            current.RunFunction(sign_in,{owner});
            __android_log_print(ANDROID_LOG_INFO,"Arm32Bridge_LocalSession",
                "Local session baseline initialized before viewport: status=%u remote=0",
                unsigned(current.MemoryRead8(owner + 0x111)));
        });
    if (!initialize) return false;
    const auto svc = host.MemoryRead32(initialize);
    if ((svc & 0xFF000000u) != 0xEF000000u) return false;
    const uint32_t init_words[] = {svc,0xE59440F0,0xE59D0040,0xE51FF004,base + 0x01F5B4EC};
    // BLoggedOn was already false here. Preserve initial 0->local login and
    // genuine 2->1 disconnects; an existing local 1 has no login status change.
    const uint32_t disconnect_words[] = {
        0xE5D83111,0xE3530001,0x059FF00C,0xE3530000,0x059FF008,0xE51FF004,
        base + 0x01F55AA8,base + 0x01F55B84,base + 0x01F55B8C};
#if defined(BL2_NATIVE_ARM32)
    const auto init_veneer = host.CreateCodeVeneer(init_words);
    const auto disconnect_veneer = host.CreateCodeVeneer(disconnect_words);
#else
    const auto allocate = [&host](const uint32_t* words, size_t count) {
        const auto address = host.GuestAlloc(static_cast<uint32_t>(count * 4),4);
        if (address) {
            for (size_t i = 0; i < count; ++i) host.MemoryWrite32(address + uint32_t(i * 4),words[i]);
            host.AddExecutableRange(address,static_cast<uint32_t>(count * 4));
            host.InvalidateGuestCode(address,static_cast<uint32_t>(count * 4));
        }
        return address;
    };
    const auto init_veneer = allocate(init_words,std::size(init_words));
    const auto disconnect_veneer = allocate(disconnect_words,std::size(disconnect_words));
#endif
    if (!init_veneer || !disconnect_veneer) return false;
    for (const auto& [site,veneer] : {std::pair{base + 0x01F5B4E4,init_veneer},
                                   std::pair{base + 0x01F55A9C,disconnect_veneer}}) {
        host.MemoryWrite32(site,0xE51FF004); host.MemoryWrite32(site + 4,veneer);
        host.InvalidateGuestCode(site,8);
        if (host.MemoryRead32(site) != 0xE51FF004 || host.MemoryRead32(site + 4) != veneer) return false;
    }
    return true;
}

static bool EnsureGameLoaded(JNIEnv* env, jobject context) {
    std::lock_guard<std::recursive_mutex> load_lock(g_load_mutex);
    CaptureHostPaths(env, context);
    switch (g_guest_load_state) {
        case GuestLoadState::Ready:
            FakeJniEnv::Instance().AttachHostEnv(env);
            return g_host != nullptr;
        case GuestLoadState::Loading:
            LOGE("Re-entrant guest call while guest JNI_OnLoad is still running");
            return false;
        case GuestLoadState::Fatal:
            return false;
        case GuestLoadState::Cold:
            break;
    }

    g_guest_load_state = GuestLoadState::Loading;
    g_host = std::make_unique<DynarmicHost>();
    if (!g_host->Initialize()) {
        LOGE("Failed to initialize Dynarmic host");
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }

    // app/ is the sole source of truth for this restoration. Never let a stale
    // loose payload in filesDir, /data/local/tmp or /sdcard shadow the guest that
    // was hashed and packaged into this APK.
    bool loaded = false;
    if (!g_saved_apk_path.empty()) {
        // Asset entries are indexed independently; the guest ELF itself is
        // mapped from a STORED APK entry and never inflated/copied wholesale.
        if (IndexApk(g_saved_apk_path.c_str()) != 0) {
            LOGE("APK asset index failed: %s", g_saved_apk_path.c_str());
            g_host.reset();
            g_guest_load_state = GuestLoadState::Fatal;
            return false;
        }
        IndexBl2CacheBase(g_saved_apk_path.c_str());
        if (g_split_source_dirs_captured) {
            std::vector<const char*> split_paths;
            split_paths.reserve(g_saved_split_apk_paths.size());
            for (const auto& path : g_saved_split_apk_paths) {
                split_paths.push_back(path.c_str());
            }
            if (IndexObbSplitApks(split_paths.data(), split_paths.size()) != 0) {
                // An incompatible/incomplete embedded set must not shadow the
                // existing external path. Downloader checks its names/sizes;
                // the original guest parses FOOB without a full-content hash.
                LOGE("No usable embedded OBB pair; retaining external OBB fallback");
            }
            IndexBl2LocaleSplits(split_paths.data(), split_paths.size(), g_saved_files_dir.c_str());
        } else {
            IndexBl2LocaleSplits(nullptr, 0, g_saved_files_dir.c_str());
            LOGE("Split-source capture incomplete; embedded OBBs unavailable");
        }
        GuestMmapView guest_view;
        if (MmapGuestFromApk(g_saved_apk_path, ApkCandidates(cfg::kGuestLib),
                             guest_view)) {
            // One SHA-256 of the 49.9 MB ELF serves every module's identity check.
            const ObbSha256::MappingScope guest_digest(guest_view.data, guest_view.size);
            VerifyBl2TouchInput(guest_view.data, guest_view.size);
            VerifyBl2PauseDebugMenuInput(guest_view.data, guest_view.size);
            VerifyBl2IoFailureProbeInput(guest_view.data, guest_view.size);
            VerifyBl2LocaleGuest(guest_view.data, guest_view.size);
            loaded = VerifyBl2NativeResolutionInput(guest_view.data, guest_view.size) &&
                g_host->LoadGameFromMemory(guest_view.data, guest_view.size);
            LOGI("Load mapped ELF from APK: %s", loaded ? "ok" : "failed");
            CloseGuestMmap(guest_view);
        }
    }

    if (!loaded) {
        // Drop the half-built host: callers guard on g_host and then go straight
        // to GetElfLoader(), which would dereference a null m_elf_loader.
        LOGE("Could not locate or extract %s", cfg::kGuestLib);
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }

    if (!InstallBl2AstcBlockBytes(*g_host) || !InstallBl2NativeResolutionPatch(*g_host) ||
        !InstallBl2FreshPersistentBufferNames(*g_host)) {
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    if (!InstallBl2LocalSessionPatch(*g_host)) {
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    if (!InstallBl2PauseDebugMenu(*g_host)) {
        LOGE("Pause Debug visibility unavailable; retaining original pause menu");
    }
    if (!InstallOfflineDlcEnumerationPatch(*g_host)) {
        LOGE("Offline DLC enumeration optimization unavailable; retaining guest path");
    }
    if (!InstallBl2LocaleOverlay(*g_host)) {
        LOGE("Optional ru_text overlay unavailable; retaining English text");
    }
    if (!InstallBl2TouchRuntime(*g_host)) {
        LOGE("Native touch observer unavailable; retaining launch controls");
    }
    if (!InstallHornReferenceAspectPatch()) {
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    if (!InstallDiagnosticGFxEntryTrace()) {
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    if (!InstallTegra4GraphicsSelectionPatch()) {
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    if (!InstallBl2GraphicsSettings(*g_host)) {
        LOGE("BL2 original graphics settings hooks failed");
        g_host.reset();g_guest_load_state = GuestLoadState::Fatal;return false;
    }
    if (!InstallBorderlandsNvsiLifecycle(*g_host) ||
        !InstallBl2RenderWithoutNvsi(*g_host)) {
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    // Large guest blocks get their own coalescing range (dynarmic_host.h);
    // before the fast paths, so their heap checks see the final layout.
    if (!g_host->EnableLargeBlockRegion())
        LOGE("Large guest block range unavailable; shared heap only");
    // This optimization is deliberately optional: exact symbol/table
    // signatures and a differential run against the unmodified guest must all
    // pass before its entry point is redirected.
    InstallHornFastPaths(*g_host);
    if (!InstallBl2VertexElementFastpath(*g_host)) {
        LOGE("Optional BL2 vertex fast path unavailable; retaining guest path");
    }
    if (!InstallBl2ConvexFastpath(*g_host,kBl2PerformanceDefaults&7)) {
        LOGE("Optional BL2 convex fast path unavailable; retaining guest path");
    }
    if (!InstallBl2LimiterPeakFastpath(*g_host,(kBl2PerformanceDefaults&32)!=0)) {
        LOGE("Optional BL2 limiter peak fast path unavailable; retaining guest path");
    }
    if (!InstallBl2ProgramLocalsFastpath(*g_host,(kBl2PerformanceDefaults&16)!=0)) {
        LOGE("Optional BL2 program locals fast path unavailable; retaining guest path");
    }
    if (!InstallBl2SetupVertexArraysFastpath(*g_host,(kBl2PerformanceDefaults&8)!=0)) {
        LOGE("Optional BL2 setup arrays fast path unavailable; retaining guest path");
    }
    if (!InstallBl2AudioDecodeFastpaths(*g_host,(kBl2PerformanceDefaults>>6)&3)) {
        LOGE("Optional BL2 audio decode fast path unavailable; retaining guest path");
    }
    if (!InstallBl2MovieSkip(*g_host)) {
        LOGE("Movie skip unavailable: unexpected FFullScreenMovieBink code");
    }
    if (!InstallBl2StartupScreens(*g_host)) {
        LOGE("Startup screen skip unavailable: unexpected registry getter code");
    }
    // Last: snapshots RELRO after every bridge patch (bl2_guest_integrity.h).
    StartBl2GuestIntegrityWatch(*g_host);
    // The original ELF passed VerifyBl2NativeResolutionInput before loading;
    // all code patches are now installed, before Native_Render starts its loop.
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    g_host->InstallPhysxCookieWatch(true);
#endif
    if (!g_host->InstallJitWarmupTable(LoadBl2JitWarmupTable()))
        LOGI("JIT warmup unavailable; retaining ordinary compilation");
    else {
        SeedBl2LearnedJitProfile(g_saved_files_dir);
        g_host->LoadLearnedJitProfile(g_saved_files_dir);
    }
    // Resolve this before FakeJniEnv is bound to the host. A missing symbol is
    // permanent for the loaded payload, but it must not leave FakeJniEnv holding
    // guest addresses from a host that is then destroyed.
    uint32_t guest_jni_on_load = Sym(cfg::kGuestJniOnLoad);
    if (!guest_jni_on_load) {
        g_host.reset();
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }

    // The guest gets its JNIEnv/JavaVM tables from here, not from the real ART ones:
    // it is 32-bit and cannot call a 64-bit function pointer.
    FakeJniEnv& fj = FakeJniEnv::Instance();
    fj.SetJavaVM(g_jvm);
    fj.Init(*g_host);
    PinHostForProcessLifetime();
    fj.AttachHostEnv(env);

    const jint guest_jni_version = static_cast<jint>(g_host->RunFunction(
        guest_jni_on_load, {fj.VmTableGuestAddr(), 0}));
    if (env->ExceptionCheck()) {
        LOGE("Guest JNI_OnLoad left a pending Java exception");
        env->ExceptionDescribe();
        env->ExceptionClear();
        // Keep g_host alive: FakeJniEnv now owns addresses inside its guest
        // memory. Retrying with a replacement host would make those stale.
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    if (!IsSupportedGuestJniVersion(guest_jni_version)) {
        LOGE("Guest JNI_OnLoad failed: 0x%08X", guest_jni_version);
        g_guest_load_state = GuestLoadState::Fatal;
        return false;
    }
    LOGI("Guest JNI_OnLoad returned 0x%08X", guest_jni_version);
    g_guest_load_state = GuestLoadState::Ready;
    return true;
}

// Guest JNIEnv* argument for a Java_* entry point.
static uint32_t FakeEnv() { return FakeJniEnv::Instance().EnvTableGuestAddr(); }

// ---------------------------------------------------------------------------
// Horn JNI bridge. The guest uses ARM32 softfp/AAPCS, so every Java argument is
// converted to a 32-bit word explicitly. 64-bit values start at an even word.
// ---------------------------------------------------------------------------

static uint32_t FloatBits(jfloat value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t JLongBits(jlong value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint32_t JLongLow(jlong value) {
    return static_cast<uint32_t>(JLongBits(value));
}

static uint32_t JLongHigh(jlong value) {
    return static_cast<uint32_t>(JLongBits(value) >> 32);
}

static jlong JLongResult(uint64_t bits) {
    jlong value = 0;
    static_assert(sizeof(bits) == sizeof(value));
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t GuestObject(jobject value) {
    return value ? FakeJniEnv::Instance().AddObjectRef(value) : 0;
}

static uint32_t GuestSymbol(const char* name) {
    uint32_t address = g_host ? g_host->FindSymbol(name) : 0;
    if (!address) LOGE("Required guest entry point missing: %s", name);
    return address;
}

static bool PrepareGuestCall(JNIEnv* env, jobject receiver) {
    if (!EnsureGameLoaded(env, receiver)) return false;
    FakeJniEnv::Instance().AttachHostEnv(env);
    return true;
}

static std::vector<uint32_t> GuestArgs(jobject receiver,
                                       std::initializer_list<uint32_t> tail) {
    std::vector<uint32_t> args;
    args.reserve(2 + tail.size());
    args.push_back(FakeEnv());
    args.push_back(GuestObject(receiver));
    args.insert(args.end(), tail.begin(), tail.end());
    return args;
}

static uint32_t RunGuest32(JNIEnv* env, jobject receiver, const char* symbol,
                           std::initializer_list<uint32_t> tail = {}) {
    if (!PrepareGuestCall(env, receiver)) return 0;
    uint32_t address = GuestSymbol(symbol);
    if (!address) return 0;
    // Horn has no Java-side nativeRender entry point. Drain around every host
    // entry so a synchronous Java callback that re-enters a NativeCallback_*
    // while the JIT is active does not remain queued forever.
    std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
    g_host->DrainDeferred();
    std::vector<uint32_t> args = GuestArgs(receiver, tail);
    uint32_t result = g_host->RunFunction(address, args);
    g_host->DrainDeferred();
    return result;
}

static uint32_t RunGuestPrepared32(jobject receiver, const char* symbol,
                                    std::initializer_list<uint32_t> tail = {}) {
    if (!g_host) return 0;
    uint32_t address = GuestSymbol(symbol);
    if (!address) return 0;
    std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
    g_host->DrainDeferred();
    std::vector<uint32_t> args = GuestArgs(receiver, tail);
    uint32_t result = g_host->RunFunction(address, args);
    g_host->DrainDeferred();
    return result;
}

static uint64_t RunGuestPrepared64(jobject receiver, const char* symbol,
                                    std::initializer_list<uint32_t> tail = {}) {
    if (!g_host) return 0;
    uint32_t address = GuestSymbol(symbol);
    if (!address) return 0;
    std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
    g_host->DrainDeferred();
    std::vector<uint32_t> args = GuestArgs(receiver, tail);
    uint64_t result = g_host->RunFunction64(address, args);
    g_host->DrainDeferred();
    return result;
}

static void RefreshGuestLanguageExtension() {
    if (!g_host) return;
    const uint32_t language_ext = g_host->FindSymbol("_Z17appGetLanguageExtv");
    if (!language_ext) return;
    if (!g_language_ext_result) g_language_ext_result = g_host->GuestAlloc(12, 4);
    if (!g_language_ext_result) return;
    // The Java locale callback arrives after SurfaceChanged on this title. Querying
    // the UE3 resolver once before NativeCallback_Initialize makes its selected
    // Coalesced_<lang>.bin available to the startup package loader.
    std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
    g_host->DrainDeferred();
    g_host->RunFunction(language_ext, {g_language_ext_result});
    g_host->DrainDeferred();
}

static jobject GuestObjectResult(JNIEnv* env, uint32_t handle) {
    (void)env;
    return FakeJniEnv::Instance().ResolveObject(handle);
}

// Borderlands 2 callbacks use the ARM32 base AAPCS ABI. Keep every guest word
// explicit so jlong alignment and the eight-float MotionEvent stay auditable.
static uint32_t RunGuestWords(JNIEnv* env, jobject receiver, const char* symbol,
                              const std::vector<uint32_t>& tail) {
    if (!PrepareGuestCall(env, receiver)) return 0;
    const uint32_t address = GuestSymbol(symbol);
    if (!address) return 0;
    std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
    g_host->DrainDeferred();
    std::vector<uint32_t> args;
    args.reserve(2 + tail.size());
    args.push_back(FakeEnv());
    args.push_back(GuestObject(receiver));
    args.insert(args.end(), tail.begin(), tail.end());
    const uint32_t result = g_host->RunFunction(address, args);
    g_host->DrainDeferred();
    return result;
}

static void B_Cleanup(JNIEnv* env, jobject self) {
    RunGuest32(env, self,
        "_Z22NativeCallback_CleanupP7_JNIEnvP8_jobject");
}

static void B_ContentPurchased(JNIEnv*, jobject, jstring) {
    // The shipped ELF has no ContentPurchased implementation; this Java native
    // declaration was added by a later storefront wrapper and remains inert.
}

static void LogDiagnosticMovieState(const char* event, int controller, int action,
                                    int key) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.horn.movie_state", value) <= 0 ||
        value[0] != '1' || !g_host) return;
    static int samples = 0;
    if (samples++ >= 24) return;
    const uint32_t global = Sym("GFullScreenMovie");
    const uint32_t scaleform_global = Sym("GScaleformFullScreenMovie");
    const uint32_t viewport_global = Sym("GFullScreenMovieViewport");
    const uint32_t gfx_global = Sym("GGFxEngine");
    const uint32_t draw_gfx_global = Sym("GDrawGFx");
    const uint32_t render_gfx_global = Sym("GRenderGFxUI");
    const uint32_t movie = global ? g_host->MemoryRead32(global) : 0;
    const uint32_t scaleform = scaleform_global
        ? g_host->MemoryRead32(scaleform_global) : 0;
    const uint32_t viewport = viewport_global
        ? g_host->MemoryRead32(viewport_global) : 0;
    const uint32_t gfx = gfx_global ? g_host->MemoryRead32(gfx_global) : 0;
    const bool gfx_valid = gfx &&
        static_cast<uint64_t>(gfx) + 0x24u <= g_host->GetGuestMemorySize();
    const bool valid = movie &&
        static_cast<uint64_t>(movie) + 0xF8u <= g_host->GetGuestMemorySize();
    LOGI("Movie state event=%s controller=%d action=%d key=%d ptr=0x%08X "
         "vtable=0x%08X bink=0x%08X active=0x%08X flags38=0x%08X "
         "count=%u index=%u skip=%u frameFlag=0x%08X "
         "scaleform=0x%08X viewport=0x%08X gfx=0x%08X "
         "openMovies=%u allMovies=%u drawGFx=%u renderGFx=%u",
         event, controller, action, key, movie,
         valid ? g_host->MemoryRead32(movie) : 0,
         valid ? g_host->MemoryRead32(movie + 0x50u) : 0,
         valid ? g_host->MemoryRead32(movie + 0x5cu) : 0,
         valid ? g_host->MemoryRead32(movie + 0x38u) : 0,
         valid ? g_host->MemoryRead32(movie + 0x84u) : 0,
         valid ? g_host->MemoryRead32(movie + 0x8cu) : 0,
         valid ? g_host->MemoryRead32(movie + 0x90u) : 0,
         valid ? g_host->MemoryRead32(movie + 0xf4u) : 0,
         scaleform, viewport, gfx,
         gfx_valid ? g_host->MemoryRead32(gfx + 0x14u) : 0,
         gfx_valid ? g_host->MemoryRead32(gfx + 0x20u) : 0,
         draw_gfx_global ? g_host->MemoryRead8(draw_gfx_global) : 0,
         render_gfx_global ? g_host->MemoryRead8(render_gfx_global) : 0);
}

static jboolean B_DeviceEvent(JNIEnv* env, jobject self, jint device,
                              jint event, jboolean connected) {
    NotifyBl2TouchPhysicalDevice(device, connected != JNI_FALSE);
    const bool accepted = RunGuest32(env, self,
        "_Z26NativeCallback_DeviceEventP7_JNIEnvP8_jobjectiih",
        {static_cast<uint32_t>(device), static_cast<uint32_t>(event),
         static_cast<uint32_t>(connected)}) != 0;
    LogDiagnosticMovieState("device", device, connected, event);
    return accepted ? JNI_TRUE : JNI_FALSE;
}

static void B_ForceGarbageCollection(JNIEnv* env, jobject self,
                                     jboolean force) {
    RunGuest32(env, self,
        "_Z37NativeCallback_ForceGarbageCollectionP7_JNIEnvP8_jobjecth",
        {static_cast<uint32_t>(force)});
}

static jstring B_GetGraphicsPath(JNIEnv* env, jobject self) {
    const uint32_t handle = RunGuest32(env, self,
        "_Z30NativeCallback_GetGraphicsPathP7_JNIEnvP8_jobject");
    return static_cast<jstring>(GuestObjectResult(env, handle));
}

static float QueryDisplayRefreshRate(JNIEnv* env, jobject activity);

static bool PrepareBl2LocalIdentity(JNIEnv* env, jobject self) {
    // prepareStartup uses the existing callback to initialize actual local save
    // and achievement managers. Its existing worker delivers the accurate
    // disconnected JNI identity. No cloud sync work exists in LocalSaveGame.
    jclass activity = env->GetObjectClass(self);
    jfieldID identity_field = activity ? env->GetFieldID(activity,"mLocalIdentity",
        "Lcom/tt2kgames/Borderlands2/LocalIdentity;") : nullptr;
    jfieldID worker_field = identity_field ? env->GetFieldID(activity,"mLocalSignInThread",
        "Ljava/lang/Thread;") : nullptr;
    jobject identity = worker_field ? env->GetObjectField(self,identity_field) : nullptr;
    jclass identity_class = identity ? env->GetObjectClass(identity) : nullptr;
    jmethodID prepare = identity_class ? env->GetMethodID(identity_class,"prepareStartup","()V") : nullptr;
    if (prepare && !env->ExceptionCheck()) env->CallVoidMethod(identity,prepare);
    jobject worker = prepare && !env->ExceptionCheck() ? env->GetObjectField(self,worker_field) : nullptr;
    jclass worker_class = worker ? env->GetObjectClass(worker) : nullptr;
    jmethodID join = worker_class ? env->GetMethodID(worker_class,"join","(J)V") : nullptr;
    jmethodID alive = join ? env->GetMethodID(worker_class,"isAlive","()Z") : nullptr;
    if (alive && !env->ExceptionCheck()) env->CallVoidMethod(worker,join,jlong{1000});
    const bool completed = alive && !env->ExceptionCheck() && !env->CallBooleanMethod(worker,alive);
    if (worker_class) env->DeleteLocalRef(worker_class);
    if (worker) env->DeleteLocalRef(worker);
    if (identity_class) env->DeleteLocalRef(identity_class);
    if (identity) env->DeleteLocalRef(identity);
    if (activity) env->DeleteLocalRef(activity);
    if (!completed || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        __android_log_print(ANDROID_LOG_ERROR,"Arm32Bridge_LocalSession","Local identity preparation failed");
        return false;
    }
    const auto tick = GuestSymbol("_Z23AndroidSteam_TickSignInv");
    const auto signed_in = GuestSymbol("_Z23AndroidSteam_IsSignedInv");
    const auto connected = GuestSymbol("_Z24AndroidSteam_IsConnectedv");
    if (!tick || !signed_in || !connected) return false;
    g_host->RunFunction(tick,{});
    const bool ready = g_host->RunFunction(signed_in,{}) && !g_host->RunFunction(connected,{});
    g_local_identity_prepared.store(ready,std::memory_order_release);
    if (ready) __android_log_print(ANDROID_LOG_INFO,"Arm32Bridge_LocalSession",
        "Real local save/achievement owners prepared before engine initialization; remote=0");
    return ready;
}

static jboolean B_Init(JNIEnv* env, jobject self, jint width, jint height,
                       jfloat scale, jboolean use_surface,
                       jobject surface, jboolean fullscreen,
                       jint preferred_format, jboolean use_depth_buffer) {
    LOGI("NativeCallback_Init %dx%d", width, height);
    // BL2 initializes before SurfaceCreated, normally with 0x0. The generic
    // object in this entry is its AssetManager; validate the real Surface only
    // in B_PostInitUpdate, which subsequently selects the render dimensions.
    if (!PrepareGuestCall(env, self)) return JNI_FALSE;
    if (!PrepareBl2LocalIdentity(env,self)) return JNI_FALSE;
    {
        // Query after guest loading: a pending startup rate may already be stale.
        float pending = g_pending_display_refresh_rate.load(std::memory_order_acquire);
        const float display_refresh_rate = QueryDisplayRefreshRate(env, self);
        if (std::isfinite(display_refresh_rate) && display_refresh_rate > 0.0f &&
            display_refresh_rate <= cfg::kMaximumSaneDisplayRate) {
            // Preserve a newer display callback that arrived during the query.
            g_pending_display_refresh_rate.compare_exchange_strong(
                pending, display_refresh_rate, std::memory_order_acq_rel);
        }
        std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
        // Init and the installed DisplayListener run on the UI thread.
        // Early callbacks retain their rate until this guest patch is ready.
        g_bl2_tick_patch_ready.store(true, std::memory_order_release);
        if (!InstallBl2MaxTickRatePatch(
                g_pending_display_refresh_rate.load(std::memory_order_acquire))) {
            g_bl2_tick_patch_ready.store(false, std::memory_order_release);
            return JNI_FALSE;
        }
    }
    ThunkManager::s_screenWidth = static_cast<uint32_t>(width);
    ThunkManager::s_screenHeight = static_cast<uint32_t>(height);
    const uint32_t result = RunGuestPrepared32(self,
        "_Z19NativeCallback_InitP7_JNIEnvP8_jobjectiifhS2_hih",
        {static_cast<uint32_t>(width), static_cast<uint32_t>(height),
         FloatBits(scale), static_cast<uint32_t>(use_surface),
         GuestObject(surface), static_cast<uint32_t>(fullscreen),
         static_cast<uint32_t>(preferred_format),
         static_cast<uint32_t>(use_depth_buffer)});
    LOGI("NativeCallback_Init end result=0x%08X", result);
    return result ? JNI_TRUE : JNI_FALSE;
}

static jboolean B_InputEvent(JNIEnv* env, jobject self, jint a, jint b,
                             jint c, jint d, jlong timestamp) {
    return RunGuest32(env, self,
        "_Z25NativeCallback_InputEventP7_JNIEnvP8_jobjectiiiix",
        {static_cast<uint32_t>(a), static_cast<uint32_t>(b),
         static_cast<uint32_t>(c), static_cast<uint32_t>(d),
         JLongLow(timestamp), JLongHigh(timestamp)}) ? JNI_TRUE : JNI_FALSE;
}

// The guest InterruptionChanged spins in usleep(100 ms), without a timeout,
// until the game thread acknowledges the pause or resume at the top of its
// frame (GHasStoppedRendering). A frame can itself wait for the UI thread (a
// Java callback) or for audio progress: Wwise sync bank work needs the OpenSL
// buffer callbacks that PauseAudioPlayback suppresses. Spinning on the UI
// thread with audio already paused deadlocked a focus loss mid-frame (ANR:
// FocusEvent not consumed). One worker keeps the guest calls in order, audio
// pauses only after the guest parked, and the UI thread waits a bounded time.
namespace lifecycle {

constexpr const char* kSymbol = "_Z34NativeCallback_InterruptionChangedP7_JNIEnvP8_jobjecth";
constexpr auto kUiWait = std::chrono::milliseconds(2000);  // ANR fires at 5 s.

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::pair<uint64_t, bool>> g_queue;
jobject g_receiver = nullptr;  // Global ref to the latest activity.
uint64_t g_next = 0, g_done = 0;
uint32_t g_result = 1;
bool g_started = false, g_failed = false;

// Pause/resume pairs the guest call with the audio pump in a deadlock-free order.
uint32_t Run(JNIEnv* env, jobject receiver, bool interrupted) {
    if (!interrupted) ResumeAudioPlayback();
    const uint32_t result = RunGuest32(env, receiver, kSymbol,
                                       {static_cast<uint32_t>(interrupted)});
    if (interrupted) PauseAudioPlayback();
    return result;
}

void Worker() {
    pthread_setname_np(pthread_self(), "BL2Lifecycle");
    JNIEnv* env = nullptr;
    if (!g_jvm || g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        __android_log_print(ANDROID_LOG_ERROR, "BL2Lifecycle", "worker could not attach to the VM");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_failed = true;  // The UI thread drains the queue itself.
        g_cv.notify_all();
        return;
    }
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        g_cv.wait(lock, [] { return !g_queue.empty(); });
        const auto [serial, interrupted] = g_queue.front();
        g_queue.pop_front();
        jobject receiver = g_receiver ? env->NewLocalRef(g_receiver) : nullptr;
        lock.unlock();
        const uint32_t result = Run(env, receiver, interrupted);
        if (receiver) env->DeleteLocalRef(receiver);
        if (env->ExceptionCheck()) env->ExceptionClear();
        lock.lock();
        g_done = serial;
        g_result = result;
        g_cv.notify_all();
    }
}

}  // namespace lifecycle

static jboolean B_InterruptionChanged(JNIEnv* env, jobject self,
                                      jboolean interrupted) {
    bool ready = false;
    {
        std::lock_guard<std::recursive_mutex> load_lock(g_load_mutex);
        ready = g_guest_load_state == GuestLoadState::Ready && g_host;
    }
    std::unique_lock<std::mutex> lock(lifecycle::g_mutex);
    if (ready && !lifecycle::g_started && !lifecycle::g_failed) {
        try {
            std::thread(lifecycle::Worker).detach();
            lifecycle::g_started = true;
        } catch (...) {
            lifecycle::g_failed = true;
        }
    }
    if (!ready || lifecycle::g_failed) {
        // Requests left by a worker that never attached run first, in order.
        std::deque<std::pair<uint64_t, bool>> pending;
        pending.swap(lifecycle::g_queue);
        lock.unlock();
        uint32_t result = 1;
        for (const auto& request : pending) result = lifecycle::Run(env, self, request.second);
        result = lifecycle::Run(env, self, interrupted != JNI_FALSE);
        return result ? JNI_TRUE : JNI_FALSE;
    }
    jobject receiver = self ? env->NewGlobalRef(self) : nullptr;
    if (lifecycle::g_receiver) env->DeleteGlobalRef(lifecycle::g_receiver);
    lifecycle::g_receiver = receiver;
    const uint64_t serial = ++lifecycle::g_next;
    lifecycle::g_queue.emplace_back(serial, interrupted != JNI_FALSE);
    lifecycle::g_cv.notify_all();
    lifecycle::g_cv.wait_for(lock, lifecycle::kUiWait,
        [serial] { return lifecycle::g_done >= serial || lifecycle::g_failed; });
    if (lifecycle::g_done >= serial)
        return lifecycle::g_done == serial && !lifecycle::g_result ? JNI_FALSE : JNI_TRUE;
    if (lifecycle::g_failed) {
        std::deque<std::pair<uint64_t, bool>> pending;
        pending.swap(lifecycle::g_queue);
        lock.unlock();
        uint32_t result = 1;
        for (const auto& request : pending) result = lifecycle::Run(env, self, request.second);
        return result ? JNI_TRUE : JNI_FALSE;
    }
    // The guest returns 1 on both paths; JNI_FALSE would make Java kill the process.
    __android_log_print(ANDROID_LOG_WARN, "BL2Lifecycle",
        "interrupted=%d still waiting for the game thread after %lld ms; UI continues",
        interrupted ? 1 : 0, static_cast<long long>(lifecycle::kUiWait.count()));
    return JNI_TRUE;
}

static jboolean B_KeyEvent(JNIEnv* env, jobject self, jint a, jint b, jint c,
                           jint d, jlong timestamp) {
    NotifyBl2TouchPhysicalKey(a, b, c);
    LogDiagnosticMovieState("key-before", a, b, c);
    const bool accepted = RunGuest32(env, self,
        "_Z23NativeCallback_KeyEventP7_JNIEnvP8_jobjectiiiix",
        {static_cast<uint32_t>(a), static_cast<uint32_t>(b),
         static_cast<uint32_t>(c), static_cast<uint32_t>(d),
         JLongLow(timestamp), JLongHigh(timestamp)}) != 0;
    LogDiagnosticMovieState("key-after", a, b, c);
    return accepted ? JNI_TRUE : JNI_FALSE;
}

static void B_KeyboardFinished(JNIEnv* env, jobject self, jstring text,
                               jboolean canceled) {
    RunGuestWords(env, self,
        "_Z31NativeCallback_KeyboardFinishedP7_JNIEnvP8_jobjectP8_jstringh",
        {GuestObject(text), static_cast<uint32_t>(canceled)});
}

static void B_LanguageSet(JNIEnv* env, jobject self, jstring language) {
    if (!PrepareGuestCall(env, self)) return;
    (void)language;
    jstring effective = env->NewStringUTF(Bl2LocaleLanguage());
    if (!effective) return;
    RunGuestPrepared32(self,
        "_Z26NativeCallback_LanguageSetP7_JNIEnvP8_jobjectP8_jstring",
        {GuestObject(effective)});
    env->DeleteLocalRef(effective);
}

static jboolean B_MotionEvent(JNIEnv* env, jobject self, jint a, jint b,
                              jfloat f0, jfloat f1, jfloat f2, jfloat f3,
                              jfloat f4, jfloat f5, jfloat f6, jfloat f7,
                              jint c, jlong timestamp) {
    NotifyBl2TouchPhysicalMotion(a, {f0, f1, f2, f3, f4, f5, f6, f7});
    const std::vector<uint32_t> words = {
        static_cast<uint32_t>(a), static_cast<uint32_t>(b),
        FloatBits(f0), FloatBits(f1), FloatBits(f2), FloatBits(f3),
        FloatBits(f4), FloatBits(f5), FloatBits(f6), FloatBits(f7),
        static_cast<uint32_t>(c), 0u, JLongLow(timestamp), JLongHigh(timestamp)};
    return RunGuestWords(env, self,
        "_Z26NativeCallback_MotionEventP7_JNIEnvP8_jobjectiiffffffffix",
        words) ? JNI_TRUE : JNI_FALSE;
}

static jboolean B_MouseEvent(JNIEnv* env, jobject self, jint action,
                             jfloat x, jfloat y, jfloat dx, jfloat dy,
                             jint buttons, jint pointer, jint meta) {
    NotifyBl2TouchPhysicalMouse(action, buttons, pointer, meta);
    return RunGuest32(env, self,
        "_Z25NativeCallback_MouseEventP7_JNIEnvP8_jobjectiffffiii",
        {static_cast<uint32_t>(action), FloatBits(x), FloatBits(y),
         FloatBits(dx), FloatBits(dy), static_cast<uint32_t>(buttons),
         static_cast<uint32_t>(pointer), static_cast<uint32_t>(meta)})
        ? JNI_TRUE : JNI_FALSE;
}

static void B_MovieFinished(JNIEnv* env, jobject self) {
    RunGuest32(env, self,
        "_Z28NativeCallback_MovieFinishedP7_JNIEnvP8_jobject");
}

static void B_NetworkUpdate(JNIEnv* env, jobject self, jboolean connected,
                            jboolean wifi, jlong timestamp) {
    RunGuest32(env, self,
        "_Z28NativeCallback_NetworkUpdateP7_JNIEnvP8_jobjecthhx",
        {static_cast<uint32_t>(connected), static_cast<uint32_t>(wifi),
         JLongLow(timestamp), JLongHigh(timestamp)});
}

static void B_PostInitUpdate(JNIEnv* env, jobject self, jobject surface,
                             jint width, jint height) {
    LOGI("NativeCallback_PostInitUpdate %dx%d", width, height);
    if (!PrepareBl2NativeResolutionSurface(env, surface, width, height) ||
        !PrepareGuestCall(env, self)) return;
    ThunkManager::s_screenWidth = static_cast<uint32_t>(width);
    ThunkManager::s_screenHeight = static_cast<uint32_t>(height);
    RunGuestPrepared32(self,
        "_Z29NativeCallback_PostInitUpdateP7_JNIEnvP8_jobjectS2_ii",
        {GuestObject(surface), static_cast<uint32_t>(width),
         static_cast<uint32_t>(height)});
    LogBl2NativeResolutionState(*g_host, "surface-changed");
}

static void B_PostVSync(JNIEnv* env, jobject self, jlong timestamp) {
    RunGuest32(env, self,
        "_Z24NativeCallback_PostVSyncP7_JNIEnvP8_jobjectx",
        {JLongLow(timestamp), JLongHigh(timestamp)});
}

static void B_Render(JNIEnv* env, jobject self, jint width, jint height) {
    // The exported guest function owns its render loop and obtains JNIEnv via
    // GetJNIEnv(); width/height are harmless trailing JNI words on this entry.
    RunGuest32(env, self, "_Z21NativeCallback_RenderPv",
        {static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
}

static void B_SignInChanged(JNIEnv* env, jobject self, jboolean signed_in,
                            jstring user, jstring token, jint provider,
                            jobjectArray permissions, jstring error) {
    RunGuest32(env, self,
        "_Z28NativeCallback_SignInChangedP7_JNIEnvP8_jobjecthP8_jstringS4_iP13_jobjectArrayS4_",
        {static_cast<uint32_t>(signed_in), GuestObject(user), GuestObject(token),
         static_cast<uint32_t>(provider), GuestObject(permissions),
         GuestObject(error)});
}

static jboolean B_SystemStats(JNIEnv* env, jobject self, jlong value,
                              jint kind) {
    return RunGuest32(env, self,
        "_Z26NativeCallback_SystemStatsP7_JNIEnvP8_jobjectxi",
        {JLongLow(value), JLongHigh(value), static_cast<uint32_t>(kind)})
        ? JNI_TRUE : JNI_FALSE;
}

static jint B_NvsiInit(JNIEnv* env, jobject self, jint flags) {
    return static_cast<jint>(RunGuest32(env, self,
        "_Z30NativeCallback_nvsiInitWrapperP7_JNIEnvP8_jobjecti",
        {static_cast<uint32_t>(flags)}));
}

static jint B_NvsiCheckStatus(JNIEnv* env, jobject self) {
    return static_cast<jint>(RunGuest32(env, self,
        "_Z37NativeCallback_nvsiCheckStatusWrapperP7_JNIEnvP8_jobject"));
}

static jint B_NvsiClose(JNIEnv* env, jobject self) {
    return static_cast<jint>(RunGuest32(env, self,
        "_Z31NativeCallback_nvsiCloseWrapperP7_JNIEnvP8_jobject"));
}

static jint B_NvsiLock(JNIEnv* env, jobject self) {
    return static_cast<jint>(RunGuest32(env, self,
        "_Z30NativeCallback_nvsiLockWrapperP7_JNIEnvP8_jobject"));
}

static jint B_NvsiPause(JNIEnv* env, jobject self) {
    return static_cast<jint>(RunGuest32(env, self,
        "_Z31NativeCallback_nvsiPauseWrapperP7_JNIEnvP8_jobject"));
}

static jint B_NvsiResume(JNIEnv* env, jobject self) {
    return static_cast<jint>(RunGuest32(env, self,
        "_Z32NativeCallback_nvsiResumeWrapperP7_JNIEnvP8_jobject"));
}

static jint B_NvsiUnlock(JNIEnv* env, jobject self) {
    return static_cast<jint>(RunGuest32(env, self,
        "_Z32NativeCallback_nvsiUnlockWrapperP7_JNIEnvP8_jobject"));
}

static jboolean B_OnPause(JNIEnv* env, jobject self) {
    const jboolean result = RunGuest32(env, self,
        "_Z22NativeCallback_onPauseP7_JNIEnvP8_jobject")
        ? JNI_TRUE : JNI_FALSE;
    // The process may be killed while paused: keep this run's learned blocks.
    if (g_host) g_host->RequestLearnedJitSave();
    // No shader formation in the background of another app.
    Bl2ShaderFormerPause(true);
    return result;
}

static jboolean B_OnResume(JNIEnv* env, jobject self) {
    Bl2ShaderFormerPause(false);
    return RunGuest32(env, self,
        "_Z23NativeCallback_onResumeP7_JNIEnvP8_jobject")
        ? JNI_TRUE : JNI_FALSE;
}

static void B_NvsiCancelVerification(JNIEnv*, jobject) {}
static void B_NvsiForceRecheck(JNIEnv*, jobject, jint) {}
static void B_NvsiOnScreenOff(JNIEnv*, jobject) {}

#define HORN_VOID0(host_name, guest_name) \
    static void host_name(JNIEnv* env, jobject self) { RunGuest32(env, self, guest_name); }
#define HORN_VOID_BOOL(host_name, guest_name) \
    static void host_name(JNIEnv* env, jobject self, jboolean value) { \
        RunGuest32(env, self, guest_name, {static_cast<uint32_t>(value)}); \
    }
#define HORN_VOID_INT(host_name, guest_name) \
    static void host_name(JNIEnv* env, jobject self, jint value) { \
        RunGuest32(env, self, guest_name, {static_cast<uint32_t>(value)}); \
    }
#define HORN_VOID_STRING(host_name, guest_name) \
    static void host_name(JNIEnv* env, jobject self, jstring value) { \
        if (!PrepareGuestCall(env, self)) return; \
        RunGuestPrepared32(self, guest_name, {GuestObject(value)}); \
    }
#define HORN_BOOL0(host_name, guest_name) \
    static jboolean host_name(JNIEnv* env, jobject self) { \
        return RunGuest32(env, self, guest_name) ? JNI_TRUE : JNI_FALSE; \
    }
#define HORN_BOOL_BOOL(host_name, guest_name) \
    static jboolean host_name(JNIEnv* env, jobject self, jboolean value) { \
        return RunGuest32(env, self, guest_name, {static_cast<uint32_t>(value)}) \
            ? JNI_TRUE : JNI_FALSE; \
    }

HORN_VOID_BOOL(H_KeyPadChange,
    "_Z27NativeCallback_KeyPadChangeP7_JNIEnvP8_jobjecth")
static jboolean H_InitEGL(JNIEnv* env, jobject self) {
    LOGI("NativeCallback_InitEGLCallback begin");
    const jboolean result = RunGuest32(env, self,
        "_Z30NativeCallback_InitEGLCallbackP7_JNIEnvP8_jobject")
        ? JNI_TRUE : JNI_FALSE;
    LOGI("NativeCallback_InitEGLCallback end result=%d", static_cast<int>(result));
    return result;
}

static float QueryPreferredRefreshRate(JNIEnv* env, jobject activity) {
    if (env->PushLocalFrame(8) != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("Could not allocate JNI local frame for preferredRefreshRate query");
        return 0.0f;
    }

    float result = 0.0f;
    do {
        jclass activity_class = env->GetObjectClass(activity);
        if (!activity_class) break;
        jmethodID get_window = env->GetMethodID(
            activity_class, "getWindow", "()Landroid/view/Window;");
        if (!get_window) break;
        jobject window = env->CallObjectMethod(activity, get_window);
        if (!window || env->ExceptionCheck()) break;

        jclass window_class = env->GetObjectClass(window);
        if (!window_class) break;
        jmethodID get_attributes = env->GetMethodID(
            window_class, "getAttributes",
            "()Landroid/view/WindowManager$LayoutParams;");
        if (!get_attributes) break;
        jobject attributes = env->CallObjectMethod(window, get_attributes);
        if (!attributes || env->ExceptionCheck()) break;

        jclass attributes_class = env->GetObjectClass(attributes);
        if (!attributes_class) break;
        jfieldID preferred_refresh_rate = env->GetFieldID(
            attributes_class, "preferredRefreshRate", "F");
        if (!preferred_refresh_rate) break;
        result = env->GetFloatField(attributes, preferred_refresh_rate);
    } while (false);

    if (env->ExceptionCheck()) {
        LOGE("JNI exception while reading Window preferredRefreshRate");
        env->ExceptionClear();
        result = 0.0f;
    }
    env->PopLocalFrame(nullptr);
    return result;
}

static float QueryDisplayRefreshRate(JNIEnv* env, jobject activity) {
    if (env->PushLocalFrame(8) != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("Could not allocate JNI local frame for display refresh query");
        return 0.0f;
    }
    float result = 0.0f;
    do {
        jclass activity_class = env->GetObjectClass(activity);
        if (!activity_class) break;
        jmethodID get_window_manager = env->GetMethodID(
            activity_class, "getWindowManager", "()Landroid/view/WindowManager;");
        if (!get_window_manager) break;
        jobject window_manager = env->CallObjectMethod(activity, get_window_manager);
        if (!window_manager || env->ExceptionCheck()) break;
        jclass manager_class = env->GetObjectClass(window_manager);
        if (!manager_class) break;
        jmethodID get_default_display = env->GetMethodID(
            manager_class, "getDefaultDisplay", "()Landroid/view/Display;");
        if (!get_default_display) break;
        jobject display = env->CallObjectMethod(window_manager, get_default_display);
        if (!display || env->ExceptionCheck()) break;
        jclass display_class = env->GetObjectClass(display);
        if (!display_class) break;
        jmethodID get_refresh_rate = env->GetMethodID(
            display_class, "getRefreshRate", "()F");
        if (!get_refresh_rate) break;
        result = env->CallFloatMethod(display, get_refresh_rate);
    } while (false);
    if (env->ExceptionCheck()) {
        LOGE("JNI exception while reading Display.getRefreshRate");
        env->ExceptionClear();
        result = 0.0f;
    }
    env->PopLocalFrame(nullptr);
    return result;
}

static jboolean H_Initialize(JNIEnv* env, jobject self, jint width, jint height,
                             jfloat scale, jboolean use_zip) {
    LOGI("NativeCallback_Initialize begin %dx%d scale=%.3f zip=%d",
         width, height, static_cast<double>(scale), static_cast<int>(use_zip));
    const float preferred_refresh_rate = QueryPreferredRefreshRate(env, self);
    if (!PrepareGuestCall(env, self)) return JNI_FALSE;
    std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
    if (!InstallAdaptiveMaxRefreshPatch(preferred_refresh_rate)) {
        LOGE("NativeCallback_Initialize: refresh-rate patch failed");
        return JNI_FALSE;
    }
    ThunkManager::s_screenWidth = static_cast<uint32_t>(width);
    ThunkManager::s_screenHeight = static_cast<uint32_t>(height);
    const jboolean result = RunGuestPrepared32(self,
        "_Z25NativeCallback_InitializeP7_JNIEnvP8_jobjectiifhS2_h",
        {static_cast<uint32_t>(width), static_cast<uint32_t>(height),
         FloatBits(scale), static_cast<uint32_t>(use_zip)}) ? JNI_TRUE : JNI_FALSE;
    LOGI("NativeCallback_Initialize end result=%d", static_cast<int>(result));
    return result;
}

static void H_InitModel(JNIEnv* env, jobject self, jstring a, jstring b, jstring c) {
    if (!PrepareGuestCall(env, self)) return;
    RunGuestPrepared32(self,
        "_Z24NativeCallback_InitModelP7_JNIEnvP8_jobjectP8_jstringS4_S4_",
        {GuestObject(a), GuestObject(b), GuestObject(c)});
}

static void H_InitZipVFS(JNIEnv* env, jobject self, jboolean value) {
    RunGuest32(env, self,
        "_Z25NativeCallback_InitZipVFSP7_JNIEnvP8_jobjecth",
        {static_cast<uint32_t>(value)});
}

static void H_PostInitUpdate(JNIEnv* env, jobject self, jint width, jint height) {
    LOGI("NativeCallback_PostInitUpdate %dx%d", width, height);
    if (!PrepareGuestCall(env, self)) return;
    std::lock_guard<std::recursive_mutex> host_lock(g_host->GetMutex());
    ThunkManager::s_screenWidth = static_cast<uint32_t>(width);
    ThunkManager::s_screenHeight = static_cast<uint32_t>(height);
    RunGuestPrepared32(self,
        "_Z29NativeCallback_PostInitUpdateP7_JNIEnvP8_jobjectii",
        {static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    RefreshGuestLanguageExtension();
}

HORN_VOID0(H_Cleanup, "_Z22NativeCallback_CleanupP7_JNIEnvP8_jobject")
HORN_VOID_STRING(H_KeyboardFinished,
    "_Z31NativeCallback_KeyboardFinishedP7_JNIEnvP8_jobjectP8_jstring")
static void H_MovieFinished(JNIEnv* env, jobject self) {
    LOGI("NativeCallback_MovieFinished");
    RunGuest32(env, self, "_Z28NativeCallback_MovieFinishedP7_JNIEnvP8_jobject");
}

static jboolean H_SystemStats(JNIEnv* env, jobject self, jlong value, jint kind) {
    // env/self consume r0/r1, so the first jlong is naturally aligned in r2:r3;
    // `kind` is the first stack word.
    return RunGuest32(env, self,
        "_Z26NativeCallback_SystemStatsP7_JNIEnvP8_jobjectxi",
        {JLongLow(value), JLongHigh(value), static_cast<uint32_t>(kind)})
        ? JNI_TRUE : JNI_FALSE;
}

static jboolean H_InterruptionChanged(JNIEnv* env, jobject self, jboolean value) {
    // Host-side counterpart of the guest interruption callback. The Java layer
    // pairs InterruptionChanged(true) with MediaPlayer.pause() and
    // InterruptionChanged(false) with resume, so the mapping is symmetric by
    // construction. Without this wiring g_audio_paused never engages and the
    // host keeps relaying guest buffer callbacks while Android expects silence.
    if (value) {
        PauseAudioPlayback();
    } else {
        ResumeAudioPlayback();
    }
    return RunGuest32(env, self,
        "_Z34NativeCallback_InterruptionChangedP7_JNIEnvP8_jobjecth",
        {static_cast<uint32_t>(value)}) ? JNI_TRUE : JNI_FALSE;
}

static void H_NetworkUpdate(JNIEnv* env, jobject self, jboolean connected,
                            jboolean wifi) {
    RunGuest32(env, self,
        "_Z28NativeCallback_NetworkUpdateP7_JNIEnvP8_jobjecthh",
        {static_cast<uint32_t>(connected), static_cast<uint32_t>(wifi)});
}

static void H_LanguageSet(JNIEnv* env, jobject self, jstring value) {
    if (!PrepareGuestCall(env, self)) return;
    RunGuestPrepared32(self,
        "_Z26NativeCallback_LanguageSetP7_JNIEnvP8_jobjectP8_jstring",
        {GuestObject(value)});
    if (g_host->GetElfLoader().GetImageSize() != cfg::kGuestImageSize) {
        LOGE("Android locale selection patch refused: guest image size 0x%08X",
             g_host->GetElfLoader().GetImageSize());
        return;
    }
    const uint32_t flag = g_host->GetElfLoader().GetBaseAddress() +
                          cfg::kAndroidLocaleSelectionFlagOffset;
    const uint32_t before = g_host->MemoryRead32(flag);
    if (before != 0 && (before & 1u) == 0) {
        LOGE("Android locale selection patch refused: flag 0x%08X has 0x%08X",
             flag, before);
        return;
    }
    if ((before & 1u) == 0) g_host->MemoryWrite32(flag, before | 1u);
    if ((g_host->MemoryRead32(flag) & 1u) == 0) {
        LOGE("Android locale selection patch write verification failed at 0x%08X", flag);
    }
}

static jboolean H_InputEvent(JNIEnv* env, jobject self, jint a, jint b, jint c,
                             jint d, jlong timestamp) {
    // Four jint arguments leave the next core/stack word index at six, already
    // even, so the jlong occupies words 6:7 without padding.
    return RunGuest32(env, self,
        "_Z25NativeCallback_InputEventP7_JNIEnvP8_jobjectiiiix",
        {static_cast<uint32_t>(a), static_cast<uint32_t>(b),
         static_cast<uint32_t>(c), static_cast<uint32_t>(d),
         JLongLow(timestamp), JLongHigh(timestamp)})
        ? JNI_TRUE : JNI_FALSE;
}

HORN_BOOL0(H_IsTegra2, "_Z29NativeCallback_IsTegra2DeviceP7_JNIEnvP8_jobject")
HORN_BOOL0(H_IsTegra3, "_Z29NativeCallback_IsTegra3DeviceP7_JNIEnvP8_jobject")
HORN_BOOL0(H_IsTegra4, "_Z29NativeCallback_IsTegra4DeviceP7_JNIEnvP8_jobject")
HORN_BOOL0(H_IsSamsung,
    "_Z39NativeCallback_IsSupportedSamsungDeviceP7_JNIEnvP8_jobject")
HORN_BOOL0(H_IsMotorola,
    "_Z40NativeCallback_IsSupportedMotorolaDeviceP7_JNIEnvP8_jobject")
HORN_BOOL0(H_IsLG,
    "_Z34NativeCallback_IsSupportedLGDeviceP7_JNIEnvP8_jobject")
HORN_BOOL0(H_IsAmazon,
    "_Z38NativeCallback_IsSupportedAmazonDeviceP7_JNIEnvP8_jobject")

static jboolean H_KeyEvent(JNIEnv* env, jobject self, jint action, jint key_code,
                           jint unicode, jobject event) {
    if (!PrepareGuestCall(env, self)) return JNI_FALSE;
    return RunGuestPrepared32(self,
        "_Z23NativeCallback_KeyEventP7_JNIEnvP8_jobjectiii",
        {static_cast<uint32_t>(action), static_cast<uint32_t>(key_code),
         static_cast<uint32_t>(unicode), GuestObject(event)}) ? JNI_TRUE : JNI_FALSE;
}

static jstring H_GetGraphicsPath(JNIEnv* env, jobject self) {
    uint32_t handle = RunGuest32(env, self,
        "_Z30NativeCallback_GetGraphicsPathP7_JNIEnvP8_jobject");
    return static_cast<jstring>(GuestObjectResult(env, handle));
}

#define HORN_VOID_STRING_INT(host_name, guest_name) \
    static void host_name(JNIEnv* env, jobject self, jstring text, jint value) { \
        if (!PrepareGuestCall(env, self)) return; \
        RunGuestPrepared32(self, guest_name, \
                   {GuestObject(text), static_cast<uint32_t>(value)}); \
    }

HORN_VOID_STRING_INT(H_TapjoyGetUpdatePoints,
    "_Z36NativeCallback_TapjoyGetUpdatePointsP7_JNIEnvP8_jobjectP8_jstringi")
HORN_VOID_STRING(H_TapjoyGetUpdatePointsFailed,
    "_Z42NativeCallback_TapjoyGetUpdatePointsFailedP7_JNIEnvP8_jobjectP8_jstring")
HORN_VOID_STRING_INT(H_TapjoyGetSpendPointsResponse,
    "_Z43NativeCallback_TapjoyGetSpendPointsResponseP7_JNIEnvP8_jobjectP8_jstringi")
HORN_VOID_STRING(H_TapjoyGetSpendPointsResponseFailed,
    "_Z49NativeCallback_TapjoyGetSpendPointsResponseFailedP7_JNIEnvP8_jobjectP8_jstring")
HORN_VOID0(H_TapjoyGetDisplayAdResponse,
    "_Z41NativeCallback_TapjoyGetDisplayAdResponseP7_JNIEnvP8_jobject")
HORN_VOID_STRING(H_TapjoyGetDisplayAdResponseFailed,
    "_Z47NativeCallback_TapjoyGetDisplayAdResponseFailedP7_JNIEnvP8_jobjectP8_jstring")
HORN_VOID_STRING_INT(H_TapjoyGetAwardPointsResponse,
    "_Z43NativeCallback_TapjoyGetAwardPointsResponseP7_JNIEnvP8_jobjectP8_jstringi")
HORN_VOID_STRING(H_TapjoyGetAwardPointsResponseFailed,
    "_Z49NativeCallback_TapjoyGetAwardPointsResponseFailedP7_JNIEnvP8_jobjectP8_jstring")
HORN_VOID_INT(H_TapjoyEarnedTapPoints,
    "_Z36NativeCallback_TapjoyEarnedTapPointsP7_JNIEnvP8_jobjecti")
HORN_VOID0(H_TapjoyVideoReady,
    "_Z31NativeCallback_TapjoyVideoReadyP7_JNIEnvP8_jobject")
HORN_VOID_STRING(H_TapjoyVideoError,
    "_Z31NativeCallback_TapjoyVideoErrorP7_JNIEnvP8_jobjectP8_jstring")
HORN_VOID0(H_TapjoyVideoComplete,
    "_Z34NativeCallback_TapjoyVideoCompleteP7_JNIEnvP8_jobject")
HORN_VOID_STRING(H_IapSucceeded,
    "_Z42NativeCallback_AndroidIAPPurchaseSucceededP7_JNIEnvP8_jobjectP8_jstring")
HORN_VOID_STRING(H_IapRestored,
    "_Z41NativeCallback_AndroidIAPPurchaseRestoredP7_JNIEnvP8_jobjectP8_jstring")
HORN_VOID_STRING(H_IapCanceled,
    "_Z41NativeCallback_AndroidIAPPurchaseCanceledP7_JNIEnvP8_jobjectP8_jstring")

static void H_IapFailed(JNIEnv* env, jobject self, jstring product, jstring reason) {
    if (!PrepareGuestCall(env, self)) return;
    RunGuestPrepared32(self,
        "_Z39NativeCallback_AndroidIAPPurchaseFailedP7_JNIEnvP8_jobjectP8_jstringS4_",
        {GuestObject(product), GuestObject(reason)});
}

static void H_ZipDump(JNIEnv* env, jclass cls, jlong handle) {
    RunGuest32(env, cls,
        "Java_com_android_vending_expansion_zipfile_ZipResourceFile_dumpZipFileContents",
        {JLongLow(handle), JLongHigh(handle)});
}

static jlong H_ZipInfo(JNIEnv* env, jclass cls, jlong handle, jstring name) {
    if (!PrepareGuestCall(env, cls)) return 0;
    // env/cls occupy r0/r1, handle is r2:r3 and name starts on the stack.
    return JLongResult(RunGuestPrepared64(cls,
        "Java_com_android_vending_expansion_zipfile_ZipResourceFile_getFileInfoHandle",
        {JLongLow(handle), JLongHigh(handle), GuestObject(name)}));
}

#define HORN_ZIP_INT(host_name, guest_name) \
    static jint host_name(JNIEnv* env, jclass cls, jlong handle) { \
        return static_cast<jint>(RunGuest32(env, cls, guest_name, \
            {JLongLow(handle), JLongHigh(handle)})); \
    }

HORN_ZIP_INT(H_ZipOffset,
    "Java_com_android_vending_expansion_zipfile_ZipResourceFile_getFileOffset")
HORN_ZIP_INT(H_ZipStatus,
    "Java_com_android_vending_expansion_zipfile_ZipResourceFile_getFileStatus")
HORN_ZIP_INT(H_ZipSize,
    "Java_com_android_vending_expansion_zipfile_ZipResourceFile_getFileUncompressedSize")

static jstring H_ZipName(JNIEnv* env, jclass cls, jlong handle) {
    uint32_t result = RunGuest32(env, cls,
        "Java_com_android_vending_expansion_zipfile_ZipResourceFile_getZipFileName",
        {JLongLow(handle), JLongHigh(handle)});
    return static_cast<jstring>(GuestObjectResult(env, result));
}

static jlong H_ZipOpen(JNIEnv* env, jclass cls, jstring name, jlong previous) {
    if (!PrepareGuestCall(env, cls)) return 0;
    // name consumes r2. AAPCS rounds the next register number from r3 to r4
    // for the 8-byte-aligned jlong, so word 3 is explicit padding and the value
    // starts at stack word 0 (overall words 4:5).
    return JLongResult(RunGuestPrepared64(cls,
        "Java_com_android_vending_expansion_zipfile_ZipResourceFile_openZipFile",
        {GuestObject(name), 0, JLongLow(previous), JLongHigh(previous)}));
}

static const JNINativeMethod g_bl2_activity_methods[] = {
    {"NativeCallback_Cleanup", "()V", (void*)B_Cleanup},
    {"NativeCallback_ContentPurchased", "(Ljava/lang/String;)V", (void*)B_ContentPurchased},
    {"NativeCallback_DeviceEvent", "(IIZ)Z", (void*)B_DeviceEvent},
    {"NativeCallback_ForceGarbageCollection", "(Z)V", (void*)B_ForceGarbageCollection},
    {"NativeCallback_GetGraphicsPath", "()Ljava/lang/String;", (void*)B_GetGraphicsPath},
    {"NativeCallback_Init", "(IIFZLjava/lang/Object;ZIZ)Z", (void*)B_Init},
    {"NativeCallback_InputEvent", "(IIIIJ)Z", (void*)B_InputEvent},
    {"NativeCallback_InterruptionChanged", "(Z)Z", (void*)B_InterruptionChanged},
    {"NativeCallback_KeyEvent", "(IIIIJ)Z", (void*)B_KeyEvent},
    {"NativeCallback_KeyboardFinished", "(Ljava/lang/String;Z)V", (void*)B_KeyboardFinished},
    {"NativeCallback_LanguageSet", "(Ljava/lang/String;)V", (void*)B_LanguageSet},
    {"NativeCallback_MotionEvent", "(IIFFFFFFFFIJ)Z", (void*)B_MotionEvent},
    {"NativeCallback_MouseEvent", "(IFFFFIII)Z", (void*)B_MouseEvent},
    {"NativeCallback_MovieFinished", "()V", (void*)B_MovieFinished},
    {"NativeCallback_NetworkUpdate", "(ZZJ)V", (void*)B_NetworkUpdate},
    {"NativeCallback_PostInitUpdate", "(Landroid/view/Surface;II)V", (void*)B_PostInitUpdate},
    {"NativeCallback_PostVSync", "(J)V", (void*)B_PostVSync},
    {"NativeCallback_Render", "(II)V", (void*)B_Render},
    {"NativeCallback_SignInChanged", "(ZLjava/lang/String;Ljava/lang/String;I[Ljava/lang/String;Ljava/lang/String;)V", (void*)B_SignInChanged},
    {"NativeCallback_SystemStats", "(JI)Z", (void*)B_SystemStats},
    {"NativeCallback_nvsiCheckStatusWrapper", "()I", (void*)B_NvsiCheckStatus},
    {"NativeCallback_nvsiCloseWrapper", "()I", (void*)B_NvsiClose},
    {"NativeCallback_nvsiInitWrapper", "(I)I", (void*)B_NvsiInit},
    {"NativeCallback_nvsiLockWrapper", "()I", (void*)B_NvsiLock},
    {"NativeCallback_nvsiPauseWrapper", "()I", (void*)B_NvsiPause},
    {"NativeCallback_nvsiResumeWrapper", "()I", (void*)B_NvsiResume},
    {"NativeCallback_nvsiUnlockWrapper", "()I", (void*)B_NvsiUnlock},
    {"NativeCallback_onPause", "()Z", (void*)B_OnPause},
    {"NativeCallback_onResume", "()Z", (void*)B_OnResume},
};

static const JNINativeMethod g_nvsi_methods[] = {
    {"native_nvsiCancelVerification", "()V", (void*)B_NvsiCancelVerification},
    {"native_nvsiForceRecheck", "(I)V", (void*)B_NvsiForceRecheck},
    {"native_nvsiOnScreenOff", "()V", (void*)B_NvsiOnScreenOff},
};

static const JNINativeMethod g_horn_activity_methods[] = {
    {"NativeCallback_KeyPadChange", "(Z)V", (void*)H_KeyPadChange},
    {"NativeCallback_InitEGLCallback", "()Z", (void*)H_InitEGL},
    {"NativeCallback_Initialize", "(IIFZ)Z", (void*)H_Initialize},
    {"NativeCallback_InitModel", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", (void*)H_InitModel},
    {"NativeCallback_InitZipVFS", "(Z)V", (void*)H_InitZipVFS},
    {"NativeCallback_PostInitUpdate", "(II)V", (void*)H_PostInitUpdate},
    {"NativeCallback_Cleanup", "()V", (void*)H_Cleanup},
    {"NativeCallback_KeyboardFinished", "(Ljava/lang/String;)V", (void*)H_KeyboardFinished},
    {"NativeCallback_MovieFinished", "()V", (void*)H_MovieFinished},
    {"NativeCallback_SystemStats", "(JI)Z", (void*)H_SystemStats},
    {"NativeCallback_InterruptionChanged", "(Z)Z", (void*)H_InterruptionChanged},
    {"NativeCallback_NetworkUpdate", "(ZZ)V", (void*)H_NetworkUpdate},
    {"NativeCallback_LanguageSet", "(Ljava/lang/String;)V", (void*)H_LanguageSet},
    {"NativeCallback_InputEvent", "(IIIIJ)Z", (void*)H_InputEvent},
    {"NativeCallback_IsTegra2Device", "()Z", (void*)H_IsTegra2},
    {"NativeCallback_IsTegra3Device", "()Z", (void*)H_IsTegra3},
    {"NativeCallback_IsTegra4Device", "()Z", (void*)H_IsTegra4},
    {"NativeCallback_IsSupportedSamsungDevice", "()Z", (void*)H_IsSamsung},
    {"NativeCallback_IsSupportedMotorolaDevice", "()Z", (void*)H_IsMotorola},
    {"NativeCallback_IsSupportedLGDevice", "()Z", (void*)H_IsLG},
    {"NativeCallback_IsSupportedAmazonDevice", "()Z", (void*)H_IsAmazon},
    {"NativeCallback_KeyEvent", "(IIILandroid/view/KeyEvent;)Z", (void*)H_KeyEvent},
    {"NativeCallback_GetGraphicsPath", "()Ljava/lang/String;", (void*)H_GetGraphicsPath},
    {"NativeCallback_TapjoyGetUpdatePoints", "(Ljava/lang/String;I)V", (void*)H_TapjoyGetUpdatePoints},
    {"NativeCallback_TapjoyGetUpdatePointsFailed", "(Ljava/lang/String;)V", (void*)H_TapjoyGetUpdatePointsFailed},
    {"NativeCallback_TapjoyGetSpendPointsResponse", "(Ljava/lang/String;I)V", (void*)H_TapjoyGetSpendPointsResponse},
    {"NativeCallback_TapjoyGetSpendPointsResponseFailed", "(Ljava/lang/String;)V", (void*)H_TapjoyGetSpendPointsResponseFailed},
    {"NativeCallback_TapjoyGetDisplayAdResponse", "()V", (void*)H_TapjoyGetDisplayAdResponse},
    {"NativeCallback_TapjoyGetDisplayAdResponseFailed", "(Ljava/lang/String;)V", (void*)H_TapjoyGetDisplayAdResponseFailed},
    {"NativeCallback_TapjoyGetAwardPointsResponse", "(Ljava/lang/String;I)V", (void*)H_TapjoyGetAwardPointsResponse},
    {"NativeCallback_TapjoyGetAwardPointsResponseFailed", "(Ljava/lang/String;)V", (void*)H_TapjoyGetAwardPointsResponseFailed},
    {"NativeCallback_TapjoyEarnedTapPoints", "(I)V", (void*)H_TapjoyEarnedTapPoints},
    {"NativeCallback_TapjoyVideoReady", "()V", (void*)H_TapjoyVideoReady},
    {"NativeCallback_TapjoyVideoError", "(Ljava/lang/String;)V", (void*)H_TapjoyVideoError},
    {"NativeCallback_TapjoyVideoComplete", "()V", (void*)H_TapjoyVideoComplete},
    {"NativeCallback_AndroidIAPPurchaseSucceeded", "(Ljava/lang/String;)V", (void*)H_IapSucceeded},
    {"NativeCallback_AndroidIAPPurchaseRestored", "(Ljava/lang/String;)V", (void*)H_IapRestored},
    {"NativeCallback_AndroidIAPPurchaseCanceled", "(Ljava/lang/String;)V", (void*)H_IapCanceled},
    {"NativeCallback_AndroidIAPPurchaseFailed", "(Ljava/lang/String;Ljava/lang/String;)V", (void*)H_IapFailed},
};

static const JNINativeMethod g_zip_methods[] = {
    {"dumpZipFileContents", "(J)V", (void*)H_ZipDump},
    {"getFileInfoHandle", "(JLjava/lang/String;)J", (void*)H_ZipInfo},
    {"getFileOffset", "(J)I", (void*)H_ZipOffset},
    {"getFileStatus", "(J)I", (void*)H_ZipStatus},
    {"getFileUncompressedSize", "(J)I", (void*)H_ZipSize},
    {"getZipFileName", "(J)Ljava/lang/String;", (void*)H_ZipName},
    {"openZipFile", "(Ljava/lang/String;J)J", (void*)H_ZipOpen},
};

static_assert(sizeof(g_bl2_activity_methods) / sizeof(g_bl2_activity_methods[0]) == 29,
              "Borderlands 2 UE3JavaApp native table must match its 29 declarations");
static_assert(sizeof(g_nvsi_methods) / sizeof(g_nvsi_methods[0]) == 3,
              "NvSecureInterfaceUtil native table must match its 3 declarations");
static_assert(sizeof(g_horn_activity_methods) / sizeof(g_horn_activity_methods[0]) == 39,
              "Horn UE3JavaApp native table must match the audited 39 rows");
static_assert(sizeof(g_zip_methods) / sizeof(g_zip_methods[0]) == 7,
              "Horn ZipResourceFile native table must match the audited 7 rows");

static jclass FindRequiredClass(JNIEnv* env, const char* class_name) {
    jclass cls = env->FindClass(class_name);
    if (cls) return cls;
    LOGE("Required native class not found: %s", class_name);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    return nullptr;
}

static bool RegisterTable(JNIEnv* env, jclass cls, const char* class_name,
                          const JNINativeMethod* methods, jint count) {
    jint result = env->RegisterNatives(cls, methods, count);
    if (result != JNI_OK || env->ExceptionCheck()) {
        LOGE("RegisterNatives failed for %s: result=%d count=%d",
             class_name, result, count);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        return false;
    }
    LOGI("Registered %d natives on %s", count, class_name);
    return true;
}

static void RollBackTable(JNIEnv* env, jclass cls, const char* class_name) {
    if (!cls) return;
    jint result = env->UnregisterNatives(cls);
    if (result != JNI_OK || env->ExceptionCheck()) {
        LOGE("UnregisterNatives rollback failed for %s: result=%d", class_name, result);
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    } else {
        LOGI("Rolled back native registration on %s", class_name);
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
#if HORN_SILENT_LOGCAT
    Bl2SilenceProcessLogs();  // Before any Java code of this process logs again.
#endif
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    __android_log_print(ANDROID_LOG_INFO, "BL2_Parity",
        "backend=arm64-dynarmic guest=armeabi-v7a JNI_OnLoad");

    jclass controls = FindRequiredClass(env, "com/tt2kgames/Borderlands2/LaunchControls");
    if (!controls) return JNI_ERR;
    g_shader_progress_method = env->GetStaticMethodID(controls, "showShaderPreparation", "(II)V");
    if (g_shader_progress_method && !env->ExceptionCheck())
        g_shader_progress_class = static_cast<jclass>(env->NewGlobalRef(controls));
    env->DeleteLocalRef(controls);
    if (!g_shader_progress_class || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        LOGE("Required shader preparation progress method unavailable");
        return JNI_ERR;
    }

    // Capture paths before any native can be invoked. CurrentApplication is the
    // fallback because JNI_OnLoad has no Activity receiver.
    {
        std::lock_guard<std::recursive_mutex> load_lock(g_load_mutex);
        CaptureHostPaths(env, nullptr);
    }

    // Resolve both declaring classes before mutating either one. Registration
    // is atomic per class, so roll UE3JavaApp back if the NVSI table fails.
    jclass activity_class = FindRequiredClass(env, cfg::kActivityClass);
    jclass nvsi_class = FindRequiredClass(env, cfg::kNvsiClass);
    if (!activity_class || !nvsi_class) {
        if (activity_class) env->DeleteLocalRef(activity_class);
        if (nvsi_class) env->DeleteLocalRef(nvsi_class);
        return JNI_ERR;
    }

    const jint activity_count = static_cast<jint>(
        sizeof(g_bl2_activity_methods) / sizeof(g_bl2_activity_methods[0]));
    const jint nvsi_count = static_cast<jint>(
        sizeof(g_nvsi_methods) / sizeof(g_nvsi_methods[0]));
    bool activity_registered = RegisterTable(
        env, activity_class, cfg::kActivityClass,
        g_bl2_activity_methods, activity_count);
    if (!activity_registered) {
        // Defensive even though ART documents RegisterNatives as all-or-nothing.
        RollBackTable(env, activity_class, cfg::kActivityClass);
        env->DeleteLocalRef(nvsi_class);
        env->DeleteLocalRef(activity_class);
        return JNI_ERR;
    }

    bool nvsi_registered = RegisterTable(
        env, nvsi_class, cfg::kNvsiClass, g_nvsi_methods, nvsi_count);
    if (!nvsi_registered) {
        RollBackTable(env, nvsi_class, cfg::kNvsiClass);
        RollBackTable(env, activity_class, cfg::kActivityClass);
        env->DeleteLocalRef(nvsi_class);
        env->DeleteLocalRef(activity_class);
        return JNI_ERR;
    }

    env->DeleteLocalRef(nvsi_class);
    env->DeleteLocalRef(activity_class);
    return JNI_VERSION_1_6;
}
