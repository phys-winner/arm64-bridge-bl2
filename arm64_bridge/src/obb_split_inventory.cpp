#include "obb_split_inventory.h"

#include "obb_pair_selection.h"
#include "obb_sha256.h"
#include "obb_split_entry.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#if defined(BL2_LOCALE_HOST_TEST)
#include "bl2_locale_host_port.h"
#else
#include <unistd.h>
#endif
#include <vector>
#include <zlib.h>

namespace {

constexpr size_t kMaxDirectoryBytes = 8 * 1024 * 1024;
constexpr uint16_t kMaxDirectoryEntries = 4096;
constexpr size_t kHashChunkBytes = 1024 * 1024;

uint16_t Le16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool ReadExact(int fd, uint64_t file_size, uint64_t offset,
               uint8_t* destination, size_t length) {
    if (!destination || offset > file_size || length > file_size - offset) {
        return false;
    }
    size_t done = 0;
    while (done < length) {
        const ssize_t got = pread(fd, destination + done, length - done,
                                  static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += static_cast<size_t>(got);
    }
    return true;
}

ObbSplitScanResult Rejected(const char* reason) {
    ObbSplitScanResult out;
    out.reason = reason;
    return out;
}

}  // namespace

ObbSplitScanResult ScanObbSplitFd(int fd,
                                 ObbSplitContinueFn should_continue,
                                 const void* continue_context,
                                 bool allow_rebalanced,
                                 bool verify_content) {
    struct stat file_stat{};
    if (fd < 0 || fstat(fd, &file_stat) != 0 || file_stat.st_size < 22) {
        return Rejected("invalid APK descriptor or size");
    }
    const uint64_t file_size = static_cast<uint64_t>(file_stat.st_size);
    const size_t tail_size = static_cast<size_t>(
        std::min<uint64_t>(file_size, 65535ULL + 22));
    const uint64_t tail_offset = file_size - tail_size;
    std::vector<uint8_t> tail(tail_size);
    if (!ReadExact(fd, file_size, tail_offset, tail.data(), tail.size())) {
        return Rejected("cannot read ZIP tail");
    }
    size_t eocd_at = tail.size();
    for (size_t at = tail.size() - 22;; --at) {
        if (Le32(tail.data() + at) == 0x06054B50 &&
            uint64_t(at) + 22 + Le16(tail.data() + at + 20) == tail.size()) {
            eocd_at = at;
            break;
        }
        if (at == 0) break;
    }
    if (eocd_at == tail.size()) return Rejected("ZIP EOCD not found");
    const uint8_t* eocd = tail.data() + eocd_at;
    const uint16_t entry_count = Le16(eocd + 10);
    const uint32_t directory_size = Le32(eocd + 12);
    const uint32_t directory_offset = Le32(eocd + 16);
    const uint64_t eocd_offset = tail_offset + eocd_at;
    if (Le16(eocd + 4) != 0 || Le16(eocd + 6) != 0 ||
        Le16(eocd + 8) != entry_count ||
        entry_count == 0xFFFF || entry_count > kMaxDirectoryEntries ||
        directory_size == 0xFFFFFFFF || directory_size > kMaxDirectoryBytes ||
        directory_offset == 0xFFFFFFFF ||
        uint64_t(directory_offset) + directory_size != eocd_offset) {
        return Rejected("unsupported or inconsistent classic ZIP directory");
    }
    std::vector<uint8_t> directory(directory_size);
    if (!ReadExact(fd, file_size, directory_offset,
                   directory.data(), directory.size())) {
        return Rejected("cannot read ZIP central directory");
    }

    bool found = false;
    bool main = false;
    uint64_t entry_offset = 0;
    uint64_t entry_size = 0;
    uint32_t expected_crc = 0;
    size_t at = 0;
    for (uint16_t i = 0; i < entry_count; ++i) {
        if (should_continue && !should_continue(continue_context)) {
            ObbSplitScanResult out;
            out.status = ObbSplitScanStatus::Cancelled;
            out.reason = "OBB split scan deadline exceeded";
            return out;
        }
        if (at > directory.size() || 46 > directory.size() - at ||
            Le32(directory.data() + at) != 0x02014B50) {
            return Rejected("invalid central record signature or length");
        }
        const uint8_t* central = directory.data() + at;
        const uint16_t name_length = Le16(central + 28);
        const uint16_t extra_length = Le16(central + 30);
        const uint16_t comment_length = Le16(central + 32);
        const uint64_t record_length = uint64_t{46} + name_length +
                                       extra_length + comment_length;
        if (record_length > directory.size() - at || Le16(central + 34) != 0) {
            return Rejected("invalid central record bounds or split disk");
        }
        const std::string_view name(
            reinterpret_cast<const char*>(central + 46), name_length);
        at += static_cast<size_t>(record_length);
        if (!name.ends_with(".obb")) continue;
        if (found) return Rejected("multiple OBB entries in one cache split");
        if (name != "assets/main.31.com.tt2kgames.Borderlands2.obb" &&
            name != "assets/patch.31.com.tt2kgames.Borderlands2.obb") {
            return Rejected("unexpected OBB entry name/version");
        }
        const uint32_t local_offset = Le32(central + 42);
        uint8_t local[30]{};
        if (local_offset == 0xFFFFFFFF ||
            !ReadExact(fd, file_size, local_offset, local, sizeof(local)) ||
            Le32(local) != 0x04034B50) {
            return Rejected("invalid local ZIP header");
        }
        const uint16_t local_name_length = Le16(local + 26);
        if (local_name_length > 127) {
            return Rejected("oversized local OBB entry name");
        }
        std::array<uint8_t, 128> local_name{};
        if (!ReadExact(fd, file_size, uint64_t(local_offset) + 30,
                       local_name.data(), local_name_length)) {
            return Rejected("cannot read local OBB entry name");
        }
        const uint16_t flags = Le16(central + 8);
        const uint32_t crc = Le32(central + 16);
        const uint32_t compressed_size = Le32(central + 20);
        const uint32_t uncompressed_size = Le32(central + 24);
        if ((flags & 8) == 0 &&
            (Le32(local + 14) != crc ||
             Le32(local + 18) != compressed_size ||
             Le32(local + 22) != uncompressed_size)) {
            return Rejected("local and central OBB checksums/sizes differ");
        }
        const auto inspected = InspectObbSplitEntry({
            name,
            std::string_view(reinterpret_cast<const char*>(local_name.data()),
                             local_name_length),
            flags, Le16(local + 6), Le16(central + 10), Le16(local + 8),
            compressed_size, uncompressed_size, file_size, directory_offset,
            local_offset, local_name_length, Le16(local + 28)});
        if (inspected.status !=
            ObbSplitEntryStatus::NeedsContentVerification) {
            return Rejected("OBB ZIP entry failed direct-read metadata gate");
        }
        found = true;
        main = inspected.main;
        entry_offset = inspected.data_offset;
        entry_size = inspected.size;
        expected_crc = crc;
    }
    if (at != directory.size()) {
        return Rejected("central directory length does not match entry count");
    }
    if (!found) {
        ObbSplitScanResult out;
        out.status = ObbSplitScanStatus::NoObbEntry;
        out.reason = "no OBB entry";
        return out;
    }

    uint8_t container_header[8]{};
    if (!ReadExact(fd, file_size, entry_offset, container_header,
                   sizeof(container_header)) ||
        Le32(container_header) != (main ? 0x8000U : 0x15000U) ||
        Le32(container_header + 4) != (main ? 8136U : 24368U)) {
        return Rejected("original OBB index header mismatch");
    }
    const bool rebalanced = entry_size == (main ? bl2_cache_payload::kMainBytes : bl2_cache_payload::kPatchBytes);
    if (rebalanced && !allow_rebalanced) return Rejected("relocated base cache files not available");
    const std::string_view expected_hash = rebalanced
        ? (main ? bl2_cache_payload::kMainSha256 : bl2_cache_payload::kPatchSha256)
        : (main ? kMainObbSha256 : kPatchObbSha256);
    if (!verify_content) {
        // PackageManager verifies installed APK signatures and seals /data/app.
        // Keep all direct-read bounds/index/version gates, without hashing GBs
        // again on every launch. Only the installed-path caller uses this mode.
        ObbSplitScanResult out;
        out.status = ObbSplitScanStatus::Verified;
        out.reason = "signed installed APK: compatible OBB metadata verified";
        out.main = main;
        out.apk_data_offset = entry_offset;
        out.obb_size = entry_size;
        out.zip_crc32 = expected_crc;
        std::copy(expected_hash.begin(), expected_hash.end(), out.sha256.begin());
        return out;
    }
    std::vector<uint8_t> buffer(kHashChunkBytes);
    ObbSha256 sha;
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t hashed = 0;
    while (hashed < entry_size) {
        if (should_continue && !should_continue(continue_context)) {
            ObbSplitScanResult out;
            out.status = ObbSplitScanStatus::Cancelled;
            out.reason = "OBB content hash deadline exceeded";
            out.bytes_hashed = hashed;
            return out;
        }
        const size_t count = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), entry_size - hashed));
        if (!ReadExact(fd, file_size, entry_offset + hashed,
                       buffer.data(), count) || !sha.Update(buffer.data(), count)) {
            return Rejected("cannot stream-hash complete OBB entry");
        }
        crc = crc32(crc, buffer.data(), static_cast<uInt>(count));
        hashed += count;
    }
    const auto hex = ObbSha256::Hex(sha.Finalize());
    if (!sha.IsValid() || std::string_view(hex.data()) != expected_hash ||
        static_cast<uint32_t>(crc) != expected_crc) {
        return Rejected("OBB SHA-256 or ZIP CRC32 mismatch");
    }
    ObbSplitScanResult out;
    out.status = ObbSplitScanStatus::Verified;
    out.reason = "full original OBB verified in STORED split entry";
    out.main = main;
    out.apk_data_offset = entry_offset;
    out.obb_size = entry_size;
    out.bytes_hashed = hashed;
    out.zip_crc32 = static_cast<uint32_t>(crc);
    out.content_verified = true;
    out.sha256 = hex;
    return out;
}

ExternalObbPair OpenExternalObbPair(const char* directory,
                                  ObbSplitContinueFn should_continue,
                                  const void* continue_context) {
    ExternalObbPair out;
    if (!directory || !*directory) return out;
    const std::string root = std::string(directory) + "/";
    out.fds = {{open((root + std::string(kMainObbName)).c_str(), O_RDONLY | O_CLOEXEC),
                open((root + std::string(kPatchObbName)).c_str(), O_RDONLY | O_CLOEXEC)}};
    if (out.fds[0] < 0 && out.fds[1] < 0) return out;
    const auto reject = [&](const char* reason, ObbSplitScanStatus status = ObbSplitScanStatus::Rejected) {
        for (int& fd : out.fds) if (fd >= 0) { close(fd); fd = -1; }
        out.status = status;
        out.reason = reason;
        return out;
    };
    if (out.fds[0] < 0 || out.fds[1] < 0) return reject("incomplete external main/patch OBB pair");
    std::vector<uint8_t> buffer(kHashChunkBytes);
    for (size_t i = 0; i < 2; ++i) {
        const bool main = i == 0;
        struct stat st{};
        const uint64_t expected_size = main ? kMainObbSize : kPatchObbSize;
        uint8_t header[8]{};
        if (fstat(out.fds[i], &st) != 0 || st.st_size < 0 ||
            static_cast<uint64_t>(st.st_size) != expected_size ||
            !ReadExact(out.fds[i], expected_size, 0, header, sizeof(header)) ||
            Le32(header) != (main ? 0x8000U : 0x15000U) ||
            Le32(header + 4) != (main ? 8136U : 24368U)) return reject("external OBB size/index mismatch");
        ObbSha256 sha;
        for (uint64_t at = 0; at < expected_size;) {
            if (should_continue && !should_continue(continue_context))
                return reject("external OBB hash deadline exceeded", ObbSplitScanStatus::Cancelled);
            const size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), expected_size - at));
            if (!ReadExact(out.fds[i], expected_size, at, buffer.data(), count) || !sha.Update(buffer.data(), count))
                return reject("cannot hash external OBB");
            at += count;
        }
        const auto digest = ObbSha256::Hex(sha.Finalize());
        if (!sha.IsValid() || std::string_view(digest.data()) != (main ? kMainObbSha256 : kPatchObbSha256))
            return reject("external OBB SHA-256 mismatch");
        out.sizes[i] = expected_size;
    }
    out.status = ObbSplitScanStatus::Verified;
    out.reason = "complete external OBB pair verified";
    return out;
}
