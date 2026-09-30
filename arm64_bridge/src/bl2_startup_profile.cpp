#include "bl2_startup_profile.h"

#include <dynarmic/interface/A32/a32.h>

#include "bridge_log.h"
#include <sys/prctl.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr const char* kTag = "BL2_StartupProfile";
constexpr uint64_t kWindowNs = 5'000'000'000ull;
constexpr uint64_t kLimitNs = 300'000'000'000ull;
constexpr size_t kRows = 256;
constexpr size_t kLoggedRows = 12;
constexpr size_t kIds = 4096;
constexpr size_t kThreads = 64;
std::atomic<uint64_t> g_start_ns{0};
std::atomic<bool> g_stopped{false};
std::atomic<uint64_t> g_last_activity_ns{0};

struct ActiveSlot {
    std::atomic<uint32_t> tid{0};
    // A single thread writes its slot. The sequence protects a reader from
    // combining the ID/LR of one call with the serial of the adjacent call.
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> id_lr{0};
    std::atomic<uint64_t> call{0};
};
struct NameSlot {
    std::atomic<uint8_t> ready{0};
    char name[64]{};
};
std::array<ActiveSlot, kThreads> g_active;
std::array<NameSlot, kIds> g_names;
std::atomic<size_t> g_active_count{0};

void RememberName(uint32_t id, const char* name) {
    if (id >= kIds) return;
    NameSlot& slot = g_names[id];
    uint8_t expected = 0;
    if (slot.ready.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
        std::snprintf(slot.name, sizeof(slot.name), "%s", name);
        slot.ready.store(2, std::memory_order_release);
    }
}

const char* NameForId(uint32_t id) {
    if (!id) return "guest_or_between_thunks";
    if (id < kIds && g_names[id].ready.load(std::memory_order_acquire) == 2) {
        return g_names[id].name;
    }
    return "unknown_id";
}

void PublishActivity(ActiveSlot& slot, uint64_t id_lr, uint64_t call) {
    slot.sequence.fetch_add(1, std::memory_order_acq_rel);
    slot.id_lr.store(id_lr, std::memory_order_relaxed);
    slot.call.store(call, std::memory_order_relaxed);
    slot.sequence.fetch_add(1, std::memory_order_release);
}

uint64_t ClockNs(clockid_t clock) {
    timespec value{};
    if (::clock_gettime(clock, &value) != 0) return 0;
    return static_cast<uint64_t>(value.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(value.tv_nsec);
}

bool Enabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        return __system_property_get("debug.bl2.startup_profile", value) > 0 &&
               std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// 0 means not classified, 1 ignored, 2 timed, 3 ends the startup interval.
uint8_t Classify(const char* name) {
    if (std::strcmp(name, "eglSwapBuffers") == 0) return 3;
    constexpr const char* names[] = {
        "sleep", "usleep", "sched_yield", "syscall",
        "pthread_cond_wait", "pthread_cond_timedwait",
        "pthread_cond_timedwait_relative_np", "pthread_mutex_lock",
        "pthread_once", "pthread_join", "sem_wait", "sem_timedwait",
        "fopen", "fopen64", "open", "open64", "read", "pread", "pread64",
        "fread", "fgetc", "fgets", "fseek", "fseeko", "fseeko64",
        "lseek", "lseek64", "mmap", "mmap2", "mmap64", "munmap",
        "inflateInit_", "inflateInit2_", "inflate", "inflateReset",
        "inflateEnd", "uncompress", "crc32"
    };
    for (const char* candidate : names) {
        if (std::strcmp(name, candidate) == 0) return 2;
    }
    return 1;
}

struct Row {
    uint32_t id = 0;
    uint32_t lr = 0;
    char name[48]{};
    uint64_t calls = 0;
    uint64_t wall_ns = 0;
    uint64_t cpu_ns = 0;
    uint64_t max_ns = 0;
    uint64_t arg0_sum = 0;
    uint64_t arg0_zero = 0;
    uint32_t arg0_min = std::numeric_limits<uint32_t>::max();
    uint32_t arg0_max = 0;
    uint32_t arg2_last = 0;
    uint32_t result_last = 0;
};

struct ThreadProfile {
    std::array<uint8_t, kIds> classes{};
    std::array<Row, kRows> rows{};
    uint64_t window_wall = 0;
    uint64_t window_cpu = 0;
    uint64_t selected_wall = 0;
    uint64_t selected_cpu = 0;
    uint64_t calls = 0;
    uint64_t dropped = 0;
    unsigned selected_depth = 0;
    unsigned sequence = 0;
    bool finished = false;
    ActiveSlot* active = nullptr;
    bool active_registered = false;
    uint64_t activity_serial = 0;
};
thread_local ThreadProfile g_thread;

double Ms(uint64_t ns) { return static_cast<double>(ns) / 1'000'000.0; }

uint64_t Since(uint64_t now, uint64_t start) {
    // Another thread can publish the global start after this thread sampled
    // its clock. Never let that harmless ordering wrap into a 300 s timeout.
    return now >= start ? now - start : 0;
}

void LogActivity(uint64_t wall) {
    uint64_t previous = g_last_activity_ns.load(std::memory_order_relaxed);
    if (Since(wall, previous) < kWindowNs ||
        !g_last_activity_ns.compare_exchange_strong(previous, wall)) return;
    const size_t total = g_active_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < std::min(total, kThreads); ++i) {
        const ActiveSlot& slot = g_active[i];
        const uint32_t tid = slot.tid.load(std::memory_order_acquire);
        if (!tid) continue;
        const uint64_t before = slot.sequence.load(std::memory_order_acquire);
        if (before & 1) continue;
        const uint64_t id_lr = slot.id_lr.load(std::memory_order_relaxed);
        const uint64_t call = slot.call.load(std::memory_order_relaxed);
        if (before != slot.sequence.load(std::memory_order_acquire)) continue;
        const uint32_t id = static_cast<uint32_t>(id_lr >> 32);
        HORN_LOGI(kTag,
            "STARTUP_ACTIVE tid=%u elapsed_ms=%.3f id=%u name=%s lr=0x%08X "
            "call=%llu observed_by=%d registry_overflow=%zu",
            tid, Ms(Since(wall, g_start_ns.load(std::memory_order_relaxed))),
            id, NameForId(id), static_cast<uint32_t>(id_lr),
            static_cast<unsigned long long>(call), gettid(),
            total > kThreads ? total - kThreads : 0);
    }
}

void Flush(ThreadProfile& state, uint64_t wall, uint64_t cpu,
           const char* reason) {
    if (!state.window_wall || state.finished) return;
    LogActivity(wall);
    const uint64_t wall_delta = wall - state.window_wall;
    const uint64_t cpu_delta = cpu - state.window_cpu;
    const uint64_t residual_wall = wall_delta > state.selected_wall
        ? wall_delta - state.selected_wall : 0;
    const uint64_t residual_cpu = cpu_delta > state.selected_cpu
        ? cpu_delta - state.selected_cpu : 0;
    HORN_LOGI(kTag,
        "STARTUP_WINDOW tid=%d seq=%u reason=%s elapsed_ms=%.3f "
        "wall_ms=%.3f cpu_ms=%.3f selected_wall_ms=%.3f selected_cpu_ms=%.3f "
        "residual_wall_ms=%.3f residual_cpu_ms=%.3f calls=%llu dropped=%llu",
        gettid(), ++state.sequence, reason,
        Ms(Since(wall, g_start_ns.load(std::memory_order_relaxed))),
        Ms(wall_delta), Ms(cpu_delta), Ms(state.selected_wall),
        Ms(state.selected_cpu), Ms(residual_wall), Ms(residual_cpu),
        static_cast<unsigned long long>(state.calls),
        static_cast<unsigned long long>(state.dropped));

    std::array<const Row*, kRows> order{};
    size_t count = 0;
    for (const Row& row : state.rows) {
        if (row.calls) order[count++] = &row;
    }
    std::sort(order.begin(), order.begin() + count,
        [](const Row* a, const Row* b) { return a->wall_ns > b->wall_ns; });
    uint64_t logged_wall = 0;
    for (size_t i = 0; i < std::min(count, kLoggedRows); ++i) {
        const Row& row = *order[i];
        logged_wall += row.wall_ns;
        HORN_LOGI(kTag,
            "STARTUP_THUNK tid=%d seq=%u id=%u name=%s lr=0x%08X "
            "calls=%llu wall_ms=%.3f cpu_ms=%.3f max_ms=%.3f "
            "a0_min=%u a0_max=%u a0_sum=%llu a0_zero=%llu "
            "a2_last=0x%08X result_last=0x%08X",
            gettid(), state.sequence, row.id, row.name, row.lr,
            static_cast<unsigned long long>(row.calls), Ms(row.wall_ns),
            Ms(row.cpu_ns), Ms(row.max_ns), row.arg0_min, row.arg0_max,
            static_cast<unsigned long long>(row.arg0_sum),
            static_cast<unsigned long long>(row.arg0_zero), row.arg2_last,
            row.result_last);
    }
    HORN_LOGI(kTag,
        "STARTUP_COVERAGE tid=%d seq=%u rows=%zu logged=%zu "
        "logged_wall_ms=%.3f selected_wall_ms=%.3f",
        gettid(), state.sequence, count, std::min(count, kLoggedRows),
        Ms(logged_wall), Ms(state.selected_wall));
    state.rows = {};
    state.calls = state.dropped = state.selected_wall = state.selected_cpu = 0;
    // Logging and sorting time are outside the following window.
    state.window_wall = ClockNs(CLOCK_MONOTONIC);
    state.window_cpu = ClockNs(CLOCK_THREAD_CPUTIME_ID);
}

} // namespace

Bl2StartupThunkScope::Bl2StartupThunkScope(uint32_t id, const char* name,
                                          Dynarmic::A32::Jit& jit) {
    if (!Enabled()) return;
    ThreadProfile& state = g_thread;
    if (state.finished) return;
    if (g_stopped.load(std::memory_order_relaxed)) {
        Flush(state, ClockNs(CLOCK_MONOTONIC), ClockNs(CLOCK_THREAD_CPUTIME_ID), "stopped");
        state.finished = true;
        return;
    }
    uint8_t kind = id < kIds ? state.classes[id] : 0;
    if (!kind) {
        RememberName(id, name);
        kind = Classify(name);
        if (id < kIds) state.classes[id] = kind;
    }
    if (!state.active_registered) {
        const size_t slot = g_active_count.fetch_add(1, std::memory_order_acq_rel);
        if (slot < kThreads) {
            state.active = &g_active[slot];
            state.active->tid.store(static_cast<uint32_t>(gettid()), std::memory_order_release);
        }
        state.active_registered = true;
    }
    if (state.active) {
        activity_slot_ = state.active;
        previous_activity_ = state.active->id_lr.load(std::memory_order_relaxed);
        previous_activity_call_ = state.active->call.load(std::memory_order_relaxed);
        PublishActivity(*state.active, (static_cast<uint64_t>(id) << 32) | jit.Regs()[14],
                        ++state.activity_serial);
    }
    // All call identities are visible to another thread's bounded watchdog,
    // but ignored calls still execute no clock reads and collect no timings.
    if (kind == 1) return;
    // Nested guest callbacks/named forwarding belong to the outer thunk.
    if (state.selected_depth) return;
    const uint64_t now = ClockNs(CLOCK_MONOTONIC);
    const uint64_t cpu = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    uint64_t start = g_start_ns.load(std::memory_order_relaxed);
    if (!start && g_start_ns.compare_exchange_strong(start, now)) {
        start = now;
        HORN_LOGI(kTag,
            "STARTUP_PROFILE enabled=1 limit_s=300 interval_s=5 "
            "stop=first_swap_call timing=selected_thunks_only");
    }
    if (!start) start = g_start_ns.load(std::memory_order_relaxed);
    const bool timeout = Since(now, start) >= kLimitNs;
    if (kind == 3 || timeout || g_stopped.load(std::memory_order_relaxed)) {
        const char* reason = kind == 3 ? "first_swap_call" : timeout ? "limit" : "stopped";
        g_stopped.store(true, std::memory_order_relaxed);
        Flush(state, now, cpu, reason);
        state.finished = true;
        return;
    }
    if (!state.window_wall) {
        state.window_wall = now;
        state.window_cpu = cpu;
        HORN_LOGI(kTag,
            "STARTUP_THREAD tid=%d timer_slack_ns=%d first_id=%u first_name=%s",
            gettid(), ::prctl(PR_GET_TIMERSLACK), id, name);
    }
    ++state.selected_depth;
    jit_ = &jit;
    std::snprintf(name_, sizeof(name_), "%s", name);
    id_ = id;
    lr_ = jit.Regs()[14];
    arg0_ = jit.Regs()[0];
    arg2_ = jit.Regs()[2];
    cpu_start_ = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    wall_start_ = ClockNs(CLOCK_MONOTONIC);
}

Bl2StartupThunkScope::~Bl2StartupThunkScope() {
    if (activity_slot_) {
        PublishActivity(*static_cast<ActiveSlot*>(activity_slot_),
                        previous_activity_, previous_activity_call_);
    }
    if (!jit_) return;
    const uint64_t wall = ClockNs(CLOCK_MONOTONIC);
    const uint64_t cpu = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    const uint64_t elapsed = wall - wall_start_;
    const uint64_t cpu_elapsed = cpu - cpu_start_;
    ThreadProfile& state = g_thread;
    --state.selected_depth;
    ++state.calls;
    state.selected_wall += elapsed;
    state.selected_cpu += cpu_elapsed;

    const size_t slot = (static_cast<uint64_t>(id_) * 2654435761u + lr_) % kRows;
    Row* selected = nullptr;
    for (size_t step = 0; step < kRows; ++step) {
        Row& row = state.rows[(slot + step) % kRows];
        if (!row.id || (row.id == id_ && row.lr == lr_)) {
            selected = &row;
            break;
        }
    }
    if (selected) {
        Row& row = *selected;
        if (!row.id) {
            row.id = id_;
            row.lr = lr_;
            std::snprintf(row.name, sizeof(row.name), "%s", name_);
        }
        ++row.calls;
        row.wall_ns += elapsed;
        row.cpu_ns += cpu_elapsed;
        row.max_ns = std::max(row.max_ns, elapsed);
        row.arg0_min = std::min(row.arg0_min, arg0_);
        row.arg0_max = std::max(row.arg0_max, arg0_);
        row.arg0_sum += arg0_;
        row.arg0_zero += arg0_ == 0;
        row.arg2_last = arg2_;
        row.result_last = jit_->Regs()[0];
    } else {
        ++state.dropped;
    }

    const bool timeout = Since(wall, g_start_ns.load(std::memory_order_relaxed)) >= kLimitNs;
    const bool stopped = g_stopped.load(std::memory_order_relaxed);
    if (timeout || stopped || wall - state.window_wall >= kWindowNs) {
        Flush(state, wall, cpu, timeout ? "limit" : stopped ? "stopped" : "interval");
        if (timeout || stopped) {
            g_stopped.store(true, std::memory_order_relaxed);
            state.finished = true;
        }
    }
}
