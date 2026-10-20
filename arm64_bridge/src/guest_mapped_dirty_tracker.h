#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <signal.h>
#include <vector>
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
#include "bl2_physx_cookie_watch.h"
#endif

// Opt-in write watch for the guest staging copies of persistent GL mappings.
// Only pages belonging to registered mappings are write-protected. A guest
// store (including Dynarmic fastmem stores) faults once, makes its page
// writable, and marks it dirty for the next draw-time synchronization.
class GuestMappedDirtyTracker {
public:
    bool Initialize(uint8_t* guest_base, size_t guest_size);
    bool Track(uint32_t guest_address, size_t length);
    void Untrack(uint32_t guest_address, size_t length);
    bool IsDirty(size_t page_index) const;
    // Caller holds the mapping mutex, excluding Initialize/Shutdown. Append
    // the same ascending dirty indexes as IsDirty over this clipped range;
    // preserve existing output entries/duplicates and do not consume state.
    void AppendDirtyPages(size_t first_page, size_t past_last_page,
                          std::vector<size_t>& output) const;
    // Lock-free: false only when no page of the range is watched. A Track()
    // racing a write protects first, so the fault path or its initial full
    // copy still observes that write.
    bool MayContainTracked(uint32_t guest_address, size_t length) const;
    // Rebacks whole guest pages with shared memory and maps a host-only RW
    // alias of them. Contents are preserved; already shared pages are kept.
    // Caller holds the mapping mutex and owns the range exclusively.
    bool ShareRange(uint8_t* guest_base, size_t guest_size,
                    uint32_t guest_address, size_t length);
    // Alias of the first byte when every page of the range has one.
    uint8_t* AliasFor(uint32_t guest_address, size_t length) const;
    // Caller holds the mapping mutex. Publishes a completed write made through
    // the alias: watched pages stay read-only and need no rearm syscall.
    void CompleteAliasWrite(uint32_t guest_address, size_t length);
    uint32_t DirtyEpoch() const {
        return dirty_epoch_.load(std::memory_order_acquire);
    }
    bool NeedsDirtyScan(uint32_t epoch) const {
        return epoch != last_scanned_epoch_;
    }
    void MarkDirtyScanComplete(uint32_t epoch) { last_scanned_epoch_ = epoch; }
    size_t PageSize() const { return page_size_; }
    void Rearm(std::vector<size_t>& pages);
    // Caller holds the mapping mutex through the native write and publication.
    bool PrepareNativeWrite(uint32_t guest_address, size_t length);
    void CompleteNativeWrite() {
        dirty_epoch_.fetch_add(1, std::memory_order_release);
    }
    void Shutdown();

private:
    static void HandleSegv(int signal, siginfo_t* info, void* context);
    static void ChainPrevious(int signal, siginfo_t* info, void* context);
    bool PageRange(uint32_t guest_address, size_t length,
                   size_t* first, size_t* past_last) const;
    void* PagePointer(size_t page_index) const;

    uint8_t* guest_base_ = nullptr;
    size_t guest_size_ = 0;
    size_t page_size_ = 0;
    size_t page_count_ = 0;
    // Low byte is the dirty tag; upper 32 bits certify an RW generation.
    std::unique_ptr<std::atomic<uint64_t>[]> page_state_;
    // Published after Initialize for lock-free readers; never reallocated.
    std::atomic<std::atomic<uint64_t>*> published_state_{nullptr};
    // Shared backing: one sparse memfd offset by guest address, and a
    // reserved alias window with the same layout.
    int shared_fd_ = -1;
    bool shared_failed_ = false;  // Setup failed once: keep the RO path.
    uint8_t* alias_base_ = nullptr;
    size_t shared_size_ = 0;
    size_t shared_page_size_ = 0;
    std::unique_ptr<std::atomic<uint8_t>[]> shared_pages_;
    // Exclusive Track/Rearm advance this before and after RO syscalls.
    // 0 permanently disables RW skipping after saturation or shutdown.
    std::atomic<uint32_t> protection_generation_{2};
    // The fault handler increments this before returning to the guest store.
    // The render thread uses it to skip page-state scans when no completed
    // guest write could have happened since its previous scan.
    std::atomic<uint32_t> dirty_epoch_{0};
    uint32_t last_scanned_epoch_ = 0;  // Caller holds the mapping mutex.
    std::vector<uint16_t> page_references_;
    struct sigaction previous_segv_{};
    bool installed_ = false;
};

// Native memcpy/memmove/memset must not fault through ART for each watched page.
// Keep GPU synchronization excluded until the completed write is published.
// Writes that touch no watched page take neither the mapping mutex nor a
// syscall; watched shared pages are written through their RW alias.
class GuestMappedWriteGuard {
public:
    // alias_allowed=false keeps the guest view (an overlapping memmove must
    // compare one address space).
    GuestMappedWriteGuard(uint32_t guest_address, size_t length,
                          bool alias_allowed = true);
    ~GuestMappedWriteGuard();
    GuestMappedWriteGuard(const GuestMappedWriteGuard&) = delete;
    GuestMappedWriteGuard& operator=(const GuestMappedWriteGuard&) = delete;
    // Host pointer the native write must use for this guest range.
    void* Destination(void* guest_view) const {
        return alias_ ? static_cast<void*>(alias_) : guest_view;
    }
private:
    std::unique_lock<std::mutex> lock_;
    bool dirty_ = false;
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    Bl2PhysxCookieWatch::NativeToken cookie_token_;
#endif
    bool software_ = false;
    uint8_t* alias_ = nullptr;
    uint32_t address_ = 0;
    size_t length_ = 0;
};
