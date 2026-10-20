#include "guest_mapped_dirty_tracker.h"

#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t size = page * 4;
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 2;
    volatile auto* bytes = static_cast<volatile uint8_t*>(memory);
    GuestMappedDirtyTracker tracker;
    unsigned cases = 0, failures = 0;
    const auto expect = [&](bool passed, const char* label) {
        ++cases;
        if (!passed) {
            ++failures;
            std::fprintf(stderr, "FAIL %s\n", label);
        }
    };
    const auto check_ranges = [&](const char* label) {
        const size_t last = std::numeric_limits<size_t>::max();
        const size_t ranges[][2] = {
            {0, 0}, {0, 1}, {0, 4}, {0, last}, {1, 3}, {1, last},
            {3, 4}, {3, 8}, {4, 8},
            {4, last}, {2, 1}, {last, last}, {last - 1, last}};
        std::vector<size_t> reference{9, 2, 2}, actual = reference;
        const uint32_t epoch_before = tracker.DirtyEpoch();
        for (const auto& range : ranges) {
            // IsDirty returns false outside the four-page fixture; cap the
            // reference loop only to avoid iterating to SIZE_MAX in this test.
            for (size_t p = range[0]; p < std::min(range[1], size_t{4}); ++p)
                if (tracker.IsDirty(p)) reference.push_back(p);
            tracker.AppendDirtyPages(range[0], range[1], actual);
        }
        expect(actual == reference && tracker.DirtyEpoch() == epoch_before,
               label);
    };

    check_ranges("uninitialized range scan leaves output/epoch unchanged");
    expect(tracker.Initialize(static_cast<uint8_t*>(memory), size), "initialize");
    check_ranges("untracked ranges equal IsDirty");
    expect(tracker.Track(1, page + 8), "track unaligned two-page range");
    check_ranges("armed ranges equal IsDirty");
    expect(!tracker.IsDirty(0) && !tracker.IsDirty(1), "new pages clean");
    bytes[1] = 0x23;
    expect(tracker.IsDirty(0) && !tracker.IsDirty(1), "first guest write dirty");
    check_ranges("sparse fault-dirty ranges retain order and duplicates");
    // Simulate another thread re-protecting a dirty page just before a write.
    expect(mprotect(memory, page, PROT_READ) == 0, "race reprotection");
    bytes[1] = 0x24;
    expect(tracker.IsDirty(0) && bytes[1] == 0x24,
           "fault on dirty reprotected page is handled");
    std::vector<size_t> dirty_pages{0};
    tracker.Rearm(dirty_pages);
    expect(!tracker.IsDirty(0) && dirty_pages.size() == 1, "rearm first page");
    bytes[page + 2] = 0x45;
    expect(tracker.IsDirty(1) && !tracker.IsDirty(0), "second page isolated");
    dirty_pages = {0, 1};
    tracker.Rearm(dirty_pages);
    const uint32_t epoch = tracker.DirtyEpoch();
    errno = EDOM;
    expect(tracker.PrepareNativeWrite(static_cast<uint32_t>(page - 2), 4),
           "native range unlocks both referenced pages");
    expect(errno == EDOM && tracker.DirtyEpoch() == epoch,
           "prepare preserves errno and waits to publish completed write");
    const uint8_t source[] = {1, 2, 3, 4};
    std::memcpy(static_cast<uint8_t*>(memory) + page - 2, source, sizeof(source));
    tracker.CompleteNativeWrite();
    expect(tracker.DirtyEpoch() == epoch + 1 && tracker.IsDirty(0) &&
               tracker.IsDirty(1) && bytes[page - 2] == 1 && bytes[page + 1] == 4,
           "cross-page memcpy writes exactly and publishes once without faults");
    check_ranges("writable-stamped native dirty ranges equal IsDirty");
    // Production re-protection is Rearm/Track, which retires RW certificates.
    dirty_pages = {0};
    tracker.Rearm(dirty_pages);
    expect(tracker.PrepareNativeWrite(1, 1),
           "native write confirms RW for a rearmed page");
    bytes[1] = 0x47;
    tracker.CompleteNativeWrite();
    expect(tracker.DirtyEpoch() == epoch + 2 && bytes[1] == 0x47,
           "rearmed native write avoids the fault path");
    expect(!tracker.PrepareNativeWrite(static_cast<uint32_t>(page * 2), page),
           "untracked destination is not dirtied");
    expect(!tracker.PrepareNativeWrite(static_cast<uint32_t>(size - 1), 2) &&
               !tracker.PrepareNativeWrite(1, 0),
           "invalid and empty native spans do not arm pages");
    dirty_pages = {0, 1};
    tracker.Rearm(dirty_pages);
    bytes[page + 2] = 0x46;
    expect(tracker.DirtyEpoch() == epoch + 3 && tracker.IsDirty(1) &&
               !tracker.IsDirty(0), "guest fault path remains active after native write");
    tracker.Untrack(1, page + 8);
    expect(!tracker.IsDirty(1), "untrack resets dirty state");
    check_ranges("untracked-after-dirty ranges equal IsDirty");
    bytes[1] = 0x67;
    bytes[page + 2] = 0x89;
    expect(bytes[1] == 0x67 && bytes[page + 2] == 0x89,
           "untracked writes remain writable");

    expect(tracker.Track(1, page + 8), "track again");
    expect(tracker.Track(static_cast<uint32_t>(page - 3), 8),
           "overlap both watched pages");
    tracker.Untrack(1, page + 8);
    bytes[page - 1] = 0xAB;
    expect(tracker.IsDirty(0), "overlapping owner keeps watch");
    check_ranges("overlapping tracked ownership range scan is read-only");
    tracker.Untrack(static_cast<uint32_t>(page - 3), 8);
    expect(!tracker.IsDirty(0), "last owner removes watch");

    // Shared alias: pages 2..3 hold a persistent mapping's staging copy.
    const uint32_t staging = static_cast<uint32_t>(page * 2);
    bytes[staging + 5] = 0x5A;
    expect(!tracker.MayContainTracked(staging, page * 2), "unwatched range is lock-free");
    expect(tracker.AliasFor(staging, 1) == nullptr, "no alias before sharing");
    expect(tracker.ShareRange(static_cast<uint8_t*>(memory), size, staging, page * 2),
           "share staging pages");
    expect(bytes[staging + 5] == 0x5A, "sharing preserves contents");
    expect(tracker.ShareRange(static_cast<uint8_t*>(memory), size, staging, page),
           "sharing is idempotent");
    uint8_t* alias = tracker.AliasFor(staging, page * 2);
    expect(alias != nullptr && tracker.AliasFor(staging - 1, 2) == nullptr,
           "alias covers only shared pages");
    if (alias) {
        alias[7] = 0x77;
        expect(bytes[staging + 7] == 0x77, "alias and guest view share bytes");
        expect(tracker.Track(staging, page * 2), "track shared staging");
        expect(tracker.MayContainTracked(staging + page - 1, 2) &&
                   !tracker.MayContainTracked(0, page), "watched range takes the slow path");
        const uint32_t before_alias = tracker.DirtyEpoch();
        const uint8_t fill[] = {9, 8, 7, 6};
        std::memcpy(alias + page - 2, fill, sizeof(fill));
        tracker.CompleteAliasWrite(staging + static_cast<uint32_t>(page) - 2, sizeof(fill));
        expect(tracker.DirtyEpoch() == before_alias + 1 &&
                   tracker.IsDirty(2) && tracker.IsDirty(3) &&
                   bytes[staging + page - 2] == 9 && bytes[staging + page + 1] == 6,
               "alias write publishes both pages once");
        check_ranges("alias-dirty-protected ranges equal IsDirty");
        std::vector<size_t> shared_dirty{2, 3};
        tracker.Rearm(shared_dirty);
        expect(!tracker.IsDirty(2) && !tracker.IsDirty(3) && shared_dirty.size() == 2,
               "alias-dirty pages rearm by tag only");
        bytes[staging + 3] = 0x31;
        expect(tracker.IsDirty(2) && !tracker.IsDirty(3) && bytes[staging + 3] == 0x31,
               "guest store still faults after alias write and rearm");
        check_ranges("mixed armed and fault-dirty ranges equal IsDirty");
        tracker.CompleteAliasWrite(staging, 1);
        expect(tracker.IsDirty(2), "alias write keeps a fault-dirty RW page dirty");
        shared_dirty = {2};
        tracker.Rearm(shared_dirty);
        bytes[staging + 4] = 0x32;
        expect(tracker.IsDirty(2), "RW page was re-protected, not just retagged");
        std::memcpy(alias + page, fill, 1);
        tracker.CompleteAliasWrite(staging + static_cast<uint32_t>(page), 1);
        expect(tracker.IsDirty(3) && bytes[staging + page] == 9, "alias-dirty third page");
        bytes[staging + page + 1] = 0x33;
        expect(tracker.IsDirty(3) && bytes[staging + page + 1] == 0x33,
               "guest store faults on an alias-dirty read-only page");
        shared_dirty = {2, 3};
        tracker.Rearm(shared_dirty);
        tracker.Untrack(staging, page * 2);
        expect(!tracker.MayContainTracked(staging, page * 2), "untrack restores fast path");
        bytes[staging + 9] = 0x99;
        expect(alias[9] == 0x99, "untracked shared pages stay writable");
    }
    expect(tracker.Track(0, size), "track all pages for dense range scan");
    for (size_t i = 0; i < 4; ++i) bytes[i * page] = static_cast<uint8_t>(i + 1);
    check_ranges("fully dirty first-to-last-page ranges equal IsDirty");
    tracker.Shutdown();
    check_ranges("shutdown range scan leaves output/epoch unchanged");
    bytes[page - 1] = 0xCD;
    expect(bytes[page - 1] == 0xCD, "shutdown restores write access");
    munmap(memory, size);
    std::printf("guest_mapped_dirty_tracker: %u cases, %u failures\n",
                cases, failures);
    return failures ? 1 : 0;
}
