#include "bl2_touch_runtime.h"
#include "bl2_touch_gameplay_sink.h"
#include "bl2_touch_gameplay_hooks.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_ui_sink.h"
#include "bl2_touch_widget_sink.h"
#include "bl2_touch_combined_sink.h"
#include "bl2_touch_ui_model_probe.h"
#include "bl2_touch_ui_model.h"
#include "bl2_pause_debug_menu.h"
#include "bl2_graphics_settings.h"
#include "bl2_performance_settings.h"
#include "bl2_perf_hint.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include <memory>

#include "bridge_log.h"
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

namespace {

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
    explicit Reader(DynarmicHost& value) : host(value), base(value.GetElfLoader().GetBaseAddress()) {}
    bool span(std::uint32_t p, std::size_t n) const {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() &&
               n <= host.GetGuestMemorySize() - p;
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
    std::string name(std::uint32_t fname) const {
        if (!span(fname, 8)) return {};
        std::uint32_t names = 0, count = 0;
        if (!array(base + 0x02FB00C8, 2000000, names, count)) return {};
        const auto index = u32(fname);
        if (index >= count) return {};
        const auto entry = u32(names + index * 4);
        if (!span(entry, 8 + 128)) return {};
        std::string result;
        for (std::uint32_t i = 0; i != 128; ++i) {
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
    if (nativeMenuOpen || (out.movie && out.movieClass != "WillowHUDGFxMovie")) out.mode = bl2::touch::Mode::Menu;
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

#if HORN_ENABLE_LOGGING
// Diagnostic builds only, for device QA of class contexts, skills and vehicles:
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
        line += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    HORN_LOGI(kTag, "qa-glog event=%u %s", jit.Regs()[2], line.c_str());
}

bool qaMirrorAttach(Reader& read) {
    if (qaMirror) return true;
    if (!qaMirrorVerified || !qaMirrorThunk) return false;
    for (std::size_t i = 0; i < kNullDeviceSlots.size(); ++i)
        if (read.u32(read.base + kNullDeviceVtable + 8 + std::uint32_t(i * 4)) != read.base + kNullDeviceSlots[i]) {
            qaMirrorVerified = false;
            HORN_LOGE(kTag, "qa-glog unavailable: FOutputDeviceNull vtable slot %zu mismatch", i);
            return false;
        }
    const auto redirector = read.u32(read.base + kGLog);
    if (!read.span(redirector, 0x44)) return false;
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
    HORN_LOGI(kTag, "qa-glog mirror attached device=%08X redirector=%08X", device, redirector);
    return true;
}

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
        HORN_LOGE(kTag, "qa-exec refused (no local player/log device): %s", command.c_str());
        return;
    }
    if (!qaScratch) qaScratch = read.host.GuestAlloc(PROP_VALUE_MAX + 8, 8);
    if (!qaScratch) return;
    for (std::size_t i = 0; i <= command.size(); ++i)
        read.host.MemoryWrite8(qaScratch + std::uint32_t(i), i < command.size() ? std::uint8_t(command[i]) : 0);
    if (qaMirrorAttach(read)) {
        qaMirrorLines.store(0, std::memory_order_relaxed);
        qaMirrorUntil.store(now + 3000, std::memory_order_relaxed);
    }
    if (command.rfind("@ondemand ", 0) == 0) {
        // FString {Data, Num incl. NUL, Max} after the text; the callee copies it.
        const auto text = command.size() - 10 + 1;
        const auto string = (qaScratch + std::uint32_t(command.size()) + 4) & ~3u;
        if (!qaOnDemandVerified || !read.object(result.controller) || string + 12 > qaScratch + PROP_VALUE_MAX + 8) {
            HORN_LOGE(kTag, "qa-exec @ondemand refused: %s", command.c_str());
            return;
        }
        read.host.MemoryWrite32(string, qaScratch + 10);
        read.host.MemoryWrite32(string + 4, std::uint32_t(text));
        read.host.MemoryWrite32(string + 8, std::uint32_t(text));
        const bool called = bl2::touch::Bl2TouchCallGuest(read.host, read.base + kClientLoadOnDemand,
            {result.controller, string});
        HORN_LOGI(kTag, "qa-exec tag=%s ondemand='%s' called=%u", request.substr(0, separator).c_str(),
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
            HORN_LOGE(kTag, "qa-exec @standalone refused: %s", command.c_str());
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
    HORN_LOGI(kTag, "qa-exec tag=%s command='%s' called=%u handled=%u netmode=%u", request.substr(0, separator).c_str(),
        command.c_str(), called ? 1u : 0u, handled, netModeAt ? unsigned(netMode) : 255u);
}
#endif

void inputBoundary(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    if (!active || insideProbe) return;
    insideProbe = true;
    struct Guard { ~Guard() { insideProbe = false; } } guard;
    SyncBl2GraphicsSettings(host);
    SyncBl2PerformanceSettings();
    Bl2PerfHintGameThread();
    const auto tick = inputTicks.fetch_add(1, std::memory_order_relaxed) + 1;
    Reader read(host);
    const auto result = observe(read, jit.Regs()[0]);
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
            context.availableActions = mask & (((std::uint64_t{1} << 15) - 1) |
                bl2::touch::actionBit(bl2::touch::Action::LastWeapon) |
                bl2::touch::actionBit(bl2::touch::Action::Chat));
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
    auto& mailbox = bl2::touch::sharedMailbox();
    [[maybe_unused]] const auto epoch = mailbox.publishContext(context);
    if (combinedSink) bl2::touch::consumeIntoEngine(mailbox, tick,
        bl2::touch::monotonicMilliseconds(), *combinedSink);
    else if (gameplaySink) bl2::touch::consumeIntoEngine(mailbox, tick,
        bl2::touch::monotonicMilliseconds(), *gameplaySink);
    else (void)mailbox.consumeFrame(tick, bl2::touch::monotonicMilliseconds());
#if HORN_ENABLE_LOGGING
    qaConsoleExec(read, result);
#endif
    [[maybe_unused]] const auto snapshot = mailbox.snapshot();
    const std::string contexts = joined(result.contexts), actions = joined(result.available);
    const auto description = std::to_string(result.identity) + '/' + result.reason + '/' + contexts + '/' + actions + '/' + std::to_string(context.capabilities);
    std::lock_guard<std::mutex> lock(observationMutex);
    if (description == lastObservation) return;
    lastObservation = description;
    if (logs.fetch_add(1, std::memory_order_relaxed) >= 64) return;
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

std::uint64_t Bl2TouchLifetimeGeneration() { return currentLifetimeGeneration(); }

bool VerifyBl2TouchInput(const std::uint8_t* bytes, std::size_t size) {
    inputVerified = false;
    if (!bytes || size != kFileBytes) return false;
    ObbSha256 sha;
    if (!sha.Update(bytes, size)) return false;
    const auto hex = ObbSha256::Hex(sha.Finalize());
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
#if HORN_ENABLE_LOGGING
    qaExecVerified = elf.GetSymbolAddress("_ZN12ULocalPlayer4ExecEPKcR13FOutputDevice") == base + kLocalPlayerExec &&
        elf.GetSymbolAddress("GLog") == base + kGLog;
    for (std::size_t i = 0; qaExecVerified && i < kLocalPlayerExecPrefix.size(); ++i)
        qaExecVerified = host.MemoryRead32(base + kLocalPlayerExec + std::uint32_t(i * 4)) == kLocalPlayerExecPrefix[i];
    if (!qaExecVerified) HORN_LOGE(kTag, "qa-exec unavailable: ULocalPlayer::Exec/GLog signature mismatch");
    qaMirrorVerified = qaExecVerified &&
        elf.GetSymbolAddress("_ZN23FOutputDeviceRedirector15AddOutputDeviceEP13FOutputDevice") == base + kAddOutputDevice &&
        elf.GetSymbolAddress("_ZTV17FOutputDeviceNull") == base + kNullDeviceVtable;
    for (std::size_t i = 0; qaMirrorVerified && i < kAddOutputDevicePrefix.size(); ++i)
        qaMirrorVerified = host.MemoryRead32(base + kAddOutputDevice + std::uint32_t(i * 4)) == kAddOutputDevicePrefix[i];
    if (qaMirrorVerified) qaMirrorThunk = host.GetThunkManager().RegisterThunk("bl2_qa_glog_serialize", qaMirrorSerialize);
    if (!qaMirrorThunk) HORN_LOGE(kTag, "qa-glog unavailable: FOutputDeviceRedirector/FOutputDeviceNull signature mismatch");
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
            [handler,continuation](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
                handler(jit,current);
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
            i == 0 ? ThunkHandler(inputBoundary) : (i == 3 ? ThunkHandler(inputDestroyed) : ThunkHandler(lifetimeBoundary)));
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
