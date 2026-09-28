#include "guest_mapped_dirty_tracker.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

constexpr uint8_t kUntracked = 0;
constexpr uint8_t kArmed = 1;
constexpr uint8_t kDirty = 2;
std::atomic<GuestMappedDirtyTracker*> s_active_tracker{nullptr};

// The signal path cannot allocate, take a mutex, log, or call GL. The kernel
// syscall keeps the fault handler independent of libc's signal-safe subset.
bool ProtectPage(void* address, size_t length, int protection) {
    return syscall(SYS_mprotect, address, length, protection) == 0;
}

}  // namespace

bool GuestMappedDirtyTracker::Initialize(uint8_t* guest_base, size_t guest_size) {
    if (installed_) return guest_base_ == guest_base && guest_size_ == guest_size;
    if (!guest_base || guest_size == 0) return false;
    const long system_page_size = sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0) return false;
    const size_t page_size = static_cast<size_t>(system_page_size);
    if ((page_size & (page_size - 1)) != 0 ||
        reinterpret_cast<uintptr_t>(guest_base) % page_size != 0) {
        return false;
    }
    const size_t page_count = guest_size / page_size;
    if (page_count == 0 || page_count > 1'048'576) return false;

    auto states = std::make_unique<std::atomic<uint8_t>[]>(page_count);
    for (size_t i = 0; i < page_count; ++i) {
        states[i].store(kUntracked, std::memory_order_relaxed);
    }
    std::vector<uint16_t> references(page_count, 0);
    guest_base_ = guest_base;
    guest_size_ = guest_size;
    page_size_ = page_size;
    page_count_ = page_count;
    page_state_ = std::move(states);
    page_references_ = std::move(references);
    dirty_epoch_.store(0, std::memory_order_relaxed);
    last_scanned_epoch_ = 0;

    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_sigaction = &GuestMappedDirtyTracker::HandleSegv;
    if (sigaction(SIGSEGV, &action, &previous_segv_) != 0) return false;
    s_active_tracker.store(this, std::memory_order_release);
    installed_ = true;
    return true;
}

bool GuestMappedDirtyTracker::PageRange(uint32_t guest_address, size_t length,
                                        size_t* first, size_t* past_last) const {
    if (!installed_ || !length || guest_address >= guest_size_ ||
        length > guest_size_ - guest_address) return false;
    *first = guest_address / page_size_;
    *past_last = (static_cast<uint64_t>(guest_address) + length - 1) /
                     page_size_ + 1;
    return *past_last <= page_count_;
}

void* GuestMappedDirtyTracker::PagePointer(size_t page_index) const {
    return guest_base_ + page_index * page_size_;
}

bool GuestMappedDirtyTracker::Track(uint32_t guest_address, size_t length) {
    size_t first = 0, past_last = 0;
    if (!PageRange(guest_address, length, &first, &past_last)) return false;
    size_t page = first;
    for (; page < past_last; ++page) {
        if (page_references_[page] == std::numeric_limits<uint16_t>::max()) break;
        if (page_references_[page]++ != 0) continue;
        // State precedes protection so the handler recognizes a concurrent
        // fault immediately after mprotect. Draw and mapping updates hold the
        // caller's mapping mutex; signal handling itself never takes it.
        page_state_[page].store(kArmed, std::memory_order_release);
        if (!ProtectPage(PagePointer(page), page_size_, PROT_READ)) {
            page_state_[page].store(kUntracked, std::memory_order_release);
            page_references_[page] = 0;
            break;
        }
    }
    if (page == past_last) return true;
    for (size_t rollback = first; rollback < page; ++rollback) {
        if (--page_references_[rollback] == 0) {
            ProtectPage(PagePointer(rollback), page_size_, PROT_READ | PROT_WRITE);
            page_state_[rollback].store(kUntracked, std::memory_order_release);
        }
    }
    return false;
}

void GuestMappedDirtyTracker::Untrack(uint32_t guest_address, size_t length) {
    size_t first = 0, past_last = 0;
    if (!PageRange(guest_address, length, &first, &past_last)) return;
    for (size_t page = first; page < past_last; ++page) {
        if (!page_references_[page] || --page_references_[page] != 0) continue;
        ProtectPage(PagePointer(page), page_size_, PROT_READ | PROT_WRITE);
        page_state_[page].store(kUntracked, std::memory_order_release);
    }
}

bool GuestMappedDirtyTracker::IsDirty(size_t page_index) const {
    return installed_ && page_index < page_count_ &&
           page_state_[page_index].load(std::memory_order_acquire) == kDirty;
}

void GuestMappedDirtyTracker::Rearm(std::vector<size_t>& pages) {
    if (!installed_ || pages.empty()) return;
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (size_t page : pages) {
        if (page >= page_count_ || page_references_[page] == 0 ||
            page_state_[page].load(std::memory_order_acquire) != kDirty) {
            continue;
        }
        page_state_[page].store(kArmed, std::memory_order_release);
        if (!ProtectPage(PagePointer(page), page_size_, PROT_READ)) {
            // A failed re-arm keeps this page on the slow, exact compare path.
            page_state_[page].store(kDirty, std::memory_order_release);
            dirty_epoch_.fetch_add(1, std::memory_order_release);
        }
    }
}

bool GuestMappedDirtyTracker::PrepareNativeWrite(uint32_t guest_address,
                                                 size_t length) {
    size_t first = 0, past_last = 0;
    if (!PageRange(guest_address, length, &first, &past_last)) return false;
    const int saved_errno = errno;
    bool tracked = false;
    for (size_t page = first; page < past_last; ++page) {
        if (!page_references_[page]) continue;
        tracked = true;
        // A guest fault racing a previous re-arm may leave a dirty page RO.
        if (ProtectPage(PagePointer(page), page_size_, PROT_READ | PROT_WRITE)) {
            page_state_[page].store(kDirty, std::memory_order_release);
        }
    }
    errno = saved_errno;
    return tracked;
}

void GuestMappedDirtyTracker::Shutdown() {
    if (!installed_) return;
    for (size_t page = 0; page < page_count_; ++page) {
        if (page_references_[page] != 0) {
            ProtectPage(PagePointer(page), page_size_, PROT_READ | PROT_WRITE);
            page_references_[page] = 0;
            page_state_[page].store(kUntracked, std::memory_order_release);
        }
    }
    struct sigaction current{};
    if (sigaction(SIGSEGV, nullptr, &current) == 0 &&
        (current.sa_flags & SA_SIGINFO) &&
        current.sa_sigaction == &GuestMappedDirtyTracker::HandleSegv) {
        sigaction(SIGSEGV, &previous_segv_, nullptr);
    }
    s_active_tracker.store(nullptr, std::memory_order_release);
    installed_ = false;
    last_scanned_epoch_ = 0;
}

void GuestMappedDirtyTracker::HandleSegv(int signal, siginfo_t* info,
                                         void* context) {
    GuestMappedDirtyTracker* tracker =
        s_active_tracker.load(std::memory_order_acquire);
    // Some device kernels report a protected guest page as MAPERR while the
    // VMA remains mapped read-only. Only retry pages we explicitly armed, and
    // only when mprotect confirms that the mapping still exists.
    if (signal == SIGSEGV && info && tracker &&
        (info->si_code == SEGV_ACCERR || info->si_code == SEGV_MAPERR)) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
        const uintptr_t base = reinterpret_cast<uintptr_t>(tracker->guest_base_);
        if (address >= base && address - base < tracker->guest_size_) {
            const size_t page = (address - base) / tracker->page_size_;
            const uint8_t state = page < tracker->page_count_
                ? tracker->page_state_[page].load(std::memory_order_acquire)
                : kUntracked;
            // Another thread may have re-protected a page while this thread
            // was still writing it. Handle both armed and already-dirty states
            // instead of passing that expected protection fault to Dynarmic.
            if ((state == kArmed || state == kDirty) &&
                ProtectPage(tracker->PagePointer(page), tracker->page_size_,
                            PROT_READ | PROT_WRITE)) {
                tracker->page_state_[page].store(kDirty,
                                                 std::memory_order_release);
                tracker->dirty_epoch_.fetch_add(1, std::memory_order_release);
                return;
            }
        }
    }
    ChainPrevious(signal, info, context);
}

void GuestMappedDirtyTracker::ChainPrevious(int signal, siginfo_t* info,
                                            void* context) {
    GuestMappedDirtyTracker* tracker =
        s_active_tracker.load(std::memory_order_acquire);
    if (!tracker) {
        syscall(SYS_exit_group, 128 + signal);
        return;
    }
    const struct sigaction& previous = tracker->previous_segv_;
    if (previous.sa_handler == SIG_IGN) {
        return;
    } else if (previous.sa_handler == SIG_DFL) {
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigaction(signal, &action, nullptr);
        syscall(SYS_tgkill, getpid(), gettid(), signal);
    } else if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, context);
    } else {
        previous.sa_handler(signal);
    }
}
