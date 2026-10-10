#include "guest_software_write_watch.h"
#include "thunks_opensles.h"
#include "bridge_log.h"
#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define LOG_TAG "Arm32Bridge_Audio"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {

constexpr size_t kGuestVtableSlots = 64;
constexpr size_t kGuestHandleBytes = 16;
constexpr SLuint32 kMaxRequestedInterfaces = 32;
constexpr uint32_t kMaxPcmBufferBytes = 64u * 1024u * 1024u;
constexpr uint32_t kPcmSliceTargetBytes = 256u * 1024u;
constexpr uint32_t kMaxPcmSlices = 8u;
constexpr uint32_t kAudioDumpMinDurationMs = 1500u;
constexpr uint32_t kMaxAudioDumpFiles = 48u;
constexpr uint64_t kMaxAudioDumpBytes = 64u * 1024u * 1024u;
constexpr const char* kAudioDumpDirectory =
    "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/bl2_audio_probe";
// A guest requeue callback is deliberately forwarded after the first slice so
// a looping sound can enqueue its next copy while the current copy's tail is
// playing. The host queue therefore needs room for that tail plus one complete
// new copy: (kMaxPcmSlices - 1) + kMaxPcmSlices slots in the worst case.
constexpr SLuint32 kMinHostBufferQueueDepth = 2u * kMaxPcmSlices;

enum class PlayerInterfaceKind {
    Play,
    BufferQueue,
    Volume,
    Configuration,
};

// AOSP IObject::RegisterCallback stores a callback/context pair and succeeds,
// including for objects realized synchronously. BL2's Wwise sink checks that
// result for its engine, output mix, and player; returning FEATURE_UNSUPPORTED
// here makes it discard the entire OpenSL sink and select CAkSinkDummy.
//
// This bridge supports synchronous object operations only (Realize and Resume
// reject async). Android does not implement resource-loss object events. Thus
// this subset has no object completion events to emit: retain/replace/unregister
// the pair with the object's lifetime, and never invent an ASYNC_TERMINATION.
// Buffer-queue callbacks are separate and continue through the real dispatcher.
// Guarded by g_state_mutex; the shared engine registration survives nonfinal
// engine-lease releases, just like the shared host object.
struct ObjectCallbackRegistration {
    uint32_t callback = 0;
    uint32_t context = 0;
};

struct PlayerState {
    SLObjectItf object = nullptr;
    SLPlayItf play = nullptr;
    SLAndroidSimpleBufferQueueItf buffer_queue = nullptr;
    SLVolumeItf volume = nullptr;
    SLAndroidConfigurationItf configuration = nullptr;

    uint32_t guest_object = 0;
    uint32_t guest_play = 0;
    uint32_t guest_buffer_queue = 0;
    uint32_t guest_volume = 0;
    uint32_t guest_configuration = 0;
    ObjectCallbackRegistration object_callback;

    std::atomic<uint32_t> completed_buffers{0};
    std::atomic<uint32_t> guest_callback{0};
    std::atomic<uint32_t> guest_callback_context{0};
    std::atomic<bool> alive{true};

    SLuint32 requested_play_state = SL_PLAYSTATE_STOPPED;
    bool realized = false;
    uint64_t enqueue_count = 0;
    uint64_t submitted_host_buffers = 0;
    uint64_t completed_host_buffers = 0;
    bool natural_stop_reported = false;

    uint64_t serial = 0;
    uint32_t create_caller = 0;
    uint32_t first_enqueue_caller = 0;
    uint32_t first_pcm_size = 0;
    uint64_t first_pcm_hash = 0;
    uint64_t total_pcm_bytes = 0;
    int64_t created_ms = 0;
    int64_t first_enqueue_ms = 0;
    int64_t first_play_ms = 0;

    SLuint32 channels = 0;
    SLuint32 sample_rate_millihz = 0;
    SLuint32 bits_per_sample = 0;
    SLuint32 container_bits = 0, pcm_endianness = 0;
    uint64_t queue_generation = 0;  // Audio-audit discontinuity witness.
    std::atomic<SLmillibel> last_volume_level{0};
    std::atomic<bool> volume_level_set{false};
    std::atomic<bool> pcm_probe_logged{false};

    // Rate-limit windows for transition telemetry. The guest creates one OpenSL
    // player per voice (FOpenSLSoundSource), so unbounded state logging would
    // flood logcat during heavy action; diagnostics must survive that instead.
    std::atomic<SLuint32> last_logged_play_state{0xFFFFFFFFu};
    std::atomic<int64_t> last_state_log_ms{-1000000};
    std::atomic<int64_t> last_underrun_log_ms{-1000000};

    // Host-side granularity for callback-driven requeue players. Large guest
    // Enqueue calls are sliced into sub-buffers: the completion of the FIRST
    // slice arrives while later slices still play, and only that one is
    // forwarded to the guest (first_slice below). Extra slice completions are
    // swallowed.
    struct QueuedPcm {
        std::shared_ptr<std::vector<uint8_t>> payload;
        bool first_slice = true;           // forwards a guest callback
    };
    std::deque<QueuedPcm> queued_pcm;
    uint32_t forwardable_completions = 0;  // guarded by g_state_mutex
};

struct PlayerInterfaceRef {
    std::shared_ptr<PlayerState> player;
    PlayerInterfaceKind kind;
};

struct OutputMixState {
    SLObjectItf object = nullptr;
    uint32_t guest_object = 0;
    bool realized = false;
    ObjectCallbackRegistration object_callback;
};

SLObjectItf g_host_engine_object = nullptr;
SLEngineItf g_host_engine = nullptr;
bool g_host_engine_realized = false;
// Android shares one engine across slCreateEngine calls, with one reference
// per successful call. Bink holds independent movie and timing-player leases.
// Destroying either lease must not invalidate the other one's live objects.
std::mutex g_engine_lifecycle_mutex;
uint32_t g_guest_engine_references = 0;
ObjectCallbackRegistration g_engine_object_callback;
bool g_engine_object_callback_live = false;  // guarded by g_state_mutex

uint32_t g_guest_engine_object = 0;
uint32_t g_guest_engine_object_vtable = 0;
uint32_t g_guest_engine_interface = 0;
uint32_t g_guest_engine_interface_vtable = 0;
uint32_t g_guest_output_mix_object_vtable = 0;
uint32_t g_guest_player_object_vtable = 0;
uint32_t g_guest_play_vtable = 0;
uint32_t g_guest_buffer_queue_vtable = 0;
uint32_t g_guest_volume_vtable = 0;
uint32_t g_guest_configuration_vtable = 0;

std::mutex g_state_mutex;
std::unordered_map<uint32_t, std::shared_ptr<PlayerState>> g_players_by_object;
std::unordered_map<uint32_t, PlayerInterfaceRef> g_players_by_interface;
std::unordered_map<uint32_t, std::shared_ptr<OutputMixState>> g_output_mixes;
// The host callback receives PlayerState*. Retired entries deliberately stay
// alive until engine teardown so a callback racing Destroy can never dereference
// freed memory.
std::vector<std::shared_ptr<PlayerState>> g_retired_players;
std::unordered_map<std::string, uint32_t> g_iid_pointer_map;

std::atomic<bool> g_audio_thread_running{false};
std::atomic<uint64_t> g_audio_thread_generation{0};
std::atomic<bool> g_audio_paused{false};
std::mutex g_audio_thread_mutex;
std::mutex g_audio_cv_mutex;
std::condition_variable g_audio_cv;
std::unique_ptr<std::thread> g_audio_thread;
std::atomic<uint64_t> g_next_player_serial{1};
std::atomic<uint32_t> g_audio_dump_files{0};
std::atomic<uint64_t> g_audio_dump_bytes{0};

// Host OpenSL ES Buffer Queue Callback (fired on real-time audio thread by Android HAL)
static void HostBqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context) {
    (void)bq;
    auto* player = static_cast<PlayerState*>(context);
    if (!player || !player->alive.load(std::memory_order_acquire)) return;

    player->completed_buffers.fetch_add(1, std::memory_order_release);
    // No guest memory, locks, logging, allocation, or JIT entry is permitted on
    // Android's real-time callback thread.
    g_audio_cv.notify_one();
}

int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool AudioProbeEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.audio_probe", value) > 0 &&
               std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0;
    }();
    return enabled;
}

bool AudioDumpEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        const bool result =
            __system_property_get("debug.horn.audio_dump", value) > 0 &&
            value[0] != '\0' && std::strcmp(value, "0") != 0 &&
            std::strcmp(value, "false") != 0;
        if (result) {
            LOGI("audio probe WAV capture enabled: %s", kAudioDumpDirectory);
        }
        return result;
    }();
    return enabled;
}

uint64_t HashPcm(const uint8_t* pcm, uint32_t size) {
    // FNV-1a is intentionally stable across builds and cheap enough for the
    // first decoded buffer of a diagnostic player.
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < size; ++i) {
        hash ^= pcm[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

double PcmDurationMs(const PlayerState& player, uint32_t size) {
    const uint64_t rate_hz = player.sample_rate_millihz / 1000u;
    const uint64_t bytes_per_sample = player.bits_per_sample / 8u;
    return player.channels != 0 && rate_hz != 0 && bytes_per_sample != 0
        ? 1000.0 * static_cast<double>(size) /
              (static_cast<double>(player.channels) * bytes_per_sample * rate_hz)
        : 0.0;
}

uint32_t PcmFrameBytes(const PlayerState& player) {
    if (player.channels == 0 || player.bits_per_sample == 0 ||
        player.bits_per_sample % 8u != 0) {
        return 0;
    }
    const uint64_t frame_bytes = static_cast<uint64_t>(player.channels) *
        (player.bits_per_sample / 8u);
    return frame_bytes <= UINT32_MAX ? static_cast<uint32_t>(frame_bytes) : 0;
}

void WriteLe16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

void WriteLe32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

void DumpPcmProbe(const PlayerState& player, const uint8_t* pcm, uint32_t size,
                  uint64_t hash, uint32_t caller) {
    if (!AudioDumpEnabled() || !pcm ||
        player.bits_per_sample != SL_PCMSAMPLEFORMAT_FIXED_16 ||
        player.guest_callback.load(std::memory_order_acquire) != 0 ||
        PcmDurationMs(player, size) < kAudioDumpMinDurationMs) {
        return;
    }

    const uint32_t file_index =
        g_audio_dump_files.fetch_add(1, std::memory_order_relaxed);
    const uint64_t byte_offset =
        g_audio_dump_bytes.fetch_add(size, std::memory_order_relaxed);
    if (file_index >= kMaxAudioDumpFiles ||
        byte_offset + size > kMaxAudioDumpBytes) {
        static std::atomic<bool> limit_logged{false};
        if (!limit_logged.exchange(true)) {
            LOGI("audio probe WAV capture limit reached");
        }
        return;
    }

    if (mkdir(kAudioDumpDirectory, 0700) != 0 && errno != EEXIST) {
        static std::atomic<bool> mkdir_error_logged{false};
        if (!mkdir_error_logged.exchange(true)) {
            LOGE("audio probe cannot create %s (errno %d)",
                 kAudioDumpDirectory, errno);
        }
        return;
    }

    char path[384] = {};
    std::snprintf(path, sizeof(path),
                  "%s/p%06llu_h%016llX_ms%06u_c%08X_e%08X.wav",
                  kAudioDumpDirectory,
                  static_cast<unsigned long long>(player.serial),
                  static_cast<unsigned long long>(hash),
                  static_cast<unsigned>(PcmDurationMs(player, size) + 0.5),
                  player.create_caller, caller);
    FILE* file = std::fopen(path, "wb");
    if (!file) {
        LOGE("audio probe cannot open WAV %s (errno %d)", path, errno);
        return;
    }

    uint8_t header[44] = {};
    std::memcpy(header + 0, "RIFF", 4);
    WriteLe32(header + 4, 36u + size);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    WriteLe32(header + 16, 16u);
    WriteLe16(header + 20, 1u);
    WriteLe16(header + 22, static_cast<uint16_t>(player.channels));
    const uint32_t rate_hz = player.sample_rate_millihz / 1000u;
    const uint16_t block_align = static_cast<uint16_t>(
        player.channels * player.bits_per_sample / 8u);
    WriteLe32(header + 24, rate_hz);
    WriteLe32(header + 28, rate_hz * block_align);
    WriteLe16(header + 32, block_align);
    WriteLe16(header + 34, static_cast<uint16_t>(player.bits_per_sample));
    std::memcpy(header + 36, "data", 4);
    WriteLe32(header + 40, size);

    const bool wrote = std::fwrite(header, 1, sizeof(header), file) ==
                           sizeof(header) &&
                       std::fwrite(pcm, 1, size, file) == size;
    const int close_result = std::fclose(file);
    if (wrote && close_result == 0) {
        LOGI("audio probe WAV serial=%llu path=%s", 
             static_cast<unsigned long long>(player.serial), path);
    } else {
        LOGE("audio probe incomplete WAV serial=%llu path=%s",
             static_cast<unsigned long long>(player.serial), path);
    }
}

void LogPcmProbe(const PlayerState& player, const uint8_t* pcm, uint32_t size,
                 uint64_t hash, uint32_t caller) {
    if (!AudioProbeEnabled() || !pcm || size < sizeof(int16_t) ||
        player.bits_per_sample != SL_PCMSAMPLEFORMAT_FIXED_16) {
        return;
    }

    const uint64_t sample_count = size / sizeof(int16_t);
    uint64_t nonzero = 0;
    uint32_t peak = 0;
    long double sum_squares = 0.0L;
    for (uint64_t i = 0; i < sample_count; ++i) {
        int16_t sample = 0;
        std::memcpy(&sample, pcm + i * sizeof(sample), sizeof(sample));
        const int32_t widened = sample;
        const uint32_t magnitude = static_cast<uint32_t>(
            widened < 0 ? -widened : widened);
        if (magnitude != 0) ++nonzero;
        if (magnitude > peak) peak = magnitude;
        sum_squares += static_cast<long double>(widened) * widened;
    }

    const long double rms = std::sqrt(sum_squares / sample_count);
    const double rms_dbfs = rms > 0.0L
        ? 20.0 * std::log10(static_cast<double>(rms) / 32768.0)
        : -120.0;
    const double nonzero_percent =
        100.0 * static_cast<double>(nonzero) / static_cast<double>(sample_count);
    LOGI("audio probe serial=%llu player=0x%08X hash=%016llX caller=0x%08X "
         "bytes=%u duration=%.1fms channels=%u callback=%u volume=%d "
         "peak=%u rms=%.1fdBFS nonzero=%.2f%%",
         static_cast<unsigned long long>(player.serial), player.guest_object,
         static_cast<unsigned long long>(hash), caller, size,
         PcmDurationMs(player, size), player.channels,
         player.guest_callback.load(std::memory_order_acquire) != 0,
         static_cast<int>(player.last_volume_level.load(std::memory_order_acquire)),
         peak, rms_dbfs, nonzero_percent);
}

// A bounded late meter distinguishes real menu/game PCM from the initial
// silent Wwise buffers. Only sums samples; it never changes queue data or gain.
static void MeterLivePcm(const PlayerState& player, const uint8_t* pcm, uint32_t size) {
    struct Meter {
        uint32_t poll=0, windows=0, peak=0;
        uint64_t player=0, samples=0, nonzero=0, squares=0;
        int64_t started=0, window=0;
        bool enabled=false, armed=false;
    };
    static thread_local Meter meter;
    if ((++meter.poll & 31u)==1u) {
        char value[PROP_VALUE_MAX]={};
        const bool requested=__system_property_get("debug.bl2.audio_meter",value)>0 && value[0]=='1';
        if (!requested) {meter.enabled=false;meter.armed=false;}
        if (requested && !meter.armed && meter.windows<60) {
            meter.armed=true;meter.enabled=true;meter.player=player.serial;
            meter.started=meter.window=SteadyNowMs();
            meter.samples=meter.nonzero=meter.squares=0;meter.peak=0;
            LOGI("AUDIO_METER_BEGIN serial=%llu max_ms=12000 sample_bits=%u channels=%u",
                 static_cast<unsigned long long>(player.serial),player.bits_per_sample,player.channels);
        }
    }
    if (!meter.enabled || meter.player!=player.serial ||
        player.bits_per_sample!=SL_PCMSAMPLEFORMAT_FIXED_16) return;
    for(uint32_t i=0;i+1<size;i+=2) {
        int16_t raw=0;std::memcpy(&raw,pcm+i,2);
        const int32_t v=raw;
        const uint32_t a=static_cast<uint32_t>(v<0?-v:v);
        ++meter.samples;meter.nonzero+=(a!=0);
        meter.squares+=static_cast<uint64_t>(a)*a;
        if(a>meter.peak)meter.peak=a;
    }
    const int64_t now=SteadyNowMs();
    if(now-meter.window<1000)return;
    const double rms=meter.samples?std::sqrt(static_cast<double>(meter.squares)/meter.samples):0.0;
    LOGI("AUDIO_METER serial=%llu window=%u elapsed_ms=%lld samples=%llu nonzero=%llu peak=%u rms_dbfs=%.2f host_gain_mb=%d",
         static_cast<unsigned long long>(player.serial),++meter.windows,
         static_cast<long long>(now-meter.started),static_cast<unsigned long long>(meter.samples),
         static_cast<unsigned long long>(meter.nonzero),meter.peak,
         rms>0?20.0*std::log10(rms/32768.0):-120.0,
         static_cast<int>(player.last_volume_level.load(std::memory_order_acquire)));
    meter.samples=meter.nonzero=meter.squares=0;meter.peak=0;meter.window=now;
    if(now-meter.started>=12000 || meter.windows>=60)meter.enabled=false;
}

// Diagnostic only. Every caller holds g_state_mutex. Never called by the real-time
// host callback. Captures SUCCESSFUL host queue submissions from ALL live players.
// No PCM writes, gain changes, guest-memory reads, files, allocation, or callbacks.
// A new positive token arms one 12-second capture. A token is used at most once
// until another value is observed. Eight captures, 32 streams, 16 MiB per capture.
struct AudioAuditStream {
    uint64_t serial = 0, initial_submitted = 0, initial_completed = 0, epoch = 0;
    uint64_t bytes = 0, samples = 0, nonzero = 0, submitted = 0, completed = 0;
    uint32_t object = 0, peak = 0;
    int64_t drained_ms = -1;
    bool disrupted = false;
};
struct AudioAuditState {
    std::array<AudioAuditStream, 32> streams{};
    uint32_t token = 0, count = 0, peak = 0;
    uint64_t bytes = 0, samples = 0, nonzero = 0, unknown_bytes = 0;
    int64_t started = 0;
    bool active = false, limited = false;
};
static AudioAuditState g_audio_audit;
static uint32_t g_audio_audit_last_token = 0, g_audio_audit_sessions = 0;
static int64_t g_audio_audit_next_poll = 0;
constexpr uint64_t kAudioAuditMaxBytes = 16u * 1024u * 1024u;
constexpr int64_t kAudioAuditDurationMs = 12000;

static uint64_t AudioAuditCompletedLocked(const PlayerState& player) {
    return player.completed_host_buffers +
        player.completed_buffers.load(std::memory_order_acquire);
}

static AudioAuditStream* AudioAuditAddLocked(const PlayerState& player,
                                            uint64_t before_submitted) {
    auto& audit = g_audio_audit;
    for (uint32_t i = 0; i < audit.count; ++i)
        if (audit.streams[i].serial == player.serial) return &audit.streams[i];
    if (audit.count == audit.streams.size()) {
        audit.limited = true;
        return nullptr;
    }
    auto& stream = audit.streams[audit.count++];
    stream.serial = player.serial;
    stream.object = player.guest_object;
    stream.epoch = player.queue_generation;
    stream.initial_submitted = before_submitted;
    // A newly seen player's current submission may already have completed on
    // the real-time thread. Keep that completion in the observed phase.
    stream.initial_completed = std::min(AudioAuditCompletedLocked(player), before_submitted);
    // Zero means no unobserved tail exists, also for a player created later.
    if (stream.initial_completed >= before_submitted) stream.drained_ms = 0;
    LOGI("AUDIO_AUDIT_STREAM_BEGIN token=%u serial=%llu object=0x%08X "
         "create=0x%08X callback=0x%08X channels=%u rate_mhz=%u bits=%u "
         "container=%u endian=%u initial_submitted=%llu initial_completed=%llu epoch=%llu",
         audit.token, static_cast<unsigned long long>(stream.serial), stream.object,
         player.create_caller, player.guest_callback.load(std::memory_order_acquire),
         player.channels, player.sample_rate_millihz, player.bits_per_sample,
         player.container_bits, player.pcm_endianness,
         static_cast<unsigned long long>(before_submitted),
         static_cast<unsigned long long>(stream.initial_completed),
         static_cast<unsigned long long>(stream.epoch));
    return &stream;
}

static void AudioAuditRefreshLocked(int64_t now) {
    auto& audit = g_audio_audit;
    for (uint32_t i = 0; i < audit.count; ++i) {
        auto& stream = audit.streams[i];
        const auto found = g_players_by_object.find(stream.object);
        if (found == g_players_by_object.end() ||
            found->second->serial != stream.serial ||
            !found->second->alive.load(std::memory_order_acquire)) {
            stream.disrupted = true;
            continue;
        }
        const auto& player = *found->second;
        stream.submitted = player.submitted_host_buffers;
        stream.completed = AudioAuditCompletedLocked(player);
        if (player.queue_generation != stream.epoch) stream.disrupted = true;
        if (stream.drained_ms < 0 && stream.completed >= stream.initial_submitted)
            stream.drained_ms = now - audit.started;
    }
}

static void AudioAuditEndLocked(int64_t now, const char* reason) {
    auto& audit = g_audio_audit;
    AudioAuditRefreshLocked(now);
    uint32_t disrupted = 0;
    int64_t max_drain = 0;
    for (uint32_t i = 0; i < audit.count; ++i) {
        const auto& stream = audit.streams[i];
        disrupted += stream.disrupted;
        if (stream.drained_ms < 0) max_drain = -1;
        else if (max_drain >= 0) max_drain = std::max(max_drain, stream.drained_ms);
        SLmillibel gain = 0;
        SLboolean mute = SL_BOOLEAN_FALSE;
        SLuint32 state = 0;
        SLresult gain_result = SL_RESULT_PRECONDITIONS_VIOLATED;
        SLresult mute_result = SL_RESULT_PRECONDITIONS_VIOLATED;
        SLresult state_result = SL_RESULT_PRECONDITIONS_VIOLATED;
        const auto found = g_players_by_object.find(stream.object);
        if (found != g_players_by_object.end() && found->second->serial == stream.serial &&
            found->second->alive.load(std::memory_order_acquire)) {
            const auto& player = *found->second;
            if (player.volume) {
                gain_result = (*player.volume)->GetVolumeLevel(player.volume, &gain);
                mute_result = (*player.volume)->GetMute(player.volume, &mute);
            }
            if (player.play) state_result = (*player.play)->GetPlayState(player.play, &state);
        }
        LOGI("AUDIO_AUDIT_STREAM_END token=%u serial=%llu bytes=%llu samples=%llu "
             "nonzero=%llu peak=%u submitted=%llu completed=%llu old_drained_ms=%lld "
             "disrupted=%u host_gain_mb=%d gain_result=%u host_mute=%u mute_result=%u "
             "host_state=%u state_result=%u",
             audit.token, static_cast<unsigned long long>(stream.serial),
             static_cast<unsigned long long>(stream.bytes),
             static_cast<unsigned long long>(stream.samples),
             static_cast<unsigned long long>(stream.nonzero), stream.peak,
             static_cast<unsigned long long>(stream.submitted),
             static_cast<unsigned long long>(stream.completed),
             static_cast<long long>(stream.drained_ms), static_cast<unsigned>(stream.disrupted),
             static_cast<int>(gain), gain_result, mute, mute_result, state, state_result);
    }
    LOGI("AUDIO_AUDIT_END token=%u reason=%s elapsed_ms=%lld streams=%u bytes=%llu "
         "samples=%llu nonzero=%llu peak=%u unknown_bytes=%llu limited=%u "
         "disrupted=%u old_drained_max_ms=%lld",
         audit.token, reason, static_cast<long long>(now - audit.started), audit.count,
         static_cast<unsigned long long>(audit.bytes),
         static_cast<unsigned long long>(audit.samples),
         static_cast<unsigned long long>(audit.nonzero), audit.peak,
         static_cast<unsigned long long>(audit.unknown_bytes), static_cast<unsigned>(audit.limited),
         disrupted, static_cast<long long>(max_drain));
    audit.active = false;
}

static void AudioAuditPollLocked() {
    const int64_t now = SteadyNowMs();
    auto& audit = g_audio_audit;
    if (audit.active && now - audit.started >= kAudioAuditDurationMs)
        AudioAuditEndLocked(now, "duration");
    if (now < g_audio_audit_next_poll) return;
    g_audio_audit_next_poll = now + 100;
    if (audit.active) AudioAuditRefreshLocked(now);
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.bl2.audio_audit", value);
    uint32_t token = 0;
    for (uint32_t i = 0; value[i]; ++i) {
        if (value[i] < '0' || value[i] > '9' || token > 99999) { token = 0; break; }
        token = token * 10 + static_cast<uint32_t>(value[i] - '0');
    }
    if (token == g_audio_audit_last_token) return;
    g_audio_audit_last_token = token;
    if (audit.active) AudioAuditEndLocked(now, "token_change");
    if (!token || g_audio_audit_sessions >= 8) return;
    ++g_audio_audit_sessions;
    audit = AudioAuditState{};
    audit.active = true;
    audit.token = token;
    audit.started = now;
    LOGI("AUDIO_AUDIT_BEGIN token=%u session=%u mono_ms=%lld max_ms=%lld "
         "max_bytes=%llu max_streams=%zu active_players=%zu",
         token, g_audio_audit_sessions, static_cast<long long>(now),
         static_cast<long long>(kAudioAuditDurationMs),
         static_cast<unsigned long long>(kAudioAuditMaxBytes), audit.streams.size(),
         g_players_by_object.size());
    for (const auto& item : g_players_by_object)
        AudioAuditAddLocked(*item.second, item.second->submitted_host_buffers);
}

static void AudioAuditSubmittedLocked(const PlayerState& player,
                                      const uint8_t* pcm, uint32_t size) {
    auto& audit = g_audio_audit;
    if (!audit.active) return;
    // Called after the successful host Enqueue and counter increment. A player
    // first seen during this phase has no earlier unobserved phase submissions.
    auto* stream = AudioAuditAddLocked(player, player.submitted_host_buffers - 1);
    if (!stream || audit.bytes + size > kAudioAuditMaxBytes) {
        audit.limited = true;
        AudioAuditEndLocked(SteadyNowMs(), "limit");
        return;
    }
    audit.bytes += size;
    stream->bytes += size;
    if (player.bits_per_sample != SL_PCMSAMPLEFORMAT_FIXED_16 ||
        player.container_bits != 16 || player.pcm_endianness != SL_BYTEORDER_LITTLEENDIAN ||
        !player.channels || size % (static_cast<uint64_t>(player.channels) * 2) != 0) {
        audit.unknown_bytes += size;
        return;
    }
    const uint64_t samples = size / 2;
    uint64_t nonzero = 0;
    uint32_t peak = 0;
    for (uint32_t offset = 0; offset < size; offset += 2) {
        int16_t sample = 0;
        std::memcpy(&sample, pcm + offset, sizeof(sample));
        const int32_t widened = sample;
        const uint32_t magnitude = static_cast<uint32_t>(widened < 0 ? -widened : widened);
        nonzero += magnitude != 0;
        peak = std::max(peak, magnitude);
    }
    audit.samples += samples;
    audit.nonzero += nonzero;
    audit.peak = std::max(audit.peak, peak);
    stream->samples += samples;
    stream->nonzero += nonzero;
    stream->peak = std::max(stream->peak, peak);
}


void LogPlayerLifecycle(const PlayerState& player, const char* event,
                        uint32_t caller, SLuint32 requested_state,
                        SLuint32 host_state) {
    if (!AudioProbeEnabled()) return;
    const int64_t now = SteadyNowMs();
    const int64_t age_ms = player.created_ms != 0
        ? now - player.created_ms : -1;
    const int64_t play_age_ms = player.first_play_ms != 0
        ? now - player.first_play_ms : -1;
    LOGI("audio lifecycle event=%s serial=%llu player=0x%08X caller=0x%08X "
         "state=%u host_state=%u age=%lldms play_age=%lldms pcm=%llu/%.1fms "
         "hash=%016llX enqueues=%llu submitted=%llu completed=%llu pending=%u "
         "owned=%zu volume=%d",
         event, static_cast<unsigned long long>(player.serial),
         player.guest_object, caller, requested_state, host_state,
         static_cast<long long>(age_ms), static_cast<long long>(play_age_ms),
         static_cast<unsigned long long>(player.total_pcm_bytes),
         PcmDurationMs(player, static_cast<uint32_t>(std::min<uint64_t>(
             player.total_pcm_bytes, UINT32_MAX))),
         static_cast<unsigned long long>(player.first_pcm_hash),
         static_cast<unsigned long long>(player.enqueue_count),
         static_cast<unsigned long long>(player.submitted_host_buffers),
         static_cast<unsigned long long>(player.completed_host_buffers),
         player.completed_buffers.load(std::memory_order_acquire),
         player.queued_pcm.size(),
         static_cast<int>(player.last_volume_level.load(
             std::memory_order_acquire)));
}

bool GuestRangeValid(const DynarmicHost& host, uint32_t address, size_t size) {
    if (address == 0) return false;
    return static_cast<uint64_t>(address) + static_cast<uint64_t>(size) <=
           static_cast<uint64_t>(host.GetGuestMemorySize());
}

bool IID_Matches(DynarmicHost& host, uint32_t iid_ptr, SLInterfaceID host_iid) {
    if (!host_iid || !GuestRangeValid(host, iid_ptr, 4)) return false;

    if (GuestRangeValid(host, iid_ptr, sizeof(struct SLInterfaceID_)) &&
        std::memcmp(host.GetGuestMemoryBase() + iid_ptr, host_iid,
                    sizeof(struct SLInterfaceID_)) == 0) {
        return true;
    }

    const uint32_t dereferenced = host.MemoryRead32(iid_ptr);
    return GuestRangeValid(host, dereferenced, sizeof(struct SLInterfaceID_)) &&
           std::memcmp(host.GetGuestMemoryBase() + dereferenced, host_iid,
                       sizeof(struct SLInterfaceID_)) == 0;
}

uint32_t AllocAndRegisterIID(ThunkManager& manager, DynarmicHost& host,
                             const std::string& name, SLInterfaceID host_iid) {
    const uint32_t struct_addr = host.GuestAlloc(sizeof(struct SLInterfaceID_), 4);
    const uint32_t pointer_addr = host.GuestAlloc(4, 4);
    if (!struct_addr || !pointer_addr || !host_iid) {
        LOGE("Failed to allocate OpenSL ES IID %s", name.c_str());
        if (struct_addr) host.GuestFree(struct_addr);
        if (pointer_addr) host.GuestFree(pointer_addr);
        manager.RegisterSymbol(name, 0);
        return 0;
    }

    std::memcpy(host.GetGuestMemoryBase() + struct_addr, host_iid,
                sizeof(struct SLInterfaceID_));
    NotifyGuestMemoryWritten(struct_addr, sizeof(struct SLInterfaceID_));
    host.MemoryWrite32(pointer_addr, struct_addr);
    manager.RegisterSymbol(name, pointer_addr);
    g_iid_pointer_map[name] = pointer_addr;
    LOGI("Registered OpenSL ES IID %s: ptr=0x%08X -> struct=0x%08X",
         name.c_str(), pointer_addr, struct_addr);
    return pointer_addr;
}

bool EnsureVtable(DynarmicHost& host, uint32_t& address,
                  std::initializer_list<std::pair<size_t, const char*>> methods) {
    if (address != 0) return true;
    address = host.GuestAlloc(kGuestVtableSlots * 4, 8);
    if (!address) {
        LOGE("Unable to allocate guest OpenSL ES vtable");
        return false;
    }

    const uint32_t unsupported = host.GetThunkManager().GetOrCreateThunkAddress(
        "SLOpenSL_UnsupportedMethod");
    for (size_t i = 0; i < kGuestVtableSlots; ++i) {
        host.MemoryWrite32(address + static_cast<uint32_t>(i * 4), unsupported);
    }
    for (const auto& method : methods) {
        host.MemoryWrite32(address + static_cast<uint32_t>(method.first * 4),
                           host.GetThunkManager().GetOrCreateThunkAddress(method.second));
    }
    return true;
}

uint32_t AllocGuestHandle(DynarmicHost& host, uint32_t vtable) {
    if (!vtable) return 0;
    const uint32_t handle = host.GuestAlloc(kGuestHandleBytes, 8);
    if (!handle) return 0;
    std::memset(host.GetGuestMemoryBase() + handle, 0, kGuestHandleBytes);
    NotifyGuestMemoryWritten(handle, kGuestHandleBytes);
    host.MemoryWrite32(handle, vtable);
    return handle;
}

SLresult ValidateRequestedInterfaces(
        DynarmicHost& host, const char* operation, SLuint32 count,
        uint32_t guest_ids, uint32_t guest_required,
        std::initializer_list<SLInterfaceID> supported) {
    if (count == 0) return SL_RESULT_SUCCESS;
    if (count > kMaxRequestedInterfaces ||
        !GuestRangeValid(host, guest_ids, static_cast<size_t>(count) * 4) ||
        !GuestRangeValid(host, guest_required, static_cast<size_t>(count) * 4)) {
        LOGE("%s: malformed interface arrays (count=%u ids=0x%08X required=0x%08X)",
             operation, count, guest_ids, guest_required);
        return SL_RESULT_PARAMETER_INVALID;
    }

    for (SLuint32 i = 0; i < count; ++i) {
        const uint32_t iid = host.MemoryRead32(guest_ids + i * 4);
        const SLboolean required = static_cast<SLboolean>(
            host.MemoryRead32(guest_required + i * 4));
        bool recognized = false;
        for (SLInterfaceID candidate : supported) {
            if (IID_Matches(host, iid, candidate)) {
                recognized = true;
                break;
            }
        }
        if (!recognized && required == SL_BOOLEAN_TRUE) {
            LOGE("%s: required guest IID 0x%08X is unsupported", operation, iid);
            return SL_RESULT_FEATURE_UNSUPPORTED;
        }
        if (!recognized) {
            LOGI("%s: ignoring optional unsupported guest IID 0x%08X", operation, iid);
        }
    }
    return SL_RESULT_SUCCESS;
}

std::shared_ptr<PlayerState> FindPlayerObject(uint32_t guest_object) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const auto it = g_players_by_object.find(guest_object);
    return it == g_players_by_object.end() ? nullptr : it->second;
}

std::shared_ptr<PlayerState> FindPlayerInterface(uint32_t guest_interface,
                                                 PlayerInterfaceKind expected,
                                                 const char* operation) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const auto it = g_players_by_interface.find(guest_interface);
    if (it == g_players_by_interface.end() || it->second.kind != expected ||
        !it->second.player->alive.load(std::memory_order_acquire)) {
        LOGE("%s: unknown/wrong guest interface 0x%08X", operation, guest_interface);
        return nullptr;
    }
    return it->second.player;
}

void RegisterPlayerInterfaceLocked(const std::shared_ptr<PlayerState>& player,
                                   uint32_t guest_interface,
                                   PlayerInterfaceKind kind) {
    g_players_by_interface[guest_interface] = PlayerInterfaceRef{player, kind};
}

void ReclaimCompletedBuffersLocked(PlayerState& player) {
    uint32_t completed = player.completed_buffers.exchange(0, std::memory_order_acq_rel);
    while (completed != 0 && !player.queued_pcm.empty()) {
        // Only the first slice of a guest chunk forwards a callback; the rest
        // of the chunk's slices complete silently (see QueuedPcm).
        const PlayerState::QueuedPcm entry = player.queued_pcm.front();
        player.queued_pcm.pop_front();
        ++player.completed_host_buffers;
        if (entry.first_slice) ++player.forwardable_completions;
        --completed;
    }
    if (completed != 0) {
        LOGE("OpenSL player 0x%08X reported %u completion(s) without owned PCM",
             player.guest_object, completed);
    }
}

SLObjectItf FindHostObject(uint32_t guest_object) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (guest_object != 0 && guest_object == g_guest_engine_object) {
        return g_host_engine_object;
    }
    const auto player = g_players_by_object.find(guest_object);
    if (player != g_players_by_object.end()) return player->second->object;
    const auto mix = g_output_mixes.find(guest_object);
    return mix == g_output_mixes.end() ? nullptr : mix->second->object;
}

SLresult RegisterGuestObjectCallback(uint32_t guest_object, uint32_t callback,
                                      uint32_t context) {
    // Resolve and update while locked, so registration cannot revive a destroyed
    // object. Do not acquire g_engine_lifecycle_mutex here: final engine teardown
    // can hold it while joining a dispatcher whose guest callback re-registers.
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    ObjectCallbackRegistration* registration = nullptr;
    if (g_engine_object_callback_live && guest_object == g_guest_engine_object) {
        registration = &g_engine_object_callback;
    } else if (const auto player = g_players_by_object.find(guest_object);
               player != g_players_by_object.end() &&
               player->second->alive.load(std::memory_order_acquire)) {
        registration = &player->second->object_callback;
    } else if (const auto mix = g_output_mixes.find(guest_object);
               mix != g_output_mixes.end() && mix->second->object) {
        registration = &mix->second->object_callback;
    }
    if (!registration) {
        LOGE("SLObject_RegisterCallback: unknown/destroyed guest object 0x%08X",
             guest_object);
        return SL_RESULT_PRECONDITIONS_VIOLATED;
    }
    *registration = {callback, context};
    if (AudioProbeEnabled()) {
        LOGI("SLObject_RegisterCallback object=0x%08X callback=0x%08X "
             "context=0x%08X mode=synchronous",
             guest_object, callback, context);
    }
    return SL_RESULT_SUCCESS;
}

bool EnsureEngineObjectVtable(DynarmicHost& host) {
    return EnsureVtable(host, g_guest_engine_object_vtable, {
        {0, "SLObject_Realize"},
        {1, "SLObject_Resume"},
        {2, "SLObject_GetState"},
        {3, "SLEngine_GetInterface"},
        {4, "SLObject_RegisterCallback"},
        {5, "SLObject_AbortAsyncOperation"},
        {6, "SLObject_Destroy"},
    });
}

bool EnsureEngineInterfaceVtable(DynarmicHost& host) {
    return EnsureVtable(host, g_guest_engine_interface_vtable, {
        {2, "SLEngine_CreateAudioPlayer"},
        {7, "SLEngine_CreateOutputMix"},
    });
}

bool EnsureOutputMixObjectVtable(DynarmicHost& host) {
    return EnsureVtable(host, g_guest_output_mix_object_vtable, {
        {0, "SLObject_Realize"},
        {1, "SLObject_Resume"},
        {2, "SLObject_GetState"},
        {4, "SLObject_RegisterCallback"},
        {5, "SLObject_AbortAsyncOperation"},
        {6, "SLObject_Destroy"},
    });
}

bool EnsurePlayerObjectVtable(DynarmicHost& host) {
    return EnsureVtable(host, g_guest_player_object_vtable, {
        {0, "SLObject_Realize"},
        {1, "SLObject_Resume"},
        {2, "SLObject_GetState"},
        {3, "SLPlayer_GetInterface"},
        {4, "SLObject_RegisterCallback"},
        {5, "SLObject_AbortAsyncOperation"},
        {6, "SLObject_Destroy"},
    });
}

bool EnsurePlayerInterfaceVtable(DynarmicHost& host, PlayerInterfaceKind kind,
                                 uint32_t*& slot) {
    switch (kind) {
    case PlayerInterfaceKind::Play:
        slot = &g_guest_play_vtable;
        return EnsureVtable(host, *slot, {
            {0, "SLPlay_SetPlayState"},
            {1, "SLPlay_GetPlayState"},
        });
    case PlayerInterfaceKind::BufferQueue:
        slot = &g_guest_buffer_queue_vtable;
        return EnsureVtable(host, *slot, {
            {0, "SLBufferQueue_Enqueue"},
            {1, "SLBufferQueue_Clear"},
            {2, "SLBufferQueue_GetState"},
            {3, "SLBufferQueue_RegisterCallback"},
        });
    case PlayerInterfaceKind::Volume:
        slot = &g_guest_volume_vtable;
        return EnsureVtable(host, *slot, {
            {0, "SLVolume_SetVolumeLevel"},
            {2, "SLVolume_GetMaxVolumeLevel"},
        });
    case PlayerInterfaceKind::Configuration:
        slot = &g_guest_configuration_vtable;
        return EnsureVtable(host, *slot, {
            {0, "SLConfig_SetConfiguration"},
            {1, "SLConfig_GetConfiguration"},
        });
    }
    slot = nullptr;
    return false;
}

uint32_t EnsurePlayerGuestInterface(DynarmicHost& host,
                                    const std::shared_ptr<PlayerState>& player,
                                    PlayerInterfaceKind kind) {
    uint32_t* vtable = nullptr;
    if (!EnsurePlayerInterfaceVtable(host, kind, vtable) || !vtable) return 0;

    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint32_t* guest_interface = nullptr;
    switch (kind) {
    case PlayerInterfaceKind::Play: guest_interface = &player->guest_play; break;
    case PlayerInterfaceKind::BufferQueue: guest_interface = &player->guest_buffer_queue; break;
    case PlayerInterfaceKind::Volume: guest_interface = &player->guest_volume; break;
    case PlayerInterfaceKind::Configuration:
        guest_interface = &player->guest_configuration;
        break;
    }
    if (!guest_interface) return 0;
    if (*guest_interface == 0) {
        *guest_interface = AllocGuestHandle(host, *vtable);
        if (*guest_interface == 0) {
            LOGE("Unable to allocate guest OpenSL player interface");
            return 0;
        }
        RegisterPlayerInterfaceLocked(player, *guest_interface, kind);
    }
    return *guest_interface;
}

struct GuestPcmSource {
    SLDataLocator_AndroidSimpleBufferQueue locator{};
    SLDataFormat_PCM format{};
};

SLresult ParseGuestPcmSource(DynarmicHost& host, uint32_t guest_source,
                             GuestPcmSource& result) {
    if (!GuestRangeValid(host, guest_source, 8)) return SL_RESULT_PARAMETER_INVALID;
    const uint32_t guest_locator = host.MemoryRead32(guest_source);
    const uint32_t guest_format = host.MemoryRead32(guest_source + 4);
    if (!GuestRangeValid(host, guest_locator, 8) ||
        !GuestRangeValid(host, guest_format, 7 * 4)) {
        return SL_RESULT_PARAMETER_INVALID;
    }

    const SLuint32 locator_type = host.MemoryRead32(guest_locator);
    if (locator_type != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE &&
        locator_type != SL_DATALOCATOR_BUFFERQUEUE) {
        LOGE("CreateAudioPlayer: unsupported source locator 0x%08X", locator_type);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    const SLuint32 queue_count = host.MemoryRead32(guest_locator + 4);
    if (queue_count == 0) return SL_RESULT_PARAMETER_INVALID;

    result.locator.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
    // The guest streams callback-driven audio in ~7-11 s chunks and asks for a
    // single-slot queue, so every refill happens only after the last sample
    // drained - an audible hiccup every chunk while the JIT decodes the next
    // one.
    // Two mechanisms give the device decoding headroom: a deeper queue AND
    // host-side slicing of large Enqueue calls (see SLBufferQueue_Enqueue).
    // One complete sliced chunk may overlap the unplayed tail of the previous
    // chunk, so a depth equal to only kMaxPcmSlices is insufficient.
    result.locator.numBuffers = queue_count < kMinHostBufferQueueDepth
        ? kMinHostBufferQueueDepth : queue_count;
    result.format.formatType = host.MemoryRead32(guest_format + 0);
    result.format.numChannels = host.MemoryRead32(guest_format + 4);
    result.format.samplesPerSec = host.MemoryRead32(guest_format + 8);
    result.format.bitsPerSample = host.MemoryRead32(guest_format + 12);
    result.format.containerSize = host.MemoryRead32(guest_format + 16);
    result.format.channelMask = host.MemoryRead32(guest_format + 20);
    result.format.endianness = host.MemoryRead32(guest_format + 24);
    if (result.format.formatType != SL_DATAFORMAT_PCM ||
        result.format.numChannels == 0 || result.format.samplesPerSec == 0) {
        LOGE("CreateAudioPlayer: malformed/unsupported PCM format");
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    return SL_RESULT_SUCCESS;
}

SLresult ResolveGuestOutputMix(DynarmicHost& host, uint32_t guest_sink,
                               std::shared_ptr<OutputMixState>& result) {
    if (!GuestRangeValid(host, guest_sink, 8)) return SL_RESULT_PARAMETER_INVALID;
    const uint32_t guest_locator = host.MemoryRead32(guest_sink);
    if (!GuestRangeValid(host, guest_locator, 8) ||
        host.MemoryRead32(guest_locator) != SL_DATALOCATOR_OUTPUTMIX) {
        LOGE("CreateAudioPlayer: unsupported/malformed output sink");
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }

    const uint32_t guest_mix = host.MemoryRead32(guest_locator + 4);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const auto it = g_output_mixes.find(guest_mix);
    if (it == g_output_mixes.end() || !it->second->realized) {
        LOGE("CreateAudioPlayer: output mix 0x%08X is unknown or unrealized", guest_mix);
        return SL_RESULT_PRECONDITIONS_VIOLATED;
    }
    result = it->second;
    return SL_RESULT_SUCCESS;
}

SLresult RealizeGuestObject(DynarmicHost&, uint32_t guest_object,
                            SLboolean async) {
    if (async == SL_BOOLEAN_TRUE) {
        LOGE("SLObject_Realize: asynchronous realization is unsupported");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }

    if (guest_object != 0 && guest_object == g_guest_engine_object) {
        if (g_host_engine_realized) return SL_RESULT_SUCCESS;
        if (!g_host_engine_object) return SL_RESULT_PRECONDITIONS_VIOLATED;
        SLresult result = (*g_host_engine_object)->Realize(
            g_host_engine_object, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) return result;
        result = (*g_host_engine_object)->GetInterface(
            g_host_engine_object, SL_IID_ENGINE, &g_host_engine);
        if (result == SL_RESULT_SUCCESS) g_host_engine_realized = true;
        return result;
    }

    if (auto player = FindPlayerObject(guest_object)) {
        if (player->realized) return SL_RESULT_SUCCESS;
        SLresult result = (*player->object)->Realize(player->object, SL_BOOLEAN_FALSE);
        if (result != SL_RESULT_SUCCESS) return result;
        result = (*player->object)->GetInterface(player->object, SL_IID_PLAY,
                                                 &player->play);
        if (result == SL_RESULT_SUCCESS) {
            result = (*player->object)->GetInterface(
                player->object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                &player->buffer_queue);
        }
        if (result == SL_RESULT_SUCCESS) {
            result = (*player->object)->GetInterface(player->object, SL_IID_VOLUME,
                                                     &player->volume);
        }
        if (result == SL_RESULT_SUCCESS) {
            result = (*player->buffer_queue)->RegisterCallback(
                player->buffer_queue, HostBqPlayerCallback, player.get());
        }
        if (result != SL_RESULT_SUCCESS) {
            LOGE("SLObject_Realize: player interface setup failed: %u", result);
            return result;
        }
        std::lock_guard<std::mutex> lock(g_state_mutex);
        player->realized = true;
        return SL_RESULT_SUCCESS;
    }

    std::shared_ptr<OutputMixState> mix;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_output_mixes.find(guest_object);
        if (it != g_output_mixes.end()) mix = it->second;
    }
    if (mix) {
        if (mix->realized) return SL_RESULT_SUCCESS;
        const SLresult result = (*mix->object)->Realize(mix->object, SL_BOOLEAN_FALSE);
        if (result == SL_RESULT_SUCCESS) {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            mix->realized = true;
        }
        return result;
    }

    LOGE("SLObject_Realize: unknown guest object 0x%08X", guest_object);
    return SL_RESULT_PRECONDITIONS_VIOLATED;
}

void ZeroGuestPlayerHandles(DynarmicHost& host, const PlayerState& player) {
    const uint32_t handles[] = {player.guest_object, player.guest_play,
        player.guest_buffer_queue, player.guest_volume, player.guest_configuration};
    for (uint32_t handle : handles) {
        if (GuestRangeValid(host, handle, 4)) host.MemoryWrite32(handle, 0);
    }
}

void DestroyGuestObject(DynarmicHost& host, uint32_t guest_object,
                        uint32_t caller) {
    if (AudioProbeEnabled()) {
        LOGI("OpenSL destroy object=0x%08X engine=0x%08X caller=0x%08X",
             guest_object, g_guest_engine_object, caller);
    }
    if (guest_object != 0 && guest_object == g_guest_engine_object) {
        std::unique_lock<std::mutex> engine_lock(g_engine_lifecycle_mutex);
        if (g_guest_engine_references == 0) {
            LOGE("OpenSL engine release without an acquired reference");
            return;
        }
        --g_guest_engine_references;
        LOGI("OpenSL engine release guest=0x%08X refs=%u caller=0x%08X",
             guest_object, g_guest_engine_references, caller);
        if (g_guest_engine_references != 0) return;
        StopAudioThread();
        std::vector<std::shared_ptr<PlayerState>> players;
        std::vector<std::shared_ptr<OutputMixState>> mixes;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_engine_object_callback_live = false;
            g_engine_object_callback = {};
            for (const auto& entry : g_players_by_object) {
                entry.second->alive.store(false, std::memory_order_release);
                entry.second->guest_callback.store(0, std::memory_order_release);
                entry.second->object_callback = {};
                players.push_back(entry.second);
            }
            for (const auto& entry : g_output_mixes) {
                entry.second->object_callback = {};
                mixes.push_back(entry.second);
            }
            g_players_by_object.clear();
            g_players_by_interface.clear();
            g_output_mixes.clear();
        }
        for (const auto& player : players) {
            LogPlayerLifecycle(*player, "engine_destroy", caller,
                               player->requested_play_state,
                               player->requested_play_state);
            if (player->object) (*player->object)->Destroy(player->object);
            ZeroGuestPlayerHandles(host, *player);
            player->queued_pcm.clear();
        }
        for (const auto& mix : mixes) {
            if (mix->object) (*mix->object)->Destroy(mix->object);
            if (GuestRangeValid(host, mix->guest_object, 4)) {
                host.MemoryWrite32(mix->guest_object, 0);
            }
        }
        if (g_host_engine_object) (*g_host_engine_object)->Destroy(g_host_engine_object);
        if (GuestRangeValid(host, g_guest_engine_object, 4)) {
            host.MemoryWrite32(g_guest_engine_object, 0);
        }
        if (GuestRangeValid(host, g_guest_engine_interface, 4)) {
            host.MemoryWrite32(g_guest_engine_interface, 0);
        }
        g_host_engine_object = nullptr;
        g_host_engine = nullptr;
        g_host_engine_realized = false;
        g_guest_engine_object = 0;
        g_guest_engine_interface = 0;
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_retired_players.clear();
        return;
    }

    std::shared_ptr<PlayerState> player;
    bool no_players_remain = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_players_by_object.find(guest_object);
        if (it != g_players_by_object.end()) {
            player = it->second;
            ReclaimCompletedBuffersLocked(*player);
            LogPlayerLifecycle(*player, "destroy", caller,
                               player->requested_play_state,
                               player->requested_play_state);
            player->alive.store(false, std::memory_order_release);
            player->guest_callback.store(0, std::memory_order_release);
            player->object_callback = {};
            const uint32_t interfaces[] = {player->guest_play, player->guest_buffer_queue,
                player->guest_volume, player->guest_configuration};
            for (uint32_t guest_interface : interfaces) {
                if (guest_interface) g_players_by_interface.erase(guest_interface);
            }
            g_players_by_object.erase(it);
            g_retired_players.push_back(player);
            no_players_remain = g_players_by_object.empty();
        }
    }
    if (player) {
        if (player->object) (*player->object)->Destroy(player->object);
        ZeroGuestPlayerHandles(host, *player);
        player->queued_pcm.clear();
        player->object = nullptr;
        player->play = nullptr;
        player->buffer_queue = nullptr;
        player->volume = nullptr;
        player->configuration = nullptr;
        if (no_players_remain) StopAudioThread();
        return;
    }

    std::shared_ptr<OutputMixState> mix;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_output_mixes.find(guest_object);
        if (it != g_output_mixes.end()) {
            mix = it->second;
            mix->object_callback = {};
            g_output_mixes.erase(it);
        }
    }
    if (mix) {
        if (mix->object) (*mix->object)->Destroy(mix->object);
        if (GuestRangeValid(host, mix->guest_object, 4)) {
            host.MemoryWrite32(mix->guest_object, 0);
        }
        return;
    }

    LOGE("SLObject_Destroy: unknown guest object 0x%08X", guest_object);
}

} // namespace

bool HasPendingAudioCallbacks() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    for (const auto& entry : g_players_by_object) {
        const auto& player = entry.second;
        if (player->alive.load(std::memory_order_acquire) &&
            player->guest_callback.load(std::memory_order_acquire) != 0 &&
            (player->completed_buffers.load(std::memory_order_acquire) != 0 ||
             player->forwardable_completions != 0)) {
            return true;
        }
    }
    return false;
}

void PumpOpenSLESCallbacks(DynarmicHost& host) {
    if (g_audio_paused.load(std::memory_order_acquire)) return;

    // Serialize re-entry on this dispatcher thread's own A32 context. Other
    // guest pthreads deliberately use independent cores and can keep running.
    std::lock_guard<std::recursive_mutex> host_lock(host.GetMutex());
    if (host.IsJitRunning()) return;

    struct Invocation {
        std::shared_ptr<PlayerState> player;
        uint32_t callback;
        uint32_t queue_interface;
        uint32_t context;
        uint32_t count;
    };
    std::vector<Invocation> invocations;

    {
        std::lock_guard<std::mutex> state_lock(g_state_mutex);
        invocations.reserve(g_players_by_object.size());
        for (const auto& entry : g_players_by_object) {
            const auto& player = entry.second;
            ReclaimCompletedBuffersLocked(*player);
            // PLAYING with a drained queue is the exact signature of the cyclic
            // "stop output session / start output session" seen in AudioFlinger:
            // the track stops on underrun and restarts at the next Enqueue. Log
            // it rate-limited so a starved pump/mixer is diagnosable on device.
            if (player->realized &&
                player->requested_play_state == SL_PLAYSTATE_PLAYING &&
                player->guest_callback.load(std::memory_order_acquire) != 0 &&
                player->queued_pcm.empty() &&
                player->completed_buffers.load(std::memory_order_acquire) == 0) {
                const int64_t now = SteadyNowMs();
                int64_t last = player->last_underrun_log_ms.load(
                    std::memory_order_relaxed);
                if (now - last >= 2000 &&
                    player->last_underrun_log_ms.compare_exchange_strong(last, now)) {
                    LOGI("OpenSL underrun: player 0x%08X PLAYING with empty buffer queue",
                         player->guest_object);
                }
            }
            uint32_t pending = player->forwardable_completions;
            player->forwardable_completions = 0;
            const uint32_t callback = player->guest_callback.load(
                std::memory_order_acquire);
            if (pending != 0 && callback != 0 && player->guest_buffer_queue != 0 &&
                player->alive.load(std::memory_order_acquire)) {
                invocations.push_back(Invocation{
                    player,
                    callback,
                    player->guest_buffer_queue,
                    player->guest_callback_context.load(std::memory_order_acquire),
                    pending});
            }
        }
    }

    for (const Invocation& invocation : invocations) {
        for (uint32_t i = 0; i < invocation.count; ++i) {
            if (!invocation.player->alive.load(std::memory_order_acquire)) break;
            host.RunFunction(invocation.callback,
                             {invocation.queue_interface, invocation.context});
        }
    }
}

static bool AudioThreadActive(uint64_t generation) {
    return g_audio_thread_running.load(std::memory_order_acquire) &&
           g_audio_thread_generation.load(std::memory_order_acquire) == generation;
}

static void AudioThreadLoop(DynarmicHost* host, uint64_t generation) {
    LOGI("OpenSL guest callback dispatcher started (non-real-time)");
#if defined(__ANDROID__)
    // Every voice's requeue callback is relayed through this thread; at default
    // nice it competes with render/streaming guest pthreads and its scheduling
    // delay lands directly in the audio buffer queue as underruns. Best-effort:
    // an unprivileged negative nice is refused and the 2 ms tick still bounds
    // the relay latency.
    if (setpriority(PRIO_PROCESS, 0, -16) != 0) {
        static std::atomic<bool> priority_warning_logged{false};
        if (!priority_warning_logged.exchange(true)) {
            LOGI("setpriority(-16) unavailable for audio dispatcher (errno %d)", errno);
        }
    }
#endif
    while (AudioThreadActive(generation)) {
        {
            std::unique_lock<std::mutex> lock(g_audio_cv_mutex);
            // The notify in the real-time callback can land between the predicate
            // check and the sleep (it must not take this mutex), so the timeout is
            // the fallback bound on relay latency, not the primary wake path.
            // HAL buffer chunks are ~10 ms of audio; 5 ms of worst-case added
            // latency left too little headroom under load and cycled the output
            // track through stop/start.
            g_audio_cv.wait_for(lock, std::chrono::milliseconds(2), [generation]() {
                return !AudioThreadActive(generation) ||
                       (!g_audio_paused.load(std::memory_order_acquire) &&
                        HasPendingAudioCallbacks());
            });
        }

        if (!AudioThreadActive(generation)) break;
        if (!host || g_audio_paused.load(std::memory_order_acquire) ||
            !HasPendingAudioCallbacks()) {
            continue;
        }

        // try_lock avoids self-teardown surprises if a callback destroys the
        // final player while this dispatcher context is being pumped.
        std::unique_lock<std::recursive_mutex> host_lock(
            host->GetMutex(), std::try_to_lock);
        if (host_lock.owns_lock() && !host->IsJitRunning()) {
            PumpOpenSLESCallbacks(*host);
        } else {
            std::unique_lock<std::mutex> wait_lock(g_audio_cv_mutex);
            g_audio_cv.wait_for(wait_lock, std::chrono::milliseconds(1));
        }
    }

    if (host) host->ReleaseCurrentThreadContext();
    LOGI("OpenSL guest callback dispatcher stopped");
}

void StartAudioThread(DynarmicHost* host) {
    if (!host) return;

    struct ShutdownGuard {
        ~ShutdownGuard() { StopAudioThread(); }
    };
    static ShutdownGuard shutdown_guard;
    (void)shutdown_guard;

    std::lock_guard<std::mutex> lock(g_audio_thread_mutex);
    if (g_audio_thread_running.load(std::memory_order_acquire)) return;
    if (g_audio_thread && g_audio_thread->joinable()) g_audio_thread->join();

    const uint64_t generation = g_audio_thread_generation.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    g_audio_thread_running.store(true, std::memory_order_release);
    g_audio_thread = std::make_unique<std::thread>(
        AudioThreadLoop, host, generation);
}

void StopAudioThread() {
    std::unique_ptr<std::thread> thread;
    {
        std::lock_guard<std::mutex> lock(g_audio_thread_mutex);
        if (!g_audio_thread) return;
        g_audio_thread_running.store(false, std::memory_order_release);
        g_audio_thread_generation.fetch_add(1, std::memory_order_acq_rel);
        g_audio_cv.notify_all();
        thread = std::move(g_audio_thread);
    }
    if (thread->joinable()) {
        if (thread->get_id() == std::this_thread::get_id()) {
            // A guest buffer-queue callback is allowed to destroy its own
            // player. The dispatcher will observe running=false after the
            // callback returns; joining the current thread would deadlock.
            thread->detach();
        } else {
            thread->join();
        }
    }
}

void PauseAudioPlayback() {
    LOGI("PauseAudioPlayback requested");
    g_audio_paused.store(true, std::memory_order_release);
    // g_state_mutex is held across the host calls: SetPlayState never re-enters
    // guest code or our thunks, and teardown needs the same lock, so a
    // concurrent SLObject_Destroy cannot free an SLPlayItf mid-call (the old
    // snapshot-then-call version raced exactly there).
    std::lock_guard<std::mutex> lock(g_state_mutex);
    for (const auto& entry : g_players_by_object) {
        const auto& player = entry.second;
        if (player->alive.load(std::memory_order_acquire) && player->play) {
            (*player->play)->SetPlayState(player->play, SL_PLAYSTATE_PAUSED);
        }
    }
}

void ResumeAudioPlayback() {
    LOGI("ResumeAudioPlayback requested");
    // Clear the flag BEFORE restoring. While the flag is set, guest-initiated
    // SLPlay_SetPlayState(PLAYING) is mapped to PAUSED; restoring states first
    // would leave that mapping armed during the window and could park a player
    // forever with requested=PLAYING, i.e. the "resume lost" symptom.
    g_audio_paused.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (const auto& entry : g_players_by_object) {
            const auto& player = entry.second;
            if (!player->alive.load(std::memory_order_acquire) || !player->play) continue;
            const SLresult result = (*player->play)->SetPlayState(
                player->play, player->requested_play_state);
            if (result != SL_RESULT_SUCCESS) {
                LOGE("ResumeAudioPlayback: player 0x%08X restore failed: %u",
                     player->guest_object, result);
            }
        }
    }
    g_audio_cv.notify_all();
}

uint32_t GetOpenSLESInterfacePointer(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_iid_pointer_map.find(name);
    if (it != g_iid_pointer_map.end()) {
        return it->second;
    }
    return 0;
}

void RegisterOpenSLESThunks(ThunkManager& manager, DynarmicHost& host) {
    LOGI("Registering OpenSL ES guest bridge thunks...");

    // Register all standard OpenSL ES interface IDs
    AllocAndRegisterIID(manager, host, "SL_IID_ENGINE", SL_IID_ENGINE);
    AllocAndRegisterIID(manager, host, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
    AllocAndRegisterIID(manager, host, "SL_IID_ANDROIDCONFIGURATION", SL_IID_ANDROIDCONFIGURATION);
    AllocAndRegisterIID(manager, host, "SL_IID_PLAY", SL_IID_PLAY);
    AllocAndRegisterIID(manager, host, "SL_IID_VOLUME", SL_IID_VOLUME);
    AllocAndRegisterIID(manager, host, "SL_IID_OUTPUTMIX", SL_IID_OUTPUTMIX);
    AllocAndRegisterIID(manager, host, "SL_IID_BUFFERQUEUE", SL_IID_BUFFERQUEUE);
    AllocAndRegisterIID(manager, host, "SL_IID_RECORD", SL_IID_RECORD);

    manager.RegisterThunk("SLOpenSL_UnsupportedMethod",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            (void)host;
            LOGE("Unsupported OpenSL ES method on guest interface 0x%08X",
                 jit.Regs()[0]);
            jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
        });

    manager.RegisterThunk("slCreateEngine", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        const uint32_t output_engine = jit.Regs()[0];
        const SLuint32 num_interfaces = jit.Regs()[3];
        const uint32_t stack = jit.Regs()[13];
        const uint32_t guest_ids = host.MemoryRead32(stack);
        const uint32_t guest_required = host.MemoryRead32(stack + 4);
        if (!GuestRangeValid(host, output_engine, 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        SLresult result = ValidateRequestedInterfaces(
            host, "slCreateEngine", num_interfaces, guest_ids, guest_required,
            {SL_IID_ENGINE});
        if (result != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = result;
            return;
        }

        std::lock_guard<std::mutex> engine_lock(g_engine_lifecycle_mutex);
        if (g_guest_engine_references == UINT32_MAX) {
            jit.Regs()[0] = SL_RESULT_RESOURCE_ERROR;
            return;
        }
        if (!g_host_engine_object) {
            result = slCreateEngine(&g_host_engine_object, 0, nullptr, 0,
                                    nullptr, nullptr);
            if (result != SL_RESULT_SUCCESS) {
                LOGE("Host slCreateEngine failed: %u", result);
                jit.Regs()[0] = result;
                return;
            }
        }
        if (!EnsureEngineObjectVtable(host)) {
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        if (!g_guest_engine_object) {
            g_guest_engine_object = AllocGuestHandle(
                host, g_guest_engine_object_vtable);
        }
        if (!g_guest_engine_object) {
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        host.MemoryWrite32(output_engine, g_guest_engine_object);
        ++g_guest_engine_references;
        {
            std::lock_guard<std::mutex> state_lock(g_state_mutex);
            g_engine_object_callback_live = true;
        }
        LOGI("OpenSL engine acquire guest=0x%08X refs=%u output=0x%08X caller=0x%08X",
             g_guest_engine_object, g_guest_engine_references,
             output_engine, jit.Regs()[14]);
        // Everything between this point and the first player creation is silent
        // by design; this marker is the anchor that proves the guest engine
        // handshake (dlsym -> slCreateEngine -> Realize) completed on device.
        static std::atomic<bool> engine_created_logged{false};
        if (!engine_created_logged.exchange(true)) {
            LOGI("Created OpenSL engine guest=0x%08X", g_guest_engine_object);
        }
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLObject_Realize", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = RealizeGuestObject(
            host, jit.Regs()[0], static_cast<SLboolean>(jit.Regs()[1]));
    });

    manager.RegisterThunk("SLObject_Resume", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        if (jit.Regs()[1] != SL_BOOLEAN_FALSE) {
            LOGE("SLObject_Resume: asynchronous resume is unsupported");
            jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
            return;
        }
        SLObjectItf object = FindHostObject(jit.Regs()[0]);
        if (!object) {
            LOGE("SLObject_Resume: unknown guest object 0x%08X", jit.Regs()[0]);
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        jit.Regs()[0] = (*object)->Resume(
            object, static_cast<SLboolean>(jit.Regs()[1]));
    });

    manager.RegisterThunk("SLObject_GetState", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t output_state = jit.Regs()[1];
        SLObjectItf object = FindHostObject(jit.Regs()[0]);
        if (!object || !GuestRangeValid(host, output_state, 4)) {
            LOGE("SLObject_GetState: invalid object/output pointer");
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        SLuint32 state = SL_OBJECT_STATE_UNREALIZED;
        const SLresult result = (*object)->GetState(object, &state);
        if (result == SL_RESULT_SUCCESS) host.MemoryWrite32(output_state, state);
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLObject_RegisterCallback", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = RegisterGuestObjectCallback(
            jit.Regs()[0], jit.Regs()[1], jit.Regs()[2]);
    });

    manager.RegisterThunk("SLObject_AbortAsyncOperation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        (void)host;
        LOGE("SLObject_AbortAsyncOperation is unsupported for guest object 0x%08X",
             jit.Regs()[0]);
    });

    manager.RegisterThunk("SLObject_Destroy", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        DestroyGuestObject(host, jit.Regs()[0], jit.Regs()[14]);
    });

    manager.RegisterThunk("SLEngine_GetInterface",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t self = jit.Regs()[0];
        const uint32_t iid = jit.Regs()[1];
        const uint32_t output_interface = jit.Regs()[2];
        if (self != g_guest_engine_object || !g_host_engine_realized ||
            !GuestRangeValid(host, output_interface, 4)) {
            LOGE("SLEngine_GetInterface: invalid/unrealized engine 0x%08X", self);
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        if (!IID_Matches(host, iid, SL_IID_ENGINE)) {
            LOGE("SLEngine_GetInterface: unsupported IID 0x%08X", iid);
            jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
            return;
        }
        if (!EnsureEngineInterfaceVtable(host)) {
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        if (!g_guest_engine_interface) {
            g_guest_engine_interface = AllocGuestHandle(
                host, g_guest_engine_interface_vtable);
        }
        if (!g_guest_engine_interface) {
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        host.MemoryWrite32(output_interface, g_guest_engine_interface);
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLEngine_CreateOutputMix",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t self = jit.Regs()[0];
        const uint32_t output_mix = jit.Regs()[1];
        const SLuint32 num_interfaces = jit.Regs()[2];
        const uint32_t guest_ids = jit.Regs()[3];
        const uint32_t guest_required = host.MemoryRead32(jit.Regs()[13]);
        if (self != g_guest_engine_interface || !g_host_engine ||
            !GuestRangeValid(host, output_mix, 4)) {
            LOGE("SLEngine_CreateOutputMix: invalid engine/output pointer");
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        SLresult result = ValidateRequestedInterfaces(
            host, "SLEngine_CreateOutputMix", num_interfaces, guest_ids,
            guest_required, {});
        if (result != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = result;
            return;
        }

        auto mix = std::make_shared<OutputMixState>();
        result = (*g_host_engine)->CreateOutputMix(
            g_host_engine, &mix->object, 0, nullptr, nullptr);
        if (result != SL_RESULT_SUCCESS) {
            LOGE("Host CreateOutputMix failed: %u", result);
            jit.Regs()[0] = result;
            return;
        }
        if (!EnsureOutputMixObjectVtable(host)) {
            (*mix->object)->Destroy(mix->object);
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        mix->guest_object = AllocGuestHandle(
            host, g_guest_output_mix_object_vtable);
        if (!mix->guest_object) {
            (*mix->object)->Destroy(mix->object);
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_output_mixes.emplace(mix->guest_object, mix);
        }
        host.MemoryWrite32(output_mix, mix->guest_object);
        static std::atomic<bool> mix_created_logged{false};
        if (!mix_created_logged.exchange(true)) {
            LOGI("Created OpenSL output mix guest=0x%08X", mix->guest_object);
        }
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLEngine_CreateAudioPlayer",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t self = jit.Regs()[0];
        const uint32_t output_player = jit.Regs()[1];
        const uint32_t guest_source = jit.Regs()[2];
        const uint32_t guest_sink = jit.Regs()[3];
        const uint32_t stack = jit.Regs()[13];
        const SLuint32 num_interfaces = host.MemoryRead32(stack);
        const uint32_t guest_ids = host.MemoryRead32(stack + 4);
        const uint32_t guest_required = host.MemoryRead32(stack + 8);
        if (self != g_guest_engine_interface || !g_host_engine ||
            !GuestRangeValid(host, output_player, 4)) {
            LOGE("SLEngine_CreateAudioPlayer: invalid engine/output pointer");
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }

        SLresult result = ValidateRequestedInterfaces(
            host, "SLEngine_CreateAudioPlayer", num_interfaces, guest_ids,
            guest_required, {SL_IID_PLAY, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                             SL_IID_BUFFERQUEUE, SL_IID_VOLUME,
                             SL_IID_ANDROIDCONFIGURATION});
        if (result != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = result;
            return;
        }

        GuestPcmSource source;
        result = ParseGuestPcmSource(host, guest_source, source);
        if (result != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = result;
            return;
        }
        std::shared_ptr<OutputMixState> mix;
        result = ResolveGuestOutputMix(host, guest_sink, mix);
        if (result != SL_RESULT_SUCCESS) {
            jit.Regs()[0] = result;
            return;
        }

        SLDataSource host_source = {&source.locator, &source.format};
        SLDataLocator_OutputMix output_locator = {
            SL_DATALOCATOR_OUTPUTMIX, mix->object};
        SLDataSink host_sink = {&output_locator, nullptr};
        const SLInterfaceID host_ids[] = {
            SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
            SL_IID_VOLUME,
            SL_IID_ANDROIDCONFIGURATION,
        };
        const SLboolean host_required[] = {
            SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
        };

        auto player = std::make_shared<PlayerState>();
        player->channels = source.format.numChannels;
        player->sample_rate_millihz = source.format.samplesPerSec;
        player->bits_per_sample = source.format.bitsPerSample;
        player->container_bits = source.format.containerSize;
        player->pcm_endianness = source.format.endianness;
        player->serial = g_next_player_serial.fetch_add(
            1, std::memory_order_relaxed);
        player->create_caller = jit.Regs()[14];
        player->created_ms = SteadyNowMs();
        result = (*g_host_engine)->CreateAudioPlayer(
            g_host_engine, &player->object, &host_source, &host_sink,
            static_cast<SLuint32>(sizeof(host_ids) / sizeof(host_ids[0])),
            host_ids, host_required);
        if (result != SL_RESULT_SUCCESS) {
            LOGE("Host CreateAudioPlayer failed: %u", result);
            jit.Regs()[0] = result;
            return;
        }

        result = (*player->object)->GetInterface(
            player->object, SL_IID_ANDROIDCONFIGURATION,
            &player->configuration);
        if (result == SL_RESULT_SUCCESS) {
            SLint32 stream_type = SL_ANDROID_STREAM_MEDIA;
            result = (*player->configuration)->SetConfiguration(
                player->configuration, SL_ANDROID_KEY_STREAM_TYPE,
                &stream_type, sizeof(stream_type));
        }
        if (result != SL_RESULT_SUCCESS) {
            LOGE("Host player configuration setup failed: %u", result);
            (*player->object)->Destroy(player->object);
            jit.Regs()[0] = result;
            return;
        }

        if (!EnsurePlayerObjectVtable(host)) {
            (*player->object)->Destroy(player->object);
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        player->guest_object = AllocGuestHandle(
            host, g_guest_player_object_vtable);
        if (!player->guest_object) {
            (*player->object)->Destroy(player->object);
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        size_t active_players = 0;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_players_by_object.emplace(player->guest_object, player);
            active_players = g_players_by_object.size();
        }
        host.MemoryWrite32(output_player, player->guest_object);
        LOGI("Created OpenSL player guest=0x%08X channels=%u rate=%u buffers=%u",
             player->guest_object, source.format.numChannels,
             source.format.samplesPerSec, source.locator.numBuffers);
        if (AudioProbeEnabled()) {
            LOGI("audio probe serial=%llu player=0x%08X created caller=0x%08X "
                 "active_players=%zu bits=%u guest_buffers=%u host_buffers=%u",
                 static_cast<unsigned long long>(player->serial),
                 player->guest_object, player->create_caller, active_players,
                 source.format.bitsPerSample,
                 host.MemoryRead32(host.MemoryRead32(guest_source) + 4),
                 source.locator.numBuffers);
        }
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLPlayer_GetInterface",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint32_t self = jit.Regs()[0];
        const uint32_t iid = jit.Regs()[1];
        const uint32_t output_interface = jit.Regs()[2];
        auto player = FindPlayerObject(self);
        if (!player || !GuestRangeValid(host, output_interface, 4)) {
            LOGE("SLPlayer_GetInterface: invalid player/output pointer");
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }

        PlayerInterfaceKind kind;
        bool supported = true;
        bool available = false;
        if (IID_Matches(host, iid, SL_IID_PLAY)) {
            kind = PlayerInterfaceKind::Play;
            available = player->realized && player->play;
        } else if (IID_Matches(host, iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) ||
                   IID_Matches(host, iid, SL_IID_BUFFERQUEUE)) {
            kind = PlayerInterfaceKind::BufferQueue;
            available = player->realized && player->buffer_queue;
        } else if (IID_Matches(host, iid, SL_IID_VOLUME)) {
            kind = PlayerInterfaceKind::Volume;
            available = player->realized && player->volume;
        } else if (IID_Matches(host, iid, SL_IID_ANDROIDCONFIGURATION)) {
            kind = PlayerInterfaceKind::Configuration;
            // Android configuration is intentionally available before Realize.
            available = player->configuration != nullptr;
        } else {
            supported = false;
            kind = PlayerInterfaceKind::Play;
        }

        if (!supported) {
            LOGE("SLPlayer_GetInterface: unsupported IID 0x%08X", iid);
            jit.Regs()[0] = SL_RESULT_FEATURE_UNSUPPORTED;
            return;
        }
        if (!available) {
            LOGE("SLPlayer_GetInterface: IID 0x%08X requested before available", iid);
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        const uint32_t guest_interface = EnsurePlayerGuestInterface(
            host, player, kind);
        if (!guest_interface) {
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        host.MemoryWrite32(output_interface, guest_interface);
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLConfig_SetConfiguration", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto player = FindPlayerInterface(jit.Regs()[0],
            PlayerInterfaceKind::Configuration, "SLConfig_SetConfiguration");
        const uint32_t key = jit.Regs()[1];
        const uint32_t value = jit.Regs()[2];
        const SLuint32 value_size = jit.Regs()[3];
        if (!player || !player->configuration || !GuestRangeValid(host, key, 1) ||
            (value_size != 0 && !GuestRangeValid(host, value, value_size))) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        jit.Regs()[0] = (*player->configuration)->SetConfiguration(
            player->configuration,
            reinterpret_cast<const SLchar*>(GuestToHost(host.GetGuestMemoryBase(), key)),
            GuestToHost(host.GetGuestMemoryBase(), value), value_size);
    });

    manager.RegisterThunk("SLConfig_GetConfiguration", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto player = FindPlayerInterface(jit.Regs()[0],
            PlayerInterfaceKind::Configuration, "SLConfig_GetConfiguration");
        const uint32_t key = jit.Regs()[1];
        const uint32_t value_size = jit.Regs()[2];
        const uint32_t value = jit.Regs()[3];
        if (!player || !player->configuration || !GuestRangeValid(host, key, 1) ||
            !GuestRangeValid(host, value_size, 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        jit.Regs()[0] = (*player->configuration)->GetConfiguration(
            player->configuration,
            reinterpret_cast<const SLchar*>(GuestToHost(host.GetGuestMemoryBase(), key)),
            reinterpret_cast<SLuint32*>(GuestToHost(host.GetGuestMemoryBase(), value_size)),
            GuestToHost(host.GetGuestMemoryBase(), value));
    });

    manager.RegisterThunk("SLPlay_SetPlayState", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        auto player = FindPlayerInterface(jit.Regs()[0], PlayerInterfaceKind::Play,
                                          "SLPlay_SetPlayState");
        if (!player || !player->play) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        const SLuint32 requested = jit.Regs()[1];
        const uint32_t caller = jit.Regs()[14];
        const SLuint32 host_state = g_audio_paused.load(std::memory_order_acquire) &&
            requested == SL_PLAYSTATE_PLAYING ? SL_PLAYSTATE_PAUSED : requested;
        const SLresult result = (*player->play)->SetPlayState(
            player->play, host_state);
        if (result == SL_RESULT_SUCCESS) {
            const bool held_paused = host_state != requested;
            std::lock_guard<std::mutex> lock(g_state_mutex);
            player->requested_play_state = requested;
            if (requested == SL_PLAYSTATE_PLAYING) {
                player->natural_stop_reported = false;
            }
            // One player per voice: log only real transitions (time-ratelimited),
            // plus every pause-held mapping, which is rare and always relevant.
            const SLuint32 previous = player->last_logged_play_state.exchange(
                requested);
            const int64_t now = SteadyNowMs();
            if (requested == SL_PLAYSTATE_PLAYING && player->first_play_ms == 0) {
                player->first_play_ms = now;
            }
            const int64_t last_ms = player->last_state_log_ms.exchange(now);
            if (AudioProbeEnabled() && previous != requested) {
                LogPlayerLifecycle(*player, "set_state", caller, requested,
                                   host_state);
            } else if (held_paused ||
                (previous != requested && now - last_ms >= 200)) {
                LOGI("player 0x%08X SetPlayState %u -> %u%s",
                     player->guest_object, requested, host_state,
                     held_paused ? " (held PAUSED until resume)" : "");
            }
        }
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLPlay_GetPlayState", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto player = FindPlayerInterface(jit.Regs()[0], PlayerInterfaceKind::Play,
                                          "SLPlay_GetPlayState");
        const uint32_t output_state = jit.Regs()[1];
        if (!player || !player->play || !GuestRangeValid(host, output_state, 4)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        SLuint32 state = SL_PLAYSTATE_STOPPED;
        const SLresult result = (*player->play)->GetPlayState(player->play, &state);
        bool synthesized_natural_stop = false;
        uint64_t completed_for_log = 0;
        if (result == SL_RESULT_SUCCESS) {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            ReclaimCompletedBuffersLocked(*player);

            // Android buffer-queue players can remain logically PLAYING after
            // the final buffer has drained. Horn's UE3 OpenSL backend instead
            // treats GetPlayState()==STOPPED as the completion signal for every
            // non-looping FSoundSource. Preserve that legacy contract using the
            // host callback we already receive for each consumed buffer. If we
            // return PLAYING forever, finished one-shots occupy the 16-source
            // pool until stolen and later cutscene voices are dropped.
            const bool has_guest_requeue_callback =
                player->guest_callback.load(std::memory_order_acquire) != 0;
            const bool queue_fully_consumed =
                player->submitted_host_buffers != 0 &&
                player->completed_host_buffers >= player->submitted_host_buffers &&
                player->queued_pcm.empty();
            if (!has_guest_requeue_callback &&
                player->requested_play_state == SL_PLAYSTATE_PLAYING &&
                state == SL_PLAYSTATE_PLAYING && queue_fully_consumed) {
                state = SL_PLAYSTATE_STOPPED;
                synthesized_natural_stop = !player->natural_stop_reported;
                player->natural_stop_reported = true;
                completed_for_log = player->completed_host_buffers;
                LogPlayerLifecycle(*player, "natural_stop", jit.Regs()[14],
                                   state, state);
            }
            host.MemoryWrite32(output_state, state);
        }
        if (synthesized_natural_stop) {
            LOGI("OpenSL one-shot complete: player 0x%08X reported STOPPED "
                 "after %llu host buffer(s)", player->guest_object,
                 static_cast<unsigned long long>(completed_for_log));
        }
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLBufferQueue_RegisterCallback", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto player = FindPlayerInterface(jit.Regs()[0],
            PlayerInterfaceKind::BufferQueue, "SLBufferQueue_RegisterCallback");
        if (!player || !player->buffer_queue) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        const uint32_t callback = jit.Regs()[1];
        const uint32_t context = jit.Regs()[2];
        player->guest_callback_context.store(context, std::memory_order_release);
        player->guest_callback.store(callback, std::memory_order_release);
        LOGI("SLBufferQueue_RegisterCallback player=0x%08X callback=0x%08X context=0x%08X",
             player->guest_object, callback, context);
        if (callback) StartAudioThread(&host);
        jit.Regs()[0] = SL_RESULT_SUCCESS;
    });

    manager.RegisterThunk("SLBufferQueue_Clear", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        auto player = FindPlayerInterface(jit.Regs()[0],
            PlayerInterfaceKind::BufferQueue, "SLBufferQueue_Clear");
        if (!player || !player->buffer_queue) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        const SLresult result = (*player->buffer_queue)->Clear(player->buffer_queue);
        if (result == SL_RESULT_SUCCESS) {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            LogPlayerLifecycle(*player, "clear", jit.Regs()[14],
                               player->requested_play_state,
                               player->requested_play_state);
            ++player->queue_generation;
            player->queued_pcm.clear();
            player->completed_buffers.store(0, std::memory_order_release);
            player->forwardable_completions = 0;
            player->submitted_host_buffers = 0;
            player->completed_host_buffers = 0;
            player->enqueue_count = 0;
            player->natural_stop_reported = false;
            player->pcm_probe_logged.store(false, std::memory_order_release);
            player->first_enqueue_caller = 0;
            player->first_pcm_size = 0;
            player->first_pcm_hash = 0;
            player->total_pcm_bytes = 0;
            player->first_enqueue_ms = 0;
            player->first_play_ms = 0;
        }
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLBufferQueue_GetState", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto player = FindPlayerInterface(jit.Regs()[0],
            PlayerInterfaceKind::BufferQueue, "SLBufferQueue_GetState");
        const uint32_t output_state = jit.Regs()[1];
        if (!player || !player->buffer_queue ||
            !GuestRangeValid(host, output_state, 8)) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }
        SLAndroidSimpleBufferQueueState state = {0, 0};
        const SLresult result = (*player->buffer_queue)->GetState(
            player->buffer_queue, &state);
        if (result == SL_RESULT_SUCCESS) {
            host.MemoryWrite32(output_state, state.count);
            host.MemoryWrite32(output_state + 4, state.index);
        }
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLBufferQueue_Enqueue", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto player = FindPlayerInterface(jit.Regs()[0],
            PlayerInterfaceKind::BufferQueue, "SLBufferQueue_Enqueue");
        const uint32_t guest_buffer = jit.Regs()[1];
        const uint32_t size = jit.Regs()[2];
        if (!player || !player->buffer_queue || size == 0 ||
            size > kMaxPcmBufferBytes || !GuestRangeValid(host, guest_buffer, size)) {
            LOGE("SLBufferQueue_Enqueue: invalid buffer 0x%08X size=%u",
                 guest_buffer, size);
            jit.Regs()[0] = size > kMaxPcmBufferBytes ?
                SL_RESULT_MEMORY_FAILURE : SL_RESULT_PARAMETER_INVALID;
            return;
        }

        std::shared_ptr<std::vector<uint8_t>> owned_pcm;
        try {
            owned_pcm = std::make_shared<std::vector<uint8_t>>(size);
        } catch (const std::bad_alloc&) {
            LOGE("SLBufferQueue_Enqueue: unable to own %u PCM bytes", size);
            jit.Regs()[0] = SL_RESULT_MEMORY_FAILURE;
            return;
        }
        std::memcpy(owned_pcm->data(),
                    GuestToHost(host.GetGuestMemoryBase(), guest_buffer), size);
        MeterLivePcm(*player, owned_pcm->data(), size);
        const uint32_t caller = jit.Regs()[14];
        const bool first_pcm = !player->pcm_probe_logged.exchange(
            true, std::memory_order_acq_rel);
        const bool inspect_pcm = first_pcm &&
            (AudioProbeEnabled() || AudioDumpEnabled());
        const uint64_t pcm_hash = inspect_pcm
            ? HashPcm(owned_pcm->data(), size) : 0;
        const auto probe_pcm = first_pcm ? owned_pcm : nullptr;
        SLresult result;
        // Loop/requeue players register a completion callback and often hand
        // over ~0.6-1 MiB mono / ~2 MiB stereo buffers. Detecting them by the
        // callback instead of a magic byte threshold leaves long one-shots
        // (including dialogue) untouched.
        // Sliced enqueues make buffer completions arrive while later slices
        // still play, so the guest decodes the next chunk before the current
        // one drains instead of after - removing the periodic audible gap.
        uint32_t slices = 1u;
        const bool callback_driven_stream =
            player->guest_callback.load(std::memory_order_acquire) != 0;
        const uint32_t frame_bytes = PcmFrameBytes(*player);
        if (callback_driven_stream && size > kPcmSliceTargetBytes &&
            frame_bytes != 0 && size % frame_bytes == 0) {
            const uint32_t requested_slices =
                (size + kPcmSliceTargetBytes - 1u) / kPcmSliceTargetBytes;
            slices = requested_slices < kMaxPcmSlices
                ? requested_slices : kMaxPcmSlices;
        }
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            if (!player->alive.load(std::memory_order_acquire)) {
                jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
                return;
            }
            ReclaimCompletedBuffersLocked(*player);
            AudioAuditPollLocked();
            if (slices == 1) {
                result = (*player->buffer_queue)->Enqueue(
                    player->buffer_queue, owned_pcm->data(), size);
                if (result == SL_RESULT_SUCCESS) {
                    ++player->submitted_host_buffers;
                    AudioAuditSubmittedLocked(*player, owned_pcm->data(), size);
                    player->queued_pcm.push_back(
                        PlayerState::QueuedPcm{std::move(owned_pcm), true});
                }
            } else {
                // Split on complete PCM frames, never arbitrary bytes. A
                // stereo 16-bit frame is four bytes; byte-wise division of a
                // 2,137,500-byte chunk would otherwise make the next slice
                // begin in the middle of a sample/channel pair and emit a
                // periodic broadband burst without any queue underrun.
                const uint32_t total_frames = size / frame_bytes;
                const uint32_t base_frames = total_frames / slices;
                const uint32_t remainder_frames = total_frames % slices;
                uint32_t offset = 0;
                result = SL_RESULT_SUCCESS;
                for (uint32_t s = 0; s < slices && result == SL_RESULT_SUCCESS; ++s) {
                    const uint32_t slice_frames =
                        base_frames + (s == slices - 1 ? remainder_frames : 0);
                    const uint32_t slice_bytes = slice_frames * frame_bytes;
                    result = (*player->buffer_queue)->Enqueue(
                        player->buffer_queue, owned_pcm->data() + offset, slice_bytes);
                    if (result == SL_RESULT_SUCCESS) {
                        // One ref per slice: the payload stays alive until the
                        // last slice of this chunk completes. Only the first
                        // slice forwards a guest callback.
                        player->queued_pcm.push_back(
                            PlayerState::QueuedPcm{owned_pcm, s == 0});
                        ++player->submitted_host_buffers;
                        AudioAuditSubmittedLocked(*player, owned_pcm->data() + offset, slice_bytes);
                        offset += slice_bytes;
                    }
                }
            }
            if (result == SL_RESULT_SUCCESS) {
                ++player->enqueue_count;
                player->total_pcm_bytes += size;
                if (first_pcm) {
                    player->first_enqueue_caller = caller;
                    player->first_pcm_size = size;
                    player->first_pcm_hash = pcm_hash;
                    player->first_enqueue_ms = SteadyNowMs();
                }
                if (player->enqueue_count <= 10 || player->enqueue_count % 200 == 0) {
                    LOGI("SLBufferQueue_Enqueue player=0x%08X count=%llu size=%u slices=%u frame=%u owned=%zu",
                         player->guest_object,
                         static_cast<unsigned long long>(player->enqueue_count), size,
                         slices, frame_bytes, player->queued_pcm.size());
                }
            } else {
                LOGE("Host buffer queue Enqueue failed for player 0x%08X: %u",
                     player->guest_object, result);
            }
        }
        if (result == SL_RESULT_SUCCESS && first_pcm && probe_pcm) {
            LogPcmProbe(*player, probe_pcm->data(), size, pcm_hash, caller);
            DumpPcmProbe(*player, probe_pcm->data(), size, pcm_hash, caller);
        }
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLVolume_SetVolumeLevel", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        auto player = FindPlayerInterface(jit.Regs()[0], PlayerInterfaceKind::Volume,
                                          "SLVolume_SetVolumeLevel");
        if (!player || !player->volume) {
            jit.Regs()[0] = SL_RESULT_PRECONDITIONS_VIOLATED;
            return;
        }
        const SLmillibel level = static_cast<SLmillibel>(
            static_cast<int32_t>(jit.Regs()[1]));
        const bool had_level = player->volume_level_set.exchange(
            true, std::memory_order_acq_rel);
        const SLmillibel previous = player->last_volume_level.exchange(
            level, std::memory_order_acq_rel);
        const SLresult result = (*player->volume)->SetVolumeLevel(
            player->volume, level);
        if (AudioProbeEnabled() && (!had_level || previous != level ||
                                    result != SL_RESULT_SUCCESS)) {
            static std::atomic<uint32_t> volume_logs{0};
            const uint32_t index = volume_logs.fetch_add(1, std::memory_order_relaxed);
            if (index < 1024 || result != SL_RESULT_SUCCESS) {
                LOGI("audio probe player=0x%08X SetVolumeLevel=%d result=%u",
                     player->guest_object, static_cast<int>(level), result);
            }
        }
        jit.Regs()[0] = result;
    });

    manager.RegisterThunk("SLVolume_GetMaxVolumeLevel", [](Dynarmic::A32::Jit& jit,
                                                             DynarmicHost& host) {
        auto player = FindPlayerInterface(jit.Regs()[0], PlayerInterfaceKind::Volume,
                                          "SLVolume_GetMaxVolumeLevel");
        const uint32_t output_level = jit.Regs()[1];
        if (!player || !player->volume ||
            !GuestRangeValid(host, output_level, sizeof(SLmillibel))) {
            jit.Regs()[0] = SL_RESULT_PARAMETER_INVALID;
            return;
        }

        SLmillibel max_level = 0;
        const SLresult result = (*player->volume)->GetMaxVolumeLevel(
            player->volume, &max_level);
        if (result == SL_RESULT_SUCCESS) {
            std::memcpy(host.GetGuestMemoryBase() + output_level,
                        &max_level, sizeof(max_level));
        }
        jit.Regs()[0] = result;
    });
}
