#pragma once

#include <cstdint>

struct ZipStoredEntryBounds {
    bool valid;
    uint64_t data_offset;
};

// Central and local headers are before the central directory in these classic
// APK ZIPs. Reject an entry whose claimed raw bytes would cross that boundary.
constexpr ZipStoredEntryBounds ResolveZipStoredEntryBounds(
    uint64_t apk_size, uint64_t central_offset, uint64_t local_offset,
    uint64_t local_name_length, uint64_t local_extra_length,
    uint64_t compressed_size, uint64_t uncompressed_size) {
    if (compressed_size != uncompressed_size ||
        central_offset > apk_size || local_offset > central_offset ||
        30 > central_offset - local_offset) {
        return {false, 0};
    }
    const uint64_t after_header = local_offset + 30;
    if (local_name_length > central_offset - after_header) return {false, 0};
    const uint64_t after_name = after_header + local_name_length;
    if (local_extra_length > central_offset - after_name) return {false, 0};
    const uint64_t data_offset = after_name + local_extra_length;
    if (compressed_size > central_offset - data_offset) return {false, 0};
    return {true, data_offset};
}
