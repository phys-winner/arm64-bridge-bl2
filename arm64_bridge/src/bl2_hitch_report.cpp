#include "bl2_hitch_report.h"
#include "bl2_frame_pacer.h"

#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

pid_t Bl2PerfHintGameTid();

namespace {

constexpr const char* kTag = "BL2Hitch";
std::atomic<uint32_t> g_jit_blocks{0}, g_draws{0}, g_swaps{0}, g_pipelines{0}, g_textures{0};
std::atomic<uint64_t> g_pipeline_ns{0}, g_texture_bytes{0}, g_buffer_bytes{0};
std::atomic<uint32_t> g_limit_bits{0};

bool Enabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return !(__system_property_get("debug.bl2.hitch_log", value) > 0 && value[0] == '0');
    }();
    return enabled;
}

uint64_t NowNs(clockid_t clock) {
    timespec value{};
    if (clock_gettime(clock, &value) != 0) return 0;
    return static_cast<uint64_t>(value.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(value.tv_nsec);
}

// Linux per-thread CPU clock of another thread (bionic pthread_getcpuclockid).
clockid_t ThreadCpuClock(pid_t tid) {
    return static_cast<clockid_t>((~static_cast<unsigned>(tid) << 3) | 6u);
}

}  // namespace

void Bl2HitchCountJitBlock() { g_jit_blocks.fetch_add(1, std::memory_order_relaxed); }
void Bl2HitchAddPipelineNs(uint64_t ns) {
    g_pipelines.fetch_add(1, std::memory_order_relaxed);
    g_pipeline_ns.fetch_add(ns, std::memory_order_relaxed);
}
void Bl2HitchAddTextureUpload(uint64_t bytes) {
    g_textures.fetch_add(1, std::memory_order_relaxed);
    g_texture_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void Bl2HitchAddBufferUpload(uint64_t bytes) {
    g_buffer_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void Bl2HitchCountDraw() { g_draws.fetch_add(1, std::memory_order_relaxed); }
void Bl2HitchCountProgramSwap() { g_swaps.fetch_add(1, std::memory_order_relaxed); }

void Bl2HitchSetFrameLimit(float fps) {
    uint32_t bits = 0;
    std::memcpy(&bits, &fps, sizeof(bits));
    g_limit_bits.store(bits, std::memory_order_relaxed);
}

void Bl2HitchFrame() {
    if (!Enabled()) return;
    // State of the presenting thread; another presenter keeps its own.
    static thread_local uint64_t last_wall = 0, last_cpu = 0, last_game_cpu = 0;
    static thread_local pid_t last_game = 0;
    static thread_local uint64_t window_start = 0;
    static thread_local uint32_t frames = 0, hitches = 0;
    static thread_local double worst_ms = 0, sum_ms = 0;
    static thread_local uint64_t sum_jit = 0, sum_pipelines = 0, sum_pipeline_ns = 0,
                                 sum_texture_bytes = 0, sum_buffer_bytes = 0, sum_draws = 0;
    const uint64_t wall = NowNs(CLOCK_MONOTONIC);
    const uint64_t cpu = NowNs(CLOCK_THREAD_CPUTIME_ID);
    const pid_t game = Bl2PerfHintGameTid();
    const uint64_t game_cpu = game ? NowNs(ThreadCpuClock(game)) : 0;
    const uint32_t jit = g_jit_blocks.exchange(0, std::memory_order_relaxed);
    const uint32_t draws = g_draws.exchange(0, std::memory_order_relaxed);
    const uint32_t swaps = g_swaps.exchange(0, std::memory_order_relaxed);
    const uint32_t pipelines = g_pipelines.exchange(0, std::memory_order_relaxed);
    const uint64_t pipeline_ns = g_pipeline_ns.exchange(0, std::memory_order_relaxed);
    const uint32_t textures = g_textures.exchange(0, std::memory_order_relaxed);
    const uint64_t texture_bytes = g_texture_bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t buffer_bytes = g_buffer_bytes.exchange(0, std::memory_order_relaxed);
    if (last_wall && wall > last_wall) {
        const double frame_ms = (wall - last_wall) / 1e6;
        const double render_ms = (cpu - last_cpu) / 1e6;
        const double game_ms = game && game == last_game && last_game_cpu
            ? (game_cpu - last_game_cpu) / 1e6 : -1.0;
        float limit = 0.0f;
        const uint32_t bits = g_limit_bits.load(std::memory_order_relaxed);
        std::memcpy(&limit, &bits, sizeof(limit));
        const double budget_ms = std::isfinite(limit) && limit > 1.0f ? 1000.0 / limit : 16.667;
        if (frame_ms > std::max(2.0 * budget_ms, 40.0)) {
            ++hitches;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "frame=%.1fms budget=%.1fms render_cpu=%.1fms game_cpu=%.1fms jit_blocks=%u "
                "pipelines=%u/%.2fms textures=%u/%.1fKiB buffers=%.1fKiB draws=%u swaps=%u",
                frame_ms, budget_ms, render_ms, game_ms, jit, pipelines, pipeline_ns / 1e6,
                textures, texture_bytes / 1024.0, buffer_bytes / 1024.0, draws, swaps);
        }
        ++frames;
        sum_ms += frame_ms;
        worst_ms = std::max(worst_ms, frame_ms);
        sum_jit += jit;
        sum_pipelines += pipelines;
        sum_pipeline_ns += pipeline_ns;
        sum_texture_bytes += texture_bytes;
        sum_buffer_bytes += buffer_bytes;
        sum_draws += draws;
        if (!window_start) window_start = wall;
        if (wall - window_start >= 10'000'000'000ull && frames) {
            // pace=0 while presents are unpaced; late = frames shown after
            // their paced refresh.
            const Bl2FramePacerStats pace = Bl2FramePacerTakeStats();
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "summary frames=%u avg=%.2fms worst=%.1fms hitches=%u jit_blocks=%llu "
                "pipelines=%llu/%.1fms textures=%.1fMiB buffers=%.1fMiB draws/frame=%.0f "
                "pace=%.1fms depth=%.1fms late=%u",
                frames, sum_ms / frames, worst_ms, hitches,
                static_cast<unsigned long long>(sum_jit),
                static_cast<unsigned long long>(sum_pipelines), sum_pipeline_ns / 1e6,
                sum_texture_bytes / 1048576.0, sum_buffer_bytes / 1048576.0,
                static_cast<double>(sum_draws) / frames, pace.interval_ns / 1e6,
                pace.depth_ns / 1e6, pace.late);
            window_start = wall;
            frames = hitches = 0;
            worst_ms = sum_ms = 0;
            sum_jit = sum_pipelines = sum_pipeline_ns = 0;
            sum_texture_bytes = sum_buffer_bytes = sum_draws = 0;
        }
    }
    last_wall = wall;
    last_cpu = cpu;
    last_game = game;
    last_game_cpu = game_cpu;
}
