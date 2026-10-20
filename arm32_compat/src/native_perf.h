#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

class DynarmicHost;
#ifndef BL2_NATIVE_PERF_PROBE
#define BL2_NATIVE_PERF_PROBE 0
#endif

#if BL2_NATIVE_PERF_PROBE
bool InstallNativePerfProbe(DynarmicHost& host);
std::uint64_t NativePerfSwapStart();
void NativePerfPresent(DynarmicHost& host, std::uint64_t swapStart, bool succeeded);

namespace bl2::native_perf {
// The root arms the existing capture token only after reaching a clear spawn
// location. A zero token must not consume this process's one-shot attempt.
inline bool ClaimOriginalCheatAttempt(std::atomic<bool>& attempted,bool requested,
        bool gameplay,bool armed,bool spawn,std::uint32_t capture) {
    return requested && gameplay && armed && (!spawn || capture!=0) &&
        !attempted.exchange(true,std::memory_order_acq_rel);
}
inline constexpr char kSpawnPawnTemplate[] = "GD_PrimalBeast.Character.Pawn_PrimalBeast";
struct SpawnPawnFrame {
    std::uint32_t data = 0;
    std::int32_t count = 0, capacity = 0;
};
static_assert(sizeof(SpawnPawnFrame) == 12);
struct SpawnPawnArgument {
    std::uint32_t dimension = 0, size = 0, offset = 0;
    std::uint64_t flags = 0;
};
// Original SpawnPawn36475 / args36474: one narrow FString, no return/out.
constexpr bool SpawnPawnSignatureValid(std::uint32_t flags, unsigned parameters,
        unsigned bytes, unsigned returnOffset, SpawnPawnArgument argument) {
    return flags == 0x00820202 && parameters == 1 && bytes == sizeof(SpawnPawnFrame) &&
        returnOffset == 0xFFFF && argument.dimension == 1 &&
        argument.size == sizeof(SpawnPawnFrame) && argument.offset == 0 &&
        argument.flags == 0x00400080;
}
struct IdentityGlobals {
    std::uint32_t objects = 0, names = 0, engine = 0, world = 0;
};
struct DamageIdentity {
    std::uint64_t targetToken = 0, localToken = 0;
    std::uint32_t controller = 0, localPawn = 0, world = 0;
    bool targetKnown = false, localKnown = false;
    char targetName[128]{}, targetClass[128]{};
    const char* reason = "unobserved";
};
// Passive diagnostic metadata read, also exercised with native fixture objects.
// A non-local target is not automatically an enemy.
DamageIdentity ReadDamageIdentity(DynarmicHost& host, IdentityGlobals globals,
                                  std::uint32_t target);
enum class Replay { Plain, Literal0, Literal1, AddPc, TailBranch, Forward };
struct Entry {
    const char* symbol;
    const char* label;
    std::uint32_t rva;
    std::array<std::uint32_t,2> words;
    Replay replay = Replay::Plain;
    std::uint32_t literalRva = 0, literalWord = 0;
    const char* existingThunk = nullptr;
};
// The catalogue is checked against the pinned original ELF before any observer
// is installed. Five entries retain the already-installed local lifecycle.
inline constexpr Entry kEntries[]{
    {"nvsiCheckLicense","license",0x01E1B260,{0xE12FFF1E,0xE92D4010}},
    {"_Z30NativeCallback_nvsiLockWrapperP7_JNIEnvP8_jobject","jni_lock",0x01E0EC44,{0xE92D4008,0xE59F3008},Replay::Literal1,0x01E0EC58,0x0111DE1C},
    {"nvsiLock","lock",0x02BA81F0,{0xE92D4010,0xE59F4018},Replay::Literal1,0x02BA8214,0x004E0690},
    {"_Z32NativeCallback_nvsiUnlockWrapperP7_JNIEnvP8_jobject","jni_unlock",0x01E0EC5C,{0xE92D4008,0xE59F3008},Replay::Literal1,0x01E0EC70,0x0111DE08},
    {"nvsiUnlock","unlock",0x02BA8218,{0xE59F301C,0xE92D4010},Replay::Literal0,0x02BA823C,0x004E0668},
    {"_Z37NativeCallback_nvsiCheckStatusWrapperP7_JNIEnvP8_jobject","jni_status",0x01E0EC74,{0xE59F302C,0xE92D4010},Replay::Literal0,0x01E0ECA8,0x0111DDEC},
    {"nvsiCheckStatus","status",0x02BA7578,{0xE92D4030,0xE24DD00C},Replay::Forward,0,0,"bl2_local_nvsiCheckStatus"},
    {"_Z31NativeCallback_nvsiCloseWrapperP7_JNIEnvP8_jobject","jni_close",0x01E0ECB4,{0xE59F302C,0xE92D4010},Replay::Literal0,0x01E0ECE8,0x0111DDAC},
    {"nvsiClose","close",0x02BA7D84,{0xE92D4070,0xE24DD018},Replay::Forward,0,0,"bl2_local_nvsiClose"},
    {"_Z31NativeCallback_nvsiPauseWrapperP7_JNIEnvP8_jobject","jni_pause",0x01E0FDF0,{0xE92D4038,0xE59F3064},Replay::Literal1,0x01E0FE60,0x0124778C},
    {"nvsiPause","pause",0x02BA7C2C,{0xE92D4010,0xE59F4058},Replay::Forward,0,0,"bl2_local_nvsiPause"},
    {"_Z32NativeCallback_nvsiResumeWrapperP7_JNIEnvP8_jobject","jni_resume",0x01E0FE80,{0xE92D4038,0xE59F3064},Replay::Literal1,0x01E0FEF0,0x012476FC},
    {"nvsiResume","resume",0x02BA7CA0,{0xE92D4070,0xE24DD008},Replay::Forward,0,0,"bl2_local_nvsiResume"},
    {"nvsiRunCalSnippet_Trajectory","trajectory",0x01E151FC,{0xE3A00000,0xE12FFF1E}},
    {"nvsiGetSharedSegment","shared_segment",0x02BA54B8,{0xE92D4070,0xE1A04000}},
    {"nvsiRunSnippet","run_snippet",0x02BA6E8C,{0xE92D4030,0xE24DD00C}},
    {"nvsiForceRecheck","force_recheck",0x02BA8638,{0xE59F3008,0xE08F3003},Replay::AddPc,0x02BA8648,0x004E024C},
    {"nvsiRunCalSnippet_CalcSlopeSlide","slope",0x01E1531C,{0xE3A00000,0xE12FFF1E}},
    {"_Z30NativeCallback_nvsiInitWrapperP7_JNIEnvP8_jobjecti","jni_init",0x01E1AF40,{0xE92D4070,0xE1A05002}},
    {"nvsiInitEx","init_ex",0x02BA7684,{0xE92D43F0,0xE1A07000},Replay::Forward,0,0,"bl2_local_nvsiInitEx"},
    {"nvsiCheckPending","pending",0x01E1B0A8,{0xE92D41F0,0xE2507000}},
    {"nvsiForceRecheckEx","force_recheck_ex",0x02BA8240,{0xE92D40F0,0xE1A06000}},
    {"nvsiInitJNI","init_jni",0x02BA51C0,{0xE92D4070,0xE3A03003}},
    {"nvsiRunCalSnippet_Ammo","ammo",0x01E1BDE8,{0xE5D01008,0xE3510000}},
    {"nvsiInitJNINative","init_jni_native",0x02BA4CE8,{0xE92D40F0,0xE1A05000}},
    {"nvsiGetGlobalData","get_global",0x02BA5614,{0xE92D4FF0,0xE1A06000}},
    {"nvsiSetGlobalData","set_global",0x02BA593C,{0xE92D43F0,0xE2508000}},
    {"nvsiInit","init",0x02BA7C24,{0xE3A01000,0xEAFFFE95},Replay::TailBranch},
    {"_ZN11FEngineLoop4TickEv","tick",0x01FCF578,{0xE92D4FF0,0xE1A07000}},
    {"_ZN11AWillowPawn17NotifyDamageTakenERK19FDamageEventSummary","pawn_damage",0x02342238,{0xE92D4FF0,0xED2D8B02}}
};
constexpr std::size_t kNvsiEntries = 28;
constexpr std::size_t kTickEntry = 28, kDamageEntry = 29;

struct ReplayCode { std::array<std::uint32_t,5> words{}; std::size_t count = 0; };
inline ReplayCode MakeReplay(const Entry& entry, std::uint32_t site, std::uint32_t forward = 0) {
    ReplayCode result{{entry.words[0],entry.words[1],0xE51FF004,site+8,0},4};
    switch (entry.replay) {
    case Replay::Plain: break;
    case Replay::Literal0:
    case Replay::Literal1: {
        const auto index = entry.replay == Replay::Literal0 ? 0u : 1u;
        result.words[index] = (entry.words[index] & ~0xFFFu) | (8u-index*4u);
        result.words[4] = entry.literalWord;
        result.count = 5;
        break;
    }
    case Replay::AddPc:
        // Original LDR r3,literal; ADD r3,pc,r3 has no flag writes. Load its
        // exact final address without introducing a relocated PC operand.
        result = {{0xE59F3004,0xE59FF004,0xE1A00000,
            site+12+entry.literalWord,site+8},5};
        break;
    case Replay::TailBranch: {
        const auto signedWords = static_cast<std::int32_t>(entry.words[1]<<8)>>8;
        result = {{entry.words[0],0xE51FF004,
            site+12+static_cast<std::uint32_t>(signedWords*4),0,0},3};
        break;
    }
    case Replay::Forward:
        result = {{0xE51FF004,forward,0,0,0},forward ? 2u : 0u};
        break;
    }
    return result;
}
} // namespace bl2::native_perf
#else
inline bool InstallNativePerfProbe(DynarmicHost&) { return true; }
inline std::uint64_t NativePerfSwapStart() { return 0; }
inline void NativePerfPresent(DynarmicHost&, std::uint64_t, bool) {}
#endif
