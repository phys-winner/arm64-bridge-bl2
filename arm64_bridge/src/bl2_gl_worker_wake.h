#pragma once

#include <atomic>
#include <cstdint>

namespace bl2::gl_worker {
constexpr std::uint32_t kAwake = 0;
constexpr std::uint32_t kArmed = 1;
constexpr std::uint32_t kWakePending = 2;

// Called after the announced head_word's seq_cst store. This seq_cst RMW
// retains the original Dekker store/load edge, including on failure. Only
// one announcement claims the wake while the worker has not resumed yet.
inline bool ClaimWake(std::atomic<std::uint32_t>& state) {
    std::uint32_t expected = kArmed;
    return state.compare_exchange_strong(expected, kWakePending,
            std::memory_order_seq_cst, std::memory_order_seq_cst);
}

// A zero/error return did not dequeue the worker. It may not have entered
// FUTEX_WAIT yet, so a later announcement must be allowed to wake it. The
// conditional reset never overwrites the worker's Awake/new Armed state.
inline void CompleteWake(std::atomic<std::uint32_t>& state, long woken) {
    if (woken > 0) return;
    std::uint32_t expected = kWakePending;
    state.compare_exchange_strong(expected, kArmed,
            std::memory_order_seq_cst, std::memory_order_seq_cst);
}

inline bool NeedsAnnouncement(std::uint32_t state) { return state == kArmed; }
}  // namespace bl2::gl_worker
