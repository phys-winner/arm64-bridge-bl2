#include "obb_sha256.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t index = 0; index < table.size(); ++index) {
        uint32_t crc = index;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0);
        }
        table[index] = crc;
    }
    return table;
}

constexpr auto kCrcTable = MakeCrcTable();

bool ParseUnsigned(const char* text, uint64_t& result) {
    if (!text || !*text || *text == '-') return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (*end != '\0' || value == std::numeric_limits<unsigned long long>::max()) {
        return false;
    }
    result = value;
    return true;
}

}  // namespace

// Host-only positive large-file test of the exact SHA primitive used by the
// Android split scanner. It reads a signed APK entry in place and never writes
// or extracts an OBB. It does not replace ScanObbSplitFd's Android runtime test.
int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: obb_sha256_file_smoke APK OFFSET SIZE SHA256 CRC32\n");
        return 2;
    }
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t expected_crc = 0;
    if (!ParseUnsigned(argv[2], offset) || !ParseUnsigned(argv[3], size) ||
        !ParseUnsigned(argv[5], expected_crc) || expected_crc > UINT32_MAX ||
        std::string_view(argv[4]).size() != 64) {
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "cannot open APK\n");
        return 2;
    }
    const auto apk_size = file.tellg();
    if (apk_size < 0 || offset > static_cast<uint64_t>(apk_size) ||
        size > static_cast<uint64_t>(apk_size) - offset) {
        std::fprintf(stderr, "entry range exceeds APK\n");
        return 2;
    }
    file.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> buffer(1024 * 1024);
    ObbSha256 sha;
    uint32_t crc = 0xFFFFFFFFU;
    const auto started = std::chrono::steady_clock::now();
    uint64_t done = 0;
    while (done < size) {
        const auto count = static_cast<std::streamsize>(
            std::min<uint64_t>(buffer.size(), size - done));
        file.read(reinterpret_cast<char*>(buffer.data()), count);
        if (file.gcount() != count || !sha.Update(buffer.data(),
                                                   static_cast<size_t>(count))) {
            std::fprintf(stderr, "entry stream ended early\n");
            return 1;
        }
        for (std::streamsize i = 0; i < count; ++i) {
            crc = (crc >> 8) ^ kCrcTable[(crc ^ buffer[i]) & 0xFFU];
        }
        done += static_cast<uint64_t>(count);
    }
    crc ^= 0xFFFFFFFFU;
    const auto digest = ObbSha256::Hex(sha.Finalize());
    const auto seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const bool match = sha.IsValid() && std::string_view(digest.data()) == argv[4] &&
                       crc == expected_crc;
    std::printf("bytes=%llu sha256=%s crc32=0x%08X elapsed_s=%.3f match=%d\n",
                static_cast<unsigned long long>(done), digest.data(), crc,
                seconds, match ? 1 : 0);
    return match ? 0 : 1;
}
