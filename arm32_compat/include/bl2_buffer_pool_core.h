#pragma once
// Pure bounded slot/budget logic shared by the isolated renderer and CPU checks.
#include <stddef.h>
#include <stdint.h>

namespace bl2vk::bufferpool {
constexpr uint32_t kPageBytes = 4u * 1024 * 1024;
constexpr uint32_t kSmallBytes = 64u * 1024;
constexpr uint32_t kMinSlot = 64;

constexpr uint32_t SlotClass(uint64_t size, uint64_t alignment, bool dedicated) {
    if (dedicated || !size || !alignment || size > kSmallBytes || alignment > kSmallBytes ||
        (alignment & (alignment - 1))) return 0;
    uint32_t slot = kMinSlot;
    while (slot < size || slot < alignment) slot *= 2;
    return slot;
}

struct PageKey {
    uint32_t memoryType{}, slotBytes{};
    constexpr bool operator==(const PageKey&) const = default;
};

class Slots {
    uint64_t used_[kPageBytes / kMinSlot / 64]{};
    uint32_t slot_{}, capacity_{}, live_{}, hint_{};
public:
    bool Configure(uint32_t slot) {
        if (live_ || slot < kMinSlot || slot > kSmallBytes || (slot & (slot - 1))) return false;
        slot_ = slot; capacity_ = kPageBytes / slot; hint_ = 0;
        for (auto& word : used_) word = 0;
        return true;
    }
    bool Acquire(uint32_t& index) {
        if (!slot_ || live_ == capacity_) return false;
        const uint32_t words = capacity_ / 64;
        for (uint32_t i = 0; i < words; ++i) {
            const uint32_t at = (hint_ + i) % words;
            const uint64_t free = ~used_[at];
            if (!free) continue;
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(free));
            index = at * 64 + bit; used_[at] |= uint64_t(1) << bit;
            ++live_; hint_ = at; return true;
        }
        return false;
    }
    bool Release(uint32_t index) {
        if (index >= capacity_ || !(used_[index / 64] & (uint64_t(1) << (index % 64)))) return false;
        used_[index / 64] &= ~(uint64_t(1) << (index % 64)); --live_;
        if (index / 64 < hint_) hint_ = index / 64;
        return true;
    }
    uint32_t Offset(uint32_t index) const { return index * slot_; }
    uint32_t Live() const { return live_; }
    uint32_t Capacity() const { return capacity_; }
};

class Budget {
    uint32_t limit_{}, live_{}, peak_{};
public:
    bool Configure(uint32_t limit) { if (live_ || !limit) return false; limit_ = limit; peak_ = 0; return true; }
    bool CanAllocate() const { return limit_ && live_ < limit_; }
    bool Allocated() { if (!CanAllocate()) return false; ++live_; if (live_ > peak_) peak_ = live_; return true; }
    bool Freed() { if (!live_) return false; --live_; return true; }
    uint32_t Live() const { return live_; }
    uint32_t Peak() const { return peak_; }
    uint32_t Limit() const { return limit_; }
};
}
