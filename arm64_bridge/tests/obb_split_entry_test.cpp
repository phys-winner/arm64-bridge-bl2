#include "obb_split_entry.h"

constexpr ObbSplitEntryMetadata kMain{
    "assets/main.31.com.tt2kgames.Borderlands2.obb",
    "assets/main.31.com.tt2kgames.Borderlands2.obb",
    0, 0, 0, 0, kMainObbSize, kMainObbSize,
    2147483647ULL, 2147480000ULL, 32768ULL, 45, 0};
static_assert(InspectObbSplitEntry(kMain).status ==
              ObbSplitEntryStatus::NeedsContentVerification);
static_assert(InspectObbSplitEntry(kMain).main);
static_assert(InspectObbSplitEntry(kMain).data_offset == 32843ULL);
static_assert(InspectObbSplitEntry(kMain).size == kMainObbSize);

constexpr auto kSlimMain = [] {
    auto entry = kMain;
    entry.compressed_size = entry.uncompressed_size = bl2_cache_payload::kMainBytes;
    return entry;
}();
static_assert(InspectObbSplitEntry(kSlimMain).status == ObbSplitEntryStatus::NeedsContentVerification);
static_assert(InspectObbSplitEntry(kSlimMain).size == bl2_cache_payload::kMainBytes);
constexpr auto kSlimTruncated = [] {
    auto entry = kSlimMain;
    --entry.compressed_size;
    return entry;
}();
static_assert(InspectObbSplitEntry(kSlimTruncated).status == ObbSplitEntryStatus::Rejected);

constexpr ObbSplitEntryMetadata kPatch{
    "assets/patch.31.com.tt2kgames.Borderlands2.obb",
    "assets/patch.31.com.tt2kgames.Borderlands2.obb",
    0, 0, 0, 0, kPatchObbSize, kPatchObbSize,
    2647421364ULL, 2647420928ULL, 628ULL, 46, 0};
static_assert(InspectObbSplitEntry(kPatch).status ==
              ObbSplitEntryStatus::NeedsContentVerification);
static_assert(!InspectObbSplitEntry(kPatch).main);
static_assert(InspectObbSplitEntry(kPatch).data_offset == 704ULL);
static_assert(InspectObbSplitEntry(kPatch).size == kPatchObbSize);

constexpr auto kPatchSignedLimit = [] {
    auto entry = kPatch;
    entry.apk_size = 2147483647ULL;
    entry.central_offset = 2147480000ULL;
    return entry;
}();
static_assert(InspectObbSplitEntry(kPatchSignedLimit).status ==
              ObbSplitEntryStatus::Rejected);

constexpr ObbSplitEntryMetadata kOther{
    "assets/other.obb", "assets/other.obb", 0, 0, 0, 0,
    kMainObbSize, kMainObbSize, 2147483647ULL, 2147480000ULL,
    32768ULL, 16, 0};
static_assert(InspectObbSplitEntry(kOther).status ==
              ObbSplitEntryStatus::NotTarget);

constexpr auto kEncrypted = [] {
    auto entry = kMain;
    entry.central_flags = 1;
    entry.local_flags = 1;
    return entry;
}();
static_assert(InspectObbSplitEntry(kEncrypted).status ==
              ObbSplitEntryStatus::Rejected);

constexpr auto kCompressed = [] {
    auto entry = kMain;
    entry.central_method = 8;
    entry.local_method = 8;
    return entry;
}();
static_assert(InspectObbSplitEntry(kCompressed).status ==
              ObbSplitEntryStatus::Rejected);

constexpr auto kMismatchedName = [] {
    auto entry = kMain;
    entry.local_name = "assets/patch.31.com.tt2kgames.Borderlands2.obb";
    return entry;
}();
static_assert(InspectObbSplitEntry(kMismatchedName).status ==
              ObbSplitEntryStatus::Rejected);

constexpr auto kTruncated = [] {
    auto entry = kMain;
    entry.uncompressed_size -= 1;
    return entry;
}();
static_assert(InspectObbSplitEntry(kTruncated).status ==
              ObbSplitEntryStatus::Rejected);

constexpr auto kCrossesDirectory = [] {
    auto entry = kMain;
    entry.central_offset = entry.local_offset + 30;
    return entry;
}();
static_assert(InspectObbSplitEntry(kCrossesDirectory).status ==
              ObbSplitEntryStatus::Rejected);

constexpr auto kDescriptor = [] {
    auto entry = kMain;
    entry.central_flags = 8;
    entry.local_flags = 8;
    return entry;
}();
static_assert(InspectObbSplitEntry(kDescriptor).status ==
              ObbSplitEntryStatus::NeedsContentVerification);

int main() { return 0; }
