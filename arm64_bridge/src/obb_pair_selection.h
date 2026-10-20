#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include "bl2_cache_payload.h"

// The bridge must never assemble main and patch from different storage sources.
// Installed signed APKs use PackageManager integrity verification; their ZIP
// bounds, container index and exact known layout are still checked here. Host
// verification can additionally stream-hash payloads. sha256 denotes layout
// identity for signed installed APKs, not a new runtime hash of their contents.
enum class ObbSource : uint8_t {
    EmbeddedSplits,
    ExternalDirectory,
};

struct ObbCandidate {
    ObbSource source;
    std::string_view logical_name;
    std::string_view sha256;
    uint64_t size;
    bool direct_readable;
};

struct ObbPairSelection {
    bool valid;
    ObbSource source;
    size_t main_index;
    size_t patch_index;
};

constexpr std::string_view kMainObbName =
    "main.31.com.tt2kgames.Borderlands2.obb";
constexpr std::string_view kPatchObbName =
    "patch.31.com.tt2kgames.Borderlands2.obb";
// These ZIP containers use the retained-region loader; original FOOB::mount
// trusts a raw OBB index header and must never receive them from readdir.
constexpr bool IsManagedBl2ZipObbName(std::string_view name) {
    constexpr std::string_view suffix = ".31.com.tt2kgames.Borderlands2.obb";
    return name.find_first_of("/\\") == std::string_view::npos &&
           (name == "russian_voice.31.com.tt2kgames.Borderlands2.obb" ||
            (name.starts_with("dlc_") && name.ends_with(suffix)));
}
constexpr uint64_t kMainObbSize = 2113753088ULL;
// This base is paired with the fully verified ASTC4 cache. Earlier seed280
// and its ETC2 cache remain the rollback pair; never infer a codec from size.
constexpr bool kBl2Astc4Textures = true;
constexpr uint64_t kPatchObbSize = 2647408640ULL;
constexpr std::string_view kMainObbSha256 =
    "0A7C277D91B59006A5D38148953DD1203DD0BAEE4533AB5395889EA5F49F7883";
constexpr std::string_view kPatchObbSha256 =
    "5AF9364A4DF8C38D2CAD281EAB57E3A5B875AD1A3C77597322ECFC2303B81FB8";

constexpr bool IsExpectedObb(const ObbCandidate& candidate, bool main) {
    return candidate.direct_readable &&
           candidate.logical_name == (main ? kMainObbName : kPatchObbName) &&
           ((candidate.size == (main ? kMainObbSize : kPatchObbSize) &&
             candidate.sha256 == (main ? kMainObbSha256 : kPatchObbSha256)) ||
            (candidate.size == (main ? bl2_cache_payload::kMainBytes : bl2_cache_payload::kPatchBytes) &&
             candidate.sha256 == (main ? bl2_cache_payload::kMainSha256 : bl2_cache_payload::kPatchSha256)));
}

constexpr ObbPairSelection SelectCompleteObbPair(
    std::span<const ObbCandidate> candidates) {
    for (ObbSource source : {ObbSource::EmbeddedSplits,
                             ObbSource::ExternalDirectory}) {
        size_t main_index = candidates.size();
        size_t patch_index = candidates.size();
        size_t main_count = 0;
        size_t patch_count = 0;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& entry = candidates[i];
            if (entry.source != source) continue;
            if (entry.logical_name == kMainObbName) {
                ++main_count;
                if (IsExpectedObb(entry, true)) main_index = i;
            } else if (entry.logical_name == kPatchObbName) {
                ++patch_count;
                if (IsExpectedObb(entry, false)) patch_index = i;
            }
        }
        if (main_count == 1 && patch_count == 1 &&
            main_index != candidates.size() && patch_index != candidates.size()) {
            return {true, source, main_index, patch_index};
        }
    }
    return {false, ObbSource::ExternalDirectory, candidates.size(), candidates.size()};
}
