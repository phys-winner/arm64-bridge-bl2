#include "bl2_gl_worker_wake.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

using namespace bl2::gl_worker;

// All interleavings of three publications/announcements (15 producer steps)
// and four sleep epochs (24 worker steps). The head acquire may see the older
// value only when the head_word SC load did not synchronize that publication.
// Futex comparison and sleep registration are atomic, as FUTEX_WAIT requires.
struct Model {
    unsigned p = 0, w = 0, head = 0, word = 0, state = kAwake, tail = 0;
    unsigned observed = 0, seen = 0, claims = 0, wakes = 0, arms = 0;
    unsigned successes = 0, last_success_arm = ~0u;
    unsigned claim_arm = 0, last_success_claim_arm = ~0u;
    bool waiting = false;
    auto key() const {
        return std::tuple(p,w,head,word,state,tail,observed,seen,claims,wakes,arms,successes,last_success_arm,claim_arm,last_success_claim_arm,waiting);
    }
    bool operator<(const Model& other) const { return key() < other.key(); }
};
enum class Mode { Coalesced, UnsafePending, NeverWake, OldDuplicateWake };
struct ModelResult { std::uint64_t states = 0; bool lost = false, duplicate = false; };

ModelResult Explore(Mode mode) {
    std::set<Model> visited;
    ModelResult result;
    std::function<void(Model)> walk = [&](Model s) {
        if (!visited.insert(s).second) return;
        ++result.states;
        if (s.p == 15 && s.waiting && s.head > s.tail) {
            result.lost = true;
            return;
        }
        if (s.p == 15 && s.head == s.tail) return;
        // A kernel timeout may resume the worker after CAS, before the
        // producer actually enters FUTEX_WAKE. Let it disarm/rearm so the
        // old wake can arrive in the next sleep epoch.
        if (s.waiting && s.p < 15 && s.p % 5 == 3 && (s.claims & (1u << (s.p / 5)))) {
            auto n = s;
            n.waiting = false;
            walk(n);
        }
        if (s.p < 15) {
            auto n = s;
            switch (s.p % 5) {
            case 0: n.head = s.p / 5 + 1; break; // release publication
            case 1: n.word = n.head; break;      // SC announcement
            case 2: {
                std::atomic<std::uint32_t> state{s.state};
                const bool claim = (mode == Mode::Coalesced || mode == Mode::UnsafePending) ? ClaimWake(state) :
                                   mode == Mode::OldDuplicateWake && s.state == kArmed;
                n.state = state.load();
                if (claim) { n.claims |= 1u << (s.p / 5); n.claim_arm = s.arms; }
                break;
            }
            case 3:
                if (s.claims & (1u << (s.p / 5))) {
                    // A wake claimed in an older epoch may dequeue a newer
                    // waiter; its current Armed state legitimately permits
                    // one more wake. Only same-epoch successful wakes coalesce.
                    if (s.last_success_arm == s.arms && s.last_success_claim_arm == s.arms)
                        result.duplicate = true;
                    ++n.wakes;
                    if (s.waiting) {
                        n.successes |= 1u << (s.p / 5);
                        n.last_success_arm = s.arms;
                        n.last_success_claim_arm = s.claim_arm;
                        n.waiting = false;
                    }
                }
                break;
            case 4:
                if (mode == Mode::Coalesced && (s.claims & (1u << (s.p / 5)))) {
                    std::atomic<std::uint32_t> state{s.state};
                    CompleteWake(state,(s.successes & (1u << (s.p / 5))) ? 1 : 0);
                    n.state = state.load();
                }
                break;
            }
            ++n.p;
            walk(n);
        }
        if (s.w >= 24 || (s.w % 6 == 4 && s.waiting)) return;
        auto n = s;
        ++n.w;
        switch (s.w % 6) {
        case 0: n.state = kArmed; ++n.arms; break; // SC arm
        case 1: n.observed = s.word; break;        // SC word load
        case 2:
            n.seen = s.head;
            walk(n);
            // An old acquire is allowed absent a matching SC announcement.
            if (s.observed < s.head && s.observed >= s.tail) {
                n.seen = s.observed;
                walk(n);
            }
            return;
        case 3: n.waiting = s.seen == s.tail && s.word == s.observed; break;
        case 4: n.state = kAwake; break;
        case 5: n.tail = s.seen; break;
        }
        walk(n);
    };
    walk({});
    return result;
}

void Stress(unsigned spin, unsigned records) {
    constexpr unsigned capacity = 256;
    std::array<std::uint64_t,capacity> ring{};
    alignas(128) std::atomic<unsigned> head{0};
    alignas(128) std::atomic<unsigned> word{0};
    alignas(128) std::atomic<std::uint32_t> state{kAwake};
    alignas(128) std::atomic<unsigned> tail{0};
    std::atomic<bool> done{false};
    std::atomic<unsigned> arms{0}, wakes{0}, successful_wakes{0}, timeouts_with_work{0};
    std::mutex mutex;
    std::condition_variable cv;
    unsigned wake_serial = 0;
    bool queued_waiter = false;
    const auto wake = [&] {
        std::lock_guard lock(mutex);
        if (!queued_waiter) return 0L;
        queued_waiter = false;
        ++wake_serial;
        ++successful_wakes;
        cv.notify_one();
        return 1L;
    };
    const auto announce = [&](unsigned position) {
        word.store(position, std::memory_order_seq_cst);
        if (ClaimWake(state)) { ++wakes; CompleteWake(state,wake()); }
    };
    std::thread worker([&] {
        unsigned consumed = 0;
        for (;;) {
            unsigned published = head.load(std::memory_order_acquire);
            for (unsigned i = 0; i < spin && published == consumed; ++i) {
                std::this_thread::yield();
                published = head.load(std::memory_order_acquire);
            }
            if (published == consumed) {
                state.store(kArmed, std::memory_order_seq_cst);
                ++arms;
                const unsigned announced = word.load(std::memory_order_seq_cst);
                if (head.load(std::memory_order_acquire) == consumed && !done.load(std::memory_order_acquire)) {
                    std::unique_lock lock(mutex);
                    if (word.load(std::memory_order_seq_cst) == announced) {
                        const unsigned serial = wake_serial;
                        queued_waiter = true;
                        if (!cv.wait_for(lock,std::chrono::milliseconds(500),[&]{ return wake_serial != serial; }) &&
                            head.load(std::memory_order_acquire) != consumed)
                            ++timeouts_with_work;
                        queued_waiter = false;
                    }
                }
                state.store(kAwake, std::memory_order_relaxed);
                if (done.load(std::memory_order_acquire) && head.load(std::memory_order_acquire) == consumed) break;
                continue;
            }
            while (consumed < published) {
                const auto expected = (std::uint64_t(consumed) << 32) ^ (0x5A17C0DEu + consumed);
                assert(ring[consumed % capacity] == expected);
                ++consumed;
                tail.store(consumed, std::memory_order_release);
            }
        }
        assert(consumed == records);
    });
    unsigned unannounced = 0;
    for (unsigned i = 0; i < records; ++i) {
        while (i - tail.load(std::memory_order_acquire) == capacity) std::this_thread::yield();
        ring[i % capacity] = (std::uint64_t(i) << 32) ^ (0x5A17C0DEu + i);
        head.store(i + 1,std::memory_order_release);
        // Force some ordinary/synchronous announcements and many idle edges.
        if (++unannounced >= 32 || i % 13 == 0 || NeedsAnnouncement(state.load(std::memory_order_relaxed))) {
            unannounced = 0;
            announce(i + 1);
        }
        if (i % 127 == 0) {
            announce(i + 1); // all work announced before this intentional pause
            while (tail.load(std::memory_order_acquire) != i + 1) std::this_thread::yield();
            // A scheduling handoff exercises the arm/wait edge without relying
            // on the host OS's sub-millisecond timer resolution.
            for (unsigned pause = 0; pause < 1000 && state.load() == kAwake; ++pause)
                std::this_thread::yield();
        }
    }
    done.store(true,std::memory_order_release);
    announce(records);
    worker.join();
    assert(tail.load() == records && timeouts_with_work.load() == 0);
    assert(successful_wakes.load() <= arms.load());
    std::printf("PASS: queue stress spin=%u records=%u sleep_epochs=%u wake_calls=%u successful_wakes=%u queued_timeouts=0\n",
                spin,records,arms.load(),wakes.load(),successful_wakes.load());
}

int main() {
    std::atomic<std::uint32_t> state{kAwake};
    assert(!ClaimWake(state));
    state.store(kArmed);
    assert(ClaimWake(state) && state.load() == kWakePending);
    for (unsigned i = 0; i < 1024; ++i) assert(!ClaimWake(state));
    assert(!NeedsAnnouncement(kAwake) && NeedsAnnouncement(kArmed) && !NeedsAnnouncement(kWakePending));
    CompleteWake(state,0);
    assert(state.load() == kArmed && ClaimWake(state));
    CompleteWake(state,1);
    assert(state.load() == kWakePending);
    CompleteWake(state,-1);
    assert(state.load() == kArmed);
    state.store(kAwake);
    CompleteWake(state,0);
    assert(state.load() == kAwake);
    state.store(kArmed);
    CompleteWake(state,0);
    assert(state.load() == kArmed);
    assert(ClaimWake(state));
    const auto correct = Explore(Mode::Coalesced);
    const auto no_wake = Explore(Mode::NeverWake);
    const auto old = Explore(Mode::OldDuplicateWake);
    const auto unsafe = Explore(Mode::UnsafePending);
    std::printf("MODEL: correct=%llu lost=%u duplicate=%u; no-wake lost=%u; old lost=%u duplicate=%u\n",
        static_cast<unsigned long long>(correct.states),correct.lost,correct.duplicate,no_wake.lost,old.lost,old.duplicate);
    std::fflush(stdout);
    assert(!correct.lost && !correct.duplicate);
    assert(no_wake.lost && !old.lost && old.duplicate && unsafe.lost);
    std::printf("PASS: exhaustive 3-publication/4-sleep-epoch model states=%llu; no lost/redundant-after-success wakes; three negative controls detected\n",
                static_cast<unsigned long long>(correct.states));
    for (unsigned spin : {0u,1u,16u}) Stress(spin,250000);
}
