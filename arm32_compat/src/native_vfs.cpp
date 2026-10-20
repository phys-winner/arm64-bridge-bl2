#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "native_vfs.h"
#include "native_jni.h"
#include "native_vulkan_shader_pack.h"
#include "bl2_shader_stage_cache.h"
#include "bl2_shader_former.h"
#include "bl2_directory_cache.h"
#include "bl2_obb_package_watch.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include "thunk_manager.h"
#include "native_host.h"
#include "bl2_locale.h"
#include "bl2_bink_audio.h"
#include "obb_pair_selection.h"
#include "obb_split_inventory.h"
#include "virtual_file_range.h"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#define VFS_LOG(...) __android_log_print(ANDROID_LOG_INFO, "BL2_NativeVfs", __VA_ARGS__)
#define VFS_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "BL2_NativeVfs", __VA_ARGS__)

extern "C" int32_t NativeOriginalLseek(int, int32_t, int) __asm__("lseek");

static_assert(offsetof(struct stat, st_mode) == 16);
static_assert(offsetof(struct stat, st_size) == 48);
static_assert(sizeof(struct stat) == 104);

namespace {

using Jit = Dynarmic::A32::Jit;
using Host = DynarmicHost;

template<class T> T* Pointer(uint32_t value) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}
template<class T> uint32_t Address(T* value) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
}
int64_t Joined64(uint32_t low, uint32_t high) {
    return static_cast<int64_t>(uint64_t(low) | (uint64_t(high) << 32));
}
void Return64(Jit& jit, int64_t value) {
    jit.Regs()[0] = static_cast<uint32_t>(value);
    jit.Regs()[1] = static_cast<uint32_t>(uint64_t(value) >> 32);
}

struct Entry {
    std::string name;
    int fd = -1;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t apk_size = 0;
    const Bl2BinkAudio* bink = nullptr;
    ~Entry() { if (fd >= 0) ::close(fd); }
};
struct FileState {
    std::shared_ptr<Entry> entry;
    int fd;
    int64_t position = 0;
    bool closed = false;
    std::mutex mutex;
    FileState(std::shared_ptr<Entry> source, int descriptor)
        : entry(std::move(source)), fd(descriptor) {}
};
struct StreamCookie { std::shared_ptr<FileState> state; FILE* stream = nullptr; };
struct DirectoryState {
    bool native_entries;
    size_t next = 0;
    struct dirent entry{};
    const std::vector<bl2_orchid::Child>* orchid_entries = nullptr;
    std::shared_ptr<const Bl2DirectorySnapshot> snapshot;
};
struct Mapping { void* base; size_t span; size_t length; };

std::array<std::shared_ptr<Entry>, 2> s_obbs;
AAssetManager* s_assets = nullptr;
jobject s_assets_ref = nullptr;
std::string s_files_root;
std::string s_cache_root;
std::string s_obb_root;
bool s_initialized = false;
std::mutex s_initialize_mutex;
std::mutex s_asset_mutex;
std::unordered_map<std::string, std::shared_ptr<Entry>> s_asset_entries;
std::unordered_map<std::string, std::shared_ptr<Entry>> s_locale_entries;
std::mutex s_files_mutex;
std::unordered_map<int, std::shared_ptr<FileState>> s_files;
std::unordered_map<FILE*, std::shared_ptr<FileState>> s_streams;
std::mutex s_directories_mutex;
std::unordered_map<DIR*, DirectoryState> s_directories;
std::mutex s_mappings_mutex;
std::unordered_map<void*, Mapping> s_mappings;

const char* Basename(const char* path) {
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}
int ObbIndex(const char* path) {
    const std::string_view leaf(Basename(path));
    return leaf == kMainObbName ? 0 : leaf == kPatchObbName ? 1 : -1;
}
std::shared_ptr<Entry> DirectEntry(const char* path) {
    const int index = ObbIndex(path);
    if (index >= 0) return s_obbs[static_cast<size_t>(index)];
    const auto* locale = FindBl2LocaleEntry(path);
    if (!locale || locale->apk_fd < 0 || locale->data_offset < 0 || locale->file_size < 0 ||
        (locale->file_size == 0 && Bl2OrchidKind(path) != bl2_orchid::NodeKind::File))
        return nullptr;
    std::lock_guard lock(s_asset_mutex);
    if (const auto found = s_locale_entries.find(locale->name); found != s_locale_entries.end())
        return found->second;
    auto entry = std::make_shared<Entry>();
    entry->fd = ::fcntl(locale->apk_fd, F_DUPFD_CLOEXEC, 0);
    struct stat st{};
    if (entry->fd < 0 || ::fstat(entry->fd, &st) != 0 || st.st_size < 0 ||
        (!locale->bink && !ResolveVirtualFileRange(locale->data_offset, locale->file_size, 0,
                                locale->file_size, st.st_size).valid)) return nullptr;
    entry->name = locale->name;
    entry->base = static_cast<uint64_t>(locale->data_offset);
    entry->size = static_cast<uint64_t>(locale->file_size);
    entry->apk_size = static_cast<uint64_t>(st.st_size);
    entry->bink = locale->bink;
    s_locale_entries.emplace(entry->name, entry);
    return entry;
}
std::string NormalizeSlashes(const char* path, bool trim_trailing = true) {
    std::string result;
    if (path) for (const char* at = path; *at; ++at) {
        if (*at != '/' || result.empty() || result.back() != '/') result += *at;
    }
    if (trim_trailing)
        while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}
bool IsObbDirectory(const char* path) {
    const auto value = NormalizeSlashes(path);
    return s_obbs[0] && s_obbs[1] &&
        (value == s_obb_root ||
         value == "/sdcard/Android/obb/com.tt2kgames.Borderlands2" ||
         value == "/storage/emulated/0/Android/obb/com.tt2kgames.Borderlands2");
}
bool IsCommandLine(const char* path) {
    return NormalizeSlashes(path, false).ends_with(
        "/Borderlands2/WillowGame/CookedAndroid_DXT/UE3CommandLine.txt");
}
char* DefaultCommandLine(bool low_quality, bool no_sound) {
    // Original ParseParam accepts space/TAB/NUL after a flag, but rejects LF.
    static char normal[] = "-nomovies -nostartupmovies \n";
    static char silent[] = "-nomovies -nostartupmovies -nosound \n";
    static char low[] = "-nomovies -nostartupmovies -SystemSettings=SystemSettingsMobile \n";
    static char low_silent[] = "-nomovies -nostartupmovies -SystemSettings=SystemSettingsMobile -nosound \n";
    return low_quality ? (no_sound ? low_silent : low) : (no_sound ? silent : normal);
}
bool IsLegacyConfigDirectory(const char* path) {
    return NormalizeSlashes(path, false) ==
        "/storage/emulated/0/Borderlands2/WillowGame/Config";
}
bool ReadOnlyMode(const char* mode) {
    return mode && mode[0] == 'r' && !std::strchr(mode, '+');
}

std::shared_ptr<Entry> AssetEntry(const char* path) {
    if (!s_assets || !path || ObbIndex(path) >= 0) return nullptr;
    std::string normalized(path);
    if (normalized.starts_with("assets/")) normalized.erase(0, 7);
    const std::array<std::string, 3> candidates = {
        normalized, std::string("data/audio/") + Basename(path), Basename(path)};
    std::lock_guard lock(s_asset_mutex);
    for (const auto& name : candidates) {
        if (name.empty() || name.front() == '/' || name.find("..") != std::string::npos)
            continue;
        if (const auto found = s_asset_entries.find(name); found != s_asset_entries.end())
            return found->second;
        AAsset* asset = AAssetManager_open(s_assets, name.c_str(), AASSET_MODE_RANDOM);
        if (!asset) continue;
        off64_t base = 0, length = 0;
        const int fd = AAsset_openFileDescriptor64(asset, &base, &length);
        AAsset_close(asset);
        if (fd < 0) continue;
        struct stat st{};
        if (::fstat(fd, &st) != 0 || base < 0 || length <= 0 || st.st_size < 0 ||
            !ResolveVirtualFileRange(base, length, 0, length, st.st_size).valid) {
            ::close(fd);
            continue;
        }
        auto entry = std::make_shared<Entry>();
        entry->name = name;
        entry->fd = fd;
        entry->base = static_cast<uint64_t>(base);
        entry->size = static_cast<uint64_t>(length);
        entry->apk_size = static_cast<uint64_t>(st.st_size);
        s_asset_entries.emplace(name, entry);
        return entry;
    }
    return nullptr;
}

std::shared_ptr<FileState> FindFile(int fd) {
    std::lock_guard lock(s_files_mutex);
    const auto found = s_files.find(fd);
    return found == s_files.end() ? nullptr : found->second;
}
int OpenEntry(const std::shared_ptr<Entry>& entry) {
    const int fd = ::fcntl(entry->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return -1;
    auto state = std::make_shared<FileState>(entry, fd);
    for (size_t i = 0; i < s_obbs.size(); ++i)
        if (entry == s_obbs[i])
            Bl2ObbPackageWatchOpen(fd, static_cast<int>(i), fd, entry->base);
    std::lock_guard lock(s_files_mutex);
    s_files.emplace(fd, state);
    return fd;
}
int CloseFile(const std::shared_ptr<FileState>& state) {
    std::lock_guard state_lock(state->mutex);
    if (state->closed) { errno = EBADF; return -1; }
    {
        std::lock_guard map_lock(s_files_mutex);
        s_files.erase(state->fd);
    }
    state->closed = true;
    Bl2ObbPackageWatchClose(state->fd);
    return ::close(state->fd);
}
int NativeClose(int fd) {
    const auto state = FindFile(fd);
    return state ? CloseFile(state) : ::close(fd);
}
ssize_t ReadFile(const std::shared_ptr<FileState>& state, void* output,
                 size_t count, int64_t offset, bool advance) {
    std::lock_guard lock(state->mutex);
    if (state->closed) { errno = EBADF; return -1; }
    const int64_t position = advance ? state->position : offset;
    if (position < 0 || (count && !output)) { errno = EINVAL; return -1; }
    if (uint64_t(position) >= state->entry->size || count == 0) return 0;
    count = static_cast<size_t>(std::min<uint64_t>(
        std::min<size_t>(count, SSIZE_MAX), state->entry->size - uint64_t(position)));
    Bl2ObbPackageWatchRead(state->fd, static_cast<uint64_t>(position));
    ssize_t result;
    do {
        result = state->entry->bink ? state->entry->bink->Read(output, count, position)
            : ::pread64(state->fd, output, count,
                          static_cast<off64_t>(state->entry->base + uint64_t(position)));
    } while (result < 0 && errno == EINTR);
    if (result > 0 && advance) state->position += result;
    return result;
}
ssize_t NativeRead(int fd, void* output, size_t count) {
    const auto state = FindFile(fd);
    return state ? ReadFile(state, output, count, 0, true) : ::read(fd, output, count);
}
ssize_t NativePread(int fd, void* output, size_t count, int64_t offset) {
    const auto state = FindFile(fd);
    return state ? ReadFile(state, output, count, offset, false)
                 : ::pread64(fd, output, count, offset);
}
constexpr bool SeekPosition(int64_t base, int64_t offset, int64_t* position) {
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset)) return false;
    const int64_t result = base + offset;
    if (result < 0) return false;
    *position = result;
    return true;
}
int64_t SeekFile(const std::shared_ptr<FileState>& state, int64_t offset,
                 int whence, bool legacy32 = false) {
    std::lock_guard lock(state->mutex);
    if (state->closed) { errno = EBADF; return -1; }
    int64_t base = 0;
    if (whence == SEEK_CUR) base = state->position;
    else if (whence == SEEK_END) base = static_cast<int64_t>(state->entry->size);
    else if (whence != SEEK_SET) { errno = EINVAL; return -1; }
    int64_t position = 0;
    if (!SeekPosition(base, offset, &position)) { errno = EINVAL; return -1; }
    if (legacy32 && position > INT32_MAX) { errno = EOVERFLOW; return -1; }
    state->position = position;
    return position;
}
int64_t NativeSeek(int fd, int64_t offset, int whence, bool legacy32) {
    const auto state = FindFile(fd);
    if (state) return SeekFile(state, offset, whence, legacy32);
    if (legacy32) {
        // Call the original LP32 ABI explicitly despite this source's LFS mode.
        return NativeOriginalLseek(fd, static_cast<int32_t>(offset), whence);
    }
    return ::lseek64(fd, offset, whence);
}

int StreamRead(void* value, char* output, int count) {
    if (count <= 0) return 0;
    return static_cast<int>(ReadFile(static_cast<StreamCookie*>(value)->state,
                                    output, static_cast<size_t>(count), 0, true));
}
fpos64_t StreamSeek(void* value, fpos64_t offset, int whence) {
    return SeekFile(static_cast<StreamCookie*>(value)->state, offset, whence);
}
int StreamClose(void* value) {
    std::unique_ptr<StreamCookie> cookie(static_cast<StreamCookie*>(value));
    {
        std::lock_guard lock(s_files_mutex);
        s_streams.erase(cookie->stream);
    }
    return CloseFile(cookie->state);
}
FILE* EntryStream(const std::shared_ptr<FileState>& state) {
    auto cookie = std::make_unique<StreamCookie>();
    cookie->state = state;
    FILE* stream = ::funopen64(cookie.get(), StreamRead, nullptr, StreamSeek, StreamClose);
    if (!stream) return nullptr;
    cookie->stream = stream;
    {
        std::lock_guard lock(s_files_mutex);
        s_streams.emplace(stream, state);
    }
    cookie.release();
    return stream;
}
FILE* EntryStream(const std::shared_ptr<Entry>& entry) {
    const int fd = OpenEntry(entry);
    if (fd < 0) return nullptr;
    FILE* stream = EntryStream(FindFile(fd));
    if (!stream) { const int error = errno; NativeClose(fd); errno = error; }
    return stream;
}
FILE* NativeFopen(const char* path, const char* mode) {
    if (!path || !mode) { errno = EINVAL; return nullptr; }
    if (Bl2OrchidKind(path) == bl2_orchid::NodeKind::Directory) { errno = EISDIR; return nullptr; }
    const bool read_only = ReadOnlyMode(mode);
    if (const auto entry = DirectEntry(path)) {
        if (!read_only) { errno = EROFS; return nullptr; }
        return EntryStream(entry);
    }
    if (read_only && IsCommandLine(path)) {
        char movies[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.horn.play_movies", movies) > 0 && movies[0] == '0') {
            char low[PROP_VALUE_MAX]{};
            const bool low_quality = __system_property_get("debug.bl2.low_quality", low) > 0 &&
                                     low[0] == '1' && low[1] == 0;
            char silent[PROP_VALUE_MAX]{};
            const bool no_sound = __system_property_get("debug.bl2.no_sound", silent) > 0 &&
                                  silent[0] == '1' && silent[1] == 0;
            char* flags = DefaultCommandLine(low_quality, no_sound);
            VFS_LOG("Native engine command line: %s", flags);
            return ::fmemopen(flags, std::strlen(flags), "r");
        }
        if (!s_files_root.empty()) {
            const auto local = s_files_root + "/WillowGame/CookedAndroid_DXT/UE3CommandLine.txt";
            if (FILE* stream = ::fopen(local.c_str(), mode)) return stream;
        }
    }
    FILE* stream = ::fopen(path, mode);
    if (!stream && read_only) {
        if (const auto entry = AssetEntry(path)) stream = EntryStream(entry);
    }
    return stream;
}
int NativeOpen(const char* path, int flags, mode_t mode) {
    if (!path) { errno = EFAULT; return -1; }
    if (Bl2OrchidKind(path) == bl2_orchid::NodeKind::Directory) { errno = EISDIR; return -1; }
    const bool read_only = !(flags & (O_CREAT | O_WRONLY | O_RDWR | O_TRUNC | O_APPEND));
    if (const auto entry = DirectEntry(path)) {
        if (!read_only) { errno = EROFS; return -1; }
        return OpenEntry(entry);
    }
    const int fd = ::open(path, flags, mode);
    if (fd >= 0 || !read_only) return fd;
    const auto entry = AssetEntry(path);
    return entry ? OpenEntry(entry) : -1;
}
void FillStat(const Entry& entry, struct stat* output) {
    std::memset(output, 0, sizeof(*output));
    output->st_mode = S_IFREG | 0444;
    output->st_nlink = 1;
    output->st_size = static_cast<int64_t>(entry.size);
    output->st_blksize = 4096;
    output->st_blocks = (entry.size + 511) / 512;
}
#if defined(BL2_NATIVE_ARM32) && BL2_ENABLE_QA_CONSOLE
// Diagnostic classification only; it never changes path resolution.
bool NativeQaDlcPath(const char* path) {
    if (!path) return false;
    size_t length = 0;
    while (length <= 1024 && path[length]) ++length;
    if (length > 1024) return false;
    const auto slash = [](char c) { return c == '/' || c == '\\'; };
    for (size_t i = 0; i + 3 <= length; ++i) {
        if ((i == 0 || slash(path[i - 1])) &&
            (path[i] == 'd' || path[i] == 'D') &&
            (path[i + 1] == 'l' || path[i + 1] == 'L') &&
            (path[i + 2] == 'c' || path[i + 2] == 'C') &&
            (i + 3 == length || slash(path[i + 3]))) return true;
    }
    return false;
}
#endif

int NativeStat(const char* path, struct stat* output) {
    if (!path || !output) { errno = EFAULT; return -1; }
    if (Bl2OrchidStat(path, *output)) return 0;
    if (const auto entry = DirectEntry(path)) { FillStat(*entry, output); return 0; }
    if (::stat(path, output) == 0) return 0;
    if (const auto entry = AssetEntry(path)) { FillStat(*entry, output); return 0; }
    return -1;
}
int NativeFstat(int fd, struct stat* output) {
    if (!output) { errno = EFAULT; return -1; }
    if (const auto state = FindFile(fd)) {
        std::lock_guard lock(state->mutex);
        if (state->closed) { errno = EBADF; return -1; }
        FillStat(*state->entry, output);
        return 0;
    }
    return ::fstat(fd, output);
}
void* NativeMmap(void* requested, size_t length, int prot, int flags,
                 int fd, int64_t offset) {
    const auto state = FindFile(fd);
    if (!state) return ::mmap64(requested, length, prot, flags, fd, offset);
    std::lock_guard state_lock(state->mutex);
    if (state->closed) { errno = EBADF; return MAP_FAILED; }
    if (state->entry->bink) {
        if (!length || offset < 0 || uint64_t(offset) > state->entry->size ||
            length > state->entry->size - uint64_t(offset)) { errno = EINVAL; return MAP_FAILED; }
        void* output = ::mmap(requested, length, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | (flags & MAP_FIXED), -1, 0);
        if (output == MAP_FAILED) return output;
        if (state->entry->bink->Read(output, length, offset) != length ||
            ::mprotect(output, length, prot) != 0) {
            const int error = errno ? errno : EIO;
            ::munmap(output, length); errno = error; return MAP_FAILED;
        }
        std::lock_guard map_lock(s_mappings_mutex);
        s_mappings.emplace(output, Mapping{output, length, length});
        return output;
    }
    const auto range = offset < 0 ? VirtualFileRange{false, 0} :
        ResolveVirtualFileRange(state->entry->base, state->entry->size,
                                offset, length, state->entry->apk_size);
    if (!range.valid) { errno = EINVAL; return MAP_FAILED; }
    const long page_value = ::sysconf(_SC_PAGESIZE);
    if (page_value <= 0) { errno = EINVAL; return MAP_FAILED; }
    const uint64_t page = static_cast<uint64_t>(page_value);
    const uint64_t aligned = range.absolute_offset / page * page;
    const uint64_t bias = range.absolute_offset - aligned;
    const uint64_t span64 = (bias + uint64_t(length) + page - 1) / page * page;
    if (span64 > SIZE_MAX || ((flags & MAP_FIXED) && bias != 0)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    void* hint = requested && reinterpret_cast<uintptr_t>(requested) >= bias
        ? reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(requested) - bias) : nullptr;
    void* base = ::mmap64(hint, static_cast<size_t>(span64), prot, flags, fd,
                          static_cast<off64_t>(aligned));
    if (base == MAP_FAILED) return base;
    void* result = static_cast<char*>(base) + bias;
    std::lock_guard map_lock(s_mappings_mutex);
    s_mappings.emplace(result, Mapping{base, static_cast<size_t>(span64), length});
    return result;
}
int NativeMunmap(void* address, size_t length) {
    std::lock_guard lock(s_mappings_mutex);
    const auto found = s_mappings.find(address);
    if (found == s_mappings.end()) return ::munmap(address, length);
    // ponytail: observed readers unmap whole regions; split mapping records if
    // a future reader needs to unmap only part of an APK-backed region.
    if (length != found->second.length) { errno = EINVAL; return -1; }
    const int result = ::munmap(found->second.base, found->second.span);
    if (result == 0) s_mappings.erase(found);
    return result;
}

DIR* NativeOpendir(const char* path) {
    if (!path) { errno = EFAULT; return nullptr; }
    if (Bl2OrchidKind(path) == bl2_orchid::NodeKind::File) { errno = ENOTDIR; return nullptr; }
    if (const auto* entries = Bl2OrchidChildren(path)) {
        DIR* dir = ::opendir(s_files_root.c_str());
        if (!dir) return nullptr;
        std::lock_guard lock(s_directories_mutex);
        DirectoryState state{false}; state.orchid_entries = entries;
        s_directories.emplace(dir, state);
        return dir;
    }
    const bool include_embedded_obbs = IsObbDirectory(path);
    auto snapshot = include_embedded_obbs ? nullptr : Bl2GetDirectorySnapshot(path);
    // Keep an owned native DIR for closedir while the snapshot supplies entries.
    DIR* dir = ::opendir(snapshot ? s_files_root.c_str() : path);
    if (snapshot) {
        if (!dir) return nullptr;
        std::lock_guard lock(s_directories_mutex);
        DirectoryState state{false}; state.snapshot = std::move(snapshot);
        s_directories.emplace(dir, std::move(state));
        return dir;
    }
    if (!include_embedded_obbs) return dir;
    const bool native_entries = dir != nullptr;
    if (!dir) dir = ::opendir(s_files_root.c_str());
    if (!dir) return nullptr;
    std::lock_guard lock(s_directories_mutex);
    s_directories.emplace(dir, DirectoryState{native_entries});
    return dir;
}
struct dirent* NativeReaddir(DIR* directory) {
    std::lock_guard lock(s_directories_mutex);
    const auto found = s_directories.find(directory);
    if (found == s_directories.end()) return ::readdir(directory);
    auto& state = found->second;
    if (state.orchid_entries) {
        errno = 0;
#if defined(BL2_NATIVE_ARM32) && BL2_ENABLE_QA_CONSOLE
        static std::atomic<unsigned> qaRows{0};
        const unsigned qaRow = qaRows.fetch_add(1, std::memory_order_relaxed) + 1;
        if (qaRow <= 128) {
            const int savedErrno = errno;
            __android_log_print(ANDROID_LOG_INFO, "BL2_NativeDlcQa",
                "readdir row=%u dir=%p index=%zu count=%zu name=%.240s",
                qaRow, static_cast<void*>(directory), state.next, state.orchid_entries->size(),
                state.next < state.orchid_entries->size()
                    ? (*state.orchid_entries)[state.next].name.c_str() : "<end>");
            errno = savedErrno;
        }
#endif
        if (state.next == state.orchid_entries->size()) return nullptr;
        const auto& entry = (*state.orchid_entries)[state.next++];
        state.entry = {};
        state.entry.d_ino = 0xB12D0000u + state.next;
        state.entry.d_off = state.next;
        state.entry.d_type = entry.kind == bl2_orchid::NodeKind::Directory ? DT_DIR : DT_REG;
        std::memcpy(state.entry.d_name, entry.name.c_str(), entry.name.size() + 1);
        state.entry.d_reclen = static_cast<unsigned short>(offsetof(struct dirent, d_name) + entry.name.size() + 1);
        return &state.entry;
    }
    if (state.snapshot) {
        errno = 0;
        if (state.next == state.snapshot->entries.size()) return nullptr;
        const auto& entry = state.snapshot->entries[state.next++];
        state.entry = {};
        state.entry.d_ino = entry.inode;
        state.entry.d_off = entry.offset;
        state.entry.d_type = entry.type;
        std::memcpy(state.entry.d_name, entry.name, sizeof(state.entry.d_name));
        state.entry.d_reclen = static_cast<unsigned short>(
            offsetof(struct dirent, d_name) + std::strlen(state.entry.d_name) + 1);
        return &state.entry;
    }
    if (state.next < 2) {
        const auto name = state.next == 0 ? kMainObbName : kPatchObbName;
        state.entry = {};
        state.entry.d_ino = 0xB1200001u + state.next;
        state.entry.d_off = ++state.next;
        state.entry.d_type = DT_REG;
        std::memcpy(state.entry.d_name, name.data(), name.size());
        state.entry.d_reclen = static_cast<unsigned short>(
            offsetof(struct dirent, d_name) + name.size() + 1);
        return &state.entry;
    }
    errno = 0;
    if (!state.native_entries) return nullptr;
    struct dirent* entry;
    do { entry = ::readdir(directory); }
    while (entry && (ObbIndex(entry->d_name) >= 0 || IsManagedBl2ZipObbName(entry->d_name)));
    return entry;
}

std::string JavaString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (!text) return {};
    std::string result(text);
    env->ReleaseStringUTFChars(value, text);
    return result;
}
std::string JavaFilePath(JNIEnv* env, jobject file) {
    if (!file) return {};
    jclass owner = env->GetObjectClass(file);
    const jmethodID method = owner ? env->GetMethodID(owner, "getAbsolutePath", "()Ljava/lang/String;") : nullptr;
    jstring path = method ? static_cast<jstring>(env->CallObjectMethod(file, method)) : nullptr;
    std::string result = !env->ExceptionCheck() ? JavaString(env, path) : std::string{};
    if (path) env->DeleteLocalRef(path);
    if (owner) env->DeleteLocalRef(owner);
    return result;
}

} // namespace

const std::string& NativeSavedFilesRoot() { return s_files_root; }
const std::string& NativeShaderCacheRoot() { return s_cache_root; }

std::shared_ptr<bl2::vk::ShaderPack> NativeOpenVulkanShaderPack(std::string& error) {
    if (!s_assets) { error = "APK asset manager is unavailable"; return {}; }
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(s_assets, "bl2_vulkan_stages.bin", AASSET_MODE_RANDOM), AAsset_close);
    if (!asset) { error = "Vulkan shader asset is missing"; return {}; }
    off64_t base = 0, bytes = 0;
    const int fd = AAsset_openFileDescriptor64(asset.get(), &base, &bytes);
    if (fd < 0) { error = "Vulkan shader asset must be stored without ZIP compression"; return {}; }
    auto pack = std::make_shared<bl2::vk::ShaderPack>();
    const bool opened = base >= 0 && bytes > 0 &&
        pack->OpenFileDescriptor(fd, static_cast<std::uint64_t>(base),
                                static_cast<std::uint64_t>(bytes), &error);
    close(fd);
    if (!opened) { if (error.empty()) error = "Invalid Vulkan shader asset range"; return {}; }
    return pack;
}

bool NativeVulkanSeedRequired(){
#if defined(BL2_VK_REQUIRE_SEED) && BL2_VK_REQUIRE_SEED
    return true;
#else
    return false;
#endif
}
bool NativeReadVulkanMandatorySeed(std::vector<std::uint8_t>& bytes,bl2vk::seed::Request& request,std::string& error){
#if defined(BL2_VK_REQUIRE_SEED) && BL2_VK_REQUIRE_SEED
    try{
        if(!s_assets){error="Mandatory Vulkan seed asset manager unavailable";return false;}
        std::unique_ptr<AAsset,decltype(&AAsset_close)> asset(AAssetManager_open(s_assets,"bl2_vulkan_seed.bin",AASSET_MODE_STREAMING),AAsset_close);
        if(!asset){error="Required Vulkan seed is missing";return false;}
        const auto length=AAsset_getLength64(asset.get());
        if(length<static_cast<off64_t>(bl2vk::seed::kHeaderBytes)||length>static_cast<off64_t>(bl2vk::seed::kHeaderBytes+bl2vk::seed::kMaxMandatory*bl2vk::seed::kRecipeBytes)){error="Required Vulkan seed size outside bound";return false;}
        bytes.resize(static_cast<std::size_t>(length));std::size_t offset=0;
        while(offset<bytes.size()){const auto got=AAsset_read(asset.get(),bytes.data()+offset,bytes.size()-offset);if(got<=0){error="Required Vulkan seed truncated";return false;}offset+=got;}
        std::uint8_t trailing=0;if(AAsset_read(asset.get(),&trailing,1)!=0){error="Required Vulkan seed has trailing data/read error";return false;}
        request={bytes,bl2vk::seed::ParseDigest(BL2_VK_SEED_CONTENT_ID),bl2vk::seed::ParseDigest(BL2_VK_SEED_SCOPE_ID),bl2vk::seed::ParseDigest(BL2_VK_SEED_RECEIPT_ID)};return true;
    }catch(const std::exception& failure){error=failure.what();return false;}
#else
    (void)bytes;(void)request;error="Mandatory Vulkan seed is not enabled in this build";return false;
#endif
}

bool PrepareBl2ShaderStageCache(Bl2ShaderStageCache& cache) {
    if (!s_assets) return false;
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(s_assets, "bl2_shader_stages.bin", AASSET_MODE_STREAMING), AAsset_close);
    if (!asset) return false;
    const auto length = AAsset_getLength64(asset.get());
    if (length <= 0 || length > 256LL * 1024 * 1024) return false;
    const Bl2ShaderStageCache::Reader reader{
        asset.get(), static_cast<std::uint64_t>(length),
        [](void* opaque, std::uint8_t* bytes, std::size_t wanted) -> std::size_t {
            const int read = AAsset_read(static_cast<AAsset*>(opaque), bytes,
                                        std::min<std::size_t>(wanted, INT_MAX));
            return read > 0 ? static_cast<std::size_t>(read) : 0;
        },
        [](void* opaque) { return AAsset_seek64(static_cast<AAsset*>(opaque), 0, SEEK_SET) == 0; }
    };
    const bool prepared = cache.PrepareStream(reader, s_cache_root,
        [](void*, std::uint32_t completed, std::uint32_t total) {
            NativeShaderPreparationProgress(completed, total);
        }, nullptr);
    // Complete is published by PrepareStream only after the last disk commit.
    NativeShaderPreparationProgress(0, 0);
    return prepared;
}

// Each worker owns a separate reader; the retained AssetManager lives for the process.
static bool OpenNativeShaderFormerAsset(const char* name, Bl2ShaderFormerAsset* asset) {
    if (!s_assets || !name || !asset) return false;
    AAsset* opened = AAssetManager_open(s_assets, name, AASSET_MODE_STREAMING);
    if (!opened) return false;
    const auto length = AAsset_getLength64(opened);
    if (length <= 0 || length > Bl2ShaderStageCache::kMaxTableBytes) {
        AAsset_close(opened);
        return false;
    }
    asset->opaque = opened;
    asset->bytes = static_cast<uint64_t>(length);
    asset->read = [](void* opaque, uint8_t* bytes, size_t wanted) -> size_t {
        const int count = AAsset_read(static_cast<AAsset*>(opaque), bytes,
                                     std::min<size_t>(wanted, INT_MAX));
        return count > 0 ? static_cast<size_t>(count) : 0;
    };
    asset->close = [](void* opaque) { AAsset_close(static_cast<AAsset*>(opaque)); };
    return true;
}

static int32_t NativeShaderFormationPermille() {
    uint32_t ready = 0, total = 0;
    if (!Bl2ShaderFormerProgress(&ready, &total)) return -1;
    return static_cast<int32_t>(uint64_t{ready} * 1000 / total);
}

bool OpenBl2ShaderStageCache(Bl2ShaderStageCache& cache) {
    const auto mode = Bl2ShaderFormerConfiguredMode();
    if (mode == Bl2ShaderFormerMode::Blocking) return PrepareBl2ShaderStageCache(cache);
    Bl2ShaderFormerAsset asset{};
    if (!OpenNativeShaderFormerAsset("bl2_shader_stages.bin", &asset)) return false;
    std::unique_ptr<AAsset, decltype(&AAsset_close)> owner(
        static_cast<AAsset*>(asset.opaque), AAsset_close);
    const Bl2ShaderStageCache::Reader reader{
        asset.opaque, asset.bytes, asset.read,
        [](void* opaque) { return AAsset_seek64(static_cast<AAsset*>(opaque), 0, SEEK_SET) == 0; }
    };
    if (!cache.OpenStream(reader, s_cache_root)) return false;
    if (!cache.Complete() && mode == Bl2ShaderFormerMode::Background) {
        extern bool Bl2TouchPlayerInWorld();
        Bl2ShaderFormerSetGameplayProbe(Bl2TouchPlayerInWorld);
        if (Bl2ShaderFormerStart(&cache, eglGetCurrentDisplay(), OpenNativeShaderFormerAsset,
                                 Bl2ShaderFormerConfiguredThreads()))
            bl2::touch::setShaderFormationProgress(NativeShaderFormationPermille);
    }
    return true;
}

std::vector<std::uint8_t> LoadBl2ShaderWarmupTable() {
    if (!s_assets) return {};
    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(s_assets, "bl2_shader_warmup.bin", AASSET_MODE_STREAMING), AAsset_close);
    if (!asset) return {};
    const auto length = AAsset_getLength64(asset.get());
    if (length <= 0 || length > 8 * 1024 * 1024) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    std::size_t used = 0;
    while (used < bytes.size()) {
        const int count = AAsset_read(asset.get(), bytes.data() + used, bytes.size() - used);
        if (count <= 0) return {};
        used += static_cast<std::size_t>(count);
    }
    return bytes;
}

bool NativeVfsSelfCheck() {
    if (std::strcmp(DefaultCommandLine(false, false), "-nomovies -nostartupmovies \n") ||
        std::strcmp(DefaultCommandLine(false, true), "-nomovies -nostartupmovies -nosound \n") ||
        std::strcmp(DefaultCommandLine(true, false), "-nomovies -nostartupmovies -SystemSettings=SystemSettingsMobile \n") ||
        std::strcmp(DefaultCommandLine(true, true), "-nomovies -nostartupmovies -SystemSettings=SystemSettingsMobile -nosound \n"))
        return false;
    if (!IsLegacyConfigDirectory("/storage/emulated/0/Borderlands2/WillowGame/Config") ||
        !IsLegacyConfigDirectory("/storage/emulated/0/Borderlands2//WillowGame/Config") ||
        IsLegacyConfigDirectory("/storage/emulated/0/Borderlands2/WillowGame/Config/") ||
        IsLegacyConfigDirectory("/storage/emulated/0/Borderlands2/WillowGame/ConfigExtra") ||
        IsLegacyConfigDirectory("/storage/emulated/0/Borderlands2/WillowGame/Config/DLCDEV") ||
        IsLegacyConfigDirectory("/storage/emulated/0/Borderlands2Extra/WillowGame/Config") ||
        IsLegacyConfigDirectory("/sdcard/Borderlands2/WillowGame/Config") ||
        IsLegacyConfigDirectory("/storage/emulated/0/Android/data/com.tt2kgames.Borderlands2/files/Config") ||
        IsLegacyConfigDirectory(nullptr) ||
        !IsCommandLine("/storage/emulated/0/Borderlands2//WillowGame/CookedAndroid_DXT/UE3CommandLine.txt") ||
        IsCommandLine("/storage/emulated/0/Borderlands2Extra/WillowGame/CookedAndroid_DXT/UE3CommandLine.txt") ||
        IsCommandLine("/storage/emulated/0/Borderlands2/WillowGame/CookedAndroid_DXT/UE3CommandLine.txt/"))
        return false;
    int64_t position = -1;
    return SeekPosition(2647408640LL, -1, &position) && position == 2647408639LL &&
        !SeekPosition(0, -1, &position) &&
        !SeekPosition(INT64_MAX, 1, &position) &&
        !SeekPosition(0, INT64_MIN, &position) &&
        ResolveVirtualFileRange(704, 2647408640ULL, 2647408639ULL, 1,
                                2647409344ULL).absolute_offset == 2647409343ULL &&
        !ResolveVirtualFileRange(704, 2647408640ULL, 2647408640ULL, 1,
                                 2647409344ULL).valid;
}

bool InitializeNativeVfs(JNIEnv* env, jobject context, const char* base_apk_path) {
    std::lock_guard initialize_lock(s_initialize_mutex);
    if (s_initialized) return true;
    if (!NativeVfsSelfCheck() || !env || !context || !base_apk_path || !*base_apk_path)
        return false;
    if (env->PushLocalFrame(80) != JNI_OK) return false;
    const auto finish = [env](bool good) {
        if (env->ExceptionCheck()) { env->ExceptionClear(); good = false; }
        if (!good && s_assets_ref) {
            env->DeleteGlobalRef(s_assets_ref);
            s_assets_ref = nullptr;
            s_assets = nullptr;
        }
        env->PopLocalFrame(nullptr);
        return good;
    };
    jclass owner = env->GetObjectClass(context);
    const jmethodID assets_method = owner ? env->GetMethodID(owner, "getAssets", "()Landroid/content/res/AssetManager;") : nullptr;
    const jmethodID info_method = owner ? env->GetMethodID(owner, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;") : nullptr;
    if (!assets_method || !info_method || env->ExceptionCheck()) return finish(false);
    jobject assets = env->CallObjectMethod(context, assets_method);
    if (!assets || env->ExceptionCheck()) return finish(false);
    jobject info = env->CallObjectMethod(context, info_method);
    if (!assets || !info || env->ExceptionCheck()) return finish(false);
    s_assets_ref = env->NewGlobalRef(assets);
    s_assets = AAssetManager_fromJava(env, assets);
    if (!s_assets_ref || !s_assets || env->ExceptionCheck()) return finish(false);
    jclass info_owner = env->GetObjectClass(info);
    if (!info_owner || env->ExceptionCheck()) return finish(false);
    const jfieldID split_field = env->GetFieldID(info_owner, "splitSourceDirs", "[Ljava/lang/String;");
    if (!split_field || env->ExceptionCheck()) return finish(false);
    jobjectArray splits = static_cast<jobjectArray>(env->GetObjectField(info, split_field));
    const jsize count = splits ? env->GetArrayLength(splits) : 0;
    if (env->ExceptionCheck() || count < 0 || count > 64) return finish(false);
    std::vector<std::string> paths;
    for (jsize i = 0; i < count; ++i) {
        jstring value = static_cast<jstring>(env->GetObjectArrayElement(splits, i));
        const std::string path = JavaString(env, value);
        if (value) env->DeleteLocalRef(value);
        if (env->ExceptionCheck() || path.empty()) return finish(false);
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
    }
    const jmethodID external_files = env->GetMethodID(owner, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
    const jmethodID internal_files = env->GetMethodID(owner, "getFilesDir", "()Ljava/io/File;");
    const jmethodID internal_cache = env->GetMethodID(owner, "getCacheDir", "()Ljava/io/File;");
    const jmethodID obb_dir = env->GetMethodID(owner, "getObbDir", "()Ljava/io/File;");
    if (!external_files || !internal_files || !internal_cache || !obb_dir || env->ExceptionCheck()) return finish(false);
    s_files_root = JavaFilePath(env, env->CallObjectMethod(context, external_files, nullptr));
    if (s_files_root.empty() && !env->ExceptionCheck())
        s_files_root = JavaFilePath(env, env->CallObjectMethod(context, internal_files));
    s_cache_root = JavaFilePath(env, env->CallObjectMethod(context, internal_cache));
    s_obb_root = NormalizeSlashes(JavaFilePath(env, env->CallObjectMethod(context, obb_dir)).c_str());
    if (env->ExceptionCheck()) return finish(false);
    SetBl2ExternalObbDirectory(s_obb_root.c_str());

    // The stock LanguageSet callback still uses INT/en. Select its direct
    // resource overlay from Android's per-app language before indexing base.
    jclass storage = env->FindClass("com/tt2kgames/Borderlands2/GameStorage");
    const jmethodID language_method = storage ? env->GetStaticMethodID(storage,
        "appLanguage", "(Landroid/content/Context;)Ljava/lang/String;") : nullptr;
    jstring language = language_method ? static_cast<jstring>(
        env->CallStaticObjectMethod(storage, language_method, context)) : nullptr;
    if (!language || env->ExceptionCheck()) return finish(false);
    const std::string selected_language = JavaString(env, language);
    if (env->ExceptionCheck() || selected_language.empty()) return finish(false);
    SetBl2LocaleLanguage(selected_language.c_str());
    IndexBl2CacheBase(base_apk_path);

    std::vector<std::shared_ptr<Entry>> entries;
    std::vector<ObbCandidate> candidates;
    std::vector<ObbSplitScanResult> scans;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    const auto keep_scanning = [](const void* value) {
        return std::chrono::steady_clock::now() <
            *static_cast<const std::chrono::steady_clock::time_point*>(value);
    };
    const auto external = OpenExternalObbPair(s_obb_root.c_str(), keep_scanning, &deadline);
    if (external.status != ObbSplitScanStatus::NoObbEntry && external.status != ObbSplitScanStatus::Verified) {
        VFS_ERROR("External OBB pair rejected: %s", external.reason);
        return finish(false);
    }
    if (external.status == ObbSplitScanStatus::Verified) {
        for (size_t i = 0; i < 2; ++i) {
            auto entry = std::make_shared<Entry>();
            entry->fd = external.fds[i];
            entry->name = std::string(i == 0 ? kMainObbName : kPatchObbName);
            entry->size = entry->apk_size = external.sizes[i];
            s_obbs[i] = std::move(entry);
        }
        SetBl2LocaleMainObb(s_obbs[0]->fd, 0, s_obbs[0]->size);
        VFS_LOG("Direct external OBB pair selected: %s", s_obb_root.c_str());
    }
    for (const auto& path : paths) {
        if (external.status == ObbSplitScanStatus::Verified) break;
        auto entry = std::make_shared<Entry>();
        entry->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (entry->fd < 0) return finish(false);
        // ApplicationInfo supplied these PackageManager-vetted installed APKs.
        // Keep exact manifest/layout/ZIP bounds; no multi-GB SHA/CRC pass.
        const auto scan = ScanObbSplitFd(entry->fd, keep_scanning, &deadline,
                                        HasBl2RelocatedCache(), false);
        if (scan.status == ObbSplitScanStatus::NoObbEntry) continue;
        if (scan.status != ObbSplitScanStatus::Verified) {
            VFS_ERROR("Split rejected: %s reason=%s bytes=%llu", path.c_str(), scan.reason,
                      static_cast<unsigned long long>(scan.bytes_hashed));
            return finish(false);
        }
        struct stat st{};
        if (::fstat(entry->fd, &st) != 0 || st.st_size < 0) return finish(false);
        entry->name = scan.main ? std::string(kMainObbName) : std::string(kPatchObbName);
        entry->base = scan.apk_data_offset;
        entry->size = scan.obb_size;
        entry->apk_size = static_cast<uint64_t>(st.st_size);
        if (!ResolveVirtualFileRange(entry->base, entry->size, 0,
                                     entry->size, entry->apk_size).valid) return finish(false);
        entries.push_back(std::move(entry));
        scans.push_back(scan);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        candidates.push_back({ObbSource::EmbeddedSplits, entries[i]->name,
                              scans[i].sha256.data(), entries[i]->size, true});
    }
    if (!entries.empty()) {
        const auto pair = SelectCompleteObbPair(candidates);
        if (!pair.valid) { VFS_ERROR("Incomplete or duplicate embedded main/patch pair"); return finish(false); }
        s_obbs = {entries[pair.main_index], entries[pair.patch_index]};
        SetBl2LocaleMainObb(s_obbs[0]->fd, s_obbs[0]->base, s_obbs[0]->size);
        VFS_LOG("Direct split pair selected: main=%llu patch=%llu; no cache extraction",
                static_cast<unsigned long long>(s_obbs[0]->size),
                static_cast<unsigned long long>(s_obbs[1]->size));
    } else if (external.status != ObbSplitScanStatus::Verified) {
        VFS_LOG("No cache splits: native external OBB mode at %s", s_obb_root.c_str());
    }
    std::vector<const char*> locale_paths{base_apk_path};
    for (const auto& path : paths) locale_paths.push_back(path.c_str());
    IndexBl2LocaleSplits(locale_paths.data(), locale_paths.size(), s_files_root.c_str());
    s_initialized = finish(true);
    return s_initialized;
}

void RegisterNativeVfsThunks(ThunkManager& manager) {
    manager.RegisterThunk("open", [](Jit& j, Host&) {
        j.Regs()[0] = NativeOpen(Pointer<const char>(j.Regs()[0]),
                                 static_cast<int>(j.Regs()[1]), j.Regs()[2]);
    });
    manager.RegisterThunk("close", [](Jit& j, Host&) { j.Regs()[0] = NativeClose(j.Regs()[0]); });
    manager.RegisterThunk("read", [](Jit& j, Host&) {
        j.Regs()[0] = NativeRead(j.Regs()[0], Pointer<void>(j.Regs()[1]), j.Regs()[2]);
    });
    manager.RegisterThunk("pread", [](Jit& j, Host&) {
        j.Regs()[0] = NativePread(j.Regs()[0], Pointer<void>(j.Regs()[1]), j.Regs()[2],
                                  static_cast<int32_t>(j.Regs()[3]));
    });
    manager.RegisterThunk("pread64", [](Jit& j, Host& h) {
        const int64_t offset = Joined64(h.MemoryRead32(j.Regs()[13]), h.MemoryRead32(j.Regs()[13] + 4));
        j.Regs()[0] = NativePread(j.Regs()[0], Pointer<void>(j.Regs()[1]), j.Regs()[2], offset);
    });
    manager.RegisterThunk("lseek", [](Jit& j, Host&) {
        j.Regs()[0] = NativeSeek(j.Regs()[0], static_cast<int32_t>(j.Regs()[1]), j.Regs()[2], true);
    });
    manager.RegisterThunk("lseek64", [](Jit& j, Host& h) {
        Return64(j, NativeSeek(j.Regs()[0], Joined64(j.Regs()[2], j.Regs()[3]),
                              h.MemoryRead32(j.Regs()[13]), false));
    });
    manager.RegisterThunk("fopen", [](Jit& j, Host&) {
        j.Regs()[0] = Address(NativeFopen(Pointer<const char>(j.Regs()[0]), Pointer<const char>(j.Regs()[1])));
    });
    manager.RegisterThunk("fdopen", [](Jit& j, Host&) {
        const int fd = j.Regs()[0];
        const char* mode = Pointer<const char>(j.Regs()[1]);
        const auto state = FindFile(fd);
        if (state && !ReadOnlyMode(mode)) { errno = EROFS; j.Regs()[0] = 0; return; }
        j.Regs()[0] = Address(state ? EntryStream(state) : ::fdopen(fd, mode));
    });
    manager.RegisterThunk("fileno", [](Jit& j, Host&) {
        FILE* stream = Pointer<FILE>(j.Regs()[0]);
        std::lock_guard lock(s_files_mutex);
        const auto found = s_streams.find(stream);
        j.Regs()[0] = found == s_streams.end() ? ::fileno(stream) : found->second->fd;
    });
    manager.RegisterThunk("fclose", [](Jit& j, Host&) { j.Regs()[0] = ::fclose(Pointer<FILE>(j.Regs()[0])); });
    manager.RegisterThunk("fread", [](Jit& j, Host&) {
        j.Regs()[0] = ::fread(Pointer<void>(j.Regs()[0]), j.Regs()[1], j.Regs()[2], Pointer<FILE>(j.Regs()[3]));
    });
    manager.RegisterThunk("fseek", [](Jit& j, Host&) {
        j.Regs()[0] = ::fseek(Pointer<FILE>(j.Regs()[0]), static_cast<int32_t>(j.Regs()[1]), j.Regs()[2]);
    });
    manager.RegisterThunk("ftell", [](Jit& j, Host&) { j.Regs()[0] = ::ftell(Pointer<FILE>(j.Regs()[0])); });
    manager.RegisterThunk("stat", [](Jit& j, Host&) {
        const char* path = Pointer<const char>(j.Regs()[0]);
        auto* output = Pointer<struct stat>(j.Regs()[1]);
        const int result = NativeStat(path, output);
#if defined(BL2_NATIVE_ARM32) && BL2_ENABLE_QA_CONSOLE
        if (NativeQaDlcPath(path)) {
            static std::atomic<unsigned> qaRows{0};
            const unsigned qaRow = qaRows.fetch_add(1, std::memory_order_relaxed) + 1;
            if (qaRow <= 64) {
                const int savedErrno = errno;
                __android_log_print(ANDROID_LOG_INFO, "BL2_NativeDlcQa",
                    "stat row=%u lr=%08x path=%.240s result=%d errno=%d mode=%08x size=%lld",
                    qaRow, j.Regs()[14], path, result, savedErrno,
                    result == 0 ? static_cast<unsigned>(output->st_mode) : 0u,
                    result == 0 ? static_cast<long long>(output->st_size) : -1LL);
                errno = savedErrno;
            }
        }
#endif
        j.Regs()[0] = result;
    });
    manager.RegisterThunk("lstat", [](Jit& j, Host&) {
        const char* path = Pointer<const char>(j.Regs()[0]);
        auto* output = Pointer<struct stat>(j.Regs()[1]);
        if (!path || !output) { errno = EFAULT; j.Regs()[0] = static_cast<uint32_t>(-1); return; }
        j.Regs()[0] = Bl2OrchidStat(path, *output) ? 0 : ::lstat(path, output);
    });
    manager.RegisterThunk("fstat", [](Jit& j, Host&) {
        j.Regs()[0] = NativeFstat(j.Regs()[0], Pointer<struct stat>(j.Regs()[1]));
    });
    manager.RegisterThunk("access", [](Jit& j, Host&) {
        const char* path = Pointer<const char>(j.Regs()[0]);
        if (!path) { errno = EFAULT; j.Regs()[0] = static_cast<uint32_t>(-1); return; }
        const int mode = j.Regs()[1];
        int orchid_result = 0;
        if (Bl2OrchidAccess(path, mode, orchid_result)) {
            j.Regs()[0] = static_cast<uint32_t>(orchid_result); return;
        }
        auto entry = DirectEntry(path);
        if (!entry && ::access(path, mode) == 0) { j.Regs()[0] = 0; return; }
        if (!entry) entry = AssetEntry(path);
        if (entry && !(mode & (W_OK | X_OK))) { j.Regs()[0] = 0; return; }
        if (entry) errno = EACCES;
        j.Regs()[0] = static_cast<uint32_t>(-1);
    });
    manager.RegisterThunk("mmap", [](Jit& j, Host& h) {
        j.Regs()[0] = Address(NativeMmap(Pointer<void>(j.Regs()[0]), j.Regs()[1], j.Regs()[2], j.Regs()[3],
            h.MemoryRead32(j.Regs()[13]), static_cast<int32_t>(h.MemoryRead32(j.Regs()[13] + 4))));
    });
    manager.RegisterThunk("mmap64", [](Jit& j, Host& h) {
        j.Regs()[0] = Address(NativeMmap(Pointer<void>(j.Regs()[0]), j.Regs()[1], j.Regs()[2], j.Regs()[3],
            h.MemoryRead32(j.Regs()[13]), Joined64(h.MemoryRead32(j.Regs()[13] + 8), h.MemoryRead32(j.Regs()[13] + 12))));
    });
    manager.RegisterThunk("munmap", [](Jit& j, Host&) { j.Regs()[0] = NativeMunmap(Pointer<void>(j.Regs()[0]), j.Regs()[1]); });
    manager.RegisterThunk("opendir", [](Jit& j, Host& h) {
        EnsureBl2OrchidMount(h);
        const char* path = Pointer<const char>(j.Regs()[0]);
#if defined(BL2_NATIVE_ARM32) && BL2_ENABLE_QA_CONSOLE
        static std::atomic<unsigned> qaRows{0};
        static std::atomic<unsigned> qaDlcRows{0};
        const unsigned qaRow = qaRows.fetch_add(1, std::memory_order_relaxed) + 1;
        const unsigned qaDlcRow = NativeQaDlcPath(path)
            ? qaDlcRows.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        const bool qaLog = qaRow <= 64 || (qaDlcRow != 0 && qaDlcRow <= 64);
        if (qaLog) {
            const int savedErrno = errno;
            const auto kind = Bl2OrchidKind(path);
            const auto* children = Bl2OrchidChildren(path);
            __android_log_print(ANDROID_LOG_INFO, "BL2_NativeDlcQa",
                "opendir row=%u dlcRow=%u stage=before lr=%08x path=%.240s kind=%u children=%zu",
                qaRow, qaDlcRow, j.Regs()[14], path ? path : "(null)",
                static_cast<unsigned>(kind), children ? children->size() : 0);
            errno = savedErrno;
        }
#endif
        DIR* result = NativeOpendir(path);
#if defined(BL2_NATIVE_ARM32) && BL2_ENABLE_QA_CONSOLE
        if (qaLog) {
            const int savedErrno = errno;
            __android_log_print(ANDROID_LOG_INFO, "BL2_NativeDlcQa",
                "opendir row=%u dlcRow=%u stage=after dir=%p errno=%d",
                qaRow, qaDlcRow, static_cast<void*>(result), savedErrno);
            errno = savedErrno;
        }
#endif
        j.Regs()[0] = Address(result);
    });
    manager.RegisterThunk("readdir", [](Jit& j, Host&) { j.Regs()[0] = Address(NativeReaddir(Pointer<DIR>(j.Regs()[0]))); });
    manager.RegisterThunk("closedir", [](Jit& j, Host&) {
        DIR* dir = Pointer<DIR>(j.Regs()[0]);
        { std::lock_guard lock(s_directories_mutex); s_directories.erase(dir); }
        j.Regs()[0] = ::closedir(dir);
    });
    manager.RegisterThunk("chdir", [](Jit& j, Host&) {
        const char* path = Pointer<const char>(j.Regs()[0]);
        if (!path) { errno = EFAULT; j.Regs()[0] = static_cast<uint32_t>(-1); return; }
        if (::chdir(path) == 0) { j.Regs()[0] = 0; return; }
        const int requested_errno = errno;
        if (!s_files_root.empty() && ::chdir(s_files_root.c_str()) == 0) {
            VFS_LOG("chdir: redirected '%s' to app files '%s'", path, s_files_root.c_str());
            j.Regs()[0] = 0;
            return;
        }
        errno = requested_errno ? requested_errno : errno;
        j.Regs()[0] = static_cast<uint32_t>(-1);
    });
    manager.RegisterThunk("mkdir", [](Jit& j, Host&) {
        const char* path = Pointer<const char>(j.Regs()[0]);
        if (!path) { errno = EFAULT; j.Regs()[0] = static_cast<uint32_t>(-1); return; }
        if (IsLegacyConfigDirectory(path) && !s_files_root.empty()) {
            const auto local = s_files_root + "/Config";
            j.Regs()[0] = ::mkdir(local.c_str(), static_cast<mode_t>(j.Regs()[1] & 0777u));
        } else {
            j.Regs()[0] = ::mkdir(path, j.Regs()[1]);
        }
    });
}
