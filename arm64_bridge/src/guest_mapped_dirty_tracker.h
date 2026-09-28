#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <signal.h>
#include <vector>

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
    std::unique_ptr<std::atomic<uint8_t>[]> page_state_;
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
class GuestMappedWriteGuard {
public:
    GuestMappedWriteGuard(uint32_t guest_address, size_t length);
    ~GuestMappedWriteGuard();
private:
    std::unique_lock<std::mutex> lock_;
    bool dirty_ = false;
};
