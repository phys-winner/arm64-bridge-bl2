#include "bl2_touch_runtime.h"
#include "bl2_touch_gameplay_sink.h"
#include "bl2_touch_gameplay_hooks.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_ui_sink.h"
#include "bl2_touch_combined_sink.h"
#include "bl2_touch_ui_model_probe.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include <memory>

#include "bridge_log.h"
#include "dynarmic_host.h"
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
    std::uint32_t viewport = 0, world = 0, controller = 0, input = 0, movie = 0, movieOwner = 0;
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
    // This leaf function is the game's actual availability check. RunGuestCallback
    // uses the secondary core/stack while the primary input core is suspended.
    const auto isAvailable = read.base + 0x0235679C;
    for (const auto action : actions) {
        const auto name = read.name(action + 0x58);
        if (!name.empty() && read.host.RunGuestCallback(isAvailable, {out.input, action})) out.available.push_back(name);
    }
    std::sort(out.available.begin(), out.available.end());
    const auto has = [&](const char* name) {
        return std::find(out.contexts.begin(), out.contexts.end(), name) != out.contexts.end();
    };
    out.reason = "observed-only";
    if (out.movie && out.movieClass != "WillowHUDGFxMovie") out.mode = bl2::touch::Mode::Menu;
    else if (has("InputContext_Cinematic")) out.mode = bl2::touch::Mode::Cinematic;
    else if (has("InputContext_DrivingVehicle") || has("InputContext_InVehicleTurret") ||
             has("InputContext_VehicleAttachedRider")) out.mode = bl2::touch::Mode::Vehicle;
    else if (has("InputContext_OnFoot") || has("InputContext_Injured") || has("InputContext_InjuredMoving"))
        out.mode = bl2::touch::Mode::Gameplay;
    else out.reason = "unclassified-live-context";
    return out;
}

std::string joined(const std::vector<std::string>& names) {
    std::string result;
    for (const auto& name : names) { if (!result.empty()) result += ','; result += name; }
    return result;
}

void inputBoundary(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    if (!active || insideProbe) return;
    insideProbe = true;
    struct Guard { ~Guard() { insideProbe = false; } } guard;
    const auto tick = inputTicks.fetch_add(1, std::memory_order_relaxed) + 1;
    Reader read(host);
    const auto result = observe(read, jit.Regs()[0]);
    if (uiModelObserverEnabled) ProbeBl2UiModel(host, result.movieOwner, result.identity);
    bl2::touch::Context context;
    context.mode = result.mode;
    context.sceneIdentity = result.identity;
    if (gameplaySink) {
        gameplaySink->expirePendingAxes();
        // Exact state witnessed on build218/PID14654, 2026-09-27 00:10:09.610.
        // Vehicles, injured, GFx screens and ambiguous multi-context states stay
        // on the temporary fallback until separately verified.
        const bool onFoot = result.mode == bl2::touch::Mode::Gameplay && !result.movie &&
            result.contexts.size() == 1 && result.contexts[0] == "InputContext_OnFoot";
        const auto mask = gameplaySink->prepare(onFoot ? result.controller : 0,
            onFoot ? result.input : 0, result.identity);
        if (onFoot && gameplaySink->ready()) {
            context.capabilities = bl2::touch::GameplayInput | bl2::touch::RelativeLook;
            context.availableActions = mask & ((std::uint64_t{1} << 15) - 1);
        }
        if (uiSink) {
            const auto caps = uiSink->prepare(result.viewport, result.controller, result.movie,
                result.movieOwner, result.identity);
            if (caps) {
                context.capabilities = caps;
                context.availableActions = bl2::touch::kUiActions;
                context.mode = result.movieClass == "WillowGFxDialogBox" ? bl2::touch::Mode::Dialog :
                    (result.movieClass == "FrontendGFxMovie" ? bl2::touch::Mode::Frontend : bl2::touch::Mode::Menu);
            }
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
        static_cast<unsigned>(result.mode), result.reason.c_str(), contexts.c_str(), actions.c_str(),
        context.capabilities, uiSink ? "native-ui-test" : (gameplaySink ? "gameplay-test" : "disabled"));
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
    if (!*mode || std::strcmp(mode, "0") == 0) return true;
    const bool tapTest = std::strcmp(mode, "ui-tap-test") == 0;
    const bool uiDelivery = tapTest || std::strcmp(mode, "ui-test") == 0;
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

    const auto code = host.GuestAlloc(sizeof(hooks) / sizeof(hooks[0]) * 20, 4);
    if (!code) return false;
    std::array<std::uint32_t, sizeof(hooks) / sizeof(hooks[0])> sites{};
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
    active = installed = true;
    if (delivery) {
        gameplaySink = std::make_unique<bl2::touch::Ue3GameplaySink>(host, currentLifetimeGeneration);
        if (!InstallBl2TouchGameplayHooks(host, gameplaySink.get())) {
            gameplaySink.reset();
            HORN_LOGE(kTag, "native-touch gameplay hooks unavailable; observer/fallback retained");
            return false;
        }
    }
    if (uiDelivery && gameplaySink) {
        uiSink = std::make_unique<bl2::touch::Ue3UiSink>(host, currentLifetimeGeneration, tapTest);
        if (!uiSink->verified()) {
            uiSink.reset();
            HORN_LOGE(kTag, "native-touch UI signatures unavailable; gameplay/fallback retained");
        } else combinedSink = std::make_unique<bl2::touch::CombinedSink>(*gameplaySink, *uiSink);
    }
    HORN_LOGI(kTag, "native-touch stage=%u mode=%s sha256=%s hooks=%zu boundedChanges=64",
              uiSink ? 3u : (gameplaySink ? 2u : 1u), mode, kSha, sites.size() + (gameplaySink ? 2 : 0));
    return true;
}
