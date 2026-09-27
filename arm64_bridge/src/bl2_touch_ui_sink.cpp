#include "bl2_touch_ui_sink.h"
#include "bl2_touch_physical.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace bl2::touch {
namespace {
constexpr std::uint32_t kMovieKey = 0x01CC5E7C;
struct Key { Action action; std::uint32_t rva; const char* name; };
constexpr Key keys[] = {
    {Action::Back,0x03009FB8,"Escape"}, {Action::Confirm,0x03009FA0,"Enter"},
    {Action::UiUp,0x03009FF0,"Up"}, {Action::UiDown,0x0300A000,"Down"},
    {Action::UiLeft,0x03009FE8,"Left"}, {Action::UiRight,0x03009FF8,"Right"}
};
struct Target {
    std::uint32_t viewport = 0, controller = 0, controllerId = 0, world = 0;
    std::uint32_t gfx = 0, movie = 0, owner = 0, interface = 0, handler = 0;
    std::uint32_t width = 0, height = 0, capabilities = 0;
    std::uint64_t controllerToken = 0, ownerToken = 0, generation = 0, scene = 0;
};
std::uint32_t floatWord(float value) {
    std::uint32_t result = 0; std::memcpy(&result, &value, sizeof(result)); return result;
}
} // namespace

struct Ue3UiSink::State {
    DynarmicHost& host;
    LifetimeGeneration generation;
    const std::uint32_t base;
    const bool tapTest;
    bool signatures = false, preparedValid = false, frameValid = false;
    unsigned logs = 0;
    std::string lastAdmission;
    std::uint32_t scratch = 0;
    std::uint64_t epoch = 0;
    Target prepared{}, frameTarget{};
    UiTransactions transactions;

    void admission(std::uint32_t viewport, std::uint32_t movie, std::uint32_t owner,
                   const std::string& reason, std::uint32_t caps) {
        if (!movie) return;
        const auto description = std::to_string(movie) + "/" + reason + "/" + std::to_string(caps);
        if (description == lastAdmission) return;
        lastAdmission = description;
        if (logs++ >= 24) return;
        (void)viewport; (void)owner;
        HORN_LOGI("BL2Bridge", "native-ui admission=%s caps=%u movie=%08X owner=%08X viewport=%08X "
            "gfxViewport=%08X size=%ux%u movieFlags=(%u,%u) ownerMovie=%08X mouseable=%u",
            reason.c_str(),caps,movie,owner,viewport,read(read(base + 0x0301F840) + 0x44),
            read(viewport + 0x3C),read(viewport + 0x40),read(movie + 0x4C),read(movie + 0x5C),
            read(owner + 0x3C),read(owner + 0x238));
    }

    State(DynarmicHost& h, LifetimeGeneration g, bool taps)
        : host(h), generation(g), base(h.GetElfLoader().GetBaseAddress()), tapTest(taps) {
        signatures = verify();
    }
    bool span(std::uint32_t p, std::size_t size) const {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() && size <= host.GetGuestMemorySize() - p;
    }
    std::uint32_t read(std::uint32_t p) const { return span(p, 4) ? host.MemoryRead32(p) : 0; }
    bool array(std::uint32_t p, std::uint32_t limit, std::uint32_t& data, std::uint32_t& count) const {
        if (!span(p, 12)) return false;
        data = read(p); count = read(p + 4);
        const auto capacity = read(p + 8);
        return count <= capacity && capacity <= limit && (!count || span(data, std::size_t(count) * 4));
    }
    std::uint64_t token(std::uint32_t object) const {
        std::uint32_t data = 0, count = 0;
        if (!span(object, 0x38) || !array(base + 0x02FBA268, 2000000, data, count)) return 0;
        const auto index = read(object + 0x20);
        if (index >= count || read(data + index * 4) != object) return 0;
        return (std::uint64_t(object) << 32) | index;
    }
    std::string name(std::uint32_t p) const {
        std::uint32_t names = 0, count = 0;
        if (!span(p, 8) || !array(base + 0x02FB00C8, 2000000, names, count) || read(p) >= count) return {};
        const auto entry = read(names + read(p) * 4);
        if (!span(entry, 136) || read(p + 4)) return {};
        std::string result;
        for (std::uint32_t i = 0; i < 128; ++i) {
            const auto c = host.MemoryRead8(entry + 8 + i);
            if (!c) return result;
            if (c < 32 || c > 126) return {};
            result += static_cast<char>(c);
        }
        return {};
    }
    bool code(std::uint32_t p) const {
        const auto address = p & ~1u;
        // Exact .text from the SHA-bound ELF; never call a vtable pointer in RAM.
        return address >= base + 0x00E823B0 && address < base + 0x02BB91EC &&
            host.MemoryReadCode(address).has_value();
    }
    bool verify() const {
        const auto& elf = host.GetElfLoader();
        if (!generation || elf.GetImageSize() != 0x03089000 ||
            std::uint64_t(base) + elf.GetImageSize() > host.GetGuestMemorySize()) return false;
        struct Signature { const char* symbol; std::uint32_t rva; std::array<std::uint32_t, 2> words; };
        constexpr Signature sites[] = {
            {"_ZN10FGFxEngine8InputKeyEiP9FGFxMovie5FName11EInputEvent", kMovieKey, {0xE24DD008,0xE92D4FF0}},
            {"_ZNK11FKdViewport8GetSizeXEv",0x01E2651C,{0xE590003C,0xE12FFF1E}},
            {"_ZNK11FKdViewport8GetSizeYEv",0x01E2652C,{0xE5900040,0xE12FFF1E}},
            {"_ZN16UGearboxGFxMovie16SetMouseableFlagEv",0x01C074F4,{0xE3A03001,0xE5803238}},
            {"_ZN15UGFxMoviePlayer11SetViewportEiiii",0x01CC52D4,{0xE59FC0C8,0xE92D47F0}}
        };
        for (const auto& site : sites) {
            if (elf.GetSymbolAddress(site.symbol) != base + site.rva) return false;
            for (std::size_t i = 0; i < site.words.size(); ++i)
                if (read(base + site.rva + static_cast<std::uint32_t>(i * 4)) != site.words[i]) return false;
        }
        // Exact original HandleEvent ABI call site: interface->vtable[0x90].
        return read(base + 0x01CC6348) == 0xE5923000 && read(base + 0x01CC634C) == 0xE5933090 &&
            read(base + 0x01CC6350) == 0xE12FFF33 &&
            read(base + 0x00F475C0) == 0xE590000C && read(base + 0x00F475C4) == 0xE7E00ED0 &&
            read(base + 0x01CC62B8) == 0 && read(base + 0x01CC62BC) == 2;
    }
    bool targetValid(const Target& t) const {
        if (!signatures || !generation || generation() != t.generation ||
            !t.ownerToken || !t.controllerToken || token(t.owner) != t.ownerToken ||
            token(t.controller) != t.controllerToken || read(base + 0x0300D940) != t.world ||
            // Match InputKey's IsPendingKill() + RF_Unreachable rejection before
            // the experimental direct MouseEvent path can bypass those checks.
            read(read(t.owner) + 0x40) != base + 0x00F475C0 || (read(t.owner + 0x0C) & 0x20000002u) ||
            !span(t.viewport, 0x44) || read(t.viewport + 0x3C) != t.width || read(t.viewport + 0x40) != t.height ||
            read(base + 0x0301F840) != t.gfx || !span(t.gfx, 0x1D4) ||
            !span(t.movie, 0x68) || read(t.movie + 0x64) != t.owner || read(t.owner + 0x3C) != t.movie ||
            !read(t.movie + 0x4C) || !read(t.movie + 0x5C) || read(t.movie + 0x34) != t.interface ||
            !span(t.interface, 4) || read(read(t.interface) + 0x90) != t.handler || !code(t.handler)) return false;
        std::uint32_t data = 0, count = 0;
        if (!array(t.gfx + 0x28, 4, data, count) || !count || read(data) != t.movie) return false;
        const auto engine = read(base + 0x03008D20);
        if (!token(engine) || !array(engine + 0x48C, 4, data, count) || count != 1) return false;
        const auto local = read(data);
        return token(local) && read(local + 0x40) == t.controller && read(local + 0x64) == t.controllerId;
    }
    bool current() const {
        if (!frameValid || physicalOwnership().ownsInput(monotonicMilliseconds()) || !targetValid(frameTarget)) return false;
        const auto snapshot = sharedMailbox().snapshot();
        return snapshot.epoch == epoch && snapshot.context.sceneIdentity == frameTarget.scene &&
            (snapshot.context.capabilities & kUiNavigation) && !(snapshot.context.capabilities & PhysicalOwnsInput);
    }
    void key(Action action) {
        if (!current()) return;
        for (const auto& key : keys) {
            if (key.action != action || name(base + key.rva) != key.name) continue;
            const auto target = frameTarget;
            const auto index = read(base + key.rva), number = read(base + key.rva + 4);
            // Exact-movie overload. This bypasses the Android joystick bitset and
            // its repeat/hold timer while retaining the game's real UI handlers.
            host.RunGuestCallback(base + kMovieKey, {target.gfx,target.controllerId,target.movie,index,number,0});
            const bool sentUp = current();
            if (sentUp) host.RunGuestCallback(base + kMovieKey,
                {target.gfx,target.controllerId,target.movie,index,number,1});
            if (logs++ < 24) HORN_LOGI("BL2Bridge", "native-ui key=%s movie=%08X epoch=%llu sentUp=%u targetRetained=%u",
                key.name,target.movie,static_cast<unsigned long long>(epoch),sentUp ? 1u : 0u,current() ? 1u : 0u);
            return;
        }
    }
    bool mouse(std::uint32_t type, float x, float y) {
        if (!current() || !scratch || !(frameTarget.capabilities & kUiTapTest)) return false;
        // Scaleform::GFx::MouseEvent, copied field-for-field from InputKey's
        // 0x1CC6304..6350 path. The native stage performs its viewport transform.
        for (std::uint32_t offset = 0; offset < 28; offset += 4) host.MemoryWrite32(scratch + offset, 0);
        host.MemoryWrite32(scratch, type);
        host.MemoryWrite32(scratch + 8, floatWord(x));
        host.MemoryWrite32(scratch + 12, floatWord(y));
        host.RunGuestCallback(frameTarget.handler, {frameTarget.interface,scratch});
        return true;
    }
    void tap(const UiTap& point) {
        if (!current() || !(frameTarget.capabilities & kUiTapTest)) return;
        const auto target = frameTarget;
        const float x = std::floor(point.x * static_cast<float>(target.width - 1));
        const float y = std::floor(point.y * static_cast<float>(target.height - 1));
        // No guest DOWN survives an Android gesture. Cancellation before this
        // commit has zero guest effects. Never call generic InputTouch as well.
        [[maybe_unused]] const bool complete = mouse(1,x,y) && mouse(2,x,y) && mouse(3,x,y);
        if (logs++ < 24) HORN_LOGI("BL2Bridge", "native-ui tap movie=%08X pixel=(%.0f,%.0f) size=%ux%u complete=%u",
            target.movie,x,y,target.width,target.height,complete ? 1u : 0u);
    }
};

Ue3UiSink::Ue3UiSink(DynarmicHost& host, LifetimeGeneration generation, bool tapTest)
    : state_(std::make_unique<State>(host,generation,tapTest)) {}
Ue3UiSink::~Ue3UiSink() = default;
bool Ue3UiSink::verified() const { return state_->signatures; }
std::uint32_t Ue3UiSink::prepare(std::uint32_t viewport, std::uint32_t controller,
    std::uint32_t movie, std::uint32_t owner, std::uint64_t scene) {
    auto& s = *state_; s.preparedValid = false;
    const auto reject = [&](const char* reason) { s.admission(viewport,movie,owner,reason,0); return 0u; };
    if (!s.signatures || !s.span(owner,0x23C) || !s.token(owner)) return reject("signature-or-owner");
    const auto cls = s.read(owner + 0x34);
    if (!s.token(cls)) return reject("owner-class");
    const auto name = s.name(cls + 0x2C);
    // Known full-screen menu families. Not HUD, map, inventory, vehicle or an
    // arbitrary movie merely because it happens to be focused.
    if (name != "FrontendGFxMovie" && name != "PauseGFxMovie" &&
        name != "OptionsGFxMovie" && name != "WillowGFxDialogBox") return reject("class-not-reviewed");
    Target t;
    t.viewport = viewport; t.controller = controller; t.movie = movie; t.owner = owner; t.scene = scene;
    t.controllerToken = s.token(controller); t.ownerToken = s.token(owner);
    t.world = s.read(s.base + 0x0300D940); t.gfx = s.read(s.base + 0x0301F840);
    t.generation = s.generation(); t.width = s.read(viewport + 0x3C); t.height = s.read(viewport + 0x40);
    if (!t.width || !t.height || t.width > 16384 || t.height > 16384) return reject("viewport-size");
    std::uint32_t players = 0, count = 0;
    const auto engine = s.read(s.base + 0x03008D20);
    if (!s.token(engine) || !s.array(engine + 0x48C,4,players,count) || count != 1) return reject("local-player-count");
    const auto local = s.read(players);
    if (!s.token(local)) return reject("local-player");
    t.controllerId = s.read(local + 0x64);
    if (t.controllerId > 3) return reject("controller-id");
    t.interface = s.read(movie + 0x34); t.handler = s.read(s.read(t.interface) + 0x90);
    if (!s.targetValid(t)) return reject("movie-focus-lifetime");
    for (const auto& key : keys) if (s.name(s.base + key.rva) != key.name) return reject("key-name");
    t.capabilities = kUiNavigation;
    // FGFxEngine stores the FViewport interface, complete FKdViewport+4. Admit
    // absolute taps only when this exact coordinate owner is the rendered one.
    if (s.tapTest && s.read(t.gfx + 0x44) == viewport + 4 && s.read(owner + 0x238) == 1 &&
        !(s.host.MemoryRead8(owner + 0xCA) & 2u)) {
        if (!s.scratch) s.scratch = s.host.GuestAlloc(32,8);
        if (s.scratch) t.capabilities |= kUiTapTest;
    }
    s.prepared = t; s.preparedValid = true;
    s.admission(viewport,movie,owner,name,t.capabilities);
    return t.capabilities;
}
void Ue3UiSink::beginTouchFrame(const Frame& frame) {
    auto& s = *state_;
    s.epoch = frame.epoch; s.frameTarget = s.prepared;
    s.frameValid = s.preparedValid && frame.context.sceneIdentity == s.prepared.scene &&
        (frame.context.capabilities & kUiNavigation) && !(frame.context.capabilities & PhysicalOwnsInput);
    s.transactions.begin(frame.epoch,frame.context.sceneIdentity,s.current(),frame.resetReasons != 0,
        s.frameTarget.width,s.frameTarget.height);
}
void Ue3UiSink::action(const ActionEdge& edge) {
    auto& s = *state_;
    if (!s.current()) { s.transactions.clear(); return; }
    if (const auto command = s.transactions.action(edge)) s.key(*command);
}
void Ue3UiSink::pointer(const PointerEvent& event) {
    auto& s = *state_;
    if (!s.current() || !(s.frameTarget.capabilities & kUiTapTest)) { s.transactions.clear(); return; }
    if (const auto tap = s.transactions.pointer(event)) s.tap(*tap);
}
void Ue3UiSink::endTouchFrame(const Frame&) {
    if (!state_->current()) state_->transactions.clear();
}
} // namespace bl2::touch
