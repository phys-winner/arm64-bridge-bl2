#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Startup-only software alternative to the RO tracker. Marking never reads
// mapping membership: fastmem stores may race registration or unregistration.
// This storage is pinned for every JIT borrowing Flags().
class GuestSoftwareWriteWatch {
public:
    bool Initialize(size_t guest_size, unsigned page_shift = 14);
    uint8_t* Flags() const {
        return reinterpret_cast<uint8_t*>(flags_.get());
    }
    unsigned PageShift() const { return page_shift_; }
    size_t PageSize() const { return size_t{1} << page_shift_; }
    size_t PageCount() const { return page_count_; }
    void MarkRange(uint32_t address, size_t length);
    // Mapping membership and consumption require the existing mapping mutex.
    bool Track(uint32_t address, size_t length);
    void Untrack(uint32_t address, size_t length);
    bool ConsumeDirty(size_t page);
    void BeginUnknown() {
        unknown_active_.fetch_add(1, std::memory_order_acq_rel);
    }
    bool UnknownActive() const {
        return unknown_active_.load(std::memory_order_acquire) != 0;
    }
    // Caller first marks CURRENT mappings under the mapping mutex. An unknown
    // call can create or remove a mapping, so an entry-time list is inadequate.
    void EndUnknown() {
        unknown_completed_.fetch_add(1, std::memory_order_relaxed);
        unknown_active_.fetch_sub(1, std::memory_order_release);
    }
    uint64_t UnknownCompleted() const {
        return unknown_completed_.load(std::memory_order_relaxed);
    }
private:
    bool PageRange(uint32_t address, size_t length,
                   size_t* first, size_t* past_last) const;
    size_t guest_size_ = 0;
    size_t page_count_ = 0;
    unsigned page_shift_ = 14;
    std::unique_ptr<std::atomic<uint8_t>[]> flags_;
    std::vector<uint16_t> references_;
    std::atomic<uint64_t> unknown_active_{0};
    std::atomic<uint64_t> unknown_completed_{0};
};

// Initialize before constructing either primary or callback JITs. No live
// switching, signal handler, protection syscall, or per-store global epoch.
bool InitializeGuestSoftwareWriteWatch(size_t guest_size);
GuestSoftwareWriteWatch* CurrentGuestSoftwareWriteWatch();
uint8_t* GuestSoftwareWriteWatchFlags();
unsigned GuestSoftwareWriteWatchPageShift();
void NotifyGuestMemoryWritten(uint32_t address, size_t length);

// The definitions live beside the private GL mapping collection. Unknown
// bodies do not hold its mutex: callbacks must be able to issue a nested draw.
class GuestUnclassifiedWriteScope {
public:
    explicit GuestUnclassifiedWriteScope(bool writes_observed);
    ~GuestUnclassifiedWriteScope();
    GuestUnclassifiedWriteScope(const GuestUnclassifiedWriteScope&) = delete;
    GuestUnclassifiedWriteScope& operator=(const GuestUnclassifiedWriteScope&) = delete;
private:
    GuestSoftwareWriteWatch* watch_ = nullptr;
};
