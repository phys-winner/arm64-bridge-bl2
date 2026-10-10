#pragma once

#include "obb_pair_selection.h"
#include "zip_stored_entry_bounds.h"

#include <cstdint>
#include <string_view>

// A narrow metadata gate for a whole-container OBB stored in one cache split.
// The caller must separately validate the ZIP record signatures/lengths,
// stream-hash the complete entry and inspect the original OBB index before
// promoting this to an ObbCandidate. This helper never certifies a pair.
enum class ObbSplitEntryStatus : uint8_t {
    NotTarget,
    Rejected,
    NeedsContentVerification,
};

struct ObbSplitEntryMetadata {
    std::string_view central_name;
    std::string_view local_name;
    uint16_t central_flags;
    uint16_t local_flags;
    uint16_t central_method;
    uint16_t local_method;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint64_t apk_size;
    uint64_t central_offset;
    uint64_t local_offset;
    uint16_t local_name_length;
    uint16_t local_extra_length;
};

struct ObbSplitEntryResult {
    ObbSplitEntryStatus status;
    bool main;
    uint64_t data_offset;
    uint64_t size;
};

constexpr ObbSplitEntryResult InspectObbSplitEntry(
    const ObbSplitEntryMetadata& entry) {
    constexpr std::string_view main_path =
        "assets/main.31.com.tt2kgames.Borderlands2.obb";
    constexpr std::string_view patch_path =
        "assets/patch.31.com.tt2kgames.Borderlands2.obb";
    static_assert(main_path.substr(7) == kMainObbName);
    static_assert(patch_path.substr(7) == kPatchObbName);
    const bool main = entry.central_name == main_path;
    if (!main && entry.central_name != patch_path) {
        return {ObbSplitEntryStatus::NotTarget, false, 0, 0};
    }
    const uint64_t expected_size = main ? kMainObbSize : kPatchObbSize;
    const uint64_t relocated_size = main ? bl2_cache_payload::kMainBytes : bl2_cache_payload::kPatchBytes;
    // Only data-descriptor and UTF-8 name flags are compatible with a raw,
    // unencrypted STORED entry. The central and local records must agree.
    constexpr uint16_t allowed_flags = (1U << 3) | (1U << 11);
    if (entry.local_name != entry.central_name ||
        entry.local_name.size() != entry.local_name_length ||
        entry.central_flags != entry.local_flags ||
        (entry.central_flags & ~allowed_flags) != 0 ||
        entry.central_method != 0 || entry.local_method != 0 ||
        entry.compressed_size != entry.uncompressed_size ||
        (entry.uncompressed_size != expected_size && entry.uncompressed_size != relocated_size)) {
        return {ObbSplitEntryStatus::Rejected, main, 0, 0};
    }
    const auto bounds = ResolveZipStoredEntryBounds(
        entry.apk_size, entry.central_offset, entry.local_offset,
        entry.local_name_length, entry.local_extra_length,
        entry.compressed_size, entry.uncompressed_size);
    if (!bounds.valid) {
        return {ObbSplitEntryStatus::Rejected, main, 0, 0};
    }
    return {ObbSplitEntryStatus::NeedsContentVerification, main,
            bounds.data_offset, entry.uncompressed_size};
}
