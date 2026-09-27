#pragma once

#include <cstdint>
#include <limits>

// Resolve a virtual file (one STORED ZIP entry) to its containing APK. Reject
// requested byte ranges that cross the entry; mmap's page-rounded margins are
// a separate constraint and must not be treated as part of the logical file.
struct VirtualFileRange {
    bool valid;
    uint64_t absolute_offset;
};

constexpr VirtualFileRange ResolveVirtualFileRange(uint64_t entry_offset,
                                                    uint64_t entry_size,
                                                    uint64_t logical_offset,
                                                    uint64_t length,
                                                    uint64_t apk_size) {
    constexpr uint64_t max_off = static_cast<uint64_t>(
        std::numeric_limits<int64_t>::max());
    if (length == 0 || apk_size > max_off || entry_offset > apk_size ||
        entry_size > apk_size - entry_offset || logical_offset > entry_size ||
        length > entry_size - logical_offset) {
        return {false, 0};
    }
    return {true, entry_offset + logical_offset};
}
