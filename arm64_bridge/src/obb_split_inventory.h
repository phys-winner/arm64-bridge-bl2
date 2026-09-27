#pragma once

#include <array>
#include <cstdint>

enum class ObbSplitScanStatus : uint8_t {
    NoObbEntry,
    Verified,
    Cancelled,
    Rejected,
};

struct ObbSplitScanResult {
    ObbSplitScanStatus status = ObbSplitScanStatus::Rejected;
    const char* reason = "not scanned";
    bool main = false;
    uint64_t apk_data_offset = 0;
    uint64_t obb_size = 0;
    uint64_t bytes_hashed = 0;
    uint32_t zip_crc32 = 0;
    std::array<char, 65> sha256{};
};

// The caller owns fd and must retain that same descriptor for direct reads.
// A Verified result proves one exact original OBB in this single classic-ZIP
// APK; it does not select or expose a complete main+patch pair by itself.
using ObbSplitContinueFn = bool (*)(const void*);
ObbSplitScanResult ScanObbSplitFd(int fd,
                                 ObbSplitContinueFn should_continue = nullptr,
                                 const void* continue_context = nullptr);
