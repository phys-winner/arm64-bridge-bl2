#include "bl2_frame_pacer.h"

#include <EGL/eglext.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

constexpr const char* kTag = "BL2Hitch";
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kSecond = 1'000'000'000;
// Refresh periods accepted from SurfaceFlinger (20..500 Hz).
constexpr int64_t kMinPeriodNs = kSecond / 500;
constexpr int64_t kMaxPeriodNs = kSecond / 20;
// Slowest pace (Android's frame pacing library defaults to the same 50 ms):
// a hot device can fall short of 30 FPS, and an even 24 FPS still moves
// smoother than frames alternating between 4 and 5 refreshes. Heavier frames
// are shown when ready.
constexpr double kMinPacedFps = 20.0;
// Lead the GPU leaves SurfaceFlinger before the target refresh. Raised when a
// frame whose GPU work finished before its refresh was still shown late.
constexpr int64_t kMinPresentMarginNs = 2 * kMs;
// Render-thread and GPU time must fit an interval with this much slack.
constexpr int64_t kFitMarginNs = kMs;
constexpr size_t kSampleWindow = 64;
constexpr size_t kLateWindow = 24;
// Late frames among the last kLateWindow that slow the pace down; fewer while
// a tighter pace or depth is on trial.
constexpr uint32_t kLateLimit = 3;
constexpr uint32_t kTrialLateLimit = 2;
constexpr size_t kMinSamples = 8;
// On-time frames before a faster pace or a shorter depth is tried. Doubles
// whenever such a change has to be undone within kTrialNs: a GPU that is
// heating up holds a faster pace for several seconds before it falls behind.
// The first kTrialFrames frames of a trial roll back on fewer late frames.
constexpr uint32_t kTightenWaitMin = 90;
constexpr uint32_t kTightenWaitMax = 2400;
constexpr uint64_t kTrialFrames = 300;
constexpr int64_t kTrialNs = 30 * kSecond;
// GPU work also runs before the swap, so the GPU time after it fitting a faster
// pace is no proof; and GPU clocks follow the load, so a faster pace that even
// that time does not fit may still be reachable. Both are tried, less often.
constexpr uint32_t kGpuProbeWait = 180;
constexpr uint32_t kGpuBlindProbeWait = 360;
// Frames after pacing starts or resumes (also after a pause in presenting) that
// can still queue behind GPU work submitted without it (the BufferQueue holds up
// to three) or meet cold clocks: their timings are no evidence about the pace,
// so they are neither sampled nor counted late.
constexpr uint32_t kSettleFrames = 3;
constexpr int64_t kGapResetNs = 250 * kMs;
constexpr int64_t kStaleFrameNs = 500 * kMs;
// NATIVE_WINDOW_TIMESTAMP_AUTO: the queue time, as before any pacing.
constexpr EGLnsecsANDROID kAutoTimestamp = INT64_MIN;

struct Egl {
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentation_time = nullptr;
    PFNEGLGETNEXTFRAMEIDANDROIDPROC next_frame_id = nullptr;
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC frame_timestamps = nullptr;
    PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC frame_timestamp_supported = nullptr;
    PFNEGLGETCOMPOSITORTIMINGANDROIDPROC compositor_timing = nullptr;
    PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC compositor_timing_supported = nullptr;
};

struct Frame {
    EGLuint64KHR id = 0;
    int64_t start = 0;  // Render-thread frame start, 0 if unknown.
    int64_t swap = 0;
    int64_t target = 0;
    uint32_t config = 0;
    bool reanchored = false;  // Its cadence slot was already out of reach.
    bool settling = false;    // One of the first kSettleFrames of a pacing run.
};

struct Sample {
    int64_t render = -1;  // Frame start to swap.
    int64_t gpu = -1;     // Swap to GPU completion.
    int64_t total = -1;   // Frame start to GPU completion.
    // Frame start or the previous frame's GPU completion, whichever is later,
    // to this frame's GPU completion: an upper bound of its GPU work.
    int64_t span = -1;
    bool late = false;
};

struct State {
    std::mutex mutex;
    float display_hz = 0.0f;
    uint32_t limit_fps = 0;
    bool allowed = false;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    bool usable = false;
    bool compositor_timing = false;
    bool timestamp_set = false;
    bool paced = false;

    int64_t period = 0;
    int64_t grid = 0;  // Any refresh on the present grid.
    bool grid_from_present = false;

    // Pace in refreshes: one frame per `interval`, each started `depth` before
    // its present.
    uint32_t interval = 0, depth = 0, n_min = 1, n_max = 1, config = 0;
    int64_t present_margin = kMinPresentMarginNs;

    int64_t last_target = 0, last_swap = 0, frame_start = 0, last_gpu_done = 0;
    Frame inflight{};
    bool inflight_valid = false;
    std::array<Frame, 8> pending{};
    size_t pending_count = 0;

    std::array<Sample, kSampleWindow> samples{};
    size_t sample_count = 0;  // Since the last configuration change.
    uint32_t on_time = 0;     // Consecutive frames shown at their refresh.
    uint32_t settle = 0;      // Settling frames still to submit.
    // The GPU work after the swap alone outlasts the render thread's frame:
    // more CPU clock cannot raise the pace, only heat the GPU's thermal budget.
    bool gpu_bound = false;
    uint64_t frames = 0, tightened_at = 0;
    int64_t tightened_at_ns = 0;
    bool tighten_trial = false;
    uint32_t tighten_wait = kTightenWaitMin;

    uint32_t late_total = 0, changes = 0;
};

// Never destroyed: guest threads can still present while the process exits.
State& PacerState() {
    static State* state = new State();
    return *state;
}

std::atomic<bool> g_active{false};
Egl g_egl;  // Written under the state mutex, before the first paced frame.

bool Enabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return !(__system_property_get("debug.bl2.frame_pacing", value) > 0 && value[0] == '0');
    }();
    return enabled;
}

bool LogEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return !(__system_property_get("debug.bl2.hitch_log", value) > 0 && value[0] == '0');
    }();
    return enabled;
}

int64_t Now() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<int64_t>(value.tv_sec) * kSecond + value.tv_nsec;
}

int64_t FloorDiv(int64_t value, int64_t divisor) {
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}

// The first refresh at or after `time`.
int64_t SnapUp(const State& s, int64_t time) {
    return s.grid + (FloorDiv(time - s.grid - 1, s.period) + 1) * s.period;
}

int64_t SnapNearest(const State& s, int64_t time) {
    return s.grid + FloorDiv(time - s.grid + s.period / 2, s.period) * s.period;
}

bool HasExtension(const char* list, const char* name) {
    if (!list) return false;
    const size_t length = std::strlen(name);
    for (const char* at = list; (at = std::strstr(at, name)) != nullptr; at += length) {
        if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == '\0'))
            return true;
    }
    return false;
}

template <typename T>
T Proc(const char* name) {
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

bool LoadEgl(EGLDisplay display) {
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!HasExtension(extensions, "EGL_ANDROID_presentation_time") ||
        !HasExtension(extensions, "EGL_ANDROID_get_frame_timestamps")) return false;
    if (!g_egl.presentation_time) {
        g_egl.presentation_time =
            Proc<PFNEGLPRESENTATIONTIMEANDROIDPROC>("eglPresentationTimeANDROID");
        g_egl.next_frame_id = Proc<PFNEGLGETNEXTFRAMEIDANDROIDPROC>("eglGetNextFrameIdANDROID");
        g_egl.frame_timestamps =
            Proc<PFNEGLGETFRAMETIMESTAMPSANDROIDPROC>("eglGetFrameTimestampsANDROID");
        g_egl.frame_timestamp_supported = Proc<PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC>(
            "eglGetFrameTimestampSupportedANDROID");
        g_egl.compositor_timing =
            Proc<PFNEGLGETCOMPOSITORTIMINGANDROIDPROC>("eglGetCompositorTimingANDROID");
        g_egl.compositor_timing_supported = Proc<PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC>(
            "eglGetCompositorTimingSupportedANDROID");
    }
    return g_egl.presentation_time && g_egl.next_frame_id && g_egl.frame_timestamps &&
           g_egl.frame_timestamp_supported && g_egl.compositor_timing &&
           g_egl.compositor_timing_supported;
}

// p-th percentile of a field over the newest `count` samples, -1 if none.
int64_t Percentile(const State& s, size_t count, int64_t Sample::*field, size_t percent) {
    std::array<int64_t, kSampleWindow> values{};
    size_t used = 0;
    const size_t available = std::min({count, s.sample_count, kSampleWindow});
    for (size_t i = 0; i < available; ++i) {
        const int64_t value = s.samples[(s.sample_count - 1 - i) % kSampleWindow].*field;
        if (value >= 0) values[used++] = value;
    }
    if (!used) return -1;
    const size_t index = (used - 1) * percent / 100;
    std::nth_element(values.begin(), values.begin() + index, values.begin() + used);
    return values[index];
}

// Refreshes from frame start to present that a frame of `total` work needs.
uint32_t DepthFor(const State& s, int64_t total) {
    if (total < 0) return s.depth;
    return static_cast<uint32_t>((total + s.present_margin + s.period - 1) / s.period);
}

void Apply(State& s, uint32_t interval, uint32_t depth, const char* reason,
           int64_t render, int64_t gpu, int64_t total) {
    interval = std::clamp(interval, s.n_min, s.n_max);
    depth = std::clamp(depth, 1u, 2 * interval);
    if (interval != s.interval || depth != s.depth) {
        if (LogEnabled()) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "pace %s: interval %u->%u depth %u->%u refresh=%.2fms render=%.1fms "
                "gpu=%.1fms total=%.1fms margin=%.1fms",
                reason, s.interval, interval, s.depth, depth, s.period / 1e6, render / 1e6,
                gpu / 1e6, total / 1e6, s.present_margin / 1e6);
        }
        s.interval = interval;
        s.depth = depth;
        ++s.config;
        ++s.changes;
    }
    // A fresh window: the old samples describe another pace (or already
    // triggered this decision).
    s.sample_count = 0;
    s.on_time = 0;
}

void UpdateBounds(State& s) {
    const double refresh = 1e9 / static_cast<double>(s.period);
    uint32_t n_min = 1;
    if (s.limit_fps > 0) {
        n_min = std::max(1u, static_cast<uint32_t>(std::ceil(refresh / s.limit_fps - 0.05)));
    }
    s.n_min = n_min;
    s.n_max = std::max(n_min, static_cast<uint32_t>(std::floor(refresh / kMinPacedFps + 0.05)));
    if (!s.interval) {
        const uint32_t start = static_cast<uint32_t>(std::lround(refresh / 60.0));
        Apply(s, start, 2 * std::clamp(start, s.n_min, s.n_max), "start", -1, -1, -1);
    } else if (s.interval < s.n_min || s.interval > s.n_max) {
        Apply(s, s.interval, s.depth, "limit", -1, -1, -1);
    }
}

void Reset(State& s, EGLDisplay display, EGLSurface surface) {
    s.display = display;
    s.surface = surface;
    s.usable = s.compositor_timing = s.timestamp_set = s.paced = false;
    s.period = s.grid = 0;
    s.grid_from_present = false;
    s.interval = s.depth = 0;
    ++s.config;
    s.present_margin = kMinPresentMarginNs;
    s.last_target = s.last_swap = s.frame_start = s.last_gpu_done = 0;
    s.inflight_valid = false;
    s.pending_count = s.sample_count = 0;
    s.on_time = s.settle = 0;
    s.gpu_bound = false;
    s.tighten_trial = false;
    s.tighten_wait = kTightenWaitMin;
    g_active.store(false, std::memory_order_relaxed);
}

void Attach(State& s, EGLDisplay display, EGLSurface surface) {
    Reset(s, display, surface);
    if (!LoadEgl(display) ||
        eglSurfaceAttrib(display, surface, EGL_TIMESTAMPS_ANDROID, EGL_TRUE) != EGL_TRUE ||
        g_egl.frame_timestamp_supported(display, surface, EGL_DISPLAY_PRESENT_TIME_ANDROID) != EGL_TRUE ||
        g_egl.frame_timestamp_supported(display, surface, EGL_RENDERING_COMPLETE_TIME_ANDROID) != EGL_TRUE) {
        eglGetError();
        if (LogEnabled()) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "pace unavailable on surface %p", surface);
        }
        return;
    }
    s.compositor_timing =
        g_egl.compositor_timing_supported(display, surface, EGL_COMPOSITE_INTERVAL_ANDROID);
    s.usable = true;
}

// Back to queue-time presentation, as without the pacer.
void Unpace(State& s) {
    if (s.timestamp_set) g_egl.presentation_time(s.display, s.surface, kAutoTimestamp);
    s.timestamp_set = s.paced = false;
    s.last_target = s.frame_start = s.last_gpu_done = 0;
    s.pending_count = 0;
    g_active.store(false, std::memory_order_relaxed);
}

void Finalize(State& s, const Frame& frame, int64_t gpu_done, int64_t present) {
    const bool shown = present > 0;
    if (shown) {
        s.grid = present;
        s.grid_from_present = true;
    }
    if (frame.settling) {
        if (gpu_done > 0) s.last_gpu_done = gpu_done;
        return;
    }
    const bool late = frame.reanchored || !shown || present > frame.target + s.period / 2;
    if (late) ++s.late_total;
    if (late && shown && !frame.reanchored && gpu_done > 0 && gpu_done < frame.target) {
        s.present_margin = std::min(
            std::max(s.present_margin, frame.target - gpu_done + kMs / 2), 2 * s.period);
    }
    const int64_t previous_gpu_done = s.last_gpu_done;
    if (gpu_done > 0) s.last_gpu_done = gpu_done;
    if (frame.config != s.config) return;
    Sample sample;
    sample.late = late;
    if (gpu_done > 0) sample.gpu = std::max<int64_t>(0, gpu_done - frame.swap);
    if (frame.start) {
        sample.render = frame.swap - frame.start;
        if (gpu_done > 0) {
            sample.total = std::max<int64_t>(0, gpu_done - frame.start);
            sample.span = std::max<int64_t>(0, gpu_done - std::max(frame.start, previous_gpu_done));
        }
    }
    s.samples[s.sample_count % kSampleWindow] = sample;
    ++s.sample_count;
    if (late) {
        s.on_time = 0;
    } else if (++s.on_time % 300 == 0 && s.present_margin > kMinPresentMarginNs) {
        s.present_margin = std::max(kMinPresentMarginNs, s.present_margin - kMs / 2);
    }
}

void Harvest(State& s, int64_t now) {
    static constexpr EGLint kNames[] = {EGL_RENDERING_COMPLETE_TIME_ANDROID,
                                        EGL_DISPLAY_PRESENT_TIME_ANDROID};
    size_t kept = 0;
    for (size_t i = 0; i < s.pending_count; ++i) {
        const Frame frame = s.pending[i];
        EGLnsecsANDROID values[2] = {};
        const bool known = g_egl.frame_timestamps(s.display, s.surface, frame.id, 2, kNames,
                                                  values) == EGL_TRUE;
        const bool waiting = known && (values[0] == EGL_TIMESTAMP_PENDING_ANDROID ||
                                       values[1] == EGL_TIMESTAMP_PENDING_ANDROID);
        if (waiting && now - frame.swap < kStaleFrameNs) {
            s.pending[kept++] = frame;
            continue;
        }
        if (known && !waiting) Finalize(s, frame, values[0], values[1]);
    }
    s.pending_count = kept;
}

void Adapt(State& s, int64_t now) {
    if (s.sample_count < kMinSamples) return;
    s.gpu_bound = Percentile(s, kSampleWindow, &Sample::gpu, 75) >
                  Percentile(s, kSampleWindow, &Sample::render, 75);
    const int64_t period = s.period;
    const size_t recent = std::min(s.sample_count, kLateWindow);
    uint32_t late = 0;
    for (size_t i = 0; i < recent; ++i)
        late += s.samples[(s.sample_count - 1 - i) % kSampleWindow].late ? 1 : 0;
    const bool on_trial = s.tighten_trial && now - s.tightened_at_ns < kTrialNs;
    const bool early_trial = on_trial && s.frames - s.tightened_at < kTrialFrames;
    if (late >= (early_trial ? kTrialLateLimit : kLateLimit) &&
        s.samples[(s.sample_count - 1) % kSampleWindow].late) {
        // Slower, one refresh of interval per decision: the interval the
        // measured work needs, as p90 over one burst of slow frames (a scene
        // change, a GPU backlog) can overstate it by whole paces; a deeper drop
        // takes another late burst. Late although that work fits: the depth
        // its measured latency needs if the current one is shorter, else (GPU
        // work before the swap, compositor) one more refresh of interval.
        const int64_t render = Percentile(s, recent, &Sample::render, 90);
        const int64_t gpu = Percentile(s, recent, &Sample::gpu, 90);
        const int64_t total = Percentile(s, recent, &Sample::total, 100);
        const auto fits = [&](uint32_t n) {
            const int64_t budget = int64_t{n} * period - kFitMarginNs;
            return render <= budget && gpu <= budget;
        };
        uint32_t interval = s.interval, depth = s.depth;
        if (interval < s.n_max && !fits(interval)) ++interval;
        if (interval == s.interval) {
            const uint32_t needed = DepthFor(s, total);
            if (needed > depth && depth < 2 * interval) depth = needed;
            else if (interval < s.n_max) ++interval;
            else if (depth < 2 * interval) ++depth;
        }
        if (on_trial) s.tighten_wait = std::min(s.tighten_wait * 2, kTightenWaitMax);
        s.tighten_trial = false;
        Apply(s, interval, std::max(depth, DepthFor(s, total)), "late", render, gpu, total);
        return;
    }
    if (s.tighten_trial && !on_trial) {
        s.tighten_trial = false;
        s.tighten_wait = kTightenWaitMin;
    }
    if (s.on_time < s.tighten_wait) return;
    // Faster or shorter only if every recent frame would have fitted.
    const size_t stored = std::min(s.sample_count, kSampleWindow);
    const int64_t render = Percentile(s, stored, &Sample::render, 100);
    const int64_t gpu = Percentile(s, stored, &Sample::gpu, 100);
    const int64_t total = Percentile(s, stored, &Sample::total, 100);
    const int64_t span = Percentile(s, stored, &Sample::span, 100);
    if (render < 0 || gpu < 0 || total < 0 || span < 0) return;
    if (s.interval > s.n_min) {
        const uint32_t faster = s.interval - 1;
        const int64_t budget = int64_t{faster} * period - kFitMarginNs;
        const bool gpu_sure = span <= budget;
        const uint32_t wait = gpu_sure ? s.tighten_wait :
            std::max(s.tighten_wait, gpu <= budget ? kGpuProbeWait : kGpuBlindProbeWait);
        if (render <= budget && s.on_time >= wait) {
            s.tighten_trial = true;
            s.tightened_at = s.frames;
            s.tightened_at_ns = now;
            Apply(s, faster, DepthFor(s, total), gpu_sure ? "faster" : "gpu-probe", render, gpu,
                  total);
            return;
        }
    }
    if (DepthFor(s, total) < s.depth) {
        s.tighten_trial = true;
        s.tightened_at = s.frames;
        s.tightened_at_ns = now;
        Apply(s, s.interval, s.depth - 1, "depth", render, gpu, total);
    }
}

}  // namespace

Bl2FramePace Bl2FramePacerConfigure(float display_hz, uint32_t limit_fps, bool allowed) {
    if (!Enabled()) return {};
    State& s = PacerState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.display_hz = display_hz;
    s.limit_fps = limit_fps;
    s.allowed = allowed;
    if (!s.paced || !s.interval || !s.period) return {};
    const double period = static_cast<double>(s.period);
    const uint32_t aim = s.gpu_bound ? s.interval : std::max(s.n_min, s.interval - 1);
    return {static_cast<float>(1e9 / (period * s.interval)),
            static_cast<float>(1e9 / (period * aim))};
}

bool Bl2FramePacerActive() { return g_active.load(std::memory_order_relaxed); }

void Bl2FramePacerBeforeSwap(EGLDisplay display, EGLSurface surface) {
    if (!Enabled()) return;
    State& s = PacerState();
    std::lock_guard<std::mutex> lock(s.mutex);
    const int64_t now = Now();
    ++s.frames;
    s.inflight_valid = false;
    if (display != s.display || surface != s.surface) Attach(s, display, surface);
    if (!s.usable) return;
    if (!s.allowed || !(s.display_hz > 0.0f)) {
        Unpace(s);
        return;
    }
    int64_t period = std::llround(1e9 / s.display_hz);
    // SurfaceFlinger's own refresh interval, once it has reported a present:
    // before that its compositor timing is a 60 Hz placeholder.
    if (s.compositor_timing && s.grid_from_present) {
        static constexpr EGLint kNames[] = {EGL_COMPOSITE_INTERVAL_ANDROID};
        EGLnsecsANDROID interval = 0;
        if (g_egl.compositor_timing(display, surface, 1, kNames, &interval) == EGL_TRUE &&
            interval >= kMinPeriodNs && interval <= kMaxPeriodNs) {
            period = interval;
        }
    }
    if (period < kMinPeriodNs || period > kMaxPeriodNs) {
        Unpace(s);
        return;
    }
    if (!s.period || std::llabs(period - s.period) * 200 > s.period) {
        // Another refresh rate: a new grid and cadence.
        s.grid = 0;
        s.grid_from_present = false;
        s.last_target = 0;
    }
    s.period = period;
    if (!s.grid) s.grid = now;  // Any phase until a present time is known.
    if (!s.paced) {
        // Pacing starts or resumes (FRAME PACING switched back on): earlier
        // samples describe another cadence.
        s.sample_count = 0;
        s.on_time = 0;
        s.settle = kSettleFrames;
    }
    UpdateBounds(s);
    if (s.last_swap && now - s.last_swap > kGapResetNs) {
        // After a pause (a dialog, an interruption) the first frames meet cold
        // clocks and caches: a new cadence, settled as at a start.
        s.last_target = s.frame_start = 0;
        s.sample_count = 0;
        s.on_time = 0;
        s.settle = kSettleFrames;
    }

    const int64_t interval_ns = int64_t{s.interval} * s.period;
    int64_t target = s.last_target ? SnapNearest(s, s.last_target + interval_ns) : 0;
    const bool reanchored = target && target < SnapUp(s, now + s.present_margin);
    if (!target || reanchored) {
        const int64_t gpu =
            s.sample_count >= 4 ? Percentile(s, kSampleWindow, &Sample::gpu, 50) : interval_ns;
        target = SnapUp(s, now + std::max<int64_t>(gpu, 0) + s.present_margin);
    }
    // Half a refresh early: SurfaceFlinger shows it at the first refresh whose
    // expected present time is not before the requested one.
    if (g_egl.presentation_time(display, surface, target - s.period / 2) != EGL_TRUE) {
        eglGetError();
        Unpace(s);
        return;
    }
    s.timestamp_set = s.paced = true;
    EGLuint64KHR id = 0;
    if (g_egl.next_frame_id(display, surface, &id) == EGL_TRUE) {
        s.inflight = {id, s.frame_start, now, target, s.config, reanchored, s.settle > 0};
        s.inflight_valid = true;
    }
    if (s.settle) --s.settle;
    s.last_target = target;
    s.last_swap = now;
    g_active.store(true, std::memory_order_relaxed);
}

uint64_t Bl2FramePacerAfterSwap() {
    if (!Enabled()) return 0;
    State& s = PacerState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.paced) return 0;
    if (s.inflight_valid) {
        if (s.pending_count == s.pending.size()) {
            std::move(s.pending.begin() + 1, s.pending.end(), s.pending.begin());
            --s.pending_count;
        }
        s.pending[s.pending_count++] = s.inflight;
        s.inflight_valid = false;
    }
    const int64_t now = Now();
    Harvest(s, now);
    Adapt(s, now);
    eglGetError();  // The guest sees the swap's error state, not these queries'.
    const int64_t interval_ns = int64_t{s.interval} * s.period;
    const int64_t start = s.last_target + interval_ns - int64_t{s.depth} * s.period;
    if (start <= now) return 0;
    if (start - now > 4 * interval_ns) {
        s.last_target = 0;  // Implausible wait: restart the cadence instead.
        return 0;
    }
    return static_cast<uint64_t>(start);
}

void Bl2FramePacerWait(uint64_t start_ns) {
    if (start_ns) {
        const timespec until{static_cast<time_t>(start_ns / kSecond),
                             static_cast<long>(start_ns % kSecond)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {}
    }
    if (!Enabled()) return;
    State& s = PacerState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.frame_start = s.paced ? Now() : 0;
}

void Bl2FramePacerSurfaceDestroyed(EGLSurface surface) {
    if (!Enabled()) return;
    State& s = PacerState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (surface == s.surface) Reset(s, EGL_NO_DISPLAY, EGL_NO_SURFACE);
}

Bl2FramePacerStats Bl2FramePacerTakeStats() {
    if (!Enabled()) return {};
    State& s = PacerState();
    std::lock_guard<std::mutex> lock(s.mutex);
    Bl2FramePacerStats stats{};
    if (s.paced && s.interval && s.period) {
        stats.interval_ns = static_cast<uint64_t>(s.interval * s.period);
        stats.depth_ns = static_cast<uint64_t>(s.depth * s.period);
    }
    stats.late = s.late_total;
    stats.changes = s.changes;
    s.late_total = s.changes = 0;
    return stats;
}
