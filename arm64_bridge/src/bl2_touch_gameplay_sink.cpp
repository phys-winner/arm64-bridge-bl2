#include "bl2_touch_gameplay_sink.h"
#include "bl2_touch_physical.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_bindings.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace bl2::touch {
namespace {
constexpr std::uint32_t kAvailability = 0x0235679C;
constexpr std::uint32_t kFireBehaviors = 0x0236D8FC;
constexpr std::uint32_t kAxisPointer = 0x0236E210;
constexpr std::uint32_t kAddAxis = 0x01821D6C;
constexpr float kRotatorUnitsPerDegree = 65536.0f / 360.0f;

std::uint32_t floatWord(float value) {
    std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
float wordFloat(std::uint32_t bits) {
    float value = 0; std::memcpy(&value, &bits, sizeof(value)); return value;
}
std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}
struct Target {
    std::uint32_t controller = 0, input = 0, world = 0;
    std::uint64_t controllerToken = 0, inputToken = 0;
    std::uint64_t scene = 0, generation = 0, fingerprint = 0;
};
struct Definition {
    std::uint32_t object = 0;
    std::uint64_t token = 0;
    bool always = false;
};
struct OwnedAction {
    Target target{};
    Definition definition{};
    std::uint64_t epoch = 0;
    bool held = false;
};
} // namespace

struct Ue3GameplaySink::State {
    DynarmicHost& host;
    LifetimeGeneration generation;
    const std::uint32_t base;
    std::uint32_t arrayScratch = 0;
    Target prepared{}, frameTarget{};
    std::array<OwnedAction, kActionCount> owned{};
    std::uint64_t frameEpoch = 0, available = 0;
    bool preparedValid = false, frameValid = false;
    bool movementPending = false, lookPending = false;
    float right = 0, forward = 0, yaw = 0, pitch = 0;

    State(DynarmicHost& value, LifetimeGeneration callback)
        : host(value), generation(callback), base(value.GetElfLoader().GetBaseAddress()) {}
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
    bool isWillowInput(std::uint32_t input) const {
        auto cls = read(input + 0x34);
        const auto willowClass = read(base + 0x0306C578);
        if (!token(input) || !token(willowClass)) return false;
        for (int i = 0; i != 48 && cls; ++i) {
            if (!token(cls) || !span(cls, 0x44)) return false;
            if (cls == willowClass) return true;
            cls = read(cls + 0x40);
        }
        return false;
    }
    std::string name(std::uint32_t address) const {
        std::uint32_t data = 0, count = 0;
        if (!span(address, 8) || !array(base + 0x02FB00C8, 2000000, data, count)) return {};
        const auto index = read(address);
        if (index >= count) return {};
        const auto entry = read(data + index * 4);
        if (!span(entry, 136)) return {};
        std::string value;
        for (std::uint32_t i = 0; i != 128; ++i) {
            const auto c = host.MemoryRead8(entry + 8 + i);
            if (!c) {
                const auto number = read(address + 4);
                if (number) value += "_" + std::to_string(number - 1);
                return value;
            }
            if (c < 32 || c > 126) return {};
            value += static_cast<char>(c);
        }
        return {};
    }
    bool localOwner(const Target& target) const {
        if (token(target.controller) != target.controllerToken || token(target.input) != target.inputToken ||
            !target.inputToken || !target.controllerToken || !isWillowInput(target.input) ||
            read(target.input + 0x28) != target.controller ||
            read(target.controller + 0x76C) != target.input ||
            read(base + 0x0300D940) != target.world) return false;
        const auto engine = read(base + 0x03008D20);
        std::uint32_t players = 0, count = 0;
        if (!token(engine) || !array(engine + 0x48C, 4, players, count) || count != 1) return false;
        const auto local = read(players);
        return token(local) && read(local + 0x40) == target.controller;
    }
    bool fingerprint(std::uint32_t input, std::uint64_t& result) const {
        result = 1;
        std::uint32_t contexts = 0, count = 0;
        if (!array(input + 0x1A4, 32, contexts, count) || !count) return false;
        result = mix(result, contexts); result = mix(result, count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto ctx = read(contexts + i * 4);
            if (ctx && !token(ctx)) return false;
            result = mix(result, token(ctx));
        }
        const auto gfx = read(base + 0x0301F840);
        if (!gfx) return true;
        std::uint32_t focus = 0, focusedCount = 0;
        if (!span(gfx, 0x34) || !array(gfx + 0x28, 4, focus, focusedCount)) return false;
        result = mix(result, gfx); result = mix(result, focusedCount);
        if (focusedCount) {
            const auto movie = read(focus);
            if (movie) {
                if (!span(movie, 0x68) || !token(read(movie + 0x64))) return false;
                result = mix(result, movie); result = mix(result, token(read(movie + 0x64)));
            }
        }
        return true;
    }
    bool targetUnchanged(const Target& target) const {
        if (!generation || generation() != target.generation || !localOwner(target)) return false;
        std::uint64_t currentFingerprint = 0;
        return fingerprint(target.input, currentFingerprint) && currentFingerprint == target.fingerprint;
    }
    bool current() const {
        if (!frameValid || physicalOwnership().ownsInput(monotonicMilliseconds()) ||
            !targetUnchanged(frameTarget)) return false;
        const auto current = sharedMailbox().snapshot();
        return current.epoch == frameEpoch && current.context.sceneIdentity == frameTarget.scene &&
            (current.context.capabilities & GameplayInput) && !(current.context.capabilities & PhysicalOwnsInput);
    }
    bool collect(std::uint32_t definition, std::vector<std::uint32_t>& actions) const {
        std::uint32_t data = 0, count = 0;
        if (!token(definition) || !array(definition + 0x3C, 256, data, count)) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto action = read(data + i * 4);
            if (!action) continue;
            if (!token(action) || !span(action, 0x60)) return false;
            actions.push_back(action);
            if (actions.size() > 4096) return false;
        }
        return true;
    }
    bool actionDefinitions(std::uint32_t input, std::vector<std::uint32_t>& actions) const {
        std::uint32_t contexts = 0, count = 0;
        if (!array(input + 0x1A4, 32, contexts, count)) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto ctx = read(contexts + i * 4);
            if (!ctx) continue;
            if (!collect(ctx, actions)) return false;
            std::uint32_t sets = 0, setCount = 0;
            if (!array(ctx + 0x48, 64, sets, setCount)) return false;
            for (std::uint32_t j = 0; j < setCount; ++j) {
                const auto set = read(sets + j * 4);
                if (set && !collect(set, actions)) return false;
            }
        }
        std::sort(actions.begin(), actions.end());
        actions.erase(std::unique(actions.begin(), actions.end()), actions.end());
        return actions.size() <= 256;
    }
    bool availableNow(const Target& target, const Definition& definition) {
        if (!targetUnchanged(target) || token(definition.object) != definition.token) return false;
        // Native FireInputActionBehaviors uses this vtable entry for ordinary
        // actions. Refuse an unreviewed subclass override rather than bypass it.
        const auto vtable = read(target.input);
        if (read(vtable + 0x18C) != base + kAvailability) return false;
        return definition.always || host.RunGuestCallback(base + kAvailability, {target.input, definition.object}) != 0;
    }
    bool resolve(const Target& target, Action action, Definition& out) {
        const char* wanted = shippedActionName(action);
        if (!wanted || !targetUnchanged(target)) return false;
        std::vector<std::uint32_t> actions;
        if (!actionDefinitions(target.input, actions)) return false;
        unsigned matches = 0;
        for (const auto object : actions) {
            if (name(object + 0x58) != wanted) continue;
            out = {object, token(object), (host.MemoryRead8(object + 0x54) & 1u) != 0};
            ++matches;
        }
        return matches == 1 && availableNow(target, out);
    }
    bool invoke(const Target& target, const Definition& definition, bool down) {
        if (!arrayScratch || !localOwner(target) || token(definition.object) != definition.token) return false;
        host.MemoryWrite32(arrayScratch, arrayScratch + 12);
        host.MemoryWrite32(arrayScratch + 4, 1);
        host.MemoryWrite32(arrayScratch + 8, 1);
        host.MemoryWrite32(arrayScratch + 12, definition.object);
        // Flags proved by 0x236D924 and 0x236D950..99C:
        // (0,0) invokes only ordinary available actions; (0,1) invokes the
        // bAlwaysAvailable=1 partition. End with (1,bAlwaysAvailable) releases
        // only our recorded action even after its availability is revoked.
        host.RunGuestCallback(base + kFireBehaviors, {
            target.input, arrayScratch, down ? 0u : 1u, down ? 0u : 1u,
            definition.always ? 1u : 0u});
        return true;
    }
    void release(std::size_t index) {
        const auto record = owned[index];
        owned[index] = {}; // Clear before callbacks: nested scene reset is safe.
        if (record.held) (void)invoke(record.target, record.definition, false);
    }
    void releaseAll() {
        for (std::size_t i = 0; i != owned.size(); ++i) release(i);
        movementPending = lookPending = false;
        right = forward = yaw = pitch = 0;
    }
    bool axis(std::uint32_t parameter, std::uint32_t expectedOffset, float delta) {
        if (!current() || !std::isfinite(delta)) return false;
        if (delta == 0) return true;
        const auto pointer = host.RunGuestCallback(base + kAxisPointer, {frameTarget.input, parameter});
        if (pointer != frameTarget.input + expectedOffset || !span(pointer, 4) || !current()) return false;
        const float value = wordFloat(read(pointer));
        if (!std::isfinite(value) || std::abs(value) > 1.0e7f) return false;
        host.RunGuestCallback(base + kAddAxis, {frameTarget.input, pointer, floatWord(delta)});
        return true;
    }
};

Ue3GameplaySink::Ue3GameplaySink(DynarmicHost& host, LifetimeGeneration generation)
    : state_(std::make_unique<State>(host, generation)) {}
Ue3GameplaySink::~Ue3GameplaySink() = default;

std::uint64_t Ue3GameplaySink::prepare(std::uint32_t controller, std::uint32_t input, std::uint64_t scene) {
    auto& s = *state_;
    s.preparedValid = false; s.available = 0;
    if (!s.generation || !s.span(controller, 0x770) || !s.span(input, 0x294)) return 0;
    Target target;
    target.controller = controller; target.input = input; target.scene = scene;
    target.controllerToken = s.token(controller); target.inputToken = s.token(input);
    target.world = s.read(s.base + 0x0300D940); target.generation = s.generation();
    if (!target.world || !s.localOwner(target) || !s.fingerprint(input, target.fingerprint)) return 0;
    if (!s.arrayScratch) s.arrayScratch = s.host.GuestAlloc(16, 8);
    if (!s.arrayScratch) return 0;
    s.prepared = target;
    std::vector<std::uint32_t> actions;
    if (!s.actionDefinitions(input, actions)) return 0;
    std::array<Definition, kActionCount> definitions{};
    std::array<unsigned, kActionCount> matches{};
    // One bounded name walk per frame, not one context traversal per button.
    for (const auto object : actions) {
        const auto name = s.name(object + 0x58);
        for (std::size_t i = 0; i != kActionCount; ++i) {
            const auto* wanted = shippedActionName(static_cast<Action>(i));
            if (!wanted || name != wanted) continue;
            definitions[i] = {object, s.token(object), (s.host.MemoryRead8(object + 0x54) & 1u) != 0};
            ++matches[i];
        }
    }
    for (std::size_t i = 0; i != kActionCount; ++i) {
        if (matches[i] == 1 && s.availableNow(target, definitions[i])) s.available |= std::uint64_t{1} << i;
    }
    s.preparedValid = s.targetUnchanged(target);
    return s.preparedValid ? s.available : 0;
}
bool Ue3GameplaySink::ready() const { return state_->preparedValid; }
void Ue3GameplaySink::expirePendingAxes() {
    state_->movementPending = state_->lookPending = false;
    state_->right = state_->forward = state_->yaw = state_->pitch = 0;
}
void Ue3GameplaySink::beginTouchFrame(const Frame& frame) {
    auto& s = *state_;
    s.frameValid = false;
    if (frame.resetReasons) s.releaseAll();
    s.frameEpoch = frame.epoch; s.frameTarget = s.prepared;
    s.frameValid = s.preparedValid && frame.context.sceneIdentity == s.prepared.scene &&
        (frame.context.capabilities & GameplayInput) && !(frame.context.capabilities & PhysicalOwnsInput);
    if (!s.current()) s.releaseAll();
}
void Ue3GameplaySink::action(const ActionEdge& edge) {
    auto& s = *state_;
    const auto index = static_cast<std::size_t>(edge.action);
    if (index >= kActionCount) return;
    if (!edge.down) {
        if (s.owned[index].held && s.owned[index].epoch == edge.sourceEpoch) s.release(index);
        return;
    }
    if (s.owned[index].held || edge.sourceEpoch != s.frameEpoch || !s.current() ||
        !(s.available & actionBit(edge.action))) return;
    Definition definition;
    if (!s.resolve(s.frameTarget, edge.action, definition) || !s.current()) return;
    // Record before dispatch: OnBegin can synchronously open a GFx scene.
    s.owned[index] = {s.frameTarget, definition, edge.sourceEpoch, true};
    if (!s.invoke(s.frameTarget, definition, true)) s.owned[index] = {};
}
void Ue3GameplaySink::pointer(const PointerEvent&) {
    // DirectUi is never advertised by this gameplay stage. Calling Native_Touch
    // and Native_Mouse together would double-submit Scaleform mouse clicks.
}
void Ue3GameplaySink::movement(float right, float forward) {
    auto& s = *state_;
    s.movementPending = false;
    if (!s.current() || !std::isfinite(right) || !std::isfinite(forward)) return;
    const auto length = std::hypot(right, forward);
    if (length > 1) { right /= length; forward /= length; }
    s.right = right; s.forward = forward; s.movementPending = true;
}
void Ue3GameplaySink::relativeLook(float yaw, float pitch) {
    auto& s = *state_;
    s.lookPending = false;
    if (!s.current() || !std::isfinite(yaw) || !std::isfinite(pitch)) return;
    const auto snapshot = sharedMailbox().snapshot();
    if (!(snapshot.context.capabilities & RelativeLook)) return;
    s.yaw = std::clamp(yaw, -kMaxPendingLookDegrees, kMaxPendingLookDegrees);
    s.pitch = std::clamp(pitch, -kMaxPendingLookDegrees, kMaxPendingLookDegrees);
    s.lookPending = true;
}
void Ue3GameplaySink::beforeAxisProcessing(std::uint32_t input) {
    auto& s = *state_;
    if (!s.movementPending || input != s.frameTarget.input) return;
    s.movementPending = false;
    // Single native add after joystick/key queue processing, before PI's normal
    // movement scaling. No second dead zone or virtual joystick callback.
    if (!s.axis(1, 0x100, s.forward) || !s.axis(7, 0x118, s.right)) s.lookPending = false;
}
void Ue3GameplaySink::beforeLookClamp(std::uint32_t input) {
    auto& s = *state_;
    if (!s.lookPending || input != s.frameTarget.input) return;
    s.lookPending = false;
    // Bytecode proof: UpdateRotation assigns int(aTurn/aLookUp) to DeltaRot.
    // This seam is after dt/FOV/mouse sensitivity and before native ignore-look
    // checks. UI, pawn, vehicle and pitch constraints remain in ProcessViewRotation.
    (void)s.axis(6, 0x114, s.yaw * kRotatorUnitsPerDegree);
    (void)s.axis(9, 0x120, s.pitch * kRotatorUnitsPerDegree);
}
void Ue3GameplaySink::retireInput(std::uint32_t input) {
    auto& s = *state_;
    for (auto& action : s.owned) if (action.target.input == input) action = {};
    if (s.frameTarget.input == input) { s.frameValid = false; expirePendingAxes(); }
    if (s.prepared.input == input) s.preparedValid = false;
}
void Ue3GameplaySink::endTouchFrame(const Frame&) {
    // OnBegin might have changed input context/movie halfway through this frame.
    if (!state_->current()) state_->releaseAll();
}

} // namespace bl2::touch
