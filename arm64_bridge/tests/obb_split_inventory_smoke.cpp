#include "obb_split_inventory.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    const bool cancel = argc == 3 && std::strcmp(argv[2], "cancel") == 0;
    if (argc == 3 && !cancel) return 2;
    const int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::perror("open");
        return 2;
    }
    const auto stop_immediately = [](const void*) { return false; };
    const auto result = cancel
        ? ScanObbSplitFd(fd, stop_immediately)
        : ScanObbSplitFd(fd);
    close(fd);
    std::printf("status=%u reason=%s main=%d offset=%llu size=%llu hashed=%llu\n",
                static_cast<unsigned>(result.status), result.reason,
                result.main ? 1 : 0,
                static_cast<unsigned long long>(result.apk_data_offset),
                static_cast<unsigned long long>(result.obb_size),
                static_cast<unsigned long long>(result.bytes_hashed));
    const auto expected = cancel ? ObbSplitScanStatus::Cancelled
                                 : ObbSplitScanStatus::NoObbEntry;
    return result.status == expected ? 0 : 1;
}
