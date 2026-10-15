#include "guest_software_write_watch.h"
#include "thunk_manager.h"
#include "bridge_log.h"
#include "bl2_locale.h"
#include "bl2_bink_audio.h"
#include "bl2_directory_cache.h"
#include "bl2_io_failure_probe.h"
#include "dynarmic_host.h"
#include "thunk_types.h"
#include "thunks_opensles.h"
#include "elf_loader.h"
#include "perf_probe.h"
#include "obb_pair_selection.h"
#include "obb_split_inventory.h"
#include "obb_sha256.h"
#include "bl2_obb_package_watch.h"
#include "bl2_touch_runtime.h"
#include "virtual_file_range.h"
#include "zip_stored_entry_bounds.h"
#include "arm32_network_abi.h"
#include "guest_mapped_dirty_tracker.h"
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <chrono>
#include <ctime>
#include <time.h>
#include <cwchar>
#include <arpa/inet.h>
#include <deque>
#include <dirent.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <limits>
#include <memory>
#include <netdb.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <linux/futex.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include <termios.h>
#include <wctype.h>
#include <zlib.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#define LOG_TAG "Arm32Bridge_Libc"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

// The ARM32 game asks for its legacy external-files directory during startup.
// Android 11+ can expose that path to Java while denying direct native access
// from this ARM64 host process, so keep the guest's working-directory contract
// without letting the engine abort its initialization when the legacy path is
// unavailable.
extern std::string g_saved_files_dir;

extern "C" FILE *funopen(const void *cookie,
                         int (*readfn)(void *, char *, int),
                         int (*writefn)(void *, const char *, int),
                         fpos_t (*seekfn)(void *, fpos_t, int),
                         int (*closefn)(void *));

using ApkEntry = Bl2LocaleEntry;

static ApkEntry g_apk_entries[128];
static int g_num_apk_entries = 0;
static char g_apk_indexed_path[1024] = {0};
static int g_apk_indexed_fd = -1;
static ApkEntry g_verified_obb_entries[2]{};
static bool g_verified_obb_pair = false;
static std::atomic<uint32_t> g_fopen_trace_count{0};
static std::atomic<uint32_t> g_open_trace_count{0};
static std::atomic<bool> g_config_engine_audited{false};
static std::atomic<bool> g_config_game_audited{false};

// Opt-in read-only capture of the actual per-user graphics profile. The
// external Config directory has restrictive traversal bits on this device, so
// adb cannot pull it even though the game process can open the files.
static void AuditUserConfig(int fd, const char* path) {
    if (fd < 0 || !path) return;
    const char* leaf = strrchr(path, '/');
    if (!leaf || strstr(path, "/Config/") == nullptr) return;
    ++leaf;
    std::atomic<bool>* audited = nullptr;
    if (strcmp(leaf, "Android-WillowEngine.ini") == 0) {
        audited = &g_config_engine_audited;
    } else if (strcmp(leaf, "Android-WillowGame.ini") == 0) {
        audited = &g_config_game_audited;
    } else {
        return;
    }
    char property[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.config_audit", property) <= 0 ||
        property[0] != '1' || audited->exchange(true)) return;

    const std::string destination =
        std::string("/sdcard/Android/data/com.tt2kgames.Borderlands2/files/config-audit-") + leaf;
    const int out = open(destination.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660);
    if (out < 0) {
        LOGE("config audit output open failed: %s errno=%d", destination.c_str(), errno);
        return;
    }
    if (fchmod(out, 0660) != 0) {
        LOGE("config audit chmod failed: %s errno=%d", destination.c_str(), errno);
    }
    ObbSha256 sha;
    std::array<uint8_t, 8192> buffer{};
    size_t total = 0;
    bool good = true;
    for (;;) {
        const ssize_t count = pread(fd, buffer.data(), buffer.size(), static_cast<off_t>(total));
        if (count < 0 || total + static_cast<size_t>(count) > 1024 * 1024) {
            good = false;
            break;
        }
        if (count == 0) break;
        sha.Update(buffer.data(), static_cast<size_t>(count));
        ssize_t written = 0;
        while (written < count) {
            const ssize_t part = write(out, buffer.data() + written,
                                       static_cast<size_t>(count - written));
            if (part <= 0) { good = false; break; }
            written += part;
        }
        if (!good) break;
        total += static_cast<size_t>(count);
    }
    close(out);
    if (good) {
        const auto digest = ObbSha256::Hex(sha.Finalize());
        LOGI("config audit: %s bytes=%zu sha256=%s", leaf, total, digest.data());
    } else {
        LOGE("config audit failed: %s after %zu bytes errno=%d", leaf, total, errno);
        unlink(destination.c_str());
    }
}

static bool IsLegacyCommandLinePath(const char* path) {
    if (!path || !strstr(path, "UE3CommandLine.txt")) return false;
    std::string normalized;
    normalized.reserve(strlen(path));
    for (const char* current = path; *current; ++current) {
        if (*current != '/' || normalized.empty() || normalized.back() != '/') {
            normalized.push_back(*current);
        }
    }
    static constexpr char kSuffix[] =
        "/Borderlands2/WillowGame/CookedAndroid_DXT/UE3CommandLine.txt";
    const size_t suffix_length = sizeof(kSuffix) - 1;
    return normalized.size() >= suffix_length &&
           normalized.compare(normalized.size() - suffix_length,
                              suffix_length, kSuffix) == 0;
}

static bool IsLegacyConfigDirectory(const char* path) {
    if (!path) return false;
    std::string normalized;
    normalized.reserve(strlen(path));
    for (const char* current = path; *current; ++current) {
        if (*current != '/' || normalized.empty() || normalized.back() != '/') {
            normalized.push_back(*current);
        }
    }
    return normalized == "/storage/emulated/0/Borderlands2/WillowGame/Config";
}

static bool IsMediaPath(const char* path) {
    if (!path) return false;
    std::string lower(path);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return lower.find(".bik") != std::string::npos ||
           lower.find(".bk2") != std::string::npos ||
           lower.find("bink") != std::string::npos ||
           lower.find("movie") != std::string::npos ||
           lower.find("splash") != std::string::npos ||
           lower.find("legal") != std::string::npos;
}

static bool ShouldTraceFileCall(std::atomic<uint32_t>& counter, uint32_t limit,
                                const char* operation, const char* path,
                                uint32_t* index) {
    const uint32_t value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (index) *index = value;
    if (value <= limit || IsMediaPath(path)) return true;
    if (value == limit + 1) {
        LOGI("%s trace: suppressing non-media calls after %u", operation, limit);
    }
    return false;
}

static std::vector<GuestThreadWorker> g_thread_workers;

const std::vector<GuestThreadWorker>& GetGuestThreadWorkers() {
    return g_thread_workers;
}

void AddGuestThreadWorker(uint32_t start_func, uint32_t arg) {
    g_thread_workers.push_back({start_func, arg});
}

struct ApkCookie {
    int apk_fd;
    off_t base_offset;
    off_t file_size;
    off_t current_offset;
    const Bl2BinkAudio* bink;
};

static int ApkRead(void* cookie, char* buf, int size) {
    ApkCookie* c = static_cast<ApkCookie*>(cookie);
    if (size <= 0 || !c || c->apk_fd < 0) return 0;

    if (c->current_offset + size > c->file_size) {
        size = static_cast<int>(c->file_size - c->current_offset);
    }
    if (size <= 0) return 0;

    ssize_t n = c->bink ? c->bink->Read(buf, size, c->current_offset)
                       : pread(c->apk_fd, buf, size, c->base_offset + c->current_offset);
    if (n > 0) {
        c->current_offset += n;
        return static_cast<int>(n);
    }
    LOGI("ApkRead: size=%d, offset=%lld, n=%zd", size, static_cast<long long>(c->current_offset), n);
    return 0;
}

static fpos_t ApkSeek(void* cookie, fpos_t offset, int whence) {
    ApkCookie* c = static_cast<ApkCookie*>(cookie);
    if (!c) return -1;
    off_t target = 0;

    if (whence == SEEK_SET) target = offset;
    else if (whence == SEEK_CUR) target = c->current_offset + offset;
    else if (whence == SEEK_END) target = c->file_size + offset;
    else return -1;

    if (target < 0) return -1;
    if (target > c->file_size) target = c->file_size;
    c->current_offset = target;
    return target;
}

static int ApkClose(void* cookie) {
    ApkCookie* c = static_cast<ApkCookie*>(cookie);
    if (c) {
        if (c->apk_fd >= 0) close(c->apk_fd);
        free(c);
    }
    return 0;
}

extern "C" int IndexApk(const char* apk_path) {
    if (!apk_path || strlen(apk_path) >= sizeof(g_apk_indexed_path)) return -1;
    if (g_apk_indexed_fd >= 0) close(g_apk_indexed_fd);
    g_apk_indexed_fd = -1;
    g_num_apk_entries = 0;
    memcpy(g_apk_indexed_path, apk_path, strlen(apk_path) + 1);
    g_apk_indexed_fd = open(g_apk_indexed_path, O_RDONLY | O_CLOEXEC);
    if (g_apk_indexed_fd < 0) {
        LOGE("IndexApk: Failed to open APK: %s (errno %d)", g_apk_indexed_path, errno);
        return -1;
    }
    const auto fail = []() {
        if (g_apk_indexed_fd >= 0) close(g_apk_indexed_fd);
        g_apk_indexed_fd = -1;
        g_num_apk_entries = 0;
        return -1;
    };
    const auto read_exact = [](int fd, void* destination, size_t length, off_t offset) {
        auto* bytes = static_cast<unsigned char*>(destination);
        size_t done = 0;
        while (done < length) {
            const ssize_t got = pread(fd, bytes + done, length - done,
                                      offset + static_cast<off_t>(done));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return false;
            done += static_cast<size_t>(got);
        }
        return true;
    };

    struct stat apk_stat{};
    if (fstat(g_apk_indexed_fd, &apk_stat) != 0 || apk_stat.st_size < 22) return fail();
    const off_t file_len = apk_stat.st_size;
    const off_t search_len = std::min<off_t>(file_len, 65536 + 22);
    const off_t search_start = file_len - search_len;
    unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(search_len)));
    if (!buf) return fail();
    if (!read_exact(g_apk_indexed_fd, buf, static_cast<size_t>(search_len), search_start)) {
        free(buf);
        return fail();
    }
    off_t eocd_offset = -1;
    for (ssize_t i = search_len - 22; i >= 0; --i) {
        if (buf[i] == 0x50 && buf[i+1] == 0x4b && buf[i+2] == 0x05 && buf[i+3] == 0x06 &&
            static_cast<uint64_t>(i) + 22 +
                static_cast<uint16_t>(buf[i+20] | (buf[i+21] << 8)) ==
                static_cast<uint64_t>(search_len)) {
            eocd_offset = search_start + i;
            break;
        }
    }
    free(buf);

    if (eocd_offset < 0) {
        LOGE("IndexApk: EOCD not found in APK!");
        return fail();
    }

    unsigned char eocd[22];
    if (!read_exact(g_apk_indexed_fd, eocd, sizeof(eocd), eocd_offset)) return fail();
    uint16_t num_entries = *reinterpret_cast<uint16_t*>(eocd + 10);
    uint32_t cd_size = *reinterpret_cast<uint32_t*>(eocd + 12);
    uint32_t cd_offset = *reinterpret_cast<uint32_t*>(eocd + 16);
    if (num_entries == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF ||
        cd_size == 0 || cd_size > 128U * 1024U * 1024U ||
        static_cast<uint64_t>(cd_offset) + cd_size >
            static_cast<uint64_t>(eocd_offset)) {
        LOGE("IndexApk: invalid or unsupported central directory in %s", apk_path);
        return fail();
    }

    unsigned char* cd = static_cast<unsigned char*>(malloc(cd_size));
    if (!cd) return fail();
    if (!read_exact(g_apk_indexed_fd, cd, cd_size, cd_offset)) {
        free(cd);
        return fail();
    }

    uint32_t pos = 0;
    for (int i = 0; i < num_entries; ++i) {
        if (static_cast<uint64_t>(pos) + 46 > cd_size ||
            *reinterpret_cast<uint32_t*>(cd + pos) != 0x02014b50) {
            free(cd);
            return fail();
        }
        uint16_t method = *reinterpret_cast<uint16_t*>(cd + pos + 10);
        uint32_t csize = *reinterpret_cast<uint32_t*>(cd + pos + 20);
        uint32_t usize = *reinterpret_cast<uint32_t*>(cd + pos + 24);
        uint16_t fn_len = *reinterpret_cast<uint16_t*>(cd + pos + 28);
        uint16_t extra_len = *reinterpret_cast<uint16_t*>(cd + pos + 30);
        uint16_t comm_len = *reinterpret_cast<uint16_t*>(cd + pos + 32);
        uint32_t local_hdr_offset = *reinterpret_cast<uint32_t*>(cd + pos + 42);
        const uint64_t next = static_cast<uint64_t>(pos) + 46 + fn_len +
                              extra_len + comm_len;
        if (next > cd_size) {
            free(cd);
            return fail();
        }

        char fn[256] = {0};
        int cpy = fn_len < 255 ? fn_len : 255;
        memcpy(fn, cd + pos + 46, cpy);

        // Keep base-APK audio indexing separate from whole-OBB cache splits.
        // An OBB becomes visible only after IndexObbSplitApks has verified an
        // exact main+patch pair; a lone base-APK .obb must never bypass that.
        //
        // Whatever you add MUST be stored uncompressed in the APK (apktool: doNotCompress,
        // aapt: -0): everything below reads raw bytes at the entry offset, so a deflated
        // entry hands the engine compressed garbage with no error anywhere.
        if (strstr(fn, "FMOD") != nullptr || strstr(fn, ".fsb") != nullptr || strstr(fn, ".fev") != nullptr
            || strstr(fn, "assets/data/audio") != nullptr) {
            if (method != 0 || csize != usize || fn_len >= sizeof(g_apk_entries[0].name)) {
                LOGE("IndexApk: refusing unsuitable direct-stream entry '%s' "
                     "(ZIP method=%u, packed=%u, size=%u, name length=%u); "
                     "OBB/audio payloads must be STORED", fn,
                     static_cast<unsigned>(method), csize, usize,
                     static_cast<unsigned>(fn_len));
                pos = static_cast<uint32_t>(next);
                continue;
            }
            unsigned char local_hdr[30];
            if (static_cast<uint64_t>(local_hdr_offset) + sizeof(local_hdr) > cd_offset ||
                !read_exact(g_apk_indexed_fd, local_hdr, sizeof(local_hdr), local_hdr_offset) ||
                *reinterpret_cast<uint32_t*>(local_hdr) != 0x04034b50 ||
                *reinterpret_cast<uint16_t*>(local_hdr + 8) != 0) {
                free(cd);
                return fail();
            }
            uint16_t l_fn_len = *reinterpret_cast<uint16_t*>(local_hdr + 26);
            uint16_t l_extra_len = *reinterpret_cast<uint16_t*>(local_hdr + 28);
            const auto bounds = ResolveZipStoredEntryBounds(
                static_cast<uint64_t>(file_len), cd_offset, local_hdr_offset,
                l_fn_len, l_extra_len, csize, usize);
            if (!bounds.valid || l_fn_len != fn_len) {
                free(cd);
                return fail();
            }
            char local_name[sizeof(g_apk_entries[0].name)] = {0};
            if (!read_exact(g_apk_indexed_fd, local_name, l_fn_len,
                            static_cast<off_t>(local_hdr_offset) + 30) ||
                memcmp(local_name, fn, l_fn_len) != 0) {
                free(cd);
                return fail();
            }
            const off_t raw_data_offset = static_cast<off_t>(bounds.data_offset);

            if (g_num_apk_entries < 128) {
                strncpy(g_apk_entries[g_num_apk_entries].name, fn, 127);
                g_apk_entries[g_num_apk_entries].data_offset = raw_data_offset;
                g_apk_entries[g_num_apk_entries].file_size = usize;
                g_apk_entries[g_num_apk_entries].apk_fd = g_apk_indexed_fd;
                LOGI("IndexApk entry [%d]: %s (method=%d, size=%u, offset=%lld)",
                     g_num_apk_entries, fn, method, usize, static_cast<long long>(raw_data_offset));
                g_num_apk_entries++;
            }
        }
        pos = static_cast<uint32_t>(next);
    }
    free(cd);
    LOGI("IndexApk: Indexed %d audio entries from APK: %s", g_num_apk_entries, apk_path);
    return 0;
}

extern "C" int IndexObbSplitApks(const char* const* paths, size_t count) {
    if (g_verified_obb_pair) {
        for (auto& entry : g_verified_obb_entries) close(entry.apk_fd);
    }
    g_verified_obb_pair = false;
    g_verified_obb_entries[0] = {};
    g_verified_obb_entries[1] = {};
    if (count == 0) return 0;
    if (!paths || count > 64) return -1;

    struct ScannedSplit {
        int fd;
        ObbSplitScanResult result;
    };
    std::vector<ScannedSplit> verified;
    verified.reserve(count);
    // The two signed cache splits contain 4.1 GiB of STORED OBB data.
    // On abb24fae the main SHA-256/CRC pass alone took 40.2 seconds; the
    // former shared 40-second deadline rejected the patch after 3 MiB.
    const auto verification_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
    const auto within_deadline = [](const void* context) {
        const auto& deadline = *static_cast<
            const std::chrono::steady_clock::time_point*>(context);
        return std::chrono::steady_clock::now() < deadline;
    };
    const auto close_all = [&verified]() {
        for (const auto& split : verified) close(split.fd);
        verified.clear();
    };
    for (size_t i = 0; i < count; ++i) {
        if (!paths[i] || !*paths[i]) {
            close_all();
            return -1;
        }
        const int fd = open(paths[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            LOGE("OBB split open failed: '%s' errno=%d", paths[i], errno);
            close_all();
            return -1;
        }
        const auto scan = ScanObbSplitFd(
            fd, within_deadline, &verification_deadline, HasBl2RelocatedCache(), false);
        if (scan.status == ObbSplitScanStatus::Rejected ||
            scan.status == ObbSplitScanStatus::Cancelled) {
            LOGE("OBB split refused: '%s' reason=%s hashed=%llu", paths[i],
                 scan.reason, static_cast<unsigned long long>(scan.bytes_hashed));
            close(fd);
            close_all();
            return -1;
        }
        if (scan.status == ObbSplitScanStatus::NoObbEntry) {
            close(fd);
            continue;
        }
        LOGI("OBB signed split metadata: '%s' %s size=%llu offset=%llu expected_sha256=%s",
             paths[i], scan.main ? "main" : "patch",
             static_cast<unsigned long long>(scan.obb_size),
             static_cast<unsigned long long>(scan.apk_data_offset),
             scan.sha256.data());
        verified.push_back({fd, scan});
    }
    if (verified.empty()) return 0;
    std::vector<ObbCandidate> candidates;
    candidates.reserve(verified.size());
    for (const auto& split : verified) {
        candidates.push_back({
            ObbSource::EmbeddedSplits,
            split.result.main ? kMainObbName : kPatchObbName,
            std::string_view(split.result.sha256.data()),
            split.result.obb_size, true});
    }
    const auto pair = SelectCompleteObbPair(candidates);
    if (!pair.valid) {
        LOGE("No unique, complete, verified main+patch split pair (%zu OBB entries)",
             verified.size());
        close_all();
        return -1;
    }
    const size_t indices[] = {pair.main_index, pair.patch_index};
    for (size_t i = 0; i < 2; ++i) {
        const auto& source = verified[indices[i]];
        auto& entry = g_verified_obb_entries[i];
        snprintf(entry.name, sizeof(entry.name), "assets/%s",
                 i == 0 ? kMainObbName.data() : kPatchObbName.data());
        entry.data_offset = static_cast<off_t>(source.result.apk_data_offset);
        entry.file_size = static_cast<off_t>(source.result.obb_size);
        entry.apk_fd = source.fd;
    }
    for (size_t i = 0; i < verified.size(); ++i) {
        if (i != pair.main_index && i != pair.patch_index) close(verified[i].fd);
    }
    g_verified_obb_pair = true;
    SetBl2LocaleMainObb(g_verified_obb_entries[0].apk_fd,
                       g_verified_obb_entries[0].data_offset, g_verified_obb_entries[0].file_size);
    LOGI("Selected verified embedded OBB pair: main_fd=%d patch_fd=%d",
         g_verified_obb_entries[0].apk_fd, g_verified_obb_entries[1].apk_fd);
    return 0;
}

static int OriginalObbIndex(const char* path) {
    if (!path) return -1;
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    if (base == std::string_view(kMainObbName)) return 0;
    if (base == std::string_view(kPatchObbName)) return 1;
    return -1;
}

static bool IsOriginalObbDirectory(const char* path) {
    if (!path) return false;
    std::string_view directory(path);
    while (directory.size() > 1 && directory.back() == '/') directory.remove_suffix(1);
    return directory == "/storage/emulated/0/Android/obb/com.tt2kgames.Borderlands2" ||
           directory == "/sdcard/Android/obb/com.tt2kgames.Borderlands2";
}

static ApkEntry* FindApkEntry(const char* path) {
    if (!path) return nullptr;
    if (auto* locale = FindBl2LocaleEntry(path)) return locale;
    const int obb_index = OriginalObbIndex(path);
    if (obb_index >= 0) {
        return g_verified_obb_pair ? &g_verified_obb_entries[obb_index] : nullptr;
    }
    if (g_num_apk_entries == 0) return nullptr;
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;

    char preferred[256];
    snprintf(preferred, sizeof(preferred), "assets/data/audio/%s", base);
    // Plain assets/<basename>: this is the rule that catches an absolute path the engine
    // built for a file that no longer exists on disk -- e.g. an expansion file the engine
    // still looks for under /storage/emulated/0/Android/obb/<pkg>/ while it actually ships
    // as assets/main.<ver>.<pkg>.obb inside the APK.
    char in_assets[256];
    snprintf(in_assets, sizeof(in_assets), "assets/%s", base);
    for (int i = 0; i < g_num_apk_entries; ++i) {
        if (strcmp(g_apk_entries[i].name, preferred) == 0 || strcmp(g_apk_entries[i].name, path) == 0
            || strcmp(g_apk_entries[i].name, in_assets) == 0) {
            return &g_apk_entries[i];
        }
    }

    // 3. Language suffix fallback (e.g. VOICE.fsb -> VOICE_eng.fsb)
    std::string base_str(base);
    if (base_str.size() > 4 && base_str.substr(base_str.size() - 4) == ".fsb") {
        std::string stem = base_str.substr(0, base_str.size() - 4);
        std::string eng_variant = "assets/data/audio/" + stem + "_eng.fsb";
        for (int i = 0; i < g_num_apk_entries; ++i) {
            if (g_apk_entries[i].name == eng_variant || strstr(g_apk_entries[i].name, (stem + "_eng").c_str()) != nullptr) {
                return &g_apk_entries[i];
            }
        }
        for (int i = 0; i < g_num_apk_entries; ++i) {
            if (strstr(g_apk_entries[i].name, stem.c_str()) != nullptr) {
                return &g_apk_entries[i];
            }
        }
    }
    return nullptr;
}

static FILE* OpenApkEntryStream(const ApkEntry& entry) {
    const int stream_fd = dup(entry.apk_fd);
    if (stream_fd < 0) return nullptr;
    auto* cookie = static_cast<ApkCookie*>(malloc(sizeof(ApkCookie)));
    if (!cookie) {
        close(stream_fd);
        errno = ENOMEM;
        return nullptr;
    }
    *cookie = {stream_fd, entry.data_offset, entry.file_size, 0, entry.bink};
    FILE* stream = funopen(cookie, ApkRead, nullptr, ApkSeek, ApkClose);
    if (!stream) {
        const int saved_errno = errno;
        ApkClose(cookie);
        errno = saved_errno;
    }
    return stream;
}

static bool EmbeddedObbStat(const char* path, struct stat& st) {
    if (const auto* locale = FindBl2LocaleEntry(path)) {
        memset(&st, 0, sizeof(st));
        st.st_size = locale->file_size;
        st.st_mode = S_IFREG | 0444;
        st.st_nlink = 1;
        return true;
    }
    const int index = OriginalObbIndex(path);
    if (index < 0 || !g_verified_obb_pair) return false;
    memset(&st, 0, sizeof(st));
    st.st_size = g_verified_obb_entries[index].file_size;
    st.st_mode = S_IFREG | 0444;
    st.st_nlink = 1;
    return true;
}

// A file region already laid into the guest arena. The mmap thunk consults this before
// making a new mapping: the arena is bump-allocated, so mapping the same 345 MB expansion
// file a second time is a second permanent hole. Engines do exactly that -- a "reinstall"
// or "reload paks" path closes every archive and reopens it.
struct GuestFileMapping {
    dev_t  dev;
    ino_t  ino;
    off_t  off;      // page-aligned file offset
    size_t span;     // page-aligned length
    uint32_t va;     // guest address
};
static std::vector<GuestFileMapping> g_guest_file_maps;
static std::mutex g_mapping_mutex;

struct VirtualFd {
    int real_apk_fd;
    size_t file_size;
    off_t file_offset; // offset within APK
    off_t current_pos; // 0 .. file_size
    const Bl2BinkAudio* bink;
};
static std::unordered_map<int, VirtualFd> g_vfd_map;
static int g_next_vfd = 1000;
static std::mutex g_vfd_mutex;
static std::atomic<uint32_t> g_apk_direct_open_log_count{0};

// Guest address of the real archive-open function, resolved by
// InstallAssetOverrideHook and tail-called from the veneer.
static uint32_t g_asset_override_real_fn = 0;

static std::unordered_map<uint32_t, FILE*> g_file_map;
static uint32_t g_next_file_handle = 0x10001000;
static std::mutex g_file_mutex;

// pthread keys are process-wide, but values are per guest thread.
static std::mutex g_tls_key_mutex;
static std::unordered_set<uint32_t> g_tls_keys;
static std::unordered_map<uint32_t, uint32_t> g_tls_destructors;
static std::atomic<uint32_t> g_next_tls_key{1};
static thread_local std::unordered_map<uint32_t, uint32_t> g_tls_values;
static std::atomic<uint32_t> g_next_guest_thread_id{1};
static thread_local uint32_t g_current_guest_thread = 0;
static thread_local bool g_thread_exit_requested = false;
static thread_local uint32_t g_thread_exit_value = 0;
static thread_local std::unordered_map<uint32_t, mbstate_t> g_mb_states;
static thread_local mbstate_t g_internal_mb_state{};
static thread_local uint32_t g_tm_zone_addr = 0;
static thread_local size_t g_tm_zone_capacity = 0;
static thread_local uint32_t g_guest_errno_address = 0;

struct GuestThreadRecord {
    uint32_t id = 0;
    uint32_t start = 0;
    uint32_t arg = 0;
    uint32_t result = 0;
    bool done = false;
    bool detached = false;
    bool join_claimed = false;
    bool worker_started = false;
    int start_error = 0;
    uint32_t stack_size = 0;
    std::mutex mutex;
    std::condition_variable completed;
};
static std::mutex g_guest_threads_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<GuestThreadRecord>> g_guest_threads;
static std::mutex g_pthread_attr_mutex;
struct GuestThreadAttr {
    bool detached = false;
    uint32_t stack_size = 0;
    int sched_policy = SCHED_OTHER;
    int sched_priority = 0;
};
struct Arm32PthreadAttr {
    uint32_t flags;
    uint32_t stack_base;
    uint32_t stack_size;
    uint32_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
};
static_assert(sizeof(Arm32PthreadAttr) == 24);
static_assert(offsetof(Arm32PthreadAttr, stack_size) == 8);
static constexpr size_t kArm32PthreadStackMin = 8192;
static std::unordered_map<uint32_t, GuestThreadAttr> g_pthread_attrs;
static std::unordered_map<uint32_t, int> g_pthread_mutex_attrs;
static std::unordered_map<uint32_t, bool> g_pthread_cond_attrs;

struct GuestMutexState {
    std::mutex mutex;
    std::condition_variable available;
    uint32_t owner = 0;
    uint32_t recursion = 0;
    uint32_t waiters = 0;
    int type = PTHREAD_MUTEX_DEFAULT;
    bool destroyed = false;
};
struct GuestCondState {
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t broadcast_generation = 0;
    uint64_t next_ticket = 1;
    std::deque<uint64_t> waiting_tickets;
    std::unordered_set<uint64_t> signaled_tickets;
    uint32_t waiters = 0;
    bool monotonic = false;
    bool destroyed = false;
};
struct GuestSemaphoreState {
    std::mutex mutex;
    std::condition_variable available;
    uint32_t value = 0;
    uint32_t waiters = 0;
    bool destroyed = false;
};
struct GuestOnceState {
    std::mutex mutex;
    std::condition_variable completed;
    uint32_t owner = 0;
    bool running = false;
    bool done = false;
};
static std::mutex g_sync_state_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<GuestMutexState>> g_guest_mutexes;
static std::unordered_map<uint32_t, std::shared_ptr<GuestCondState>> g_guest_conds;
static std::unordered_map<uint32_t, std::shared_ptr<GuestSemaphoreState>> g_guest_semaphores;
static std::unordered_map<uint32_t, std::shared_ptr<GuestOnceState>> g_guest_once_states;

struct GuestDirHandle {
    DIR* dir = nullptr;
    uint32_t guest_dirent = 0;
    bool include_embedded_obbs = false;
    uint8_t next_embedded_obb = 0;
    std::shared_ptr<const Bl2DirectorySnapshot> snapshot;
    size_t next_snapshot_entry = 0;
};
static std::mutex g_dir_mutex;
static std::unordered_map<uint32_t, GuestDirHandle> g_dir_map;
static uint32_t g_next_dir_handle = 0xD1000000u;

struct GuestAddrInfoAllocation {
    std::vector<uint32_t> blocks;
};
static std::mutex g_addrinfo_mutex;
static std::unordered_map<uint32_t, GuestAddrInfoAllocation> g_addrinfo_allocations;

static std::mutex g_gz_mutex;
static std::unordered_map<uint32_t, gzFile> g_gz_files;
static uint32_t g_next_gz_handle = 0xD2000000u;

struct InflateState {
    z_stream stream{};
    uint32_t message = 0;
};
static std::mutex g_inflate_mutex;
static std::unordered_map<uint32_t, std::unique_ptr<InflateState>> g_inflate_states;

static std::mutex g_guard_mutex;
static std::unordered_set<uint32_t> g_guards_in_progress;

static std::mutex g_atomic_mutex;

static std::unordered_map<uint32_t, uint32_t> g_fgetwln_buffers;

static uint32_t GuestErrnoAddress(DynarmicHost& host) {
    if (!g_guest_errno_address) {
        g_guest_errno_address = host.GuestAlloc(sizeof(uint32_t), alignof(uint32_t));
        if (g_guest_errno_address) host.MemoryWrite32(g_guest_errno_address, 0);
        else g_guest_errno_address = 0x300; // initialized fallback; allocation failure remains visible elsewhere
    }
    return g_guest_errno_address;
}

// memmove through a write alias must not compare two address spaces.
static bool GuestRangesDisjoint(uint32_t first, uint32_t second, size_t length) {
    return uint64_t{first} + length <= second || uint64_t{second} + length <= first;
}

static void SetGuestErrno(DynarmicHost& host, int value) {
    host.MemoryWrite32(GuestErrnoAddress(host), static_cast<uint32_t>(value));
}

static void NetworkResult(Dynarmic::A32::Jit& jit, DynarmicHost& host,
                          int64_t result) {
    const int error = errno;
    if (result < 0) SetGuestErrno(host, error);
    jit.Regs()[0] = static_cast<uint32_t>(result);
}

static void NetworkFailure(Dynarmic::A32::Jit& jit, DynarmicHost& host, int error) {
    SetGuestErrno(host, error);
    jit.Regs()[0] = static_cast<uint32_t>(-1);
}

static void NetworkBindConnect(Dynarmic::A32::Jit& jit, DynarmicHost& host,
                               bool connect_socket) {
    const uint32_t address = jit.Regs()[1];
    const socklen_t length = jit.Regs()[2];
    const auto* native = address ? static_cast<const sockaddr*>(GuestSpan(
        host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, length)) : nullptr;
    if ((address && !native) || (!address && length)) {
        NetworkFailure(jit, host, EFAULT); return;
    }
    const int fd = static_cast<int32_t>(jit.Regs()[0]);
    NetworkResult(jit, host, connect_socket ? ::connect(fd, native, length)
                                           : ::bind(fd, native, length));
}

static void NetworkTransfer(Dynarmic::A32::Jit& jit, DynarmicHost& host,
                            bool receive, bool datagram) {
    void* buffer = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                             jit.Regs()[1], jit.Regs()[2]);
    if (!buffer) { NetworkFailure(jit, host, EFAULT); return; }
    const int fd = static_cast<int32_t>(jit.Regs()[0]);
    const int flags = static_cast<int32_t>(jit.Regs()[3]);
    if (!datagram) {
        NetworkResult(jit, host, receive ? ::recv(fd, buffer, jit.Regs()[2], flags)
                                       : ::send(fd, buffer, jit.Regs()[2], flags));
        return;
    }
    Arm32CallArgs args(jit, host, 4);
    const uint32_t address = args.Ptr();
    const uint32_t length_argument = args.U32();
    if (receive && length_argument && !GuestSpan(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), length_argument, sizeof(uint32_t))) {
        NetworkFailure(jit, host, EFAULT); return;
    }
    socklen_t length = receive ? (length_argument ? host.MemoryRead32(length_argument) : 0)
                              : length_argument;
    auto* native_address = address ? static_cast<sockaddr*>(GuestSpan(
        host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, length)) : nullptr;
    if ((address && !native_address) || (receive && address && !length_argument)) {
        NetworkFailure(jit, host, EFAULT); return;
    }
    const ssize_t result = receive
        ? ::recvfrom(fd, buffer, jit.Regs()[2], flags, native_address,
                     length_argument ? &length : nullptr)
        : ::sendto(fd, buffer, jit.Regs()[2], flags, native_address, length);
    const int error = errno;
    if (receive && length_argument && result >= 0) host.MemoryWrite32(length_argument, length);
    if (result < 0) SetGuestErrno(host, error);
    jit.Regs()[0] = static_cast<uint32_t>(result);
}

static void NetworkSocketOption(Dynarmic::A32::Jit& jit, DynarmicHost& host,
                                bool get) {
    Arm32CallArgs args(jit, host, 4);
    const uint32_t length_argument = args.U32();
    if (get && !GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                         length_argument, sizeof(uint32_t))) {
        NetworkFailure(jit, host, EFAULT); return;
    }
    socklen_t length = get ? host.MemoryRead32(length_argument) : length_argument;
    void* value = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                            jit.Regs()[3], length);
    if (!value) { NetworkFailure(jit, host, EFAULT); return; }
    const int fd = static_cast<int32_t>(jit.Regs()[0]);
    const int level = static_cast<int32_t>(jit.Regs()[1]);
    const int option = static_cast<int32_t>(jit.Regs()[2]);
    int result;
    if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO)) {
        timeval native{};
        Arm32Timeval guest{};
        if (!get) {
            if (length < sizeof(guest)) { NetworkFailure(jit, host, EINVAL); return; }
            std::memcpy(&guest, value, sizeof(guest));
            if (!UnpackArm32Timeval(guest, native)) { NetworkFailure(jit, host, EINVAL); return; }
            result = ::setsockopt(fd, level, option, &native, sizeof(native));
        } else {
            socklen_t native_length = sizeof(native);
            result = ::getsockopt(fd, level, option, &native, &native_length);
            if (result == 0) {
                if (!PackArm32Timeval(native, guest)) { NetworkFailure(jit, host, EOVERFLOW); return; }
                length = std::min<socklen_t>(length, sizeof(guest));
                std::memcpy(value, &guest, length);
            }
        }
    } else {
        // Audited UE3/RakNet options use int, linger, or raw byte payloads.
        result = get ? ::getsockopt(fd, level, option, value, &length)
                     : ::setsockopt(fd, level, option, value, length);
    }
    const int error = errno;
    if (get && result == 0) host.MemoryWrite32(length_argument, length);
    if (result < 0) SetGuestErrno(host, error);
    jit.Regs()[0] = static_cast<uint32_t>(result);
}

static void NetworkIoctl(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    const int fd = static_cast<int32_t>(jit.Regs()[0]);
    const unsigned long request = jit.Regs()[1];
    const uint32_t address = jit.Regs()[2];
    if (request == SIOCGIFCONF) {
        auto* storage = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                  address, sizeof(Arm32IfConf));
        if (!storage) { NetworkFailure(jit, host, EFAULT); return; }
        Arm32IfConf guest{};
        std::memcpy(&guest, storage, sizeof(guest));
        if (guest.length < 0 || static_cast<size_t>(guest.length) / kArm32IfReqSize >
                static_cast<size_t>(INT_MAX) / sizeof(ifreq)) {
            NetworkFailure(jit, host, EINVAL); return;
        }
        void* output = guest.buffer ? GuestSpan(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), guest.buffer, guest.length) : nullptr;
        if (guest.buffer && !output) { NetworkFailure(jit, host, EFAULT); return; }
        const size_t capacity = guest.buffer ? guest.length / kArm32IfReqSize : 0;
        try {
            std::vector<ifreq> entries(guest.buffer ? std::max<size_t>(capacity, 1) : 0);
            ifconf native{};
            native.ifc_len = static_cast<int>(capacity * sizeof(ifreq));
            native.ifc_req = guest.buffer ? entries.data() : nullptr;
            const int result = ::ioctl(fd, request, &native);
            const int error = errno;
            if (result == 0) {
                const size_t count = native.ifc_len / sizeof(ifreq);
                if (guest.buffer) {
                    if (count > capacity) { NetworkFailure(jit, host, EIO); return; }
                    for (size_t i = 0; i < count; ++i)
                        std::memcpy(static_cast<uint8_t*>(output) + i * kArm32IfReqSize,
                                    &entries[i], kArm32IfReqSize);
                }
                guest.length = static_cast<int32_t>(count * kArm32IfReqSize);
                std::memcpy(storage, &guest, sizeof(guest));
            } else SetGuestErrno(host, error);
            jit.Regs()[0] = static_cast<uint32_t>(result);
        } catch (const std::bad_alloc&) { NetworkFailure(jit, host, ENOMEM); }
        return;
    }
    size_t size = 0;
    if (request == FIONREAD || request == FIONBIO) size = sizeof(int32_t);
    else if (request == TIOCGWINSZ) size = sizeof(winsize);
    else if (request == SIOCGIFADDR || request == SIOCGIFNETMASK ||
             request == SIOCGIFBRDADDR || request == SIOCGIFDSTADDR ||
             request == SIOCGIFFLAGS || request == SIOCGIFINDEX || request == SIOCGIFMTU)
        size = kArm32IfReqSize;
    else { NetworkFailure(jit, host, ENOTTY); return; }
    void* argument = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, size);
    if (!argument) { NetworkFailure(jit, host, EFAULT); return; }
    if (size == kArm32IfReqSize) {
        ifreq native{};
        std::memcpy(&native, argument, kArm32IfReqSize);
        const int result = ::ioctl(fd, request, &native);
        const int error = errno;
        if (result == 0) std::memcpy(argument, &native, kArm32IfReqSize);
        else SetGuestErrno(host, error);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    } else NetworkResult(jit, host, ::ioctl(fd, request, argument));
}

static uint32_t CurrentGuestThreadId() {
    if (!g_current_guest_thread) {
        g_current_guest_thread = g_next_guest_thread_id.fetch_add(1, std::memory_order_relaxed);
        if (!g_current_guest_thread) {
            g_current_guest_thread = g_next_guest_thread_id.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return g_current_guest_thread;
}

static void RunGuestTlsDestructors(DynarmicHost& host) {
    constexpr int kDestructorIterations = 4;
    for (int iteration = 0; iteration < kDestructorIterations; ++iteration) {
        std::vector<std::pair<uint32_t, uint32_t>> calls;
        {
            std::lock_guard<std::mutex> lock(g_tls_key_mutex);
            for (auto& entry : g_tls_values) {
                auto destructor = g_tls_destructors.find(entry.first);
                if (entry.second && destructor != g_tls_destructors.end() && destructor->second) {
                    calls.emplace_back(destructor->second, entry.second);
                    entry.second = 0;
                }
            }
        }
        if (calls.empty()) break;
        for (const auto& call : calls) host.RunFunction(call.first, {call.second});
    }
}

static std::shared_ptr<GuestMutexState> GetGuestMutex(DynarmicHost& host,
                                                       uint32_t address, bool create) {
    if (!address) return {};
    std::lock_guard<std::mutex> lock(g_sync_state_mutex);
    auto it = g_guest_mutexes.find(address);
    if (it != g_guest_mutexes.end()) return it->second;
    if (!create) return {};
    auto state = std::make_shared<GuestMutexState>();
    const uint32_t encoded_type = (host.MemoryRead32(address) >> 14) & 3u;
    if (encoded_type <= static_cast<uint32_t>(PTHREAD_MUTEX_ERRORCHECK)) {
        state->type = static_cast<int>(encoded_type);
    }
    g_guest_mutexes.emplace(address, state);
    return state;
}

static std::shared_ptr<GuestCondState> GetGuestCond(DynarmicHost& host,
                                                    uint32_t address, bool create) {
    if (!address) return {};
    std::lock_guard<std::mutex> lock(g_sync_state_mutex);
    auto it = g_guest_conds.find(address);
    if (it != g_guest_conds.end()) return it->second;
    if (!create) return {};
    auto state = std::make_shared<GuestCondState>();
    state->monotonic = (host.MemoryRead32(address) & (1u << 1)) != 0;
    g_guest_conds.emplace(address, state);
    return state;
}

static int LockGuestMutex(const std::shared_ptr<GuestMutexState>& state,
                          uint32_t owner, bool try_only, uint32_t depth = 1) {
    if (!state) return EINVAL;
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->destroyed) return EINVAL;
    if (state->owner == owner) {
        if (state->type == PTHREAD_MUTEX_RECURSIVE) {
            if (UINT32_MAX - state->recursion < depth) return EAGAIN;
            state->recursion += depth;
            return 0;
        }
        return try_only ? EBUSY : EDEADLK;
    }
    if (try_only && state->owner) return EBUSY;
    const bool waited = state->owner != 0;
    if (waited) ++state->waiters;
    while (state->owner && !state->destroyed) state->available.wait(lock);
    if (waited) --state->waiters;
    if (state->destroyed) return EINVAL;
    state->owner = owner;
    state->recursion = depth;
    return 0;
}

static int UnlockGuestMutex(const std::shared_ptr<GuestMutexState>& state,
                            uint32_t owner, uint32_t* released_depth = nullptr) {
    if (!state) return EINVAL;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->destroyed) return EINVAL;
    if (state->owner != owner || !state->recursion) return EPERM;
    if (released_depth) {
        *released_depth = state->recursion;
        state->recursion = 0;
    } else {
        --state->recursion;
    }
    if (!state->recursion) {
        state->owner = 0;
        state->available.notify_one();
    }
    return 0;
}

static std::shared_ptr<GuestSemaphoreState> GetGuestSemaphore(uint32_t address) {
    if (!address) return {};
    std::lock_guard<std::mutex> lock(g_sync_state_mutex);
    auto it = g_guest_semaphores.find(address);
    return it == g_guest_semaphores.end() ? std::shared_ptr<GuestSemaphoreState>{} : it->second;
}

#undef st_atime
#undef st_mtime
#undef st_ctime
#undef st_atime_nsec
#undef st_mtime_nsec
#undef st_ctime_nsec

// 32-bit Android ARM struct stat layout.  The bridge itself is AArch64, so its
// native struct stat is not ABI-compatible with the ARM32 guest.  In particular
// UE3 reads st_mode at +16 and the 64-bit st_size at +48.
struct Arm32Stat {
    uint64_t st_dev;
    unsigned char __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    unsigned char __pad3[4];
    int64_t st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint64_t st_ino;
};

static_assert(offsetof(Arm32Stat, st_mode) == 16);
static_assert(offsetof(Arm32Stat, st_size) == 48);
static_assert(sizeof(Arm32Stat) == 104);

static void WriteArm32Stat(uint8_t* guest_base, uint32_t gaddr, const struct stat& st) {
    if (!gaddr) return;
    Arm32Stat* arm_st = reinterpret_cast<Arm32Stat*>(GuestToHost(guest_base, gaddr));
    if (!arm_st) return;
    memset(arm_st, 0, sizeof(Arm32Stat));
    arm_st->st_dev = static_cast<uint64_t>(st.st_dev);
    arm_st->__st_ino = static_cast<uint32_t>(st.st_ino);
    arm_st->st_mode = static_cast<uint32_t>(st.st_mode);
    arm_st->st_nlink = static_cast<uint32_t>(st.st_nlink);
    arm_st->st_uid = static_cast<uint32_t>(st.st_uid);
    arm_st->st_gid = static_cast<uint32_t>(st.st_gid);
    arm_st->st_rdev = static_cast<uint64_t>(st.st_rdev);
    arm_st->st_size = static_cast<int64_t>(st.st_size);
    arm_st->st_blksize = static_cast<uint32_t>(st.st_blksize);
    arm_st->st_blocks = static_cast<uint64_t>(st.st_blocks);
    arm_st->st_atime = static_cast<uint32_t>(st.st_atim.tv_sec);
    arm_st->st_atime_nsec = static_cast<uint32_t>(st.st_atim.tv_nsec);
    arm_st->st_mtime = static_cast<uint32_t>(st.st_mtim.tv_sec);
    arm_st->st_mtime_nsec = static_cast<uint32_t>(st.st_mtim.tv_nsec);
    arm_st->st_ctime = static_cast<uint32_t>(st.st_ctim.tv_sec);
    arm_st->st_ctime_nsec = static_cast<uint32_t>(st.st_ctim.tv_nsec);
    arm_st->st_ino = static_cast<uint64_t>(st.st_ino);
}

// Explicit Android/ARM32 layouts.  Never memcpy LP64 host structures into the guest:
// time_t/long/pointers are 32-bit in Horn even though this bridge is AArch64.
struct Arm32Tm {
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int32_t tm_wday, tm_yday, tm_isdst;
    int32_t tm_gmtoff;
    uint32_t tm_zone;
};
static_assert(sizeof(Arm32Tm) == 44);

struct Arm32Dirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};
static_assert(offsetof(Arm32Dirent, d_name) == 19);

struct Arm32ZStream {
    uint32_t next_in, avail_in, total_in;
    uint32_t next_out, avail_out, total_out;
    uint32_t msg, state, zalloc, zfree, opaque;
    int32_t data_type;
    uint32_t adler, reserved;
};
static_assert(sizeof(Arm32ZStream) == 56);

struct Arm32Sysinfo {
    int32_t uptime;
    uint32_t loads[3];
    uint32_t totalram, freeram, sharedram, bufferram;
    uint32_t totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t totalhigh, freehigh, mem_unit;
    char reserved[8];
};
static_assert(sizeof(Arm32Sysinfo) == 64);

static uint32_t GuestString(DynarmicHost& host, const char* value) {
    if (!value) return 0;
    const size_t len = strlen(value) + 1;
    uint32_t out = host.GuestAlloc(len, 1);
    if (out) memcpy(GuestToHost(host.GetGuestMemoryBase(), out), value, len);
    return out;
}

static bool WriteArm32Tm(DynarmicHost& host, uint32_t out, const tm& value) {
    if (!out) return false;
    Arm32Tm result{};
    result.tm_sec = value.tm_sec;
    result.tm_min = value.tm_min;
    result.tm_hour = value.tm_hour;
    result.tm_mday = value.tm_mday;
    result.tm_mon = value.tm_mon;
    result.tm_year = value.tm_year;
    result.tm_wday = value.tm_wday;
    result.tm_yday = value.tm_yday;
    result.tm_isdst = value.tm_isdst;
    result.tm_gmtoff = static_cast<int32_t>(value.tm_gmtoff);
    const char* zone = value.tm_zone ? value.tm_zone : "";
    const size_t zone_size = strlen(zone) + 1;
    if (zone_size > g_tm_zone_capacity) {
        if (g_tm_zone_addr) host.GuestFree(g_tm_zone_addr);
        g_tm_zone_addr = host.GuestAlloc(zone_size, 1);
        g_tm_zone_capacity = g_tm_zone_addr ? zone_size : 0;
    }
    if (g_tm_zone_addr) memcpy(GuestToHost(host.GetGuestMemoryBase(), g_tm_zone_addr), zone, zone_size);
    result.tm_zone = g_tm_zone_addr;
    memcpy(GuestToHost(host.GetGuestMemoryBase(), out), &result, sizeof(result));
    return true;
}

static bool ReadArm32Tm(DynarmicHost& host, uint32_t in, tm* value) {
    if (!in || !value) return false;
    Arm32Tm source{};
    memcpy(&source, GuestToHost(host.GetGuestMemoryBase(), in), sizeof(source));
    memset(value, 0, sizeof(*value));
    value->tm_sec = source.tm_sec;
    value->tm_min = source.tm_min;
    value->tm_hour = source.tm_hour;
    value->tm_mday = source.tm_mday;
    value->tm_mon = source.tm_mon;
    value->tm_year = source.tm_year;
    value->tm_wday = source.tm_wday;
    value->tm_yday = source.tm_yday;
    value->tm_isdst = source.tm_isdst;
    value->tm_gmtoff = source.tm_gmtoff;
    value->tm_zone = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), source.tm_zone));
    return true;
}

static FILE* GuestFile(uint32_t handle) {
    // Caller holds g_file_mutex for the complete FILE* operation so fclose on
    // another guest pthread cannot invalidate the pointer after lookup.
    if (handle == 0x200u) return stdin; // ARM32 Bionic __sF[0]
    auto it = g_file_map.find(handle);
    return it == g_file_map.end() ? nullptr : it->second;
}

static const wchar_t* GuestWideCString(DynarmicHost& host, uint32_t address) {
    static_assert(sizeof(wchar_t) == sizeof(uint32_t));
    const size_t arena_size = host.GetGuestMemorySize();
    if (!GuestRangeValid(arena_size, address, sizeof(wchar_t))) return nullptr;
    const auto* text = reinterpret_cast<const wchar_t*>(
        host.GetGuestMemoryBase() + address);
    const size_t limit = std::min(
        (arena_size - address) / sizeof(wchar_t), size_t{1024 * 1024});
    return std::wmemchr(text, L'\0', limit) ? text : nullptr;
}

static bool SyncInflateFromGuest(DynarmicHost& host, uint32_t addr, InflateState& state) {
    if (!addr) return false;
    Arm32ZStream* in = reinterpret_cast<Arm32ZStream*>(GuestToHost(host.GetGuestMemoryBase(), addr));
    if (!in) return false;
    state.stream.next_in = reinterpret_cast<Bytef*>(GuestToHost(host.GetGuestMemoryBase(), in->next_in));
    state.stream.avail_in = in->avail_in;
    state.stream.next_out = reinterpret_cast<Bytef*>(GuestToHost(host.GetGuestMemoryBase(), in->next_out));
    state.stream.avail_out = in->avail_out;
    return true;
}

static void SyncInflateToGuest(DynarmicHost& host, uint32_t addr, InflateState& state,
                               uint32_t old_in, uint32_t old_out) {
    Arm32ZStream* out = reinterpret_cast<Arm32ZStream*>(GuestToHost(host.GetGuestMemoryBase(), addr));
    if (!out) return;
    out->next_in = state.stream.next_in ? HostToGuest(host.GetGuestMemoryBase(), state.stream.next_in) : old_in;
    out->avail_in = state.stream.avail_in;
    out->total_in = static_cast<uint32_t>(state.stream.total_in);
    out->next_out = state.stream.next_out ? HostToGuest(host.GetGuestMemoryBase(), state.stream.next_out) : old_out;
    out->avail_out = state.stream.avail_out;
    out->total_out = static_cast<uint32_t>(state.stream.total_out);
    out->data_type = state.stream.data_type;
    out->adler = static_cast<uint32_t>(state.stream.adler);
    out->reserved = static_cast<uint32_t>(state.stream.reserved);
    if (state.stream.msg) {
        if (state.message) host.GuestFree(state.message);
        state.message = GuestString(host, state.stream.msg);
    }
    out->msg = state.message;
    // z_stream internals are host-owned and deliberately never exposed as LP64 pointers.
    out->state = 0;
}

static bool TraceGuestZlib() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.horn.zlib_trace", value) > 0 && value[0] == '1';
}

static int StoreScanInteger(DynarmicHost& host, uint32_t dest, int length,
                            bool is_signed, uint64_t value) {
    if (!dest) return 0;
    void* out = GuestToHost(host.GetGuestMemoryBase(), dest);
    if (!out) return 0;
    if (length == 2 || length == 3 || length == 6) { // ll, j, L
        uint64_t v = value;
        memcpy(out, &v, 8);
    } else if (length == -2) {
        uint8_t v = static_cast<uint8_t>(value);
        memcpy(out, &v, 1);
    } else if (length == -1) {
        uint16_t v = static_cast<uint16_t>(value);
        memcpy(out, &v, 2);
    } else {
        uint32_t v = static_cast<uint32_t>(value);
        memcpy(out, &v, 4);
    }
    (void)is_signed;
    return 1;
}

// ARM32 va_list is a cursor over 32-bit argument words for this softfp guest.
// This scanner implements the ISO conversions UE3 uses instead of pretending
// success: invalid handles/formats fail with errno and are logged.
static int GuestVfscanf(DynarmicHost& host, uint32_t file_handle,
                        const char* format, uint32_t va_cursor) {
    std::lock_guard<std::mutex> file_lock(g_file_mutex);
    FILE* file = GuestFile(file_handle);
    if (!file || !format || !va_cursor) {
        SetGuestErrno(host, file ? EINVAL : EBADF);
        LOGE("vfscanf: invalid FILE/format/ARM32 va_list");
        return EOF;
    }
    char line[8192];
    if (!fgets(line, sizeof(line), file)) return EOF;
    const char* input = line;
    const char* begin = input;
    int assigned = 0;
    auto next_dest = [&]() {
        uint32_t p = host.MemoryRead32(va_cursor);
        va_cursor += 4;
        return p;
    };
    for (const char* f = format; *f;) {
        if (isspace(static_cast<unsigned char>(*f))) {
            while (isspace(static_cast<unsigned char>(*f))) ++f;
            while (isspace(static_cast<unsigned char>(*input))) ++input;
            continue;
        }
        if (*f != '%') {
            if (*input != *f) break;
            ++input; ++f;
            continue;
        }
        ++f;
        if (*f == '%') {
            if (*input != '%') break;
            ++input; ++f;
            continue;
        }
        bool suppress = false;
        if (*f == '*') { suppress = true; ++f; }
        int width = 0;
        while (isdigit(static_cast<unsigned char>(*f))) width = width * 10 + (*f++ - '0');
        int length = 0;
        if (*f == 'h') { length = -1; ++f; if (*f == 'h') { length = -2; ++f; } }
        else if (*f == 'l') { length = 1; ++f; if (*f == 'l') { length = 2; ++f; } }
        else if (*f == 'j') { length = 3; ++f; }
        else if (*f == 'z') { length = 4; ++f; }
        else if (*f == 't') { length = 5; ++f; }
        else if (*f == 'L') { length = 6; ++f; }
        const char conv = *f ? *f++ : '\0';
        if (conv != 'c' && conv != '[' && conv != 'n') {
            while (isspace(static_cast<unsigned char>(*input))) ++input;
        }
        uint32_t dest = suppress ? 0 : next_dest();
        if (conv == 'n') {
            if (!suppress) assigned += StoreScanInteger(host, dest, length, true,
                static_cast<uint64_t>(input - begin));
            continue;
        }
        if (conv == 's' || conv == 'c') {
            int limit = width ? width : (conv == 'c' ? 1 : INT_MAX);
            char* out = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), dest));
            int n = 0;
            while (*input && n < limit && (conv == 'c' || !isspace(static_cast<unsigned char>(*input)))) {
                if (!suppress && out) out[n] = *input;
                ++input; ++n;
            }
            if (!n) break;
            if (!suppress && out && conv == 's') out[n] = '\0';
            if (!suppress) ++assigned;
            continue;
        }
        if (conv == '[') {
            bool invert = false;
            if (*f == '^') { invert = true; ++f; }
            bool table[256]{};
            if (*f == ']') table[static_cast<unsigned char>(*f++)] = true;
            while (*f && *f != ']') table[static_cast<unsigned char>(*f++)] = true;
            if (*f == ']') ++f;
            char* out = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), dest));
            int n = 0, limit = width ? width : INT_MAX;
            while (*input && n < limit && (table[static_cast<unsigned char>(*input)] != invert)) {
                if (!suppress && out) out[n] = *input;
                ++input; ++n;
            }
            if (!n) break;
            if (!suppress && out) out[n] = '\0';
            if (!suppress) ++assigned;
            continue;
        }
        char* end = nullptr;
        if (strchr("diouxXp", conv)) {
            int base_num = (conv == 'o') ? 8 : ((conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10);
            uint64_t value = (conv == 'd' || conv == 'i')
                ? static_cast<uint64_t>(strtoll(input, &end, conv == 'i' ? 0 : base_num))
                : strtoull(input, &end, conv == 'i' ? 0 : base_num);
            if (end == input) break;
            if (width && end - input > width) end = const_cast<char*>(input + width);
            input = end;
            if (!suppress) assigned += StoreScanInteger(host, dest, length, conv == 'd' || conv == 'i', value);
            continue;
        }
        if (strchr("aAeEfFgG", conv)) {
            double value = strtod(input, &end);
            if (end == input) break;
            input = end;
            if (!suppress && dest) {
                void* out = GuestToHost(host.GetGuestMemoryBase(), dest);
                if (length == 1 || length == 6) memcpy(out, &value, 8);
                else { float v = static_cast<float>(value); memcpy(out, &v, 4); }
                ++assigned;
            }
            continue;
        }
        LOGE("vfscanf: unsupported conversion '%%%c'", conv ? conv : '?');
        SetGuestErrno(host, EINVAL);
        break;
    }
    // fgets may have read beyond the last conversion. Restore unread bytes so
    // subsequent stdio observes the same stream position as fscanf.
    size_t unread = strlen(input);
    while (unread) ungetc(static_cast<unsigned char>(input[--unread]), file);
    return assigned;
}

// Guest printf formatter. ARMv7 softfp still follows AAPCS32 for variadic
// arguments: float is promoted to double, and every 64-bit value starts at an
// even core-register/stack word. Treating %f as one word from r3 turned 1.0
// (whose low double word is zero) into 0.000000 throughout UE3.
class GuestPrintfArgs {
public:
    GuestPrintfArgs(Dynarmic::A32::Jit& jit, DynarmicHost& host,
                    uint32_t first_word)
        : m_jit(&jit), m_host(host), m_word(first_word), m_memory_cursor(0) {}

    GuestPrintfArgs(DynarmicHost& host, uint32_t va_list_guest)
        : m_jit(nullptr), m_host(host), m_word(0),
          m_memory_cursor(va_list_guest) {}

    uint32_t NextWord() {
        if (!m_jit) {
            const uint32_t value = m_host.MemoryRead32(m_memory_cursor);
            m_memory_cursor += 4;
            return value;
        }
        return ReadDirectWord(m_word++);
    }

    uint64_t NextWide() {
        if (!m_jit) {
            m_memory_cursor = (m_memory_cursor + 7u) & ~7u;
            const uint64_t value =
                static_cast<uint64_t>(m_host.MemoryRead32(m_memory_cursor)) |
                (static_cast<uint64_t>(m_host.MemoryRead32(m_memory_cursor + 4))
                 << 32);
            m_memory_cursor += 8;
            return value;
        }
        m_word = (m_word + 1u) & ~1u;
        const uint64_t value =
            static_cast<uint64_t>(ReadDirectWord(m_word)) |
            (static_cast<uint64_t>(ReadDirectWord(m_word + 1)) << 32);
        m_word += 2;
        return value;
    }

private:
    uint32_t ReadDirectWord(uint32_t word) const {
        if (word < 4) return m_jit->Regs()[word];
        return m_host.MemoryRead32(
            m_jit->Regs()[13] + static_cast<uint32_t>((word - 4) * 4));
    }

    Dynarmic::A32::Jit* m_jit;
    DynarmicHost& m_host;
    uint32_t m_word;
    uint32_t m_memory_cursor;
};

enum class GuestPrintfLength {
    None,
    Char,
    Short,
    Long,
    LongLong,
    IntMax,
    Size,
    PtrDiff,
    LongDouble,
};

template <typename T>
static bool AppendHostFormatted(std::string& result, const std::string& spec,
                                T value) {
    char local[256];
    const int count = snprintf(local, sizeof(local), spec.c_str(), value);
    if (count < 0) return false;
    if (static_cast<size_t>(count) < sizeof(local)) {
        result.append(local, static_cast<size_t>(count));
        return true;
    }
    std::vector<char> expanded(static_cast<size_t>(count) + 1);
    const int repeated =
        snprintf(expanded.data(), expanded.size(), spec.c_str(), value);
    if (repeated < 0) return false;
    result.append(expanded.data(), static_cast<size_t>(repeated));
    return true;
}

static void AppendUtf8(std::string& output, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back('?');
    }
}

static std::string ReadGuestWideUtf8(DynarmicHost& host, uint32_t address) {
    if (!address) return "(null)";
    std::string result;
    for (size_t i = 0; i < 1024 * 1024; ++i) {
        const uint32_t codepoint = host.MemoryRead32(
            address + static_cast<uint32_t>(i * sizeof(uint32_t)));
        if (codepoint == 0) break;
        AppendUtf8(result, codepoint);
    }
    return result;
}

static std::string FormatGuestString(DynarmicHost& host, const char* fmt,
                                     GuestPrintfArgs& args) {
    if (!fmt) return "";
    std::string result;
    const char* p = fmt;
    while (*p) {
        if (*p != '%') {
            result.push_back(*p++);
            continue;
        }
        ++p;
        if (*p == '%') {
            result.push_back('%');
            ++p;
            continue;
        }

        std::string prefix = "%";
        while (*p && strchr("-+ #0'", *p)) prefix.push_back(*p++);

        if (*p == '*') {
            int32_t width = static_cast<int32_t>(args.NextWord());
            ++p;
            if (width < 0) {
                if (prefix.find('-') == std::string::npos) prefix.push_back('-');
                width = width == INT32_MIN ? INT32_MAX : -width;
            }
            prefix += std::to_string(width);
        } else {
            while (isdigit(static_cast<unsigned char>(*p))) {
                prefix.push_back(*p++);
            }
        }
        if (*p == '.') {
            prefix.push_back(*p++);
            if (*p == '*') {
                const int32_t precision = static_cast<int32_t>(args.NextWord());
                ++p;
                if (precision >= 0) prefix += std::to_string(precision);
                else prefix.pop_back();
            } else {
                while (isdigit(static_cast<unsigned char>(*p))) {
                    prefix.push_back(*p++);
                }
            }
        }

        GuestPrintfLength length = GuestPrintfLength::None;
        if (*p == 'h') {
            ++p;
            if (*p == 'h') {
                ++p;
                length = GuestPrintfLength::Char;
            } else {
                length = GuestPrintfLength::Short;
            }
        } else if (*p == 'l') {
            ++p;
            if (*p == 'l') {
                ++p;
                length = GuestPrintfLength::LongLong;
            } else {
                length = GuestPrintfLength::Long;
            }
        } else if (*p == 'j') {
            ++p;
            length = GuestPrintfLength::IntMax;
        } else if (*p == 'z') {
            ++p;
            length = GuestPrintfLength::Size;
        } else if (*p == 't') {
            ++p;
            length = GuestPrintfLength::PtrDiff;
        } else if (*p == 'L') {
            ++p;
            length = GuestPrintfLength::LongDouble;
        } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
            p += 3;
            length = GuestPrintfLength::LongLong;
        }

        const char conversion = *p ? *p++ : '\0';
        if (!conversion) break;
        if (strchr("di", conversion)) {
            if (length == GuestPrintfLength::LongLong ||
                length == GuestPrintfLength::IntMax) {
                const int64_t value = static_cast<int64_t>(args.NextWide());
                AppendHostFormatted(result, prefix + "ll" + conversion,
                                    static_cast<long long>(value));
            } else {
                int32_t value = static_cast<int32_t>(args.NextWord());
                if (length == GuestPrintfLength::Char)
                    value = static_cast<int8_t>(value);
                else if (length == GuestPrintfLength::Short)
                    value = static_cast<int16_t>(value);
                AppendHostFormatted(result, prefix + conversion,
                                    static_cast<int>(value));
            }
        } else if (strchr("ouxX", conversion)) {
            if (length == GuestPrintfLength::LongLong ||
                length == GuestPrintfLength::IntMax) {
                AppendHostFormatted(result, prefix + "ll" + conversion,
                                    static_cast<unsigned long long>(args.NextWide()));
            } else {
                uint32_t value = args.NextWord();
                if (length == GuestPrintfLength::Char)
                    value = static_cast<uint8_t>(value);
                else if (length == GuestPrintfLength::Short)
                    value = static_cast<uint16_t>(value);
                AppendHostFormatted(result, prefix + conversion,
                                    static_cast<unsigned int>(value));
            }
        } else if (strchr("aAeEfFgG", conversion)) {
            const uint64_t bits = args.NextWide();
            double value = 0.0;
            memcpy(&value, &bits, sizeof(value));
            AppendHostFormatted(result, prefix + conversion, value);
        } else if (conversion == 's') {
            const uint32_t address = args.NextWord();
            if (length == GuestPrintfLength::Long) {
                const std::string value = ReadGuestWideUtf8(host, address);
                AppendHostFormatted(result, prefix + "s", value.c_str());
            } else {
                const char* value = address
                    ? reinterpret_cast<const char*>(
                          GuestToHost(host.GetGuestMemoryBase(), address))
                    : "(null)";
                AppendHostFormatted(result, prefix + "s", value);
            }
        } else if (conversion == 'c') {
            const uint32_t value = args.NextWord();
            if (length == GuestPrintfLength::Long) {
                std::string encoded;
                AppendUtf8(encoded, value);
                AppendHostFormatted(result, prefix + "s", encoded.c_str());
            } else {
                AppendHostFormatted(result, prefix + "c",
                                    static_cast<int>(value));
            }
        } else if (conversion == 'p') {
            const uintptr_t value = args.NextWord();
            AppendHostFormatted(result, prefix + "p",
                                reinterpret_cast<void*>(value));
        } else if (conversion == 'n') {
            const uint32_t destination = args.NextWord();
            const uint64_t count = result.size();
            if (destination) {
                if (length == GuestPrintfLength::Char)
                    host.MemoryWrite8(destination, static_cast<uint8_t>(count));
                else if (length == GuestPrintfLength::Short)
                    host.MemoryWrite16(destination, static_cast<uint16_t>(count));
                else if (length == GuestPrintfLength::LongLong ||
                         length == GuestPrintfLength::IntMax)
                    host.MemoryWrite64(destination, count);
                else
                    host.MemoryWrite32(destination, static_cast<uint32_t>(count));
            }
        } else {
            // Keep malformed/unsupported conversions visible instead of silently
            // consuming an argument with an unknown type.
            result += prefix;
            result.push_back(conversion);
        }
    }
    return result;
}

static std::string FormatGuestString(Dynarmic::A32::Jit& jit,
                                     DynarmicHost& host, const char* fmt,
                                     uint32_t first_arg_word) {
    GuestPrintfArgs args(jit, host, first_arg_word);
    return FormatGuestString(host, fmt, args);
}

static std::string FormatGuestVaList(DynarmicHost& host, const char* fmt,
                                     uint32_t va_list_guest) {
    if (!va_list_guest) return "";
    GuestPrintfArgs args(host, va_list_guest);
    return FormatGuestString(host, fmt, args);
}

void ThunkManager::RegisterLibcThunks() {
    uint8_t* mem = m_host.GetGuestMemoryBase();

    *reinterpret_cast<uint32_t*>(mem + 0x100) = 0xDEADBEEF;
    *reinterpret_cast<uint32_t*>(mem + 0x300) = 0;

    RegisterSymbol("__stack_chk_guard", 0x100);
    RegisterSymbol("__sF", 0x200);

    RegisterThunk("__errno", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = GuestErrnoAddress(host);
    });

    // Imported legacy Bionic data objects. `_ctype_` and `_toupper_tab_` are
    // ARM32 pointer-valued symbols, so the guest first loads the symbol and
    // then dereferences it to obtain the table. Keep both pointer cells and
    // tables in guest memory; Bionic indexes the tables at [c + 1].
    const uint32_t dso_handle = m_host.GuestAlloc(4, 4);
    const uint32_t ctype_symbol = m_host.GuestAlloc(4, 4);
    const uint32_t toupper_symbol = m_host.GuestAlloc(4, 4);
    const uint32_t ctype_table = m_host.GuestAlloc(257, 1);
    const uint32_t toupper_table = m_host.GuestAlloc(257 * sizeof(int16_t), alignof(int16_t));
    if (!dso_handle || !ctype_symbol || !toupper_symbol ||
        !ctype_table || !toupper_table) {
        LOGE("FATAL: unable to allocate ARM32 Bionic data symbols");
    } else {
        m_host.MemoryWrite32(dso_handle, 0);
        m_host.MemoryWrite32(ctype_symbol, ctype_table);
        m_host.MemoryWrite32(toupper_symbol, toupper_table);
        auto* ct = reinterpret_cast<uint8_t*>(GuestToHost(mem, ctype_table));
        auto* up = reinterpret_cast<int16_t*>(GuestToHost(mem, toupper_table));
        memset(ct, 0, 257);
        up[0] = -1;
        for (int c = 0; c < 256; ++c) {
            uint8_t flags = 0;
            if (c >= 'A' && c <= 'Z') flags |= 0x01;
            if (c >= 'a' && c <= 'z') flags |= 0x02;
            if (c >= '0' && c <= '9') flags |= 0x04;
            if (isspace(c)) flags |= 0x08;
            if (ispunct(c)) flags |= 0x10;
            if (iscntrl(c)) flags |= 0x20;
            if (isxdigit(c)) flags |= 0x40;
            if (c == ' ' || c == '\t') flags |= 0x80;
            ct[c + 1] = flags;
            up[c + 1] = static_cast<int16_t>(toupper(c));
        }
        NotifyGuestMemoryWritten(ctype_table, 257);
        NotifyGuestMemoryWritten(toupper_table, 257 * sizeof(int16_t));
    }
    RegisterSymbol("__dso_handle", dso_handle);
    RegisterSymbol("_ctype_", ctype_symbol);
    RegisterSymbol("_toupper_tab_", toupper_symbol);

    RegisterThunk("__assert", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* file = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const int line = static_cast<int>(jit.Regs()[1]);
        const char* expr = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        LOGE("Guest assertion failed: %s at %s:%d", expr ? expr : "", file ? file : "", line);
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
    });
    RegisterThunk("__atomic_cmpxchg", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_atomic_mutex);
        uint32_t* ptr = reinterpret_cast<uint32_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        if (!ptr) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 1; return; }
        if (*ptr == jit.Regs()[0]) { *ptr = jit.Regs()[1]; jit.Regs()[0] = 0; }
        else jit.Regs()[0] = 1;
    });
    auto atomic_delta = [](int delta) {
        return [delta](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            std::lock_guard<std::mutex> lock(g_atomic_mutex);
            uint32_t* ptr = reinterpret_cast<uint32_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
            if (!ptr) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
            uint32_t old = *ptr;
            *ptr = static_cast<uint32_t>(old + delta);
            jit.Regs()[0] = old; // legacy Bionic atomics return the pre-operation value
        };
    };
    RegisterThunk("__atomic_inc", atomic_delta(1));
    RegisterThunk("__atomic_dec", atomic_delta(-1));
    RegisterThunk("__atomic_swap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_atomic_mutex);
        uint32_t* ptr = reinterpret_cast<uint32_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!ptr) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        const uint32_t old = *ptr;
        *ptr = jit.Regs()[0];
        jit.Regs()[0] = old;
    });
    RegisterThunk("__cxa_guard_acquire", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t guard = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        if (!guard) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        if (host.MemoryRead8(guard) != 0) { jit.Regs()[0] = 0; return; }
        if (!g_guards_in_progress.insert(guard).second) {
            LOGE("__cxa_guard_acquire recursive/in-progress guard 0x%08X", guard);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = 1;
    });
    RegisterThunk("__cxa_guard_release", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t guard = jit.Regs()[0];
        if (guard) host.MemoryWrite8(guard, 1);
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        g_guards_in_progress.erase(guard);
    });
    RegisterThunk("__isfinitef", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = std::isfinite(ReadSF(jit, 0)); });
    RegisterThunk("__isinff", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = std::isinf(ReadSF(jit, 0)); });
    RegisterThunk("__isinf", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = std::isinf(ReadDF(jit, 0)); });

    auto unary_float = [](float (*fn)(float)) {
        return [fn](Dynarmic::A32::Jit& jit, DynarmicHost&) { WriteSF(jit, 0, fn(ReadSF(jit, 0))); };
    };
    RegisterThunk("asinf", unary_float(asinf));
    RegisterThunk("ceilf", unary_float(ceilf));
    RegisterThunk("expf", unary_float(expf));
    RegisterThunk("floorf", unary_float(floorf));
    RegisterThunk("logf", unary_float(logf));
    RegisterThunk("roundf", unary_float(roundf));
    RegisterThunk("tanf", unary_float(tanf));
    RegisterThunk("atan2f", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { WriteSF(jit, 0, atan2f(ReadSF(jit, 0), ReadSF(jit, 1))); });
    RegisterThunk("fmodf", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { WriteSF(jit, 0, fmodf(ReadSF(jit, 0), ReadSF(jit, 1))); });
    RegisterThunk("fmod", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { WriteDF(jit, 0, fmod(ReadDF(jit, 0), ReadDF(jit, 1))); });
    RegisterThunk("frexpf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int exponent = 0;
        const float result = std::frexp(ReadSF(jit, 0), &exponent);
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       jit.Regs()[1], sizeof(exponent))) {
            SetGuestErrno(host, EFAULT);
            return;
        }
        host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(exponent));
        WriteSF(jit, 0, result);
    });
    RegisterThunk("difftime", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        WriteDF(jit, 0, difftime(static_cast<time_t>(static_cast<int32_t>(jit.Regs()[0])),
                                 static_cast<time_t>(static_cast<int32_t>(jit.Regs()[1]))));
    });

    // -------------------------------------------------------------
    // Additional POSIX & Math Thunks for FMOD
    // -------------------------------------------------------------

    auto read_double_01 = [](const Dynarmic::A32::Jit& jit) -> double {
        uint64_t raw = (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
        double val;
        memcpy(&val, &raw, sizeof(double));
        return val;
    };
    auto write_double_01 = [](Dynarmic::A32::Jit& jit, double val) {
        uint64_t raw;
        memcpy(&raw, &val, sizeof(double));
        jit.Regs()[0] = static_cast<uint32_t>(raw & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>((raw >> 32) & 0xFFFFFFFF);
    };

    RegisterThunk("sin", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, sin(read_double_01(jit)));
    });
    RegisterThunk("cos", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, cos(read_double_01(jit)));
    });
    RegisterThunk("tan", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, tan(read_double_01(jit)));
    });
    RegisterThunk("acos", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, acos(read_double_01(jit)));
    });
    RegisterThunk("asin", [=](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        write_double_01(jit, asin(read_double_01(jit)));
    });
    RegisterThunk("atan", [=](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        write_double_01(jit, atan(read_double_01(jit)));
    });
    RegisterThunk("ceil", [=](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        write_double_01(jit, ceil(read_double_01(jit)));
    });
    RegisterThunk("atan2", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        double y = read_double_01(jit);
        uint64_t raw_x = (static_cast<uint64_t>(jit.Regs()[3]) << 32) | jit.Regs()[2];
        double x;
        memcpy(&x, &raw_x, sizeof(double));
        write_double_01(jit, atan2(y, x));
    });
    RegisterThunk("exp", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, exp(read_double_01(jit)));
    });
    RegisterThunk("log", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, log(read_double_01(jit)));
    });
    RegisterThunk("log10", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, log10(read_double_01(jit)));
    });
    RegisterThunk("log10f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = log10f(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });
    RegisterThunk("pow", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        double x = read_double_01(jit);
        uint64_t raw_y = (static_cast<uint64_t>(jit.Regs()[3]) << 32) | jit.Regs()[2];
        double y;
        memcpy(&y, &raw_y, sizeof(double));
        write_double_01(jit, pow(x, y));
    });
    RegisterThunk("sqrt", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, sqrt(read_double_01(jit)));
    });
    RegisterThunk("floor", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, floor(read_double_01(jit)));
    });
    RegisterThunk("rint", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        write_double_01(jit, rint(read_double_01(jit)));
    });
    RegisterThunk("lrintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(lrintf(val));
    });
    RegisterThunk("frexp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t raw = (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
        double val;
        memcpy(&val, &raw, sizeof(double));
        uint32_t exp_ptr = jit.Regs()[2];
        int exp_val = 0;
        double res = frexp(val, &exp_val);
        if (exp_ptr) host.MemoryWrite32(exp_ptr, static_cast<uint32_t>(exp_val));
        uint64_t res_raw;
        memcpy(&res_raw, &res, sizeof(double));
        jit.Regs()[0] = static_cast<uint32_t>(res_raw & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>((res_raw >> 32) & 0xFFFFFFFF);
    });
    RegisterThunk("modf", [=](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        double integral = 0.0;
        const double fractional = modf(read_double_01(jit), &integral);
        const uint32_t integral_address = jit.Regs()[2];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       integral_address, sizeof(integral))) {
            SetGuestErrno(host, EFAULT);
            write_double_01(jit, 0.0);
            return;
        }
        std::memcpy(GuestToHost(host.GetGuestMemoryBase(), integral_address),
                    &integral, sizeof(integral));
        write_double_01(jit, fractional);
    });
    RegisterThunk("ldexp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t raw = (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
        double val;
        memcpy(&val, &raw, sizeof(double));
        int exp_val = static_cast<int>(jit.Regs()[2]);
        double res = ldexp(val, exp_val);
        uint64_t res_raw;
        memcpy(&res_raw, &res, sizeof(double));
        jit.Regs()[0] = static_cast<uint32_t>(res_raw & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>((res_raw >> 32) & 0xFFFFFFFF);
    });

    // Memory & Strings
    RegisterThunk("memmove", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dst = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        void* h_dst = GuestToHost(host.GetGuestMemoryBase(), dst);
        const void* h_src = GuestToHost(host.GetGuestMemoryBase(), src);
        if (GuestLowGuardHit(dst, n)) { host.ReportLowGuardWrite(dst, n); h_dst = nullptr; }
        if (h_dst && h_src && n > 0) {
            GuestMappedWriteGuard write(dst, n, GuestRangesDisjoint(dst, src, n));
            memmove(write.Destination(h_dst), h_src, n);
        }
        jit.Regs()[0] = dst;
    });
    RegisterThunk("memmem", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t haystack = jit.Regs()[0];
        uint32_t hlen = jit.Regs()[1];
        uint32_t needle = jit.Regs()[2];
        uint32_t nlen = jit.Regs()[3];
        const void* h_hay = GuestToHost(host.GetGuestMemoryBase(), haystack);
        const void* h_nee = GuestToHost(host.GetGuestMemoryBase(), needle);
        if (h_hay && h_nee && hlen >= nlen) {
            const void* found = memmem(h_hay, hlen, h_nee, nlen);
            if (found) {
                uint32_t offset = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(found) - reinterpret_cast<const uint8_t*>(h_hay));
                jit.Regs()[0] = haystack + offset;
                return;
            }
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("stpcpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t dst = jit.Regs()[0];
        const uint32_t src = jit.Regs()[1];
        const char* source = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), src);
        if (!source) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }

        const size_t length = std::strlen(source);
        if (length > std::numeric_limits<uint32_t>::max() - dst) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        auto* destination = static_cast<char*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), dst,
            length + 1));
        if (!destination) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }

        std::memcpy(destination, source, length + 1);
        jit.Regs()[0] = dst + static_cast<uint32_t>(length);
    });
    RegisterThunk("toupper", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const int value = static_cast<int32_t>(jit.Regs()[0]);
        if (value != EOF && value >= 0 && value <= UCHAR_MAX) {
            jit.Regs()[0] = static_cast<uint32_t>(std::toupper(value));
        }
    });
    RegisterThunk("tolower", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const int value = static_cast<int32_t>(jit.Regs()[0]);
        if (value != EOF && value >= 0 && value <= UCHAR_MAX) {
            jit.Regs()[0] = static_cast<uint32_t>(std::tolower(value));
        }
    });
    RegisterThunk("isspace", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const int value = static_cast<int32_t>(jit.Regs()[0]);
        jit.Regs()[0] = value == EOF ? 0u :
            (value >= 0 && value <= UCHAR_MAX
                ? static_cast<uint32_t>(std::isspace(value)) : 0u);
    });
    RegisterThunk("ispunct", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const int value = static_cast<int32_t>(jit.Regs()[0]);
        jit.Regs()[0] = value == EOF ? 0u :
            (value >= 0 && value <= UCHAR_MAX
                ? static_cast<uint32_t>(std::ispunct(value)) : 0u);
    });
    RegisterThunk("strcat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dst = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        char* h_dst = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), dst));
        const char* h_src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), src));
        if (h_dst && h_src) strcat(h_dst, h_src);
        jit.Regs()[0] = dst;
    });
    RegisterThunk("strncpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dst = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        char* h_dst = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), dst));
        const char* h_src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), src));
        if (GuestLowGuardHit(dst, n)) { host.ReportLowGuardWrite(dst, n); h_dst = nullptr; }
        if (h_dst && h_src && n > 0) strncpy(h_dst, h_src, n);
        jit.Regs()[0] = dst;
    });
    RegisterThunk("strncat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t dst = jit.Regs()[0];
        const uint32_t src = jit.Regs()[1];
        const size_t limit = jit.Regs()[2];
        const char* h_dst = GuestCString(host.GetGuestMemoryBase(),
                                         host.GetGuestMemorySize(), dst);
        const char* h_src = GuestCString(host.GetGuestMemoryBase(),
                                         host.GetGuestMemorySize(), src);
        if (!h_dst || !h_src) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = dst;
            return;
        }
        const size_t dst_length = std::strlen(h_dst);
        const size_t copy_length = std::min(limit, std::strlen(h_src));
        if (copy_length > std::numeric_limits<size_t>::max() - dst_length - 1 ||
            !GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), dst,
                       dst_length + copy_length + 1)) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = dst;
            return;
        }
        std::memcpy(GuestToHost(host.GetGuestMemoryBase(), dst + dst_length),
                    h_src, copy_length);
        host.MemoryWrite8(dst + static_cast<uint32_t>(dst_length + copy_length), 0);
        jit.Regs()[0] = dst;
    });
    RegisterThunk("strcspn", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* text = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), jit.Regs()[0]);
        const char* reject = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[1]);
        if (!text || !reject) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = static_cast<uint32_t>(std::strcspn(text, reject));
    });
    RegisterThunk("strspn", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* text = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), jit.Regs()[0]);
        const char* accept = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[1]);
        if (!text || !accept) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = static_cast<uint32_t>(std::strspn(text, accept));
    });
    RegisterThunk("strpbrk", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t text_address = jit.Regs()[0];
        const char* text = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), text_address);
        const char* accept = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[1]);
        if (!text || !accept) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        const char* result = std::strpbrk(text, accept);
        jit.Regs()[0] = result ? text_address + static_cast<uint32_t>(result - text) : 0;
    });
    RegisterThunk("strtok", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t text_address = jit.Regs()[0];
        char* text = text_address
            ? const_cast<char*>(GuestCString(host.GetGuestMemoryBase(),
                                             host.GetGuestMemorySize(), text_address))
            : nullptr;
        const char* delimiters = GuestCString(host.GetGuestMemoryBase(),
                                              host.GetGuestMemorySize(), jit.Regs()[1]);
        if ((!text_address && !delimiters) || (text_address && !text) || !delimiters) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        char* result = std::strtok(text, delimiters);
        jit.Regs()[0] = result ? HostToGuest(host.GetGuestMemoryBase(), result) : 0;
    });
    RegisterThunk("fnmatch", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* pattern = GuestCString(host.GetGuestMemoryBase(),
                                           host.GetGuestMemorySize(), jit.Regs()[0]);
        const char* text = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), jit.Regs()[1]);
        if (!pattern || !text) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = FNM_NOMATCH;
            return;
        }
        jit.Regs()[0] = static_cast<uint32_t>(
            ::fnmatch(pattern, text, static_cast<int>(jit.Regs()[2])));
    });
    RegisterThunk("strncasecmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t s1 = jit.Regs()[0];
        uint32_t s2 = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        const char* h_s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), s1));
        const char* h_s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), s2));
        jit.Regs()[0] = (h_s1 && h_s2) ? static_cast<uint32_t>(strncasecmp(h_s1, h_s2, n)) : 0;
    });
    RegisterThunk("strtol", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t nptr = jit.Regs()[0];
        uint32_t endptr = jit.Regs()[1];
        int base = static_cast<int>(jit.Regs()[2]);
        const char* h_nptr = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), nptr));
        if (!h_nptr) { jit.Regs()[0] = 0; return; }
        char* h_end = nullptr;
        long val = strtol(h_nptr, &h_end, base);
        if (endptr && h_end) {
            uint32_t offset = static_cast<uint32_t>(h_end - h_nptr);
            host.MemoryWrite32(endptr, nptr + offset);
        }
        jit.Regs()[0] = static_cast<uint32_t>(val);
    });
    RegisterThunk("atol", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* text = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), jit.Regs()[0]);
        if (!text) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = static_cast<uint32_t>(
            static_cast<int32_t>(std::strtol(text, nullptr, 10)));
    });
    RegisterThunk("strtoull", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t text_address = jit.Regs()[0];
        const uint32_t end_address = jit.Regs()[1];
        const char* text = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), text_address);
        if (!text || (end_address && !GuestSpan(host.GetGuestMemoryBase(),
                host.GetGuestMemorySize(), end_address, sizeof(uint32_t)))) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            return;
        }
        char* end = nullptr;
        const unsigned long long value = std::strtoull(
            text, &end, static_cast<int>(jit.Regs()[2]));
        if (end_address) {
            host.MemoryWrite32(end_address,
                text_address + static_cast<uint32_t>(end - text));
        }
        jit.Regs()[0] = static_cast<uint32_t>(value);
        jit.Regs()[1] = static_cast<uint32_t>(value >> 32);
    });
    RegisterThunk("strtod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t text_address = jit.Regs()[0];
        const uint32_t end_address = jit.Regs()[1];
        const char* text = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), text_address);
        if (!text || (end_address && !GuestSpan(host.GetGuestMemoryBase(),
                host.GetGuestMemorySize(), end_address, sizeof(uint32_t)))) {
            SetGuestErrno(host, EFAULT);
            WriteDF(jit, 0, 0.0);
            return;
        }
        char* end = nullptr;
        const double value = std::strtod(text, &end);
        if (end_address) {
            host.MemoryWrite32(end_address,
                text_address + static_cast<uint32_t>(end - text));
        }
        WriteDF(jit, 0, value);
    });
    RegisterThunk("wcscmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const wchar_t* left = GuestWideCString(host, jit.Regs()[0]);
        const wchar_t* right = GuestWideCString(host, jit.Regs()[1]);
        if (!left || !right) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = static_cast<uint32_t>(std::wcscmp(left, right));
    });
    RegisterThunk("wcscoll", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const wchar_t* left = GuestWideCString(host, jit.Regs()[0]);
        const wchar_t* right = GuestWideCString(host, jit.Regs()[1]);
        if (!left || !right) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = static_cast<uint32_t>(std::wcscoll(left, right));
    });
    RegisterThunk("wcscat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t destination_address = jit.Regs()[0];
        const uint32_t source_address = jit.Regs()[1];
        const wchar_t* source = GuestWideCString(host, source_address);
        const wchar_t* destination = GuestWideCString(host, destination_address);
        if (!source || !destination) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = destination_address;
            return;
        }
        const size_t destination_length = std::wcslen(destination);
        const size_t source_length = std::wcslen(source);
        const size_t total_units = destination_length + source_length + 1;
        if (total_units > std::numeric_limits<size_t>::max() / sizeof(wchar_t) ||
            !GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       destination_address, total_units * sizeof(wchar_t))) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = destination_address;
            return;
        }
        std::wcscat(const_cast<wchar_t*>(destination), source);
        jit.Regs()[0] = destination_address;
    });
    RegisterThunk("wcstombs", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t destination_address = jit.Regs()[0];
        const wchar_t* source = GuestWideCString(host, jit.Regs()[1]);
        const size_t capacity = jit.Regs()[2];
        if (!source) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        char* destination = nullptr;
        if (destination_address || capacity) {
            destination = static_cast<char*>(GuestSpan(
                host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                destination_address, capacity));
            if (!destination) {
                SetGuestErrno(host, EFAULT);
                jit.Regs()[0] = static_cast<uint32_t>(-1);
                return;
            }
        }
        const size_t result = std::wcstombs(destination, source, capacity);
        if (result == static_cast<size_t>(-1)) SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("puts", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t s = jit.Regs()[0];
        const char* str = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), s));
        if (str) LOGI("[puts] %s", str);
        jit.Regs()[0] = 1;
    });
    RegisterThunk("getenv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), jit.Regs()[0]);
        if (!name) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        const char* value = std::getenv(name);
        if (!value) {
            jit.Regs()[0] = 0;
            return;
        }
        static std::mutex environment_mutex;
        static std::unordered_map<std::string, uint32_t> environment_values;
        std::lock_guard<std::mutex> lock(environment_mutex);
        auto [it, inserted] = environment_values.try_emplace(name, 0);
        if (inserted) {
            const size_t length = std::strlen(value) + 1;
            it->second = host.GuestAlloc(length, 1);
            void* output = it->second
                ? GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                            it->second, length) : nullptr;
            if (!output) {
                environment_values.erase(it);
                SetGuestErrno(host, ENOMEM);
                jit.Regs()[0] = 0;
                return;
            }
            std::memcpy(output, value, length);
        }
        jit.Regs()[0] = it->second;
    });
    RegisterThunk("vsnprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t buf = jit.Regs()[0];
        uint32_t size = jit.Regs()[1];
        uint32_t fmt = jit.Regs()[2];
        uint32_t va_list_guest = jit.Regs()[3];
        char* h_buf = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), buf));
        const char* h_fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), fmt));
        if (!h_fmt) {
            if (h_buf && size) h_buf[0] = 0;
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        const std::string formatted =
            FormatGuestVaList(host, h_fmt, va_list_guest);
        if (h_buf && size) {
            const size_t copied =
                std::min<size_t>(formatted.size(), static_cast<size_t>(size - 1));
            memcpy(h_buf, formatted.data(), copied);
            h_buf[copied] = 0;
        }
        jit.Regs()[0] = static_cast<uint32_t>(formatted.size());
    });
    RegisterThunk("vsprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t buffer_address = jit.Regs()[0];
        const char* format = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[1]);
        const uint32_t va_list_address = jit.Regs()[2];
        if (!format || !GuestSpan(host.GetGuestMemoryBase(),
                host.GetGuestMemorySize(), va_list_address, sizeof(uint32_t))) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        const std::string formatted =
            FormatGuestVaList(host, format, va_list_address);
        char* output = static_cast<char*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), buffer_address,
            formatted.size() + 1));
        if (!output) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        std::memcpy(output, formatted.c_str(), formatted.size() + 1);
        jit.Regs()[0] = static_cast<uint32_t>(formatted.size());
    });
    RegisterThunk("vprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* format = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[0]);
        const uint32_t va_list_address = jit.Regs()[1];
        if (!format || !GuestSpan(host.GetGuestMemoryBase(),
                host.GetGuestMemorySize(), va_list_address, sizeof(uint32_t))) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        const std::string formatted =
            FormatGuestVaList(host, format, va_list_address);
        LOGI("[vprintf] %s", formatted.c_str());
        jit.Regs()[0] = static_cast<uint32_t>(formatted.size());
    });

    // Time & Threading
    RegisterThunk("gettimeofday", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t tv_ptr = jit.Regs()[0];
        if (tv_ptr) {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            host.MemoryWrite32(tv_ptr + 0, static_cast<uint32_t>(tv.tv_sec));
            host.MemoryWrite32(tv_ptr + 4, static_cast<uint32_t>(tv.tv_usec));
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("clock_gettime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const clockid_t clock_id = static_cast<clockid_t>(jit.Regs()[0]);
        const uint32_t guest_timespec = jit.Regs()[1];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       guest_timespec, 8)) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        struct timespec value {};
        if (::clock_gettime(clock_id, &value) != 0) {
            SetGuestErrno(host, errno);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        // The ARM32 guest uses Bionic's 32-bit timespec (two 32-bit fields).
        host.MemoryWrite32(guest_timespec,
                           static_cast<uint32_t>(value.tv_sec));
        host.MemoryWrite32(guest_timespec + 4,
                           static_cast<uint32_t>(value.tv_nsec));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("clock", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const clock_t value = ::clock();
        if (value == static_cast<clock_t>(-1)) SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(value);
    });
    RegisterThunk("gettid", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(::gettid());
    });
    RegisterThunk("sleep", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const unsigned int remaining = ::sleep(jit.Regs()[0]);
        if (remaining) SetGuestErrno(host, errno);
        jit.Regs()[0] = remaining;
    });
    RegisterThunk("syscall", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const int number = static_cast<int32_t>(args.U32());
        uint32_t syscall_args[6]{};
        for (uint32_t& value : syscall_args) value = args.U32();
        switch (number) {
            case 224: // __NR_gettid in the ARM EABI
                jit.Regs()[0] = static_cast<uint32_t>(::gettid());
                return;
            case 78: // __NR_gettimeofday
                jit.Regs()[0] = syscall_args[0];
                jit.Regs()[1] = syscall_args[1];
                host.GetThunkManager().DispatchNamedThunk("gettimeofday", jit);
                return;
            case 263: // __NR_clock_gettime
                jit.Regs()[0] = syscall_args[0];
                jit.Regs()[1] = syscall_args[1];
                host.GetThunkManager().DispatchNamedThunk("clock_gettime", jit);
                return;
            case 3: case 4: case 5: case 6: case 19: {
                const char* thunk = number == 3 ? "read" : number == 4 ? "write" :
                    number == 5 ? "open" : number == 6 ? "close" : "lseek";
                jit.Regs()[0] = syscall_args[0];
                jit.Regs()[1] = syscall_args[1];
                jit.Regs()[2] = syscall_args[2];
                host.GetThunkManager().DispatchNamedThunk(thunk, jit);
                return;
            }
            case 240: { // __NR_futex; only WAIT/WAKE forms are safe to forward.
                const uint32_t guest_address = syscall_args[0];
                const int operation = static_cast<int32_t>(syscall_args[1]);
                const int command = operation & FUTEX_CMD_MASK;
                const bool wait = command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET;
                const bool wake = command == FUTEX_WAKE || command == FUTEX_WAKE_BITSET;
                auto* address = static_cast<uint32_t*>(GuestSpan(
                    host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                    guest_address, sizeof(uint32_t)));
                if ((!wait && !wake) || !address) {
                    SetGuestErrno(host, !address ? EFAULT : ENOSYS);
                    jit.Regs()[0] = static_cast<uint32_t>(-1);
                    return;
                }
                timespec timeout{};
                timespec* timeout_ptr = nullptr;
                if (wait && syscall_args[3]) {
                    if (!GuestSpan(host.GetGuestMemoryBase(),
                                   host.GetGuestMemorySize(), syscall_args[3], 8)) {
                        SetGuestErrno(host, EFAULT);
                        jit.Regs()[0] = static_cast<uint32_t>(-1);
                        return;
                    }
                    timeout.tv_sec = static_cast<int32_t>(host.MemoryRead32(syscall_args[3]));
                    timeout.tv_nsec = static_cast<int32_t>(host.MemoryRead32(syscall_args[3] + 4));
                    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 ||
                        timeout.tv_nsec >= 1000000000) {
                        SetGuestErrno(host, EINVAL);
                        jit.Regs()[0] = static_cast<uint32_t>(-1);
                        return;
                    }
                    timeout_ptr = &timeout;
                }
                void* address2 = nullptr;
                if (syscall_args[4]) {
                    address2 = GuestSpan(host.GetGuestMemoryBase(),
                                         host.GetGuestMemorySize(), syscall_args[4],
                                         sizeof(uint32_t));
                    if (!address2) {
                        SetGuestErrno(host, EFAULT);
                        jit.Regs()[0] = static_cast<uint32_t>(-1);
                        return;
                    }
                }
                const long result = ::syscall(
                    SYS_futex, address, operation,
                    static_cast<int>(syscall_args[2]), timeout_ptr, address2,
                    syscall_args[5]);
                if (result < 0) SetGuestErrno(host, errno);
                jit.Regs()[0] = static_cast<uint32_t>(result);
                return;
            }
            default:
                SetGuestErrno(host, ENOSYS);
                jit.Regs()[0] = static_cast<uint32_t>(-1);
                return;
        }
    });
    constexpr uint32_t kGuestJmpBufMagic = 0x4A4D5031u;
    constexpr size_t kGuestJmpBufWords = 30;
    RegisterThunk("setjmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        constexpr uint32_t kMagic = kGuestJmpBufMagic;
        constexpr size_t kWords = kGuestJmpBufWords;
        const uint32_t address = jit.Regs()[0];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       address, kWords * sizeof(uint32_t))) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        uint32_t words[kWords]{};
        words[0] = kMagic;
        words[1] = CurrentGuestThreadId();
        for (size_t i = 0; i < 8; ++i) words[2 + i] = jit.Regs()[4 + i];
        words[10] = jit.Regs()[13];
        words[11] = jit.Regs()[14];
        words[12] = jit.Cpsr();
        words[13] = jit.Fpscr();
        for (size_t i = 0; i < 16; ++i) words[14 + i] = jit.ExtRegs()[16 + i];
        for (size_t i = 0; i < kWords; ++i) {
            host.MemoryWrite32(address + static_cast<uint32_t>(i * 4), words[i]);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("longjmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        constexpr uint32_t kMagic = kGuestJmpBufMagic;
        constexpr size_t kWords = kGuestJmpBufWords;
        const uint32_t address = jit.Regs()[0];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       address, kWords * sizeof(uint32_t)) ||
            host.MemoryRead32(address) != kMagic ||
            host.MemoryRead32(address + 4) != CurrentGuestThreadId()) {
            SetGuestErrno(host, EINVAL);
            jit.HaltExecution();
            return;
        }
        uint32_t words[kWords]{};
        for (size_t i = 0; i < kWords; ++i) {
            words[i] = host.MemoryRead32(address + static_cast<uint32_t>(i * 4));
        }
        const uint32_t return_value = jit.Regs()[1] ? jit.Regs()[1] : 1u;
        for (size_t i = 0; i < 8; ++i) jit.Regs()[4 + i] = words[2 + i];
        jit.Regs()[13] = words[10];
        jit.Regs()[14] = words[11];
        jit.SetCpsr((words[12] & ~0x20u) | ((words[11] & 1u) ? 0x20u : 0u));
        jit.SetFpscr(words[13]);
        for (size_t i = 0; i < 16; ++i) jit.ExtRegs()[16 + i] = words[14 + i];
        jit.ClearExclusiveState();
        jit.Regs()[0] = return_value;
        jit.Regs()[15] = words[11] & ~1u;
    });
    RegisterThunk("tcgetattr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        SetGuestErrno(host, ENOTTY);
        jit.Regs()[0] = static_cast<uint32_t>(-1);
    });
    RegisterThunk("tcsetattr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        SetGuestErrno(host, ENOTTY);
        jit.Regs()[0] = static_cast<uint32_t>(-1);
    });
    RegisterThunk("usleep", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        useconds_t remaining = static_cast<useconds_t>(jit.Regs()[0]);
        // A frame-limit sleep is idle time: translate learned guest code of
        // this thread's role for all but its last millisecond, then sleep on.
        if (remaining >= 2000) {
            timespec start{}, end{};
            clock_gettime(CLOCK_MONOTONIC, &start);
            host.WarmLearnedBlocksInCallback(jit, (uint64_t{remaining} - 1000) * 1000);
            clock_gettime(CLOCK_MONOTONIC, &end);
            const int64_t spent = (int64_t{end.tv_sec} - start.tv_sec) * 1000000 +
                                  (end.tv_nsec - start.tv_nsec) / 1000;
            remaining = spent >= remaining ? 0 : remaining - static_cast<useconds_t>(spent);
        }
        const int result = remaining ? ::usleep(remaining) : 0;
        if (result != 0) SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("rand", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(::rand());
    });
    RegisterThunk("srand", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        ::srand(jit.Regs()[0]);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("lrand48", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = static_cast<uint32_t>(lrand48());
    });
    RegisterThunk("srand48", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        srand48(static_cast<long>(jit.Regs()[0]));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("utime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("unlink", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("chown", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });
    RegisterThunk("fcntl", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const int command = static_cast<int32_t>(jit.Regs()[1]);
        switch (command) {
            case F_DUPFD: case F_DUPFD_CLOEXEC: case F_GETFD: case F_SETFD:
            case F_GETFL: case F_SETFL:
                NetworkResult(jit, host, ::fcntl(static_cast<int32_t>(jit.Regs()[0]),
                    command, static_cast<int32_t>(jit.Regs()[2])));
                break;
            default: // ARM32 record-lock/pointer layouts need their own translation.
                NetworkFailure(jit, host, ENOSYS);
        }
    });

    // Semaphores & Pthread
    RegisterThunk("sem_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        if (!address || jit.Regs()[1] != 0 || jit.Regs()[2] > static_cast<uint32_t>(INT_MAX)) {
            SetGuestErrno(host, !address ? EINVAL : (jit.Regs()[1] ? ENOSYS : EINVAL));
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        auto state = std::make_shared<GuestSemaphoreState>();
        state->value = jit.Regs()[2];
        {
            std::lock_guard<std::mutex> lock(g_sync_state_mutex);
            if (g_guest_semaphores.count(address)) {
                SetGuestErrno(host, EBUSY);
                jit.Regs()[0] = static_cast<uint32_t>(-1);
                return;
            }
            g_guest_semaphores.emplace(address, state);
        }
        host.MemoryWrite32(address, state->value);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sem_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto state = GetGuestSemaphore(jit.Regs()[0]);
        if (!state) { SetGuestErrno(host, EINVAL); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->waiters) { SetGuestErrno(host, EBUSY); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
            state->destroyed = true;
        }
        std::lock_guard<std::mutex> map_lock(g_sync_state_mutex);
        g_guest_semaphores.erase(jit.Regs()[0]);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sem_wait", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        auto state = GetGuestSemaphore(address);
        if (!state) { SetGuestErrno(host, EINVAL); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        std::unique_lock<std::mutex> lock(state->mutex);
        ++state->waiters;
        state->available.wait(lock, [&] { return state->destroyed || state->value != 0; });
        --state->waiters;
        if (state->destroyed) { SetGuestErrno(host, EINVAL); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        --state->value;
        host.MemoryWrite32(address, state->value);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sem_trywait", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        auto state = GetGuestSemaphore(address);
        if (!state) { SetGuestErrno(host, EINVAL); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->destroyed || !state->value) {
            SetGuestErrno(host, state->destroyed ? EINVAL : EAGAIN);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        --state->value;
        host.MemoryWrite32(address, state->value);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sem_post", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        auto state = GetGuestSemaphore(address);
        if (!state) { SetGuestErrno(host, EINVAL); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->destroyed || state->value == static_cast<uint32_t>(INT_MAX)) {
                SetGuestErrno(host, state->destroyed ? EINVAL : EOVERFLOW);
                jit.Regs()[0] = static_cast<uint32_t>(-1);
                return;
            }
            ++state->value;
            host.MemoryWrite32(address, state->value);
        }
        state->available.notify_one();
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_self", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = CurrentGuestThreadId();
    });
    RegisterThunk("pthread_setname_np", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t thread = jit.Regs()[0];
        const char* name = GuestCString(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize(), jit.Regs()[1]);
        if (!name) { jit.Regs()[0] = EFAULT; return; }
        if (thread != CurrentGuestThreadId()) { jit.Regs()[0] = ESRCH; return; }
        jit.Regs()[0] = static_cast<uint32_t>(
            ::pthread_setname_np(::pthread_self(), name));
    });
    RegisterThunk("pthread_getschedparam", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t thread = jit.Regs()[0];
        const uint32_t policy_address = jit.Regs()[1];
        const uint32_t parameter_address = jit.Regs()[2];
        if (thread != CurrentGuestThreadId()) { jit.Regs()[0] = ESRCH; return; }
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       policy_address, sizeof(int32_t)) ||
            !GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       parameter_address, sizeof(int32_t))) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        int policy = 0;
        sched_param parameter{};
        const int result = ::pthread_getschedparam(
            ::pthread_self(), &policy, &parameter);
        if (result == 0) {
            host.MemoryWrite32(policy_address, static_cast<uint32_t>(policy));
            host.MemoryWrite32(parameter_address,
                               static_cast<uint32_t>(parameter.sched_priority));
        }
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("pthread_setschedparam", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t thread = jit.Regs()[0];
        const int policy = static_cast<int32_t>(jit.Regs()[1]);
        const uint32_t parameter_address = jit.Regs()[2];
        if (thread != CurrentGuestThreadId()) { jit.Regs()[0] = ESRCH; return; }
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       parameter_address, sizeof(int32_t))) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        sched_param parameter{};
        parameter.sched_priority = static_cast<int32_t>(
            host.MemoryRead32(parameter_address));
        jit.Regs()[0] = static_cast<uint32_t>(
            ::pthread_setschedparam(::pthread_self(), policy, &parameter));
    });
    RegisterThunk("pthread_attr_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t attr = jit.Regs()[0];
        if (!attr || static_cast<uint64_t>(attr) + sizeof(Arm32PthreadAttr) >
                         host.GetGuestMemorySize()) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        g_pthread_attrs[attr] = {};
        memset(GuestToHost(host.GetGuestMemoryBase(), attr), 0,
               sizeof(Arm32PthreadAttr));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        jit.Regs()[0] = g_pthread_attrs.erase(jit.Regs()[0]) ? 0 : EINVAL;
    });
    RegisterThunk("pthread_attr_setdetachstate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (jit.Regs()[1] != PTHREAD_CREATE_JOINABLE && jit.Regs()[1] != PTHREAD_CREATE_DETACHED) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        auto it = g_pthread_attrs.find(jit.Regs()[0]);
        if (it == g_pthread_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
        it->second.detached = jit.Regs()[1] == PTHREAD_CREATE_DETACHED;
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_setstacksize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t requested = jit.Regs()[1];
        if (requested < kArm32PthreadStackMin) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        auto it = g_pthread_attrs.find(jit.Regs()[0]);
        if (it == g_pthread_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
        it->second.stack_size = requested;
        host.MemoryWrite32(jit.Regs()[0] + offsetof(Arm32PthreadAttr, stack_size),
                           requested);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_setschedpolicy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        const int policy = static_cast<int32_t>(jit.Regs()[1]);
        if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        auto it = g_pthread_attrs.find(address);
        if (it == g_pthread_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
        it->second.sched_policy = policy;
        host.MemoryWrite32(address + offsetof(Arm32PthreadAttr, sched_policy),
                           static_cast<uint32_t>(policy));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_setschedparam", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        const uint32_t parameter_address = jit.Regs()[1];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       parameter_address, sizeof(int32_t))) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        auto it = g_pthread_attrs.find(address);
        if (it == g_pthread_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
        const int priority = static_cast<int32_t>(host.MemoryRead32(parameter_address));
        const int minimum = sched_get_priority_min(it->second.sched_policy);
        const int maximum = sched_get_priority_max(it->second.sched_policy);
        if (minimum < 0 || maximum < 0 || priority < minimum || priority > maximum) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        it->second.sched_priority = priority;
        host.MemoryWrite32(address + offsetof(Arm32PthreadAttr, sched_priority),
                           static_cast<uint32_t>(priority));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_attr_getschedparam", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        const uint32_t parameter_address = jit.Regs()[1];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       parameter_address, sizeof(int32_t))) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        auto it = g_pthread_attrs.find(address);
        if (it == g_pthread_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
        host.MemoryWrite32(parameter_address,
                           static_cast<uint32_t>(it->second.sched_priority));
        jit.Regs()[0] = 0;
    });
    RegisterThunk("sched_get_priority_min", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(
            ::sched_get_priority_min(static_cast<int32_t>(jit.Regs()[0])));
    });
    RegisterThunk("sched_get_priority_max", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(
            ::sched_get_priority_max(static_cast<int32_t>(jit.Regs()[0])));
    });
    RegisterThunk("pthread_condattr_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[0];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       address, sizeof(uint32_t))) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        g_pthread_cond_attrs[address] = false;
        host.MemoryWrite32(address, 0);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_condattr_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        jit.Regs()[0] = g_pthread_cond_attrs.erase(jit.Regs()[0]) ? 0u : EINVAL;
    });
    RegisterThunk("pthread_condattr_setclock", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const uint32_t address = jit.Regs()[0];
        const clockid_t clock_id = static_cast<clockid_t>(jit.Regs()[1]);
        if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        auto it = g_pthread_cond_attrs.find(address);
        if (it == g_pthread_cond_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
        it->second = clock_id == CLOCK_MONOTONIC;
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutex_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t mtx = jit.Regs()[0];
        if (!mtx) { jit.Regs()[0] = EINVAL; return; }
        int type = PTHREAD_MUTEX_DEFAULT;
        if (jit.Regs()[1]) {
            std::lock_guard<std::mutex> attr_lock(g_pthread_attr_mutex);
            auto attr = g_pthread_mutex_attrs.find(jit.Regs()[1]);
            if (attr == g_pthread_mutex_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
            type = attr->second;
        }
        auto state = std::make_shared<GuestMutexState>();
        state->type = type;
        {
            std::lock_guard<std::mutex> lock(g_sync_state_mutex);
            if (g_guest_mutexes.count(mtx)) { jit.Regs()[0] = EBUSY; return; }
            g_guest_mutexes.emplace(mtx, state);
        }
        host.MemoryWrite32(mtx, (static_cast<uint32_t>(type) & 3u) << 14);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutex_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto state = GetGuestMutex(host, jit.Regs()[0], true);
        if (!state) { jit.Regs()[0] = EINVAL; return; }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->owner || state->waiters) { jit.Regs()[0] = EBUSY; return; }
            state->destroyed = true;
        }
        std::lock_guard<std::mutex> map_lock(g_sync_state_mutex);
        g_guest_mutexes.erase(jit.Regs()[0]);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutexattr_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!jit.Regs()[0]) { jit.Regs()[0] = EINVAL; return; }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        g_pthread_mutex_attrs[jit.Regs()[0]] = PTHREAD_MUTEX_DEFAULT;
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_mutexattr_settype", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const int type = static_cast<int>(jit.Regs()[1]);
        if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_DEFAULT &&
            type != PTHREAD_MUTEX_RECURSIVE && type != PTHREAD_MUTEX_ERRORCHECK) {
            jit.Regs()[0] = EINVAL;
            return;
        }
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        auto it = g_pthread_mutex_attrs.find(jit.Regs()[0]);
        if (it == g_pthread_mutex_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
        it->second = type;
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_create", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t thread_out = jit.Regs()[0];
        uint32_t attr = jit.Regs()[1];
        uint32_t start_func = jit.Regs()[2];
        uint32_t arg = jit.Regs()[3];
        if (!thread_out || !start_func) {
            LOGE("pthread_create: invalid thread/result pointer or start routine");
            jit.Regs()[0] = EINVAL;
            return;
        }
        bool detached = false;
        uint32_t stack_size = 0;
        if (attr) {
            std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
            auto it = g_pthread_attrs.find(attr);
            if (it == g_pthread_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
            detached = it->second.detached;
            stack_size = it->second.stack_size;
        }
        auto record = std::make_shared<GuestThreadRecord>();
        record->id = g_next_guest_thread_id.fetch_add(1, std::memory_order_relaxed);
        if (!record->id) record->id = g_next_guest_thread_id.fetch_add(1, std::memory_order_relaxed);
        record->start = start_func;
        record->arg = arg;
        record->detached = detached;
        record->stack_size = stack_size;
        LOGI("pthread_create id=0x%08X start=0x%08X arg=0x%08X attr=0x%08X detached=%d stack=%u",
             record->id, record->start, record->arg, attr,
             record->detached ? 1 : 0, record->stack_size);
        const uint32_t android_thread_proc =
            host.FindSymbol("_ZN22FRunnableThreadAndroid11_ThreadProcEPv");
        const uint64_t arg_end = static_cast<uint64_t>(arg) + 0x10u;
        if (android_thread_proc && start_func == android_thread_proc &&
            arg_end <= host.GetGuestMemorySize()) {
            const uint32_t runnable = host.MemoryRead32(arg + 0x0Cu);
            const uint64_t runnable_end = static_cast<uint64_t>(runnable) + 4u;
            if (runnable && runnable_end <= host.GetGuestMemorySize()) {
                const uint32_t vtable = host.MemoryRead32(runnable);
                const uint64_t vtable_end = static_cast<uint64_t>(vtable) + 0x18u;
                if (vtable && vtable_end <= host.GetGuestMemorySize()) {
                    LOGI("FRunnableThreadAndroid id=0x%08X object=0x%08X "
                         "runnable=0x%08X vtable=0x%08X methods="
                         "[0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X]",
                         record->id, arg, runnable, vtable,
                         host.MemoryRead32(vtable + 0x00u),
                         host.MemoryRead32(vtable + 0x04u),
                         host.MemoryRead32(vtable + 0x08u),
                         host.MemoryRead32(vtable + 0x0Cu),
                         host.MemoryRead32(vtable + 0x10u),
                         host.MemoryRead32(vtable + 0x14u));
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_guest_threads_mutex);
            g_guest_threads.emplace(record->id, record);
        }
        DynarmicHost* host_ptr = &host;
        try {
            std::thread([record, host_ptr]() {
                g_current_guest_thread = record->id;
                g_thread_exit_requested = false;
                g_thread_exit_value = 0;
                bool attached = false;
                try {
                    attached = host_ptr->AttachCurrentThreadContext(record->stack_size);
                } catch (const std::exception& error) {
                    LOGE("pthread_create: worker A32 context attach threw: %s", error.what());
                } catch (...) {
                    LOGE("pthread_create: worker A32 context attach threw an unknown exception");
                }
                if (!attached) {
                    // GetOrCreate normally publishes nothing on failure. This
                    // also releases an idle stale thread-id binding if a grow
                    // failed after std::thread reused an OS thread identifier.
                    try {
                        host_ptr->ReleaseCurrentThreadContext();
                    } catch (...) {
                        LOGE("pthread_create: failed to roll back worker A32 context attach");
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(record->mutex);
                    record->worker_started = true;
                    record->start_error = attached ? 0 : EAGAIN;
                    if (!attached) record->done = true;
                }
                record->completed.notify_all();
                if (!attached) return;

                uint32_t result = 0xFFFFFFFFu; // PTHREAD_CANCELED on an unexpected host failure
                static std::atomic<unsigned> host_failure_reports{0};
                try {
                    result = host_ptr->RunFunction(record->start, {record->arg});
                    if (g_thread_exit_requested) result = g_thread_exit_value;
                    const uint32_t render_entry =
                        host_ptr->FindSymbol("_Z21NativeCallback_RenderPv");
                    static std::atomic<unsigned> render_return_reports{0};
                    if (render_entry && record->start == render_entry &&
                        render_return_reports.fetch_add(1, std::memory_order_relaxed) < 8) {
                        const auto read_global = [&](const char* name) {
                            const uint32_t address = host_ptr->FindSymbol(name);
                            return address && static_cast<uint64_t>(address) + 4 <= host_ptr->GetGuestMemorySize()
                                ? host_ptr->MemoryRead32(address) : 0xFFFFFFFFu;
                        };
                        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                             "render return id=%08X result=%08X pthread_exit=%d phase=%u started=%u guarded=%u critical=%u "
                             "request_exit=%u main_exit=%u force_stop=%u paused=%u stopped=%u healthy=%u engine=0x%08X world=0x%08X",
                             record->id, result, g_thread_exit_requested ? 1 : 0,
                             read_global("GPrimaryUE3StartupPhase"),
                             read_global("GIsStarted"),
                             read_global("GIsGuarded"),
                             read_global("GIsCriticalError"),
                             read_global("GIsRequestingExit"),
                             read_global("GMainThreadExit"),
                             read_global("GForceStopRendering"),
                             read_global("GPausedMode"), read_global("GHasStoppedRendering"),
                             read_global("GIsRenderingThreadHealthy"),
                             read_global("GEngine"), read_global("GWorld"));
                    }
                    RunGuestTlsDestructors(*host_ptr);
                } catch (const std::exception& error) {
                    if (host_failure_reports.fetch_add(1, std::memory_order_relaxed) < 8)
                        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "guest pthread host failure id=%08X start=%08X: %s", record->id, record->start, error.what());
                    LOGE("guest pthread 0x%08X failed in host: %s", record->id, error.what());
                } catch (...) {
                    if (host_failure_reports.fetch_add(1, std::memory_order_relaxed) < 8)
                        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "guest pthread unknown host failure id=%08X start=%08X", record->id, record->start);
                    LOGE("guest pthread 0x%08X failed with an unknown host exception", record->id);
                }
                LOGI("guest pthread complete id=0x%08X start=0x%08X result=0x%08X pthread_exit=%d",
                     record->id, record->start, result,
                     g_thread_exit_requested ? 1 : 0);
                host_ptr->ReleaseCurrentThreadContext();
                bool erase = false;
                {
                    std::lock_guard<std::mutex> lock(record->mutex);
                    record->result = result;
                    record->done = true;
                    erase = record->detached;
                }
                record->completed.notify_all();
                if (erase) {
                    std::lock_guard<std::mutex> map_lock(g_guest_threads_mutex);
                    auto it = g_guest_threads.find(record->id);
                    if (it != g_guest_threads.end() && it->second == record) g_guest_threads.erase(it);
                }
            }).detach();
        } catch (const std::system_error& error) {
            LOGE("pthread_create: host thread failed: %s", error.what());
            std::lock_guard<std::mutex> lock(g_guest_threads_mutex);
            g_guest_threads.erase(record->id);
            jit.Regs()[0] = EAGAIN;
            return;
        }
        {
            std::unique_lock<std::mutex> lock(record->mutex);
            record->completed.wait(lock, [&] { return record->worker_started; });
            if (record->start_error != 0) {
                const int start_error = record->start_error;
                lock.unlock();
                std::lock_guard<std::mutex> map_lock(g_guest_threads_mutex);
                auto it = g_guest_threads.find(record->id);
                if (it != g_guest_threads.end() && it->second == record) {
                    g_guest_threads.erase(it);
                }
                LOGE("pthread_create: worker A32 context attach failed");
                jit.Regs()[0] = static_cast<uint32_t>(start_error);
                return;
            }
        }
        host.MemoryWrite32(thread_out, record->id);
        jit.Regs()[0] = 0;
    });

    // POSIX sockets use real host fds; sockaddr_in/in6 and socklen_t are ABI-compatible.
    RegisterThunk("socket", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        NetworkResult(jit, host, ::socket(static_cast<int32_t>(jit.Regs()[0]),
            static_cast<int32_t>(jit.Regs()[1]), static_cast<int32_t>(jit.Regs()[2])));
    });
    RegisterThunk("bind", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkBindConnect(jit, host, false); });
    RegisterThunk("connect", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkBindConnect(jit, host, true); });
    RegisterThunk("listen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        NetworkResult(jit, host, ::listen(static_cast<int32_t>(jit.Regs()[0]),
                                          static_cast<int32_t>(jit.Regs()[1])));
    });
    RegisterThunk("accept", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t address = jit.Regs()[1], length_address = jit.Regs()[2];
        if (address && !GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                 length_address, sizeof(uint32_t))) {
            NetworkFailure(jit, host, EFAULT); return;
        }
        socklen_t length = address ? host.MemoryRead32(length_address) : 0;
        auto* native = address ? static_cast<sockaddr*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, length)) : nullptr;
        if (address && !native) { NetworkFailure(jit, host, EFAULT); return; }
        const int result = ::accept(static_cast<int32_t>(jit.Regs()[0]), native,
                                     address ? &length : nullptr);
        const int error = errno;
        if (result >= 0 && address) host.MemoryWrite32(length_address, length);
        if (result < 0) SetGuestErrno(host, error);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("send", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkTransfer(jit, host, false, false); });
    RegisterThunk("recv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkTransfer(jit, host, true, false); });
    RegisterThunk("sendto", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkTransfer(jit, host, false, true); });
    RegisterThunk("recvfrom", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkTransfer(jit, host, true, true); });
    RegisterThunk("select", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const int nfds = args.I32();
        const uint32_t addresses[] = {args.Ptr(), args.Ptr(), args.Ptr()};
        const uint32_t timeout_address = args.Ptr();
        if (nfds < 0 || nfds > FD_SETSIZE) { NetworkFailure(jit, host, EINVAL); return; }
        fd_set native[3]{};
        void* guest[3]{};
        for (size_t i = 0; i < 3; ++i) {
            if (!addresses[i]) continue;
            guest[i] = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                 addresses[i], sizeof(Arm32FdSet));
            if (!guest[i]) { NetworkFailure(jit, host, EFAULT); return; }
            Arm32FdSet value{};
            std::memcpy(&value, guest[i], sizeof(value));
            UnpackArm32FdSet(value, native[i].fds_bits);
        }
        timeval timeout{};
        void* guest_timeout = timeout_address ? GuestSpan(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), timeout_address, sizeof(Arm32Timeval)) : nullptr;
        if (timeout_address) {
            if (!guest_timeout) { NetworkFailure(jit, host, EFAULT); return; }
            Arm32Timeval value{};
            std::memcpy(&value, guest_timeout, sizeof(value));
            if (!UnpackArm32Timeval(value, timeout)) { NetworkFailure(jit, host, EINVAL); return; }
        }
        const int result = ::select(nfds, guest[0] ? &native[0] : nullptr,
            guest[1] ? &native[1] : nullptr, guest[2] ? &native[2] : nullptr,
            guest_timeout ? &timeout : nullptr);
        const int error = errno;
        if (result >= 0) {
            for (size_t i = 0; i < 3; ++i) if (guest[i]) {
                Arm32FdSet value{};
                PackArm32FdSet(native[i].fds_bits, value);
                std::memcpy(guest[i], &value, sizeof(value));
            }
        }
        if (guest_timeout) {
            Arm32Timeval value{};
            if (PackArm32Timeval(timeout, value)) std::memcpy(guest_timeout, &value, sizeof(value));
        }
        if (result < 0) SetGuestErrno(host, error);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("setsockopt", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkSocketOption(jit, host, false); });
    RegisterThunk("getsockopt", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { NetworkSocketOption(jit, host, true); });
    RegisterThunk("inet_addr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* text = GuestCString(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[0]);
        if (!text) { NetworkFailure(jit, host, EFAULT); return; }
        jit.Regs()[0] = ::inet_addr(text);
    });
    RegisterThunk("inet_ntoa", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        in_addr address{};
        address.s_addr = jit.Regs()[0]; // in_addr is a four-byte by-value AAPCS argument.
        const char* text = ::inet_ntoa(address);
        if (!text) { jit.Regs()[0] = 0; return; }
        static thread_local uint32_t guest_buffer = 0;
        if (!guest_buffer) guest_buffer = host.GuestAlloc(16, 1);
        char* output = guest_buffer ? static_cast<char*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), guest_buffer, 16)) : nullptr;
        if (!output) {
            SetGuestErrno(host, ENOMEM);
            jit.Regs()[0] = 0;
            return;
        }
        std::snprintf(output, 16, "%s", text);
        jit.Regs()[0] = guest_buffer;
    });
    RegisterThunk("shutdown", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const int result = ::shutdown(static_cast<int32_t>(jit.Regs()[0]),
                                      static_cast<int>(jit.Regs()[1]));
        if (result != 0) SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("gethostbyname", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = GuestCString(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[0]);
        if (!name) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        const hostent* native = ::gethostbyname(name);
        if (!native) { SetGuestErrno(host, h_errno == NETDB_INTERNAL ? errno : EHOSTUNREACH); jit.Regs()[0] = 0; return; }
        size_t aliases = 0, addresses = 0;
        size_t strings = std::strlen(native->h_name) + 1;
        while (native->h_aliases && native->h_aliases[aliases])
            strings += std::strlen(native->h_aliases[aliases++]) + 1;
        while (native->h_addr_list && native->h_addr_list[addresses]) ++addresses;
        const size_t bytes = sizeof(Arm32HostEnt) + (aliases + addresses + 2) * 4 +
                             addresses * native->h_length + strings;
        // Library-owned storage: one reusable block per host/guest thread, like inet_ntoa.
        static thread_local uint32_t guest_buffer = 0;
        static thread_local size_t capacity = 0;
        if (bytes > capacity) {
            const uint32_t replacement = host.GuestAlloc(bytes, 4);
            if (!replacement) { SetGuestErrno(host, ENOMEM); jit.Regs()[0] = 0; return; }
            if (guest_buffer) host.GuestFree(guest_buffer);
            guest_buffer = replacement; capacity = bytes;
        }
        auto* output = static_cast<uint8_t*>(GuestSpan(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), guest_buffer, bytes));
        if (!output) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        const uint32_t alias_list = guest_buffer + sizeof(Arm32HostEnt);
        const uint32_t address_list = alias_list + static_cast<uint32_t>((aliases + 1) * 4);
        uint32_t cursor = address_list + static_cast<uint32_t>((addresses + 1) * 4);
        for (size_t i = 0; i < addresses; ++i) {
            host.MemoryWrite32(address_list + static_cast<uint32_t>(i * 4), cursor);
            std::memcpy(output + cursor - guest_buffer, native->h_addr_list[i], native->h_length);
            cursor += native->h_length;
        }
        host.MemoryWrite32(address_list + static_cast<uint32_t>(addresses * 4), 0);
        Arm32HostEnt value{cursor, alias_list, native->h_addrtype, native->h_length, address_list};
        const size_t name_bytes = std::strlen(native->h_name) + 1;
        std::memcpy(output + cursor - guest_buffer, native->h_name, name_bytes);
        cursor += static_cast<uint32_t>(name_bytes);
        for (size_t i = 0; i < aliases; ++i) {
            host.MemoryWrite32(alias_list + static_cast<uint32_t>(i * 4), cursor);
            const size_t alias_bytes = std::strlen(native->h_aliases[i]) + 1;
            std::memcpy(output + cursor - guest_buffer, native->h_aliases[i], alias_bytes);
            cursor += static_cast<uint32_t>(alias_bytes);
        }
        host.MemoryWrite32(alias_list + static_cast<uint32_t>(aliases * 4), 0);
        std::memcpy(output, &value, sizeof(value));
        jit.Regs()[0] = guest_buffer;
    });
    RegisterThunk("gethostname", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* name = static_cast<char*>(GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                                 jit.Regs()[0], jit.Regs()[1]));
        int result = name ? ::gethostname(name, jit.Regs()[1]) : -1;
        if (result < 0) SetGuestErrno(host, name ? errno : EFAULT);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("getsockname", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t length_addr = jit.Regs()[2];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), length_addr, 4)) {
            NetworkFailure(jit, host, EFAULT); return;
        }
        socklen_t length = host.MemoryRead32(length_addr);
        auto* address = static_cast<sockaddr*>(GuestSpan(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), jit.Regs()[1], length));
        if (!address) { NetworkFailure(jit, host, EFAULT); return; }
        int result = ::getsockname(static_cast<int32_t>(jit.Regs()[0]), address, &length);
        const int error = errno;
        if (result == 0) host.MemoryWrite32(length_addr, length);
        if (result < 0) SetGuestErrno(host, error);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("ioctl", NetworkIoctl);
    RegisterThunk("getaddrinfo", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t output_address = jit.Regs()[3];
        const char* node = jit.Regs()[0] ? GuestCString(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), jit.Regs()[0]) : nullptr;
        const char* service = jit.Regs()[1] ? GuestCString(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), jit.Regs()[1]) : nullptr;
        auto* guest_hints = jit.Regs()[2] ? GuestSpan(host.GetGuestMemoryBase(),
            host.GetGuestMemorySize(), jit.Regs()[2], sizeof(Arm32AddrInfo)) : nullptr;
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), output_address, 4) ||
            (jit.Regs()[0] && !node) || (jit.Regs()[1] && !service) || (jit.Regs()[2] && !guest_hints)) {
            SetGuestErrno(host, EFAULT); jit.Regs()[0] = static_cast<uint32_t>(EAI_SYSTEM); return;
        }
        host.MemoryWrite32(output_address, 0);
        addrinfo hints{};
        if (guest_hints) {
            Arm32AddrInfo value{};
            std::memcpy(&value, guest_hints, sizeof(value));
            hints.ai_flags = value.flags; hints.ai_family = value.family;
            hints.ai_socktype = value.socktype; hints.ai_protocol = value.protocol;
        }
        addrinfo* list = nullptr;
        int result = ::getaddrinfo(node, service, guest_hints ? &hints : nullptr, &list);
        if (result != 0) {
            if (result == EAI_SYSTEM) SetGuestErrno(host, errno);
            jit.Regs()[0] = static_cast<uint32_t>(result); return;
        }
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> native_owner(list, ::freeaddrinfo);
        GuestAddrInfoAllocation allocation;
        uint32_t head = 0, previous = 0;
        try {
            for (const addrinfo* native = list; native; native = native->ai_next) {
                const size_t name_bytes = native->ai_canonname ? std::strlen(native->ai_canonname) + 1 : 0;
                const size_t bytes = sizeof(Arm32AddrInfo) + native->ai_addrlen + name_bytes;
                const uint32_t address = host.GuestAlloc(bytes, 4);
                if (!address) { result = EAI_MEMORY; break; }
                try { allocation.blocks.push_back(address); }
                catch (...) { host.GuestFree(address); throw; }
                Arm32AddrInfo value{native->ai_flags, native->ai_family, native->ai_socktype,
                    native->ai_protocol, native->ai_addrlen, 0, 0, 0};
                if (native->ai_addr && native->ai_addrlen) {
                    value.addr = address + sizeof(value);
                    std::memcpy(host.GetGuestMemoryBase() + value.addr, native->ai_addr, native->ai_addrlen);
                }
                if (name_bytes) {
                    value.canonname = address + sizeof(value) + native->ai_addrlen;
                    std::memcpy(host.GetGuestMemoryBase() + value.canonname, native->ai_canonname, name_bytes);
                }
                std::memcpy(host.GetGuestMemoryBase() + address, &value, sizeof(value));
                if (previous) host.MemoryWrite32(previous + offsetof(Arm32AddrInfo, next), address);
                else head = address;
                previous = address;
            }
            if (result == 0 && head) {
                std::lock_guard<std::mutex> lock(g_addrinfo_mutex);
                auto inserted = g_addrinfo_allocations.try_emplace(head);
                inserted.first->second = std::move(allocation);
            }
        } catch (const std::bad_alloc&) { result = EAI_MEMORY; }
        if (result != 0) for (uint32_t block : allocation.blocks) host.GuestFree(block);
        else host.MemoryWrite32(output_address, head);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("freeaddrinfo", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!jit.Regs()[0]) return;
        GuestAddrInfoAllocation allocation;
        {
            std::lock_guard<std::mutex> lock(g_addrinfo_mutex);
            auto found = g_addrinfo_allocations.find(jit.Regs()[0]);
            if (found == g_addrinfo_allocations.end()) {
                SetGuestErrno(host, EINVAL); return;
            }
            allocation = std::move(found->second);
            g_addrinfo_allocations.erase(found);
        }
        for (uint32_t block : allocation.blocks) host.GuestFree(block);
    });

    // Android & Runtime
    RegisterThunk("__android_log_write", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
#if HORN_ENABLE_LOGGING
        int prio = static_cast<int>(jit.Regs()[0]);
        uint32_t tag_ptr = jit.Regs()[1];
        uint32_t msg_ptr = jit.Regs()[2];
        const char* tag = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), tag_ptr));
        const char* msg = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), msg_ptr));
        if (tag && msg) __android_log_write(prio, tag, msg);
#else
        (void)host;
#endif
        jit.Regs()[0] = 1;
    });
    RegisterThunk("__aeabi_atexit", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_pure_virtual", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        LOGE("__cxa_pure_virtual called!");
        jit.Regs()[0] = 0;
    });
    RegisterThunk("__cxa_type_match", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_begin_cleanup", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_call_unexpected", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__gnu_Unwind_Find_exidx", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("dlopen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t name_ptr = jit.Regs()[0];
        const char* name = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), name_ptr));
        LOGI("dlopen('%s', %d)", name ? name : "NULL", jit.Regs()[1]);
        jit.Regs()[0] = 0x50005000; // Non-null handle
    });
    RegisterThunk("dlclose", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("dlsym", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sym_ptr = jit.Regs()[1];
        const char* sym = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), sym_ptr));
        LOGI("dlsym(handle, '%s')", sym ? sym : "NULL");
        if (sym) {
            if (strncmp(sym, "SL_IID_", 7) == 0) {
                uint32_t iid_ptr = GetOpenSLESInterfacePointer(sym);
                if (iid_ptr != 0) {
                    jit.Regs()[0] = iid_ptr;
                    return;
                }
            }
            if (strcmp(sym, "slCreateEngine") == 0) {
                jit.Regs()[0] = host.GetThunkManager().GetOrCreateThunkAddress("slCreateEngine");
                return;
            }
            auto it = host.GetThunkManager().GetThunkSymbolTable().find(sym);
            if (it != host.GetThunkManager().GetThunkSymbolTable().end()) {
                jit.Regs()[0] = it->second;
                return;
            }
            if (host.GetFmodEventLoader()) {
                uint32_t addr = host.GetFmodEventLoader()->GetSymbolAddress(sym);
                if (addr != 0) { jit.Regs()[0] = addr; return; }
            }
            if (host.GetFmodExLoader()) {
                uint32_t addr = host.GetFmodExLoader()->GetSymbolAddress(sym);
                if (addr != 0) { jit.Regs()[0] = addr; return; }
            }
            uint32_t addr = host.GetThunkManager().GetOrCreateThunkAddress(sym);
            jit.Regs()[0] = addr;
            return;
        }
        jit.Regs()[0] = 0;
    });

    RegisterThunk("__system_property_get", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        uint32_t val_ptr = jit.Regs()[1];
        LOGI("__system_property_get('%s')", name ? name : "null");
        if (name && val_ptr) {
            char* out = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), val_ptr));
            if (strcmp(name, "ro.build.version.sdk") == 0) {
                strcpy(out, "28");
                jit.Regs()[0] = 2;
                return;
            }
            strcpy(out, "");
        }
        jit.Regs()[0] = 0;
    });

    // ARM32 zlib uses 32-bit uLong and a 56-byte z_stream. Translate every
    // pointer/counter rather than exposing an LP64 host z_stream.
    RegisterThunk("crc32", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const Bytef* data = reinterpret_cast<const Bytef*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = static_cast<uint32_t>(crc32(jit.Regs()[0], data, jit.Regs()[2]));
    });
    RegisterThunk("compress", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Bytef* dst = reinterpret_cast<Bytef*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        uint32_t length_addr = jit.Regs()[1];
        const Bytef* src = reinterpret_cast<const Bytef*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        if (!dst || !length_addr || !src) { jit.Regs()[0] = Z_STREAM_ERROR; return; }
        uLongf length = host.MemoryRead32(length_addr);
        HornPerfScope perf(HornPerfCategory::Decompress);
        int result = compress(dst, &length, src, jit.Regs()[3]);
        perf.SetBytes(static_cast<uint64_t>(length));
        host.MemoryWrite32(length_addr, static_cast<uint32_t>(length));
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("compress2", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const uint32_t dst_address = args.Ptr();
        const uint32_t length_address = args.Ptr();
        const uint32_t src_address = args.Ptr();
        const uint32_t src_length = args.U32();
        const int level = args.I32();
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       length_address, sizeof(uint32_t))) {
            jit.Regs()[0] = Z_STREAM_ERROR;
            return;
        }
        const uint32_t capacity = host.MemoryRead32(length_address);
        Bytef* dst = static_cast<Bytef*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), dst_address,
            capacity));
        const Bytef* src = static_cast<const Bytef*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), src_address,
            src_length));
        if (!dst || !src) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = Z_STREAM_ERROR;
            return;
        }
        uLongf output_length = capacity;
        const int result = ::compress2(dst, &output_length, src, src_length, level);
        host.MemoryWrite32(length_address, static_cast<uint32_t>(output_length));
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("uncompress", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Bytef* dst = reinterpret_cast<Bytef*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        uint32_t length_addr = jit.Regs()[1];
        const Bytef* src = reinterpret_cast<const Bytef*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        if (!dst || !length_addr || !src) { jit.Regs()[0] = Z_STREAM_ERROR; return; }
        uLongf length = host.MemoryRead32(length_addr);
        HornPerfScope perf(HornPerfCategory::Decompress);
        int result = uncompress(dst, &length, src, jit.Regs()[3]);
        perf.SetBytes(static_cast<uint64_t>(length));
        if (TraceGuestZlib())
            LOGI("zlib uncompress src=0x%08X input=%u output=%lu result=%d",
                 jit.Regs()[2], jit.Regs()[3], static_cast<unsigned long>(length), result);
        host.MemoryWrite32(length_addr, static_cast<uint32_t>(length));
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("get_crc_table", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static uint32_t guest_table = 0;
        if (!guest_table) {
            const z_crc_t* table = get_crc_table();
            guest_table = host.GuestAlloc(256 * sizeof(uint32_t), 4);
            if (guest_table && table) {
                auto* out = reinterpret_cast<uint32_t*>(GuestToHost(host.GetGuestMemoryBase(), guest_table));
                for (size_t i = 0; i < 256; ++i) out[i] = static_cast<uint32_t>(table[i]);
            }
        }
        jit.Regs()[0] = guest_table;
    });
    RegisterThunk("inflateInit2_", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t stream_addr = jit.Regs()[0];
        const int window_bits = static_cast<int>(jit.Regs()[1]);
        if (!stream_addr || jit.Regs()[3] != sizeof(Arm32ZStream)) {
            LOGE("inflateInit2_: ARM32 z_stream size=%u, expected %zu", jit.Regs()[3], sizeof(Arm32ZStream));
            jit.Regs()[0] = Z_VERSION_ERROR;
            return;
        }
        auto state = std::make_unique<InflateState>();
        Arm32ZStream* guest = reinterpret_cast<Arm32ZStream*>(GuestToHost(host.GetGuestMemoryBase(), stream_addr));
        if (!guest || static_cast<bool>(guest->zalloc) != static_cast<bool>(guest->zfree)) {
            LOGE("inflateInit2_: invalid guest allocator pair (zalloc=0x%08X zfree=0x%08X)",
                 guest ? guest->zalloc : 0, guest ? guest->zfree : 0);
            jit.Regs()[0] = Z_STREAM_ERROR;
            return;
        }
        // Host zlib state never crosses into the ARM32 z_stream. Its internal
        // allocations must therefore use host malloc/free, even when the
        // guest supplied its own allocator pair for a native ARM32 zlib.
        if (guest->zalloc && TraceGuestZlib())
            LOGI("zlib inflateInit2 using host-owned allocator stream=0x%08X", stream_addr);
        uint32_t old_in = guest->next_in, old_out = guest->next_out;
        int result = inflateInit2(&state->stream, window_bits);
        if (result == Z_OK) {
            SyncInflateFromGuest(host, stream_addr, *state);
            SyncInflateToGuest(host, stream_addr, *state, old_in, old_out);
            std::lock_guard<std::mutex> lock(g_inflate_mutex);
            g_inflate_states[stream_addr] = std::move(state);
        }
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("inflateInit_", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t stream_addr = jit.Regs()[0];
        const uint32_t version_addr = jit.Regs()[1];
        const uint32_t stream_size = jit.Regs()[2];
        const char* version = GuestCString(host.GetGuestMemoryBase(),
                                           host.GetGuestMemorySize(), version_addr);
        if (!version || !stream_addr || stream_size != sizeof(Arm32ZStream) ||
            version[0] != ZLIB_VERSION[0] || version[1] != ZLIB_VERSION[1] ||
            version[2] != ZLIB_VERSION[2]) {
            if (TraceGuestZlib())
                LOGI("zlib inflateInit rejected stream=0x%08X version='%s' "
                     "size=%u expected=%zu",
                     stream_addr, version ? version : "", stream_size,
                     sizeof(Arm32ZStream));
            jit.Regs()[0] = Z_VERSION_ERROR;
            return;
        }
        auto state = std::make_unique<InflateState>();
        auto* guest = static_cast<Arm32ZStream*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), stream_addr,
            sizeof(Arm32ZStream)));
        if (!guest || static_cast<bool>(guest->zalloc) != static_cast<bool>(guest->zfree)) {
            if (TraceGuestZlib())
                LOGI("zlib inflateInit rejected invalid allocator pair stream=0x%08X "
                     "zalloc=0x%08X zfree=0x%08X",
                     stream_addr, guest ? guest->zalloc : 0,
                     guest ? guest->zfree : 0);
            jit.Regs()[0] = Z_STREAM_ERROR;
            return;
        }
        if (guest->zalloc && TraceGuestZlib())
            LOGI("zlib inflateInit using host-owned allocator stream=0x%08X "
                 "guestZalloc=0x%08X guestZfree=0x%08X",
                 stream_addr, guest->zalloc, guest->zfree);
        const uint32_t old_in = guest->next_in;
        const uint32_t old_out = guest->next_out;
        const int result = ::inflateInit2(&state->stream, MAX_WBITS);
        if (TraceGuestZlib())
            LOGI("zlib inflateInit stream=0x%08X version='%s' size=%u result=%d",
                 stream_addr, version, stream_size, result);
        if (result == Z_OK) {
            SyncInflateFromGuest(host, stream_addr, *state);
            SyncInflateToGuest(host, stream_addr, *state, old_in, old_out);
            std::lock_guard<std::mutex> lock(g_inflate_mutex);
            g_inflate_states[stream_addr] = std::move(state);
        }
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("inflate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::Decompress);
        std::lock_guard<std::mutex> lock(g_inflate_mutex);
        auto it = g_inflate_states.find(jit.Regs()[0]);
        if (it == g_inflate_states.end()) {
            if (TraceGuestZlib())
                LOGI("zlib inflate missing state stream=0x%08X flush=%u",
                     jit.Regs()[0], jit.Regs()[1]);
            jit.Regs()[0] = Z_STREAM_ERROR;
            return;
        }
        Arm32ZStream* guest = reinterpret_cast<Arm32ZStream*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        uint32_t old_in = guest ? guest->next_in : 0, old_out = guest ? guest->next_out : 0;
        if (!SyncInflateFromGuest(host, jit.Regs()[0], *it->second)) { jit.Regs()[0] = Z_STREAM_ERROR; return; }
        const uLong before_output = it->second->stream.total_out;
        int result = inflate(&it->second->stream, static_cast<int>(jit.Regs()[1]));
        perf.SetBytes(static_cast<uint64_t>(
            it->second->stream.total_out - before_output));
        SyncInflateToGuest(host, jit.Regs()[0], *it->second, old_in, old_out);
        if (TraceGuestZlib()) {
            const z_stream& state = it->second->stream;
            LOGI("zlib inflate stream=0x%08X flush=%u result=%d in=%lu avail_in=%u "
                 "out=%lu avail_out=%u msg='%s'",
                 jit.Regs()[0], jit.Regs()[1], result,
                 static_cast<unsigned long>(state.total_in), state.avail_in,
                 static_cast<unsigned long>(state.total_out), state.avail_out,
                 state.msg ? state.msg : "");
        }
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("inflateEnd", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_inflate_mutex);
        auto it = g_inflate_states.find(jit.Regs()[0]);
        if (it == g_inflate_states.end()) { jit.Regs()[0] = Z_STREAM_ERROR; return; }
        int result = inflateEnd(&it->second->stream);
        if (it->second->message) host.GuestFree(it->second->message);
        g_inflate_states.erase(it);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("inflateReset", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_inflate_mutex);
        auto it = g_inflate_states.find(jit.Regs()[0]);
        if (it == g_inflate_states.end()) {
            jit.Regs()[0] = Z_STREAM_ERROR;
            return;
        }
        auto* guest = static_cast<Arm32ZStream*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), jit.Regs()[0],
            sizeof(Arm32ZStream)));
        if (!guest || !SyncInflateFromGuest(host, jit.Regs()[0], *it->second)) {
            jit.Regs()[0] = Z_STREAM_ERROR;
            return;
        }
        const uint32_t old_in = guest->next_in;
        const uint32_t old_out = guest->next_out;
        const int result = ::inflateReset(&it->second->stream);
        SyncInflateToGuest(host, jit.Regs()[0], *it->second, old_in, old_out);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("gzopen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* mode = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!path || !mode || strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) {
            LOGE("gzopen: invalid or non-read-only request for '%s' mode '%s'", path ? path : "", mode ? mode : "");
            jit.Regs()[0] = 0;
            return;
        }
        gzFile file = gzopen(path, mode);
        if (!file) { jit.Regs()[0] = 0; return; }
        std::lock_guard<std::mutex> lock(g_gz_mutex);
        uint32_t handle = g_next_gz_handle++;
        g_gz_files.emplace(handle, file);
        jit.Regs()[0] = handle;
    });
    RegisterThunk("gzgets", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_gz_mutex);
        auto it = g_gz_files.find(jit.Regs()[0]);
        char* buffer = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (it == g_gz_files.end() || !buffer || !jit.Regs()[2]) { jit.Regs()[0] = 0; return; }
        jit.Regs()[0] = gzgets(it->second, buffer, static_cast<int>(jit.Regs()[2])) ? jit.Regs()[1] : 0;
    });
    RegisterThunk("gzclose", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_gz_mutex);
        auto it = g_gz_files.find(jit.Regs()[0]);
        if (it == g_gz_files.end()) { jit.Regs()[0] = Z_STREAM_ERROR; return; }
        int result = gzclose(it->second);
        g_gz_files.erase(it);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });

    // C++ Memory Allocation operators
    RegisterThunk("_Znaj", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_Znwj", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_ZnajRKSt9nothrow_t", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_ZnwjRKSt9nothrow_t", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("_ZdaPv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        host.GuestFree(jit.Regs()[0]);
    });

    RegisterThunk("_ZdlPv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        host.GuestFree(jit.Regs()[0]);
    });

    RegisterThunk("_ZNSt8ios_base4InitC1Ev", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_ZNSt8ios_base4InitD1Ev", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});

    // Memory Allocation inside guest space
    RegisterThunk("malloc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t size = jit.Regs()[0];
        jit.Regs()[0] = host.GuestAlloc(size, 8);
    });

    RegisterThunk("calloc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t num = jit.Regs()[0];
        size_t size = jit.Regs()[1];
        // size_t is 32-bit in the guest even though this thunk is compiled as
        // ARM64. Reject products the guest ABI cannot represent instead of
        // silently allocating a truncated/oversized block.
        if (num != 0 && size > std::numeric_limits<uint32_t>::max() / num) {
            SetGuestErrno(host, ENOMEM);
            jit.Regs()[0] = 0;
            return;
        }
        const size_t total = num * size;
        uint32_t addr = host.GuestAlloc(total, 8);
        if (addr) {
            memset(GuestToHost(host.GetGuestMemoryBase(), addr), 0, total);
        } else {
            SetGuestErrno(host, ENOMEM);
        }
        jit.Regs()[0] = addr;
    });

    RegisterThunk("realloc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t old_addr = jit.Regs()[0];
        size_t new_size = jit.Regs()[1];

        if (old_addr == 0) {
            const uint32_t addr = host.GuestAlloc(new_size, 8);
            if (!addr) SetGuestErrno(host, ENOMEM);
            jit.Regs()[0] = addr;
            return;
        }
        if (new_size == 0) {
            host.GuestFree(old_addr);
            jit.Regs()[0] = 0;
            return;
        }

        const uint32_t old_size = host.GuestAllocSize(old_addr);
        if (old_size == 0) {
            // Every pointer returned by this bridge's malloc/calloc/realloc has
            // an allocator header. Treat anything else as invalid rather than
            // reading an arbitrary new_size bytes past an unknown allocation.
            LOGE("realloc rejected foreign/freed guest pointer 0x%08X", old_addr);
            SetGuestErrno(host, EINVAL);
            jit.Regs()[0] = 0;
            return;
        }

        uint32_t new_addr = host.GuestAlloc(new_size, 8);
        if (new_addr) {
            memcpy(GuestToHost(host.GetGuestMemoryBase(), new_addr),
                   GuestToHost(host.GetGuestMemoryBase(), old_addr),
                   std::min<size_t>(old_size, new_size));
            host.GuestFree(old_addr);
        } else {
            // C realloc leaves the original allocation live on failure.
            SetGuestErrno(host, ENOMEM);
        }
        jit.Regs()[0] = new_addr;
    });

    RegisterThunk("free", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        host.GuestFree(jit.Regs()[0]);
    });

    RegisterThunk("memalign", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        size_t alignment = jit.Regs()[0];
        size_t size = jit.Regs()[1];
        jit.Regs()[0] = host.GuestAlloc(size, alignment);
    });

    // Logging & Formatted output
    RegisterThunk("__android_log_print", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        ObserveBl2IoFailureLog(jit, host);
#if HORN_ENABLE_LOGGING
        int prio = static_cast<int>(jit.Regs()[0]);
        const char* tag = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::string formatted = FormatGuestString(jit, host, fmt, 3);
        // ZIP enumeration emits one row per asset during startup. On ColorOS
        // this trips the per-process log quota and hides useful diagnostics.
        if (tag && strcmp(tag, "libzip") == 0) {
            const bool important = formatted.find("error") != std::string::npos ||
                                   formatted.find("Error") != std::string::npos ||
                                   formatted.find("fail") != std::string::npos ||
                                   formatted.find("Fail") != std::string::npos ||
                                   formatted.find("not found") != std::string::npos ||
                                   formatted.find("Not found") != std::string::npos;
            if (important) {
                __android_log_print(prio, tag, "[GUEST] %s", formatted.c_str());
            }
            jit.Regs()[0] = 0;
            return;
        }
        __android_log_print(prio, tag ? tag : "Arm32Bridge_Guest", "[GUEST] %s", formatted.c_str());
#else
        (void)host;
#endif
        jit.Regs()[0] = 0;
    });
    RegisterThunk("__android_log_assert", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
#if HORN_ENABLE_LOGGING
        const char* condition = GuestCString(host.GetGuestMemoryBase(),
                                             host.GetGuestMemorySize(), jit.Regs()[0]);
        const char* tag = GuestCString(host.GetGuestMemoryBase(),
                                       host.GetGuestMemorySize(), jit.Regs()[1]);
        const char* format = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[2]);
        if (!format) {
            SetGuestErrno(host, EFAULT);
            __android_log_assert(condition, tag ? tag : "Borderlands2",
                                 "invalid guest assertion format");
            return;
        }
        const std::string message = FormatGuestString(jit, host, format, 3);
        __android_log_assert(condition, tag ? tag : "Borderlands2", "%s",
                             message.c_str());
#else
        // Assertions remain fatal when log output is compiled out.
        (void)jit;
        (void)host;
        std::abort();
#endif
    });

    RegisterThunk("printf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        std::string formatted = FormatGuestString(jit, host, fmt, 1);
        LOGI("[printf] %s", formatted.c_str());
        jit.Regs()[0] = formatted.length();
    });

    RegisterThunk("fprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        std::string formatted = FormatGuestString(jit, host, fmt, 2);
        LOGI("[fprintf] %s", formatted.c_str());
        jit.Regs()[0] = formatted.length();
    });

    RegisterThunk("sprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* str = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        std::string formatted = FormatGuestString(jit, host, fmt, 2);
        if (str) {
            strcpy(str, formatted.c_str());
            jit.Regs()[0] = formatted.length();
        } else {
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("snprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* str = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        size_t size = jit.Regs()[1];
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::string formatted = FormatGuestString(jit, host, fmt, 3);
        if (str && size > 0) {
            strncpy(str, formatted.c_str(), size - 1);
            str[size - 1] = '\0';
            jit.Regs()[0] = formatted.length();
        } else {
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("sscanf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("vfscanf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = static_cast<uint32_t>(GuestVfscanf(host, jit.Regs()[0], fmt, jit.Regs()[2]));
    });
    RegisterThunk("__svfscanf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* fmt = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = static_cast<uint32_t>(GuestVfscanf(host, jit.Regs()[0], fmt, jit.Regs()[2]));
    });
    RegisterThunk("vscanf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* format = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[0]);
        jit.Regs()[0] = static_cast<uint32_t>(
            GuestVfscanf(host, 0x200u, format, jit.Regs()[1]));
    });
    RegisterThunk("vfprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("vasprintf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("putc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = jit.Regs()[0]; });
    RegisterThunk("fputc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = jit.Regs()[0]; });

    // fputs(const char *s, FILE *stream). Lesson 2.16: on Android fd 1 and 2 go nowhere, and
    // that is exactly where a C++ runtime writes the reason it is giving up. A stream the
    // guest really opened is written for real; anything else goes to the log instead of being
    // swallowed. In Angry Birds Go this was the last default stub still firing on a cold
    // start, and the first run with a real thunk printed what had been thrown away for
    // weeks: "io::IOException".
    RegisterThunk("fputs", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        if (!s) { jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(jit.Regs()[1]);
        if (it != g_file_map.end()) {
            jit.Regs()[0] = static_cast<uint32_t>(fputs(s, it->second));
            return;
        }
        LOGI("[guest fputs] %s", s);
        jit.Regs()[0] = static_cast<uint32_t>(strlen(s));
    });

    // Math functions
    RegisterThunk("sinf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = sinf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("cosf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = cosf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("sqrtf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = sqrtf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("atanf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = atanf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("acosf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float val = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float res = acosf(val);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    RegisterThunk("powf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float base = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float exp = *reinterpret_cast<float*>(&jit.Regs()[1]);
        float res = powf(base, exp);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&res);
    });

    // String & Memory functions
    RegisterThunk("strcmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = (s1 && s2) ? strcmp(s1, s2) : 0;
    });

    RegisterThunk("strncmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        size_t n = jit.Regs()[2];
        jit.Regs()[0] = (s1 && s2) ? strncmp(s1, s2, n) : 0;
    });

    RegisterThunk("strcasecmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s1 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* s2 = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[0] = (s1 && s2) ? strcasecmp(s1, s2) : 0;
    });

    RegisterThunk("strlen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = s ? static_cast<uint32_t>(strlen(s)) : 0;
    });

    RegisterThunk("strcpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* dest = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (src && GuestLowGuardHit(jit.Regs()[0], strlen(src) + 1)) {
            host.ReportLowGuardWrite(jit.Regs()[0], strlen(src) + 1);
            dest = nullptr;
        }
        if (dest && src) strcpy(dest, src);
    });

    RegisterThunk("strdup", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        if (!src) { jit.Regs()[0] = 0; return; }
        size_t len = strlen(src) + 1;
        uint32_t dest_addr = host.GuestAlloc(len);
        memcpy(GuestToHost(host.GetGuestMemoryBase(), dest_addr), src, len);
        jit.Regs()[0] = dest_addr;
    });

    RegisterThunk("strrchr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* s = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int c = static_cast<int>(jit.Regs()[1]);
        if (!s) { jit.Regs()[0] = 0; return; }
        const char* res = strrchr(s, c);
        jit.Regs()[0] = res ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(res) - reinterpret_cast<uintptr_t>(host.GetGuestMemoryBase())) : 0;
    });

    // strerror hands back storage the caller does not own and never frees, so a
    // fresh block per call is a block per failed open - and a guest that probes
    // for files that are not there does that constantly. One block per errno.
    RegisterThunk("strerror", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static std::mutex m;
        static std::unordered_map<int, uint32_t> cache;
        int err_no = static_cast<int>(jit.Regs()[0]);
        std::lock_guard<std::mutex> lock(m);
        auto it = cache.find(err_no);
        if (it == cache.end()) {
            const char* err = strerror(err_no);
            size_t len = strlen(err) + 1;
            uint32_t dest = host.GuestAlloc(len);
            if (dest) memcpy(GuestToHost(host.GetGuestMemoryBase(), dest), err, len);
            it = cache.emplace(err_no, dest).first;
        }
        jit.Regs()[0] = it->second;
    });

    RegisterThunk("strtoul", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* nptr = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int base = static_cast<int>(jit.Regs()[2]);
        jit.Regs()[0] = nptr ? static_cast<uint32_t>(strtoul(nptr, nullptr, base)) : 0;
    });

    RegisterThunk("atoi", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* str = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = str ? atoi(str) : 0;
    });

    RegisterThunk("islower", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = islower(static_cast<int>(jit.Regs()[0]));
    });

    RegisterThunk("isxdigit", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = isxdigit(static_cast<int>(jit.Regs()[0]));
    });
    RegisterThunk("isalpha", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = isalpha(static_cast<unsigned char>(jit.Regs()[0]));
    });
    auto wide_predicate = [](int (*fn)(wint_t)) {
        return [fn](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            jit.Regs()[0] = fn(static_cast<wint_t>(jit.Regs()[0]));
        };
    };
    RegisterThunk("iswalnum", wide_predicate(iswalnum));
    RegisterThunk("iswalpha", wide_predicate(iswalpha));
    RegisterThunk("iswdigit", wide_predicate(iswdigit));
    RegisterThunk("iswspace", wide_predicate(iswspace));
    RegisterThunk("iswupper", wide_predicate(iswupper));
    RegisterThunk("iswxdigit", wide_predicate(iswxdigit));
    RegisterThunk("towupper", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = static_cast<uint32_t>(towupper(jit.Regs()[0])); });
    RegisterThunk("btowc", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = static_cast<uint32_t>(btowc(static_cast<int>(jit.Regs()[0]))); });
    RegisterThunk("wctob", [](Dynarmic::A32::Jit& jit, DynarmicHost&) { jit.Regs()[0] = static_cast<uint32_t>(wctob(static_cast<wint_t>(jit.Regs()[0]))); });
    RegisterThunk("strcoll", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* a = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* b = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!a || !b) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        jit.Regs()[0] = static_cast<uint32_t>(strcoll(a, b));
    });
    RegisterThunk("strchr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t addr = jit.Regs()[0];
        const char* value = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), addr));
        const char* found = value ? strchr(value, static_cast<int>(jit.Regs()[1])) : nullptr;
        jit.Regs()[0] = found ? addr + static_cast<uint32_t>(found - value) : 0;
    });
    RegisterThunk("strstr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t addr = jit.Regs()[0];
        const char* value = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), addr));
        const char* needle = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        const char* found = value && needle ? strstr(value, needle) : nullptr;
        jit.Regs()[0] = found ? addr + static_cast<uint32_t>(found - value) : 0;
    });
    RegisterThunk("strtoimax", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t src_addr = jit.Regs()[0], end_addr = jit.Regs()[1];
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), src_addr));
        char* end = nullptr;
        intmax_t value = src ? strtoimax(src, &end, static_cast<int>(jit.Regs()[2])) : 0;
        if (end_addr) host.MemoryWrite32(end_addr, end ? src_addr + static_cast<uint32_t>(end - src) : 0);
        uint64_t raw = static_cast<uint64_t>(value);
        jit.Regs()[0] = static_cast<uint32_t>(raw);
        jit.Regs()[1] = static_cast<uint32_t>(raw >> 32);
    });
    RegisterThunk("strtoumax", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t src_addr = jit.Regs()[0], end_addr = jit.Regs()[1];
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), src_addr));
        char* end = nullptr;
        uintmax_t value = src ? strtoumax(src, &end, static_cast<int>(jit.Regs()[2])) : 0;
        if (end_addr) host.MemoryWrite32(end_addr, end ? src_addr + static_cast<uint32_t>(end - src) : 0);
        jit.Regs()[0] = static_cast<uint32_t>(value);
        jit.Regs()[1] = static_cast<uint32_t>(value >> 32);
    });
    RegisterThunk("wcscpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        wchar_t* dst = reinterpret_cast<wchar_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const wchar_t* src = reinterpret_cast<const wchar_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!dst || !src) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        wcscpy(dst, src);
    });
    RegisterThunk("wcslen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const wchar_t* src = reinterpret_cast<const wchar_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = src ? static_cast<uint32_t>(wcslen(src)) : 0;
    });
    RegisterThunk("wmemchr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t addr = jit.Regs()[0];
        const wchar_t* src = reinterpret_cast<const wchar_t*>(GuestToHost(host.GetGuestMemoryBase(), addr));
        const wchar_t* found = src ? wmemchr(src, static_cast<wchar_t>(jit.Regs()[1]), jit.Regs()[2]) : nullptr;
        jit.Regs()[0] = found ? addr + static_cast<uint32_t>((found - src) * sizeof(wchar_t)) : 0;
    });
    RegisterThunk("mbrtowc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        wchar_t* out = reinterpret_cast<wchar_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        mbstate_t* state = jit.Regs()[3] ? &g_mb_states[jit.Regs()[3]] : &g_internal_mb_state;
        jit.Regs()[0] = static_cast<uint32_t>(mbrtowc(out, src, jit.Regs()[2], state));
    });
    RegisterThunk("mbrlen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* src = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        mbstate_t* state = jit.Regs()[2] ? &g_mb_states[jit.Regs()[2]] : &g_internal_mb_state;
        jit.Regs()[0] = static_cast<uint32_t>(mbrlen(src, jit.Regs()[1], state));
    });
    RegisterThunk("wcrtomb", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* out = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        mbstate_t* state = jit.Regs()[2] ? &g_mb_states[jit.Regs()[2]] : &g_internal_mb_state;
        jit.Regs()[0] = static_cast<uint32_t>(wcrtomb(out, static_cast<wchar_t>(jit.Regs()[1]), state));
    });
    RegisterThunk("mbsrtowcs", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        wchar_t* out = reinterpret_cast<wchar_t*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const uint32_t source_pointer = jit.Regs()[1];
        uint32_t source_addr = source_pointer ? host.MemoryRead32(source_pointer) : 0;
        const char* source = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), source_addr));
        const char* cursor = source;
        mbstate_t* state = jit.Regs()[3] ? &g_mb_states[jit.Regs()[3]] : &g_internal_mb_state;
        size_t result = source ? mbsrtowcs(out, &cursor, jit.Regs()[2], state) : static_cast<size_t>(-1);
        if (source_pointer) host.MemoryWrite32(source_pointer,
            cursor ? source_addr + static_cast<uint32_t>(cursor - source) : 0);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("wcsrtombs", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        char* out = reinterpret_cast<char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const uint32_t source_pointer = jit.Regs()[1];
        uint32_t source_addr = source_pointer ? host.MemoryRead32(source_pointer) : 0;
        const wchar_t* source = reinterpret_cast<const wchar_t*>(GuestToHost(host.GetGuestMemoryBase(), source_addr));
        const wchar_t* cursor = source;
        mbstate_t* state = jit.Regs()[3] ? &g_mb_states[jit.Regs()[3]] : &g_internal_mb_state;
        size_t result = source ? wcsrtombs(out, &cursor, jit.Regs()[2], state) : static_cast<size_t>(-1);
        if (source_pointer) host.MemoryWrite32(source_pointer,
            cursor ? source_addr + static_cast<uint32_t>((cursor - source) * sizeof(wchar_t)) : 0);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });

    RegisterThunk("memset", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        void* s = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        int c = static_cast<int>(jit.Regs()[1]);
        size_t n = jit.Regs()[2];
        if (GuestLowGuardHit(jit.Regs()[0], n)) { host.ReportLowGuardWrite(jit.Regs()[0], n); s = nullptr; }
        if (s) {
            GuestMappedWriteGuard write(jit.Regs()[0], n);
            memset(write.Destination(s), c, n);
        }
    });

    RegisterThunk("memcpy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        void* dest = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        const void* src = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]);
        size_t n = jit.Regs()[2];
        if (GuestLowGuardHit(jit.Regs()[0], n)) { host.ReportLowGuardWrite(jit.Regs()[0], n); dest = nullptr; }
        if (dest && src) {
            GuestMappedWriteGuard write(jit.Regs()[0], n);
            memcpy(write.Destination(dest), src, n);
        }
    });

    RegisterThunk("memcmp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const void* s1 = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        const void* s2 = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]);
        size_t n = jit.Regs()[2];
        jit.Regs()[0] = (s1 && s2) ? memcmp(s1, s2, n) : 0;
    });

    RegisterThunk("memchr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const void* s = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        int c = static_cast<int>(jit.Regs()[1]);
        size_t n = jit.Regs()[2];
        void* res = s ? const_cast<void*>(memchr(s, c, n)) : nullptr;
        jit.Regs()[0] = res ? HostToGuest(host.GetGuestMemoryBase(), res) : 0;
    });

    RegisterThunk("qsort", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t base_addr = jit.Regs()[0];
        const uint32_t count = jit.Regs()[1];
        const uint32_t size = jit.Regs()[2];
        const uint32_t compare = jit.Regs()[3];
        if (count < 2) return;
        if (!base_addr || !size || !compare ||
            static_cast<uint64_t>(base_addr) + static_cast<uint64_t>(count) * size > host.GetGuestMemorySize()) {
            LOGE("qsort: invalid ARM32 range/comparator base=0x%08X count=%u size=%u cmp=0x%08X",
                 base_addr, count, size, compare);
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        const size_t bytes = static_cast<size_t>(count) * size;
        std::vector<uint8_t> scratch;
        try { scratch.resize(bytes); }
        catch (...) {
            LOGE("qsort: unable to allocate %zu-byte merge buffer", bytes);
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        uint8_t* base = reinterpret_cast<uint8_t*>(GuestToHost(host.GetGuestMemoryBase(), base_addr));
        // RunGuestCallback owns a separate nested A32 JIT/stack, so comparator calls
        // are synchronous even though qsort itself is executing inside the main JIT.
        for (uint32_t width = 1; width < count; width = width > count / 2 ? count : width * 2) {
            for (uint32_t left = 0; left < count; left += 2 * width) {
                const uint32_t mid = std::min(left + width, count);
                const uint32_t right = std::min(left + 2 * width, count);
                uint32_t i = left, j = mid, k = left;
                while (i < mid && j < right) {
                    int32_t order = static_cast<int32_t>(host.RunGuestCallback(
                        compare, {base_addr + i * size, base_addr + j * size}));
                    const uint32_t chosen = order <= 0 ? i++ : j++;
                    memcpy(scratch.data() + static_cast<size_t>(k++) * size,
                           base + static_cast<size_t>(chosen) * size, size);
                }
                while (i < mid) {
                    memcpy(scratch.data() + static_cast<size_t>(k++) * size,
                           base + static_cast<size_t>(i++) * size, size);
                }
                while (j < right) {
                    memcpy(scratch.data() + static_cast<size_t>(k++) * size,
                           base + static_cast<size_t>(j++) * size, size);
                }
                memcpy(base + static_cast<size_t>(left) * size,
                       scratch.data() + static_cast<size_t>(left) * size,
                       static_cast<size_t>(right - left) * size);
            }
        }
    });

    // ARM AEABI Helpers
    RegisterThunk("__aeabi_idiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int32_t num = static_cast<int32_t>(jit.Regs()[0]);
        int32_t den = static_cast<int32_t>(jit.Regs()[1]);
        jit.Regs()[0] = den ? (num / den) : 0;
    });

    RegisterThunk("__aeabi_uidiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t num = jit.Regs()[0];
        uint32_t den = jit.Regs()[1];
        jit.Regs()[0] = den ? (num / den) : 0;
    });

    RegisterThunk("__aeabi_idivmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int32_t num = static_cast<int32_t>(jit.Regs()[0]);
        int32_t den = static_cast<int32_t>(jit.Regs()[1]);
        if (den != 0) {
            jit.Regs()[0] = num / den;
            jit.Regs()[1] = num % den;
        } else {
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
        }
    });

    RegisterThunk("__aeabi_uidivmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t num = jit.Regs()[0];
        uint32_t den = jit.Regs()[1];
        if (den != 0) {
            jit.Regs()[0] = num / den;
            jit.Regs()[1] = num % den;
        } else {
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
        }
    });

    RegisterThunk("__aeabi_uldivmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t num = static_cast<uint64_t>(jit.Regs()[0]) | (static_cast<uint64_t>(jit.Regs()[1]) << 32);
        uint64_t den = static_cast<uint64_t>(jit.Regs()[2]) | (static_cast<uint64_t>(jit.Regs()[3]) << 32);
        if (den != 0) {
            uint64_t q = num / den;
            uint64_t r = num % den;
            jit.Regs()[0] = static_cast<uint32_t>(q & 0xFFFFFFFF);
            jit.Regs()[1] = static_cast<uint32_t>(q >> 32);
            jit.Regs()[2] = static_cast<uint32_t>(r & 0xFFFFFFFF);
            jit.Regs()[3] = static_cast<uint32_t>(r >> 32);
        }
    });

    RegisterThunk("__gnu_ldivmod_helper", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t num = static_cast<int64_t>(jit.Regs()[0]) | (static_cast<int64_t>(jit.Regs()[1]) << 32);
        int64_t den = static_cast<int64_t>(jit.Regs()[2]) | (static_cast<int64_t>(jit.Regs()[3]) << 32);
        if (den != 0) {
            int64_t q = num / den;
            int64_t r = num % den;
            jit.Regs()[0] = static_cast<uint32_t>(q & 0xFFFFFFFF);
            jit.Regs()[1] = static_cast<uint32_t>(q >> 32);
            jit.Regs()[2] = static_cast<uint32_t>(r & 0xFFFFFFFF);
            jit.Regs()[3] = static_cast<uint32_t>(r >> 32);
        }
    });

    RegisterThunk("__aeabi_f2lz", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float f = *reinterpret_cast<float*>(&jit.Regs()[0]);
        int64_t i = static_cast<int64_t>(f);
        jit.Regs()[0] = static_cast<uint32_t>(i & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(i >> 32);
    });

    RegisterThunk("__aeabi_f2ulz", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float f = *reinterpret_cast<float*>(&jit.Regs()[0]);
        uint64_t u = static_cast<uint64_t>(f);
        jit.Regs()[0] = static_cast<uint32_t>(u & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(u >> 32);
    });

    RegisterThunk("__aeabi_l2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t i = static_cast<int64_t>(jit.Regs()[0]) | (static_cast<int64_t>(jit.Regs()[1]) << 32);
        float f = static_cast<float>(i);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&f);
    });

    RegisterThunk("__aeabi_ul2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint64_t u = static_cast<uint64_t>(jit.Regs()[0]) | (static_cast<uint64_t>(jit.Regs()[1]) << 32);
        float f = static_cast<float>(u);
        jit.Regs()[0] = *reinterpret_cast<uint32_t*>(&f);
    });

    RegisterThunk("__aeabi_l2d", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int64_t i = static_cast<int64_t>(jit.Regs()[0]) | (static_cast<int64_t>(jit.Regs()[1]) << 32);
        double d = static_cast<double>(i);
        uint64_t u = *reinterpret_cast<uint64_t*>(&d);
        jit.Regs()[0] = static_cast<uint32_t>(u & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(u >> 32);
    });

    // AEABI memory functions
    auto aeabi_memclr_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t n = jit.Regs()[1];
        if (GuestLowGuardHit(dest, n)) { host.ReportLowGuardWrite(dest, n); dest = 0; }
        if (dest && n) {
            GuestMappedWriteGuard write(dest, n);
            memset(write.Destination(GuestToHost(host.GetGuestMemoryBase(), dest)), 0, n);
        }
    };
    RegisterThunk("__aeabi_memclr", aeabi_memclr_handler);
    RegisterThunk("__aeabi_memclr4", aeabi_memclr_handler);
    RegisterThunk("__aeabi_memclr8", aeabi_memclr_handler);

    auto aeabi_memset_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t n = jit.Regs()[1];
        int c = static_cast<int>(jit.Regs()[2]);
        if (GuestLowGuardHit(dest, n)) { host.ReportLowGuardWrite(dest, n); dest = 0; }
        if (dest && n) {
            GuestMappedWriteGuard write(dest, n);
            memset(write.Destination(GuestToHost(host.GetGuestMemoryBase(), dest)), c, n);
        }
    };
    RegisterThunk("__aeabi_memset", aeabi_memset_handler);
    RegisterThunk("__aeabi_memset4", aeabi_memset_handler);
    RegisterThunk("__aeabi_memset8", aeabi_memset_handler);

    auto aeabi_memcpy_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        if (GuestLowGuardHit(dest, n)) { host.ReportLowGuardWrite(dest, n); dest = 0; }
        if (dest && src && n) {
            GuestMappedWriteGuard write(dest, n);
            memcpy(write.Destination(GuestToHost(host.GetGuestMemoryBase(), dest)),
                   GuestToHost(host.GetGuestMemoryBase(), src), n);
        }
    };
    RegisterThunk("__aeabi_memcpy", aeabi_memcpy_handler);
    RegisterThunk("__aeabi_memcpy4", aeabi_memcpy_handler);
    RegisterThunk("__aeabi_memcpy8", aeabi_memcpy_handler);

    auto aeabi_memmove_handler = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t dest = jit.Regs()[0];
        uint32_t src = jit.Regs()[1];
        uint32_t n = jit.Regs()[2];
        if (GuestLowGuardHit(dest, n)) { host.ReportLowGuardWrite(dest, n); dest = 0; }
        if (dest && src && n) {
            GuestMappedWriteGuard write(dest, n, GuestRangesDisjoint(dest, src, n));
            memmove(write.Destination(GuestToHost(host.GetGuestMemoryBase(), dest)),
                    GuestToHost(host.GetGuestMemoryBase(), src), n);
        }
    };
    RegisterThunk("__aeabi_memmove", aeabi_memmove_handler);
    RegisterThunk("__aeabi_memmove4", aeabi_memmove_handler);
    RegisterThunk("__aeabi_memmove8", aeabi_memmove_handler);

    // Host-backed pthread synchronization. Map locks protect only address->state
    // lookup; they are always released before a potentially blocking wait.
    RegisterThunk("pthread_mutex_lock", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = LockGuestMutex(
            GetGuestMutex(host, jit.Regs()[0], true), CurrentGuestThreadId(), false);
    });
    RegisterThunk("pthread_mutex_trylock", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = LockGuestMutex(GetGuestMutex(host, jit.Regs()[0], true), CurrentGuestThreadId(), true);
    });
    RegisterThunk("pthread_mutex_unlock", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = UnlockGuestMutex(GetGuestMutex(host, jit.Regs()[0], true), CurrentGuestThreadId());
    });
    RegisterThunk("pthread_cond_init", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!jit.Regs()[0]) { jit.Regs()[0] = EINVAL; return; }
        auto state = std::make_shared<GuestCondState>();
        if (jit.Regs()[1]) {
            std::lock_guard<std::mutex> attr_lock(g_pthread_attr_mutex);
            auto attr = g_pthread_cond_attrs.find(jit.Regs()[1]);
            if (attr == g_pthread_cond_attrs.end()) { jit.Regs()[0] = EINVAL; return; }
            state->monotonic = attr->second;
        }
        {
            std::lock_guard<std::mutex> lock(g_sync_state_mutex);
            if (g_guest_conds.count(jit.Regs()[0])) { jit.Regs()[0] = EBUSY; return; }
            g_guest_conds.emplace(jit.Regs()[0], state);
        }
        host.MemoryWrite32(jit.Regs()[0], 0);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_cond_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto state = GetGuestCond(host, jit.Regs()[0], true);
        if (!state) { jit.Regs()[0] = EINVAL; return; }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->waiters) { jit.Regs()[0] = EBUSY; return; }
            state->destroyed = true;
        }
        std::lock_guard<std::mutex> map_lock(g_sync_state_mutex);
        g_guest_conds.erase(jit.Regs()[0]);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_cond_signal", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto state = GetGuestCond(host, jit.Regs()[0], true);
        if (!state) { jit.Regs()[0] = EINVAL; return; }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->destroyed) { jit.Regs()[0] = EINVAL; return; }
            if (!state->waiting_tickets.empty()) {
                uint64_t ticket = state->waiting_tickets.front();
                state->waiting_tickets.pop_front();
                state->signaled_tickets.insert(ticket);
            }
        }
        // notify_all lets the ticket predicate select exactly one pre-existing
        // waiter; a later waiter cannot consume an earlier signal.
        state->changed.notify_all();
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_cond_broadcast", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto state = GetGuestCond(host, jit.Regs()[0], true);
        if (!state) { jit.Regs()[0] = EINVAL; return; }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->destroyed) { jit.Regs()[0] = EINVAL; return; }
            ++state->broadcast_generation;
            state->waiting_tickets.clear();
            state->signaled_tickets.clear();
        }
        state->changed.notify_all();
        jit.Regs()[0] = 0;
    });
    auto cond_wait_handler = [](bool timed) {
        return [timed](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            const uint32_t cond_addr = jit.Regs()[0];
            const uint32_t mutex_addr = jit.Regs()[1];
            auto condition = GetGuestCond(host, cond_addr, true);
            auto mutex = GetGuestMutex(host, mutex_addr, true);
            if (!condition || !mutex) {
                jit.Regs()[0] = EINVAL;
                return;
            }

            std::chrono::system_clock::time_point realtime_deadline;
            std::chrono::steady_clock::time_point monotonic_deadline;
            if (timed) {
                const uint32_t abstime = jit.Regs()[2];
                if (!abstime) { jit.Regs()[0] = EINVAL; return; }
                const int32_t sec = static_cast<int32_t>(host.MemoryRead32(abstime));
                const int32_t nsec = static_cast<int32_t>(host.MemoryRead32(abstime + 4));
                if (nsec < 0 || nsec >= 1000000000) { jit.Regs()[0] = EINVAL; return; }
                const auto target = std::chrono::seconds(sec) +
                                    std::chrono::nanoseconds(nsec);
                if (condition->monotonic) {
                    timespec now{};
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    const auto current = std::chrono::seconds(now.tv_sec) +
                                         std::chrono::nanoseconds(now.tv_nsec);
                    monotonic_deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            target - current);
                } else {
                    realtime_deadline = std::chrono::system_clock::time_point(
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                            target));
                }
            }

            const uint32_t owner = CurrentGuestThreadId();
            std::unique_lock<std::mutex> cond_lock(condition->mutex);
            if (condition->destroyed) { jit.Regs()[0] = EINVAL; return; }
            const uint64_t broadcast_generation = condition->broadcast_generation;
            const uint64_t ticket = condition->next_ticket++;
            condition->waiting_tickets.push_back(ticket);
            ++condition->waiters;
            uint32_t depth = 0;
            int result = UnlockGuestMutex(mutex, owner, &depth);
            if (result != 0) {
                auto ticket_it = std::find(condition->waiting_tickets.begin(),
                                           condition->waiting_tickets.end(), ticket);
                if (ticket_it != condition->waiting_tickets.end()) condition->waiting_tickets.erase(ticket_it);
                --condition->waiters;
                jit.Regs()[0] = result;
                return;
            }
            bool signaled = true;
            if (timed) {
                auto predicate = [&] {
                    return condition->destroyed ||
                           condition->signaled_tickets.count(ticket) != 0 ||
                           condition->broadcast_generation != broadcast_generation;
                };
                signaled = condition->monotonic
                    ? condition->changed.wait_until(cond_lock, monotonic_deadline, predicate)
                    : condition->changed.wait_until(cond_lock, realtime_deadline, predicate);
            } else {
                condition->changed.wait(cond_lock, [&] {
                    return condition->destroyed || condition->signaled_tickets.count(ticket) != 0 ||
                           condition->broadcast_generation != broadcast_generation;
                });
            }
            const bool destroyed = condition->destroyed;
            condition->signaled_tickets.erase(ticket);
            auto ticket_it = std::find(condition->waiting_tickets.begin(),
                                       condition->waiting_tickets.end(), ticket);
            if (ticket_it != condition->waiting_tickets.end()) condition->waiting_tickets.erase(ticket_it);
            --condition->waiters;
            cond_lock.unlock();
            int relock = LockGuestMutex(mutex, owner, false, depth);
            if (relock != 0) result = relock;
            else if (destroyed) result = EINVAL;
            else if (!signaled) result = ETIMEDOUT;
            else result = 0;
            jit.Regs()[0] = result;
        };
    };
    RegisterThunk("pthread_cond_wait", cond_wait_handler(false));
    RegisterThunk("pthread_cond_timedwait", cond_wait_handler(true));
    const auto absolute_timed_wait = cond_wait_handler(true);
    RegisterThunk("pthread_cond_timedwait_relative_np",
        [absolute_timed_wait](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            const uint32_t relative_address = jit.Regs()[2];
            if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                           relative_address, 8)) {
                jit.Regs()[0] = EINVAL;
                return;
            }
            const int32_t relative_seconds =
                static_cast<int32_t>(host.MemoryRead32(relative_address));
            const int32_t relative_nanoseconds =
                static_cast<int32_t>(host.MemoryRead32(relative_address + 4));
            if (relative_seconds < 0 || relative_nanoseconds < 0 ||
                relative_nanoseconds >= 1000000000) {
                jit.Regs()[0] = EINVAL;
                return;
            }
            auto condition = GetGuestCond(host, jit.Regs()[0], true);
            if (!condition) { jit.Regs()[0] = EINVAL; return; }
            timespec now{};
            const clockid_t clock_id = condition->monotonic
                ? CLOCK_MONOTONIC : CLOCK_REALTIME;
            if (::clock_gettime(clock_id, &now) != 0) {
                const int error = errno;
                SetGuestErrno(host, error);
                jit.Regs()[0] = error;
                return;
            }
            int64_t seconds = static_cast<int64_t>(now.tv_sec) + relative_seconds;
            int64_t nanoseconds = static_cast<int64_t>(now.tv_nsec) +
                                  relative_nanoseconds;
            seconds += nanoseconds / 1000000000;
            nanoseconds %= 1000000000;
            if (seconds > INT32_MAX) { jit.Regs()[0] = EOVERFLOW; return; }

            const uint32_t absolute_address = host.GuestAlloc(8, 4);
            if (!absolute_address) { jit.Regs()[0] = ENOMEM; return; }
            host.MemoryWrite32(absolute_address, static_cast<uint32_t>(seconds));
            host.MemoryWrite32(absolute_address + 4,
                               static_cast<uint32_t>(nanoseconds));
            jit.Regs()[2] = absolute_address;
            absolute_timed_wait(jit, host);
            host.GuestFree(absolute_address);
        });
    RegisterThunk("pthread_join", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t id = jit.Regs()[0];
        const uint32_t result_addr = jit.Regs()[1];
        if (!id || id == CurrentGuestThreadId()) { jit.Regs()[0] = id ? EDEADLK : ESRCH; return; }
        std::shared_ptr<GuestThreadRecord> record;
        {
            std::lock_guard<std::mutex> map_lock(g_guest_threads_mutex);
            auto it = g_guest_threads.find(id);
            if (it == g_guest_threads.end()) { jit.Regs()[0] = ESRCH; return; }
            record = it->second;
        }
        uint32_t result = 0;
        {
            std::unique_lock<std::mutex> lock(record->mutex);
            if (record->detached || record->join_claimed) { jit.Regs()[0] = EINVAL; return; }
            record->join_claimed = true;
            record->completed.wait(lock, [&] { return record->done; });
            result = record->result;
        }
        {
            std::lock_guard<std::mutex> map_lock(g_guest_threads_mutex);
            auto it = g_guest_threads.find(id);
            if (it != g_guest_threads.end() && it->second == record) g_guest_threads.erase(it);
        }
        if (result_addr) host.MemoryWrite32(result_addr, result);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_detach", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const uint32_t id = jit.Regs()[0];
        std::shared_ptr<GuestThreadRecord> record;
        {
            std::lock_guard<std::mutex> map_lock(g_guest_threads_mutex);
            auto it = g_guest_threads.find(id);
            if (it == g_guest_threads.end()) { jit.Regs()[0] = ESRCH; return; }
            record = it->second;
        }
        bool erase = false;
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            if (record->detached || record->join_claimed) { jit.Regs()[0] = EINVAL; return; }
            record->detached = true;
            erase = record->done;
        }
        if (erase) {
            std::lock_guard<std::mutex> map_lock(g_guest_threads_mutex);
            auto it = g_guest_threads.find(id);
            if (it != g_guest_threads.end() && it->second == record) g_guest_threads.erase(it);
        }
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_exit", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        g_thread_exit_requested = true;
        g_thread_exit_value = jit.Regs()[0];
        LOGI("pthread_exit from guest thread 0x%08X, result=0x%08X", CurrentGuestThreadId(), g_thread_exit_value);
        // Keep orderly pthread termination distinct from all error halts. The
        // host clears this dedicated bit before a later TLS-destructor entry.
        host.HaltCurrentThreadExecution(Dynarmic::HaltReason::UserDefined2);
    });
    RegisterThunk("pthread_mutexattr_destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_pthread_attr_mutex);
        jit.Regs()[0] = g_pthread_mutex_attrs.erase(jit.Regs()[0]) ? 0 : EINVAL;
    });
    RegisterThunk("sched_yield", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        // A guest spin-wait (appSleep(0) while another thread loads) is idle
        // time too: translate a few learned blocks of this thread's role first.
        // Returns at once for other threads and once the role's list is done.
        // Not while the player is in the world: the game and render threads
        // also yield inside every frame there (Redmi Note 8 Pro, whose lists
        // last for minutes: 34.3 instead of 32.3 ms per frame in one scene).
        // debug.bl2.yield_warmup=0 before launch turns it off (Pixel 10a warm
        // start without movies: main menu 1.2 s later without it).
        constexpr uint64_t kYieldWarmupNs = 100'000;
        static const bool yield_warmup = [] {
            char value[PROP_VALUE_MAX] = {};
            return !(__system_property_get("debug.bl2.yield_warmup", value) > 0 && value[0] == '0');
        }();
        if (yield_warmup && !Bl2TouchPlayerInWorld()) host.WarmLearnedBlocksInCallback(jit, kYieldWarmupNs);
        std::this_thread::yield();
        jit.Regs()[0] = 0;
    });

    RegisterThunk("pthread_once", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t control_addr = jit.Regs()[0];
        uint32_t init_routine = jit.Regs()[1];
        if (!control_addr || !init_routine) { jit.Regs()[0] = EINVAL; return; }
        std::shared_ptr<GuestOnceState> once;
        {
            std::lock_guard<std::mutex> map_lock(g_sync_state_mutex);
            auto& slot = g_guest_once_states[control_addr];
            if (!slot) slot = std::make_shared<GuestOnceState>();
            once = slot;
        }
        const uint32_t owner = CurrentGuestThreadId();
        {
            std::unique_lock<std::mutex> lock(once->mutex);
            while (once->running && once->owner != owner) once->completed.wait(lock);
            if (once->done || host.MemoryRead32(control_addr) != 0) {
                once->done = true;
                jit.Regs()[0] = 0;
                return;
            }
            if (once->running && once->owner == owner) {
                LOGE("pthread_once: recursive initialization for control 0x%08X", control_addr);
                jit.Regs()[0] = EDEADLK;
                return;
            }
            once->running = true;
            once->owner = owner;
        }
        host.RunGuestCallback(init_routine, {});
        if (g_thread_exit_requested) {
            {
                std::lock_guard<std::mutex> lock(once->mutex);
                once->running = false;
                once->owner = 0;
                once->done = false;
                host.MemoryWrite32(control_addr, 0);
            }
            once->completed.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(once->mutex);
            once->running = false;
            once->owner = 0;
            once->done = true;
            host.MemoryWrite32(control_addr, 1);
        }
        once->completed.notify_all();
        jit.Regs()[0] = 0;
    });

    RegisterThunk("pthread_key_create", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t key_ptr = jit.Regs()[0];
        if (!key_ptr) { jit.Regs()[0] = EINVAL; return; }
        uint32_t key = g_next_tls_key.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(g_tls_key_mutex);
            g_tls_keys.insert(key);
            g_tls_destructors[key] = jit.Regs()[1];
        }
        host.MemoryWrite32(key_ptr, key);
        jit.Regs()[0] = 0;
    });
    RegisterThunk("pthread_key_delete", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        std::lock_guard<std::mutex> lock(g_tls_key_mutex);
        if (!g_tls_keys.erase(jit.Regs()[0])) { jit.Regs()[0] = EINVAL; return; }
        g_tls_destructors.erase(jit.Regs()[0]);
        g_tls_values.erase(jit.Regs()[0]);
        jit.Regs()[0] = 0;
    });

    RegisterThunk("pthread_setspecific", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t key = jit.Regs()[0];
        uint32_t val = jit.Regs()[1];
        {
            std::lock_guard<std::mutex> lock(g_tls_key_mutex);
            if (!g_tls_keys.count(key)) { jit.Regs()[0] = EINVAL; return; }
        }
        g_tls_values[key] = val;
        jit.Regs()[0] = 0;
    });

    RegisterThunk("pthread_getspecific", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t key = jit.Regs()[0];
        auto it = g_tls_values.find(key);
        jit.Regs()[0] = it != g_tls_values.end() ? it->second : 0;
    });

    RegisterThunk("fopen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* mode = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (!path) { jit.Regs()[0] = 0; return; }

        uint32_t trace_index = 0;
        const bool trace = ShouldTraceFileCall(
            g_fopen_trace_count, 64, "fopen", path, &trace_index);

        ApkEntry* entry = FindApkEntry(path);
        const bool embedded_obb = entry &&
            (OriginalObbIndex(path) >= 0 || IsBl2LocalePath(path));
        const char* requested_mode = mode ? mode : "r";
        const bool read_only = requested_mode[0] == 'r' &&
                               strchr(requested_mode, '+') == nullptr;
        FILE* f = nullptr;
        bool used_apk_stream = false;
        if (embedded_obb) {
            if (read_only) {
                f = OpenApkEntryStream(*entry);
                used_apk_stream = f != nullptr;
            }
            else errno = EROFS;
        } else {
            const bool legacy_command_line = read_only &&
                IsLegacyCommandLinePath(path);
            if (legacy_command_line) {
                // Movies play by default again (user request 2026-10-11; the
                // 2026-09-25 default skip is now the QA opt-in
                // debug.horn.play_movies=0). Without any QA flag the engine
                // reads the ordinary command line below, as the original game.
                char play_movies[PROP_VALUE_MAX] = {};
                const bool skip_movies =
                    __system_property_get("debug.horn.play_movies", play_movies) > 0 &&
                    play_movies[0] == '0';
                char no_sound[PROP_VALUE_MAX] = {};
                const bool no_sound_enabled =
                    __system_property_get("debug.bl2.no_sound", no_sound) > 0 &&
                    std::strcmp(no_sound, "1") == 0;
                char low_quality[PROP_VALUE_MAX] = {};
                const bool low_quality_enabled =
                    __system_property_get("debug.bl2.low_quality", low_quality) > 0 &&
                    low_quality[0] == '1' && low_quality[1] == '\0';
                // Index: skip movies 4, low quality 2, no sound 1. ParseParam
                // recognizes a space after its final argument.
                static char kQaCommandLines[8][80] = {
                    "",
                    "-nosound \n",
                    "-SystemSettings=SystemSettingsMobile\n",
                    "-SystemSettings=SystemSettingsMobile -nosound \n",
                    "-nomovies -nostartupmovies\n",
                    "-nomovies -nostartupmovies -nosound \n",
                    "-nomovies -nostartupmovies -SystemSettings=SystemSettingsMobile\n",
                    "-nomovies -nostartupmovies -SystemSettings=SystemSettingsMobile -nosound \n",
                };
                const unsigned qa_variant = (skip_movies ? 4u : 0u) |
                    (low_quality_enabled ? 2u : 0u) | (no_sound_enabled ? 1u : 0u);
                if (qa_variant) {
                    char* flags = kQaCommandLines[qa_variant];
                    f = fmemopen(flags, strlen(flags), "r");
                    if (f && no_sound_enabled) {
                        static std::atomic<bool> logged{false};
                        if (!logged.exchange(true, std::memory_order_relaxed)) {
                            __android_log_print(ANDROID_LOG_INFO, "BL2_Parity",
                                "ARM64 noSound command selected low_quality=%d",
                                low_quality_enabled);
                        }
                    }
                    if (f) LOGI("fopen: arm64 QA command line skip_movies=%d "
                                "low_quality=%d no_sound=%d", skip_movies,
                                low_quality_enabled, no_sound_enabled);
                }
                if (!f && !g_saved_files_dir.empty()) {
                    const std::string app_command_line = g_saved_files_dir +
                        "/WillowGame/CookedAndroid_DXT/UE3CommandLine.txt";
                    f = fopen(app_command_line.c_str(), requested_mode);
                    if (f) {
                        LOGI("fopen: redirected legacy command line '%s' to '%s'",
                             path, app_command_line.c_str());
                    } else {
                        LOGI("fopen: app command line '%s' absent, errno=%d",
                             app_command_line.c_str(), errno);
                    }
                }
            } else if (path && strstr(path, "UE3CommandLine.txt")) {
                LOGE("fopen: legacy command line redirect unavailable: match=%d saved_dir=%d",
                     legacy_command_line ? 1 : 0,
                     g_saved_files_dir.empty() ? 0 : 1);
            }
            if (!f) f = fopen(path, requested_mode);
            if (!f && entry && read_only) {
                f = OpenApkEntryStream(*entry);
                used_apk_stream = f != nullptr;
            }
        }
        if (used_apk_stream) {
            NoteBl2LocaleOpen(path, "fopen");
            LOGI("fopen: APK direct stream opened '%s' -> '%s' (size %lld, offset %lld)",
                 path, entry->name, static_cast<long long>(entry->file_size),
                 static_cast<long long>(entry->data_offset));
        }

        const int result_errno = f ? 0 : errno;
        if (trace) {
            LOGI("fopen trace #%u path='%s' mode='%s' result=%p errno=%d",
                 trace_index, path, mode ? mode : "r", f, result_errno);
        }

        if (f) {
            std::lock_guard<std::mutex> lock(g_file_mutex);
            const uint32_t handle = g_next_file_handle++;
            g_file_map.emplace(handle, f);
            jit.Regs()[0] = handle;
        } else {
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("fdopen", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        const char* mode = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        FILE* f = fdopen(fd, mode);
        if (f) {
            std::lock_guard<std::mutex> lock(g_file_mutex);
            const uint32_t handle = g_next_file_handle++;
            g_file_map.emplace(handle, f);
            jit.Regs()[0] = handle;
        } else {
            jit.Regs()[0] = 0;
        }
    });

    RegisterThunk("fclose", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        HornPerfScope perf(HornPerfCategory::FileSync);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        if (it != g_file_map.end()) {
            fclose(it->second);
            g_file_map.erase(it);
            jit.Regs()[0] = 0;
        } else {
            jit.Regs()[0] = -1;
        }
    });

    RegisterThunk("fread", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        void* ptr = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        size_t size = jit.Regs()[1];
        size_t count = jit.Regs()[2];
        uint32_t handle = jit.Regs()[3];
        if (GuestLowGuardHit(jit.Regs()[0], size * count)) {
            host.ReportLowGuardWrite(jit.Regs()[0], size * count);
            ptr = nullptr;
        }
        HornPerfScope perf(HornPerfCategory::FileRead);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        size_t res = (it != g_file_map.end() && ptr) ? fread(ptr, size, count, it->second) : 0;
        if (res > 0 && size > 0) {
            perf.SetBytes(static_cast<uint64_t>(res) * size);
        }
        jit.Regs()[0] = res;
    });

    RegisterThunk("fgets", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t buffer_address = jit.Regs()[0];
        const int capacity = static_cast<int32_t>(jit.Regs()[1]);
        const uint32_t handle = jit.Regs()[2];
        if (capacity <= 0) { jit.Regs()[0] = 0; return; }
        char* buffer = static_cast<char*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(), buffer_address,
            static_cast<size_t>(capacity)));
        if (!buffer) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        std::lock_guard<std::mutex> lock(g_file_mutex);
        FILE* file = GuestFile(handle);
        if (!file) {
            SetGuestErrno(host, EBADF);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = ::fgets(buffer, capacity, file) ? buffer_address : 0;
    });

    RegisterThunk("fwrite", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const void* ptr = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]);
        size_t size = jit.Regs()[1];
        size_t count = jit.Regs()[2];
        uint32_t handle = jit.Regs()[3];
        HornPerfScope perf(HornPerfCategory::FileWrite);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        const size_t result =
            (it != g_file_map.end() && ptr) ? fwrite(ptr, size, count, it->second) : 0;
        if (result > 0 && size > 0) {
            perf.SetBytes(static_cast<uint64_t>(result) * size);
        }
        jit.Regs()[0] = result;
    });

    RegisterThunk("fseek", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        long offset = static_cast<long>(static_cast<int32_t>(jit.Regs()[1]));
        int whence = static_cast<int>(jit.Regs()[2]);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        int res = (it != g_file_map.end()) ? fseek(it->second, offset, whence) : -1;
        jit.Regs()[0] = res;
    });

    RegisterThunk("fsetpos", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t handle = jit.Regs()[0];
        const uint32_t position_address = jit.Regs()[1];
        if (!GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                       position_address, sizeof(uint64_t))) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        const off_t position = static_cast<off_t>(
            static_cast<int64_t>(host.MemoryRead64(position_address)));
        std::lock_guard<std::mutex> lock(g_file_mutex);
        FILE* file = GuestFile(handle);
        const int result = file ? ::fseeko(file, position, SEEK_SET) : -1;
        if (!file) SetGuestErrno(host, EBADF);
        else if (result != 0) SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("rewind", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        FILE* file = GuestFile(jit.Regs()[0]);
        if (file) ::rewind(file);
        else SetGuestErrno(host, EBADF);
    });
    RegisterThunk("getchar", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(std::getchar());
    });
    RegisterThunk("putchar", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = static_cast<uint32_t>(std::putchar(
            static_cast<int>(jit.Regs()[0])));
    });

    RegisterThunk("fseeko", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        off_t offset = static_cast<off_t>(static_cast<int32_t>(jit.Regs()[1]));
        int whence = static_cast<int>(jit.Regs()[2]);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        int res = (it != g_file_map.end()) ? fseeko(it->second, offset, whence) : -1;
        jit.Regs()[0] = res;
    });

    RegisterThunk("ftell", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? static_cast<uint32_t>(ftell(it->second)) : 0;
    });

    RegisterThunk("ftello", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        off_t res = (it != g_file_map.end()) ? ftello(it->second) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("fileno", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? fileno(it->second) : -1;
    });
    RegisterThunk("_fileno", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        FILE* file = GuestFile(jit.Regs()[0]);
        if (!file) { SetGuestErrno(host, EBADF); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        int result = fileno(file);
        if (result < 0) SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("_fgetwln", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        FILE* file = GuestFile(jit.Regs()[0]);
        if (!file) { SetGuestErrno(host, EBADF); jit.Regs()[0] = 0; return; }
        wchar_t line[4096];
        if (!fgetws(line, static_cast<int>(std::size(line)), file)) { jit.Regs()[0] = 0; return; }
        const size_t length = wcslen(line);
        uint32_t& buffer = g_fgetwln_buffers[jit.Regs()[0]];
        if (buffer) host.GuestFree(buffer);
        buffer = host.GuestAlloc((length + 1) * sizeof(wchar_t), alignof(wchar_t));
        if (!buffer) { SetGuestErrno(host, ENOMEM); jit.Regs()[0] = 0; return; }
        memcpy(GuestToHost(host.GetGuestMemoryBase(), buffer), line, (length + 1) * sizeof(wchar_t));
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(length));
        jit.Regs()[0] = buffer;
    });
    RegisterThunk("_fparseln", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        FILE* file = GuestFile(jit.Regs()[0]);
        if (!file) { SetGuestErrno(host, EBADF); jit.Regs()[0] = 0; return; }
        const char* delimiters = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        const char escape = delimiters ? delimiters[0] : '\\';
        const char continuation = delimiters ? delimiters[1] : '\\';
        const char comment = delimiters ? delimiters[2] : '#';
        std::string parsed;
        char line[4096];
        bool continued;
        do {
            if (!fgets(line, sizeof(line), file)) break;
            if (jit.Regs()[2]) host.MemoryWrite32(jit.Regs()[2], host.MemoryRead32(jit.Regs()[2]) + 1);
            std::string part(line);
            if (!part.empty() && part.back() == '\n') part.pop_back();
            continued = !part.empty() && part.back() == continuation;
            if (continued) part.pop_back();
            bool escaped = false;
            for (char c : part) {
                if (!escaped && c == comment) break;
                if (!escaped && c == escape) { escaped = true; continue; }
                parsed.push_back(c);
                escaped = false;
            }
        } while (continued);
        if (parsed.empty() && feof(file)) { jit.Regs()[0] = 0; return; }
        uint32_t out = host.GuestAlloc(parsed.size() + 1, 1);
        if (!out) { SetGuestErrno(host, ENOMEM); jit.Regs()[0] = 0; return; }
        memcpy(GuestToHost(host.GetGuestMemoryBase(), out), parsed.c_str(), parsed.size() + 1);
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], static_cast<uint32_t>(parsed.size()));
        jit.Regs()[0] = out;
    });

    RegisterThunk("clearerr", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        if (it != g_file_map.end()) clearerr(it->second);
    });

    RegisterThunk("ferror", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? ferror(it->second) : 0;
    });

    RegisterThunk("open", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int flags = static_cast<int>(jit.Regs()[1]);
        int mode = static_cast<int>(jit.Regs()[2]);
        uint32_t trace_index = 0;
        const bool trace = ShouldTraceFileCall(
            g_open_trace_count, 96, "open", path, &trace_index);

        int fd = -1;
        const bool embedded_obb = (g_verified_obb_pair &&
                                  OriginalObbIndex(path) >= 0) || IsBl2LocalePath(path);
        if (embedded_obb &&
            (flags & (O_CREAT | O_WRONLY | O_RDWR | O_TRUNC | O_APPEND)) != 0) {
            SetGuestErrno(host, EROFS);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }
        if (path && (flags & O_CREAT) == 0 && (flags & O_WRONLY) == 0 && (flags & O_RDWR) == 0) {
            ApkEntry* entry = FindApkEntry(path);
            if (entry && entry->apk_fd >= 0) {
                int dup_fd = dup(entry->apk_fd);
                if (dup_fd >= 0) {
                    std::lock_guard<std::mutex> lock(g_vfd_mutex);
                    int vfd = g_next_vfd++;
                    g_vfd_map[vfd] = { dup_fd, static_cast<size_t>(entry->file_size), entry->data_offset, 0, entry->bink };
                    const uint32_t log_index =
                        g_apk_direct_open_log_count.fetch_add(1, std::memory_order_relaxed);
                    if (log_index < 4) {
                        LOGI("open: APK direct stream opened '%s' -> '%s' (vfd %d, size %zu, offset %lld)",
                             path, entry->name, vfd, entry->file_size,
                             static_cast<long long>(entry->data_offset));
                    } else if (log_index == 4) {
                        LOGI("open: suppressing further APK direct-stream success logs");
                    }
                    if (trace) {
                        LOGI("open trace #%u path='%s' flags=0x%X mode=0%o "
                             "result=vfd:%d errno=0",
                             trace_index, path, flags, mode, vfd);
                    }
                    NoteBl2LocaleOpen(path, "open");
                    Bl2ObbPackageWatchOpen(vfd, OriginalObbIndex(path), dup_fd, entry->data_offset);
                    jit.Regs()[0] = static_cast<uint32_t>(vfd);
                    return;
                }
            }
        }

        if (embedded_obb) {
            SetGuestErrno(host, errno ? errno : EBADF);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }

        fd = open(path ? path : "", flags, mode);
        if (fd >= 0 && (flags & (O_CREAT | O_WRONLY | O_RDWR | O_TRUNC | O_APPEND)) == 0) {
            AuditUserConfig(fd, path);
            Bl2ObbPackageWatchOpen(fd, OriginalObbIndex(path), fd, 0);
        }
        const int result_errno = fd >= 0 ? 0 : errno;
        if (trace) {
            LOGI("open trace #%u path='%s' flags=0x%X mode=0%o result=%d errno=%d",
                 trace_index, path ? path : "(null)", flags, mode, fd,
                 result_errno);
        }
        jit.Regs()[0] = (fd >= 0) ? static_cast<uint32_t>(fd) : static_cast<uint32_t>(-1);
    });

    RegisterThunk("close", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        HornPerfScope perf(HornPerfCategory::FileSync);
        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                Bl2ObbPackageWatchClose(fd);
                close(it->second.real_apk_fd);
                g_vfd_map.erase(it);
                jit.Regs()[0] = 0;
                return;
            }
        }
        Bl2ObbPackageWatchClose(fd);
        NetworkResult(jit, host, ::close(fd));
    });

    RegisterThunk("read", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        void* buf = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                             jit.Regs()[1], jit.Regs()[2]);
        size_t count = jit.Regs()[2];
        HornPerfScope perf(HornPerfCategory::FileRead);
        if (!buf) { NetworkFailure(jit, host, EFAULT); return; }

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                VirtualFd& vfd = it->second;
                if (vfd.current_pos >= static_cast<off_t>(vfd.file_size)) {
                    jit.Regs()[0] = 0; // EOF
                    return;
                }
                size_t avail = vfd.file_size - vfd.current_pos;
                size_t to_read = (count < avail) ? count : avail;
                Bl2ObbPackageWatchRead(fd, static_cast<uint64_t>(vfd.current_pos));
                ssize_t nread = vfd.bink ? vfd.bink->Read(buf, to_read, vfd.current_pos)
                                       : pread(vfd.real_apk_fd, buf, to_read, vfd.file_offset + vfd.current_pos);
                const int error = errno;
                if (nread > 0) {
                    vfd.current_pos += nread;
                    perf.SetBytes(static_cast<uint64_t>(nread));
                }
                if (nread < 0) SetGuestErrno(host, error);
                jit.Regs()[0] = static_cast<uint32_t>(nread);
                return;
            }
        }

        if (Bl2ObbPackageWatched(fd)) {
            const off_t position = ::lseek(fd, 0, SEEK_CUR);
            if (position >= 0) Bl2ObbPackageWatchRead(fd, static_cast<uint64_t>(position));
        }
        ssize_t res = ::read(fd, buf, count);
        const int error = errno;
        if (res > 0) perf.SetBytes(static_cast<uint64_t>(res));
        if (res < 0) SetGuestErrno(host, error);
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("write", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        const void* buf = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                   jit.Regs()[1], jit.Regs()[2]);
        size_t count = jit.Regs()[2];
        HornPerfScope perf(HornPerfCategory::FileWrite);
        if (!buf) { NetworkFailure(jit, host, EFAULT); return; }
        ssize_t res = ::write(fd, buf, count);
        const int error = errno;
        if (res > 0) perf.SetBytes(static_cast<uint64_t>(res));
        if (res < 0) SetGuestErrno(host, error);
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("lseek", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        off_t offset = static_cast<off_t>(static_cast<int32_t>(jit.Regs()[1]));
        int whence = static_cast<int>(jit.Regs()[2]);

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                VirtualFd& vfd = it->second;
                off_t new_pos = vfd.current_pos;
                if (whence == SEEK_SET) new_pos = offset;
                else if (whence == SEEK_CUR) new_pos += offset;
                else if (whence == SEEK_END) new_pos = static_cast<off_t>(vfd.file_size) + offset;
                else { jit.Regs()[0] = static_cast<uint32_t>(-1); return; }

                if (new_pos < 0) { jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
                vfd.current_pos = new_pos;
                jit.Regs()[0] = static_cast<uint32_t>(new_pos);
                return;
            }
        }

        off_t res = (fd >= 0) ? lseek(fd, offset, whence) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res);
    });

    RegisterThunk("lseek64", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        int64_t offset = static_cast<int64_t>(jit.Regs()[2]) | (static_cast<int64_t>(jit.Regs()[3]) << 32);
        int whence = static_cast<int>(host.MemoryRead32(jit.Regs()[13]));

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                VirtualFd& vfd = it->second;
                off_t new_pos = vfd.current_pos;
                if (whence == SEEK_SET) new_pos = offset;
                else if (whence == SEEK_CUR) new_pos += offset;
                else if (whence == SEEK_END) new_pos = static_cast<off_t>(vfd.file_size) + offset;
                else { jit.Regs()[0] = static_cast<uint32_t>(-1); jit.Regs()[1] = static_cast<uint32_t>(-1); return; }

                if (new_pos < 0) { jit.Regs()[0] = static_cast<uint32_t>(-1); jit.Regs()[1] = static_cast<uint32_t>(-1); return; }
                vfd.current_pos = new_pos;
                jit.Regs()[0] = static_cast<uint32_t>(new_pos & 0xFFFFFFFF);
                jit.Regs()[1] = static_cast<uint32_t>(new_pos >> 32);
                return;
            }
        }

        off64_t res = (fd >= 0) ? lseek64(fd, offset, whence) : -1;
        jit.Regs()[0] = static_cast<uint32_t>(res & 0xFFFFFFFF);
        jit.Regs()[1] = static_cast<uint32_t>(res >> 32);
    });

    RegisterThunk("access", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int mode = static_cast<int>(jit.Regs()[1]);
        if ((g_verified_obb_pair && OriginalObbIndex(path) >= 0) || IsBl2LocalePath(path)) {
            if ((mode & W_OK) != 0 || (mode & X_OK) != 0) {
                SetGuestErrno(host, EACCES);
                jit.Regs()[0] = static_cast<uint32_t>(-1);
            } else {
                jit.Regs()[0] = 0;
            }
            return;
        }
        int res = access(path ? path : "", mode);
        if (res != 0 && FindApkEntry(path)) {
            res = 0;
        }
        jit.Regs()[0] = res;
    });
    auto deny_filesystem_mutation = [](const char* operation) {
        return [operation](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
            LOGE("%s: denied by bridge filesystem policy for '%s'", operation, path ? path : "(null)");
            SetGuestErrno(host, EPERM);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
        };
    };
    RegisterThunk("chdir", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        if (!path) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = static_cast<uint32_t>(-1);
            return;
        }

        // Prefer the requested directory when the host can access it. If
        // scoped storage rejects the old /storage/emulated/0/... path, use
        // Context.getFilesDir(), captured before the guest is started.
        if (::chdir(path) == 0) {
            jit.Regs()[0] = 0;
            return;
        }
        const int requested_errno = errno;
        if (!g_saved_files_dir.empty() && ::chdir(g_saved_files_dir.c_str()) == 0) {
            LOGI("chdir: redirected legacy path '%s' to app files '%s'", path, g_saved_files_dir.c_str());
            jit.Regs()[0] = 0;
            return;
        }

        LOGE("chdir: unable to use '%s' or app files '%s'", path,
             g_saved_files_dir.empty() ? "(unavailable)" : g_saved_files_dir.c_str());
        SetGuestErrno(host, requested_errno ? requested_errno : errno);
        jit.Regs()[0] = static_cast<uint32_t>(-1);
    });
    RegisterThunk("mkdir", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        if (IsLegacyConfigDirectory(path) && !g_saved_files_dir.empty()) {
            const std::string app_config_dir = g_saved_files_dir + "/Config";
            const int result = ::mkdir(app_config_dir.c_str(),
                                       static_cast<mode_t>(jit.Regs()[1] & 0777u));
            const int result_errno = result == 0 ? 0 : errno;
            LOGI("mkdir: redirected legacy config directory '%s' to '%s' result=%d errno=%d",
                 path, app_config_dir.c_str(), result, result_errno);
            if (result != 0) SetGuestErrno(host, result_errno);
            jit.Regs()[0] = static_cast<uint32_t>(result);
            return;
        }
        LOGE("mkdir: denied by bridge filesystem policy for '%s'", path ? path : "(null)");
        SetGuestErrno(host, path ? EPERM : EFAULT);
        jit.Regs()[0] = static_cast<uint32_t>(-1);
    });
    RegisterThunk("rmdir", deny_filesystem_mutation("rmdir"));
    RegisterThunk("utimes", deny_filesystem_mutation("utimes"));

    RegisterThunk("opendir", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const bool include_embedded_obbs = g_verified_obb_pair && IsOriginalObbDirectory(path);
        std::shared_ptr<const Bl2DirectorySnapshot> snapshot = include_embedded_obbs
            ? nullptr : Bl2GetDirectorySnapshot(path);
        DIR* dir = path && !snapshot ? opendir(path) : nullptr;
        if (!dir && !snapshot && !include_embedded_obbs) {
            SetGuestErrno(host, path ? errno : EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        uint32_t dirent_addr = host.GuestAlloc(sizeof(Arm32Dirent), 8);
        if (!dirent_addr) {
            if (dir) closedir(dir);
            SetGuestErrno(host, ENOMEM);
            jit.Regs()[0] = 0;
            return;
        }
        std::lock_guard<std::mutex> lock(g_dir_mutex);
        uint32_t handle = g_next_dir_handle++;
        g_dir_map.emplace(handle, GuestDirHandle{
            dir, dirent_addr, include_embedded_obbs, 0, std::move(snapshot), 0});
        if (include_embedded_obbs) {
            LOGI("opendir: verified embedded OBB names virtualized for '%s' (native_dir=%d)",
                 path, dir != nullptr);
        }
        jit.Regs()[0] = handle;
    });
    RegisterThunk("readdir", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_dir_mutex);
        auto it = g_dir_map.find(jit.Regs()[0]);
        if (it == g_dir_map.end()) { SetGuestErrno(host, EBADF); jit.Regs()[0] = 0; return; }
        GuestDirHandle& handle = it->second;
        if (handle.snapshot) {
            Bl2DirectoryCacheRecordRead(true);
            errno = 0;
            if (handle.next_snapshot_entry == handle.snapshot->entries.size()) {
                jit.Regs()[0] = 0;
                return;
            }
            const Bl2DirectoryEntry& entry = handle.snapshot->entries[handle.next_snapshot_entry++];
            Arm32Dirent out{};
            out.d_ino = entry.inode;
            out.d_off = entry.offset;
            out.d_type = entry.type;
            memcpy(out.d_name, entry.name, sizeof(out.d_name));
            out.d_reclen = static_cast<uint16_t>(offsetof(Arm32Dirent, d_name) + strlen(out.d_name) + 1);
            memcpy(GuestToHost(host.GetGuestMemoryBase(), handle.guest_dirent), &out, sizeof(out));
            jit.Regs()[0] = handle.guest_dirent;
            return;
        }
        if (!handle.include_embedded_obbs) Bl2DirectoryCacheRecordRead(false);
        if (handle.include_embedded_obbs && handle.next_embedded_obb < 2) {
            const std::string_view name = handle.next_embedded_obb == 0 ?
                kMainObbName : kPatchObbName;
            Arm32Dirent out{};
            out.d_ino = 0xB1200001u + handle.next_embedded_obb;
            out.d_off = handle.next_embedded_obb + 1;
            out.d_type = DT_REG;
            memcpy(out.d_name, name.data(), name.size());
            out.d_reclen = static_cast<uint16_t>(offsetof(Arm32Dirent, d_name) + name.size() + 1);
            ++handle.next_embedded_obb;
            memcpy(GuestToHost(host.GetGuestMemoryBase(), handle.guest_dirent), &out, sizeof(out));
            jit.Regs()[0] = handle.guest_dirent;
            return;
        }
        errno = 0;
        dirent* native = handle.dir ? readdir(handle.dir) : nullptr;
        while (native && handle.include_embedded_obbs && OriginalObbIndex(native->d_name) >= 0) {
            native = readdir(handle.dir);
        }
        if (!native) { if (errno) SetGuestErrno(host, errno); jit.Regs()[0] = 0; return; }
        Arm32Dirent out{};
        out.d_ino = native->d_ino;
        out.d_off = native->d_off;
        out.d_type = native->d_type;
        strncpy(out.d_name, native->d_name, sizeof(out.d_name) - 1);
        out.d_reclen = static_cast<uint16_t>(offsetof(Arm32Dirent, d_name) + strlen(out.d_name) + 1);
        memcpy(GuestToHost(host.GetGuestMemoryBase(), handle.guest_dirent), &out, sizeof(out));
        jit.Regs()[0] = handle.guest_dirent;
    });
    RegisterThunk("closedir", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(g_dir_mutex);
        auto it = g_dir_map.find(jit.Regs()[0]);
        if (it == g_dir_map.end()) { SetGuestErrno(host, EBADF); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        int result = it->second.dir ? closedir(it->second.dir) : 0;
        const int close_errno = errno;
        if (!it->second.include_embedded_obbs) Bl2DirectoryCacheRecordClose();
        host.GuestFree(it->second.guest_dirent);
        g_dir_map.erase(it);
        if (result < 0) SetGuestErrno(host, close_errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });

    RegisterThunk("stat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        uint32_t buf_gaddr = jit.Regs()[1];
        struct stat st{};
        int res = EmbeddedObbStat(path, st) ? 0 : stat(path ? path : "", &st);
        if (res != 0) {
            ApkEntry* entry = FindApkEntry(path);
            if (entry) {
                memset(&st, 0, sizeof(st));
                st.st_size = entry->file_size;
                st.st_mode = S_IFREG | 0644;
                st.st_nlink = 1;
                res = 0;
            }
        }
        if (res == 0) {
            WriteArm32Stat(host.GetGuestMemoryBase(), buf_gaddr, st);
        }
        jit.Regs()[0] = res;
    });
    RegisterThunk("lstat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        struct stat native{};
        int result = EmbeddedObbStat(path, native) ? 0 :
                     (path ? lstat(path, &native) : -1);
        if (result == 0) WriteArm32Stat(host.GetGuestMemoryBase(), jit.Regs()[1], native);
        else SetGuestErrno(host, path ? errno : EFAULT);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });

    RegisterThunk("feof", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? feof(it->second) : 1;
    });

    RegisterThunk("fflush", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t handle = jit.Regs()[0];
        HornPerfScope perf(HornPerfCategory::FileSync);
        std::lock_guard<std::mutex> lock(g_file_mutex);
        auto it = g_file_map.find(handle);
        jit.Regs()[0] = (it != g_file_map.end()) ? fflush(it->second) : 0;
    });

    RegisterThunk("setvbuf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = 0;
    });

    // mmap/munmap. Do NOT stub these as "hand out a fresh heap block": a caller that checks
    // the returned address (Boehm does, see lesson 2.15) will abort, and an engine that maps
    // its archive gets 345 MB of zeroes -- which reads as "the pak has no entries", not as an
    // error, and parks the boot on a download screen.
    //
    // The guest arena is one anonymous MAP_PRIVATE reservation, so a file mapping can be laid
    // straight into it with MAP_FIXED and the guest reads it at base + vaddr like any other
    // memory: no copy, no RSS. MAP_PRIVATE, never MAP_SHARED, so a stray guest write cannot
    // reach the file on disk.
    RegisterThunk("mmap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t len   = jit.Regs()[1];
        const uint32_t flags = jit.Regs()[3];
        const uint32_t sp    = jit.Regs()[13];         // AAPCS: args 5 and 6 are on the stack
        int            fd    = static_cast<int>(host.MemoryRead32(sp));
        off_t          off   = static_cast<off_t>(host.MemoryRead32(sp + 4));

        constexpr uint32_t kMapAnonymous = 0x20;       // Linux MAP_ANONYMOUS on arm
        constexpr uint32_t kMapFailed    = 0xFFFFFFFF;
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));

        if (len == 0) { jit.Regs()[0] = kMapFailed; return; }

        // A mapped OBB window names the cooked package being loaded.
        if (fd >= 0 && !(flags & kMapAnonymous)) Bl2ObbPackageWatchRead(fd, static_cast<uint64_t>(off));

        if (fd < 0 || (flags & kMapAnonymous)) {
            // Anonymous: fresh mapping space, which the arena zero-fills for us.
            const uint32_t va = host.GuestAllocMapping((len + page - 1) & ~(page - 1), page);
            jit.Regs()[0] = va ? va : kMapFailed;
            return;
        }

        // A virtual fd is an entry inside the APK: map the real APK descriptor at the entry's
        // offset instead. This is what lets an expansion file ship inside the APK -- the
        // engine fopen()s the external path, the APK fallback answers, and the mmap that
        // follows lands on the same bytes without anything being unpacked.
        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                if (it->second.bink) {
                    if (off < 0 || uint64_t(off) > it->second.file_size ||
                        len > it->second.file_size - uint64_t(off)) {
                        SetGuestErrno(host, EINVAL); jit.Regs()[0] = kMapFailed; return;
                    }
                    // shortcut: mmap materializes the requested movie window; use scatter mappings if whole-movie maps appear.
                    const size_t span = (uint64_t(len) + page - 1) & ~(page - 1);
                    const uint32_t va = host.GuestAllocMapping(span, page);
                    if (!va || uint64_t(va) + span > host.GetGuestMemorySize() ||
                        it->second.bink->Read(host.GetGuestMemoryBase() + va, len, off) != len) {
                        SetGuestErrno(host, EIO); jit.Regs()[0] = kMapFailed; return;
                    }
                    jit.Regs()[0] = va; return;
                }
                struct stat apk_stat{};
                if (fstat(it->second.real_apk_fd, &apk_stat) != 0 || apk_stat.st_size < 0) {
                    SetGuestErrno(host, EBADF);
                    jit.Regs()[0] = kMapFailed;
                    return;
                }
                const auto range = ResolveVirtualFileRange(
                    static_cast<uint64_t>(it->second.file_offset),
                    static_cast<uint64_t>(it->second.file_size),
                    static_cast<uint64_t>(off), static_cast<uint64_t>(len),
                    static_cast<uint64_t>(apk_stat.st_size));
                if (!range.valid) {
                    LOGE("mmap: virtual fd %d range [%lld,+%u) exceeds its APK entry",
                         fd, static_cast<long long>(off), len);
                    SetGuestErrno(host, EINVAL);
                    jit.Regs()[0] = kMapFailed;
                    return;
                }
                off = static_cast<off_t>(range.absolute_offset);
                fd = it->second.real_apk_fd;
            }
        }
        if (fd < 0) { jit.Regs()[0] = kMapFailed; return; }

        // mmap needs a page-aligned file offset; keep the remainder as a bias on the result.
        const off_t  aligned = off & ~static_cast<off_t>(page - 1);
        const size_t bias    = static_cast<size_t>(off - aligned);
        const size_t span    = (len + bias + page - 1) & ~(page - 1);

        struct stat fst{};
        if (fstat(fd, &fst) == 0) {
            std::lock_guard<std::mutex> lock(g_mapping_mutex);
            for (const auto& m : g_guest_file_maps) {
                if (m.dev == fst.st_dev && m.ino == fst.st_ino && m.off == aligned && m.span == span) {
                    jit.Regs()[0] = m.va + static_cast<uint32_t>(bias);
                    return;
                }
            }
        }

        const uint32_t va = host.GuestAllocMapping(span, page);
        if (!va) { LOGE("mmap: no guest arena for %zu bytes", span); jit.Regs()[0] = kMapFailed; return; }
        if (static_cast<uint64_t>(va) + span > host.GetGuestMemorySize()) {
            LOGE("mmap: 0x%08X + %zu would run past the guest arena", va, span);
            jit.Regs()[0] = kMapFailed;
            return;
        }
        void* want = host.GetGuestMemoryBase() + va;
        void* got = ::mmap(want, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, aligned);
        if (got == MAP_FAILED) {
            LOGE("mmap(fd=%d, len=%u, off=%lld) failed: %s", fd, len,
                 static_cast<long long>(off), strerror(errno));
            jit.Regs()[0] = kMapFailed;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mapping_mutex);
            g_guest_file_maps.push_back({fst.st_dev, fst.st_ino, aligned, span, va});
        }
        LOGI("mmap: fd=%d len=%u off=%lld -> guest 0x%08X", fd, len,
             static_cast<long long>(off), va + static_cast<uint32_t>(bias));
        jit.Regs()[0] = va + static_cast<uint32_t>(bias);
    });

    RegisterThunk("munmap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t addr = jit.Regs()[0];
        const uint32_t len  = jit.Regs()[1];
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        if (!addr || !len) { jit.Regs()[0] = 0; return; }

        const uint32_t base_va = addr & ~static_cast<uint32_t>(page - 1);
        const size_t span = (static_cast<uint64_t>(len) + (addr - base_va) + page - 1) & ~(page - 1);

        // A cached file mapping stays: another filesystem may still be reading through it,
        // and the arena address is never reused anyway.
        {
            std::lock_guard<std::mutex> lock(g_mapping_mutex);
            for (const auto& m : g_guest_file_maps) {
                if (base_va >= m.va && base_va < m.va + m.span) { jit.Regs()[0] = 0; return; }
            }
        }
        // MAP_FIXED writes to *host* address space, and both the address and the length come
        // straight from the guest -- an out-of-range pair would unmap whatever the host keeps
        // after the arena, and that surfaces half a second later as a SIGSEGV in an unrelated
        // free(). Bounds-check before touching anything.
        if (static_cast<uint64_t>(base_va) + span > host.GetGuestMemorySize()) {
            LOGE("munmap(0x%08X, %u) out of arena, ignored", addr, len);
            jit.Regs()[0] = 0;
            return;
        }
        // Put anonymous pages back rather than leaving a hole: the arena has to stay one
        // continuous mapping or the next access through base + vaddr faults.
        ::mmap(host.GetGuestMemoryBase() + base_va, span, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        jit.Regs()[0] = 0;
    });

    RegisterThunk("fstat", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int fd = static_cast<int>(jit.Regs()[0]);
        uint32_t buf_gaddr = jit.Regs()[1];
        struct stat st;
        int res = -1;

        {
            std::lock_guard<std::mutex> lock(g_vfd_mutex);
            auto it = g_vfd_map.find(fd);
            if (it != g_vfd_map.end()) {
                memset(&st, 0, sizeof(st));
                st.st_size = it->second.file_size;
                st.st_mode = S_IFREG | 0644;
                st.st_nlink = 1;
                res = 0;
            }
        }

        if (res != 0) {
            res = fstat(fd, &st);
        }

        if (res == 0) {
            WriteArm32Stat(host.GetGuestMemoryBase(), buf_gaddr, st);
        }
        jit.Regs()[0] = res;
    });

    RegisterThunk("remove", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* path = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        jit.Regs()[0] = remove(path ? path : "");
    });

    RegisterThunk("rename", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* oldp = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        const char* newp = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        HornPerfScope perf(HornPerfCategory::FileSync);
        jit.Regs()[0] = rename(oldp ? oldp : "", newp ? newp : "");
    });

    RegisterThunk("chmod", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("umask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("mkstemp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = -1; });
    RegisterThunk("getpid", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = getpid(); });

    RegisterThunk("time", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = static_cast<uint32_t>(time(nullptr));
    });

    RegisterThunk("localtime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static thread_local uint32_t gtm = 0;
        if (!jit.Regs()[0]) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        time_t t = static_cast<time_t>(static_cast<int32_t>(host.MemoryRead32(jit.Regs()[0])));
        tm value{};
        if (!gtm) gtm = host.GuestAlloc(sizeof(Arm32Tm), 4);
        jit.Regs()[0] = localtime_r(&t, &value) && WriteArm32Tm(host, gtm, value) ? gtm : 0;
    });
    RegisterThunk("gmtime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static thread_local uint32_t gtm = 0;
        if (!jit.Regs()[0]) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        time_t t = static_cast<time_t>(static_cast<int32_t>(host.MemoryRead32(jit.Regs()[0])));
        tm value{};
        if (!gtm) gtm = host.GuestAlloc(sizeof(Arm32Tm), 4);
        jit.Regs()[0] = gmtime_r(&t, &value) && WriteArm32Tm(host, gtm, value) ? gtm : 0;
    });
    RegisterThunk("localtime_r", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!jit.Regs()[0] || !jit.Regs()[1]) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        time_t t = static_cast<time_t>(static_cast<int32_t>(host.MemoryRead32(jit.Regs()[0])));
        tm value{};
        jit.Regs()[0] = localtime_r(&t, &value) && WriteArm32Tm(host, jit.Regs()[1], value) ? jit.Regs()[1] : 0;
    });
    RegisterThunk("gmtime_r", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!jit.Regs()[0] || !jit.Regs()[1]) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = 0; return; }
        time_t t = static_cast<time_t>(static_cast<int32_t>(host.MemoryRead32(jit.Regs()[0])));
        tm value{};
        jit.Regs()[0] = gmtime_r(&t, &value) && WriteArm32Tm(host, jit.Regs()[1], value) ? jit.Regs()[1] : 0;
    });

    RegisterThunk("mktime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        tm value{};
        jit.Regs()[0] = ReadArm32Tm(host, jit.Regs()[0], &value)
            ? static_cast<uint32_t>(mktime(&value)) : static_cast<uint32_t>(-1);
    });
    RegisterThunk("strftime", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t output_address = jit.Regs()[0];
        const size_t capacity = jit.Regs()[1];
        const char* format = GuestCString(host.GetGuestMemoryBase(),
                                          host.GetGuestMemorySize(), jit.Regs()[2]);
        const uint32_t tm_address = jit.Regs()[3];
        if (!format || !GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                  tm_address, sizeof(Arm32Tm))) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        char* output = nullptr;
        if (capacity) {
            output = static_cast<char*>(GuestSpan(
                host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                output_address, capacity));
            if (!output) {
                SetGuestErrno(host, EFAULT);
                jit.Regs()[0] = 0;
                return;
            }
        }
        tm value{};
        if (!ReadArm32Tm(host, tm_address, &value)) {
            SetGuestErrno(host, EFAULT);
            jit.Regs()[0] = 0;
            return;
        }
        if (value.tm_zone) {
            const uint32_t zone_address = HostToGuest(
                host.GetGuestMemoryBase(), const_cast<char*>(value.tm_zone));
            if (!GuestCString(host.GetGuestMemoryBase(),
                              host.GetGuestMemorySize(), zone_address)) {
                SetGuestErrno(host, EFAULT);
                jit.Regs()[0] = 0;
                return;
            }
        }
        jit.Regs()[0] = static_cast<uint32_t>(
            capacity ? std::strftime(output, capacity, format, &value) : 0);
    });

    RegisterThunk("sysconf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        errno = 0;
        long value = sysconf(static_cast<int>(jit.Regs()[0]));
        if (value < 0 && errno) SetGuestErrno(host, errno);
        if (value > INT32_MAX) value = INT32_MAX;
        jit.Regs()[0] = static_cast<uint32_t>(static_cast<int32_t>(value));
    });
    RegisterThunk("sysinfo", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (!jit.Regs()[0]) { SetGuestErrno(host, EFAULT); jit.Regs()[0] = static_cast<uint32_t>(-1); return; }
        struct sysinfo native{};
        int result = ::sysinfo(&native);
        if (result == 0) {
            Arm32Sysinfo out{};
            out.uptime = static_cast<int32_t>(native.uptime);
            for (int i = 0; i < 3; ++i) out.loads[i] = static_cast<uint32_t>(native.loads[i]);
            out.totalram = static_cast<uint32_t>(native.totalram);
            out.freeram = static_cast<uint32_t>(native.freeram);
            out.sharedram = static_cast<uint32_t>(native.sharedram);
            out.bufferram = static_cast<uint32_t>(native.bufferram);
            out.totalswap = static_cast<uint32_t>(native.totalswap);
            out.freeswap = static_cast<uint32_t>(native.freeswap);
            out.procs = native.procs;
            out.totalhigh = static_cast<uint32_t>(native.totalhigh);
            out.freehigh = static_cast<uint32_t>(native.freehigh);
            out.mem_unit = native.mem_unit;
            memcpy(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]), &out, sizeof(out));
        } else SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("isatty", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        int result = isatty(static_cast<int>(jit.Regs()[0]));
        if (!result) SetGuestErrno(host, errno);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    RegisterThunk("pause", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        // Blocking the sole main guest JIT would prevent delivery of a guest signal.
        LOGE("pause: guest signal suspension is unavailable; reporting EINTR");
        SetGuestErrno(host, EINTR);
        jit.Regs()[0] = static_cast<uint32_t>(-1);
    });
    RegisterThunk("exit", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        LOGE("Guest requested exit(%d); stopping guest execution, not the ARM64 app process",
             static_cast<int>(jit.Regs()[0]));
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
    });

    RegisterThunk("raise", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });

    // Runtime guards & hooks
    RegisterThunk("__stack_chk_fail", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        // This function is noreturn. Returning executes the caller's literal
        // pool in BL2 afterSolver and obscures the initiating guard failure.
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        const auto regs = jit.Regs();
        LOGE("Guest stack check failed at PC: 0x%08X LR=0x%08X CPSR=0x%08X; "
             "guest execution halted", regs[15], regs[14], jit.Cpsr());
        static std::atomic<uint32_t> dumps{0};
        if (dumps.fetch_add(1, std::memory_order_relaxed) >= 4) return;
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "Stack guard snapshot PC=%08X LR=%08X SP=%08X CPSR=%08X "
            "R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X R6=%08X R7=%08X "
            "R8=%08X R9=%08X R10=%08X R11=%08X R12=%08X",
            regs[15], regs[14], regs[13], jit.Cpsr(), regs[0], regs[1], regs[2],
            regs[3], regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10],
            regs[11], regs[12]);
#endif
        for (size_t first = 0; first < 16; first += 4) {
            LOGE("Stack guard registers r%zu..r%zu={%08X,%08X,%08X,%08X}",
                 first, first + 3, regs[first], regs[first + 1],
                 regs[first + 2], regs[first + 3]);
        }
        // A failed canary may mean even FP or the GOT chain was overwritten.
        // Read our own address space through pread, which returns EIO/EFAULT
        // for inaccessible pages instead of dereferencing a corrupt pointer.
        const int memory_fd = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
        if (memory_fd < 0) {
            LOGE("Stack guard diagnostic memory open failed errno=%d", errno);
            return;
        }
        const auto read_guest = [&](const char* label, uint64_t address,
                                    void* output, size_t bytes) {
            const uint64_t arena = host.GetGuestMemorySize();
            const uint64_t base = reinterpret_cast<uintptr_t>(host.GetGuestMemoryBase());
            if (address > arena || bytes > arena - address ||
                base > static_cast<uint64_t>(INT64_MAX) ||
                address > static_cast<uint64_t>(INT64_MAX) - base ||
                bytes > static_cast<uint64_t>(INT64_MAX) - base - address) {
                LOGE("Stack guard read label=%s guest=0x%llX bytes=%zu invalid_range",
                     label, static_cast<unsigned long long>(address), bytes);
                return false;
            }
            errno = 0;
            const ssize_t got = ::pread64(memory_fd, output, bytes,
                                          static_cast<off64_t>(base + address));
            const int error = errno;
            LOGE("Stack guard read label=%s guest=0x%llX bytes=%zu got=%zd errno=%d",
                 label, static_cast<unsigned long long>(address), bytes, got, error);
            return got == static_cast<ssize_t>(bytes);
        };
#if defined(__ANDROID__)
        uint32_t global_guard = 0;
        const uint32_t guard_address = host.GetThunkManager().FindRegisteredSymbol("__stack_chk_guard");
        const bool guard_read = guard_address &&
            read_guest("global_guard", guard_address, &global_guard, sizeof(global_guard));
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "Stack guard global address=%08X read=%d value=%08X", guard_address,
            guard_read ? 1 : 0, global_guard);
        std::array<uint32_t, 80> stack{};
        if (read_guest("stack", regs[13], stack.data(), sizeof(stack))) {
            for (size_t first = 0; first < stack.size(); first += 16) {
                char line[160] = {};
                size_t used = 0;
                for (size_t i = first; i < first + 16; ++i) {
                    const int written = std::snprintf(line + used, sizeof(line) - used,
                                                      " %08X", stack[i]);
                    if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - used) break;
                    used += static_cast<size_t>(written);
                }
                __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                    "Stack guard stack address=%08X words=%s",
                    regs[13] + static_cast<uint32_t>(first * sizeof(uint32_t)), line);
            }
        }
#endif
        if (regs[11] >= 0x88) {
            uint32_t frame[36] = {};
            const uint32_t address = regs[11] - 0x88;
            if (read_guest("fp_frame", address, frame, sizeof(frame))) {
                for (size_t i = 0; i < std::size(frame); i += 4) {
                    LOGE("Stack guard frame guest=0x%08X words={%08X,%08X,%08X,%08X}",
                         address + static_cast<uint32_t>(i * 4),
                         frame[i], frame[i + 1], frame[i + 2], frame[i + 3]);
                }
            }
        }
        const uint32_t after_solver = host.FindSymbol("_ZN5Scene11afterSolverEv");
        const uint32_t generate_islands = host.FindSymbol("_ZN7PxScene15generateIslandsEb");
#if defined(__ANDROID__)
        if (after_solver >= 0x024E536C && host.GetElfLoader().GetImageSize() == 0x03089000) {
            const uint32_t slot = after_solver - 0x024E536C + 0x02F366E8;
            uint32_t pointer = 0, value = 0;
            const bool slot_read = read_guest("bl2_guard_got", slot, &pointer, sizeof(pointer));
            const bool value_read = slot_read && pointer &&
                read_guest("bl2_guard_value", pointer, &value, sizeof(value));
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                "Stack guard BL2 chain slot=%08X slot_read=%d pointer=%08X value_read=%d value=%08X",
                slot, slot_read ? 1 : 0, pointer, value_read ? 1 : 0, value);
        }
#endif
        const bool failed_after_solver = after_solver >= 0x024E536C &&
            regs[14] == after_solver + 0xCE0;
        const bool failed_generate_islands = generate_islands >= 0x02573280 &&
            regs[14] == generate_islands + 0x5C0;
        if ((failed_after_solver || failed_generate_islands) &&
            host.GetElfLoader().GetImageSize() == 0x03089000) {
            const uint32_t base = failed_after_solver
                ? after_solver - 0x024E536C : generate_islands - 0x02573280;
            const uint32_t computed_slot =
                regs[failed_after_solver ? 5 : 0] + regs[12];
            const uint32_t expected_slot = base + 0x02F366E8;
            LOGE("Stack guard BL2 %s saved_compare=0x%08X "
                 "computed_slot=0x%08X expected_slot=0x%08X",
                 failed_after_solver ? "afterSolver" : "generateIslands",
                 regs[2], computed_slot, expected_slot);
            for (const uint32_t slot : {computed_slot, expected_slot}) {
                uint32_t pointer = 0, value = 0;
                if (read_guest("guard_slot", slot, &pointer, sizeof(pointer))) {
                    LOGE("Stack guard slot=0x%08X pointer=0x%08X", slot, pointer);
                    if (read_guest("guard_value", pointer, &value, sizeof(value))) {
                        LOGE("Stack guard slot=0x%08X guard=0x%08X", slot, value);
                    }
                }
            }
        }
        ::close(memory_fd);
    });

    RegisterThunk("abort", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        LOGE("Guest called abort()!");
    });

    RegisterThunk("__assert2", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* file = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[0]));
        int line = static_cast<int>(jit.Regs()[1]);
        const char* func = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        const char* expr = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        LOGE("Assertion failed: %s at %s:%d (%s)", expr ? expr : "", file ? file : "", line, func ? func : "");
    });

    RegisterThunk("__cxa_atexit", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__cxa_finalize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__aeabi_unwind_cpp_pr0", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("__aeabi_unwind_cpp_pr1", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_Unwind_Resume", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_Unwind_RaiseException", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_DeleteException", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {});
    RegisterThunk("_Unwind_GetLanguageSpecificData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_GetRegionStart", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_VRS_Get", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("_Unwind_VRS_Set", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });
    RegisterThunk("__gnu_unwind_frame", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) { jit.Regs()[0] = 0; });

    // Asset override: see InstallAssetOverrideHook below. R1 is the archive-relative
    // entry name; returning ip=0 makes the veneer return NULL, so the engine's own
    // "not in the archive" path takes over and opens the plain file instead.
    RegisterThunk("asset_override_archive_open", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        jit.Regs()[12] = HasAssetOverride(name) ? 0u : g_asset_override_real_fn;
    });
}

// ---------------------------------------------------------------------------
// Loading modified assets from <externalFilesDir>/...
//
// Engines of this era resolve a file through a chain: archive -> optional second
// archive -> plain fopen(rootDir + path). Making the first archive report "not
// found" for any file the user dropped into the override root turns that existing
// fallback into a full mod mechanism, with no archive-format code and no risk to
// the normal path. Point InstallAssetOverrideHook at the guest's archive-open PLT
// slot; everything below is engine-agnostic.
// ---------------------------------------------------------------------------

static std::string g_override_root;   // e.g. /sdcard/Android/data/<pkg>/files/
static bool g_override_enabled = false;

void SetAssetOverrideRoot(const std::string& root) {
    g_override_root = root;
    if (!g_override_root.empty() && g_override_root.back() != '/') g_override_root += '/';
    struct stat st;
    g_override_enabled = !g_override_root.empty() &&
                         stat(g_override_root.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    LOGI("Asset override root: '%s' (%s)", g_override_root.c_str(), g_override_enabled ? "active" : "inactive");
}

bool HasAssetOverride(const char* entry_name) {
    if (!g_override_enabled || !entry_name) return false;
    // Entry names carry the archive layout, e.g. "assets/data/textures/x.bin" or
    // "<expansion-prefix>/assets/data/textures/x.bin"; the override root mirrors
    // everything below "assets/". Adjust this split if the guest archives differ.
    const char* rel = strstr(entry_name, "assets/");
    if (!rel) return false;
    rel += 7;
    if (!*rel) return false;
    std::string path = g_override_root + rel;
    if (access(path.c_str(), R_OK) != 0) return false;
    LOGI("Asset override: '%s' -> '%s'", entry_name, path.c_str());
    return true;
}

void InstallAssetOverrideHook(DynarmicHost& host, uint32_t plt_slot, const char* real_symbol) {
    if (!g_override_enabled || !plt_slot || !real_symbol) return;

    static bool s_installed = false;
    if (s_installed) return;

    g_asset_override_real_fn = host.GetElfLoader().GetSymbolAddress(real_symbol);
    if (!g_asset_override_real_fn) {
        LOGE("InstallAssetOverrideHook: '%s' not found in the guest", real_symbol);
        return;
    }

    uint32_t hook = host.GetThunkManager().GetOrCreateThunkAddress("asset_override_archive_open");
    uint32_t svc = host.MemoryRead32(hook);
    if ((svc & 0xFF000000) != 0xEF000000) {
        LOGE("InstallAssetOverrideHook: unexpected trampoline at 0x%08X", hook);
        return;
    }

    // ARM veneer: run the hook, then either tail-call the real archive-open (ip != 0)
    // or return NULL. BX reaches the whole address space and handles the Thumb bit,
    // so unlike a B-based trampoline there is no +-32 MB limit.
    uint32_t veneer = host.GuestAlloc(32, 4);
    host.MemoryWrite32(veneer + 0,  svc);         // svc  #id
    host.MemoryWrite32(veneer + 4,  0xE35C0000);  // cmp  ip, #0
    host.MemoryWrite32(veneer + 8,  0x112FFF1C);  // bxne ip
    host.MemoryWrite32(veneer + 12, 0xE3A00000);  // mov  r0, #0
    host.MemoryWrite32(veneer + 16, 0xE12FFF1E);  // bx   lr
    host.InvalidateGuestCode(veneer, 32);

    host.MemoryWrite32(plt_slot + 0, 0xE51FF004);  // ldr pc, [pc, #-4]
    host.MemoryWrite32(plt_slot + 4, veneer);
    host.InvalidateGuestCode(plt_slot, 12);
    // The veneer is executable guest code living in the heap, so it must be
    // exempt from runaway-PC containment.
    host.AddExecutableRange(veneer, 32);

    s_installed = true;
    LOGI("Installed asset-override hook on '%s' (plt 0x%08X, veneer 0x%08X)",
         real_symbol, plt_slot, veneer);
}
