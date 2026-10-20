#include "bl2_locale.h"

#ifndef BL2_ENABLE_DLC_CONTENT
#define BL2_ENABLE_DLC_CONTENT 1
#endif
#include "bl2_locale_payload.h"
#include "bl2_voice_payload.h"
#include "bl2_cache_payload.h"
#include "bl2_bink_audio.h"
#include "bl2_coalesced_dlc.h"
#include "bl2_orchid_admission_data.h"
#include "bl2_sage_admission_data.h"
#include "bl2_iris_admission_data.h"
#include "bl2_orchid_root.h"
#if defined(BL2_LOCALE_HOST_TEST)
#include "bl2_locale_host_port.h"
#else
#include "bridge_log.h"
#if defined(BL2_NATIVE_ARM32)
#include "native_host.h"
#else
#include "dynarmic_host.h"
#include "elf_loader.h"
#endif
#include "thunk_manager.h"
#endif
#include "obb_sha256.h"
#include "obb_pair_selection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#if !defined(BL2_LOCALE_HOST_TEST)
#include <unistd.h>
#endif
#include <utility>
#include <vector>
#include <zlib.h>
#if !defined(_WIN32)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

#define LOG_TAG "Arm32Bridge_Locale"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)
#if BL2_ENABLE_QA_CONSOLE && !defined(BL2_LOCALE_HOST_TEST)
#include <android/log.h>
#undef LOGI
#undef LOGE
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

namespace {
using namespace bl2_locale_payload;
constexpr char kDescriptor[] = "assets/bl2_locale/ru_text/pack.json";
constexpr char kVoiceDescriptor[] = "assets/bl2_locale/ru_voice/pack.json";
constexpr char kGuestSha256[] =
    "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
constexpr uint32_t kLookupRva = 0x01E1CA4C;
constexpr uint32_t kLookupPreimage[] = {0xE92D43F8, 0xE1A07000,
                                      0xE59F30B0, 0xE08F3003};
constexpr uint32_t kPackageFindRva = 0x00F3EB30, kPackageCacheRva = 0x00F45BA4;
constexpr uint32_t kPackageFindPreimage[] = {0xE92D4FF0, 0xE24DDF49, 0xE59DC148, 0xE1A07000};
constexpr uint32_t kPackageCachePreimage[] = {0xE92D4FF0, 0xE24DD07C, 0xE1A07000, 0xE59F4414};
constexpr uint32_t kPackageCacheVtableRva = 0x02D27258;
constexpr size_t kMaxPath = 4096;
constexpr size_t kTextFileCount = std::size(kFiles);
constexpr size_t kVoiceFileCount = std::size(bl2_voice_payload::kFiles);
constexpr size_t kLocaleFileCount = kTextFileCount + kVoiceFileCount;
constexpr size_t kFileCount = kLocaleFileCount + std::size(bl2_cache_payload::kFiles);
[[maybe_unused]] constexpr size_t kBindingCount = std::size(kBindings) + std::size(bl2_voice_payload::kBindings);
std::array<Bl2LocaleEntry, kFileCount> s_entries{};
std::array<Bl2BinkAudio, kVoiceFileCount> s_bink_movies{};
int s_main_fd = -1;
uint64_t s_main_offset = 0, s_main_bytes = 0;
std::array<std::atomic<uint32_t>, kFileCount> s_opens{};
std::atomic<bool> s_active{false};
bool s_indexed = false;
bool s_payload_valid = false;
bool s_guest_valid = false;
bool s_cache_valid = false;
bool s_russian = true;  // Legacy callers; the ARM64 Context selects ru/en before indexing.
int s_cache_fd = -1;
std::array<int, 2> s_apk_fds{{-1, -1}};
std::array<bool, 2> s_component_valid{};
std::string s_files_root;
std::string s_obb_directory;
struct ExternalEntry { std::string path; Bl2LocaleEntry entry{}; };
std::vector<ExternalEntry> s_external_entries;
std::vector<int> s_external_fds;
int s_composed_locale_fd = -1;
int s_orchid_voice_fd = -1;
std::array<Bl2LocaleEntry, 2> s_orchid_voice_entries{};
constexpr std::array<const char*, 2> kOrchidVoicePaths{{
    "WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks_Orchid.pck",
    "WillowGame/CookedAndroid_DXT/English(US)/Audio_Streaming_Orchid.pck"}};
std::atomic<uint32_t> s_external_opens{0};
bl2_orchid::PathIndex s_orchid_index;
std::map<std::string, Bl2LocaleEntry> s_orchid_files;
std::vector<int> s_orchid_mount_fds;
std::string s_orchid_absolute_root;
std::atomic<bool> s_orchid_published{false};
std::mutex s_orchid_publish_mutex;
using OrchidRole = bl2_orchid_admission::Role;
constexpr size_t kOrchidRoleCount = static_cast<size_t>(OrchidRole::Count);
std::array<std::vector<bl2_orchid::FileRow>, kOrchidRoleCount> s_orchid_archive_rows;
std::vector<bl2_orchid::FileRow> s_orchid_pending_rows;
std::atomic<bool> s_orchid_admitted{false};
bool s_orchid_ownership_installed = false;  // Existing retained thunk; no new SVC IDs.
using SageRole = bl2_sage_admission::Role;
constexpr size_t kSageRoleCount = static_cast<size_t>(SageRole::Count);
std::array<std::vector<bl2_orchid::FileRow>, kSageRoleCount> s_sage_archive_rows;
std::vector<bl2_orchid::FileRow> s_sage_pending_rows;
std::atomic<bool> s_sage_admitted{false}, s_sage_published{false};
int s_sage_voice_fd = -1;
bool s_sage_text_composed = false;
std::array<Bl2LocaleEntry, 2> s_sage_voice_entries{};
constexpr std::array<const char*, 2> kSageVoicePaths{{
    "WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks_Sage.pck",
    "WillowGame/CookedAndroid_DXT/English(US)/Audio_Streaming_Sage.pck"}};
using IrisRole = bl2_iris_admission::Role;
constexpr size_t kIrisRoleCount = static_cast<size_t>(IrisRole::Count);
std::array<std::vector<bl2_orchid::FileRow>, kIrisRoleCount> s_iris_archive_rows;
std::vector<bl2_orchid::FileRow> s_iris_pending_rows;
std::atomic<bool> s_iris_admitted{false}, s_iris_published{false};
int s_iris_voice_fd = -1;
bool s_iris_text_composed = false;
std::array<Bl2LocaleEntry, 2> s_iris_voice_entries{};
constexpr std::array<const char*, 2> kIrisVoicePaths{{
    "WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks_Iris.pck",
    "WillowGame/CookedAndroid_DXT/English(US)/Audio_Streaming_Iris.pck"}};
bool AnyDlcPublished() {
    return s_orchid_published.load(std::memory_order_acquire) || s_sage_published.load(std::memory_order_acquire) || s_iris_published.load(std::memory_order_acquire);
}

std::string_view OrchidPath(const char* path) {
    if (!path) return {};
    size_t length = 0;
    while (length <= bl2_orchid::PathIndex::kMaxPath && path[length]) ++length;
    return length <= bl2_orchid::PathIndex::kMaxPath
        ? std::string_view(path, length) : std::string_view{};
}

Bl2LocaleEntry* OrchidFile(const char* path) {
    if (!AnyDlcPublished()) return nullptr;
    const auto* file = s_orchid_index.Find(OrchidPath(path), s_orchid_absolute_root);
    if (!file) return nullptr;
    const auto found = s_orchid_files.find(file->path);
    return found == s_orchid_files.end() ? nullptr : &found->second;
}

uint16_t U16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool Bounds(uint64_t total, uint64_t at, uint64_t length) {
    return at <= total && length <= total - at;
}
bool ReadExact(int fd, uint64_t size, uint64_t at, uint8_t* dest, size_t length) {
    if (!Bounds(size, at, length)) return false;
    size_t done = 0;
    while (done < length) {
        const ssize_t read = pread(fd, dest + done, length - done,
                                   static_cast<off_t>(at + done));
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) return false;
        done += static_cast<size_t>(read);
    }
    return true;
}

struct ZipEntry {
    std::string name;
    uint32_t bytes = 0;
    uint32_t packed = 0;
    uint32_t crc = 0;
    uint64_t offset = 0;
    uint16_t method = 0;
};
struct Zip {
    int fd = -1;
    uint64_t bytes = 0;
    bool trusted_installed = true;
    bool orchid_metadata = false;
    bool sage_metadata = false;
    bool iris_metadata = false;
    std::vector<ZipEntry> entries;
    ~Zip() { if (fd >= 0) close(fd); }

    const ZipEntry* Find(std::string_view name) const {
        for (const auto& entry : entries) if (entry.name == name) return &entry;
        return nullptr;
    }
    bool Open(const char* path) {
        fd = open(path, O_RDONLY | O_CLOEXEC);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 22 ||
            static_cast<uint64_t>(st.st_size) > 0xFFFFFFFFull) return false;
        bytes = static_cast<uint64_t>(st.st_size);
        std::vector<uint8_t> tail(static_cast<size_t>(std::min<uint64_t>(bytes, 65557)));
        const uint64_t tail_offset = bytes - tail.size();
        if (!ReadExact(fd, bytes, tail_offset, tail.data(), tail.size())) return false;
        size_t end = tail.size();
        for (size_t at = tail.size() - 22;; --at) {
            if (U32(tail.data() + at) == 0x06054B50 &&
                at + 22 + U16(tail.data() + at + 20) == tail.size()) {
                end = at;
                break;
            }
            if (at == 0) break;
        }
        if (end == tail.size()) return false;
        const uint8_t* eocd = tail.data() + end;
        const uint16_t count = U16(eocd + 10);
        const uint32_t cd_size = U32(eocd + 12), cd_offset = U32(eocd + 16);
        if (U16(eocd + 4) || U16(eocd + 6) || U16(eocd + 8) != count ||
            count == 0xFFFF || count > 8192 || cd_size > 8 * 1024 * 1024 ||
            cd_offset == 0xFFFFFFFF ||
            uint64_t(cd_offset) + cd_size != tail_offset + end) return false;
        std::vector<uint8_t> directory(cd_size);
        if (!ReadExact(fd, bytes, cd_offset, directory.data(), directory.size())) return false;
        size_t at = 0;
        std::vector<std::pair<uint64_t, uint64_t>> spans;
        for (uint16_t i = 0; i < count; ++i) {
            if (!Bounds(directory.size(), at, 46) || U32(directory.data() + at) != 0x02014B50) return false;
            const uint8_t* c = directory.data() + at;
            const uint16_t name_len = U16(c + 28), extra_len = U16(c + 30), comment_len = U16(c + 32);
            const size_t record_size = 46u + name_len + extra_len + comment_len;
            if (!Bounds(directory.size(), at, record_size) || U16(c + 34)) return false;
            const std::string name(reinterpret_cast<const char*>(c + 46), name_len);
            at += record_size;
            // Only these records participate in the locale contract. Other
            // package resources remain Android's responsibility.
            if (name != "AndroidManifest.xml" &&
                name != "bl2-content.index" && name != "bl2-content.json" &&
                !std::string_view(name).starts_with("WillowGame/") &&
                !(orchid_metadata && std::string_view(name).starts_with("DLC/Orchid/")) &&
                !(sage_metadata && std::string_view(name).starts_with("DLC/Sage/")) &&
                !(iris_metadata && std::string_view(name).starts_with("DLC/Iris/")) &&
                !std::string_view(name).starts_with("assets/bl2_locale/") &&
                !std::string_view(name).starts_with("assets/bl2_cache/")) {
                if (!trusted_installed) return false;
                continue;
            }
            if (name.find('\0') != std::string::npos || name.size() > 512 || Find(name)) return false;
            const uint16_t flags = U16(c + 8), method = U16(c + 10);
            const uint32_t local_offset = U32(c + 42);
            const uint32_t packed = U32(c + 20), unpacked = U32(c + 24);
            uint8_t local[30]{};
            if ((flags & ~0x0808u) || (method != 0 && method != 8) ||
                local_offset == 0xFFFFFFFF || packed == 0xFFFFFFFF || unpacked == 0xFFFFFFFF ||
                !Bounds(cd_offset, local_offset, sizeof(local)) ||
                !ReadExact(fd, bytes, local_offset, local, sizeof(local)) || U32(local) != 0x04034B50 ||
                U16(local + 6) != flags || U16(local + 8) != method || U16(local + 26) != name_len) return false;
            const uint64_t data = uint64_t(local_offset) + 30 + name_len + U16(local + 28);
            std::vector<uint8_t> local_name(name_len);
            if (!Bounds(cd_offset, data, packed) ||
                !ReadExact(fd, bytes, uint64_t(local_offset) + 30, local_name.data(), name_len) ||
                std::memcmp(local_name.data(), name.data(), name_len) != 0 ||
                (method == 0 && packed != unpacked)) return false;
            const bool locale = std::string_view(name).starts_with("assets/bl2_locale/");
            if (!(flags & 8) && (((!locale || !trusted_installed) && U32(local + 14) != U32(c + 16)) ||
                U32(local + 18) != packed || U32(local + 22) != unpacked)) return false;
            entries.push_back({name, unpacked, packed, U32(c + 16), data, method});
            spans.emplace_back(local_offset, data + packed);
        }
        if (at != directory.size()) return false;
        std::sort(spans.begin(), spans.end());
        for (size_t i = 1; i < spans.size(); ++i) if (spans[i].first < spans[i-1].second) return false;
        return true;
    }
    bool ReadSmall(const ZipEntry& entry, std::vector<uint8_t>& out) const {
        if (entry.bytes > 1024 * 1024 || entry.packed > 1024 * 1024) return false;
        std::vector<uint8_t> packed(entry.packed);
        if (!ReadExact(fd, bytes, entry.offset, packed.data(), packed.size())) return false;
        if (entry.method == 0) out = std::move(packed);
        else {
            out.resize(entry.bytes);
            z_stream stream{};
            stream.next_in = packed.data();
            stream.avail_in = static_cast<uInt>(packed.size());
            stream.next_out = out.data();
            stream.avail_out = static_cast<uInt>(out.size());
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
            const int result = inflate(&stream, Z_FINISH);
            const bool exact = result == Z_STREAM_END && stream.total_in == packed.size() && stream.total_out == out.size();
            inflateEnd(&stream);
            if (!exact) return false;
        }
        return out.size() == entry.bytes && crc32(0, out.data(), static_cast<uInt>(out.size())) == entry.crc;
    }
    bool Verify(const ZipEntry& entry, uint64_t expected_size, const char* digest) const {
        if (entry.method != 0 || entry.bytes != expected_size || entry.packed != expected_size) return false;
#if !defined(BL2_LOCALE_HOST_TEST)
        // Installed signed APKs already carry authenticated payloads. Hash only
        // the small pack descriptor used to select this exact compatible locale.
        if (trusted_installed && expected_size > 65536) return true;
#endif
        std::array<uint8_t, 65536> buffer;
        ObbSha256 hash;
        uLong crc = 0;
        for (uint64_t at = 0; at < entry.bytes;) {
            const size_t length = static_cast<size_t>(std::min<uint64_t>(buffer.size(), entry.bytes - at));
            if (!ReadExact(fd, bytes, entry.offset + at, buffer.data(), length) || !hash.Update(buffer.data(), length)) return false;
            crc = crc32(crc, buffer.data(), static_cast<uInt>(length));
            at += length;
        }
        return crc == entry.crc && std::strcmp(ObbSha256::Hex(hash.Finalize()).data(), digest) == 0;
    }
};

// The external index is deliberately small: SHA256<TAB>bytes<TAB>exact ZIP name.
bool VerifyExternalIndex(Zip& zip, std::string_view prefix, std::vector<const ZipEntry*>& files) {
    zip.trusted_installed = false;
    const auto* index = zip.Find("bl2-content.index");
    std::vector<uint8_t> body;
    if (!index || index->method != 0 || !zip.ReadSmall(*index, body) || body.empty() || body.back() != '\n') return false;
    std::string_view lines(reinterpret_cast<const char*>(body.data()), body.size());
    while (!lines.empty()) {
        const size_t newline = lines.find('\n');
        if (newline == std::string_view::npos) return false;
        const std::string_view line = lines.substr(0, newline);
        lines.remove_prefix(newline + 1);
        const size_t tab = line.find('\t', 65);
        if (line.size() < 68 || line[64] != '\t' || tab == std::string_view::npos || tab == 65) return false;
        for (const char c : line.substr(0, 64)) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
        uint64_t bytes = 0;
        for (const char c : line.substr(65, tab - 65)) {
            if (c < '0' || c > '9' || bytes > (0xFFFFFFFFull - (c - '0')) / 10) return false;
            bytes = bytes * 10 + (c - '0');
        }
        const auto name = line.substr(tab + 1);
        if (!name.starts_with(prefix) || name.size() >= sizeof(Bl2LocaleEntry{}.name) ||
            name.find('\0') != std::string_view::npos || name.find('\\') != std::string_view::npos ||
            name.find("..") != std::string_view::npos || name.find("//") != std::string_view::npos) return false;
        const auto* entry = zip.Find(name);
        if (!entry || std::find(files.begin(), files.end(), entry) != files.end()) return false;
        const std::string digest(line.substr(0, 64));
        if (!zip.Verify(*entry, bytes, digest.c_str())) return false;
        files.push_back(entry);
    }
    if (files.empty()) return false;
    for (const auto& entry : zip.entries) {
        if (entry.name == "bl2-content.index" || entry.name == "bl2-content.json") continue;
        if (std::find(files.begin(), files.end(), &entry) == files.end()) return false;
    }
    return true;
}

// Small binary-XML reader for installed APK identity. Using manifest strings
// works on API24/25 too; ApplicationInfo.splitNames is not accessed.
struct Strings {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    uint32_t count = 0, offsets = 0, start = 0;
    bool utf8 = false;
    bool Load(const uint8_t* p, size_t size) {
        if (size < 28 || U16(p + 2) < 28 || U32(p + 4) != size) return false;
        count = U32(p + 8); offsets = U16(p + 2); start = U32(p + 20);
        if (count > 4096 || !Bounds(size, offsets, uint64_t(count) * 4) || start > size) return false;
        data = p; bytes = size; utf8 = (U32(p + 16) & 0x100) != 0;
        return true;
    }
    bool Get(uint32_t index, std::string& out) const {
        out.clear();
        if (!data || index >= count) return false;
        uint64_t at = uint64_t(start) + U32(data + offsets + index * 4);
        if (utf8) {
            const auto length = [&](uint32_t& n) {
                if (!Bounds(bytes, at, 1)) return false;
                n = data[at++];
                if (n & 0x80) {
                    if (!Bounds(bytes, at, 1)) return false;
                    n = ((n & 0x7F) << 8) | data[at++];
                }
                return true;
            };
            uint32_t chars = 0, size = 0;
            if (!length(chars) || !length(size) || size > 4096 || !Bounds(bytes, at, uint64_t(size) + 1) || data[at + size]) return false;
            out.assign(reinterpret_cast<const char*>(data + at), size);
            return out.find('\0') == std::string::npos;
        }
        if (!Bounds(bytes, at, 2)) return false;
        uint32_t length = U16(data + at); at += 2;
        if (length & 0x8000) {
            if (!Bounds(bytes, at, 2)) return false;
            length = ((length & 0x7FFF) << 16) | U16(data + at); at += 2;
        }
        if (length > 4096 || !Bounds(bytes, at, uint64_t(length + 1) * 2) || U16(data + at + length * 2)) return false;
        // Names and the manifest identities in this code are ASCII. Refuse
        // non-ASCII values instead of silently truncating UTF16 into bytes.
        for (uint32_t i = 0; i < length; ++i) {
            const uint16_t c = U16(data + at + i * 2);
            if (c == 0 || c > 127) return false;
            out.push_back(static_cast<char>(c));
        }
        return true;
    }
};

bool ManifestIdentity(const std::vector<uint8_t>& xml, std::string& split) {
    split.clear();
    if (xml.size() < 8 || U16(xml.data()) != 3 || U16(xml.data()+2) != 8 || U32(xml.data()+4) != xml.size()) return false;
    Strings strings;
    bool manifest_seen = false, application_seen = false, has_code = true;
    std::string package;
    uint32_t version = 0;
    for (size_t at = 8; at < xml.size();) {
        if (!Bounds(xml.size(), at, 8)) return false;
        const uint8_t* c = xml.data() + at;
        const uint16_t type = U16(c), header = U16(c + 2);
        const uint32_t size = U32(c + 4);
        if (header < 8 || size < header || !Bounds(xml.size(), at, size)) return false;
        if (type == 1) {
            if (strings.data || !strings.Load(c, size)) return false;
        } else if (type == 0x0102) {
            if (header != 16 || size < 36) return false;
            std::string element;
            if (!strings.Get(U32(c + 20), element)) return false;
            const uint16_t attr_start = U16(c + 24), attr_size = U16(c + 26), attr_count = U16(c + 28);
            if (attr_start < 20 || attr_size != 20 || attr_count > 128 || !Bounds(size, 16u + attr_start, uint64_t(attr_size)*attr_count)) return false;
            const bool root = element == "manifest", app = element == "application";
            if (root && std::exchange(manifest_seen, true)) return false;
            if (app && std::exchange(application_seen, true)) return false;
            bool saw_package = false, saw_split = false, saw_version = false, saw_code = false;
            for (uint16_t i = 0; i < attr_count; ++i) {
                const uint8_t* a = c + 16 + attr_start + i * attr_size;
                std::string name, ns, value;
                if (!strings.Get(U32(a + 4), name) || U16(a + 12) != 8) return false;
                if (U32(a) != 0xFFFFFFFF && !strings.Get(U32(a), ns)) return false;
                const auto text_value = [&]() { return a[15] == 3 && strings.Get(U32(a + 16), value); };
                if (root && name == "package" && ns.empty()) {
                    if (std::exchange(saw_package, true) || !text_value()) return false;
                    package = value;
                } else if (root && name == "split" && ns.empty()) {
                    if (std::exchange(saw_split, true) || !text_value()) return false;
                    split = value;
                } else if (root && name == "versionCode" && ns == "http://schemas.android.com/apk/res/android") {
                    if (std::exchange(saw_version, true) || (a[15] != 0x10 && a[15] != 0x11)) return false;
                    version = U32(a + 16);
                } else if (app && name == "hasCode" && ns == "http://schemas.android.com/apk/res/android") {
                    if (std::exchange(saw_code, true) || a[15] != 0x12) return false;
                    has_code = U32(a + 16) != 0;
                }
            }
        }
        at += size;
    }
    const bool locale_split = split == "ru_text" || split == "ru_voice";
    return manifest_seen && (!locale_split || (application_seen && !has_code &&
        package == "com.tt2kgames.Borderlands2" && version == 33));
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
bool EqualFold(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}
bool PrefixFold(std::string_view a, std::string_view prefix) {
    return a.size() >= prefix.size() && EqualFold(a.substr(0, prefix.size()), prefix);
}

bool Normalize(const char* input, std::string& result) {
    result.clear();
    if (!input) return false;
    const size_t length = strnlen(input, kMaxPath);
    if (length == kMaxPath || length == 0) return false;
    std::string path;
    path.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        char c = input[i] == '\\' ? '/' : input[i];
        if (c == '/' && !path.empty() && path.back() == '/') continue;
        path.push_back(c);
    }
    std::string_view relative = path;
    if (relative.front() == '/') {
        bool root = false;
        for (std::string_view prefix : {std::string_view("/sdcard/Borderlands2/"),
                std::string_view("/mnt/sdcard/Borderlands2/")}) {
            if (relative.starts_with(prefix)) { relative.remove_prefix(prefix.size()); root = true; break; }
        }
        if (!root && !s_files_root.empty() && relative.starts_with(s_files_root) &&
            relative.size() > s_files_root.size() && relative[s_files_root.size()] == '/') {
            relative.remove_prefix(s_files_root.size() + 1); root = true;
        }
        if (!root && relative.starts_with("/storage/emulated/")) {
            constexpr size_t start = sizeof("/storage/emulated/") - 1;
            size_t pos = start;
            while (pos < relative.size() && relative[pos] >= '0' && relative[pos] <= '9') ++pos;
            constexpr std::string_view suffix = "/Borderlands2/";
            if (pos > start && relative.substr(pos).starts_with(suffix)) {
                relative.remove_prefix(pos + suffix.size()); root = true;
            }
        }
        if (!root) return false;
    } else {
        while (relative.starts_with("./")) relative.remove_prefix(2);
        // This is UE3's documented game-relative form, not arbitrary parent
        // traversal against the process cwd.
        if (PrefixFold(relative, "../../WillowGame/")) relative.remove_prefix(6);
    }
    if (PrefixFold(relative, "CookedAndroid_DXT/")) result = "WillowGame/";
    else if (!PrefixFold(relative, "WillowGame/")) return false;
    std::vector<std::string_view> parts;
    if (!result.empty()) parts.push_back("WillowGame");
    while (!relative.empty()) {
        const size_t slash = relative.find('/');
        const auto piece = relative.substr(0, slash);
        if (piece == "..") {
            if (parts.size() <= 1) return false;
            parts.pop_back();
        } else if (!piece.empty() && piece != ".") parts.push_back(piece);
        if (slash == std::string_view::npos) break;
        relative.remove_prefix(slash + 1);
    }
    result.clear();
    for (const auto piece : parts) { if (!result.empty()) result.push_back('/'); result.append(piece); }
    return true;
}

size_t FileForPath(const char* path) {
    std::string key;
    if (!Normalize(path, key)) return kFileCount;
    for (const auto& binding : kBindings) if (EqualFold(key, binding.path)) return binding.file;
    for (const auto& binding : bl2_voice_payload::kBindings)
        if (EqualFold(key, binding.path)) return kTextFileCount + binding.file;
    for (size_t i = 0; i < std::size(bl2_cache_payload::kFiles); ++i)
        if (EqualFold(key, bl2_cache_payload::kFiles[i].path)) return kLocaleFileCount + i;
    return kFileCount;
}

// Russian movie audio (.bka) is spliced into the video of the main OBB. Without
// the embedded split pair (OBB files copied to Android/obb by hand) the same
// main OBB is read from there.
void OpenExternalMainObb() {
    if (s_main_fd >= 0) return;
    if (!s_obb_directory.empty()) {
        const std::string path = s_obb_directory + "/" + std::string(kMainObbName);
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st{};
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size == static_cast<off_t>(kMainObbSize)) {
            s_main_fd = fd; s_main_offset = 0; s_main_bytes = static_cast<uint64_t>(st.st_size);
            return;
        }
        if (fd >= 0) close(fd);
    }
    for (const std::string_view directory : {std::string_view("/storage/emulated/0/Android/obb/com.tt2kgames.Borderlands2/"),
                                             std::string_view("/sdcard/Android/obb/com.tt2kgames.Borderlands2/")}) {
        const std::string path = std::string(directory) + std::string(kMainObbName);
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st{};
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            s_main_fd = fd; s_main_offset = 0; s_main_bytes = static_cast<uint64_t>(st.st_size);
            LOGI("voice movies read their video from the external main OBB: %s", path.c_str());
            return;
        }
        close(fd);
    }
}

bool IndexLocaleComponent(Zip& zip, size_t component) {
    const bool voice = component == 1;
    const auto* descriptor = zip.Find(voice ? kVoiceDescriptor : kDescriptor);
    // Android verifies installed APK signatures; locale assets need only valid ZIP ranges.
    if (!descriptor || descriptor->method != 0) return false;
    const size_t first = voice ? kTextFileCount : 0;
    const size_t stop = first + (voice ? kVoiceFileCount : kTextFileCount);
    std::array<Bl2LocaleEntry, kFileCount> candidate{};
    [[maybe_unused]] size_t movies = 0, russianMovies = 0;  // Diagnostic builds log both.
    for (size_t f = first; f < stop; ++f) {
        const char* asset = voice ? bl2_voice_payload::kFiles[f - first].asset : kFiles[f].asset;
        const std::string_view name(asset);
        const bool movie = voice && name.ends_with(".bka");
        movies += movie;
        const auto* entry = zip.Find(asset);
        // Voice splits before R3 carried every movie whole (.bik).
        if (!entry && movie) entry = zip.Find(std::string(name.substr(0, name.size() - 4)) + ".bik");
        if (!entry || entry->method != 0 ||
            entry->name.size() >= sizeof(candidate[f].name)) {
            // The dialogue banks are required. A movie without its Russian
            // audio keeps the original track, like the lines left in English.
            if (movie) continue;
            return false;
        }
        std::memcpy(candidate[f].name, entry->name.c_str(), entry->name.size() + 1);
        candidate[f].data_offset = static_cast<off_t>(entry->offset);
        candidate[f].file_size = static_cast<off_t>(entry->bytes);
        if (movie && std::string_view(entry->name).ends_with(".bka")) {
            OpenExternalMainObb();
            auto& bink = s_bink_movies[f - first];
            if (!bink.Load(zip.fd, entry->offset, entry->bytes,
                           s_main_fd, s_main_offset, s_main_bytes)) {
                LOGE("voice movie keeps its original audio (no matching main OBB video): %s", asset);
                candidate[f] = {};
                continue;
            }
            candidate[f].bink = &bink;
            candidate[f].file_size = static_cast<off_t>(bink.Size());
        }
        russianMovies += movie;
    }
    if (voice) LOGI("voice movies with Russian audio: %zu of %zu", russianMovies, movies);
    const int fd = dup(zip.fd);
    if (fd < 0) return false;
    if (s_apk_fds[component] >= 0) close(s_apk_fds[component]);
    s_apk_fds[component] = fd;
    for (size_t f = first; f < stop; ++f) {
        candidate[f].apk_fd = fd;
        if (candidate[f].bink) s_bink_movies[f - first].SetAudioFd(fd);
        s_entries[f] = candidate[f];
    }
    s_component_valid[component] = s_payload_valid = true;
    return true;
}

// Compose before the original config loader opens its one complete file; a
// second LoadCoalescedFile would empty the already loaded base configuration.
bool IndexOrchidRussianText(Zip& zip) {
    constexpr char asset[] = "assets/bl2_locale/dlc_orchid/ru_text/payload/Coalesced_Orchid_INT.delta.bin";
    constexpr char base_sha[] = "74D1057DB9DE6452904E5B39C83F54FCE5C8F399769EB276AE22823E6AD1B96E";
    constexpr char delta_sha[] = "9FE4D7AA100C48947A4F72DE196086F97BA29D246A97950DD92A428EDB95894F";
    constexpr char result_sha[] = "38A23A5664E8F1F8BE9FE007D9DC111476EF3D1D73F294A72E291B55F44A8099";
    std::vector<const ZipEntry*> files;
    if (!s_russian || !s_component_valid[0] || s_composed_locale_fd >= 0 ||
        !VerifyExternalIndex(zip, "assets/bl2_locale/dlc_orchid/", files) ||
        (files.size() != 1 && files.size() != 3)) return false;
    const auto* text = zip.Find(asset);
    if (!text || std::find(files.begin(), files.end(), text) == files.end() ||
        !zip.Verify(*text, 703120, delta_sha)) return false;
    std::array<Bl2LocaleEntry, 2> voice_entries{};
    if (files.size() == 3) {
        constexpr std::array<const char*, 2> assets{{
            "assets/bl2_locale/dlc_orchid/ru_voice/payload/English(US)/Audio_Banks_orchid.pck",
            "assets/bl2_locale/dlc_orchid/ru_voice/payload/English(US)/Audio_Streaming_orchid.pck"}};
        constexpr std::array<uint64_t, 2> sizes{{14454458, 179786776}};
        constexpr std::array<const char*, 2> hashes{{
            "77D45D0C8B1B557175A6C25CC445DF29A2BAAF7C269637295D2DAE602BBDF850",
            "C430661A8ECA41288F5824681355ED97FAB13975B68B2D25170A6A196D27165D"}};
        for (size_t i = 0; i < assets.size(); ++i) {
            const auto* file = zip.Find(assets[i]);
            if (!file || std::find(files.begin(), files.end(), file) == files.end() ||
                !zip.Verify(*file, sizes[i], hashes[i])) return false;
            std::memcpy(voice_entries[i].name, assets[i], std::strlen(assets[i]) + 1);
            voice_entries[i].apk_fd = zip.fd;
            voice_entries[i].data_offset = static_cast<off_t>(file->offset);
            voice_entries[i].file_size = static_cast<off_t>(file->bytes);
        }
    }
    const auto& original = s_entries[0];
    if (original.apk_fd < 0 || original.data_offset < 0 || original.file_size != 5676447) return false;
    std::vector<uint8_t> base(static_cast<size_t>(original.file_size)), delta, merged;
    ObbSha256::Digest digest{};
    if (!ReadExact(original.apk_fd, uint64_t(original.data_offset) + base.size(),
                   original.data_offset, base.data(), base.size()) ||
        !ObbSha256::OfMapping(base.data(), base.size(), digest) ||
        std::strcmp(ObbSha256::Hex(digest).data(), base_sha) != 0 ||
        !zip.ReadSmall(*text, delta) || !bl2::coalesced::Merge(base, delta, merged) ||
        merged.size() != 6447212 || !ObbSha256::OfMapping(merged.data(), merged.size(), digest) ||
        std::strcmp(ObbSha256::Hex(digest).data(), result_sha) != 0) return false;
    int fd = -1;
#if defined(_WIN32) && defined(BL2_LOCALE_HOST_TEST)
    FILE* temporary = std::tmpfile();
    if (!temporary) return false;
    if (std::fwrite(merged.data(), 1, merged.size(), temporary) == merged.size() &&
        std::fflush(temporary) == 0) fd = dup(_fileno(temporary));
    std::fclose(temporary);
#else
    fd = static_cast<int>(syscall(__NR_memfd_create, "bl2-orchid-russian-text", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd >= 0) {
        size_t done = 0;
        while (done < merged.size()) {
            const ssize_t count = write(fd, merged.data() + done, merged.size() - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            done += static_cast<size_t>(count);
        }
        if (done != merged.size() ||
            fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
            close(fd); fd = -1;
        }
    }
#endif
    if (fd < 0) return false;
    struct stat status{};
    if (fstat(fd, &status) != 0 || status.st_size != static_cast<off_t>(merged.size())) {
        close(fd); return false;
    }
    s_composed_locale_fd = fd;
    s_entries[0].apk_fd = fd;
    s_entries[0].data_offset = 0;
    s_entries[0].file_size = static_cast<off_t>(merged.size());
    if (files.size() == 3) {
        // Only these two pinned Orchid aliases may supersede English DLC VO.
        s_orchid_voice_entries = voice_entries;
        s_orchid_voice_fd = std::exchange(zip.fd, -1);
    }
    LOGI("Orchid Russian text composed: files=656 bytes=%zu", merged.size());
    return true;
}

void RememberOrchidArchive(const std::string& name, const Zip& zip,
                          const std::vector<const ZipEntry*>& files) {
    for (const auto& expected : bl2_orchid_admission::kArchives) {
        if (name != expected.name) continue;
        const auto* index = zip.Find("bl2-content.index");
        std::vector<uint8_t> bytes;
        ObbSha256::Digest digest{};
        if (!index || !zip.ReadSmall(*index, bytes) ||
            !ObbSha256::OfMapping(bytes.data(), bytes.size(), digest) ||
            std::strcmp(ObbSha256::Hex(digest).data(), expected.index_sha256) != 0) continue;
        auto& rows = s_orchid_archive_rows[static_cast<size_t>(expected.role)];
        if (!rows.empty()) return;
        for (const auto* file : files)
            rows.push_back({file->name, zip.fd, file->offset, file->bytes});
        return;
    }
}

bool RememberSageArchive(const std::string& name, const Zip& zip,
                         const std::vector<const ZipEntry*>& files) {
    for (const auto& expected : bl2_sage_admission::kArchives) {
        if (name != expected.name || zip.bytes != expected.bytes) continue;
        const auto* index = zip.Find("bl2-content.index");
        std::vector<uint8_t> bytes;
        ObbSha256::Digest digest{};
        if (!index || !zip.ReadSmall(*index, bytes) ||
            !ObbSha256::OfMapping(bytes.data(), bytes.size(), digest) ||
            std::strcmp(ObbSha256::Hex(digest).data(), expected.index_sha256) != 0) return false;
        auto& target = s_sage_archive_rows[static_cast<size_t>(expected.role)];
        if (!target.empty()) return false;
        std::vector<bl2_orchid::FileRow> rows;
        for (const auto* file : files) rows.push_back({file->name, zip.fd, file->offset, file->bytes});
        target = std::move(rows);
        return true;
    }
    return false;
}

// Cold indexing is sorted: the existing Orchid composition precedes Sage.
// Two exact inputs are admitted: original base RU, or original base+Orchid RU.
bool IndexSageRussianText(Zip& zip) {
    if (!bl2_sage_admission::kFaithfulClosureReady || !s_russian || !s_component_valid[0] ||
        s_active.load(std::memory_order_acquire) || s_sage_text_composed || s_sage_voice_fd >= 0) return false;
    for (size_t i = 0; i < kSageRoleCount; ++i)
        if (i != static_cast<size_t>(SageRole::Voice) && s_sage_archive_rows[i].empty()) return false;
    constexpr char asset[] = "assets/bl2_locale/dlc_sage/ru_text/payload/Coalesced_Sage_INT.delta.bin";
    std::vector<const ZipEntry*> files;
    if (!VerifyExternalIndex(zip, "assets/bl2_locale/dlc_sage/", files) || files.size() != 3) return false;
    const auto* text = zip.Find(asset);
    if (!text || !zip.Verify(*text, 661806,
        "6CE8B6C1753FE5602EE576D7FFEDB4E8E92E224D16D9C40DFFF4C04DC6ABADA8")) return false;
    constexpr std::array<const char*, 2> assets{{
        "assets/bl2_locale/dlc_sage/ru_voice/payload/English(US)/Audio_Banks_sage.pck",
        "assets/bl2_locale/dlc_sage/ru_voice/payload/English(US)/Audio_Streaming_sage.pck"}};
    constexpr std::array<uint64_t, 2> sizes{{7508680, 137413128}};
    constexpr std::array<const char*, 2> hashes{{
        "6C4EA4C240F6E899562430D7AF7F14035CEEBDCD271EE0E8773F8D15E5B96FC1",
        "240CEE285857DD40BD08DDDCE0705A7388E5E03109A8E88FE6F789DDE3A33E58"}};
    std::array<Bl2LocaleEntry, 2> voice{};
    for (size_t i = 0; i < assets.size(); ++i) {
        const auto* file = zip.Find(assets[i]);
        if (!file || !zip.Verify(*file, sizes[i], hashes[i])) return false;
        std::memcpy(voice[i].name, assets[i], std::strlen(assets[i]) + 1);
        voice[i].apk_fd = zip.fd; voice[i].data_offset = static_cast<off_t>(file->offset);
        voice[i].file_size = static_cast<off_t>(file->bytes);
    }
    const auto original = s_entries[0];
    if (original.apk_fd < 0 || original.data_offset < 0 ||
        (original.file_size != 5676447 && original.file_size != 6447212)) return false;
    const bool with_orchid = original.file_size == 6447212;
    const char* input_sha = with_orchid
        ? "38A23A5664E8F1F8BE9FE007D9DC111476EF3D1D73F294A72E291B55F44A8099"
        : "74D1057DB9DE6452904E5B39C83F54FCE5C8F399769EB276AE22823E6AD1B96E";
    const char* output_sha = with_orchid
        ? "6EB0EC51205861A4AE0336D38EBDCB1A70A5184CE6EB0CF7D1EA8AE14624DFBC"
        : "642BB064ADD9E0D091B677D3A0CA49532553A7D5D5A83B91D1A3EDFF114B81F7";
    const size_t expected_bytes = with_orchid ? 7108700 : 6372047;
    std::vector<uint8_t> base(static_cast<size_t>(original.file_size)), delta, merged;
    ObbSha256::Digest digest{};
    if (!ReadExact(original.apk_fd, uint64_t(original.data_offset) + base.size(), original.data_offset, base.data(), base.size()) ||
        !ObbSha256::OfMapping(base.data(), base.size(), digest) || std::strcmp(ObbSha256::Hex(digest).data(), input_sha) != 0 ||
        !zip.ReadSmall(*text, delta) || !bl2::coalesced::Merge(base, delta, merged) || merged.size() != expected_bytes ||
        !ObbSha256::OfMapping(merged.data(), merged.size(), digest) || std::strcmp(ObbSha256::Hex(digest).data(), output_sha) != 0) return false;
    int fd = -1;
#if defined(_WIN32) && defined(BL2_LOCALE_HOST_TEST)
    FILE* temporary = std::tmpfile();
    if (!temporary) return false;
    if (std::fwrite(merged.data(), 1, merged.size(), temporary) == merged.size() && std::fflush(temporary) == 0) fd = dup(_fileno(temporary));
    std::fclose(temporary);
#else
    fd = static_cast<int>(syscall(__NR_memfd_create, "bl2-sage-russian-text", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd >= 0) {
        size_t done = 0;
        while (done < merged.size()) {
            const ssize_t count = write(fd, merged.data() + done, merged.size() - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            done += static_cast<size_t>(count);
        }
        if (done != merged.size() || fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
            close(fd); fd = -1;
        }
    }
#endif
    if (fd < 0) return false;
    struct stat status{};
    if (fstat(fd, &status) != 0 || status.st_size != static_cast<off_t>(merged.size()) ||
        !RememberSageArchive("dlc_sir_hammerlock_russian_voice.31.com.tt2kgames.Borderlands2.obb", zip, files)) {
        close(fd); return false;
    }
    const int old_composed = s_composed_locale_fd;
    s_composed_locale_fd = fd; s_entries[0].apk_fd = fd; s_entries[0].data_offset = 0;
    s_entries[0].file_size = static_cast<off_t>(merged.size());
    s_sage_voice_entries = voice; s_sage_voice_fd = std::exchange(zip.fd, -1); s_sage_text_composed = true;
    if (old_composed >= 0) close(old_composed);
    LOGI("Sage Russian text composed: Orchid=%u bytes=%zu", unsigned(with_orchid), merged.size());
    return true;
}

bool CollectSageRows(std::vector<bl2_orchid::FileRow>& output) {
    for (const auto& rows : s_sage_archive_rows) if (rows.empty()) return false;
    if (!s_russian || s_sage_voice_fd < 0 || !s_sage_text_composed || s_composed_locale_fd < 0) return false;
    std::vector<bl2_orchid::FileRow> candidate;
    for (const auto& alias : bl2_sage_admission::kAliases) {
        const auto& rows = s_sage_archive_rows[static_cast<size_t>(alias.role)];
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.path == alias.source; });
        if (found == rows.end()) return false;
        candidate.push_back({alias.path, found->fd, found->offset, found->size});
    }
    for (const auto& voice : s_sage_voice_entries)
        if (voice.apk_fd != s_sage_voice_fd || voice.data_offset < 0 || voice.file_size <= 0) return false;
    const auto license = std::find_if(candidate.begin(), candidate.end(), [](const auto& row) { return row.path == "DLC/Sage/Lic/Licenses/steam_0.bin"; });
    constexpr uint8_t expected[] = {0x02,0x41,0x03,0,3,0,0,0};
    uint8_t bytes[sizeof(expected)]{};
    if (license == candidate.end() || license->size != sizeof(bytes) ||
        !ReadExact(license->fd, license->offset + license->size, license->offset, bytes, sizeof(bytes)) ||
        std::memcmp(bytes, expected, sizeof(bytes)) != 0) return false;
    bl2_orchid::PathIndex checked;
    if (!bl2_orchid::PathIndex::BuildWithCampaigns(candidate, bl2_orchid::Campaigns::Sage, checked)) return false;
    output = std::move(candidate);
    return true;
}


bool RememberIrisArchive(const std::string& name, const Zip& zip,
                         const std::vector<const ZipEntry*>& files) {
    for (const auto& expected : bl2_iris_admission::kArchives) {
        if (name != expected.name || zip.bytes != expected.bytes) continue;
        const auto* index = zip.Find("bl2-content.index");
        std::vector<uint8_t> bytes;
        ObbSha256::Digest digest{};
        if (!index || !zip.ReadSmall(*index, bytes) ||
            !ObbSha256::OfMapping(bytes.data(), bytes.size(), digest) ||
            std::strcmp(ObbSha256::Hex(digest).data(), expected.index_sha256) != 0) continue;
        auto& target = s_iris_archive_rows[static_cast<size_t>(expected.role)];
        if (!target.empty()) return false;
        std::vector<bl2_orchid::FileRow> rows;
        for (const auto* file : files) rows.push_back({file->name, zip.fd, file->offset, file->bytes});
        target = std::move(rows);
        return true;
    }
    return false;
}

// Iris RU is deliberately deferred until the existing Orchid/Sage scan ends.
// Only the four exact pre-Iris input/output pairs below are admitted.
bool IndexIrisRussianText(Zip& zip) {
    if (!bl2_iris_admission::kFaithfulClosureReady || !s_russian || !s_component_valid[0] ||
        s_active.load(std::memory_order_acquire) || s_iris_text_composed || s_iris_voice_fd >= 0) return false;
    for (size_t i = 0; i < kIrisRoleCount; ++i)
        if (i != static_cast<size_t>(IrisRole::Voice) && s_iris_archive_rows[i].empty()) return false;
    constexpr char asset[] = "assets/bl2_locale/dlc_iris/ru_text/payload/Coalesced_Iris_INT.delta.bin";
    std::vector<const ZipEntry*> files;
    if (!VerifyExternalIndex(zip, "assets/bl2_locale/dlc_iris/", files) || files.size() != 3) return false;
    const auto* text = zip.Find(asset);
    if (!text || !zip.Verify(*text, 859650,
        "E9A06FC94CA6F3FEFF3247CA90E340C415F94999B1754E1CE3E83E74DE989763")) return false;
    constexpr std::array<const char*, 2> assets{{
        "assets/bl2_locale/dlc_iris/ru_voice/payload/English(US)/Audio_Banks_iris.pck",
        "assets/bl2_locale/dlc_iris/ru_voice/payload/English(US)/Audio_Streaming_iris.pck"}};
    constexpr std::array<uint64_t, 2> sizes{{9366776, 239685016}};
    constexpr std::array<const char*, 2> hashes{{
        "38B0F3A913A1B9D8BCA8B945A0A2ED7CC19028432D4330630C9E862A65E7624D",
        "5F55767FD9FB235316933280255DBDC4BDCC7A7A0D001D6513817152CE316F8D"}};
    std::array<Bl2LocaleEntry, 2> voice{};
    for (size_t i = 0; i < assets.size(); ++i) {
        const auto* file = zip.Find(assets[i]);
        if (!file || !zip.Verify(*file, sizes[i], hashes[i])) return false;
        std::memcpy(voice[i].name, assets[i], std::strlen(assets[i]) + 1);
        voice[i].apk_fd = zip.fd; voice[i].data_offset = static_cast<off_t>(file->offset);
        voice[i].file_size = static_cast<off_t>(file->bytes);
    }
    const auto original = s_entries[0];
    if (original.apk_fd < 0 || original.data_offset < 0) return false;
    struct Known { uint64_t input_bytes; const char* input_sha; size_t output_bytes; const char* output_sha; };
    constexpr std::array<Known, 4> known{{
        {5676447ull, "74D1057DB9DE6452904E5B39C83F54FCE5C8F399769EB276AE22823E6AD1B96E", 6603782u, "905E74C4B781353F0C6E894AEF4CD47B0379D7B32FD2E33963FD0BF819F17982"},
        {6447212ull, "38A23A5664E8F1F8BE9FE007D9DC111476EF3D1D73F294A72E291B55F44A8099", 7306228u, "00C70B32A715D8B4DD6694154BAEEF67427EDAB83F8BD323D37FC1110422439C"},
        {6372047ull, "642BB064ADD9E0D091B677D3A0CA49532553A7D5D5A83B91D1A3EDFF114B81F7", 7265270u, "723841AB02B68C7F59E329F1E6120E4EEF6032240E9C628D6362CE723D01DA50"},
        {7108700ull, "6EB0EC51205861A4AE0336D38EBDCB1A70A5184CE6EB0CF7D1EA8AE14624DFBC", 7967716u, "691808F845A8A7C2E22328FCB8A2D44347FBADD76BB863AE93A5EC513101A13A"},
    }};
    const auto selected = std::find_if(known.begin(), known.end(), [&](const auto& item) {
        return original.file_size >= 0 && static_cast<uint64_t>(original.file_size) == item.input_bytes;
    });
    if (selected == known.end()) return false;
    const char* input_sha = selected->input_sha;
    const char* output_sha = selected->output_sha;
    const size_t expected_bytes = selected->output_bytes;
    std::vector<uint8_t> base(static_cast<size_t>(original.file_size)), delta, merged;
    ObbSha256::Digest digest{};
    if (!ReadExact(original.apk_fd, uint64_t(original.data_offset) + base.size(), original.data_offset, base.data(), base.size()) ||
        !ObbSha256::OfMapping(base.data(), base.size(), digest) || std::strcmp(ObbSha256::Hex(digest).data(), input_sha) != 0 ||
        !zip.ReadSmall(*text, delta) || !bl2::coalesced::Merge(base, delta, merged) || merged.size() != expected_bytes ||
        !ObbSha256::OfMapping(merged.data(), merged.size(), digest) || std::strcmp(ObbSha256::Hex(digest).data(), output_sha) != 0) return false;
    int fd = -1;
#if defined(_WIN32) && defined(BL2_LOCALE_HOST_TEST)
    FILE* temporary = std::tmpfile();
    if (!temporary) return false;
    if (std::fwrite(merged.data(), 1, merged.size(), temporary) == merged.size() && std::fflush(temporary) == 0) fd = dup(_fileno(temporary));
    std::fclose(temporary);
#else
    fd = static_cast<int>(syscall(__NR_memfd_create, "bl2-iris-russian-text", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd >= 0) {
        size_t done = 0;
        while (done < merged.size()) {
            const ssize_t count = write(fd, merged.data() + done, merged.size() - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            done += static_cast<size_t>(count);
        }
        if (done != merged.size() || fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
            close(fd); fd = -1;
        }
    }
#endif
    if (fd < 0) return false;
    struct stat status{};
    if (fstat(fd, &status) != 0 || status.st_size != static_cast<off_t>(merged.size()) ||
        !RememberIrisArchive("dlc_mr_torgue_russian_voice.31.com.tt2kgames.Borderlands2.obb", zip, files)) {
        close(fd); return false;
    }
    const int old_composed = s_composed_locale_fd;
    s_composed_locale_fd = fd; s_entries[0].apk_fd = fd; s_entries[0].data_offset = 0;
    s_entries[0].file_size = static_cast<off_t>(merged.size());
    s_iris_voice_entries = voice; s_iris_voice_fd = std::exchange(zip.fd, -1); s_iris_text_composed = true;
    if (old_composed >= 0) close(old_composed);
    LOGI("Iris Russian text composed: prior_bytes=%llu bytes=%zu", static_cast<unsigned long long>(original.file_size), merged.size());
    return true;
}

bool CollectIrisRows(std::vector<bl2_orchid::FileRow>& output) {
    for (const auto& rows : s_iris_archive_rows) if (rows.empty()) return false;
    if (!s_russian || s_iris_voice_fd < 0 || !s_iris_text_composed || s_composed_locale_fd < 0) return false;
    std::vector<bl2_orchid::FileRow> candidate;
    for (const auto& alias : bl2_iris_admission::kAliases) {
        const auto& rows = s_iris_archive_rows[static_cast<size_t>(alias.role)];
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.path == alias.source; });
        if (found == rows.end()) return false;
        candidate.push_back({alias.path, found->fd, found->offset, found->size});
    }
    for (const auto& voice : s_iris_voice_entries)
        if (voice.apk_fd != s_iris_voice_fd || voice.data_offset < 0 || voice.file_size <= 0) return false;
    const auto license = std::find_if(candidate.begin(), candidate.end(), [](const auto& row) { return row.path == "DLC/Iris/Lic/Licenses/steam_0.bin"; });
    constexpr uint8_t expected[] = {0xDA,0x40,0x03,0,3,0,0,0};
    uint8_t bytes[sizeof(expected)]{};
    if (license == candidate.end() || license->size != sizeof(bytes) ||
        !ReadExact(license->fd, license->offset + license->size, license->offset, bytes, sizeof(bytes)) ||
        std::memcmp(bytes, expected, sizeof(bytes)) != 0) return false;
    bl2_orchid::PathIndex checked;
    if (!bl2_orchid::PathIndex::BuildWithCampaigns(candidate, bl2_orchid::Campaigns::Iris, checked)) return false;
    output = std::move(candidate);
    return true;
}



bool CollectOrchidRows(std::vector<bl2_orchid::FileRow>& output) {
    // Every selected archive is mandatory. The pinned package index is changed
    // only after its faithful closure audit; archive JSON cannot enable it.
    for (const auto& rows : s_orchid_archive_rows) if (rows.empty()) return false;
    if (!s_russian || s_orchid_voice_fd < 0 || s_composed_locale_fd < 0) return false;
    std::vector<bl2_orchid::FileRow> candidate;
    for (const auto& alias : bl2_orchid_admission::kAliases) {
        const auto& rows = s_orchid_archive_rows[static_cast<size_t>(alias.role)];
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.path == alias.source; });
        if (found == rows.end()) return false;
        candidate.push_back({alias.path, found->fd, found->offset, found->size});
    }
    for (size_t i = 0; i < kOrchidVoicePaths.size(); ++i) {
        const auto& file = s_orchid_voice_entries[i];
        if (file.apk_fd != s_orchid_voice_fd || file.data_offset < 0 || file.file_size <= 0) return false;
        const std::string name = std::string(kOrchidVoicePaths[i]).substr(std::strlen("WillowGame/CookedAndroid_DXT/"));
        candidate.push_back({"DLC/Orchid/Lic/Audio/" + name, file.apk_fd,
            static_cast<uint64_t>(file.data_offset), static_cast<uint64_t>(file.file_size)});
    }
    const auto license = std::find_if(candidate.begin(), candidate.end(), [](const auto& row) {
        return row.path == "DLC/Orchid/Lic/Licenses/steam_0.bin";
    });
    constexpr uint8_t original_license[] = {0xFE,0x2B,0x03,0,3,0,0,0};
    uint8_t bytes[sizeof(original_license)]{};
    if (license == candidate.end() || license->size != sizeof(bytes) ||
        !ReadExact(license->fd, license->offset + license->size, license->offset, bytes, sizeof(bytes)) ||
        std::memcmp(bytes, original_license, sizeof(bytes)) != 0) return false;
    bl2_orchid::PathIndex checked;
    if (!bl2_orchid::PathIndex::Build(candidate, checked)) return false;
    output = std::move(candidate);
    return true;
}

void IndexExternalArchives() {
    if (s_obb_directory.empty()) return;
    LOGI("External DLC scan begin: %s", s_obb_directory.c_str());
    if (s_russian) {
        Zip voice;
        voice.trusted_installed = false;
        std::vector<const ZipEntry*> files;
        const std::string path = s_obb_directory + "/russian_voice.31.com.tt2kgames.Borderlands2.obb";
        if (voice.Open(path.c_str())) {
            if (VerifyExternalIndex(voice, "assets/bl2_locale/ru_voice/", files) && IndexLocaleComponent(voice, 1))
                LOGI("External Russian voice OBB indexed: %s", path.c_str());
            else LOGE("External Russian voice OBB rejected: %s", path.c_str());
        }
    }
    // Base-only updates retain the separate base Russian voice above, but
    // never inspect, admit, compose or mount existing optional DLC archives.
    if (!BL2_ENABLE_DLC_CONTENT) return;
    std::error_code error;
    std::vector<std::filesystem::path> paths;
    for (std::filesystem::directory_iterator it(s_obb_directory, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.starts_with("dlc_") && name.ends_with(".31.com.tt2kgames.Borderlands2.obb") &&
            it->is_regular_file(error) && !it->is_symlink(error)) paths.push_back(it->path());
        if (paths.size() > 64) { LOGE("External DLC inventory rejected: more than 64 archives"); return; }
        if (error) break;
    }
    if (error) { LOGE("External DLC inventory failed: %s", error.message().c_str()); return; }
    std::sort(paths.begin(), paths.end());
    std::filesystem::path iris_voice_path;
    for (const auto& path : paths) {
        Zip zip;
        zip.trusted_installed = false;
        const std::string name = path.filename().string();
        LOGI("External DLC candidate begin: %s", name.c_str());
        if (name == "dlc_mr_torgue_campaign_of_carnage_metadata.31.com.tt2kgames.Borderlands2.obb") {
            zip.iris_metadata = true;
            std::vector<const ZipEntry*> metadata;
            if (!zip.Open(path.string().c_str()) || !VerifyExternalIndex(zip, "DLC/Iris/", metadata) ||
                !RememberIrisArchive(name, zip, metadata)) {
                LOGE("Iris directory metadata rejected: %s", path.string().c_str()); continue;
            }
            s_external_fds.push_back(std::exchange(zip.fd, -1));
            continue;
        }
        if (name == "dlc_mr_torgue_russian_voice.31.com.tt2kgames.Borderlands2.obb") {
            iris_voice_path = path;  // No parse/composition before existing Sage RU.
            continue;
        }
        if (name == "dlc_sir_hammerlock_big_game_hunt_metadata.31.com.tt2kgames.Borderlands2.obb") {
            zip.sage_metadata = true;
            std::vector<const ZipEntry*> metadata;
            if (!zip.Open(path.string().c_str()) || !VerifyExternalIndex(zip, "DLC/Sage/", metadata) ||
                !RememberSageArchive(name, zip, metadata)) {
                LOGE("Sage directory metadata rejected: %s", path.string().c_str()); continue;
            }
            s_external_fds.push_back(std::exchange(zip.fd, -1));
            continue;
        }
        if (name == "dlc_sir_hammerlock_russian_voice.31.com.tt2kgames.Borderlands2.obb") {
            if (s_russian && (!zip.Open(path.string().c_str()) || !IndexSageRussianText(zip)))
                LOGE("Sage Russian OBB rejected: %s", path.string().c_str());
            continue;
        }
        if (name == "dlc_captain_scarlett_registration.31.com.tt2kgames.Borderlands2.obb") {
            zip.orchid_metadata = true;
            std::vector<const ZipEntry*> metadata;
            if (!zip.Open(path.string().c_str()) || !VerifyExternalIndex(zip, "DLC/Orchid/", metadata)) {
                LOGE("Orchid directory metadata rejected: %s", path.string().c_str());
                continue;
            }
            RememberOrchidArchive(name, zip, metadata);
            s_external_fds.push_back(std::exchange(zip.fd, -1));
            continue;
        }
        if (name.find("_russian_voice.") != std::string::npos) {
            if (s_russian && (name != "dlc_captain_scarlett_russian_voice.31.com.tt2kgames.Borderlands2.obb" ||
                !zip.Open(path.string().c_str()) || !IndexOrchidRussianText(zip)))
                LOGE("Russian DLC OBB rejected: %s", path.string().c_str());
            continue;
        }
        std::vector<const ZipEntry*> files;
        if (!zip.Open(path.string().c_str()) || !VerifyExternalIndex(zip, "WillowGame/CookedAndroid_DXT/", files)) {
            LOGE("External DLC OBB rejected: %s", path.string().c_str());
            continue;
        }
        std::vector<ExternalEntry> candidate;
        for (const auto* file : files) {
            std::string normalized;
            if (!Normalize(file->name.c_str(), normalized) || normalized != file->name ||
                FileForPath(normalized.c_str()) < kFileCount ||
                std::any_of(s_external_entries.begin(), s_external_entries.end(), [&](const auto& entry) {
                    return EqualFold(entry.path, normalized);
                }) || std::any_of(candidate.begin(), candidate.end(), [&](const auto& entry) {
                    return EqualFold(entry.path, normalized);
                })) { candidate.clear(); break; }
            ExternalEntry entry;
            entry.path = normalized;
            std::memcpy(entry.entry.name, file->name.c_str(), file->name.size() + 1);
            entry.entry.apk_fd = zip.fd;
            entry.entry.data_offset = static_cast<off_t>(file->offset);
            entry.entry.file_size = static_cast<off_t>(file->bytes);
            candidate.push_back(std::move(entry));
        }
        if (candidate.empty()) { LOGE("External DLC OBB has colliding or invalid paths: %s", path.string().c_str()); continue; }
        RememberOrchidArchive(name, zip, files);
        RememberSageArchive(name, zip, files);
        RememberIrisArchive(name, zip, files);
        s_external_entries.insert(s_external_entries.end(), std::make_move_iterator(candidate.begin()), std::make_move_iterator(candidate.end()));
        s_external_fds.push_back(std::exchange(zip.fd, -1));
        s_payload_valid = true;
        LOGI("External DLC OBB indexed: %s files=%zu", path.string().c_str(), files.size());
    }
    if (s_russian && !iris_voice_path.empty()) {
        Zip zip; zip.trusted_installed = false;
        if (!zip.Open(iris_voice_path.string().c_str()) || !IndexIrisRussianText(zip))
            LOGE("Iris Russian OBB rejected: %s", iris_voice_path.string().c_str());
    }
    if (bl2_orchid_admission::kFaithfulClosureReady && CollectOrchidRows(s_orchid_pending_rows)) {
        s_orchid_admitted.store(true, std::memory_order_release);
        LOGI("Orchid local content admitted: regions=%zu; original Android root pending", s_orchid_pending_rows.size());
    }
    if (bl2_sage_admission::kFaithfulClosureReady && CollectSageRows(s_sage_pending_rows)) {
        s_sage_admitted.store(true, std::memory_order_release);
        LOGI("Sage local content admitted: regions=%zu; original Android root pending", s_sage_pending_rows.size());
    }
    if (bl2_iris_admission::kFaithfulClosureReady && CollectIrisRows(s_iris_pending_rows)) {
        s_iris_admitted.store(true, std::memory_order_release);
        LOGI("Iris local content admitted: regions=%zu; original Android root pending", s_iris_pending_rows.size());
    }
    LOGI("External DLC scan end: candidates=%zu Orchid=%u Sage=%u Iris=%u", paths.size(), unsigned(s_orchid_admitted.load()), unsigned(s_sage_admitted.load()), unsigned(s_iris_admitted.load()));
}

const ExternalEntry* ExternalPackageForRequest(std::string_view request) {
    const auto slash = request.find_last_of("/\\");
    if (slash != std::string_view::npos) request.remove_prefix(slash + 1);
    if (request.size() >= 4 && EqualFold(request.substr(request.size() - 4), ".xxx")) request.remove_suffix(4);
    const ExternalEntry* match = nullptr;
    for (const auto& entry : s_external_entries) {
        std::string_view filename(entry.path);
        filename.remove_prefix(filename.find_last_of('/') + 1);
        if (!EqualFold(filename.substr(filename.size() - std::min<size_t>(4, filename.size())), ".xxx")) continue;
        filename.remove_suffix(4);
        if (EqualFold(filename, request)) {
            if (match) return nullptr;
            match = &entry;
        }
    }
    return match;
}

#if !defined(BL2_LOCALE_HOST_TEST)
bool ReadGuestPath(DynarmicHost& host, uint32_t address, std::string& path) {
    path.clear();
    if (!address || !Bounds(host.GetGuestMemorySize(), address, kMaxPath)) return false;
    for (size_t i = 0; i < kMaxPath; ++i) {
        const uint8_t c = host.MemoryRead8(address + static_cast<uint32_t>(i));
        if (!c) return true;
        path.push_back(static_cast<char>(c));
    }
    return false;
}

bool IsPackageCache(DynarmicHost& host, uint32_t base, uint32_t object) {
    return !s_external_entries.empty() && object && Bounds(host.GetGuestMemorySize(), object, 4) &&
        host.MemoryRead32(object) == base + kPackageCacheVtableRva;
}

void CacheExternalPackage(DynarmicHost& host, uint32_t base, uint32_t cache, uint32_t request) {
    std::string path;
    if (!ReadGuestPath(host, request, path)) return;
    const auto* entry = ExternalPackageForRequest(path);
    if (!entry) return;
    const uint32_t filename = host.GuestAlloc(entry->path.size() + 1, 4);
    if (!filename) return;
    for (size_t i = 0; i <= entry->path.size(); ++i)
        host.MemoryWrite8(filename + static_cast<uint32_t>(i), static_cast<uint8_t>(entry->path.c_str()[i]));
    [[maybe_unused]] uint32_t accepted = 0;
#if defined(BL2_NATIVE_ARM32)
    accepted = host.RunGuestCallback(base + kPackageCacheRva, {cache, filename, 0, 0});
#else
    host.CallGuest(base + kPackageCacheRva, {cache, filename, 0, 0}, accepted);
#endif
    host.GuestFree(filename);
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 16) LOGI("External DLC package cache: path=%s accepted=%u", entry->path.c_str(), accepted);
}
#endif
}  // namespace

bool PublishBl2OrchidMount(const std::vector<bl2_orchid::FileRow>& verified_rows,
                          const char* observed_absolute_dlc_root) {
    std::lock_guard lock(s_orchid_publish_mutex);
    if (AnyDlcPublished() || verified_rows.empty() ||
        verified_rows.size() > bl2_orchid::PathIndex::kMaxFiles) return false;
    std::string root(OrchidPath(observed_absolute_dlc_root));
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    if (root.empty() || (root.front() != '/' && root.front() != '\\')) return false;
    auto rows = verified_rows;
    const bool orchid = std::any_of(rows.begin(), rows.end(), [](const auto& row) { return PrefixFold(row.path, "DLC/Orchid/"); });
    const bool sage = std::any_of(rows.begin(), rows.end(), [](const auto& row) { return PrefixFold(row.path, "DLC/Sage/"); });
    const bool iris = std::any_of(rows.begin(), rows.end(), [](const auto& row) { return PrefixFold(row.path, "DLC/Iris/"); });
    if ((!orchid && !sage && !iris) || (sage && !s_sage_admitted.load(std::memory_order_acquire)) ||
        (iris && !s_iris_admitted.load(std::memory_order_acquire))) return false;
    const auto campaigns = static_cast<bl2_orchid::Campaigns>((orchid ? 1u : 0u) | (sage ? 2u : 0u) | (iris ? 4u : 0u));
    std::map<int, int> retained;
    std::vector<int> descriptors;
    const auto reject = [&] {
        for (const int fd : descriptors) close(fd);
        return false;
    };
    constexpr uint64_t maximum = static_cast<uint64_t>(
        std::numeric_limits<decltype(Bl2LocaleEntry{}.data_offset)>::max());
    for (const auto& row : rows) {
        if (row.fd < 0 || row.path.size() >= sizeof(Bl2LocaleEntry{}.name) || row.offset > maximum ||
            row.size > maximum - row.offset) return reject();
        struct stat status{};
        if (fstat(row.fd, &status) != 0 || status.st_size < 0 ||
            !Bounds(static_cast<uint64_t>(status.st_size), row.offset, row.size)) return reject();
    }
    for (auto& row : rows) {
        auto found = retained.find(row.fd);
        if (found == retained.end()) {
            const int fd = dup(row.fd);
            if (fd < 0) return reject();
            descriptors.push_back(fd);
            found = retained.emplace(row.fd, fd).first;
        }
        row.fd = found->second;
    }
    bl2_orchid::PathIndex candidate;
    if (!bl2_orchid::PathIndex::BuildWithCampaigns(rows, campaigns, candidate) ||
        (orchid && candidate.Kind(root + "/Orchid", root) != bl2_orchid::NodeKind::Directory) ||
        (sage && candidate.Kind(root + "/Sage", root) != bl2_orchid::NodeKind::Directory) ||
        (iris && candidate.Kind(root + "/Iris", root) != bl2_orchid::NodeKind::Directory)) return reject();
    std::map<std::string, Bl2LocaleEntry> entries;
    for (const auto& row : rows) {
        const auto* file = candidate.Find(row.path);
        if (!file) return reject();
        Bl2LocaleEntry entry{};
        std::memcpy(entry.name, file->path.c_str(), file->path.size() + 1);
        entry.apk_fd = file->fd;
        entry.data_offset = static_cast<decltype(entry.data_offset)>(file->offset);
        entry.file_size = static_cast<decltype(entry.file_size)>(file->size);
        entries.emplace(file->path, entry);
    }
    s_orchid_index = std::move(candidate);
    s_orchid_files = std::move(entries);
    s_orchid_absolute_root = std::move(root);
    s_orchid_mount_fds = std::move(descriptors);
    s_orchid_published.store(orchid, std::memory_order_release);
    s_sage_published.store(sage, std::memory_order_release);
    s_iris_published.store(iris, std::memory_order_release);
    return true;
}

bl2_orchid::NodeKind Bl2OrchidKind(const char* path) {
    return AnyDlcPublished()
        ? s_orchid_index.Kind(OrchidPath(path), s_orchid_absolute_root)
        : bl2_orchid::NodeKind::Missing;
}

const std::vector<bl2_orchid::Child>* Bl2OrchidChildren(const char* path) {
    return AnyDlcPublished()
        ? s_orchid_index.Children(OrchidPath(path), s_orchid_absolute_root) : nullptr;
}

bool Bl2OrchidStat(const char* path, struct stat& output) {
    const auto kind = Bl2OrchidKind(path);
    if (kind == bl2_orchid::NodeKind::Missing) return false;
    std::memset(&output, 0, sizeof(output));
    const bool directory = kind == bl2_orchid::NodeKind::Directory;
    output.st_mode = directory ? S_IFDIR | 0555 : S_IFREG | 0444;
    output.st_nlink = directory ? 2 : 1;
    if (!directory) output.st_size = OrchidFile(path)->file_size;
#if !defined(_WIN32)
    output.st_blksize = 4096;
    output.st_blocks = (static_cast<uint64_t>(output.st_size) + 511) / 512;
#endif
    return true;
}

bool Bl2OrchidAccess(const char* path, int mode, int& result) {
    const auto kind = Bl2OrchidKind(path);
    if (kind == bl2_orchid::NodeKind::Missing) return false;
    result = -1;
    if ((mode & ~7) != 0) errno = EINVAL;
    else if ((mode & 2) != 0 || ((mode & 1) != 0 && kind != bl2_orchid::NodeKind::Directory)) errno = EACCES;
    else result = 0;
    return true;
}

bool Bl2IrisContentAdmitted() { return s_iris_admitted.load(std::memory_order_acquire); }

bool Bl2OrchidOwnsApp(uint32_t app_id) {
    if (app_id == 207870) return s_orchid_admitted.load(std::memory_order_acquire) && s_orchid_published.load(std::memory_order_acquire);
    if (app_id == bl2_sage_admission::kAppId) return s_sage_admitted.load(std::memory_order_acquire) && s_sage_published.load(std::memory_order_acquire);
    if (app_id == bl2_iris_admission::kAppId) return s_iris_admitted.load(std::memory_order_acquire) && s_iris_published.load(std::memory_order_acquire);
    return false;
}

#if !defined(BL2_LOCALE_HOST_TEST)
namespace {
bool ReadableRootBytes(DynarmicHost& host, uint32_t address, size_t count) {
    if (address < 0x10000 || !Bounds(host.GetGuestMemorySize(), address, count)) return false;
    const auto begin = reinterpret_cast<uintptr_t>(host.GetGuestMemoryBase()) + uint64_t(address);
    if (begin > std::numeric_limits<uintptr_t>::max() || count > std::numeric_limits<uintptr_t>::max() - begin) return false;
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long first = 0, past = 0;
        char mode[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &first, &past, mode) == 3 &&
            mode[0] == 'r' && begin >= first && begin <= past && count <= past - begin) {
            found = true;
            break;
        }
    }
    std::fclose(maps);
    return found;
}
}

void EnsureBl2OrchidMount(DynarmicHost& host) {
    if (AnyDlcPublished() ||
        (!s_orchid_admitted.load(std::memory_order_acquire) && !s_sage_admitted.load(std::memory_order_acquire) && !s_iris_admitted.load(std::memory_order_acquire)) ||
        !s_active.load(std::memory_order_acquire) || !s_guest_valid) return;
    const auto& elf = host.GetElfLoader();
    const uint32_t base = elf.GetBaseAddress();
    constexpr uint32_t system_rva = 0x03057590, convert_rva = 0x01E223AC;
    constexpr uint32_t prefix[] = {0xE92D4FF0,0xE1A01002,0xE24DD014,0xE59F5414};
    if (elf.GetImageSize() != 0x03089000 || uint64_t(base) + elf.GetImageSize() > host.GetGuestMemorySize() ||
        elf.GetSymbolAddress("GAndroidSystemMemory") != base + system_rva ||
        elf.GetSymbolAddress("_ZN19FFileManagerAndroid21ConvertToAbsolutePathEPKc") != base + convert_rva ||
        !elf.IsReadOnlyRange(base + convert_rva, sizeof(prefix))) return;
    for (size_t i = 0; i < std::size(prefix); ++i)
        if (host.MemoryRead32(base + convert_rva + static_cast<uint32_t>(i * 4)) != prefix[i]) return;
    // The field is an original process-lifetime ANSI FString initialized after
    // JNI. Snapshot/recheck its header; an empty early root is retried later.
    const uint32_t field = base + system_rva + 0xF4;
    const uint32_t data = host.MemoryRead32(field), count = host.MemoryRead32(field + 4), capacity = host.MemoryRead32(field + 8);
    if (!count || count > 512 || count > capacity || capacity > 4096 || !ReadableRootBytes(host, data, count)) return;
    std::array<uint8_t, 512> text{};
    for (uint32_t i = 0; i < count; ++i) text[i] = host.MemoryRead8(data + i);
    if (host.MemoryRead32(field) != data || host.MemoryRead32(field + 4) != count ||
        host.MemoryRead32(field + 8) != capacity) return;
    std::string root;
    if (!bl2_orchid::DecodeDlcRoot(text.data(), count, root)) return;
    auto rows = s_orchid_pending_rows;
    if (s_sage_admitted.load(std::memory_order_acquire)) rows.insert(rows.end(), s_sage_pending_rows.begin(), s_sage_pending_rows.end());
    if (s_iris_admitted.load(std::memory_order_acquire)) rows.insert(rows.end(), s_iris_pending_rows.begin(), s_iris_pending_rows.end());
    if (PublishBl2OrchidMount(rows, root.c_str()))
        LOGI("Admitted DLC directories published: root=%s regions=%zu", root.c_str(), rows.size());
}

bool InstallBl2OrchidOwnership(DynarmicHost& host) {
    if (s_orchid_ownership_installed) return true;
    const auto& elf = host.GetElfLoader();
    const uint32_t base = elf.GetBaseAddress(), site = base + 0x01F58DF4;
    if (!s_guest_valid || elf.GetImageSize() != 0x03089000 ||
        uint64_t(base) + elf.GetImageSize() > host.GetGuestMemorySize() ||
        elf.GetSymbolAddress("_ZN26UOnlineSubsystemSteamworks15QueryDlcLicenseEi") != site ||
        !elf.IsReadOnlyRange(site, 8) || host.MemoryRead32(site) != 0xE3A00000 ||
        host.MemoryRead32(site + 4) != 0xE12FFF1E) return false;
    // Original is exactly MOV r0,#0; BX lr. This retained leaf returns its
    // original false for every case except our fully admitted, published ID.
    // EABI: r0=this, r1=signed AppId. No installed masks/results are written.
    const uint32_t thunk = host.GetThunkManager().RegisterThunk("bl2_orchid_local_ownership",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            const uint32_t appId = jit.Regs()[1];
            const uint32_t result = Bl2OrchidOwnsApp(appId) ? 1u : 0u;
#if defined(BL2_NATIVE_ARM32) && BL2_ENABLE_QA_CONSOLE
            static std::atomic<unsigned> qaRows{0};
            const unsigned qaRow = qaRows.fetch_add(1, std::memory_order_relaxed) + 1;
            if (qaRow <= 16) {
                const int savedErrno = errno;
                __android_log_print(ANDROID_LOG_INFO, "BL2_NativeDlcQa",
                    "ownership row=%u lr=%08x appId=%u result=%u", qaRow,
                    jit.Regs()[14], appId, result);
                errno = savedErrno;
            }
#endif
            jit.Regs()[0] = result;
        });
    if (!thunk) return false;
#if !defined(BL2_NATIVE_ARM32)
    if ((host.MemoryRead32(thunk) & 0xFF000000u) != 0xEF000000u) return false;
#endif
    host.MemoryWrite32(site + 4, thunk);
    host.MemoryWrite32(site, 0xE51FF004);
    host.InvalidateGuestCode(site, 8);
    if (host.MemoryRead32(site) != 0xE51FF004 || host.MemoryRead32(site + 4) != thunk) return false;
    s_orchid_ownership_installed = true;
    LOGI("Orchid local ownership leaf installed: AppId=207870 admission=%u published=%u",
        unsigned(s_orchid_admitted.load()), unsigned(s_orchid_published.load()));
    return true;
}
#endif

void SetBl2LocaleLanguage(const char* language) {
    if (!s_indexed) s_russian = language && EqualFold(language, "ru");
}
bool Bl2RussianLocaleSelected() { return s_russian; }
void SetBl2ExternalObbDirectory(const char* directory) {
    if (!s_indexed) s_obb_directory = directory ? directory : "";
}
const char* Bl2ExternalObbDirectory() { return s_obb_directory.c_str(); }

void SetBl2LocaleMainObb(int fd, uint64_t offset, uint64_t bytes) {
    s_main_fd = fd; s_main_offset = offset; s_main_bytes = bytes;
}

bool IndexBl2CacheBase(const char* path) {
    if (s_cache_fd >= 0) return s_cache_valid;
    if (!path) return false;
    Zip zip;
    if (!zip.Open(path)) return false;
    if (s_russian && zip.Find(kDescriptor)) {
        if (IndexLocaleComponent(zip, 0)) LOGI("base Russian text indexed: direct=1");
        else LOGE("base Russian text rejected: incomplete or incompatible assets");
    }
    // Base assets are admitted as one set: a slim OBB must never become usable
    // without every relocated file and its exact original content.
    std::array<Bl2LocaleEntry, std::size(bl2_cache_payload::kFiles)> candidate{};
    for (size_t i = 0; i < candidate.size(); ++i) {
        const auto& expected = bl2_cache_payload::kFiles[i];
        const auto* entry = zip.Find(expected.asset);
        if (!entry || !zip.Verify(*entry, expected.bytes, expected.sha256) ||
            entry->name.size() >= sizeof(candidate[i].name)) return false;
        std::memcpy(candidate[i].name, entry->name.c_str(), entry->name.size() + 1);
        candidate[i].apk_fd = zip.fd;
        candidate[i].data_offset = static_cast<off_t>(entry->offset);
        candidate[i].file_size = static_cast<off_t>(entry->bytes);
    }
    for (size_t i = 0; i < candidate.size(); ++i) s_entries[kLocaleFileCount + i] = candidate[i];
    s_cache_fd = std::exchange(zip.fd, -1);
    s_cache_valid = s_payload_valid = true;
    LOGI("base cache payload verified: files=%zu direct=1", candidate.size());
    return true;
}

bool HasBl2RelocatedCache() { return s_cache_valid; }

void IndexBl2LocaleSplits(const char* const* paths, size_t count, const char* game_files_root) {
    if (s_indexed) return;
    s_indexed = true;
    s_files_root = game_files_root ? game_files_root : "";
    while (!s_files_root.empty() && s_files_root.back() == '/') s_files_root.pop_back();
    if (count > 64 || (count && !paths)) { LOGE("locale rejected: invalid installed split inventory"); return; }
    std::array<bool, 2> found = s_component_valid, rejected{};
    for (size_t i = 0; i < count; ++i) {
        if (!s_russian) break;
        if (!paths[i] || !*paths[i]) { rejected.fill(true); continue; }
        Zip zip;
        if (!zip.Open(paths[i])) { rejected.fill(true); LOGE("locale rejected malformed APK"); continue; }
        const auto* manifest = zip.Find("AndroidManifest.xml");
        std::vector<uint8_t> xml;
        std::string split;
        const bool identity = manifest && zip.ReadSmall(*manifest, xml) && ManifestIdentity(xml, split);
        const size_t component = split == "ru_text" ? 0 : split == "ru_voice" ? 1 : 2;
        if (!identity) {
            if (component < 2) rejected[component] = true;
            else rejected.fill(true);
            LOGE("locale rejected binary manifest"); continue;
        }
        if (component == 2) continue;
        // Embedded text owns this component; an obsolete installed ru_text split
        // must not override the base or invalidate its compatible font packages.
        if (component == 0 && s_component_valid[0]) continue;
        if (std::exchange(found[component], true)) { rejected[component] = true; continue; }
        if (!IndexLocaleComponent(zip, component)) {
            rejected[component] = true; LOGE("locale rejected incomplete payload"); continue;
        }
        LOGI("locale indexed: component=%s direct=1", split.c_str());
    }
    for (size_t component = 0; component < 2; ++component) {
        s_component_valid[component] = found[component] && !rejected[component] && s_apk_fds[component] >= 0;
        if (s_component_valid[component]) continue;
        if (s_apk_fds[component] >= 0) close(std::exchange(s_apk_fds[component], -1));
        const size_t first = component ? kTextFileCount : 0;
        const size_t stop = first + (component ? kVoiceFileCount : kTextFileCount);
        for (size_t f = first; f < stop; ++f) s_entries[f] = {};
    }
    s_payload_valid = s_cache_valid || s_component_valid[0] || s_component_valid[1];
    IndexExternalArchives();
    LOGI("locale indexed: ru_text=%u ru_voice=%u engine=INT audio=English(US)",
         unsigned(s_component_valid[0]), unsigned(s_component_valid[1]));
}

bool VerifyBl2LocaleGuest(const uint8_t* data, size_t bytes) {
    s_guest_valid = false;
    ObbSha256::Digest digest{};
    s_guest_valid = data && bytes == 49885152 && ObbSha256::OfMapping(data, bytes, digest) &&
        std::strcmp(ObbSha256::Hex(digest).data(), kGuestSha256) == 0;
    if (!s_guest_valid) LOGE("guest ELF identity mismatch");
    return s_guest_valid;
}

#if !defined(BL2_LOCALE_HOST_TEST)
bool InstallBl2LocaleOverlay(DynarmicHost& host) {
    if (!s_payload_valid) return true;
    if (s_active.load(std::memory_order_acquire)) return true;
    auto& loader = host.GetElfLoader();
    const uint32_t base = loader.GetBaseAddress();
    const uint32_t lookup = loader.GetSymbolAddress("_ZN15FOOBFileManager11pathToEntryEPKc");
    if (!s_guest_valid || loader.GetImageSize() != 0x03089000 || lookup != base + kLookupRva ||
        !Bounds(host.GetGuestMemorySize(), lookup, sizeof(kLookupPreimage))) {
        LOGE("ru_text disabled: guest image / FOOB symbol guard failed"); return false;
    }
    for (size_t i = 0; i < std::size(kLookupPreimage); ++i) {
        if (host.MemoryRead32(lookup + static_cast<uint32_t>(i*4)) != kLookupPreimage[i]) {
            LOGE("ru_text disabled: FOOB instruction preimage changed"); return false;
        }
    }
    const bool packages = !s_external_entries.empty();
    const uint32_t package_find = base + kPackageFindRva;
    if (packages) {
        if (loader.GetSymbolAddress("_ZN20FMapPackageFileCache15FindPackageFileEPKcPK5FGuidR7FStringS1_") != package_find ||
            loader.GetSymbolAddress("_ZN20FMapPackageFileCache12CachePackageEPKcjj") != base + kPackageCacheRva) return false;
        for (size_t i = 0; i < std::size(kPackageFindPreimage); ++i)
            if (host.MemoryRead32(package_find + static_cast<uint32_t>(i * 4)) != kPackageFindPreimage[i] ||
                host.MemoryRead32(base + kPackageCacheRva + static_cast<uint32_t>(i * 4)) != kPackageCachePreimage[i]) return false;
    }
#if defined(BL2_NATIVE_ARM32)
    // Re-execute only the two position-independent entry instructions before
    // resuming the original ARM function at its unchanged PC-relative loads.
    const uint32_t original_code[] = {
        kLookupPreimage[0], kLookupPreimage[1], 0xE51FF004, lookup + 8};
    const uint32_t original = host.CreateCodeVeneer(original_code);
    if (!original) return false;
    const uint32_t package_code[] = {kPackageFindPreimage[0], kPackageFindPreimage[1], 0xE51FF004, package_find + 8};
    const uint32_t package_original = packages ? host.CreateCodeVeneer(package_code) : 0;
    if (packages && !package_original) return false;
    const uint32_t thunk = host.GetThunkManager().RegisterThunk("bl2_locale_foob_lookup",
        [original, package_original, base](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
            if (package_original && IsPackageCache(current, base, jit.Regs()[0])) {
                const auto args = jit.Regs();
                const uint32_t language = current.MemoryRead32(args[13]);
                CacheExternalPackage(current, base, args[0], args[1]);
                jit.Regs()[0] = current.RunGuestCallback(package_original, {args[0], args[1], args[2], args[3], language});
                return;
            }
            std::string path;
            const bool overlaid = ReadGuestPath(current, jit.Regs()[0], path) &&
                                  IsBl2LocalePath(path.c_str());
            jit.Regs()[0] = overlaid ? 0 : current.RunGuestCallback(original, {jit.Regs()[0]});
        });
    if (!thunk) return false;
    host.MemoryWrite32(lookup + 4, thunk);
    host.MemoryWrite32(lookup, 0xE51FF004);
    host.InvalidateGuestCode(lookup, 8);
    if (packages) {
        host.MemoryWrite32(package_find + 4, thunk);
        host.MemoryWrite32(package_find, 0xE51FF004);
        host.InvalidateGuestCode(package_find, 8);
    }
    if (host.MemoryRead32(lookup) != 0xE51FF004 || host.MemoryRead32(lookup + 4) != thunk) {
        LOGE("ru_text disabled: native FOOB hook write verification failed"); return false;
    }
    [[maybe_unused]] const uint32_t veneer = original;
#else
    // First two instructions are position independent. Keep the real function
    // at its original address from +8 onward (its literal/GOT loads are PC relative).
    const uint32_t veneer = host.GuestAlloc(packages ? 56 : 40, 4);
    if (!veneer) return false;
    const uint32_t thunk = host.GetThunkManager().RegisterThunk("bl2_locale_foob_lookup",
        [veneer, base, packages](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
            if (packages && IsPackageCache(current, base, jit.Regs()[0])) {
                CacheExternalPackage(current, base, jit.Regs()[0], jit.Regs()[1]);
                jit.Regs()[12] = veneer + 40;
                return;
            }
            std::string path;
            const bool overlaid = ReadGuestPath(current, jit.Regs()[0], path) && IsBl2LocalePath(path.c_str());
            jit.Regs()[12] = overlaid ? 0 : veneer + 20;
        });
    if (!thunk || (host.MemoryRead32(thunk) & 0xFF000000u) != 0xEF000000u) {
        host.GuestFree(veneer); return false;
    }
    const uint32_t code[] = {
        host.MemoryRead32(thunk), 0xE35C0000, 0x112FFF1C, 0xE3A00000, 0xE12FFF1E,
        kLookupPreimage[0], kLookupPreimage[1], 0xE51FF004, lookup + 8, 0xE7F000F0};
    for (size_t i = 0; i < std::size(code); ++i) host.MemoryWrite32(veneer + static_cast<uint32_t>(i*4), code[i]);
    host.AddExecutableRange(veneer, sizeof(code));
    host.InvalidateGuestCode(veneer, sizeof(code));
    if (packages) {
        const uint32_t package_code[] = {kPackageFindPreimage[0], kPackageFindPreimage[1], 0xE51FF004, package_find + 8};
        for (size_t i = 0; i < std::size(package_code); ++i) host.MemoryWrite32(veneer + 40 + static_cast<uint32_t>(i * 4), package_code[i]);
        host.AddExecutableRange(veneer + 40, sizeof(package_code));
        host.InvalidateGuestCode(veneer + 40, sizeof(package_code));
        host.MemoryWrite32(package_find, 0xE51FF004);
        host.MemoryWrite32(package_find + 4, veneer);
        host.InvalidateGuestCode(package_find, 8);
    }
    host.MemoryWrite32(lookup, 0xE51FF004);
    host.MemoryWrite32(lookup + 4, veneer);
    host.InvalidateGuestCode(lookup, 8);
    if (host.MemoryRead32(lookup) != 0xE51FF004 || host.MemoryRead32(lookup+4) != veneer) {
        LOGE("ru_text disabled: FOOB hook write verification failed"); return false;
    }
#endif
    if (packages && (host.MemoryRead32(package_find) != 0xE51FF004 ||
        host.MemoryRead32(package_find + 4) != host.MemoryRead32(lookup + 4))) {
        LOGE("External DLC package lookup hook write verification failed");
        return false;
    }
    s_active.store(true, std::memory_order_release);
    LOGI("locale active: FOOB=%08X veneer=%08X paths=%zu ru_text=%u ru_voice=%u engine=INT audio=English(US)",
         lookup, veneer, kBindingCount, unsigned(s_component_valid[0]), unsigned(s_component_valid[1]));
    return true;
}
#endif

// Both languages use the stock INT and English(US) lookup paths. The selected
// Russian overlays retain Android package IDs, fonts, bank IDs and stream cues.
const char* Bl2LocaleLanguage() { return "en"; }
Bl2LocaleEntry* FindBl2LocaleEntry(const char* path) {
    if (auto* orchid = OrchidFile(path)) return orchid;
    if (!s_active.load(std::memory_order_acquire)) return nullptr;
    const size_t file = FileForPath(path);
    if (file < kFileCount && s_entries[file].name[0]) return &s_entries[file];
    std::string normalized;
    if (Normalize(path, normalized)) {
        if (s_orchid_voice_fd >= 0) for (size_t i = 0; i < kOrchidVoicePaths.size(); ++i)
            if (EqualFold(kOrchidVoicePaths[i], normalized)) return &s_orchid_voice_entries[i];
        if (s_sage_admitted.load(std::memory_order_acquire) && s_sage_published.load(std::memory_order_acquire) && s_sage_voice_fd >= 0)
            for (size_t i = 0; i < kSageVoicePaths.size(); ++i)
                if (EqualFold(kSageVoicePaths[i], normalized)) return &s_sage_voice_entries[i];
        if (s_iris_admitted.load(std::memory_order_acquire) && s_iris_published.load(std::memory_order_acquire) && s_iris_voice_fd >= 0)
            for (size_t i = 0; i < kIrisVoicePaths.size(); ++i)
                if (EqualFold(kIrisVoicePaths[i], normalized)) return &s_iris_voice_entries[i];
        for (auto& entry : s_external_entries)
            if (EqualFold(entry.path, normalized)) return &entry.entry;
    }
    return nullptr;
}
bool IsBl2LocalePath(const char* path) { return FindBl2LocaleEntry(path) != nullptr; }
void NoteBl2LocaleOpen(const char* path, [[maybe_unused]] const char* operation) {
    if (!s_active.load(std::memory_order_acquire)) return;
    const size_t file = FileForPath(path);
    if (file >= kFileCount || !s_entries[file].name[0]) {
        if (const auto* entry = FindBl2LocaleEntry(path); entry && s_external_opens.fetch_add(1) < 8)
            LOGI("External OBB overlay opened: op=%s path=%s bytes=%lld", operation, path, static_cast<long long>(entry->file_size));
        return;
    }
    const uint32_t count = s_opens[file].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 4) LOGI("APK overlay opened: op=%s path=%s asset=%s bytes=%lld offset=%lld count=%u",
        operation, path, s_entries[file].name, static_cast<long long>(s_entries[file].file_size),
        static_cast<long long>(s_entries[file].data_offset), count);
}
