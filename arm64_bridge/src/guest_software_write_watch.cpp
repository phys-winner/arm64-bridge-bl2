#include "guest_software_write_watch.h"

#include <limits>
#include <new>

namespace {
static_assert(sizeof(std::atomic<uint8_t>) == 1);
static_assert(alignof(std::atomic<uint8_t>) == 1);
static_assert(std::atomic<uint8_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
constexpr uint64_t kA32AddressSpace = uint64_t{1} << 32;
std::atomic<GuestSoftwareWriteWatch*> active_watch{nullptr};
}

bool GuestSoftwareWriteWatch::Initialize(size_t guest_size, unsigned page_shift) {
    if (flags_) return guest_size_ == guest_size && page_shift_ == page_shift;
    if (!guest_size || guest_size > kA32AddressSpace ||
        page_shift < 12 || page_shift > 30) return false;
    const size_t page_count = size_t{1} << (32 - page_shift);
    try {
        auto flags = std::make_unique<std::atomic<uint8_t>[]>(page_count);
        std::vector<uint16_t> references(page_count, 0);
        for (size_t page = 0; page < page_count; ++page) {
            flags[page].store(0, std::memory_order_relaxed);
        }
        flags_ = std::move(flags);
        references_ = std::move(references);
    } catch (const std::bad_alloc&) {
        return false;
    }
    guest_size_ = guest_size;
    page_count_ = page_count;
    page_shift_ = page_shift;
    return true;
}

bool GuestSoftwareWriteWatch::PageRange(uint32_t address, size_t length,
                                       size_t* first, size_t* past_last) const {
    if (!flags_ || !length || address >= guest_size_ ||
        length > guest_size_ - address) return false;
    *first = address >> page_shift_;
    *past_last = ((static_cast<uint64_t>(address) + length - 1) >> page_shift_) + 1;
    return *past_last <= page_count_;
}

void GuestSoftwareWriteWatch::MarkRange(uint32_t address, size_t length) {
    size_t first = 0, past_last = 0;
    if (!PageRange(address, length, &first, &past_last)) return;
    for (size_t page = first; page < past_last; ++page) {
        // Do not skip this store when already dirty: a consumer may clear the
        // flag concurrently between the guest write and this publication.
        flags_[page].store(1, std::memory_order_release);
    }
}

bool GuestSoftwareWriteWatch::Track(uint32_t address, size_t length) {
    size_t first = 0, past_last = 0;
    if (!PageRange(address, length, &first, &past_last)) return false;
    for (size_t page = first; page < past_last; ++page) {
        if (references_[page] == std::numeric_limits<uint16_t>::max()) return false;
    }
    for (size_t page = first; page < past_last; ++page) {
        if (references_[page]++ == 0) {
            // The mandatory initial copy occurs after this clear. A mark
            // racing that copy remains set and is consumed by a later draw.
            flags_[page].exchange(0, std::memory_order_acq_rel);
        }
    }
    return true;
}

void GuestSoftwareWriteWatch::Untrack(uint32_t address, size_t length) {
    size_t first = 0, past_last = 0;
    if (!PageRange(address, length, &first, &past_last)) return;
    for (size_t page = first; page < past_last; ++page) {
        if (references_[page]) --references_[page];
    }
}

bool GuestSoftwareWriteWatch::ConsumeDirty(size_t page) {
    return flags_ && page < page_count_ && references_[page] &&
        flags_[page].load(std::memory_order_acquire) &&
        flags_[page].exchange(0, std::memory_order_acq_rel) != 0;
}

bool InitializeGuestSoftwareWriteWatch(size_t guest_size) {
    // Never destroy or replace a table whose raw pointer can be cached in a JIT.
    static auto* watch = new GuestSoftwareWriteWatch;
    if (!watch->Initialize(guest_size)) return false;
    active_watch.store(watch, std::memory_order_release);
    return true;
}

GuestSoftwareWriteWatch* CurrentGuestSoftwareWriteWatch() {
    return active_watch.load(std::memory_order_acquire);
}

uint8_t* GuestSoftwareWriteWatchFlags() {
    auto* watch = CurrentGuestSoftwareWriteWatch();
    return watch ? watch->Flags() : nullptr;
}

unsigned GuestSoftwareWriteWatchPageShift() {
    auto* watch = CurrentGuestSoftwareWriteWatch();
    return watch ? watch->PageShift() : 14;
}

void NotifyGuestMemoryWritten(uint32_t address, size_t length) {
    if (auto* watch = CurrentGuestSoftwareWriteWatch()) watch->MarkRange(address, length);
}
