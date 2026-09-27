#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The bridge must never assemble main and patch from different storage sources.
// An entry reaches this selector only after its ZIP bounds/container index and
// full SHA-256 have been verified by the source-specific reader.
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
constexpr uint64_t kMainObbSize = 2113753088ULL;
constexpr uint64_t kPatchObbSize = 2009309184ULL;
constexpr std::string_view kMainObbSha256 =
    "0A7C277D91B59006A5D38148953DD1203DD0BAEE4533AB5395889EA5F49F7883";
constexpr std::string_view kPatchObbSha256 =
    "8CFC8A76747EDA5CF01A6D7625F2D612B2119CF034ECA8537CD27D8B4A38EA24";

constexpr bool IsExpectedObb(const ObbCandidate& candidate, bool main) {
    return candidate.direct_readable &&
           candidate.logical_name == (main ? kMainObbName : kPatchObbName) &&
           candidate.size == (main ? kMainObbSize : kPatchObbSize) &&
           candidate.sha256 == (main ? kMainObbSha256 : kPatchObbSha256);
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
