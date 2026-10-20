// Move UE3's periodic garbage collection out of gameplay.
//
// BL2's UWorld::Tick runs a full reachability analysis (FArchiveRealtimeGC)
// whenever TimeSinceLastPendingKillPurge exceeds
// GEngine->TimeBetweenPurgingPendingKillObjects (60 s). Measured on Adreno 650
// (Outwash): ~35 ms of game-thread work, one 54-62 ms frame every minute of play.
//
// The scheduler runs on the game thread at the touch input boundary, the thread
// that ticks the world. In gameplay it raises the engine interval tenfold; the
// first menu, dialog or frontend frame after the original interval forces the
// collection there, as AWorldInfo::ForceGarbageCollection does (counter set
// above the interval). A forced collection that has not started when the player
// returns to the world (async loading or a pending incremental purge postpone
// it) is put back under the interval for the next menu. Level loads, script and
// Java low-memory requests and the raised interval still collect.
//
// The field offsets belong to the shipped binary and are checked against its
// code before use. debug.bl2.gc_defer=0 (read once) keeps the engine schedule.
#include "dynarmic_host.h"
#include "elf_loader.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <cstdint>
#include <cstring>

namespace {
constexpr const char* kTag = "BL2GcDefer";
constexpr std::uint32_t kGEngine = 0x03008D20;        // UEngine* GEngine
constexpr std::uint32_t kGWorld = 0x0300D940;         // UWorld* GWorld
constexpr std::uint32_t kPurgeInterval = 0x484;       // UEngine::TimeBetweenPurgingPendingKillObjects
constexpr std::uint32_t kSincePurge = 0x1B4;          // UWorld::TimeSinceLastPendingKillPurge
constexpr float kGameplayFactor = 10.0f;

struct CodeWord {
    std::uint32_t address, word;
};
// AWorldInfo::ForceGarbageCollection and the interval test of UWorld::Tick.
constexpr CodeWord kCode[] = {
    {0x014D4D98, 0xE2822D12},  // add  r2, r2, #0x480
    {0x014D4DA0, 0xED927A01},  // vldr s14, [r2, #4]
    {0x014D4DB8, 0xEDC37A6D},  // vstr s15, [r3, #0x1b4]
    {0x0110D380, 0xED9B7A6D},  // vldr s14, [r11, #0x1b4]
    {0x0110D388, 0xE2833D12},  // add  r3, r3, #0x480
    {0x0110D38C, 0xEDD37A01},  // vldr s15, [r3, #4]
};

enum class State { Unchecked, Off, Active };
// Game thread only.
State g_state = State::Unchecked;
float g_native = 0.0f, g_deferred = 0.0f;
bool g_forced = false;
std::uint32_t g_forced_world = 0;

bool Readable(DynarmicHost& host, std::uint32_t address) {
    return address >= 0x10000 && address <= host.GetGuestMemorySize() - 4;
}

float ReadFloat(DynarmicHost& host, std::uint32_t address) {
    const std::uint32_t bits = host.MemoryRead32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void WriteFloat(DynarmicHost& host, std::uint32_t address, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    host.MemoryWrite32(address, bits);
}

bool Activate(DynarmicHost& host, std::uint32_t base) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.gc_defer", value) > 0 && value[0] == '0') {
        __android_log_print(ANDROID_LOG_INFO, kTag, "disabled by debug.bl2.gc_defer=0");
        return false;
    }
    for (const auto& code : kCode) {
        const std::uint32_t word = host.MemoryRead32(base + code.address);
        if (word != code.word) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "off: code %08X is %08X, expected %08X",
                                code.address, word, code.word);
            return false;
        }
    }
    return true;
}
}  // namespace

void Bl2GcDeferTick(DynarmicHost& host, bool ui_frame) {
#if defined(BL2_NATIVE_ARM32)
    // Keep the engine schedule in the native process's limited 32-bit address space.
    return;
#endif
    if (g_state == State::Off) return;
    const std::uint32_t base = host.GetElfLoader().GetBaseAddress();
    if (g_state == State::Unchecked) g_state = Activate(host, base) ? State::Active : State::Off;
    if (g_state != State::Active) return;
    const std::uint32_t engine = host.MemoryRead32(base + kGEngine);
    const std::uint32_t world = host.MemoryRead32(base + kGWorld);
    if (!engine || !world || !Readable(host, engine + kPurgeInterval) || !Readable(host, world + kSincePurge))
        return;
    const float interval = ReadFloat(host, engine + kPurgeInterval);
    if (g_native == 0.0f) {
        if (!(interval >= 1.0f && interval <= 3600.0f)) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "off: engine interval %.3f s", interval);
            g_state = State::Off;
            return;
        }
        g_native = interval;
        g_deferred = interval * kGameplayFactor;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "active: collections every %.0f s move to menus, gameplay interval %.0f s",
                            g_native, g_deferred);
    }
    if (interval == g_native) {
        WriteFloat(host, engine + kPurgeInterval, g_deferred);
    } else if (interval != g_deferred) {
        return;  // Someone else set the interval; leave the schedule to the engine.
    }
    const float since = ReadFloat(host, world + kSincePurge);
    if (ui_frame) {
        // Due by the original schedule and not already requested by the engine.
        if (since >= g_native && since <= g_deferred) {
            WriteFloat(host, world + kSincePurge, g_deferred + 1.0f);
            g_forced = true;
            g_forced_world = world;
            __android_log_print(ANDROID_LOG_INFO, kTag, "collection forced in UI after %.0f s", since);
        }
    } else if (g_forced) {
        if (world == g_forced_world && since > g_deferred) {
            WriteFloat(host, world + kSincePurge, g_native);
            __android_log_print(ANDROID_LOG_INFO, kTag, "forced collection postponed to the next menu");
        }
        g_forced = false;
    }
}
