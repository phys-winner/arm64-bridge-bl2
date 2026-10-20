#include "guest_mapped_dirty_tracker.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

constexpr uint8_t kUntracked = 0;
constexpr uint8_t kArmed = 1;
constexpr uint8_t kDirty = 2;
// Dirty through the shared alias: the guest view is still read-only, so a
// rearm only resets the tag and a guest store still faults.
constexpr uint8_t kDirtyProtected = 3;

// A stamp is valid only while the caller's mapping lock excludes RO changes.
// Capture it before RW mprotect: a delayed handler must not certify a later RO.
static_assert(std::atomic<uint64_t>::is_always_lock_free);
constexpr uint64_t kStateMask = 0xFF;
uint64_t DirtyWithWritableStamp(uint32_t generation) {
    return kDirty | ((generation && !(generation & 1)
        ? static_cast<uint64_t>(generation) : uint64_t{0}) << 32);
}
bool HasWritableStamp(uint64_t state, uint32_t generation) {
    return generation && !(generation & 1) &&
           (state & kStateMask) == kDirty && state >> 32 == generation;
}

// Track/Rearm have exclusive mapping ownership. Odd means RO is in progress.
// Never reuse a generation: saturation disables this optimization until reset.
class ReadOnlyGenerationScope {
public:
    explicit ReadOnlyGenerationScope(std::atomic<uint32_t>& generation)
        : generation_(generation) {
        const uint32_t current = generation_.load(std::memory_order_relaxed);
        generation_.store(current && current < UINT32_MAX - 1 ? current + 1 : 0,
                          std::memory_order_release);
    }
    ~ReadOnlyGenerationScope() {
        const uint32_t current = generation_.load(std::memory_order_relaxed);
        if (current) generation_.store(current + 1, std::memory_order_release);
    }
private:
    std::atomic<uint32_t>& generation_;
};
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
    // Lock-free writers may still read a published table: never replace it.
    if (page_state_ && page_count != page_count_) return false;

    if (!page_state_) {
        auto states = std::make_unique<std::atomic<uint64_t>[]>(page_count);
        for (size_t i = 0; i < page_count; ++i) {
            states[i].store(kUntracked, std::memory_order_relaxed);
        }
        page_state_ = std::move(states);
    }
    std::vector<uint16_t> references(page_count, 0);
    guest_base_ = guest_base;
    guest_size_ = guest_size;
    page_size_ = page_size;
    page_count_ = page_count;
    page_references_ = std::move(references);
    protection_generation_.store(2, std::memory_order_relaxed);
    dirty_epoch_.store(0, std::memory_order_relaxed);
    last_scanned_epoch_ = 0;

    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_sigaction = &GuestMappedDirtyTracker::HandleSegv;
    if (sigaction(SIGSEGV, &action, &previous_segv_) != 0) return false;
    s_active_tracker.store(this, std::memory_order_release);
    installed_ = true;
    published_state_.store(page_state_.get(), std::memory_order_release);
    return true;
}

bool GuestMappedDirtyTracker::MayContainTracked(uint32_t guest_address,
                                                size_t length) const {
    const auto* states = published_state_.load(std::memory_order_acquire);
    // The release store above orders page_size_/page_count_ before the table.
    if (!states || !length) return false;
    const uint64_t first = guest_address / page_size_;
    const uint64_t past_last =
        std::min<uint64_t>((uint64_t{guest_address} + length - 1) / page_size_ + 1,
                           page_count_);
    for (uint64_t page = first; page < past_last; ++page) {
        if ((states[page].load(std::memory_order_acquire) & kStateMask) != kUntracked)
            return true;
    }
    return false;
}

bool GuestMappedDirtyTracker::ShareRange(uint8_t* guest_base, size_t guest_size,
                                         uint32_t guest_address, size_t length) {
    const long system_page = sysconf(_SC_PAGESIZE);
    if (system_page <= 0 || !guest_base || !length ||
        guest_address >= guest_size || length > guest_size - guest_address) return false;
    const size_t page = static_cast<size_t>(system_page);
    if ((page & (page - 1)) || guest_address % page || length % page ||
        reinterpret_cast<uintptr_t>(guest_base) % page) return false;
    if (shared_fd_ < 0) {
        if (shared_failed_) return false;
        shared_failed_ = true;
        const int fd = static_cast<int>(syscall(__NR_memfd_create, "bl2-mapped-staging",
                                                 MFD_CLOEXEC));
        void* alias = mmap(nullptr, guest_size, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (fd < 0 || alias == MAP_FAILED || ftruncate(fd, static_cast<off_t>(guest_size))) {
            if (fd >= 0) close(fd);
            if (alias != MAP_FAILED) munmap(alias, guest_size);
            return false;
        }
        auto pages = std::make_unique<std::atomic<uint8_t>[]>(guest_size / page);
        for (size_t i = 0; i < guest_size / page; ++i)
            pages[i].store(0, std::memory_order_relaxed);
        shared_failed_ = false;
        shared_fd_ = fd;
        alias_base_ = static_cast<uint8_t*>(alias);
        shared_size_ = guest_size;
        shared_page_size_ = page;
        shared_pages_ = std::move(pages);
    }
    if (guest_size != shared_size_) return false;
    // Contents move into the shared object before the guest view switches;
    // the caller's exclusive ownership keeps the copy race-free.
    const size_t first = guest_address / page;
    const size_t past_last = first + length / page;
    for (size_t run = first; run < past_last;) {
        if (shared_pages_[run].load(std::memory_order_acquire)) { ++run; continue; }
        size_t end = run + 1;
        while (end < past_last && !shared_pages_[end].load(std::memory_order_acquire)) ++end;
        const size_t offset = run * page;
        const size_t bytes = (end - run) * page;
        uint8_t* alias = alias_base_ + offset;
        uint8_t* view = guest_base + offset;
        if (mmap(alias, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                 shared_fd_, static_cast<off_t>(offset)) == MAP_FAILED) return false;
        std::memcpy(alias, view, bytes);
        if (mmap(view, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                 shared_fd_, static_cast<off_t>(offset)) == MAP_FAILED) {
            // MAP_FIXED may already have dropped the old pages: restore them.
            if (mmap(view, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != MAP_FAILED)
                std::memcpy(view, alias, bytes);
            mmap(alias, bytes, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
            return false;
        }
        for (size_t item = run; item < end; ++item)
            shared_pages_[item].store(1, std::memory_order_release);
        run = end;
    }
    return true;
}

uint8_t* GuestMappedDirtyTracker::AliasFor(uint32_t guest_address,
                                           size_t length) const {
    if (shared_fd_ < 0 || !length) return nullptr;
    const uint64_t first = guest_address / shared_page_size_;
    const uint64_t past_last = (uint64_t{guest_address} + length - 1) / shared_page_size_ + 1;
    if (past_last * shared_page_size_ > shared_size_) return nullptr;
    for (uint64_t page = first; page < past_last; ++page) {
        if (!shared_pages_[page].load(std::memory_order_acquire)) return nullptr;
    }
    return alias_base_ + guest_address;
}

void GuestMappedDirtyTracker::CompleteAliasWrite(uint32_t guest_address,
                                                 size_t length) {
    size_t first = 0, past_last = 0;
    if (!PageRange(guest_address, length, &first, &past_last)) return;
    bool tracked = false;
    for (size_t page = first; page < past_last; ++page) {
        if (!page_references_[page]) continue;
        tracked = true;
        // A concurrent guest-store fault may already have made it RW/dirty.
        uint64_t expected = kArmed;
        page_state_[page].compare_exchange_strong(expected, kDirtyProtected,
                                                  std::memory_order_acq_rel);
    }
    // Every state precedes the notification, as in the fault handler.
    if (tracked) dirty_epoch_.fetch_add(1, std::memory_order_release);
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
    const ReadOnlyGenerationScope protection_scope(protection_generation_);
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
    if (!installed_ || page_index >= page_count_) return false;
    const uint64_t state =
        page_state_[page_index].load(std::memory_order_acquire) & kStateMask;
    return state == kDirty || state == kDirtyProtected;
}

void GuestMappedDirtyTracker::AppendDirtyPages(size_t first_page,
                                               size_t past_last_page,
                                               std::vector<size_t>& output) const {
    if (!installed_) return;
    const size_t end = std::min(past_last_page, page_count_);
    const auto* states = page_state_.get();
    for (size_t page = first_page; page < end; ++page) {
        const uint64_t state =
            states[page].load(std::memory_order_acquire) & kStateMask;
        if (state == kDirty || state == kDirtyProtected) output.push_back(page);
    }
}

void GuestMappedDirtyTracker::Rearm(std::vector<size_t>& pages) {
    if (!installed_ || pages.empty()) return;
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    // Alias-written pages never left read-only. A guest fault racing this
    // reset wins the CAS and is re-protected below as an ordinary dirty page.
    for (const size_t page : pages) {
        if (page >= page_count_ || page_references_[page] == 0) continue;
        uint64_t expected = kDirtyProtected;
        page_state_[page].compare_exchange_strong(expected, kArmed,
                                                  std::memory_order_acq_rel);
    }
    const ReadOnlyGenerationScope protection_scope(protection_generation_);
    for (size_t i = 0; i < pages.size();) {
        const size_t begin = i;
        const size_t first = pages[i++];
        if (first >= page_count_ || page_references_[first] == 0 ||
            (page_state_[first].load(std::memory_order_acquire) & kStateMask) != kDirty) {
            continue;
        }
        size_t past_last = first + 1;
        while (i < pages.size()) {
            const size_t next = pages[i];
            if (next >= page_count_ || page_references_[next] == 0 ||
                (page_state_[next].load(std::memory_order_acquire) & kStateMask) != kDirty) {
                break;
            }
            // Exclusive mapping ownership keeps references stable. Already
            // watched gap pages may be included in RO, never untracked pages.
            size_t gap = past_last;
            while (gap < next && page_references_[gap] != 0) ++gap;
            if (gap != next) break;
            past_last = next + 1;
            ++i;
        }
        // Clear only the captured dirty pages. A gap may become dirty after
        // the scan; retaining its state and epoch preserves that later write.
        for (size_t item = begin; item < i; ++item) {
            page_state_[pages[item]].store(kArmed, std::memory_order_release);
        }
        if (ProtectPage(PagePointer(first), (past_last - first) * page_size_,
                        PROT_READ)) continue;
        // Gap states are untouched even when a range partially succeeds.
        for (size_t item = begin; item < i; ++item) {
            const size_t page = pages[item];
            if (past_last - first > 1 &&
                ProtectPage(PagePointer(page), page_size_, PROT_READ)) continue;
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
    const uint32_t generation = protection_generation_.load(std::memory_order_acquire);
    bool tracked = false;
    for (size_t page = first; page < past_last;) {
        if (!page_references_[page]) { ++page; continue; }
        tracked = true;
        if (HasWritableStamp(page_state_[page].load(std::memory_order_acquire),
                             generation)) { ++page; continue; }
        const size_t begin = page++;
        // Retain one batch for the remaining contiguous tracked run, even
        // when an already-ready interior page could otherwise split it.
        while (page < past_last && page_references_[page] != 0) ++page;
        const bool writable = ProtectPage(PagePointer(begin),
            (page - begin) * page_size_, PROT_READ | PROT_WRITE);
        for (size_t current = begin; current < page; ++current) {
            const bool confirmed = writable || (page - begin > 1 &&
                ProtectPage(PagePointer(current), page_size_,
                            PROT_READ | PROT_WRITE));
            // A failed/partial operation stays dirty with no RW certification.
            page_state_[current].store(confirmed ? DirtyWithWritableStamp(generation) : kDirty,
                                        std::memory_order_release);
        }
    }
    errno = saved_errno;
    return tracked;
}

void GuestMappedDirtyTracker::Shutdown() {
    if (!installed_) return;
    protection_generation_.store(0, std::memory_order_release);
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
                ? tracker->page_state_[page].load(std::memory_order_acquire) & kStateMask
                : kUntracked;
            // Another thread may have re-protected a page while this thread
            // was still writing it. Handle both armed and already-dirty states
            // instead of passing that expected protection fault to Dynarmic.
            // Alias-dirty pages are still read-only in the guest view.
            if (state == kArmed || state == kDirty || state == kDirtyProtected) {
                const uint32_t generation =
                    tracker->protection_generation_.load(std::memory_order_acquire);
                const bool writable = ProtectPage(tracker->PagePointer(page),
                    tracker->page_size_, PROT_READ | PROT_WRITE);
                tracker->page_state_[page].store(
                    writable ? DirtyWithWritableStamp(generation) : kDirty,
                    std::memory_order_release);
                // State must precede notification, even on failure. A scan
                // racing this publication must observe a later epoch change.
                tracker->dirty_epoch_.fetch_add(1, std::memory_order_release);
                if (writable) return;
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
