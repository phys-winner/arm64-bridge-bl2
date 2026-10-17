#include "bl2_locale_host_port.h"
#include "obb_split_inventory.h"
#include "obb_pair_selection.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    assert(argc == 3);
    std::array<ObbCandidate, 2> candidates{};
    for (int i = 0; i < 2; ++i) {
        const int fd = open(argv[i + 1], O_RDONLY);
        assert(fd >= 0);
        const auto result = ScanObbSplitFd(fd, nullptr, nullptr, false, false);
        const bool main = i == 0;
        assert(result.status == ObbSplitScanStatus::Verified && result.main == main);
        assert(result.obb_size == (main ? kMainObbSize : kPatchObbSize));
        assert(std::strcmp(result.sha256.data(), (main ? kMainObbSha256 : kPatchObbSha256).data()) == 0);
        assert(!result.content_verified && result.bytes_hashed == 0);
        std::array<uint8_t, 32> tail{};
        assert(pread(fd, tail.data(), tail.size(), result.apk_data_offset + result.obb_size - tail.size()) == tail.size());
        candidates[i] = {ObbSource::EmbeddedSplits, main ? kMainObbName : kPatchObbName,
                         main ? kMainObbSha256 : kPatchObbSha256, result.obb_size, true};
        const auto cancel = [](const void*) {return false;};
        assert(ScanObbSplitFd(fd, cancel, nullptr, false, false).status == ObbSplitScanStatus::Cancelled);
        std::printf("PASS: %s, full OBB %llu bytes, offset %llu, tail readable without signed-32 truncation\n",
                    argv[i + 1], static_cast<unsigned long long>(result.obb_size),
                    static_cast<unsigned long long>(result.apk_data_offset));
        close(fd);
    }
    assert(SelectCompleteObbPair(candidates).valid);
}
