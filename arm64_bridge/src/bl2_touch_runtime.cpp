#include "bl2_touch_runtime.h"
#include "bl2_iris_activation.h"
#include "bl2_touch_gameplay_sink.h"
#include "bl2_touch_gameplay_hooks.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_ui_sink.h"
#include "bl2_touch_widget_sink.h"
#include "bl2_touch_combined_sink.h"
#include "bl2_touch_ui_model_probe.h"
#include "bl2_touch_ui_model.h"
#include "bl2_pause_debug_menu.h"
#include "bl2_touch_gamepad_glyphs.h"
#include "bl2_graphics_settings.h"
#include "bl2_performance_settings.h"
#include "bl2_perf_hint.h"
#include "bl2_subtitle_timing.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include <memory>

#include "bridge_log.h"
#if BL2_ENABLE_QA_CONSOLE
#include "bl2_voice_timing_trace.h"
#endif
#ifndef BL2_ENABLE_QA_CONSOLE
#define BL2_ENABLE_QA_CONSOLE 0
#endif
#if BL2_ENABLE_QA_CONSOLE && !defined(BL2_NATIVE_ARM32) && !defined(BL2_STATIC_AOT)
#define BL2_DLC_MATERIAL_TRACE_QA 1
#include "bl2_dlc_material_trace.h"
#include "bl2_oasis_cache_snapshot.h"
#include "bl2_orchid_content_snapshot.h"
#include "bl2_voice_fx_trace.h"
#include "bl2_animtree_trace.h"
#include "bl2_iris_wheeled_runtime.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <limits>
#include <cerrno>
#include "bl2_voice_timing_trace.h"
#include "bl2_locale.h"
#include <cstdio>
#include <cmath>
#include <sys/syscall.h>
#include <unistd.h>
#else
#define BL2_DLC_MATERIAL_TRACE_QA 0
#endif
#if HORN_ENABLE_LOGGING || BL2_ENABLE_QA_CONSOLE
#include <android/log.h>
#endif
#include "dynarmic_host.h"
#include "bl2_touch_guest_call.h"
#include "elf_loader.h"
#include "obb_sha256.h"
#include "thunk_manager.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"

#include <sys/system_properties.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// bl2_gc_defer.cpp
void Bl2GcDeferTick(DynarmicHost& host, bool ui_frame);

namespace {

std::atomic<std::uint32_t> publishedMode{0};
std::atomic<std::uint64_t> publishedModeMs{0};

[[maybe_unused]] constexpr char kTag[] = "Arm32Bridge_Touch";
constexpr char kSha[] = "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
constexpr std::size_t kFileBytes = 49885152;
constexpr std::uint32_t kImageBytes = 0x03089000;
bool inputVerified = false;
bool installed = false;
bool active = false;
std::atomic<std::uint64_t> lifetimeGeneration{1};
std::atomic<std::uint64_t> inputTicks{0};
std::atomic<std::uint32_t> logs{0};
std::mutex observationMutex;
std::string lastObservation;
thread_local bool insideProbe = false;
std::unique_ptr<bl2::touch::Ue3GameplaySink> gameplaySink;
std::unique_ptr<bl2::touch::Ue3UiSink> uiSink;
std::unique_ptr<bl2::touch::Ue3WidgetSink> widgetSink;
std::unique_ptr<bl2::touch::CombinedSink> combinedSink;
bool uiModelObserverEnabled = false;
std::uint64_t currentLifetimeGeneration() { return lifetimeGeneration.load(std::memory_order_relaxed); }

struct Hook {
    const char* symbol;
    const char* thunk;
    std::uint32_t rva;
    std::uint32_t offset;
    std::array<std::uint32_t, 4> prefix;
};

// Only the two identified, PC-independent instructions are moved. The input
// destructor's first LDR is PC relative: leave it in place and hook at +4.
constexpr Hook hooks[] = {
    {"_ZN11FKdViewport12ProcessInputEf", "bl2_touch_input_boundary", 0x01E228FC, 0,
        {0xE92D4FF0, 0xED2D8B04, 0xE1A04000, 0xE59F5368}},
    {"_ZN10FGFxEngine10StartSceneEP9FGFxMovieP22UTextureRenderTarget2Djj",
        "bl2_touch_movie_start", 0x01CD8734, 0,
        {0xE92D47F0, 0xED2D8B04, 0xE1A04001, 0xE59F8370}},
    {"_ZN10FGFxEngine10CloseSceneEP9FGFxMoviej", "bl2_touch_movie_close", 0x01CD1678, 0,
        {0xE92D47F0, 0xE1A08000, 0xE59002D8, 0xE24DD008}},
    {"_ZN18UWillowPlayerInputD1Ev", "bl2_touch_input_destroy", 0x023898B0, 4,
        {0xE59F3338, 0xE92D4FF0, 0xE1A04000, 0xE59F7330}},
};

struct Global { const char* symbol; std::uint32_t rva; };
constexpr Global globals[] = {
    {"GEngine", 0x03008D20}, {"GWorld", 0x0300D940}, {"GGFxEngine", 0x0301F840},
    {"_ZN7UObject11GObjObjectsE", 0x02FBA268}, {"_ZN5FName5NamesE", 0x02FB00C8},
    {"_ZN18UWillowPlayerInput18PrivateStaticClassE", 0x0306C578},
};

// All offsets below are reads proved by this exact ELF's instructions. See
// touch_native_design/analysis/runtime_seams_disassembly.txt and RUNTIME_STAGE1.md.
// Guest pointers never leave this engine-thread reader.
class Reader {
public:
#if BL2_DLC_MATERIAL_TRACE_QA
    using SpanFilter=bool(*)(const Reader&,std::uint32_t,std::size_t);
    explicit Reader(DynarmicHost& value,SpanFilter filter=nullptr) : host(value),base(value.GetElfLoader().GetBaseAddress()),spanFilter(filter) {}
#else
    explicit Reader(DynarmicHost& value) : host(value), base(value.GetElfLoader().GetBaseAddress()) {}
#endif
    bool span(std::uint32_t p, std::size_t n) const {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() &&
               n <= host.GetGuestMemorySize() - p
#if BL2_DLC_MATERIAL_TRACE_QA
               && (!spanFilter || spanFilter(*this,p,n))
#endif
               ;
    }
    static bool copyMemory(const void* context, std::uint32_t p, void* out, std::size_t n) {
        const auto& read = *static_cast<const Reader*>(context);
#if defined(BL2_NATIVE_ARM32)
        if (!read.span(p,n)) return false;
        std::memcpy(out,reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p)),n);
#else
        if (!read.span(p,n) || !read.host.GetGuestMemoryBase()) return false;
        std::memcpy(out,read.host.GetGuestMemoryBase() + p,n);
#endif
        return true;
    }
    std::uint32_t u32(std::uint32_t p) const {
        return span(p, 4) ? host.MemoryRead32(p) : 0;
    }
    bool array(std::uint32_t p, std::uint32_t limit, std::uint32_t& data,
               std::uint32_t& count) const {
        if (!span(p, 12)) return false;
        data = u32(p); count = u32(p + 4);
        const auto capacity = u32(p + 8);
        return count <= capacity && capacity <= limit &&
               (count == 0 || span(data, std::size_t(count) * 4));
    }
    bool object(std::uint32_t p) const {
        if (!span(p, 0x38)) return false;
        std::uint32_t objects = 0, count = 0;
        if (!array(base + 0x02FBA268, 2000000, objects, count)) return false;
        const auto index = u32(p + 0x20);
        return index < count && u32(objects + index * 4) == p;
    }
    std::string name(std::uint32_t fname,std::uint32_t limit=128) const {
        if (!limit || limit>512 || !span(fname, 8)) return {};
        std::uint32_t names = 0, count = 0;
        if (!array(base + 0x02FB00C8, 2000000, names, count)) return {};
        const auto index = u32(fname);
        if (index >= count) return {};
        const auto entry = u32(names + index * 4);
        if (!span(entry, 8) || (limit <= 128 && !span(entry, 8 + limit))) return {};
        std::string result;
        for (std::uint32_t i = 0; i != limit; ++i) {
            if (limit > 128 && (std::uint64_t(entry) + 8 + i > UINT32_MAX || !span(entry + 8 + i, 1))) return {};
            const char ch = static_cast<char>(host.MemoryRead8(entry + 8 + i));
            if (!ch) {
                const auto number = u32(fname + 4);
                if (number) result += "_" + std::to_string(number - 1);
                return result;
            }
            // Only native identifiers are used here, never localized UI text.
            if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) return {};
            result += ch;
        }
        return {};
    }
    std::string objectName(std::uint32_t p) const { return object(p) ? name(p + 0x2C) : std::string{}; }
    std::string className(std::uint32_t p) const { return object(p) ? objectName(u32(p + 0x34)) : std::string{}; }
    bool isA(std::uint32_t p, std::uint32_t wanted) const {
        if (!object(p) || !object(wanted)) return false;
        auto cls = u32(p + 0x34);
        for (int depth = 0; depth != 48 && cls; ++depth) {
            if (!object(cls) || !span(cls, 0x44)) return false;
            if (cls == wanted) return true;
            cls = u32(cls + 0x40);
        }
        return false;
    }
    std::uint64_t objectToken(std::uint32_t p) const {
        return object(p) ? (std::uint64_t(p) << 32) | u32(p + 0x20) : 0;
    }
    DynarmicHost& host;
    std::uint32_t base;
#if BL2_DLC_MATERIAL_TRACE_QA
    SpanFilter spanFilter=nullptr;
#endif
};

std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}

struct Observation {
    std::uint32_t viewport = 0, world = 0, local = 0, controller = 0, input = 0, movie = 0, movieOwner = 0;
    std::vector<std::string> contexts;
    std::vector<std::string> available;
    std::string movieClass;
    std::string reason = "no-local-player";
    bl2::touch::Mode mode = bl2::touch::Mode::Unknown;
    std::uint64_t identity = 1;
};

bool collectActions(Reader& read, std::uint32_t definition, std::vector<std::uint32_t>& actions) {
    std::uint32_t data = 0, count = 0;
    if (!read.array(definition + 0x3C, 256, data, count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto action = read.u32(data + i * 4);
        if (!action) continue;
        if (!read.object(action) || !read.span(action, 0x60)) return false;
        actions.push_back(action);
    }
    return true;
}

Observation observe(Reader& read, std::uint32_t viewport) {
    Observation out;
    out.viewport = viewport;
    out.world = read.u32(read.base + 0x0300D940);
    out.identity = mix(lifetimeGeneration.load(std::memory_order_relaxed), out.world);
    out.identity = mix(out.identity, viewport);
    if (!read.span(viewport, 0x24) || !read.u32(viewport + 0x20)) {
        out.reason = "viewport-without-client"; return out;
    }
    const auto engine = read.u32(read.base + 0x03008D20);
    if (!read.object(engine)) return out;
    std::uint32_t players = 0, count = 0;
    if (!read.array(engine + 0x48C, 4, players, count) || count != 1) {
        out.reason = "requires-single-local-player"; return out;
    }
    const auto local = read.u32(players);
    if (!read.object(local) || !read.span(local, 0x68)) return out;
    out.local = local;
    out.controller = read.u32(local + 0x40);
    if (!read.object(out.controller) || !read.span(out.controller, 0x770)) return out;
    out.identity = mix(out.identity, read.objectToken(out.controller));
    out.input = read.u32(out.controller + 0x76C);
    if (!read.isA(out.input, read.u32(read.base + 0x0306C578)) || !read.span(out.input, 0x294)) {
        out.reason = "no-live-willow-input"; return out;
    }
    out.identity = mix(out.identity, read.objectToken(out.input));
    if (read.u32(out.input + 0x28) != out.controller) {
        out.reason = "input-outer-mismatch"; return out;
    }

    // For exactly one local player, the preferred focus is slot 0. This reads
    // the branch at GetFocusedMovieForInput+0x114 without calling guest code.
    const auto gfx = read.u32(read.base + 0x0301F840);
    std::uint32_t focused = 0, focusedCount = 0;
    if (read.span(gfx, 0x34) && read.array(gfx + 0x28, 4, focused, focusedCount) && focusedCount) {
        const auto movie = read.u32(focused);
        if (read.span(movie, 0x68)) {
            const auto owner = read.u32(movie + 0x64);
            if (read.object(owner)) {
                out.movie = movie; out.movieOwner = owner; out.movieClass = read.className(owner);
                out.identity = mix(out.identity, movie);
                out.identity = mix(out.identity, read.objectToken(owner));
                if (out.movieClass == "StatusMenuExGFxMovie" || out.movieClass == "CharacterSelectionReduxGFxMovie") {
                    const auto direct = bl2::touch::CaptureBl2DirectUi({&read,Reader::copyMemory},read.base,owner,0);
                    // Changing a status tab or character UI state retires its
                    // old pointer capture even when the Movie address is reused.
                    // Selection itself stays outside this scene identity.
                    out.identity = mix(out.identity,direct.valid ? direct.structure : 0);
                }
            }
        }
    }

    std::uint32_t contexts = 0, contextCount = 0;
    if (!read.array(out.input + 0x1A4, 32, contexts, contextCount)) {
        out.reason = "invalid-context-array"; return out;
    }
    std::vector<std::uint32_t> actions;
    for (std::uint32_t i = 0; i < contextCount; ++i) {
        const auto ctx = read.u32(contexts + i * 4);
        if (!ctx) continue;
        if (!read.object(ctx) || !read.span(ctx, 0x54)) {
            out.reason = "invalid-context-object"; return out;
        }
        const auto name = read.objectName(ctx);
        if (name.empty()) { out.reason = "unresolved-context-name"; return out; }
        out.contexts.push_back(name);
        if (!collectActions(read, ctx, actions)) { out.reason = "invalid-context-actions"; return out; }
        std::uint32_t sets = 0, setCount = 0;
        if (!read.array(ctx + 0x48, 64, sets, setCount)) { out.reason = "invalid-context-sets"; return out; }
        for (std::uint32_t j = 0; j < setCount; ++j) {
            const auto set = read.u32(sets + j * 4);
            if (set && (!read.object(set) || !collectActions(read, set, actions))) {
                out.reason = "invalid-set-actions"; return out;
            }
        }
    }
    std::sort(out.contexts.begin(), out.contexts.end());
    for (const auto& name : out.contexts) for (unsigned char ch : name) out.identity = mix(out.identity, ch);
    std::sort(actions.begin(), actions.end());
    actions.erase(std::unique(actions.begin(), actions.end()), actions.end());
    if (actions.size() > 256) { out.reason = "too-many-context-actions"; return out; }
    // This leaf is the game's actual availability check. Its result must be
    // synchronous even when ProcessInput runs on a suspended callback core.
    const auto isAvailable = read.base + 0x0235679C;
    for (const auto action : actions) {
        const auto name = read.name(action + 0x58);
#if defined(BL2_NATIVE_ARM32)
        if (!name.empty() && read.host.RunGuestCallback(isAvailable, {out.input, action})) out.available.push_back(name);
#else
        if (name.empty()) continue;
        std::uint32_t value = 0;
        if (!bl2::touch::Bl2TouchCallGuest(read.host,isAvailable,{out.input,action},value)) {
            out.reason = "availability-call-failed"; return out;
        }
        if (value) out.available.push_back(name);
#endif
    }
    std::sort(out.available.begin(), out.available.end());
    out.reason = "observed-only";
    bool nativeMenuOpen = false;
    if (!out.movie) {
        const auto client = bl2::touch::Bl2TouchGameViewportClient({&read,Reader::copyMemory},read.base);
        const auto nativeUi = bl2::touch::CaptureBl2UiModel({&read,Reader::copyMemory},
            read.base,client);
        if (nativeUi.valid) {
            out.identity = mix(out.identity,nativeUi.structure);
            nativeMenuOpen = nativeUi.nativeMenuOpen;
        }
    }
    if (out.movie && out.movieClass != "WillowHUDGFxMovie") {
        out.mode = bl2::touch::Bl2TouchFocusedMovieMode(out.movieClass);
        if (out.mode == bl2::touch::Mode::Loading) out.reason = "blocking-online-message";
    } else if (nativeMenuOpen) out.mode = bl2::touch::Mode::Menu;
    else {
        out.mode = bl2::touch::Bl2TouchContextMode(out.contexts);
        if (out.mode == bl2::touch::Mode::Unknown)
            out.reason = out.contexts.size() > 2 ? "too-many-live-contexts" : "unclassified-live-context";
    }
    return out;
}

std::string joined(const std::vector<std::string>& names) {
    std::string result;
    for (const auto& name : names) { if (!result.empty()) result += ','; result += name; }
    return result;
}

#if HORN_ENABLE_LOGGING || BL2_ENABLE_QA_CONSOLE
// Opt-in QA console, including quiet benchmark builds:
// debug.bl2bridge.qa_exec="<tag>:<console command>" runs the command once
// through the original ULocalPlayer::Exec(const TCHAR*, FOutputDevice&) with
// *GLog, exactly as the game's own console would. A new tag repeats it.
constexpr std::uint32_t kLocalPlayerExec = 0x01623F50;
constexpr std::uint32_t kGLog = 0x02F3A200;
constexpr std::array<std::uint32_t, 4> kLocalPlayerExecPrefix = {0xE92D4FF0, 0xED2D8B02, 0xE1A07000, 0xE59F4794};
bool qaExecVerified = false;
std::string qaLastExec;
std::uint64_t qaPollMs = 0;
std::uint32_t qaScratch = 0;
#if BL2_ENABLE_QA_CONSOLE
std::atomic<std::uint64_t> qaSubtitleUntil{0};
std::atomic<unsigned> qaSubtitleRows{0};
#endif
// ClientMessage and script `log text of a QA command reach only GLog's output
// devices. A 16-byte guest FOutputDevice (vptr, 0, 0, 1: the base fields as
// FOutputDeviceRedirector's constructor sets them) whose vtable copies
// _ZTV17FOutputDeviceNull with Serialize (address point + 8, the slot
// FOutputDevice::Log calls) bound to a host thunk joins the original redirector
// at the first command. Only a 3 s window after each command reaches logcat.
constexpr std::uint32_t kAddOutputDevice = 0x00FFA120;
constexpr std::array<std::uint32_t, 4> kAddOutputDevicePrefix = {0xE92D40F8, 0xE2807040, 0xE59F30B8, 0xE1A04001};
constexpr std::uint32_t kNullDeviceVtable = 0x02D26170;
constexpr std::array<std::uint32_t, 3> kNullDeviceSlots = {0x00F250B8, 0x00F265A4, 0x00F24CBC};
// "@ondemand <package>" asks the original client RPC body to stream a vehicle
// package (UWillowPlayerPawnDataManager::LoadDataPackageAsyncNoCallback, which
// appends _SF), as the server does for a Catch-A-Ride. SpawnVehicle needs it.
constexpr std::uint32_t kClientLoadOnDemand = 0x02369C18;
constexpr std::array<std::uint32_t, 4> kClientLoadOnDemandPrefix = {0xE92D4038, 0xE1A05001, 0xE59F302C, 0xE1A04000};
bool qaOnDemandVerified = false;
// "@standalone <command>": WillowCheatManager.SpawnVehicle returns unless
// WorldInfo.NetMode is NM_Standalone, and BL2 hosts single player as a listen
// server. UWorld::GetNetMode reads GWorld->PersistentLevel(+0x50)->Actors
// (+0x3C)[0]->NetMode (byte +0x370); only this one Exec call sees 0.
constexpr std::uint32_t kWorldGetNetMode = 0x017BA4D4;
constexpr std::array<std::uint32_t, 5> kWorldGetNetModeWords = {0xE5903050, 0xE593303C, 0xE5933000, 0xE5D30370, 0xE12FFF1E};
bool qaStandaloneVerified = false;
bool qaMirrorVerified = false;
std::uint32_t qaMirrorThunk = 0, qaMirror = 0;
std::atomic<std::uint64_t> qaMirrorUntil{0};
std::atomic<unsigned> qaMirrorLines{0};

void qaMirrorSerialize(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    if (bl2::touch::monotonicMilliseconds() > qaMirrorUntil.load(std::memory_order_relaxed) ||
        qaMirrorLines.fetch_add(1, std::memory_order_relaxed) >= 200) return;
    const auto text = jit.Regs()[1];
    std::string line;
    for (std::uint32_t i = 0; i != 480 && text >= 0x10000 && text + i < host.GetGuestMemorySize(); ++i) {
        const auto c = host.MemoryRead8(text + i);
        if (!c) break;
        line += c >= 32 && c != 127 ? static_cast<char>(c) : '?';
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "qa-glog event=%u %s", jit.Regs()[2], line.c_str());
}

bool qaMirrorAttach(Reader& read) {
    if (qaMirror) return true;
    if (!qaMirrorVerified) return false;
    for (std::size_t i = 0; i < kNullDeviceSlots.size(); ++i)
        if (read.u32(read.base + kNullDeviceVtable + 8 + std::uint32_t(i * 4)) != read.base + kNullDeviceSlots[i]) {
            qaMirrorVerified = false;
            __android_log_print(ANDROID_LOG_ERROR, kTag, "qa-glog unavailable: FOutputDeviceNull vtable slot %zu mismatch", i);
            return false;
        }
    const auto redirector = read.u32(read.base + kGLog);
    if (!read.span(redirector, 0x44)) return false;
    // Register only after startup so quiet QA does not shift existing SVC IDs.
    if (!qaMirrorThunk) qaMirrorThunk = read.host.GetThunkManager().RegisterThunk("bl2_qa_glog_serialize", qaMirrorSerialize);
    if (!qaMirrorThunk) return false;
    const auto block = read.host.GuestAlloc(7 * 4 + 16, 8);
    if (!block) return false;
    for (std::uint32_t i = 0; i != 7; ++i)
        read.host.MemoryWrite32(block + i * 4, read.u32(read.base + kNullDeviceVtable + i * 4));
    read.host.MemoryWrite32(block + 4 * 4, qaMirrorThunk);
    const auto device = block + 7 * 4;
    read.host.MemoryWrite32(device, block + 8);
    read.host.MemoryWrite32(device + 4, 0);
    read.host.MemoryWrite32(device + 8, 0);
    read.host.MemoryWrite32(device + 12, 1);
    if (!bl2::touch::Bl2TouchCallGuest(read.host, read.base + kAddOutputDevice, {redirector, device})) return false;
    qaMirror = device;
    __android_log_print(ANDROID_LOG_INFO, kTag, "qa-glog mirror attached device=%08X redirector=%08X", device, redirector);
    return true;
}

#if BL2_DLC_MATERIAL_TRACE_QA
bl2::dlc::trace::Budget qaMaterialBudget;
bool qaMaterialAttempted=false,qaMaterialInstalled=false,qaSpawnErrorInstalled=false;
std::vector<std::pair<std::uintptr_t,std::uintptr_t>> qaMaterialReadable;
std::mutex qaReadableMutex;

bool qaRefreshReadable() {
    auto* maps=std::fopen("/proc/self/maps","r");if(!maps)return false;
    std::vector<std::pair<std::uintptr_t,std::uintptr_t>> ranges;char line[512];
    while(std::fgets(line,sizeof(line),maps)) {
        unsigned long long low=0,high=0;char permissions[5]{};
        if(std::sscanf(line,"%llx-%llx %4s",&low,&high,permissions)==3 && permissions[0]=='r' && low<high && high<=UINTPTR_MAX)
            ranges.emplace_back(static_cast<std::uintptr_t>(low),static_cast<std::uintptr_t>(high));
    }
    std::fclose(maps);if(ranges.empty())return false;
    std::lock_guard<std::mutex> lock(qaReadableMutex);
    qaMaterialReadable=std::move(ranges);return true;
}

bool qaMaterialSpan(const Reader& read,std::uint32_t p,std::size_t n) {
    const auto memory=reinterpret_cast<std::uintptr_t>(read.host.GetGuestMemoryBase());
    if(!memory || p>UINTPTR_MAX-memory) return false;
    const auto at=memory+p;
    if(n>UINTPTR_MAX-at) return false;
    std::lock_guard<std::mutex> lock(qaReadableMutex);
    for(const auto& [low,high]:qaMaterialReadable) if(at>=low && at+n<=high) return true;
    return false;
}

std::atomic<unsigned> qaCacheEntries{0},qaCacheEligible{0},qaCacheReused{0},qaCacheChecked{0},qaCacheValid{0};
std::atomic<unsigned> qaCacheFailureSnapshots{0};
std::atomic<std::uint32_t> qaCacheReadOwner{0};
std::atomic<std::uint64_t> qaCacheUntil{0};
bool qaCacheAttempted=false;
constexpr std::array<bl2::dlc::trace::Hook,2> qaCacheHooks{{
    {0x01249A60,0xE59D3030,0xE1A02000,"bl2_qa_cache_entry"},
    {0x01249D94,0xE3500000,0xE59D3008,"bl2_qa_cache_uniform_result"},
}};

void qaCacheSnapshot(const char* phase,DynarmicHost& host) {
    if(!qaRefreshReadable())return;
    Reader read(host,qaMaterialSpan);std::uint32_t objects=0,n=0,manager=0,matches=0;
    if(read.array(read.base+0x02FBA268,2000000,objects,n))for(std::uint32_t i=0;i<n;++i) {
        const auto p=read.u32(objects+i*4);
        if(read.object(p) && read.objectName(p).rfind("WillowPlayerPawnDataManager_",0)==0 &&
           bl2::dlc::cacheqa::Class(read,p,"WillowPlayerPawnDataManager","WillowGame")) {manager=p;++matches;}
    }
    const auto s=bl2::dlc::cacheqa::Capture(read,matches==1?manager:0);
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "qa-oasis-cache phase=%s objects_ok=%u objects=%u package_matches=%u package_ok=%u package=%08X loaded_raw=%u package_flags=%016llX cache_matches=%u cache_ok=%u cache=%08X cache_flags=%016llX platform=%u",
        phase,unsigned(s.objectsOk),s.objectCount,s.packageMatches,unsigned(s.packageOk),s.package,s.fullyLoaded,
        static_cast<unsigned long long>(s.packageFlags),s.cacheMatches,unsigned(s.cacheOk),s.cache,
        static_cast<unsigned long long>(s.cacheFlags),s.platform);
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "qa-oasis-cache phase=%s ref_matches=%u ref_ok=%u ref=%08X ref_flags=%016llX refs_ok=%u refs=%u contains_cache=%u manager_matches=%u manager_ok=%u manager=%08X owners_ok=%u owners=%u retained=%u async_ok=%u async_count=%u async_matches=%u task=%08X linker=%08X percent_bits=%08X cursors=%u,%u,%u",
        phase,s.referencerMatches,unsigned(s.referencerOk),s.referencer,static_cast<unsigned long long>(s.referencerFlags),
        unsigned(s.refsOk),s.refCount,unsigned(s.containsCache),matches,unsigned(s.managerOk),s.manager,
        unsigned(s.ownersOk),s.ownerCount,unsigned(s.retained),unsigned(s.asyncOk),s.asyncCount,s.asyncMatches,s.task,s.linker,
        s.percentageBits,s.exportCursor,s.preloadCursor,s.postloadCursor);
    const std::array<std::pair<const char*,bl2::dlc::cacheqa::RawHeader>,2> headers{{{"cache",s.cacheMaps},{"registry6",s.registry6}}};
    for(const auto& [name,h]:headers)__android_log_print(ANDROID_LOG_INFO,kTag,
        "qa-oasis-cache phase=%s header=%s readable=%u bounded=%u data=%08X storage_num=%u capacity=%u inline_hash=%08X hash_data=%08X hash_size=%u",
        phase,name,unsigned(h.readable),unsigned(h.bounded),h.data,h.storageNum,h.capacity,h.inlineHash,h.hashData,h.hashSize);
    __android_log_print(ANDROID_LOG_INFO,kTag,"qa-oasis-cache phase=%s entries=%u version_eligible=%u reused=%u uniform_checked=%u uniform_valid=%u",
        phase,qaCacheEntries.load(),qaCacheEligible.load(),qaCacheReused.load(),qaCacheChecked.load(),qaCacheValid.load());
}

void qaCacheObserve(unsigned stage,Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    if(bl2::touch::monotonicMilliseconds()>qaCacheUntil.load(std::memory_order_acquire))return;
    const auto& r=jit.Regs();
    if(qaCacheReadOwner.load()!=r[9]) {
        if(!qaRefreshReadable())return;
        qaCacheReadOwner.store(r[9]);
    }
    Reader read(host,qaMaterialSpan);
    if(!read.object(r[9]) || read.objectName(r[9])!="CacheObject" ||
       read.objectName(read.u32(r[9]+0x28))!=bl2::dlc::cacheqa::kPackage)return;
    const auto key=r[8];bool target=read.span(key,64);
    for(unsigned i=0;target && i<4;++i)target=read.u32(key+i*4)==bl2::dlc::cacheqa::kEmptyStaticKey[i];
    unsigned row=0;
    if(stage==0) {
        row=qaCacheEntries.fetch_add(1)+1;if(row>4096)return;
        const auto a=static_cast<std::int32_t>(read.u32(r[13]+0x30));
        const auto b=static_cast<std::int32_t>(read.u32(r[13]+0x38));
        const auto c=static_cast<std::int32_t>(read.u32(r[13]+0x40));
        if(a>=832 && b>=0 && c>=0)qaCacheEligible.fetch_add(1);
        if(r[0])qaCacheReused.fetch_add(1);
        if(row<=6 || target)__android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-oasis-load stage=entry row=%u target=%u cache=%08X key=%08X guid=%s versions=%d,%d,%d existing=%08X",
            row,unsigned(target),r[9],key,bl2::dlc::trace::Guid(read,key).c_str(),a,b,c,r[0]);
    } else {
        row=qaCacheChecked.fetch_add(1)+1;if(row>4096)return;
        if(r[0])qaCacheValid.fetch_add(1);
        if(row<=6 || target)__android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-oasis-load stage=uniform row=%u target=%u cache=%08X key=%08X guid=%s map=%08X valid=%u",
            row,unsigned(target),r[9],key,bl2::dlc::trace::Guid(read,key).c_str(),read.u32(r[13]+8),r[0]);
    }
}

void qaCacheActivate(Reader& read) {
    char enabled[PROP_VALUE_MAX]{};
    if(qaCacheAttempted || __system_property_get("debug.bl2.dlc_cache_trace",enabled)<=0 || std::strcmp(enabled,"1"))return;
    qaCacheAttempted=true;auto& host=read.host;auto& elf=host.GetElfLoader();const auto base=read.base;
    if(!inputVerified || elf.GetImageSize()!=kImageBytes || base>UINT32_MAX-kImageBytes ||
       elf.GetSymbolAddress("_ZN12UShaderCache4LoadER8FArchivej")!=base+0x01249910 ||
       elf.GetSymbolAddress("_ZNK18FMaterialShaderMap27IsUniformExpressionSetValidEv")!=base+0x01849430 ||
       read.u32(base+0x01249A5C)!=0xE12FFF33 || read.u32(base+0x01249D90)!=0xE12FFF31) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-oasis-load refused identity/call-context");return;
    }
    for(const auto& h:qaCacheHooks)if(!elf.IsReadOnlyRange(base+h.rva,8) || read.u32(base+h.rva)!=h.first || read.u32(base+h.rva+4)!=h.second) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-oasis-load refused preimage");return;
    }
    std::array<std::uint32_t,2> veneers{},thunks{};
    for(unsigned i=0;i<2;++i) {
        const auto words=bl2::dlc::trace::Veneer(qaCacheHooks[i],base);
        veneers[i]=host.GuestAlloc(sizeof(words),8);if(!veneers[i])return;
        for(unsigned j=0;j<words.size();++j)host.MemoryWrite32(veneers[i]+j*4,words[j]);
        host.AddExecutableRange(veneers[i],sizeof(words));host.InvalidateGuestCode(veneers[i],sizeof(words));
    }
    for(unsigned i=0;i<2;++i) {
        thunks[i]=host.GetThunkManager().RegisterThunk(qaCacheHooks[i].thunk,[i,veneer=veneers[i]](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
            jit.Regs()[15]=veneer+((jit.Cpsr()&0x200u)?16u:0u);qaCacheObserve(i,jit,current);
        });
        if(!thunks[i])return;
        const auto svc=read.u32(thunks[i]);if((svc&0xFF000000u)!=0xEF000000u || !(svc&0xFFFFFFu))return;
    }
    for(unsigned i=0;i<2;++i) {
        host.MemoryWrite32(base+qaCacheHooks[i].rva,read.u32(thunks[i]));host.InvalidateGuestCode(base+qaCacheHooks[i].rva,4);
        if(read.u32(base+qaCacheHooks[i].rva)!=read.u32(thunks[i]) || read.u32(base+qaCacheHooks[i].rva+4)!=qaCacheHooks[i].second)return;
    }
    qaCacheUntil.store(bl2::touch::monotonicMilliseconds()+90000,std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO,kTag,"qa-oasis-load armed hooks=2 window_ms=90000 owner=Orchid_OasisShaderCache_SF");
}

void qaMaterialObserve(unsigned stage,Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    const auto now=bl2::touch::monotonicMilliseconds();std::uint32_t row=0;
    if(!qaMaterialBudget.Take(now,row)) return;
    Reader read(host,qaMaterialSpan);
    const auto& regs=jit.Regs();
    const auto owner=stage==0?regs[6]:0;
    const auto resource=stage==0?(read.object(owner)&&read.span(owner,0x3DC)?read.u32(owner+0x3D8):0):regs[4];
    const auto readable=read.span(resource,0x64);
    const auto map=readable?read.u32(resource+0x50):0;
    const auto guid=readable?bl2::dlc::trace::Guid(read,resource+0x54):std::string("<unreadable>");
    const auto rhi=read.u32(read.base+0x03002390);
    const auto tid=static_cast<long>(syscall(SYS_gettid));
    if(stage==0) {
        const auto path=bl2::dlc::trace::FullPath(read,owner);
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-dlc-material row=%u ms=%llu tid=%ld stage=cache-result owner=%08X path_ok=%u path=%s resource=%08X resource_ok=%u result=%08X map=%08X guid=%s rhi=%u flags=%08X",
            row,static_cast<unsigned long long>(now),tid,owner,unsigned(path.valid),path.text.c_str(),resource,
            unsigned(readable),regs[5],map,guid.c_str(),rhi,jit.Cpsr());
    } else {
        const auto key=regs[5];const auto parameters=bl2::dlc::trace::StaticArrays(read,key);
        const auto staticGuid=bl2::dlc::trace::Guid(read,key);
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-dlc-material row=%u ms=%llu tid=%ld stage=findid-post resource=%08X resource_ok=%u key=%08X result=%08X oldmap=%08X guid=%s static_guid=%s requested=%u rhi=%u counts=%u,%u,%u,%u caps=%u,%u,%u,%u arrays_ok=%X flags=%08X",
            row,static_cast<unsigned long long>(now),tid,resource,unsigned(readable),key,regs[0],map,guid.c_str(),
            staticGuid.c_str(),regs[8],rhi,parameters.count[0],parameters.count[1],parameters.count[2],parameters.count[3],
            parameters.capacity[0],parameters.capacity[1],parameters.capacity[2],parameters.capacity[3],parameters.validMask,jit.Cpsr());
    }
    if(stage==1 && !regs[0] && qaCacheFailureSnapshots.fetch_add(1)<2)qaCacheSnapshot("material-findid-zero",host);
}

bool qaLinkerAttempted=false;
std::atomic<unsigned> qaLinkerRows{0};
constexpr bl2::dlc::trace::Hook qaLinkerHook{0x00FBB3D4,0xE594E0F4,0xE30120FC,"bl2_qa_linker_import_assert"};

void qaLinkerObserve(Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    if(qaLinkerRows.fetch_add(1,std::memory_order_relaxed)>=4 || !qaRefreshReadable())return;
    Reader read(host,qaMaterialSpan);const auto& r=jit.Regs();const auto linker=r[4];
    std::string message;
    for(unsigned i=0;i<480 && read.span(r[3]+i,1);++i) {
        const auto c=host.MemoryRead8(r[3]+i);if(!c)break;
        message+=c>=32 && c!=127?static_cast<char>(c):'?';
    }
    __android_log_print(ANDROID_LOG_ERROR,kTag,
        "qa-dlc-linker linker=%08X bad_import=%u input_index=%d imports=%08X,%u,%u exports=%08X,%u,%u caller=%08X sp=%08X cpsr=%08X message=%s",
        linker,r[6],static_cast<std::int32_t>(~r[6]),read.u32(linker+0xF0),read.u32(linker+0xF4),read.u32(linker+0xF8),
        read.u32(linker+0x504),read.u32(linker+0x508),read.u32(linker+0x50C),read.u32(r[13]+0x34),r[13],jit.Cpsr(),message.c_str());
    const auto root=read.u32(linker+0x3C),object=read.u32(read.base+0x02F964CC),property=read.u32(read.base+0x02F964D0);
    const auto rootPath=bl2::dlc::trace::FullPath(read,root),objectPath=bl2::dlc::trace::FullPath(read,object),propertyPath=bl2::dlc::trace::FullPath(read,property);
    std::string filename;const auto filenameData=read.u32(linker+0x558),filenameCount=read.u32(linker+0x55C);
    if(filenameCount<=512 && read.span(filenameData,filenameCount))for(unsigned i=0;i<filenameCount;++i) {
        const auto c=host.MemoryRead8(filenameData+i);if(!c)break;filename+=c>=32 && c!=127?static_cast<char>(c):'?';
    }
    __android_log_print(ANDROID_LOG_ERROR,kTag,
        "qa-dlc-linker context root=%08X root_path=%s object=%08X object_path=%s property=%08X property_path=%s filename=%s export_index=%d import_index=%d export_linker=%08X package_linker=%08X import_linker=%08X",
        root,rootPath.text.c_str(),object,objectPath.text.c_str(),property,propertyPath.text.c_str(),filename.c_str(),
        static_cast<std::int32_t>(read.u32(read.base+0x02F3A1F8)),static_cast<std::int32_t>(read.u32(read.base+0x02F3A1FC)),
        read.u32(read.base+0x02F964C0),read.u32(read.base+0x02F964C4),read.u32(read.base+0x02F964C8));
    for(unsigned first=0;first<96;first+=8) {
        std::string words;
        for(unsigned i=first;i<first+8;++i) {char value[12]{};std::snprintf(value,sizeof(value)," %08X",read.u32(r[13]+i*4));words+=value;}
        __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-dlc-linker stack_word=%u%s",first,words.c_str());
    }
}

void qaLinkerActivate(Reader& read) {
    char enabled[PROP_VALUE_MAX]{};
    if(qaLinkerAttempted || __system_property_get("debug.bl2.dlc_linker_trace",enabled)<=0 || std::strcmp(enabled,"1"))return;
    qaLinkerAttempted=true;auto& host=read.host;auto& elf=host.GetElfLoader();const auto base=read.base;
    if(!inputVerified || elf.GetImageSize()!=kImageBytes || base>UINT32_MAX-kImageBytes ||
       elf.GetSymbolAddress("_ZN11ULinkerLoad13IndexToObjectEi")!=base+0x00FBB248 ||
       !elf.IsReadOnlyRange(base+qaLinkerHook.rva,8) || read.u32(base+qaLinkerHook.rva)!=qaLinkerHook.first ||
       read.u32(base+qaLinkerHook.rva+4)!=qaLinkerHook.second) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-dlc-linker refused identity/preimage");return;
    }
    const auto words=bl2::dlc::trace::Veneer(qaLinkerHook,base);
    const auto veneer=host.GuestAlloc(sizeof(words),8);if(!veneer)return;
    for(unsigned i=0;i<words.size();++i)host.MemoryWrite32(veneer+i*4,words[i]);
    host.AddExecutableRange(veneer,sizeof(words));host.InvalidateGuestCode(veneer,sizeof(words));
    const auto thunk=host.GetThunkManager().RegisterThunk(qaLinkerHook.thunk,[veneer](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
        jit.Regs()[15]=veneer+((jit.Cpsr()&0x200u)?16u:0u);qaLinkerObserve(jit,current);
    });
    if(!thunk)return;
    const auto svc=read.u32(thunk);if((svc&0xFF000000u)!=0xEF000000u || !(svc&0xFFFFFFu))return;
    host.MemoryWrite32(base+qaLinkerHook.rva,svc);host.InvalidateGuestCode(base+qaLinkerHook.rva,4);
    if(read.u32(base+qaLinkerHook.rva)!=svc || read.u32(base+qaLinkerHook.rva+4)!=qaLinkerHook.second)return;
    __android_log_print(ANDROID_LOG_INFO,kTag,"qa-dlc-linker armed rows=4 seam=00FBB3D4 bridge_only=1");
}

void qaMaterialActivate(Reader& read) {
    char enabled[PROP_VALUE_MAX]{};
    if(__system_property_get("debug.bl2.dlc_material_trace",enabled)<=0 || std::strcmp(enabled,"1")) return;
    if(qaMaterialInstalled) {
        if(!qaRefreshReadable())return;
        const auto now=bl2::touch::monotonicMilliseconds();qaMaterialBudget.Start(now);
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-dlc-material armed rows=128 window_ms=15000 hooks=2 bridge_only=1 reused=1 now_ms=%llu",
            static_cast<unsigned long long>(now));
        return;
    }
    if(qaMaterialAttempted)return;
    qaMaterialAttempted=true;
    const auto refused=[](const char* reason) { __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-dlc-material refused reason=%s",reason); };
    auto& host=read.host;auto& loader=host.GetElfLoader();const auto base=read.base;
    if(!inputVerified || loader.GetImageSize()!=kImageBytes || base>UINT32_MAX-kImageBytes) {refused("guest-identity");return;}
    for(const auto& [name,rva]:std::array<std::pair<const char*,std::uint32_t>,6>{{
        {"_ZN9UMaterial20CacheResourceShadersE15EShaderPlatformjj",0x0118126C},
        {"_ZN9FMaterial13InitShaderMapEP19FStaticParameterSet15EShaderPlatform",0x0120F490},
        {"_ZN18FMaterialShaderMap6FindIdERK19FStaticParameterSet15EShaderPlatform",0x01835AC0},
        {"_ZN5FName5NamesE",0x02FB00C8},{"_ZN7UObject11GObjObjectsE",0x02FBA268},{"GRHIShaderPlatform",0x03002390}}})
        if(loader.GetSymbolAddress(name)!=base+rva) {refused(name);return;}
    for(const auto& hook:bl2::dlc::trace::kHooks)
        if(!loader.IsReadOnlyRange(base+hook.rva,8) || read.u32(base+hook.rva)!=hook.first || read.u32(base+hook.rva+4)!=hook.second) {refused("hook-preimage");return;}
    if(read.u32(base+0x01181340)!=0xE12FFF33 || read.u32(base+0x01181344)!=0xE1A05000 ||
       read.u32(base+0x0120F518)!=0xE7963003 || read.u32(base+0x0120F51C)!=0xE12FFF33 ||
       read.u32(base+0x02F169F0)!=base+0x01835AC0) {refused("original-call-context");return;}
    if(!qaRefreshReadable()){refused("readable-map-snapshot");return;}
    std::array<std::uint32_t,2> veneers{},thunks{};
    for(unsigned i=0;i<2;++i) {
        const auto words=bl2::dlc::trace::Veneer(bl2::dlc::trace::kHooks[i],base);
        veneers[i]=host.GuestAlloc(sizeof(words),8);if(!veneers[i]){refused("veneer-allocation");return;}
        for(unsigned j=0;j<words.size();++j)host.MemoryWrite32(veneers[i]+j*4,words[j]);
        host.AddExecutableRange(veneers[i],sizeof(words));host.InvalidateGuestCode(veneers[i],sizeof(words));
    }
    // Late registration only, immediately before the first explicitly traced on-demand load.
    for(unsigned i=0;i<2;++i) {
        thunks[i]=host.GetThunkManager().RegisterThunk(bl2::dlc::trace::kHooks[i].thunk,
            [i,veneer=veneers[i]](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                jit.Regs()[15]=veneer+((jit.Cpsr()&0x200u)?16u:0u);
                qaMaterialObserve(i,jit,current);
            });
        if(!thunks[i]){refused("late-thunk-registration");return;}
    }
    std::array<std::uint32_t,2> instructions{};
    for(unsigned i=0;i<2;++i) {
        instructions[i]=read.u32(thunks[i]);
        if((instructions[i]&0xFF000000u)!=0xEF000000u || !(instructions[i]&0xFFFFFFu)) {refused("registered-svc-shape");return;}
    }
    for(unsigned i=0;i<2;++i) {
        const auto site=base+bl2::dlc::trace::kHooks[i].rva;
        const auto svc=instructions[i];
        // Inline the registered ARM instruction, not a data-endian-sensitive
        // literal jump. The callback resumes the owned exact-word veneer.
        host.MemoryWrite32(site,svc);host.InvalidateGuestCode(site,4);
        if(read.u32(site)!=svc || read.u32(site+4)!=bl2::dlc::trace::kHooks[i].second) {refused("hook-write-verification");return;}
    }
    qaMaterialInstalled=true;
    const auto now=bl2::touch::monotonicMilliseconds();qaMaterialBudget.Start(now);
    __android_log_print(ANDROID_LOG_INFO,kTag,"qa-dlc-material armed rows=128 window_ms=15000 hooks=2 bridge_only=1 now_ms=%llu",
        static_cast<unsigned long long>(now));
}

std::atomic<std::uint64_t> qaVoiceUntil{0};
std::atomic<unsigned> qaVoiceRows{0};
std::atomic<unsigned> qaVoiceWrapRows{0};
std::uint64_t qaVoiceStart=0;
std::uint32_t qaVoiceEvent=0,qaVoiceScratch=0;
bool qaVoiceInstalled=false;

void qaVoiceObserve(unsigned stage,Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    const auto now=bl2::touch::monotonicMilliseconds();
    if(now>qaVoiceUntil.load(std::memory_order_acquire))return;
    if(!qaVoiceEvent && !qaRefreshReadable())return;
    Reader read(host,qaMaterialSpan);const auto& regs=jit.Regs();
    const auto matches=[&](std::uint32_t info) {
        if(!read.span(info,0x34))return false;
        const auto event=read.u32(info);
        return qaVoiceEvent?event==qaVoiceEvent:
            read.object(event) && read.className(event)=="AkEvent" && bl2::voice::trace::ValidName(read.objectName(event));
    };
    if(stage==3) {
        if(!read.span(regs[4],0x28))return;
        const auto info=read.u32(regs[4]); // QueueSubtitles uses FPlayingSoundInfo* as its map key.
        if(!matches(info))return;
        auto row=qaVoiceWrapRows.fetch_add(1,std::memory_order_relaxed)+1;
        if(row>(qaVoiceEvent?bl2::voice::trace::kWrapRows:96u))return;
        const auto snapshot=bl2::voice::trace::ReadWrap(read,regs[13]);
        const auto perByte=bl2::voice::trace::Float(jit.ExtRegs()[18]);
        const auto duration=bl2::voice::trace::Float(jit.ExtRegs()[19]);
        const auto first=bl2::voice::trace::Float(jit.ExtRegs()[17]);
        const auto id=read.u32(info+0xC);
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice row=%u elapsed_ms=%llu stage=wrap info=%08X playing=%u ok=%u source_bytes=%u utf16_units=%u lines=%u source_cues=%u per_byte=%.9f duration=%.6f first=%.6f source_fnv=%08X preview_hex=%s",
            row,static_cast<unsigned long long>(now-qaVoiceStart),info,id,unsigned(snapshot.valid),snapshot.bytes,snapshot.units,snapshot.lines,
            read.u32(regs[4]+0x20),perByte,duration,first,snapshot.hash,snapshot.previewHex.c_str());
        if(snapshot.valid) {
            float elapsed=0.f;
            for(unsigned i=0;i<snapshot.lines;++i) {
                row=qaVoiceWrapRows.fetch_add(1,std::memory_order_relaxed)+1;
                if(row>(qaVoiceEvent?bl2::voice::trace::kWrapRows:96u))break;
                const float cue=first+elapsed;
                const float next=std::fma(float(snapshot.lineUnits[i]),perByte,elapsed);
                __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice row=%u elapsed_ms=%llu stage=wrapline info=%08X playing=%u index=%u units=%u cue_offset=%.6f cue_start=%.6f next_offset=%.6f",
                    row,static_cast<unsigned long long>(now-qaVoiceStart),info,id,i,snapshot.lineUnits[i],elapsed,cue,next);
                elapsed=next;
            }
        }
        return;
    }
    const auto info=stage==0?regs[3]:(stage==1?regs[12]:regs[1]);
    if(!matches(info))return;
    const auto row=qaVoiceRows.fetch_add(1,std::memory_order_relaxed)+1;
    if(row>bl2::voice::trace::kRows)return;
    const auto duration=bl2::voice::trace::Float(read.u32(info+0x24));
    const auto id=read.u32(info+0xC);
    if(stage==0) {
        const auto event=read.u32(info);
        if(!qaVoiceEvent && read.object(event) && read.className(event)=="AkEvent")
            __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice identity elapsed_ms=%llu info=%08X playing=%u event=%08X short_id=%u name=%s",
                static_cast<unsigned long long>(now-qaVoiceStart),info,id,event,read.u32(event+0x3C),read.objectName(event).c_str());
        const auto entry=regs[6];const bool valid=read.span(entry,28);
        const auto actual=valid?bl2::voice::trace::Float(read.u32(entry+0xC)):-1.f;
        const auto estimated=valid?bl2::voice::trace::Float(read.u32(entry+0x10)):-1.f;
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice row=%u elapsed_ms=%llu stage=duration info=%08X playing=%u stored=%.6f callback_ok=%u actual_ms=%.3f estimated_ms=%.3f",
            row,static_cast<unsigned long long>(now-qaVoiceStart),info,id,duration,unsigned(valid),actual,estimated);
    } else if(stage==1) {
        const auto stack=regs[13];const auto cues=read.u32(stack+0xA0),count=read.u32(stack+0xA4);
        const bool valid=count>0&&count<=512&&read.span(cues,std::size_t(count)*16);
        const auto first=valid?bl2::voice::trace::Float(read.u32(cues+12)):-1.f;
        const auto last=valid?bl2::voice::trace::Float(read.u32(cues+(count-1)*16+12)):-1.f;
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice row=%u elapsed_ms=%llu stage=subtitle info=%08X playing=%u stored=%.6f cues=%u cues_ok=%u first=%.6f last=%.6f",
            row,static_cast<unsigned long long>(now-qaVoiceStart),info,id,duration,count,unsigned(valid),first,last);
    } else {
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice row=%u elapsed_ms=%llu stage=end info=%08X playing=%u stored=%.6f",
            row,static_cast<unsigned long long>(now-qaVoiceStart),info,id,duration);
    }
}



bl2::animtree::trace::Budget qaAnimTreeBudget;
bool qaAnimTreeInstalled=false;
std::atomic<std::uint32_t> qaAnimTreeReadOwner{0};

void qaAnimTreeIdentity(const char* role,unsigned row,Reader& read,std::uint32_t object) {
    const bool valid=read.object(object);
    const auto path=bl2::dlc::trace::FullPath(read,object);
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "qa-animtree-id row=%u role=%s object=%08X object_ok=%u class=%s path_ok=%u path=%s",
        row,role,object,unsigned(valid),valid?read.className(object).c_str():"<invalid>",unsigned(path.valid),path.text.c_str());
}

void qaAnimTreeObserve(Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    const auto savedErrno=errno;
    struct RestoreErrno {int value;~RestoreErrno(){errno=value;}} restore{savedErrno};
    const auto now=bl2::touch::monotonicMilliseconds();
    if(!qaAnimTreeBudget.Open(now))return;
    const auto& r=jit.Regs();Reader read(host,qaMaterialSpan);
    if(qaAnimTreeReadOwner.load(std::memory_order_relaxed)!=r[4]||!read.object(r[4])) {
        if(!qaRefreshReadable())return;
        qaAnimTreeReadOwner.store(r[4],std::memory_order_relaxed);
    }
    auto s=bl2::animtree::trace::Capture(read,r[4],r[6],r[8],r[7]);
    if(s.componentOk&&!s.headerBounded) {
        if(!qaRefreshReadable())return;
        s=bl2::animtree::trace::Capture(read,r[4],r[6],r[8],r[7]);
    }
    unsigned row=0;std::uint64_t elapsed=0;
    const auto kind=bl2::animtree::trace::Classify(s);
    if(!qaAnimTreeBudget.Take(now,kind,row,elapsed))return;
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "qa-animtree row=%u elapsed_ms=%llu kind=%u component=%08X component_ok=%u component_stable=%u component_index=%u index=%u loop_count=%u animations=%08X header_readable=%u header_bounded=%u header_stable=%u data=%08X count=%u capacity=%u in_bounds=%u entry_read=%u node=%08X node_object=%u vtable_read=%u vtable=%08X dispatch_read=%u dispatch=%08X cached_tree=%08X cached_readable=%u cached_data=%08X cached_count=%u cached_capacity=%u",
        row,static_cast<unsigned long long>(elapsed),unsigned(kind),s.component,unsigned(s.componentOk),unsigned(s.componentStable),s.componentIndex,
        s.index,s.loopCount,s.animations,unsigned(s.headerReadable),unsigned(s.headerBounded),unsigned(s.headerStable),s.data,s.count,s.capacity,
        unsigned(s.indexInBounds),unsigned(s.entryRead),s.node,unsigned(s.nodeObject),unsigned(s.vtableRead),s.nodeVtable,unsigned(s.dispatchRead),s.nodeDispatch,
        s.cachedTree,unsigned(s.cachedHeaderReadable),s.cachedData,s.cachedCount,s.cachedCapacity);
    qaAnimTreeIdentity("component",row,read,s.component);
    qaAnimTreeIdentity("animations",row,read,s.animations);
    qaAnimTreeIdentity("node",row,read,s.node);
    if(s.cachedTree)qaAnimTreeIdentity("cached_tree_r7",row,read,s.cachedTree);
}

bool qaAnimTreeActivate(Reader& read) {
    namespace at=bl2::animtree::trace;
    auto& host=read.host;auto& loader=host.GetElfLoader();const auto base=read.base;
    const auto refused=[](const char* why){__android_log_print(ANDROID_LOG_ERROR,kTag,"qa-animtree refused reason=%s",why);return false;};
    if(!inputVerified||loader.GetImageSize()!=kImageBytes||base>UINT32_MAX-kImageBytes||
       loader.GetSymbolAddress(at::kSymbol)!=base+at::kRva||!loader.IsReadOnlyRange(base+at::kRva,16))return refused("guest-identity");
    for(unsigned i=0;i<at::kPrefix.size();++i)
        if(read.u32(base+at::kRva+i*4)!=at::kPrefix[i])return refused("original-prefix");
    if(!qaAnimTreeInstalled) {
        const auto& hook=at::kHook;const auto site=base+hook.rva;
        if(!loader.IsReadOnlyRange(site,8)||read.u32(site)!=hook.first||read.u32(site+4)!=hook.second)return refused("loop-preimage");
        if(!qaRefreshReadable())return refused("readable-map");
        const auto words=bl2::dlc::trace::Veneer(hook,base);
        const auto veneer=host.GuestAlloc(sizeof(words),8);if(!veneer)return refused("veneer-allocation");
        for(unsigned j=0;j<words.size();++j)host.MemoryWrite32(veneer+j*4,words[j]);
        host.AddExecutableRange(veneer,sizeof(words));host.InvalidateGuestCode(veneer,sizeof(words));
        // Opt-in registration only; existing startup thunk IDs are untouched.
        const auto thunk=host.GetThunkManager().RegisterThunk(hook.thunk,
            [veneer](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                jit.Regs()[15]=veneer+((jit.Cpsr()&0x200u)?16u:0u);qaAnimTreeObserve(jit,current);
            });
        if(!thunk)return refused("late-thunk");
        const auto instruction=read.u32(thunk);
        if((instruction&0xFF000000u)!=0xEF000000u||!(instruction&0xFFFFFFu))return refused("registered-svc");
        host.MemoryWrite32(site,instruction);host.InvalidateGuestCode(site,4);
        if(read.u32(site)!=instruction||read.u32(site+4)!=hook.second)return refused("write-verification");
        qaAnimTreeInstalled=true;
    }
    if(!qaRefreshReadable())return refused("readable-map");
    qaAnimTreeReadOwner.store(0,std::memory_order_relaxed);
    qaAnimTreeBudget.Start(bl2::touch::monotonicMilliseconds());
    __android_log_print(ANDROID_LOG_INFO,kTag,"qa-animtree armed=1 window_ms=120000 target46_first_rows=8 null_rows=8 loop_rva=01721EF8 bridge_only=1 passive=1");
    return true;
}

bl2::voice::fxtrace::Budget qaVoiceFxBudget;
bool qaVoiceFxInstalled=false;

void qaVoiceFxObserve(unsigned path,Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    const auto savedErrno=errno;
    struct RestoreErrno {int value;~RestoreErrno(){errno=value;}} restore{savedErrno};
    const auto now=bl2::touch::monotonicMilliseconds();const auto& r=jit.Regs();
    if(!qaVoiceFxBudget.Open(now)||r[6]!=0)return;
    Reader read(host,qaMaterialSpan);std::uint32_t id=0;
    // Cheap exact-node filter first. Refresh on a new mapping, and always
    // refresh again before capturing an eligible live descriptor.
    if(!bl2::voice::fxtrace::NodeId(read,r[4],id)) {
        if(!qaRefreshReadable()||!bl2::voice::fxtrace::NodeId(read,r[4],id))return;
    }
    if(!bl2::voice::fxtrace::Target(id)||!qaRefreshReadable())return;
    const auto s=bl2::voice::fxtrace::Capture(read,r[4],r[6],r[7],r[5]);
    if(!s.target)return;
    unsigned row=0;std::uint64_t elapsed=0;
    if(!qaVoiceFxBudget.Take(now,row,elapsed))return;
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "qa-voice-fx row=%u elapsed_ms=%llu return_path=%u node=%08X node_id=%u slot=%u gameobj=%08X descriptor=%08X descriptor_ok=%u fx=%08X fx_readable=%u fx_vtable=%08X bypass=%u chunk=%08X chunk_ok=%u declared_fx=%u shared=%u node_flags=%02X",
        row,static_cast<unsigned long long>(elapsed),path,s.node,s.nodeId,s.slot,s.gameObject,s.descriptor,
        unsigned(s.descriptorOk),s.fx,unsigned(s.fxReadable),s.fxVtable,s.bypass,s.chunk,unsigned(s.chunkOk),s.declaredFxId,s.shared,s.nodeFlags);
}

bool qaVoiceFxActivate(Reader& read) {
    auto& host=read.host;auto& loader=host.GetElfLoader();const auto base=read.base;
    const auto refused=[](const char* why){__android_log_print(ANDROID_LOG_ERROR,kTag,"qa-voice-fx refused reason=%s",why);return false;};
    if(!inputVerified||loader.GetImageSize()!=kImageBytes||base>UINT32_MAX-kImageBytes||
       loader.GetSymbolAddress(bl2::voice::fxtrace::kGetFxSymbol)!=base+bl2::voice::fxtrace::kGetFxRva||
       !loader.IsReadOnlyRange(base+bl2::voice::fxtrace::kGetFxRva,16))return refused("guest-identity");
    for(unsigned i=0;i<bl2::voice::fxtrace::kPrefix.size();++i)
        if(read.u32(base+bl2::voice::fxtrace::kGetFxRva+i*4)!=bl2::voice::fxtrace::kPrefix[i])return refused("original-prefix");
    if(!qaVoiceFxInstalled) {
        for(const auto& hook:bl2::voice::fxtrace::kHooks)
            if(!loader.IsReadOnlyRange(base+hook.rva,8)||read.u32(base+hook.rva)!=hook.first||
               read.u32(base+hook.rva+4)!=hook.second)return refused("return-preimage");
        std::array<std::uint32_t,2> veneers{},thunks{},instructions{};
        for(unsigned i=0;i<2;++i) {
            const auto words=bl2::dlc::trace::Veneer(bl2::voice::fxtrace::kHooks[i],base);
            veneers[i]=host.GuestAlloc(sizeof(words),8);if(!veneers[i])return refused("veneer-allocation");
            for(unsigned j=0;j<words.size();++j)host.MemoryWrite32(veneers[i]+j*4,words[j]);
            host.AddExecutableRange(veneers[i],sizeof(words));host.InvalidateGuestCode(veneers[i],sizeof(words));
        }
        // Appended only after existing voice QA registration, never startup.
        for(unsigned i=0;i<2;++i) {
            thunks[i]=host.GetThunkManager().RegisterThunk(bl2::voice::fxtrace::kHooks[i].thunk,
                [i,veneer=veneers[i]](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                    jit.Regs()[15]=veneer+((jit.Cpsr()&0x200u)?16u:0u);qaVoiceFxObserve(i,jit,current);
                });
            if(!thunks[i])return refused("late-thunk");
            instructions[i]=read.u32(thunks[i]);
            if((instructions[i]&0xFF000000u)!=0xEF000000u||!(instructions[i]&0xFFFFFFu))return refused("registered-svc");
        }
        for(unsigned i=0;i<2;++i) {
            const auto& hook=bl2::voice::fxtrace::kHooks[i];const auto site=base+hook.rva;
            host.MemoryWrite32(site,instructions[i]);host.InvalidateGuestCode(site,4);
            if(read.u32(site)!=instructions[i]||read.u32(site+4)!=hook.second)return refused("write-verification");
        }
        qaVoiceFxInstalled=true;
    }
    if(!qaRefreshReadable())return refused("readable-map");
    qaVoiceFxBudget.Start(bl2::touch::monotonicMilliseconds());
    __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice-fx armed=1 rows=32 window_ms=120000 node_ids=287859413,223536463 slot=0 hooks=2 bridge_only=1");
    return true;
}

bool qaVoiceActivate(Reader& read,std::uint32_t event) {
    auto& host=read.host;auto& loader=host.GetElfLoader();const auto base=read.base;
    if(!inputVerified || loader.GetImageSize()!=kImageBytes || base>UINT32_MAX-kImageBytes ||
       loader.GetSymbolAddress("_ZN18AWorldSoundManager19StaticPlayUIAkEventEP8UAkEvent")!=base+bl2::voice::trace::kPlayRva ||
       loader.GetSymbolAddress("_ZN16FSubtitleManager20SplitLinesToSafeZoneER8FIntRect")!=base+0x01796B3C)return false;
    for(unsigned i=0;i<bl2::voice::trace::kPlayPrefix.size();++i)
        if(read.u32(base+bl2::voice::trace::kPlayRva+i*4)!=bl2::voice::trace::kPlayPrefix[i])return false;
    if(!qaRefreshReadable())return false;
    if(!qaVoiceInstalled) {
        for(const auto& hook:bl2::voice::trace::kHooks)
            if(!loader.IsReadOnlyRange(base+hook.rva,8) || !bl2::voice::trace::HookMatches(hook,read.u32(base+hook.rva),read.u32(base+hook.rva+4)))return false;
        std::array<std::uint32_t,bl2::voice::trace::kHooks.size()> veneers{},thunks{},instructions{};
        for(unsigned i=0;i<bl2::voice::trace::kHooks.size();++i) {
            const auto words=bl2::dlc::trace::Veneer(bl2::voice::trace::kHooks[i],base);
            veneers[i]=host.GuestAlloc(sizeof(words),8);if(!veneers[i])return false;
            for(unsigned j=0;j<words.size();++j)host.MemoryWrite32(veneers[i]+j*4,words[j]);
            host.AddExecutableRange(veneers[i],sizeof(words));host.InvalidateGuestCode(veneers[i],sizeof(words));
        }
        for(unsigned i=0;i<bl2::voice::trace::kHooks.size();++i) {
            thunks[i]=host.GetThunkManager().RegisterThunk(bl2::voice::trace::kHooks[i].thunk,
                [i,veneer=veneers[i]](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                    jit.Regs()[15]=veneer+((jit.Cpsr()&0x200u)?16u:0u);
                    qaVoiceObserve(i,jit,current);
                });
            if(!thunks[i])return false;
            instructions[i]=read.u32(thunks[i]);
            if((instructions[i]&0xFF000000u)!=0xEF000000u || !(instructions[i]&0xFFFFFFu))return false;
        }
        for(unsigned i=0;i<bl2::voice::trace::kHooks.size();++i) {
            const auto site=base+bl2::voice::trace::kHooks[i].rva;
            host.MemoryWrite32(site,instructions[i]);host.InvalidateGuestCode(site,4);
            if(read.u32(site)!=instructions[i] || read.u32(site+4)!=bl2::voice::trace::kHooks[i].second)return false;
        }
        qaVoiceInstalled=true;
    }
    if(!qaVoiceScratch)qaVoiceScratch=host.GuestAlloc(8,8);
    if(!qaVoiceScratch)return false;
    host.MemoryWrite32(qaVoiceScratch,0);host.MemoryWrite32(qaVoiceScratch+4,0);
    qaVoiceEvent=event;qaVoiceStart=bl2::touch::monotonicMilliseconds();qaVoiceRows.store(0);qaVoiceWrapRows.store(0);
    qaVoiceUntil.store(qaVoiceStart+bl2::voice::trace::kWindowMs,std::memory_order_release);
    return true;
}
#endif

#if BL2_DLC_MATERIAL_TRACE_QA
bool qaIrisPinnedMetadata(Reader& read,const char* alias,std::size_t expectedBytes,const char* expectedSha) {
    if(!alias||!expectedBytes||expectedBytes>16384)return false;
    EnsureBl2OrchidMount(read.host);
    if(!Bl2OrchidOwnsApp(213210))return false;
    const auto* entry=FindBl2LocaleEntry(alias);
    if(!entry||entry->apk_fd<0||entry->file_size!=off_t(expectedBytes)||entry->data_offset<0||entry->bink)return false;
    struct stat st{};if(::fstat(entry->apk_fd,&st)||st.st_size<0)return false;
    const auto offset=std::uint64_t(entry->data_offset),total=std::uint64_t(st.st_size);
    if(offset>total||expectedBytes>total-offset||offset>std::uint64_t(std::numeric_limits<off_t>::max())-expectedBytes)return false;
    ObbSha256 hash;std::array<std::uint8_t,512> bytes{};std::size_t done=0;
    while(done<expectedBytes){
        const auto count=::pread(entry->apk_fd,bytes.data(),std::min(bytes.size(),expectedBytes-done),entry->data_offset+off_t(done));
        if(count<0&&errno==EINTR)continue;
        if(count<=0||!hash.Update(bytes.data(),std::size_t(count)))return false;
        done+=std::size_t(count);
    }
    const bool valid=std::strcmp(ObbSha256::Hex(hash.Finalize()).data(),expectedSha)==0;
    __android_log_print(valid?ANDROID_LOG_INFO:ANDROID_LOG_ERROR,kTag,
        "qa-irisfile alias=%s fd=%d offset=%lld bytes=%zu pin=%u retained_fd=1",alias,entry->apk_fd,
        static_cast<long long>(entry->data_offset),done,valid?1:0);
    // The immutable VFS owns this FD. Never close it here.
    return valid;
}
#endif

void qaConsoleExec(Reader& read, const Observation& result) {
    const auto now = bl2::touch::monotonicMilliseconds();
    if (!qaExecVerified || now - qaPollMs < 250) return;
    qaPollMs = now;
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.bl2bridge.qa_exec", value);
    const std::string request(value);
    const auto separator = request.find(':');
    if (request == qaLastExec || separator == std::string::npos || separator + 1 >= request.size()) return;
    qaLastExec = request;
    const auto command = request.substr(separator + 1);
    const auto device = read.u32(read.base + kGLog);
    if (!read.object(result.local) || !read.span(device, 4) || !read.span(read.u32(device), 4)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "qa-exec refused (no local player/log device): %s", command.c_str());
        return;
    }
    if (!qaScratch) qaScratch = read.host.GuestAlloc(PROP_VALUE_MAX + 8, 8);
    if (!qaScratch) return;
    for (std::size_t i = 0; i <= command.size(); ++i)
        read.host.MemoryWrite8(qaScratch + std::uint32_t(i), i < command.size() ? std::uint8_t(command[i]) : 0);
    if (qaMirrorAttach(read)) {
        qaMirrorLines.store(0, std::memory_order_relaxed);
        qaMirrorUntil.store(std::max(now + 3000, qaMirrorUntil.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    }
#if BL2_DLC_MATERIAL_TRACE_QA
    if(command=="@iriscdo") {
        const auto& state=bl2::iris::wheeled::Current();
        const bool valid=inputVerified&&state.Host==&read.host&&state.CallbacksInstalled&&
            read.host.GetElfLoader().GetImageSize()==kImageBytes;
        const bool globals=valid&&read.span(read.base+0x02FFA2A8,4)&&read.span(read.base+0x02F3C100,4);
        const std::uint32_t firstGc=globals?read.u32(read.base+0x02FFA2A8):0;
        const std::uint32_t lastNonGc=globals?read.u32(read.base+0x02F3C100):0;
        const auto snapshot=[&](const char* label,std::uint32_t descriptor,std::uint32_t expected) {
            const bool descOk=valid&&read.span(descriptor,0xF8);
            const std::uint32_t current=descOk?read.u32(descriptor+0xF0):0;
            __android_log_print(ANDROID_LOG_INFO,kTag,
                "qa-iriscdo label=%s valid=%u desc=%08X desc_ok=%u flags=%08X:%08X classflags=%08X current=%08X original=%08X binding_stage=%u vtable=%08X class=%08X objectindex=%08X gc_globals=%u first_gc=%08X last_nongc=%08X",
                label,unsigned(valid),descriptor,unsigned(descOk),descOk?read.u32(descriptor+0xC):0,
                descOk?read.u32(descriptor+8):0,descOk?read.u32(descriptor+0xC0):0,current,expected,
                unsigned(state.Binding.Stage),descOk?read.u32(descriptor):0,descOk?read.u32(descriptor+0x34):0,
                descOk?read.u32(descriptor+0x20):0,unsigned(globals),firstGc,lastNonGc);
            const auto object=[&](const char* which,std::uint32_t address) {
                const bool readable=valid&&read.span(address,0x3C);
                std::array<std::uint32_t,15> words{};
                if(readable)for(unsigned i=0;i<words.size();++i)words[i]=read.u32(address+i*4);
                const bool stable=descOk&&read.u32(descriptor+0xF0)==current;
                __android_log_print(ANDROID_LOG_INFO,kTag,
                    "qa-iriscdo-object label=%s which=%s ptr=%08X readable=%u stable=%u vtable=%08X word04=%08X flags=%08X:%08X word10=%08X word14=%08X linker=%08X linkerindex=%08X objectindex=%08X word24=%08X outer=%08X name0=%08X name1=%08X class=%08X archetype=%08X",
                    label,which,address,unsigned(readable),unsigned(stable),words[0],words[1],words[3],words[2],
                    words[4],words[5],words[6],words[7],words[8],words[9],words[10],words[11],words[12],words[13],words[14]);
            };
            object("binding",expected);
            if(current!=expected)object("current",current);
        };
        snapshot("native",state.Binding.Descriptor,state.Binding.DefaultObject);
        const auto& activation=bl2::iris::activation::Current();
        snapshot("script",activation.ScriptClass,activation.ScriptCdo);
        return;
    }
    if(command=="@irisnodebind") {
        // Exact QA-only loose input; no generic file path/property execution.
        constexpr char metadataPath[]="DLC/Iris/Lic/Content/Iris_WheeledPawnSupplement_B.xxx";
        constexpr char expectedSha[]="84A00CB868C0933915F7EBA17CB4A865D099B9BB643D586F83F01AF45D8C26ED";
        constexpr std::size_t expectedBytes=4911;
        struct Original {std::uint32_t rva;const char* symbol;std::array<std::uint32_t,4> prefix;};
        constexpr std::array<Original,8> originals{{
            {0x00F5D9C8,"_ZN6UClassC1E18EStaticConstructormmmPKcS2_S2_yPFvPvEM7UObjectFvvES8_",{0xE92D4FF0,0xE24DD00C,0xE1CD44D0,0xE1A07002}},
            {0x00F60C08,"_Z28InitializePrivateStaticClassP6UClassS0_S0_",{0xE92D4038,0xE1510000,0xE59F5080,0x03A00000}},
            {0x01000AE0,"_ZN7UObject14GetInitializedEv",{0xE59F3008,0xE08F3003,0xE5930024,0xE12FFF1E}},
            {0x01007DAC,"_ZN7UObject16StaticFindObjectEP6UClassPS_PKcj",{0xE59FC1B0,0xE92D43F0,0xE1A09003,0xE59F41A8}},
            {0x0100D228,"_ZN7UObject11LoadPackageEP8UPackagePKcm",{0xE92D4FF0,0xE1A08000,0xE5D10000,0xE24DD054}},
            {0x010160F0,"_ZN7UObject19InternalConstructorEPv",{0xE59F300C,0xE08F3003,0xE2833008,0xE5803000}},
            {0x0132938C,"_ZN18UAnimNodeBlendBase8TickAnimEf",{0xE92D4070,0xE1A05000,0xE59060B8,0xE3560000}},
            {0x0132A9D8,"_ZN9UAnimNode16OnBecomeRelevantEv",{0xE92D4038,0xE1A04000,0xE5D03040,0xE3130040}},
        }};
        auto& host=read.host;const auto& elf=host.GetElfLoader();
        bool valid=inputVerified&&elf.GetImageSize()==kImageBytes&&read.base<=UINT32_MAX-kImageBytes;
        for(const auto& original:originals){
            const auto pc=read.base+original.rva;
            valid=valid&&elf.GetSymbolAddress(original.symbol)==pc&&elf.IsReadOnlyRange(pc,16);
            for(unsigned i=0;valid&&i<4;++i)valid=read.u32(pc+i*4)==original.prefix[i];
        }
        const bool fileValid=qaIrisPinnedMetadata(read,metadataPath,expectedBytes,expectedSha);
        if(!valid||!fileValid){
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-irisnodebind refused guestpin=%u filepin=%u",valid?1:0,fileValid?1:0);
            return;
        }
        const auto binding=bl2::iris::wheeled::Initialize(host,metadataPath,true);
        const auto& state=bl2::iris::wheeled::Current().Binding;
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-irisnodebind called=1 result=%u stage=%u base_extent=%X linked_extent=%X class=%08X package=%08X cdo=%08X ready=%u",
            unsigned(binding),unsigned(state.Stage),state.BaseExtent,state.LinkedExtent,state.Descriptor,state.Package,state.DefaultObject,
            binding==wheeled_pawn_arm_candidate::BindingResult::Ready?1:0);
        return;
    }
    if(command=="@irisactionbind") {
        const auto& bound=bl2::iris::wheeled::Current().Binding;
        if(bound.Stage!=wheeled_pawn_arm_candidate::BindingStage::Ready){
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-irisactionbind refused native-class-not-ready");return;
        }
        constexpr char metadataPath[]="DLC/Iris/Lic/Content/Iris_BikeMoveScriptSupplement_B.xxx";
        constexpr char expectedSha[]="C46BBFC7C6DE046BF9E9D096C47FB37CBCDD07595F22D3A7798A0132B9C10665";
        constexpr std::size_t expectedBytes=11061;
        if(!qaIrisPinnedMetadata(read,metadataPath,expectedBytes,expectedSha)){
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-irisactionbind refused filepin");return;
        }
        auto& host=read.host;
        const auto scratch=host.GuestAlloc(512,16);
        if(!scratch)return;
        struct Release{DynarmicHost& h;std::uint32_t p;~Release(){h.GuestFree(p);}}release{host,scratch};
        bl2::iris::wheeled::HostApi api{host};
        const auto put=[&](unsigned off,const char* value){for(unsigned i=0;;++i){host.MemoryWrite8(scratch+off+i,std::uint8_t(value[i]));if(!value[i])break;}};
        put(0,metadataPath);put(160,"WillowGame.Action_BikeMove");put(224,"WillowGame.Default__Action_BikeMove");put(288,"WillowGame.WillowActionSequencePawn");
        const std::uint32_t loadArgs[]={0,scratch,0};std::uint32_t package=0,cls=0,cdo=0,parent=0;
        bool called=api.Call(read.base+0x0100D228,loadArgs,3,package);
        const auto find=[&](unsigned off,std::uint32_t& out){const std::uint32_t args[]={0,UINT32_MAX,scratch+off,0};return api.Call(read.base+0x01007DAC,args,4,out);};
        bool found=called&&package&&find(160,cls)&&cls&&find(224,cdo)&&cdo&&find(288,parent)&&parent;
        bool fields=found&&read.span(cls,0xF4)&&read.span(cdo,0x38)&&
            read.u32(cls+0xF0)==cdo&&read.u32(cdo+0x34)==cls&&read.u32(cls+0x40)==parent;
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-irisactionbind called=%u package=%08X class=%08X cdo=%08X parent=%08X fields=%u ready=%u",
            called?1:0,package,cls,cdo,parent,fields?1:0,fields?1:0);
        return;
    }
    if(command=="@irisctorcheck") {
        struct Original {std::uint32_t rva;const char* symbol;std::array<std::uint32_t,4> prefix;};
        constexpr std::array<Original,3> originals{{
            {0x00F5D9C8,"_ZN6UClassC1E18EStaticConstructormmmPKcS2_S2_yPFvPvEM7UObjectFvvES8_",{0xE92D4FF0,0xE24DD00C,0xE1CD44D0,0xE1A07002}},
            {0x00FFBAEC,"_ZN7UObjectC1E18EStaticConstructorPKcS2_y",{0xE92D0030,0xEDDF1B1C,0xE3A01000,0xE59F5070}},
            {0x01000AE0,"_ZN7UObject14GetInitializedEv",{0xE59F3008,0xE08F3003,0xE5930024,0xE12FFF1E}}
        }};
        const auto& elf=read.host.GetElfLoader();
        bool valid=inputVerified&&elf.GetImageSize()==kImageBytes&&read.base<=UINT32_MAX-kImageBytes;
        for(const auto& original:originals) {
            const auto pc=read.base+original.rva;
            valid=valid&&elf.GetSymbolAddress(original.symbol)==pc&&elf.IsReadOnlyRange(pc,16);
            for(unsigned i=0;valid&&i<4;++i)valid=read.u32(pc+i*4)==original.prefix[i];
        }
        std::uint32_t initialized=0;
        if(!valid||!read.host.CallGuest(read.base+0x01000AE0,{},initialized)||initialized!=1) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-irisctor refused identity-or-initialization");return;
        }
        auto& host=read.host;
        const auto buffer=host.GuestAlloc(512,16);
        if(!buffer)return;
        struct FreeBuffer {DynarmicHost& host;std::uint32_t p;~FreeBuffer(){host.GuestFree(p);}} release{host,buffer};
        if(std::uint64_t{buffer}+512>UINT32_MAX)return;
        for(unsigned i=0;i<512;++i)host.MemoryWrite8(buffer+i,0);
        const auto put=[&](unsigned offset,const char* text) {
            for(unsigned i=0;text[i];++i)host.MemoryWrite8(buffer+offset+i,std::uint8_t(text[i]));
        };
        put(0x180,"BL2IrisCtorProbe");put(0x1A0,"WillowGame");put(0x1B0,"Engine");
        const auto b=read.base;
        const std::vector<std::uint32_t> words{buffer,0,0x100,0,
            0,buffer+0x180,buffer+0x1A0,buffer+0x1B0,0x4000,0x04084084,
            b+0x010160F0,b+0x00FF94D8,0,b+0x00FF94DC,0};
        const auto pendingBefore=read.u32(b+0x02FBA278);
        std::uint32_t returned=0;
        const bool called=host.CallGuest(b+0x00F5D9C8,words,returned);
        bool fields=called&&returned==buffer;
        constexpr std::array<unsigned,16> offsets{0x08,0x0C,0x20,0x28,0x2C,0x34,0x40,0x48,0xC0,0xC4,0xCC,0xF4,0xF8,0xFC,0x100,0x104};
        const std::array<std::uint32_t,16> expected{0x4000,0x04084084,UINT32_MAX,buffer+0x1A0,buffer+0x180,
            0,0,0x01000100,0x80,0,0,b+0x010160F0,b+0x00FF94D8,0,b+0x00FF94DC,0};
        for(unsigned i=0;i<offsets.size();++i)fields=fields&&host.MemoryRead32(buffer+offsets[i])==expected[i];
        fields=fields&&host.MemoryRead32(buffer+0xD0)==buffer+0x1B0;
        const bool pendingUnchanged=pendingBefore==read.u32(b+0x02FBA278);
        std::vector<std::uint32_t> boundary(32);
        for(unsigned i=0;i<boundary.size();++i)boundary[i]=0x5A000000+i;
        std::uint32_t boundaryResult=0;
        const bool maxAccepted=host.CallGuest(b+0x01000AE0,boundary,boundaryResult)&&boundaryResult==1;
        boundary.push_back(0);boundaryResult=0xA5A5A5A5;
        const bool overRejected=!host.CallGuest(b+0x01000AE0,boundary,boundaryResult)&&boundaryResult==0xA5A5A5A5;
        // This temporary descriptor is never initialized/registered or exposed to engine ownership.
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-irisctor called=%u words=%zu returned=%08X expected=%08X fields=%u pending_unchanged=%u max32=%u reject33=%u registered=0 cdo=0 pass=%u",
            unsigned(called),words.size(),returned,buffer,unsigned(fields),unsigned(pendingUnchanged),
            unsigned(maxAccepted),unsigned(overRejected),unsigned(fields&&pendingUnchanged&&maxAccepted&&overRejected));
        return;
    }
    if(command=="@animtreeobserve") {qaAnimTreeActivate(read);return;}
    if(command=="@voiceobserve") {
        if(!qaVoiceActivate(read,0)) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-voiceobserve refused original-seams");return;
        }
        qaVoiceUntil.store(qaVoiceStart+120000,std::memory_order_release);
        qaVoiceFxActivate(read);
        qaSubtitleRows.store(0,std::memory_order_relaxed);
        qaSubtitleUntil.store(qaVoiceStart+120000,std::memory_order_release);
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voiceobserve armed=1 window_ms=120000 manual_playback=0");return;
    }
    if(command=="@spawnerror") {
        constexpr std::uint32_t siteRva=0x0146A9B8,targetRva=0x00FD6C98;
        const auto& elf=read.host.GetElfLoader();const auto site=read.base+siteRva;
        if(qaSpawnErrorInstalled)return;
        if(!inputVerified || elf.GetImageSize()!=kImageBytes ||
           elf.GetSymbolAddress("_Z22appFailAssertFuncDebugPKcS0_iS0_z")!=read.base+targetRva ||
           !elf.IsReadOnlyRange(site,8) || read.u32(site)!=0xE12FFF3C || read.u32(site+4)!=0xE51F35B8) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-spawnerror refused identity/preimage");return;
        }
        const auto thunk=read.host.GetThunkManager().RegisterThunk("bl2_qa_loadmap_spawn_error",
            [base=read.base](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                const auto target=jit.Regs()[12],sp=jit.Regs()[13];
                std::string error;std::uint32_t count=0,capacity=0,pointer=0;
                if(qaRefreshReadable()) {
                    Reader memory(current,qaMaterialSpan);
                    pointer=memory.u32(sp);count=memory.u32(sp+0x94);capacity=memory.u32(sp+0x98);
                    if(target==base+0x00FD6C98 && jit.Regs()[2]==3476 && count && count<=2048 &&
                       count<=capacity && capacity<=8192 && pointer==memory.u32(sp+0x90) && memory.span(pointer,count)) {
                        for(unsigned i=0;i+1<count && i<512;++i) {
                            const auto c=current.MemoryRead8(pointer+i);
                            if(!c)break;
                            error+=c>=32?static_cast<char>(c):'?';
                        }
                    }
                }
                __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-spawnerror original_target=%08X pointer=%08X count=%u capacity=%u error=%s",
                    target,pointer,count,capacity,error.c_str());
                // Replay the replaced original BLX IP, including interworking.
                jit.Regs()[14]=base+0x0146A9BC;
                jit.SetCpsr((jit.Cpsr()&~0x20u)|((target&1u)<<5));
                jit.Regs()[15]=target&((target&1u)?~1u:~3u);
            });
        const auto instruction=thunk?read.u32(thunk):0;
        if((instruction&0xFF000000u)!=0xEF000000u || !(instruction&0xFFFFFFu))return;
        read.host.MemoryWrite32(site,instruction);read.host.InvalidateGuestCode(site,4);
        qaSpawnErrorInstalled=read.u32(site)==instruction && read.u32(site+4)==0xE51F35B8;
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-spawnerror armed=%u site=%08X",unsigned(qaSpawnErrorInstalled),site);
        return;
    }
    if(command=="@gloglong") {
        qaMirrorUntil.store(now+90000,std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-glog explicit_window_ms=90000");return;
    }
    if(command=="@oasiscache") {
        qaCacheSnapshot("explicit",read.host);
        const auto& elf=read.host.GetElfLoader();bool valid=inputVerified && elf.GetImageSize()==kImageBytes &&
            elf.GetSymbolAddress(bl2::dlc::cacheqa::kFindIdSymbol)==read.base+bl2::dlc::cacheqa::kFindIdRva;
        for(unsigned i=0;valid && i<bl2::dlc::cacheqa::kFindIdPrefix.size();++i)
            valid=read.u32(read.base+bl2::dlc::cacheqa::kFindIdRva+i*4)==bl2::dlc::cacheqa::kFindIdPrefix[i];
        if(!valid) {__android_log_print(ANDROID_LOG_ERROR,kTag,"qa-oasis-lookup refused identity/preimage");return;}
        for(unsigned i=0;i<bl2::dlc::cacheqa::kEmptyStaticKey.size();++i)read.host.MemoryWrite32(qaScratch+i*4,bl2::dlc::cacheqa::kEmptyStaticKey[i]);
        std::uint32_t map=0;
        const bool called=bl2::touch::Bl2TouchCallGuest(read.host,read.base+bl2::dlc::cacheqa::kFindIdRva,{qaScratch,6},map);
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-oasis-lookup expected=5a730b019ab22d4fb49316dd3fc5aed8 platform=6 called=%u result=%08X",unsigned(called),map);
        return;
    }
    if(command=="@orchidstate") {
        if(!qaRefreshReadable())return;
        Reader memory(read.host,qaMaterialSpan);
        std::uint32_t objects=0,count=0,manager=0,package=0,managers=0,packages=0;
        if(!memory.array(memory.base+0x02FBA268,2000000,objects,count))return;
        for(std::uint32_t i=0;i<count;++i) {
            const auto p=memory.u32(objects+i*4);if(!p)continue;
            if(!memory.object(p))return;
            if(memory.objectName(p)=="WillowDownloadableContentManager_0" &&
               bl2::dlc::cacheqa::Class(memory,p,"WillowDownloadableContentManager","WillowGame")) {manager=p;++managers;}
            if(memory.objectName(p)=="PackageDef_Orchid" &&
               memory.className(p)=="DownloadablePackageDefinition" &&
               memory.objectName(memory.u32(p+0x28))=="GD_OrchidPackageDef") {package=p;++packages;}
        }
        const auto s=bl2::dlc::contentqa::Capture(memory,managers==1?manager:0,packages==1?package:0);
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-orchidstate managers=%u packages=%u objects_ok=%u header_ok=%u valid=%u found=%u manager=%08X package=%08X storage=%u capacity=%u hash_data=%08X hash_size=%u visited=%u",
            managers,packages,unsigned(s.objectsOk),unsigned(s.headerOk),unsigned(s.valid),unsigned(s.found),
            manager,package,s.storageNum,s.capacity,s.hashData,s.hashSize,s.visited);
        if(s.valid && s.found)__android_log_print(ANDROID_LOG_INFO,kTag,
            "qa-orchidstate masks state=%08X index=%u package_mask=%u licensed=%u installed=%u fully_licensed=%u fully_installed=%u",
            s.state,s.index,s.packageMask,s.licensed,s.installed,unsigned(s.fullyLicensed),unsigned(s.fullyInstalled));
        return;
    }
    if(command=="@dlcpaths") {
        constexpr std::uint32_t convertRva=0x01E223AC,destroyRva=0x00F288E8;
        constexpr std::array<std::uint32_t,4> convertWords{{0xE92D4FF0,0xE1A01002,0xE24DD014,0xE59F5414}};
        constexpr std::array<std::uint32_t,4> destroyWords{{0xE92D4030,0xE1A04000,0xE5901000,0xE24DD00C}};
        const auto& elf=read.host.GetElfLoader();
        bool valid=inputVerified && elf.GetImageSize()==kImageBytes &&
            elf.GetSymbolAddress("_ZN19FFileManagerAndroid21ConvertToAbsolutePathEPKc")==read.base+convertRva &&
            elf.GetSymbolAddress("_ZN7FStringD1Ev")==read.base+destroyRva &&
            elf.GetSymbolAddress("GFileManager")==read.base+0x02F91330 &&
            elf.GetSymbolAddress("GAndroidSystemMemory")==read.base+0x03057590;
        for(unsigned i=0;valid && i<4;++i)
            valid=read.u32(read.base+convertRva+i*4)==convertWords[i] && read.u32(read.base+destroyRva+i*4)==destroyWords[i];
        const auto manager=read.u32(read.base+0x02F91330);
        if(!valid || !read.span(manager,4)) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-dlcpaths refused identity/manager");return;
        }
        const auto stringAt=[&](std::uint32_t p) {
            const auto data=read.u32(p),count=read.u32(p+4),capacity=read.u32(p+8);
            std::string out;
            if(!count || count>512 || count>capacity || capacity>4096 || !read.span(data,count))return out;
            for(unsigned i=0;i<count;++i) {
                const auto c=read.host.MemoryRead8(data+i);
                if(!c)return i+1==count?out:std::string{};
                if(c<32 || c>126)return std::string{};
                out+=static_cast<char>(c);
            }
            return std::string{};
        };
        const auto root=stringAt(read.base+0x03057590+0xF4);
        constexpr char relative[]="../../DLC/";
        for(unsigned i=0;i<3;++i)read.host.MemoryWrite32(qaScratch+i*4,0);
        for(unsigned i=0;i<sizeof(relative);++i)read.host.MemoryWrite8(qaScratch+32+i,relative[i]);
        std::uint32_t ignored=0;
        const bool called=bl2::touch::Bl2TouchCallGuest(read.host,read.base+convertRva,{qaScratch,manager,qaScratch+32},ignored);
        const auto absolute=called?stringAt(qaScratch):std::string{};
        const bool destroyed=called && bl2::touch::Bl2TouchCallGuest(read.host,read.base+destroyRva,{qaScratch},ignored);
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-dlcpaths original_called=%u destroyed=%u manager=%08X system_root=%s relative=%s absolute=%s",
            unsigned(called),unsigned(destroyed),manager,root.c_str(),relative,absolute.c_str());
        const auto* children=Bl2OrchidChildren(absolute.c_str());
        const auto content=absolute+"Orchid/Lic/Content/Orchid_OasisTown_P.xxx";
        const auto* map=FindBl2LocaleEntry(content.c_str());
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-dlcmount local_owned=%u root_children=%zu main_map_region=%u",
            unsigned(Bl2OrchidOwnsApp(207870)),children?children->size():0,unsigned(map!=nullptr));
        return;
    }
    if(command=="@travelrefresh") {
        constexpr std::uint32_t getRva=0x0224EA60,invalidateRva=0x02252474;
        constexpr std::array<std::uint32_t,7> getWords{{0xE59F3010,0xE79F3003,0xE5930000,0xE3500000,0x15900140,0xE12FFF1E,0x00CC53E0}};
        constexpr std::array<std::uint32_t,3> invalidateWords{{0xE3A03000,0xE5803134,0xE12FFF1E}};
        const auto& elf=read.host.GetElfLoader();
        bool valid=inputVerified && elf.GetImageSize()==kImageBytes &&
            elf.GetSymbolAddress("_ZN14UWillowGlobals22StaticGetWillowGlobalsEv")==read.base+getRva &&
            elf.GetSymbolAddress("_ZN14UWillowGlobals37InvalidateMasterFastTravelStationListEv")==read.base+invalidateRva;
        for(unsigned i=0;valid && i<getWords.size();++i)valid=read.u32(read.base+getRva+i*4)==getWords[i];
        for(unsigned i=0;valid && i<invalidateWords.size();++i)valid=read.u32(read.base+invalidateRva+i*4)==invalidateWords[i];
        std::uint32_t globals=0,ignored=0;
        if(!valid || !bl2::touch::Bl2TouchCallGuest(read.host,read.base+getRva,{},globals) ||
           !bl2::dlc::cacheqa::Class(read,globals,"WillowGlobals","WillowGame") || !read.span(globals,0x138)) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-travelrefresh refused identity/globals");return;
        }
        const auto before=read.u32(globals+0x134);
        const bool called=bl2::touch::Bl2TouchCallGuest(read.host,read.base+invalidateRva,{globals},ignored);
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-travelrefresh original_called=%u globals=%08X lookup_before=%08X lookup_after=%08X",
            unsigned(called),globals,before,read.u32(globals+0x134));
        return;
    }
    if(command=="@orchidvoice" || command=="@orchidsfx") {
        const bool localized=command=="@orchidvoice";
        constexpr std::array<std::uint32_t,4> getPrefix{0xE92D4008,0xE59F3028,0xE79F3003,0xE5930000};
        constexpr std::array<std::uint32_t,4> loadPrefix{0xE92D4010,0xE3520000,0xE24DD010,0xE3A02000};
        constexpr std::uint32_t getRva=0x01FA263C,loadRva=0x01FA8160;
        const auto& elf=read.host.GetElfLoader();
        bool valid=inputVerified && (!localized || Bl2RussianLocaleSelected()) && elf.GetImageSize()==kImageBytes &&
            elf.GetSymbolAddress("_ZN14UAkAudioDevice3GetEv")==read.base+getRva &&
            elf.GetSymbolAddress("_ZN14UAkAudioDevice15LoadFilePackageERK7FStringjRj")==read.base+loadRva;
        for(unsigned i=0;valid && i<4;++i)
            valid=read.u32(read.base+getRva+i*4)==getPrefix[i] && read.u32(read.base+loadRva+i*4)==loadPrefix[i];
        for(const char* name:{"Audio_Banks_Orchid.pck","Audio_Streaming_Orchid.pck"}) {
            const std::string path=std::string("WillowGame/CookedAndroid_DXT/")+(localized?"English(US)/":"")+name;
            valid=valid && IsBl2LocalePath(path.c_str());
        }
        std::uint32_t device=0;
        if(!valid || !bl2::touch::Bl2TouchCallGuest(read.host,read.base+getRva,{},device) || !read.span(device,4)) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-%s refused validated_payload=%u device=%08X",localized?"orchidvoice":"orchidsfx",unsigned(valid),device);return;
        }
        for(const char* name:{"Audio_Banks_Orchid.pck","Audio_Streaming_Orchid.pck"}) {
            const auto size=std::strlen(name)+1;
            if(size+16>PROP_VALUE_MAX+8)return;
            for(std::size_t i=0;i<size;++i)read.host.MemoryWrite8(qaScratch+16+std::uint32_t(i),std::uint8_t(name[i]));
            read.host.MemoryWrite32(qaScratch,qaScratch+16);
            read.host.MemoryWrite32(qaScratch+4,std::uint32_t(size));read.host.MemoryWrite32(qaScratch+8,std::uint32_t(size));
            read.host.MemoryWrite32(qaScratch+12,0);
            std::uint32_t accepted=0;
            const bool called=bl2::touch::Bl2TouchCallGuest(read.host,read.base+loadRva,{device,qaScratch,unsigned(localized),qaScratch+12},accepted);
            __android_log_print(ANDROID_LOG_INFO,kTag,"qa-%s load name=%s localized=%u called=%u accepted=%u package_id=%u",
                localized?"orchidvoice":"orchidsfx",name,unsigned(localized),unsigned(called),accepted,read.u32(qaScratch+12));
            if(!called || accepted!=1)return;
        }
        return;
    }
    if(command.rfind("@voice ",0)==0) {
        const auto name=command.substr(7);const auto match=bl2::voice::trace::FindEvent(read,name);
        if(match.count!=1 || !qaVoiceActivate(read,match.event)) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-voice refused name=%s matches=%u",name.c_str(),match.count);return;
        }
        const bool called=bl2::touch::Bl2TouchCallGuest(read.host,read.base+bl2::voice::trace::kPlayRva,{qaVoiceScratch,match.event});
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voice play name=%s short_id=%u called=%u source=%08X playing=%u start_ms=%llu",
            name.c_str(),read.u32(match.event+0x3C),unsigned(called),read.u32(qaVoiceScratch),read.u32(qaVoiceScratch+4),
            static_cast<unsigned long long>(qaVoiceStart));
        return;
    }
#endif
#if BL2_ENABLE_QA_CONSOLE
    if(command.rfind("@voiceplay ",0)==0) {
        const auto name=command.substr(11);const auto match=bl2::voice::trace::FindEvent(read,name);
        const auto& elf=read.host.GetElfLoader();
        bool valid=inputVerified && match.count==1 && elf.GetImageSize()==kImageBytes &&
            elf.GetSymbolAddress("_ZN18AWorldSoundManager19StaticPlayUIAkEventEP8UAkEvent")==read.base+bl2::voice::trace::kPlayRva;
        for(unsigned i=0;valid && i<bl2::voice::trace::kPlayPrefix.size();++i)
            valid=read.u32(read.base+bl2::voice::trace::kPlayRva+i*4)==bl2::voice::trace::kPlayPrefix[i];
        if(!valid) { __android_log_print(ANDROID_LOG_ERROR,kTag,"qa-voiceplay refused name=%s matches=%u",name.c_str(),match.count);return; }
        read.host.MemoryWrite32(qaScratch,0);read.host.MemoryWrite32(qaScratch+4,0);
        const auto start=bl2::touch::monotonicMilliseconds();qaSubtitleRows.store(0);
        qaSubtitleUntil.store(start+15000,std::memory_order_release);
        const bool called=bl2::touch::Bl2TouchCallGuest(read.host,read.base+bl2::voice::trace::kPlayRva,{qaScratch,match.event});
        __android_log_print(ANDROID_LOG_INFO,kTag,"qa-voiceplay play name=%s short_id=%u called=%u source=%08X playing=%u start_ms=%llu",
            name.c_str(),read.u32(match.event+0x3C),unsigned(called),read.u32(qaScratch),read.u32(qaScratch+4),
            static_cast<unsigned long long>(start));
        if(!called)qaSubtitleUntil.store(0);
        return;
    }
#endif
    if (command.rfind("@ondemand ", 0) == 0) {
        // FString {Data, Num incl. NUL, Max} after the text; the callee copies it.
        const auto text = command.size() - 10 + 1;
        const auto string = (qaScratch + std::uint32_t(command.size()) + 4) & ~3u;
        if (!qaOnDemandVerified || !read.object(result.controller) || string + 12 > qaScratch + PROP_VALUE_MAX + 8) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "qa-exec @ondemand refused: %s", command.c_str());
            return;
        }
        read.host.MemoryWrite32(string, qaScratch + 10);
        read.host.MemoryWrite32(string + 4, std::uint32_t(text));
        read.host.MemoryWrite32(string + 8, std::uint32_t(text));
#if BL2_DLC_MATERIAL_TRACE_QA
        if(command=="@ondemand Orchid_OasisShaderCache_SF")qaCacheActivate(read);
        qaLinkerActivate(read);
        qaMaterialActivate(read);
#endif
        const bool called = bl2::touch::Bl2TouchCallGuest(read.host, read.base + kClientLoadOnDemand,
            {result.controller, string});
        __android_log_print(ANDROID_LOG_INFO, kTag, "qa-exec tag=%s ondemand='%s' called=%u", request.substr(0, separator).c_str(),
            command.substr(10).c_str(), called ? 1u : 0u);
        return;
    }
    std::uint32_t netModeAt = 0;
    std::uint8_t netMode = 0;
    if (command.rfind("@standalone ", 0) == 0) {
        const auto world = read.u32(read.base + 0x0300D940);
        const auto level = read.object(world) ? read.u32(world + 0x50) : 0;
        const auto info = read.object(level) ? read.u32(read.u32(level + 0x3C)) : 0;
        if (!qaStandaloneVerified || read.className(info) != "WorldInfo" || !read.span(info + 0x370, 1) ||
            (netMode = read.host.MemoryRead8(info + 0x370)) > 3) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "qa-exec @standalone refused: %s", command.c_str());
            return;
        }
        netModeAt = info + 0x370;
        const auto rest = command.substr(12);
        for (std::size_t i = 0; i <= rest.size(); ++i)
            read.host.MemoryWrite8(qaScratch + std::uint32_t(i), i < rest.size() ? std::uint8_t(rest[i]) : 0);
        read.host.MemoryWrite8(netModeAt, 0);
    }
    std::uint32_t handled = 0;
    const bool called = bl2::touch::Bl2TouchCallGuest(read.host, read.base + kLocalPlayerExec,
        {result.local, qaScratch, device}, handled);
    if (netModeAt) read.host.MemoryWrite8(netModeAt, netMode);
    __android_log_print(ANDROID_LOG_INFO, kTag, "qa-exec tag=%s command='%s' called=%u handled=%u netmode=%u", request.substr(0, separator).c_str(),
        command.c_str(), called ? 1u : 0u, handled, netModeAt ? unsigned(netMode) : 255u);
}
#endif

bool inputBoundary(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    if (!active) return false;
    if (insideProbe) return bl2::iris::activation::Current().Inside;
    insideProbe = true;
    struct Guard { ~Guard() { insideProbe = false; } } guard;
    // Keys sent from here are the touch route's; ProcessInput's own body
    // handles the queued device keys after this hook returns.
    const bl2::touch::TouchDispatchScope dispatching;
    SyncBl2GraphicsSettings(host);
    SyncBl2PerformanceSettings();
    Bl2PerfHintGameThread();
    const auto tick = inputTicks.fetch_add(1, std::memory_order_relaxed) + 1;
    Reader read(host);
    const auto result = observe(read, jit.Regs()[0]);
    const auto iris=bl2::iris::activation::Tick(host,
        inputVerified,bl2::touch::monotonicMilliseconds());
#if HORN_ENABLE_LOGGING || BL2_ENABLE_QA_CONSOLE
    static auto priorIris=bl2::iris::activation::Result::NotApplicable;static unsigned irisRows=0;
    if(iris!=priorIris&&irisRows<8) {
        priorIris=iris;++irisRows;
        __android_log_print(iris==bl2::iris::activation::Result::Failed?ANDROID_LOG_ERROR:ANDROID_LOG_INFO,
            "BL2IrisNativeNode","iris-activation result=%u original_input=%u rows=%u",unsigned(iris),
            unsigned(bl2::iris::activation::AllowInput(iris)),irisRows);
    }
#endif
    if(iris==bl2::iris::activation::Result::Failed)
        bl2::iris::wheeled::CallbackFailure(wheeled_pawn_arm_candidate::Status::ClassUnavailable,"frontend-activation");
    if(!bl2::iris::activation::AllowInput(iris))return true;
    if (uiModelObserverEnabled) ProbeBl2UiModel(host, result.movieOwner, result.identity);
    bl2::touch::Context context;
    context.mode = result.mode;
    context.sceneIdentity = result.identity;
    if (gameplaySink) {
        gameplaySink->expirePendingAxes();
        // Injured uses the same original PlayerWalking axes. Its Pawn state
        // keeps the native crawl/zero-speed timing and ProcessInjuredViewRotation.
        // Native14 witnessed that the OnFoot-only gate suppressed both axes in
        // FFYL. Class contexts (Siren's added SirenOnFoot, every action skill,
        // Mercenary FFYL) keep the same route; Execute keeps actions only.
        // Focused GFx and unlisted or 3+ contexts stay denied.
        const bool gameplayActions = bl2::touch::Bl2TouchGameplayActions(result.mode,result.movie,result.contexts);
        const bool gameplayAxes = bl2::touch::Bl2TouchGameplayAxes(result.mode,result.movie,result.contexts);
        // InputContext_Cinematic holds SkipCinematic and ShowMenu (PauseMenu):
        // the same resolver and IsActionAvailable, no axes and no HUD telemetry.
        // Skip stays only while ServerSkipMatinee could skip the camera matinee.
        const bool cinematicActions = bl2::touch::Bl2TouchCinematicActions(result.mode,result.movie,result.contexts);
        // Vehicle seats resolve their own shipped actions (ExitVehicle,
        // SwitchSeats, Handbrake, Afterburner, Horn, DriverFireTurret, LookBack,
        // Fire, Aim) and add the stick to the RawJoy fields PlayerDriving reads.
        const bool vehicleActions = bl2::touch::Bl2TouchVehicleActions(result.mode,result.movie,result.contexts);
        const bool vehicleAxes = bl2::touch::Bl2TouchVehicleAxes(result.mode,result.movie,result.contexts);
        const bool routed = gameplayActions || vehicleActions || cinematicActions;
        using Scene = bl2::touch::Ue3GameplaySink::Scene;
        const auto scene = gameplayActions ? Scene::Gameplay : (vehicleActions ? Scene::Vehicle : Scene::Cinematic);
        const auto mask = gameplaySink->prepare(routed ? result.controller : 0, routed ? result.input : 0,
            result.identity, result.viewport, gameplayAxes || vehicleAxes, scene);
        if (cinematicActions && gameplaySink->ready()) {
            context.availableActions = mask & (bl2::touch::actionBit(bl2::touch::Action::Skip) |
                bl2::touch::actionBit(bl2::touch::Action::Pause));
            if (context.availableActions) context.capabilities = bl2::touch::CanSkip;
        }
        if (gameplayActions && gameplaySink->ready()) {
            context.capabilities = bl2::touch::GameplayInput | (gameplayAxes ? bl2::touch::RelativeLook : 0u);
            // Protocol 5: weapon slot cells over the ammo strip, the FFYL
            // give-up and the status tabs opened by HUD taps.
            context.availableActions = mask & (((std::uint64_t{1} << 15) - 1) |
                bl2::touch::actionBit(bl2::touch::Action::LastWeapon) |
                bl2::touch::actionBit(bl2::touch::Action::Chat) |
                bl2::touch::actionBit(bl2::touch::Action::Slot1) | bl2::touch::actionBit(bl2::touch::Action::Slot2) |
                bl2::touch::actionBit(bl2::touch::Action::Slot3) | bl2::touch::actionBit(bl2::touch::Action::Slot4) |
                bl2::touch::actionBit(bl2::touch::Action::Suicide) |
                bl2::touch::actionBit(bl2::touch::Action::Skills) |
                bl2::touch::actionBit(bl2::touch::Action::Logs) |
                bl2::touch::actionBit(bl2::touch::Action::Challenges));
            gameplaySink->telemetry(context);
        }
        if (vehicleActions && gameplaySink->ready()) {
            context.capabilities = bl2::touch::GameplayInput | (vehicleAxes ? bl2::touch::RelativeLook : 0u);
            context.availableActions = mask & (((std::uint64_t{1} << 15) - 1) |
                bl2::touch::actionBit(bl2::touch::Action::VehicleBrake) |
                bl2::touch::actionBit(bl2::touch::Action::VehicleBoost) |
                bl2::touch::actionBit(bl2::touch::Action::VehicleHorn) |
                bl2::touch::actionBit(bl2::touch::Action::LastWeapon) |
                bl2::touch::actionBit(bl2::touch::Action::Chat) |
                bl2::touch::actionBit(bl2::touch::Action::ExitVehicle) |
                bl2::touch::actionBit(bl2::touch::Action::SwitchSeats) |
                bl2::touch::actionBit(bl2::touch::Action::DriverFireTurret) |
                bl2::touch::actionBit(bl2::touch::Action::LookBack));
        }
        if (uiSink || widgetSink) {
            const auto caps = widgetSink ? widgetSink->prepare(result.viewport,result.controller,result.movie,result.movieOwner,result.identity) :
                uiSink->prepare(result.viewport, result.controller, result.movie,result.movieOwner, result.identity);
            if (caps) {
                context.capabilities = caps;
                context.availableActions = (caps & bl2::touch::DebugCanvas) ?
                    bl2::touch::kDebugMenuActions : bl2::touch::kUiActions;
                context.mode = bl2::touch::Bl2TouchNativeDialogMovie(result.movieClass) ? bl2::touch::Mode::Dialog :
                    (result.movieClass == "FrontendGFxMovie" ? bl2::touch::Mode::Frontend : bl2::touch::Mode::Menu);
            }
        }
        if (context.mode == bl2::touch::Mode::Menu || context.mode == bl2::touch::Mode::Dialog) {
#if HORN_ENABLE_LOGGING
            std::string pauseReason;
            const auto diagnostic = &pauseReason;
#else
            std::string* diagnostic = nullptr;
#endif
            const bool paused = bl2::touch::Bl2TouchPausedWorld({&read,Reader::copyMemory},read.base,result.controller,
                result.movieClass == "PauseGFxMovie" ? result.movieOwner : 0,diagnostic);
            if (paused) context.capabilities |= bl2::touch::PauseBackdrop;
#if HORN_ENABLE_LOGGING
            static std::string lastPauseReason; static unsigned pauseLogs = 0;
            const auto state = result.movieClass + "/" + pauseReason;
            if (pauseLogs < 24 && state != lastPauseReason) {
                lastPauseReason = state; ++pauseLogs;
                HORN_LOGI("BL2TouchDiagnostics","pause backdrop active=%u movie=%s owner=%08X controller=%08X reason=%s",
                    paused ? 1u : 0u,result.movieClass.c_str(),result.movieOwner,result.controller,pauseReason.c_str());
            }
#endif
        }
        if (bl2::touch::physicalOwnership().ownsInput(bl2::touch::monotonicMilliseconds())) {
            context.capabilities = bl2::touch::PhysicalOwnsInput;
            context.availableActions = 0;
        }
    }
    publishedMode.store(static_cast<std::uint32_t>(context.mode), std::memory_order_relaxed);
    publishedModeMs.store(bl2::touch::monotonicMilliseconds(), std::memory_order_relaxed);
    // UE3's periodic garbage collection waits for menu frames (bl2_gc_defer.cpp).
    Bl2GcDeferTick(host, context.mode == bl2::touch::Mode::Menu || context.mode == bl2::touch::Mode::Dialog ||
                             context.mode == bl2::touch::Mode::TextInput ||
                             context.mode == bl2::touch::Mode::Frontend);
    // HIDE GAMEPAD BUTTONS rewrites the StringAliasMap button images (bl2_touch_gamepad_glyphs.cpp).
    Bl2TouchGamepadGlyphsTick(host);
    auto& mailbox = bl2::touch::sharedMailbox();
    [[maybe_unused]] const auto epoch = mailbox.publishContext(context);
    if (combinedSink) bl2::touch::consumeIntoEngine(mailbox, tick,
        bl2::touch::monotonicMilliseconds(), *combinedSink);
    else if (gameplaySink) bl2::touch::consumeIntoEngine(mailbox, tick,
        bl2::touch::monotonicMilliseconds(), *gameplaySink);
    else (void)mailbox.consumeFrame(tick, bl2::touch::monotonicMilliseconds());
#if HORN_ENABLE_LOGGING || BL2_ENABLE_QA_CONSOLE
    qaConsoleExec(read, result);
#endif
    [[maybe_unused]] const auto snapshot = mailbox.snapshot();
    const std::string contexts = joined(result.contexts), actions = joined(result.available);
    const auto description = std::to_string(result.identity) + '/' + result.reason + '/' + contexts + '/' + actions + '/' + std::to_string(context.capabilities);
    std::lock_guard<std::mutex> lock(observationMutex);
    if (description == lastObservation) return false;
    lastObservation = description;
    if (logs.fetch_add(1, std::memory_order_relaxed) >= 64) return false;
    HORN_LOGI(kTag, "native-touch observation tick=%llu epoch=%llu frame=%llu viewport=%08X world=%08X "
        "controller=%08X input=%08X movie=%08X owner=%08X movieClass='%s' mode=%u reason=%s "
        "contexts=[%s] available=[%s] capabilities=%u delivery=%s",
        static_cast<unsigned long long>(tick), static_cast<unsigned long long>(epoch),
        static_cast<unsigned long long>(snapshot.frameSerial), result.viewport, result.world,
        result.controller, result.input, result.movie, result.movieOwner, result.movieClass.c_str(),
#if defined(BL2_NATIVE_ARM32)
        static_cast<unsigned>(context.mode), result.reason.c_str(), contexts.c_str(), actions.c_str(),
        context.capabilities, widgetSink ? "native-widgets" : (uiSink ? "native-ui-test" : (gameplaySink ? "gameplay-test" : "disabled")));
#else
        static_cast<unsigned>(result.mode), result.reason.c_str(), contexts.c_str(), actions.c_str(),
        context.capabilities, uiSink ? "native-ui-test" : (gameplaySink ? "gameplay-test" : "disabled"));
#endif
    return false;
}

void lifetimeBoundary(Dynarmic::A32::Jit&, DynarmicHost&) {
    lifetimeGeneration.fetch_add(1, std::memory_order_relaxed);
    bl2::touch::sharedMailbox().reset(bl2::touch::ContextChanged);
}
void inputDestroyed(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    if (gameplaySink) gameplaySink->retireInput(jit.Regs()[0]);
    lifetimeBoundary(jit, host);
}

} // namespace

bool InstallBl2SubtitleTiming(DynarmicHost& host) {
    static bool ready=false;
    if(ready)return true;
    const auto& elf=host.GetElfLoader();const auto base=elf.GetBaseAddress();
    if(!inputVerified || elf.GetImageSize()!=kImageBytes || std::uint64_t(base)+kImageBytes>host.GetGuestMemorySize() ||
       elf.GetSymbolAddress("_ZN16FSubtitleManager20SplitLinesToSafeZoneER8FIntRect")!=base+bl2::subtitle::kFunctionRva)return false;
    const auto site=base+bl2::subtitle::kRva;
    if(!elf.IsReadOnlyRange(site,8) || !bl2::subtitle::Matches(host.MemoryRead32(site),host.MemoryRead32(site+4)))return false;
    const auto words=bl2::subtitle::Replay(base);
#if defined(BL2_NATIVE_ARM32)
    const auto replay=host.CreateCodeVeneer(words);
    if(!replay)return false;
#else
    const auto replay=host.GuestAlloc(sizeof(words),8);
    if(!replay)return false;
    for(unsigned i=0;i<words.size();++i)host.MemoryWrite32(replay+i*4,words[i]);
    host.AddExecutableRange(replay,sizeof(words));host.InvalidateGuestCode(replay,sizeof(words));
#endif
    const auto thunk=host.GetThunkManager().RegisterThunk("bl2_subtitle_utf16_timing",
        [replay](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
            Reader read(current);
#if BL2_ENABLE_QA_CONSOLE
            const auto oldDenominator=jit.ExtRegs()[18];
#endif
            jit.ExtRegs()[18]=bl2::subtitle::Denominator(read,jit.Regs()[4],jit.Regs()[13],jit.ExtRegs()[18],jit.ExtRegs()[19]);
#if BL2_ENABLE_QA_CONSOLE
            if(oldDenominator!=jit.ExtRegs()[18] && bl2::touch::monotonicMilliseconds()<=qaSubtitleUntil.load(std::memory_order_acquire) &&
               qaSubtitleRows.fetch_add(1,std::memory_order_relaxed)<8)
                __android_log_print(ANDROID_LOG_INFO,kTag,"qa-subtitle denominator old=%08X corrected=%08X duration=%08X bytes=%u source_cues=%u",
                    oldDenominator,jit.ExtRegs()[18],jit.ExtRegs()[19],read.u32(jit.Regs()[13]+0x84)-1,read.u32(jit.Regs()[4]+0x20));
#endif
            jit.Regs()[15]=replay+((jit.Cpsr()&0x200u)?16u:0u);
        });
    if(!thunk)return false;
#if !defined(BL2_NATIVE_ARM32)
    const auto instruction=host.MemoryRead32(thunk);
    if((instruction&0xFF000000u)!=0xEF000000u || !(instruction&0xFFFFFFu))return false;
#endif
    // Publish only after both the dispatcher and exact original replay exist.
    host.MemoryWrite32(site,0xE51FF004);host.MemoryWrite32(site+4,thunk);host.InvalidateGuestCode(site,8);
    if(host.MemoryRead32(site)!=0xE51FF004 || host.MemoryRead32(site+4)!=thunk)return false;
    ready=true;return true;
}

std::uint64_t Bl2TouchLifetimeGeneration() { return currentLifetimeGeneration(); }

bool Bl2TouchPlayerInWorld() {
    const std::uint64_t published = publishedModeMs.load(std::memory_order_relaxed);
    if (!published || bl2::touch::monotonicMilliseconds() - published > 750) return false;
    const auto mode = static_cast<bl2::touch::Mode>(publishedMode.load(std::memory_order_relaxed));
    return mode == bl2::touch::Mode::Gameplay || mode == bl2::touch::Mode::Vehicle ||
           mode == bl2::touch::Mode::Cinematic;
}

bool VerifyBl2TouchInput(const std::uint8_t* bytes, std::size_t size) {
    inputVerified = false;
    if (!bytes || size != kFileBytes) return false;
    ObbSha256::Digest digest{};
    if (!ObbSha256::OfMapping(bytes, size, digest)) return false;
    const auto hex = ObbSha256::Hex(digest);
    inputVerified = std::strcmp(hex.data(), kSha) == 0;
    if (!inputVerified) HORN_LOGE(kTag, "native-touch input refused: sha256=%s", hex.data());
    return inputVerified;
}

bool InstallBl2TouchRuntime(DynarmicHost& host) {
    if (installed) return true;
    char mode[PROP_VALUE_MAX] = {};
    __system_property_get("debug.bl2.native_touch", mode);
    if (std::strcmp(mode,"off")==0) return true;
    const bool nativeDefault=!*mode || std::strcmp(mode,"0")==0 || std::strcmp(mode,"native")==0;
    const bool tapTest = std::strcmp(mode, "ui-tap-test") == 0;
    const bool uiDelivery = nativeDefault || tapTest || std::strcmp(mode, "ui-test") == 0;
    const bool delivery = uiDelivery || std::strcmp(mode, "gameplay-test") == 0;
    if (std::strcmp(mode, "observe") != 0 && !delivery) {
        HORN_LOGE(kTag, "native-touch mode refused; valid modes: 0, observe, gameplay-test, ui-test, ui-tap-test");
        return false;
    }
    char modelMode[PROP_VALUE_MAX] = {};
    __system_property_get("debug.bl2bridge.ui_model", modelMode);
    uiModelObserverEnabled = std::strcmp(modelMode, "observe") == 0;
    const auto& elf = host.GetElfLoader();
    const auto base = elf.GetBaseAddress();
    if (!inputVerified || elf.GetImageSize() != kImageBytes ||
        std::uint64_t(base) + kImageBytes > host.GetGuestMemorySize()) return false;
    for (const auto& global : globals) {
        if (elf.GetSymbolAddress(global.symbol) != base + global.rva) {
            HORN_LOGE(kTag, "native-touch global signature mismatch: %s", global.symbol); return false;
        }
    }
    for (const auto& hook : hooks) {
        if (elf.GetSymbolAddress(hook.symbol) != base + hook.rva) return false;
        for (std::size_t i = 0; i < hook.prefix.size(); ++i) {
            if (host.MemoryRead32(base + hook.rva + std::uint32_t(i * 4)) != hook.prefix[i]) {
                HORN_LOGE(kTag, "native-touch preimage mismatch: %s +%zu", hook.symbol, i * 4); return false;
            }
        }
    }
    // Guard the actual leaf invoked from the input hook in addition to SHA/RVA.
    constexpr std::array<std::uint32_t, 4> availability = {0xE92D0070,0xE5903290,0xE3530000,0x1A000002};
    for (std::size_t i = 0; i < availability.size(); ++i)
        if (host.MemoryRead32(base + 0x0235679C + std::uint32_t(i * 4)) != availability[i]) return false;
    // UWillowPlayerInput::InputKey, the original X button route of touch Use.
    constexpr std::array<std::uint32_t, 4> inputKey = {0xE92D40F0,0xE1A04000,0xE5D0017C,0xE24DD01C};
    if (elf.GetSymbolAddress("_ZN18UWillowPlayerInput8InputKeyEi5FName11EInputEventfj") != base + 0x0236E058) return false;
    for (std::size_t i = 0; i < inputKey.size(); ++i)
        if (host.MemoryRead32(base + 0x0236E058 + std::uint32_t(i * 4)) != inputKey[i]) return false;
#if HORN_ENABLE_LOGGING || BL2_ENABLE_QA_CONSOLE
    qaExecVerified = elf.GetSymbolAddress("_ZN12ULocalPlayer4ExecEPKcR13FOutputDevice") == base + kLocalPlayerExec &&
        elf.GetSymbolAddress("GLog") == base + kGLog;
    for (std::size_t i = 0; qaExecVerified && i < kLocalPlayerExecPrefix.size(); ++i)
        qaExecVerified = host.MemoryRead32(base + kLocalPlayerExec + std::uint32_t(i * 4)) == kLocalPlayerExecPrefix[i];
    if (!qaExecVerified) __android_log_print(ANDROID_LOG_ERROR, kTag, "qa-exec unavailable: ULocalPlayer::Exec/GLog signature mismatch");
    qaMirrorVerified = qaExecVerified &&
        elf.GetSymbolAddress("_ZN23FOutputDeviceRedirector15AddOutputDeviceEP13FOutputDevice") == base + kAddOutputDevice &&
        elf.GetSymbolAddress("_ZTV17FOutputDeviceNull") == base + kNullDeviceVtable;
    for (std::size_t i = 0; qaMirrorVerified && i < kAddOutputDevicePrefix.size(); ++i)
        qaMirrorVerified = host.MemoryRead32(base + kAddOutputDevice + std::uint32_t(i * 4)) == kAddOutputDevicePrefix[i];
    if (!qaMirrorVerified) __android_log_print(ANDROID_LOG_ERROR, kTag, "qa-glog unavailable: FOutputDeviceRedirector/FOutputDeviceNull signature mismatch");
    qaOnDemandVerified = qaExecVerified &&
        elf.GetSymbolAddress("_ZN23AWillowPlayerController25ClientLoadOnDemandPackageERK7FString") == base + kClientLoadOnDemand;
    for (std::size_t i = 0; qaOnDemandVerified && i < kClientLoadOnDemandPrefix.size(); ++i)
        qaOnDemandVerified = host.MemoryRead32(base + kClientLoadOnDemand + std::uint32_t(i * 4)) == kClientLoadOnDemandPrefix[i];
    qaStandaloneVerified = qaExecVerified && elf.GetSymbolAddress("_ZNK6UWorld10GetNetModeEv") == base + kWorldGetNetMode;
    for (std::size_t i = 0; qaStandaloneVerified && i < kWorldGetNetModeWords.size(); ++i)
        qaStandaloneVerified = host.MemoryRead32(base + kWorldGetNetMode + std::uint32_t(i * 4)) == kWorldGetNetModeWords[i];
    // A request left over from an earlier process is never replayed.
    char staleRequest[PROP_VALUE_MAX] = {};
    __system_property_get("debug.bl2bridge.qa_exec", staleRequest);
    qaLastExec = staleRequest;
#endif

    std::array<std::uint32_t, sizeof(hooks) / sizeof(hooks[0])> sites{};
#if defined(BL2_NATIVE_ARM32)
    std::array<std::uint32_t, sites.size()> dispatchers{};
    for (std::size_t i = 0; i < sites.size(); ++i) {
        const auto& hook = hooks[i];
        const auto site = base + hook.rva + hook.offset;
        sites[i] = site;
        const std::array<std::uint32_t, 4> words = {
            host.MemoryRead32(site), host.MemoryRead32(site + 4), 0xE51FF004, site + 8};
        const auto continuation = host.CreateCodeVeneer(words);
        if (!continuation) return false;
        const ThunkHandler handler = i == 0 ? ThunkHandler(inputBoundary) :
            (i == 3 ? ThunkHandler(inputDestroyed) : ThunkHandler(lifetimeBoundary));
        dispatchers[i] = host.GetThunkManager().RegisterThunk(hook.thunk,
            [handler,continuation,i](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
                if(i==0&&inputBoundary(jit,current)) {
                    bl2::iris::activation::ReturnBeforePrologue<true>(jit);
                    return;
                }
                if(i!=0)handler(jit,current);
                jit.Regs()[15] = continuation;
            });
        if (!dispatchers[i]) return false;
    }
    for (std::size_t i = 0; i < sites.size(); ++i) {
        host.MemoryWrite32(sites[i], 0xE51FF004);
        host.MemoryWrite32(sites[i] + 4, dispatchers[i]);
        host.InvalidateGuestCode(sites[i], 8);
    }
#else
    const auto code = host.GuestAlloc(sizeof(hooks) / sizeof(hooks[0]) * 20, 4);
    if (!code) return false;
    // Prepare and validate every veneer before touching any original entry.
    for (std::size_t i = 0; i < sites.size(); ++i) {
        const auto& hook = hooks[i];
        const auto thunk = host.GetThunkManager().RegisterThunk(hook.thunk,
            i == 0 ? ThunkHandler([](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                if(inputBoundary(jit,current))bl2::iris::activation::ReturnBeforePrologue<false>(jit);
            }) : (i == 3 ? ThunkHandler(inputDestroyed) : ThunkHandler(lifetimeBoundary)));
        const auto svc = host.MemoryRead32(thunk);
        if (!thunk || (svc & 0xFF000000) != 0xEF000000) return false;
        const auto veneer = code + std::uint32_t(i * 20);
        const auto site = base + hook.rva + hook.offset;
        sites[i] = site;
        host.MemoryWrite32(veneer + 0, svc);
        host.MemoryWrite32(veneer + 4, host.MemoryRead32(site));
        host.MemoryWrite32(veneer + 8, host.MemoryRead32(site + 4));
        host.MemoryWrite32(veneer + 12, 0xE51FF004); // ldr pc,[pc,#-4]
        host.MemoryWrite32(veneer + 16, site + 8);
    }
    host.AddExecutableRange(code, static_cast<std::uint32_t>(sites.size() * 20));
    host.InvalidateGuestCode(code, sites.size() * 20);
    for (std::size_t i = 0; i < sites.size(); ++i) {
        host.MemoryWrite32(sites[i], 0xE51FF004);
        host.MemoryWrite32(sites[i] + 4, code + std::uint32_t(i * 20));
        host.InvalidateGuestCode(sites[i], 8);
    }
#endif
    active = installed = true;
    if (delivery) {
        gameplaySink = std::make_unique<bl2::touch::Ue3GameplaySink>(host, currentLifetimeGeneration);
        if (!InstallBl2TouchGameplayHooks(host, gameplaySink.get())) {
            gameplaySink.reset();
            HORN_LOGE(kTag, "native-touch gameplay hooks unavailable; observer/fallback retained");
            return false;
        }
    }
    if(nativeDefault && gameplaySink) {
        widgetSink=std::make_unique<bl2::touch::Ue3WidgetSink>(host,currentLifetimeGeneration);
        if(!widgetSink->verified()) { widgetSink.reset();return false; }
        combinedSink=std::make_unique<bl2::touch::CombinedSink>(*gameplaySink,*widgetSink);
    } else if (uiDelivery && gameplaySink) {
        uiSink = std::make_unique<bl2::touch::Ue3UiSink>(host, currentLifetimeGeneration, tapTest);
        if (!uiSink->verified()) {
            uiSink.reset();
            HORN_LOGE(kTag, "native-touch UI signatures unavailable; gameplay/fallback retained");
        } else combinedSink = std::make_unique<bl2::touch::CombinedSink>(*gameplaySink, *uiSink);
    }
    HORN_LOGI(kTag, "native-touch stage=%u mode=%s sha256=%s hooks=%zu boundedChanges=64",
#if defined(BL2_NATIVE_ARM32)
              widgetSink ? 4u : (uiSink ? 3u : (gameplaySink ? 2u : 1u)),
#else
              uiSink ? 3u : (gameplaySink ? 2u : 1u),
#endif
              mode, kSha, sites.size() + (gameplaySink ? 2 : 0));
    return true;
}
