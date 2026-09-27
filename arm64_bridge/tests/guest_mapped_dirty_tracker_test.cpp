#include "guest_mapped_dirty_tracker.h"

#include <cstdio>
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

    expect(tracker.Initialize(static_cast<uint8_t*>(memory), size), "initialize");
    expect(tracker.Track(1, page + 8), "track unaligned two-page range");
    expect(!tracker.IsDirty(0) && !tracker.IsDirty(1), "new pages clean");
    bytes[1] = 0x23;
    expect(tracker.IsDirty(0) && !tracker.IsDirty(1), "first guest write dirty");
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
    tracker.Untrack(1, page + 8);
    expect(!tracker.IsDirty(1), "untrack resets dirty state");
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
    tracker.Untrack(static_cast<uint32_t>(page - 3), 8);
    expect(!tracker.IsDirty(0), "last owner removes watch");
    tracker.Shutdown();
    bytes[page - 1] = 0xCD;
    expect(bytes[page - 1] == 0xCD, "shutdown restores write access");
    munmap(memory, size);
    std::printf("guest_mapped_dirty_tracker: %u cases, %u failures\n",
                cases, failures);
    return failures ? 1 : 0;
}
