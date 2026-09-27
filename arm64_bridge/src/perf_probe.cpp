#include "perf_probe.h"

#include <android/log.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr const char* kLogTag = "HornPerf";
constexpr uint64_t kSlowFrameNs = 50'000'000;
constexpr uint64_t kSlowEventNs = 8'000'000;
constexpr uint64_t kShaderEventNs = 1'000'000;
constexpr uint64_t kSummaryNs = 1'000'000'000;
constexpr size_t kMaxGuestPcSamples = 8192;
constexpr size_t kMaxLoggedHotPcs = 8;
constexpr size_t kProcessPcSlotCount = 4096;
constexpr size_t kMaxLoggedProcessHotPcs = 16;
constexpr size_t kCategoryCount =
    static_cast<size_t>(HornPerfCategory::Count);

constexpr std::array<const char*, kCategoryCount> kCategoryNames = {
    "swap", "draw", "shader_compile", "shader_link", "texture",
    "mipmap", "buffer", "gpu_sync", "read", "write", "file_sync",
    "decompress", "guest_alloc", "java",
};

struct AtomicStat {
    std::atomic<uint64_t> ns{0};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> max_ns{0};
};

struct PlainStat {
    uint64_t ns = 0;
    uint64_t calls = 0;
    uint64_t bytes = 0;
};

struct ThreadState {
    std::array<PlainStat, kCategoryCount> frame{};
    uint64_t frame_translated_instructions = 0;
    std::array<uint32_t, kMaxGuestPcSamples> frame_guest_pc_samples{};
    size_t frame_guest_pc_sample_count = 0;
    uint64_t frame_guest_pc_samples_dropped = 0;
    uint64_t last_frame_wall_ns = 0;
    uint64_t last_frame_cpu_ns = 0;
    uint64_t frame_sequence = 0;

    uint64_t summary_start_ns = 0;
    uint64_t summary_frame_ns = 0;
    uint64_t summary_cpu_ns = 0;
    uint64_t summary_frames = 0;
    uint64_t summary_slow_frames = 0;
    uint64_t summary_max_frame_ns = 0;
    uint64_t last_process_cpu_ns = 0;
    long last_minor_faults = 0;
    long last_major_faults = 0;
    long last_voluntary_switches = 0;
    long last_involuntary_switches = 0;
};

std::array<AtomicStat, kCategoryCount> g_stats;
std::atomic<uint64_t> g_translated_instructions{0};
std::array<std::atomic<uint64_t>, kProcessPcSlotCount> g_process_pc_slots;
std::atomic<uint64_t> g_process_pc_samples_dropped{0};
std::atomic<uint64_t> g_last_process_hot_pc_log_ns{0};
thread_local ThreadState g_thread_state;

uint64_t ClockNs(clockid_t clock) {
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

void UpdateMaximum(std::atomic<uint64_t>& destination, uint64_t value) {
    uint64_t current = destination.load(std::memory_order_relaxed);
    while (current < value &&
           !destination.compare_exchange_weak(
               current, value, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

double Milliseconds(uint64_t ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}

double Mebibytes(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void RecordProcessGuestSample(uint32_t pc) {
    static_assert((kProcessPcSlotCount & (kProcessPcSlotCount - 1)) == 0);
    if (pc == 0) return;
    size_t slot_index =
        (static_cast<uint64_t>(pc) * 2654435761u) &
        (kProcessPcSlotCount - 1);
    for (size_t probe = 0; probe < 16; ++probe) {
        std::atomic<uint64_t>& slot = g_process_pc_slots[slot_index];
        uint64_t current = slot.load(std::memory_order_relaxed);
        for (;;) {
            const uint32_t current_pc = static_cast<uint32_t>(current);
            if (current_pc != 0 && current_pc != pc) break;
            const uint64_t count = current_pc == pc ? current >> 32 : 0;
            const uint64_t next_count =
                std::min<uint64_t>(count + 1, 0xFFFFFFFFull);
            const uint64_t desired = (next_count << 32) | pc;
            if (slot.compare_exchange_weak(
                    current, desired, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
        }
        slot_index = (slot_index + 1) & (kProcessPcSlotCount - 1);
    }
    g_process_pc_samples_dropped.fetch_add(1, std::memory_order_relaxed);
}

void LogProcessHotPcs() {
    struct HotPc {
        uint32_t pc = 0;
        uint64_t count = 0;
    };
    std::vector<HotPc> samples;
    samples.reserve(kProcessPcSlotCount);
    for (std::atomic<uint64_t>& slot : g_process_pc_slots) {
        const uint64_t packed = slot.exchange(0, std::memory_order_relaxed);
        const uint32_t pc = static_cast<uint32_t>(packed);
        const uint64_t count = packed >> 32;
        if (pc && count) samples.push_back({pc, count});
    }
    if (samples.empty()) return;

    std::sort(samples.begin(), samples.end(), [](const HotPc& a, const HotPc& b) {
        return a.pc < b.pc;
    });
    size_t merged_count = 0;
    uint64_t total = 0;
    for (const HotPc& sample : samples) {
        total += sample.count;
        if (merged_count && samples[merged_count - 1].pc == sample.pc) {
            samples[merged_count - 1].count += sample.count;
        } else {
            samples[merged_count++] = sample;
        }
    }
    samples.resize(merged_count);
    std::sort(samples.begin(), samples.end(), [](const HotPc& a, const HotPc& b) {
        return a.count > b.count;
    });

    char details[1024] = {};
    size_t used = 0;
    const size_t limit = std::min(samples.size(), kMaxLoggedProcessHotPcs);
    for (size_t i = 0; i < limit && used < sizeof(details); ++i) {
        const int written = snprintf(
            details + used, sizeof(details) - used, " 0x%08X/%llu",
            samples[i].pc,
            static_cast<unsigned long long>(samples[i].count));
        if (written < 0) break;
        used = std::min(sizeof(details), used + static_cast<size_t>(written));
    }
    __android_log_print(
        ANDROID_LOG_INFO, kLogTag,
        "PERF_PROCESS_HOTPCS samples=%llu dropped=%llu |%s",
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(
            g_process_pc_samples_dropped.exchange(0, std::memory_order_relaxed)),
        details);
}

double CurrentRssMiB() {
    FILE* file = fopen("/proc/self/statm", "r");
    if (!file) return -1.0;
    unsigned long total_pages = 0;
    unsigned long resident_pages = 0;
    const int fields = fscanf(file, "%lu %lu", &total_pages, &resident_pages);
    fclose(file);
    if (fields != 2) return -1.0;
    return static_cast<double>(resident_pages) *
           static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

void SnapshotRusage(ThreadState& state) {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return;
    state.last_minor_faults = usage.ru_minflt;
    state.last_major_faults = usage.ru_majflt;
    state.last_voluntary_switches = usage.ru_nvcsw;
    state.last_involuntary_switches = usage.ru_nivcsw;
}

void LogSummary(ThreadState& state, uint64_t now_ns) {
    const uint64_t window_ns = now_ns - state.summary_start_ns;
    if (window_ns < kSummaryNs) return;

    std::array<PlainStat, kCategoryCount> interval{};
    std::array<uint64_t, kCategoryCount> maxima{};
    for (size_t i = 0; i < kCategoryCount; ++i) {
        interval[i].ns = g_stats[i].ns.exchange(0, std::memory_order_relaxed);
        interval[i].calls =
            g_stats[i].calls.exchange(0, std::memory_order_relaxed);
        interval[i].bytes =
            g_stats[i].bytes.exchange(0, std::memory_order_relaxed);
        maxima[i] = g_stats[i].max_ns.exchange(0, std::memory_order_relaxed);
    }

    rusage usage{};
    const bool have_usage = getrusage(RUSAGE_SELF, &usage) == 0;
    const long minor_faults = have_usage
        ? usage.ru_minflt - state.last_minor_faults : 0;
    const long major_faults = have_usage
        ? usage.ru_majflt - state.last_major_faults : 0;
    const long voluntary_switches = have_usage
        ? usage.ru_nvcsw - state.last_voluntary_switches : 0;
    const long involuntary_switches = have_usage
        ? usage.ru_nivcsw - state.last_involuntary_switches : 0;

    const double window_seconds = static_cast<double>(window_ns) / 1e9;
    const double fps = window_seconds > 0.0
        ? static_cast<double>(state.summary_frames) / window_seconds : 0.0;
    const double average_frame_ms = state.summary_frames != 0
        ? Milliseconds(state.summary_frame_ns) /
              static_cast<double>(state.summary_frames)
        : 0.0;
    const double cpu_percent = state.summary_frame_ns != 0
        ? 100.0 * static_cast<double>(state.summary_cpu_ns) /
              static_cast<double>(state.summary_frame_ns)
        : 0.0;
    const uint64_t process_cpu_now = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
    const uint64_t process_cpu_ns = state.last_process_cpu_ns != 0 &&
                                            process_cpu_now >= state.last_process_cpu_ns
        ? process_cpu_now - state.last_process_cpu_ns
        : 0;
    const double process_cpu_percent = window_ns != 0
        ? 100.0 * static_cast<double>(process_cpu_ns) /
              static_cast<double>(window_ns)
        : 0.0;

    char details[2300] = {};
    size_t used = 0;
    for (size_t i = 0; i < kCategoryCount && used < sizeof(details); ++i) {
        if (interval[i].calls == 0) continue;
        const int written = snprintf(
            details + used, sizeof(details) - used,
            " %s=%.2fms/%llu/max%.2f/%.2fMiB",
            kCategoryNames[i], Milliseconds(interval[i].ns),
            static_cast<unsigned long long>(interval[i].calls),
            Milliseconds(maxima[i]), Mebibytes(interval[i].bytes));
        if (written < 0) break;
        used = std::min(sizeof(details), used + static_cast<size_t>(written));
    }

    __android_log_print(
        ANDROID_LOG_INFO, kLogTag,
        "PERF_SUMMARY window=%.2fs frames=%llu fps=%.1f avg=%.2fms "
        "max=%.2fms slow=%llu render_cpu=%.1f%% process_cpu=%.1f%% rss=%.1fMiB "
        "faults=%ld/%ld ctxsw=%ld/%ld jit_translate=%llu |%s",
        window_seconds,
        static_cast<unsigned long long>(state.summary_frames), fps,
        average_frame_ms, Milliseconds(state.summary_max_frame_ns),
        static_cast<unsigned long long>(state.summary_slow_frames),
        cpu_percent, process_cpu_percent, CurrentRssMiB(), minor_faults, major_faults,
        voluntary_switches, involuntary_switches,
        static_cast<unsigned long long>(
            g_translated_instructions.exchange(0, std::memory_order_relaxed)),
        details);
    LogProcessHotPcs();

    state.summary_start_ns = now_ns;
    state.summary_frame_ns = 0;
    state.summary_cpu_ns = 0;
    state.summary_frames = 0;
    state.summary_slow_frames = 0;
    state.summary_max_frame_ns = 0;
    state.last_process_cpu_ns = process_cpu_now;
    if (have_usage) SnapshotRusage(state);
}

}  // namespace

bool HornPerfEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        const bool result =
            __system_property_get("debug.horn.perf_probe", value) > 0 &&
            value[0] != '\0' && strcmp(value, "0") != 0 &&
            strcmp(value, "false") != 0;
        if (result) {
            __android_log_print(
                ANDROID_LOG_INFO, kLogTag,
                "PERF_CONFIG enabled=1 slow_frame_ms=50 slow_event_ms=8");
        }
        return result;
    }();
    return enabled;
}

uint64_t HornPerfNowNs() {
    return ClockNs(CLOCK_MONOTONIC);
}

void HornPerfRecord(HornPerfCategory category, uint64_t elapsed_ns,
                    uint64_t bytes) {
    if (!HornPerfEnabled()) return;
    const size_t index = static_cast<size_t>(category);
    if (index >= kCategoryCount) return;

    AtomicStat& global = g_stats[index];
    global.ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
    global.calls.fetch_add(1, std::memory_order_relaxed);
    global.bytes.fetch_add(bytes, std::memory_order_relaxed);
    UpdateMaximum(global.max_ns, elapsed_ns);

    PlainStat& local = g_thread_state.frame[index];
    local.ns += elapsed_ns;
    ++local.calls;
    local.bytes += bytes;

    const uint64_t threshold =
        category == HornPerfCategory::ShaderCompile ||
                category == HornPerfCategory::ShaderLink
            ? kShaderEventNs
            : kSlowEventNs;
    if (elapsed_ns >= threshold) {
        __android_log_print(
            ANDROID_LOG_INFO, kLogTag,
            "PERF_EVENT type=%s wall=%.2fms bytes=%llu tid=%d",
            kCategoryNames[index], Milliseconds(elapsed_ns),
            static_cast<unsigned long long>(bytes), gettid());
    }
}

void HornPerfRecordTranslation(uint32_t) {
    if (!HornPerfEnabled()) return;
    ++g_thread_state.frame_translated_instructions;
    g_translated_instructions.fetch_add(1, std::memory_order_relaxed);
}

void HornPerfRecordGuestSample(uint32_t guest_pc) {
    if (!HornPerfEnabled()) return;
    ThreadState& state = g_thread_state;
    const uint32_t normalized_pc = guest_pc & ~1u;
    RecordProcessGuestSample(normalized_pc);
    const uint64_t now_ns = HornPerfNowNs();
    uint64_t last_ns =
        g_last_process_hot_pc_log_ns.load(std::memory_order_relaxed);
    if (now_ns - last_ns >= kSummaryNs &&
        g_last_process_hot_pc_log_ns.compare_exchange_strong(
            last_ns, now_ns, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "PERF_HEARTBEAT pc=0x%08X tid=%d",
                            normalized_pc, gettid());
        LogProcessHotPcs();
    }
    if (state.frame_guest_pc_sample_count < state.frame_guest_pc_samples.size()) {
        state.frame_guest_pc_samples[state.frame_guest_pc_sample_count++] =
            normalized_pc;
    } else {
        ++state.frame_guest_pc_samples_dropped;
    }
}

void HornPerfFrameBoundary() {
    if (!HornPerfEnabled()) return;

    ThreadState& state = g_thread_state;
    const uint64_t now_ns = HornPerfNowNs();
    const uint64_t cpu_ns = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    if (state.last_frame_wall_ns == 0 || state.last_frame_cpu_ns == 0) {
        state.last_frame_wall_ns = now_ns;
        state.last_frame_cpu_ns = cpu_ns;
        state.summary_start_ns = now_ns;
        state.last_process_cpu_ns = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
        state.frame.fill({});
        state.frame_translated_instructions = 0;
        state.frame_guest_pc_sample_count = 0;
        state.frame_guest_pc_samples_dropped = 0;
        for (AtomicStat& stat : g_stats) {
            stat.ns.store(0, std::memory_order_relaxed);
            stat.calls.store(0, std::memory_order_relaxed);
            stat.bytes.store(0, std::memory_order_relaxed);
            stat.max_ns.store(0, std::memory_order_relaxed);
        }
        SnapshotRusage(state);
        return;
    }

    const uint64_t frame_ns = now_ns - state.last_frame_wall_ns;
    const uint64_t frame_cpu_ns = cpu_ns - state.last_frame_cpu_ns;
    state.last_frame_wall_ns = now_ns;
    state.last_frame_cpu_ns = cpu_ns;
    ++state.frame_sequence;

    uint64_t classified_ns = 0;
    for (const PlainStat& stat : state.frame) classified_ns += stat.ns;
    const uint64_t unclassified_ns = frame_ns > classified_ns
        ? frame_ns - classified_ns : 0;
    const uint64_t wait_ns = frame_ns > frame_cpu_ns
        ? frame_ns - frame_cpu_ns : 0;

    const bool slow = frame_ns >= kSlowFrameNs;
    if (slow) {
        const auto& swap = state.frame[static_cast<size_t>(HornPerfCategory::Swap)];
        const auto& draw = state.frame[static_cast<size_t>(HornPerfCategory::Draw)];
        const auto& compile =
            state.frame[static_cast<size_t>(HornPerfCategory::ShaderCompile)];
        const auto& link =
            state.frame[static_cast<size_t>(HornPerfCategory::ShaderLink)];
        const auto& texture =
            state.frame[static_cast<size_t>(HornPerfCategory::TextureUpload)];
        const auto& mipmap =
            state.frame[static_cast<size_t>(HornPerfCategory::Mipmap)];
        const auto& buffer =
            state.frame[static_cast<size_t>(HornPerfCategory::BufferUpload)];
        const auto& sync =
            state.frame[static_cast<size_t>(HornPerfCategory::GpuSync)];
        const auto& read =
            state.frame[static_cast<size_t>(HornPerfCategory::FileRead)];
        const auto& write =
            state.frame[static_cast<size_t>(HornPerfCategory::FileWrite)];
        const auto& file_sync =
            state.frame[static_cast<size_t>(HornPerfCategory::FileSync)];
        const auto& decompress =
            state.frame[static_cast<size_t>(HornPerfCategory::Decompress)];
        const auto& guest_alloc =
            state.frame[static_cast<size_t>(HornPerfCategory::GuestAlloc)];
        const auto& java_call =
            state.frame[static_cast<size_t>(HornPerfCategory::JavaCall)];
        __android_log_print(
            ANDROID_LOG_INFO, kLogTag,
            "PERF_FRAME seq=%llu wall=%.2fms fps=%.1f cpu=%.2fms "
            "wait=%.2fms unknown=%.2fms | swap=%.2f draw=%.2f/%llu "
            "shader=%.2f+%.2f tex=%.2f mip=%.2f buf=%.2f sync=%.2f "
            "read=%.2f/%.2fMiB write=%.2f/%.2fMiB fsync=%.2f zlib=%.2f "
            "alloc=%.2f/%llu/%.2fMiB jit_translate=%llu java=%.2f",
            static_cast<unsigned long long>(state.frame_sequence),
            Milliseconds(frame_ns), 1e9 / static_cast<double>(frame_ns),
            Milliseconds(frame_cpu_ns), Milliseconds(wait_ns),
            Milliseconds(unclassified_ns), Milliseconds(swap.ns),
            Milliseconds(draw.ns),
            static_cast<unsigned long long>(draw.calls),
            Milliseconds(compile.ns), Milliseconds(link.ns),
            Milliseconds(texture.ns), Milliseconds(mipmap.ns),
            Milliseconds(buffer.ns), Milliseconds(sync.ns),
            Milliseconds(read.ns), Mebibytes(read.bytes),
            Milliseconds(write.ns), Mebibytes(write.bytes),
            Milliseconds(file_sync.ns), Milliseconds(decompress.ns),
            Milliseconds(guest_alloc.ns),
            static_cast<unsigned long long>(guest_alloc.calls),
            Mebibytes(guest_alloc.bytes),
            static_cast<unsigned long long>(
                state.frame_translated_instructions),
            Milliseconds(java_call.ns));

        if (state.frame_guest_pc_sample_count != 0) {
            auto first = state.frame_guest_pc_samples.begin();
            auto last = first + state.frame_guest_pc_sample_count;
            std::sort(first, last);

            struct HotPc {
                uint32_t pc = 0;
                uint32_t count = 0;
            };
            std::array<HotPc, kMaxLoggedHotPcs> top{};
            for (auto cursor = first; cursor != last;) {
                const uint32_t pc = *cursor;
                const auto run_end = std::upper_bound(cursor, last, pc);
                const uint32_t count = static_cast<uint32_t>(run_end - cursor);
                size_t position = 0;
                while (position < top.size() && top[position].count >= count) {
                    ++position;
                }
                if (position < top.size()) {
                    for (size_t i = top.size() - 1; i > position; --i) {
                        top[i] = top[i - 1];
                    }
                    top[position] = HotPc{pc, count};
                }
                cursor = run_end;
            }

            char hot_details[512] = {};
            size_t used = 0;
            for (const HotPc& hot : top) {
                if (hot.count == 0 || used >= sizeof(hot_details)) break;
                const int written = snprintf(
                    hot_details + used, sizeof(hot_details) - used,
                    " 0x%08X/%u", hot.pc, hot.count);
                if (written < 0) break;
                used = std::min(sizeof(hot_details),
                                used + static_cast<size_t>(written));
            }
            __android_log_print(
                ANDROID_LOG_INFO, kLogTag,
                "PERF_HOTPCS seq=%llu samples=%zu dropped=%llu |%s",
                static_cast<unsigned long long>(state.frame_sequence),
                state.frame_guest_pc_sample_count,
                static_cast<unsigned long long>(
                    state.frame_guest_pc_samples_dropped),
                hot_details);
        }
    }

    state.summary_frame_ns += frame_ns;
    state.summary_cpu_ns += frame_cpu_ns;
    ++state.summary_frames;
    if (slow) ++state.summary_slow_frames;
    state.summary_max_frame_ns =
        std::max(state.summary_max_frame_ns, frame_ns);
    state.frame.fill({});
    state.frame_translated_instructions = 0;
    state.frame_guest_pc_sample_count = 0;
    state.frame_guest_pc_samples_dropped = 0;

    LogSummary(state, now_ns);
}
