#include "bl2_locale.h"
#include "bl2_locale_payload.h"
#if defined(BL2_LOCALE_HOST_TEST)
#include "bl2_locale_host_port.h"
#else
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#endif
#include "obb_sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <string>
#include <string_view>
#include <sys/stat.h>
#if !defined(BL2_LOCALE_HOST_TEST)
#include <unistd.h>
#endif
#include <utility>
#include <vector>
#include <zlib.h>

#define LOG_TAG "Arm32Bridge_Locale"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {
using namespace bl2_locale_payload;
constexpr char kDescriptor[] = "assets/bl2_locale/ru_text/pack.json";
constexpr char kGuestSha256[] =
    "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
constexpr uint32_t kLookupRva = 0x01E1CA4C;
constexpr uint32_t kLookupPreimage[] = {0xE92D43F8, 0xE1A07000,
                                      0xE59F30B0, 0xE08F3003};
constexpr size_t kMaxPath = 4096;
constexpr size_t kFileCount = std::size(kFiles);
[[maybe_unused]] constexpr size_t kBindingCount = std::size(kBindings);
std::array<Bl2LocaleEntry, kFileCount> s_entries{};
std::array<std::atomic<uint32_t>, kFileCount> s_opens{};
std::atomic<bool> s_active{false};
bool s_indexed = false;
bool s_payload_valid = false;
bool s_guest_valid = false;
int s_apk_fd = -1;
std::string s_files_root;

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
            if (name != "AndroidManifest.xml" && name != kDescriptor &&
                !std::string_view(name).starts_with("assets/bl2_locale/ru_text/")) continue;
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
            if (!(flags & 8) && (U32(local + 14) != U32(c + 16) ||
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

bool ManifestIdentity(const std::vector<uint8_t>& xml, bool& is_text_split) {
    is_text_split = false;
    if (xml.size() < 8 || U16(xml.data()) != 3 || U16(xml.data()+2) != 8 || U32(xml.data()+4) != xml.size()) return false;
    Strings strings;
    bool manifest_seen = false, application_seen = false, has_code = true;
    std::string package, split;
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
    is_text_split = split == "ru_text";
    return manifest_seen && (!is_text_split || (application_seen && !has_code &&
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
    return kFileCount;
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
#endif
}  // namespace

void IndexBl2LocaleSplits(const char* const* paths, size_t count, const char* game_files_root) {
    if (s_indexed) return;
    s_indexed = true;
    s_files_root = game_files_root ? game_files_root : "";
    while (!s_files_root.empty() && s_files_root.back() == '/') s_files_root.pop_back();
    if (count > 64 || (count && !paths)) { LOGE("ru_text rejected: invalid installed split path inventory"); return; }
    bool found = false, rejected = false;
    for (size_t i = 0; i < count; ++i) {
        if (!paths[i] || !*paths[i]) { rejected = true; continue; }
        Zip zip;
        if (!zip.Open(paths[i])) { LOGE("locale scan rejected malformed installed APK: %s", paths[i]); rejected = true; continue; }
        const auto* manifest = zip.Find("AndroidManifest.xml");
        std::vector<uint8_t> xml;
        bool text = false;
        if (!manifest || !zip.ReadSmall(*manifest, xml) || !ManifestIdentity(xml, text)) {
            LOGE("locale scan rejected binary manifest: %s", paths[i]); rejected = true; continue;
        }
        if (!text) continue;
        if (found) { LOGE("ru_text rejected: duplicate installed text split"); rejected = true; continue; }
        found = true;
        const auto* descriptor = zip.Find(kDescriptor);
        if (!descriptor || !zip.Verify(*descriptor, kDescriptorBytes, kDescriptorSha256)) {
            LOGE("ru_text rejected: descriptor schema/revision/identity or ZIP method mismatch"); rejected = true; continue;
        }
        bool valid = true;
        std::array<Bl2LocaleEntry, kFileCount> candidate{};
        for (size_t f = 0; f < kFileCount; ++f) {
            const auto& expected = kFiles[f];
            const auto* entry = zip.Find(expected.asset);
            if (!entry || !zip.Verify(*entry, expected.bytes, expected.sha256)) {
                LOGE("ru_text rejected: incomplete/incompatible payload %s", expected.asset); valid = false; break;
            }
            if (entry->name.size() >= sizeof(candidate[f].name)) { valid = false; break; }
            std::memcpy(candidate[f].name, entry->name.c_str(), entry->name.size()+1);
            candidate[f].apk_fd = zip.fd;
            candidate[f].data_offset = static_cast<off_t>(entry->offset);
            candidate[f].file_size = static_cast<off_t>(entry->bytes);
        }
        if (!valid) { rejected = true; continue; }
        s_entries = candidate;
        s_apk_fd = std::exchange(zip.fd, -1);
        LOGI("ru_text payload verified: split=%s descriptor=%s files=%zu no-extraction=1 fonts=GFx+native322/remap324 voice=English(US)",
             paths[i], kDescriptorSha256, kFileCount);
    }
    if (rejected || !found || s_apk_fd < 0) {
        if (s_apk_fd >= 0) close(std::exchange(s_apk_fd, -1));
        s_entries = {};
        LOGI("locale fallback: text=INT/en voice=English(US) reason=%s", rejected ? "optional split validation failed" : "ru_text absent");
        return;
    }
    s_payload_valid = true;
}

bool VerifyBl2LocaleGuest(const uint8_t* data, size_t bytes) {
    s_guest_valid = false;
    if (!s_payload_valid) return true;
    ObbSha256 hash;
    s_guest_valid = data && bytes == 49885152 && hash.Update(data, bytes) &&
        std::strcmp(ObbSha256::Hex(hash.Finalize()).data(), kGuestSha256) == 0;
    if (!s_guest_valid) LOGE("ru_text disabled: guest ELF identity mismatch");
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
    // First two instructions are position independent. Keep the real function
    // at its original address from +8 onward (its literal/GOT loads are PC relative).
    const uint32_t veneer = host.GuestAlloc(40, 4);
    if (!veneer) return false;
    const uint32_t thunk = host.GetThunkManager().RegisterThunk("bl2_locale_foob_lookup",
        [veneer](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
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
    host.MemoryWrite32(lookup, 0xE51FF004);
    host.MemoryWrite32(lookup + 4, veneer);
    host.InvalidateGuestCode(lookup, 8);
    if (host.MemoryRead32(lookup) != 0xE51FF004 || host.MemoryRead32(lookup+4) != veneer) {
        LOGE("ru_text disabled: FOOB hook write verification failed"); return false;
    }
    s_active.store(true, std::memory_order_release);
    LOGI("ru_text active: FOOB=%08X veneer=%08X paths=%zu text=ru engine=INT voice=English(US) font-policy=Android-derived-graft ETC2 native=322/remap324 Yo=203/235",
         lookup, veneer, kBindingCount);
    return true;
}
#endif

// The verified optional pack replaces INT resources. Keep the stock INT lookup
// path in both modes; removing/rejecting the split restores the original data.
const char* Bl2LocaleLanguage() { return "en"; }
Bl2LocaleEntry* FindBl2LocaleEntry(const char* path) {
    if (!s_active.load(std::memory_order_acquire)) return nullptr;
    const size_t file = FileForPath(path);
    return file < kFileCount ? &s_entries[file] : nullptr;
}
bool IsBl2LocalePath(const char* path) { return FindBl2LocaleEntry(path) != nullptr; }
void NoteBl2LocaleOpen(const char* path, [[maybe_unused]] const char* operation) {
    if (!s_active.load(std::memory_order_acquire)) return;
    const size_t file = FileForPath(path);
    if (file >= kFileCount) return;
    const uint32_t count = s_opens[file].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 4) LOGI("ru_text opened: op=%s path=%s asset=%s bytes=%lld offset=%lld sha256=%s count=%u",
        operation, path, s_entries[file].name, static_cast<long long>(s_entries[file].file_size),
        static_cast<long long>(s_entries[file].data_offset), kFiles[file].sha256, count);
}
