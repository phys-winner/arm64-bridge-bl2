#include "bl2_directory_cache.h"

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

constexpr const char* kTag = "BL2_DirectoryCache";
constexpr size_t kMaxDirectories = 64;
constexpr size_t kMaxCacheBytes = 2 * 1024 * 1024;
constexpr size_t kMaxDirectoryEntries = 1024 * 1024 / sizeof(Bl2DirectoryEntry);
constexpr size_t kMaxDiagnosticPaths = 16;

struct CachedDirectory {
    struct stat stamp{};
    std::shared_ptr<const Bl2DirectorySnapshot> snapshot;
    uint64_t last_use = 0;
    size_t bytes = 0;
};

std::mutex g_mutex;
std::unordered_map<std::string, CachedDirectory> g_directories;
size_t g_cache_bytes = 0;
uint64_t g_serial = 0;
uint64_t g_hits = 0;
uint64_t g_builds = 0;
uint64_t g_invalidations = 0;
uint64_t g_fallbacks = 0;
uint64_t g_open_requests = 0;
uint64_t g_eligible_requests = 0;
std::atomic<uint64_t> g_native_reads{0};
std::atomic<uint64_t> g_snapshot_reads{0};
std::atomic<uint64_t> g_closes{0};
std::atomic<bool> g_reported_startup{false};
struct DiagnosticPath {
    std::string path;
    std::string first_cwd;
    std::string last_cwd;
    uint64_t opens = 0;
    uint64_t cwd_changes = 0;
};
std::vector<DiagnosticPath> g_paths;
uint64_t g_untracked_path_opens = 0;

int Mode() {
    static const int mode = [] {
        char value[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.bl2.dir_cache", value) <= 0) return 0;
        if (std::strcmp(value, "1") == 0) return 1;
        return std::strcmp(value, "2") == 0 ? 2 : 0;
    }();
    return mode;
}

bool Eligible(const std::string& path) {
    // These physical game directories are evidenced by current startup path
    // capture/open logs. Never cache app userdata, config, saves or pseudo-fs.
    constexpr const char* roots[] = {
        "/storage/emulated/0/Borderlands2/WillowGame/CookedAndroid_DXT",
        "/storage/emulated/0/Android/obb/com.tt2kgames.Borderlands2"
    };
    // Redundant separators appear in the actual UE3 path. Collapsing them is
    // safe on Android; dot components are rejected rather than normalized
    // across a possible symlink into a directory outside this allowlist.
    std::string normal;
    normal.reserve(path.size());
    for (char c : path) {
        if (c == '/' && !normal.empty() && normal.back() == '/') continue;
        normal.push_back(c);
    }
    if (normal.size() > 1 && normal.back() == '/') normal.pop_back();
    if (normal.find("/./") != std::string::npos || normal.find("/../") != std::string::npos ||
        normal.ends_with("/.") || normal.ends_with("/..")) return false;
    // Seed221 proves that basename-only case searches scan this exact cwd.
    // This does not authorize snapshots of its other descendants.
    if (normal == "/storage/emulated/0/Borderlands2") return true;
    for (const char* root : roots) {
        const size_t length = std::strlen(root);
        if (normal == root || (normal.size() > length &&
            normal.compare(0, length, root) == 0 && normal[length] == '/')) return true;
    }
    return false;
}

bool SameDirectory(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
           a.st_mode == b.st_mode && a.st_nlink == b.st_nlink &&
           a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

void Erase(std::unordered_map<std::string, CachedDirectory>::iterator it) {
    g_cache_bytes -= it->second.bytes;
    g_directories.erase(it);
}

void LogFallback(const char* path, const char* reason, int error) {
    if (++g_fallbacks <= 16) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "DIR_CACHE fallback path='%s' reason=%s errno=%d count=%llu",
            path, reason, error, static_cast<unsigned long long>(g_fallbacks));
    }
}

} // namespace

std::shared_ptr<const Bl2DirectorySnapshot> Bl2GetDirectorySnapshot(const char* path) {
    const int mode = Mode();
    if (!mode || !path) return {};
    // Keep key storage bounded as well as entry storage. Only literal "." is
    // resolved below; other relative paths and all dot components are rejected.
    if (strnlen(path, 4097) > 4096) return {};
    const int saved_errno = errno;
    struct ErrnoGuard {
        int value;
        ~ErrnoGuard() { errno = value; }
    } errno_guard{saved_errno};
    const std::string request(path);
    std::string key(request);
    std::string current_cwd;
    if (request == ".") {
        // Resolve afresh on every request. Keys and metadata access must never
        // reuse "." after process-wide chdir selects a different directory.
        char cwd[4096]{};
        if (::getcwd(cwd, sizeof(cwd))) {
            key = cwd;
            current_cwd = key;
        } else {
            current_cwd = "unavailable_errno_" + std::to_string(errno);
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_open_requests;
    const bool eligible = Eligible(key);
    auto diagnostic = std::find_if(g_paths.begin(), g_paths.end(),
        [&](const DiagnosticPath& value) { return value.path == request; });
    if (diagnostic != g_paths.end()) {
        ++diagnostic->opens;
        if (!current_cwd.empty() && diagnostic->last_cwd != current_cwd) {
            diagnostic->last_cwd = current_cwd;
            ++diagnostic->cwd_changes;
            if (diagnostic->cwd_changes <= 8) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                    "DIR_CACHE cwd_change path='%s' cwd='%s' changes=%llu opens=%llu",
                    path, current_cwd.c_str(),
                    static_cast<unsigned long long>(diagnostic->cwd_changes),
                    static_cast<unsigned long long>(diagnostic->opens));
            }
        }
    } else if (g_paths.size() < kMaxDiagnosticPaths) {
        std::string first_cwd(current_cwd);
        if (first_cwd.empty() && *path != '/') {
            char cwd[4096]{};
            if (::getcwd(cwd, sizeof(cwd))) first_cwd = cwd;
            else first_cwd = "unavailable_errno_" + std::to_string(errno);
        }
        g_paths.push_back(DiagnosticPath{request, first_cwd, first_cwd, 1, 0});
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "DIR_CACHE path='%s' first_cwd='%s' eligible=%d mode=%d opens=%llu",
            path, g_paths.back().first_cwd.c_str(), eligible ? 1 : 0, mode,
            static_cast<unsigned long long>(g_open_requests));
    } else {
        ++g_untracked_path_opens;
    }
    if (!eligible) return {};
    ++g_eligible_requests;
    if (mode == 2) return {};
    const char* resolved_path = key.c_str();
    struct stat current{};
    auto found = g_directories.find(key);
    if (::stat(resolved_path, &current) != 0 || !S_ISDIR(current.st_mode)) {
        const int error = errno;
        if (found != g_directories.end()) {
            Erase(found);
            ++g_invalidations;
        }
        LogFallback(resolved_path, "stat", error);
        return {};
    }
    if (found != g_directories.end()) {
        if (SameDirectory(found->second.stamp, current)) {
            found->second.last_use = ++g_serial;
            ++g_hits;
            if ((g_hits & (g_hits - 1)) == 0) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                    "DIR_CACHE hit opens=%llu eligible=%llu hits=%llu builds=%llu invalidations=%llu directories=%zu bytes=%zu",
                    static_cast<unsigned long long>(g_open_requests),
                    static_cast<unsigned long long>(g_eligible_requests),
                    static_cast<unsigned long long>(g_hits),
                    static_cast<unsigned long long>(g_builds),
                    static_cast<unsigned long long>(g_invalidations),
                    g_directories.size(), g_cache_bytes);
            }
            return found->second.snapshot;
        }
        Erase(found);
        ++g_invalidations;
    }

    DIR* directory = ::opendir(resolved_path);
    if (!directory) {
        LogFallback(resolved_path, "opendir", errno);
        return {};
    }
    struct DirectoryGuard {
        DIR* value;
        ~DirectoryGuard() { ::closedir(value); }
    } directory_guard{directory};
    struct stat before{};
    if (::fstat(dirfd(directory), &before) != 0 || !SameDirectory(current, before)) {
        LogFallback(resolved_path, "changed_before_read", errno);
        return {};
    }

    auto snapshot = std::make_shared<Bl2DirectorySnapshot>();
    for (;;) {
        errno = 0;
        const dirent* native = ::readdir(directory);
        if (!native) {
            if (errno) {
                LogFallback(resolved_path, "readdir", errno);
                return {};
            }
            break;
        }
        if (snapshot->entries.size() >= kMaxDirectoryEntries) {
            LogFallback(resolved_path, "entry_limit", 0);
            return {};
        }
        Bl2DirectoryEntry entry;
        entry.inode = native->d_ino;
        entry.offset = native->d_off;
        entry.type = native->d_type;
        std::strncpy(entry.name, native->d_name, sizeof(entry.name) - 1);
        snapshot->entries.push_back(entry);
    }
    struct stat after{};
    struct stat path_after{};
    if (::fstat(dirfd(directory), &after) != 0 ||
        ::stat(resolved_path, &path_after) != 0 ||
        !SameDirectory(before, after) || !SameDirectory(before, path_after)) {
        LogFallback(resolved_path, "changed_during_read", errno);
        return {};
    }
    snapshot->entries.shrink_to_fit();
    const size_t bytes = snapshot->entries.capacity() * sizeof(Bl2DirectoryEntry);
    if (bytes > kMaxCacheBytes) {
        LogFallback(resolved_path, "byte_limit", 0);
        return {};
    }
    while (!g_directories.empty() &&
           (g_directories.size() >= kMaxDirectories || g_cache_bytes + bytes > kMaxCacheBytes)) {
        auto oldest = std::min_element(g_directories.begin(), g_directories.end(),
            [](const auto& a, const auto& b) { return a.second.last_use < b.second.last_use; });
        Erase(oldest);
    }
    g_cache_bytes += bytes;
    g_directories.emplace(key, CachedDirectory{before, snapshot, ++g_serial, bytes});
    ++g_builds;
    if (g_builds <= 32) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "DIR_CACHE build path='%s' entries=%zu builds=%llu directories=%zu bytes=%zu",
            resolved_path, snapshot->entries.size(), static_cast<unsigned long long>(g_builds),
            g_directories.size(), g_cache_bytes);
    }
    return snapshot;
}

void Bl2DirectoryCacheRecordRead(bool snapshot) {
    if (!Mode()) return;
    (snapshot ? g_snapshot_reads : g_native_reads).fetch_add(1, std::memory_order_relaxed);
}

void Bl2DirectoryCacheRecordClose() {
    if (Mode()) g_closes.fetch_add(1, std::memory_order_relaxed);
}

void Bl2DirectoryCacheReportStartup() {
    const int mode = Mode();
    if (!mode || g_reported_startup.exchange(true, std::memory_order_relaxed)) return;
    const int saved_errno = errno;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "DIR_CACHE startup mode=%d opens=%llu eligible=%llu hits=%llu builds=%llu "
            "invalidations=%llu fallbacks=%llu native_reads=%llu snapshot_reads=%llu "
            "closes=%llu directories=%zu bytes=%zu tracked_paths=%zu untracked_opens=%llu",
            mode, static_cast<unsigned long long>(g_open_requests),
            static_cast<unsigned long long>(g_eligible_requests),
            static_cast<unsigned long long>(g_hits), static_cast<unsigned long long>(g_builds),
            static_cast<unsigned long long>(g_invalidations), static_cast<unsigned long long>(g_fallbacks),
            static_cast<unsigned long long>(g_native_reads.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_snapshot_reads.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_closes.load(std::memory_order_relaxed)),
            g_directories.size(), g_cache_bytes, g_paths.size(),
            static_cast<unsigned long long>(g_untracked_path_opens));
        std::array<const DiagnosticPath*, kMaxDiagnosticPaths> order{};
        for (size_t i = 0; i < g_paths.size(); ++i) order[i] = &g_paths[i];
        std::sort(order.begin(), order.begin() + g_paths.size(),
            [](const auto* a, const auto* b) { return a->opens > b->opens; });
        for (size_t i = 0; i < g_paths.size(); ++i) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "DIR_CACHE hotpath rank=%zu path='%s' first_cwd='%s' last_cwd='%s' opens=%llu cwd_changes=%llu",
                i + 1, order[i]->path.c_str(), order[i]->first_cwd.c_str(),
                order[i]->last_cwd.c_str(), static_cast<unsigned long long>(order[i]->opens),
                static_cast<unsigned long long>(order[i]->cwd_changes));
        }
        char cwd[4096]{};
        if (::getcwd(cwd, sizeof(cwd))) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "DIR_CACHE cwd_final path='%s'", cwd);
        } else {
            __android_log_print(ANDROID_LOG_INFO, kTag, "DIR_CACHE cwd_final errno=%d", errno);
        }
    }
    errno = saved_errno;
}
