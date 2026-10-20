#include "bl2_perf_hint.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace {

// The render thread runs the guest RHI and limits the frame; the game thread
// is the other per-frame critical path. The scheduler spreads them over mid
// cores at low clocks. Reporting the larger per-frame CPU work against a
// target below the frame budget lets the power HAL raise their clamp before a
// heavy (combat) frame misses its deadline.
constexpr double kTargetFraction = 0.6;
constexpr int64_t kMinTargetNs = 2'000'000;
constexpr int64_t kMaxTargetNs = 100'000'000;

using GetManagerFn = void* (*)();
using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
using UpdateTargetFn = int (*)(void*, int64_t);
using ReportActualFn = int (*)(void*, int64_t);
using SetThreadsFn = int (*)(void*, const int32_t*, size_t);

struct HintApi {
    GetManagerFn get_manager = nullptr;
    CreateSessionFn create_session = nullptr;
    UpdateTargetFn update_target = nullptr;
    ReportActualFn report_actual = nullptr;
    SetThreadsFn set_threads = nullptr;  // API 34; optional.
};

std::atomic<pid_t> g_game_tid{0};
std::atomic<pid_t> g_producer_tid{0};

// APerformanceHint is API 33; this library targets API 24, so resolve lazily.
const HintApi* LoadHintApi() {
    static const HintApi api = [] {
        HintApi result;
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.bl2.perf_hint", value) > 0 && value[0] == '0')
            return result;
        void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return result;
        result.get_manager = reinterpret_cast<GetManagerFn>(
            dlsym(library, "APerformanceHint_getManager"));
        result.create_session = reinterpret_cast<CreateSessionFn>(
            dlsym(library, "APerformanceHint_createSession"));
        result.update_target = reinterpret_cast<UpdateTargetFn>(
            dlsym(library, "APerformanceHint_updateTargetWorkDuration"));
        result.report_actual = reinterpret_cast<ReportActualFn>(
            dlsym(library, "APerformanceHint_reportActualWorkDuration"));
        result.set_threads = reinterpret_cast<SetThreadsFn>(
            dlsym(library, "APerformanceHint_setThreads"));
        return result;
    }();
    return api.get_manager && api.create_session && api.update_target &&
            api.report_actual ? &api : nullptr;
}

int64_t CpuNs(clockid_t clock) {
    timespec now{};
    if (clock_gettime(clock, &now) != 0) return -1;
    return static_cast<int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

// Linux per-thread scheduler clock of another thread in this process; the
// same encoding bionic's pthread_getcpuclockid uses.
clockid_t ThreadCpuClock(pid_t tid) {
    return static_cast<clockid_t>((~static_cast<unsigned>(tid) << 3) | 6u);
}

int64_t TargetFor(float frame_limit_fps) {
    if (!std::isfinite(frame_limit_fps) || frame_limit_fps <= 0.0f) return 0;
    const double budget = 1e9 / static_cast<double>(frame_limit_fps);
    return std::clamp(static_cast<int64_t>(budget * kTargetFraction),
                      kMinTargetNs, kMaxTargetNs);
}

}  // namespace

pid_t Bl2PerfHintGameTid() { return g_game_tid.load(std::memory_order_relaxed); }

void Bl2PerfHintGameThread() {
    if (g_game_tid.load(std::memory_order_relaxed)) return;
    pid_t expected = 0;
    g_game_tid.compare_exchange_strong(expected, gettid(), std::memory_order_relaxed);
}

// The guest rendering thread when its GL calls run on another host thread
// (bl2_gl_worker.cpp); that thread then presents.
void Bl2PerfHintSetProducer(pid_t tid) { g_producer_tid.store(tid, std::memory_order_relaxed); }

void Bl2PerfHintFrame(float frame_limit_fps) {
    // The caller serializes presents (host mutex); the first presenting
    // thread owns this state and other presenters return early.
    static pid_t owner = 0;
    static void* session = nullptr;
    static bool disabled = false;
    static int64_t target = 0;
    static int64_t last_cpu = -1;
    static pid_t game = 0, producer = 0;
    static int64_t last_game_cpu = -1, last_producer_cpu = -1;
    if (disabled) return;
    const pid_t tid = gettid();
    if (owner && owner != tid) return;
    const int64_t wanted = TargetFor(frame_limit_fps);
    const int64_t cpu = CpuNs(CLOCK_THREAD_CPUTIME_ID);
    if (!wanted || cpu < 0) return;
    const HintApi* api = LoadHintApi();
    const pid_t known_game = g_game_tid.load(std::memory_order_relaxed);
    const pid_t known_producer = g_producer_tid.load(std::memory_order_relaxed);
    const bool with_game = known_game && known_game != tid;
    const bool with_producer = known_producer && known_producer != tid && known_producer != known_game;
    int32_t threads[3] = {static_cast<int32_t>(tid)};
    size_t thread_count = 1;
    if (with_game) threads[thread_count++] = static_cast<int32_t>(known_game);
    if (with_producer) threads[thread_count++] = static_cast<int32_t>(known_producer);
    const auto adopt = [&] {
        game = with_game ? known_game : 0;
        producer = with_producer ? known_producer : 0;
        last_game_cpu = game ? CpuNs(ThreadCpuClock(game)) : -1;
        last_producer_cpu = producer ? CpuNs(ThreadCpuClock(producer)) : -1;
    };
    if (!session) {
        void* manager = api ? api->get_manager() : nullptr;
        session = manager ? api->create_session(manager, threads, thread_count, wanted) : nullptr;
        if (!session) {
            disabled = true;
            return;
        }
        owner = tid;
        target = wanted;
        last_cpu = cpu;
        adopt();
        __android_log_print(ANDROID_LOG_INFO, "BL2_Parity",
                            "ADPF session render=%d game=%d producer=%d target=%.2fms limit=%.1fFPS",
                            tid, game, producer, wanted / 1e6, frame_limit_fps);
        return;
    }
    if (((!game && with_game) || producer != (with_producer ? known_producer : 0)) && api->set_threads) {
        if (api->set_threads(session, threads, thread_count) == 0) {
            adopt();
            __android_log_print(ANDROID_LOG_INFO, "BL2_Parity",
                                "ADPF session threads game=%d producer=%d", game, producer);
        }
    }
    if (wanted != target && api->update_target(session, wanted) == 0) target = wanted;
    int64_t actual = cpu - last_cpu;
    last_cpu = cpu;
    if (game) {
        const int64_t game_cpu = CpuNs(ThreadCpuClock(game));
        if (game_cpu >= 0 && last_game_cpu >= 0) actual = std::max(actual, game_cpu - last_game_cpu);
        last_game_cpu = game_cpu;
    }
    if (producer) {
        const int64_t producer_cpu = CpuNs(ThreadCpuClock(producer));
        if (producer_cpu >= 0 && last_producer_cpu >= 0) actual = std::max(actual, producer_cpu - last_producer_cpu);
        last_producer_cpu = producer_cpu;
    }
    api->report_actual(session, std::clamp<int64_t>(actual, 1'000, 1'000'000'000));
}
