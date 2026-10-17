#include "obb_pair_selection.h"

#include <array>

constexpr ObbCandidate kMainSplit{ObbSource::EmbeddedSplits, kMainObbName,
                                  kMainObbSha256, kMainObbSize, true};
constexpr ObbCandidate kPatchSplit{ObbSource::EmbeddedSplits, kPatchObbName,
                                   kPatchObbSha256, kPatchObbSize, true};
constexpr ObbCandidate kMainExternal{ObbSource::ExternalDirectory, kMainObbName,
                                     kMainObbSha256, kMainObbSize, true};
constexpr ObbCandidate kPatchExternal{ObbSource::ExternalDirectory, kPatchObbName,
                                      kPatchObbSha256, kPatchObbSize, true};

constexpr std::array kBothSources{kMainExternal, kPatchExternal,
                                  kMainSplit, kPatchSplit};
static_assert(SelectCompleteObbPair(kBothSources).valid);
static_assert(SelectCompleteObbPair(kBothSources).source ==
              ObbSource::EmbeddedSplits);
static_assert(SelectCompleteObbPair(kBothSources).main_index == 2);
static_assert(SelectCompleteObbPair(kBothSources).patch_index == 3);

constexpr std::array kExternalOnly{kMainExternal, kPatchExternal};
static_assert(SelectCompleteObbPair(kExternalOnly).valid);
static_assert(SelectCompleteObbPair(kExternalOnly).source ==
              ObbSource::ExternalDirectory);

constexpr std::array kMixed{kMainSplit, kPatchExternal};
static_assert(!SelectCompleteObbPair(kMixed).valid);

constexpr std::array kMissingPatch{kMainSplit};
static_assert(!SelectCompleteObbPair(kMissingPatch).valid);

constexpr ObbCandidate kCorruptMain{ObbSource::EmbeddedSplits, kMainObbName,
                                    "WRONG", kMainObbSize, true};
constexpr std::array kCorruptSplitWithExternal{kCorruptMain, kPatchSplit,
                                                kMainExternal, kPatchExternal};
static_assert(SelectCompleteObbPair(kCorruptSplitWithExternal).valid);
static_assert(SelectCompleteObbPair(kCorruptSplitWithExternal).source ==
              ObbSource::ExternalDirectory);

constexpr std::array kDuplicateMain{kMainSplit, kMainSplit, kPatchSplit};
static_assert(!SelectCompleteObbPair(kDuplicateMain).valid);

constexpr ObbCandidate kCompressedPatch{ObbSource::EmbeddedSplits,
                                        kPatchObbName, kPatchObbSha256,
                                        kPatchObbSize, false};
constexpr std::array kNotDirectReadable{kMainSplit, kCompressedPatch};
static_assert(!SelectCompleteObbPair(kNotDirectReadable).valid);

constexpr ObbCandidate kTruncatedMain{ObbSource::EmbeddedSplits,
                                      kMainObbName, kMainObbSha256,
                                      kMainObbSize - 1, true};
constexpr std::array kWrongSize{kTruncatedMain, kPatchSplit};
static_assert(!SelectCompleteObbPair(kWrongSize).valid);

constexpr ObbCandidate kWrongVersion{ObbSource::EmbeddedSplits,
                                     "main.33.com.tt2kgames.Borderlands2.obb",
                                     kMainObbSha256, kMainObbSize, true};
constexpr std::array kWrongName{kWrongVersion, kPatchSplit};
static_assert(!SelectCompleteObbPair(kWrongName).valid);
