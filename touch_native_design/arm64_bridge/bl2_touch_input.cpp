#include "bl2_touch_input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace bl2::touch {
namespace {
constexpr std::uint32_t kKnownCapabilities = GameplayInput | RelativeLook | DirectUi |
    PhysicalOwnsInput | CanSkip | UiNavigation | UiTapTest | UiWidgets | PauseBackdrop | DebugCanvas | MapGestures;
// Appending gameplay LastWeapon/Chat actions must not change GFx transaction
// cadence: an unfinished DOWN still carries into the next original UI frame.
constexpr std::size_t kUiEdgesPerFrame = 25;
bool sameSceneContext(const Context& a, const Context& b) {
    return a.mode == b.mode && a.capabilities == b.capabilities &&
           a.sceneIdentity == b.sceneIdentity;
}
float limitedLook(float pending, float next) {
    return std::clamp(pending + std::clamp(next, -kMaxLookDegreesPerPacket,
                                         kMaxLookDegreesPerPacket),
                      -kMaxPendingLookDegrees, kMaxPendingLookDegrees);
}
bool validSurfaceRect(const SurfaceRect& r) {
    return std::isfinite(r.left) && std::isfinite(r.top) &&
        std::isfinite(r.right) && std::isfinite(r.bottom) &&
        r.left >= 0 && r.top >= 0 && r.right <= 1 && r.bottom <= 1 &&
        r.left < r.right && r.top < r.bottom;
}
std::uint64_t floatPair(float low, float high) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t a = 0, b = 0;
    std::memcpy(&a, &low, sizeof(a));
    std::memcpy(&b, &high, sizeof(b));
    return a | (std::uint64_t{b} << 32);
}
// The equipped slot and every type code must belong to an occupied slot.
std::uint32_t validWeaponSlots(std::uint32_t word) {
    if ((word & kWeaponSlotsKnown) == 0 || (word & 0x78F0u) != 0) return 0;
    const auto occupied = word & 15u, equipped = (word >> 8) & 7u;
    if (equipped > 4 || (equipped && (occupied & (1u << (equipped - 1))) == 0)) return 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        const auto type = (word >> (16 + 4 * slot)) & 15u;
        if (type > 6 || (type && (occupied & (1u << slot)) == 0)) return 0;
    }
    return word;
}
} // namespace

std::array<std::uint64_t, kSnapshotWords> Snapshot::wire() const {
    return {kProtocolVersion, epoch, static_cast<std::uint64_t>(context.mode),
            context.capabilities, context.availableActions, frameSerial,
            context.knownActions, static_cast<std::uint64_t>(context.weaponCount),
            static_cast<std::uint64_t>(context.grenadeCount), context.hudValidMask,
            floatPair(context.mapRect.left, context.mapRect.top),
            floatPair(context.mapRect.right, context.mapRect.bottom),
            floatPair(context.ammoRect.left, context.ammoRect.top),
            floatPair(context.ammoRect.right, context.ammoRect.bottom),
            floatPair(context.objectiveRect.left, context.objectiveRect.top),
            floatPair(context.objectiveRect.right, context.objectiveRect.bottom),
            floatPair(context.useRect.left, context.useRect.top),
            floatPair(context.useRect.right, context.useRect.bottom),
            floatPair(context.skillRect.left, context.skillRect.top),
            floatPair(context.skillRect.right, context.skillRect.bottom),
            floatPair(context.experienceRect.left, context.experienceRect.top),
            floatPair(context.experienceRect.right, context.experienceRect.bottom),
            floatPair(context.challengeRect.left, context.challengeRect.top),
            floatPair(context.challengeRect.right, context.challengeRect.bottom),
            context.weaponSlots};
}

std::uint64_t Mailbox::publishContext(Context context, bool forceNewScene) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<unsigned>(context.mode) > static_cast<unsigned>(Mode::Dead)) {
        context.mode = Mode::Unknown;
    }
    context.capabilities &= kKnownCapabilities;
    context.availableActions &= kKnownActions;
    context.knownActions &= kKnownActions;
    if (context.weaponCount < -1 || context.weaponCount > 64) context.weaponCount = -1;
    if (context.grenadeCount < -1 || context.grenadeCount > 65535) context.grenadeCount = -1;
    context.hudValidMask &= HudMinimap | HudAmmo | HudObjectives | HudObjectivesEmpty | HudUse |
        HudSkill | HudExperience | HudChallenge;
    if ((context.hudValidMask & HudMinimap) == 0 || !validSurfaceRect(context.mapRect)) {
        context.hudValidMask &= ~HudMinimap;
        context.mapRect = {};
    }
    if ((context.hudValidMask & HudAmmo) == 0 || !validSurfaceRect(context.ammoRect)) {
        context.hudValidMask &= ~HudAmmo;
        context.ammoRect = {};
    }
    const auto objectives = context.hudValidMask & (HudObjectives | HudObjectivesEmpty);
    if (objectives == (HudObjectives | HudObjectivesEmpty) ||
        (objectives == HudObjectives && !validSurfaceRect(context.objectiveRect))) {
        context.hudValidMask &= ~(HudObjectives | HudObjectivesEmpty);
    }
    if ((context.hudValidMask & HudObjectives) == 0) context.objectiveRect = {};
    if ((context.hudValidMask & HudUse) == 0 || !validSurfaceRect(context.useRect)) {
        context.hudValidMask &= ~HudUse;
        context.useRect = {};
    }
    for (const auto& [region, rect] : {std::pair{HudSkill, &context.skillRect},
             std::pair{HudExperience, &context.experienceRect}, std::pair{HudChallenge, &context.challengeRect}}) {
        if ((context.hudValidMask & region) == 0 || !validSurfaceRect(*rect)) {
            context.hudValidMask &= ~region;
            *rect = {};
        }
    }
    context.weaponSlots = validWeaponSlots(context.weaponSlots);
    if (!sameSceneContext(context, context_) || forceNewScene) {
        context_ = context;
        invalidateLocked(ContextChanged);
    } else if (context.availableActions != context_.availableActions) {
        const auto revoked = context_.availableActions & ~context.availableActions;
        context_ = context;
        desiredActions_ &= ~revoked;
        for (std::size_t i = 0; i < kActionCount; ++i) {
            const auto bit = std::uint64_t{1} << i;
            if ((revoked & bit) == 0) continue;
            transitions_[i].clear();
            if ((guestHeldActions_ & bit) != 0 && pendingResetReasons_ == NoReset) {
                transitions_[i].push(ActionEdge{static_cast<Action>(i), false,
                                                guestActionEpochs_[i], ++sequence_});
            }
        }
    } else {
        // Counts, known-state and projected HUD bounds must remain live even
        // when the action mask and the input lifetime are unchanged.
        context_ = context;
    }
    return epoch_;
}

Snapshot Mailbox::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Snapshot{epoch_, context_, frameSerial_};
}

bool Mailbox::inputAllowedLocked() const {
    return context_.mode != Mode::Unknown && context_.mode != Mode::Loading &&
           context_.mode != Mode::TextInput &&
           (context_.capabilities & PhysicalOwnsInput) == 0;
}

bool Mailbox::gameplayAllowedLocked() const {
    return inputAllowedLocked() && (context_.capabilities & GameplayInput) != 0 &&
           (context_.mode == Mode::Gameplay || context_.mode == Mode::Vehicle);
}

void Mailbox::invalidateLocked(std::uint32_t reason) {
    ++epoch_;
    if (epoch_ == 0) ++epoch_;
    pendingResetReasons_ |= reason;
    armed_ = false;
    heartbeatMs_ = 0;
    moveX_ = moveY_ = yawDegrees_ = pitchDegrees_ = 0;
    desiredActions_ = 0;
    for (auto& queue : transitions_) queue.clear();
    pointers_.clear();
    submittedPointers_.fill(PointerState{});
    // guestHeldActions_ and guestPointers_ survive until the reset frame. They
    // identify exactly which touch-owned state the engine must release/cancel.
}

void Mailbox::reset(std::uint32_t reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    invalidateLocked(reason == NoReset ? ManualReset : reason);
}

bool Mailbox::submit(std::uint64_t epoch, float moveX, float moveY,
                     float yawDegrees, float pitchDegrees,
                     std::uint64_t heldActions, std::uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch != epoch_ || !inputAllowedLocked()) return false;
    if (!std::isfinite(moveX) || !std::isfinite(moveY) ||
        !std::isfinite(yawDegrees) || !std::isfinite(pitchDegrees)) {
        invalidateLocked(InvalidNumeric);
        return false;
    }
    if (!armed_) {
        // Only an explicit all-neutral packet acknowledges a new epoch. Do not
        // silently turn a finger that survived scene/lifecycle change into DOWN.
        if (moveX != 0 || moveY != 0 || yawDegrees != 0 || pitchDegrees != 0 ||
            heldActions != 0) return false;
        armed_ = true;
        heartbeatMs_ = nowMs;
        return true;
    }

    heldActions &= context_.availableActions & kKnownActions;
    if ((context_.capabilities & CanSkip) == 0) heldActions &= ~actionBit(Action::Skip);
    const auto changed = heldActions ^ desiredActions_;
    for (std::size_t i = 0; i < kActionCount; ++i) {
        if ((changed & (std::uint64_t{1} << i)) != 0 && transitions_[i].full()) {
            invalidateLocked(QueueOverflow);
            return false;
        }
    }
    for (std::size_t i = 0; i < kActionCount; ++i) {
        const auto bit = std::uint64_t{1} << i;
        if ((changed & bit) != 0) {
            transitions_[i].push(ActionEdge{static_cast<Action>(i),
                (heldActions & bit) != 0, epoch_, ++sequence_});
        }
    }
    desiredActions_ = heldActions;
    heartbeatMs_ = nowMs;
    if (gameplayAllowedLocked()) {
        moveX_ = std::clamp(moveX, -1.0f, 1.0f);
        moveY_ = std::clamp(moveY, -1.0f, 1.0f);
        const float length = std::sqrt(moveX_ * moveX_ + moveY_ * moveY_);
        if (length > 1) { moveX_ /= length; moveY_ /= length; }
        if ((context_.capabilities & RelativeLook) != 0) {
            yawDegrees_ = limitedLook(yawDegrees_, yawDegrees);
            pitchDegrees_ = limitedLook(pitchDegrees_, pitchDegrees);
        } else {
            yawDegrees_ = pitchDegrees_ = 0;
        }
    } else {
        moveX_ = moveY_ = yawDegrees_ = pitchDegrees_ = 0;
    }
    return true;
}

int Mailbox::pointerSlot(const std::array<PointerState, kMaxPointers>& states,
                         std::int32_t id) {
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (states[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

int Mailbox::freePointerSlot(const std::array<PointerState, kMaxPointers>& states) {
    return pointerSlot(states, -1);
}

bool Mailbox::uiPointer(std::uint64_t epoch, std::int32_t pointerId,
                        PointerPhase phase, float surfaceX, float surfaceY,
                        std::uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch != epoch_ || !armed_ || !inputAllowedLocked() ||
        (context_.capabilities & (DirectUi | UiTapTest | UiWidgets)) == 0) return false;
    if (!std::isfinite(surfaceX) || !std::isfinite(surfaceY)) {
        invalidateLocked(InvalidNumeric);
        return false;
    }
    if (pointerId < 0 || static_cast<unsigned>(phase) > static_cast<unsigned>(PointerPhase::Cancel)) {
        invalidateLocked(InvalidPointer);
        return false;
    }
    int slot = pointerSlot(submittedPointers_, pointerId);
    const bool outside = surfaceX < 0 || surfaceX > 1 || surfaceY < 0 || surfaceY > 1;
    if (phase == PointerPhase::Down) {
        if (outside) return false; // Never clamp a new gesture onto a UI edge.
        if (slot >= 0) { invalidateLocked(InvalidPointer); return false; }
        slot = freePointerSlot(submittedPointers_);
        if (slot < 0) { invalidateLocked(QueueOverflow); return false; }
    } else if (slot < 0) {
        return false; // An orphan MOVE/UP never creates a synthetic DOWN.
    } else if (outside) {
        // Leaving the Surface retires this pointer's capture only. Keeping the
        // last valid coordinates prevents an out-of-bounds UP becoming a click
        // on the nearest edge widget. Other touch sources keep their owners.
        const auto& previous = submittedPointers_[static_cast<std::size_t>(slot)];
        phase = PointerPhase::Cancel;
        surfaceX = previous.x;
        surfaceY = previous.y;
    }
    const PointerEvent event{pointerId, phase, surfaceX, surfaceY, epoch_, ++sequence_, nowMs};
    // Only consecutive MOVE events of the same pointer can be coalesced. DOWN,
    // UP and CANCEL are never overwritten or moved ahead of another event.
    if (phase == PointerPhase::Move && !pointers_.empty() &&
        pointers_.back().phase == PointerPhase::Move &&
        pointers_.back().pointerId == pointerId) {
        pointers_.back() = event;
    } else {
        if (pointers_.full()) { invalidateLocked(QueueOverflow); return false; }
        pointers_.push(event);
    }
    auto& state = submittedPointers_[static_cast<std::size_t>(slot)];
    state = (phase == PointerPhase::Up || phase == PointerPhase::Cancel)
                ? PointerState{} : PointerState{pointerId, surfaceX, surfaceY, epoch_};
    heartbeatMs_ = nowMs;
    return true;
}

void Mailbox::appendReleasesLocked(Frame& frame) {
    for (std::size_t i = 0; i < kActionCount; ++i) {
        if ((guestHeldActions_ & (std::uint64_t{1} << i)) != 0) {
            frame.actionEdges[frame.actionEdgeCount++] = ActionEdge{
                static_cast<Action>(i), false, guestActionEpochs_[i], ++sequence_};
        }
    }
    guestHeldActions_ = 0;
    guestActionEpochs_.fill(0);
    for (const auto& pointer : guestPointers_) {
        if (pointer.id >= 0) {
            frame.pointerEvents[frame.pointerEventCount++] = PointerEvent{
                pointer.id, PointerPhase::Cancel, pointer.x, pointer.y,
                pointer.epoch, ++sequence_};
        }
    }
    guestPointers_.fill(PointerState{});
}

void Mailbox::rememberPointerLocked(const PointerEvent& event) {
    int slot = pointerSlot(guestPointers_, event.pointerId);
    if (event.phase == PointerPhase::Down && slot < 0) slot = freePointerSlot(guestPointers_);
    if (slot < 0) return;
    guestPointers_[static_cast<std::size_t>(slot)] =
        (event.phase == PointerPhase::Up || event.phase == PointerPhase::Cancel)
            ? PointerState{} : PointerState{event.pointerId, event.surfaceX,
                                            event.surfaceY, event.sourceEpoch};
}

Frame Mailbox::consumeFrame(std::uint64_t engineTick, std::uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    Frame frame;
    frame.epoch = epoch_;
    frame.context = context_;
    frame.frameSerial = frameSerial_;
    if (hasEngineTick_ && engineTick <= lastEngineTick_) return frame;
    hasEngineTick_ = true;
    lastEngineTick_ = engineTick;
    frame.fresh = true;
    frame.frameSerial = ++frameSerial_;
    if (armed_ && (nowMs < heartbeatMs_ || nowMs - heartbeatMs_ > kHeartbeatTimeoutMs)) {
        invalidateLocked(HeartbeatLost);
    }
    frame.epoch = epoch_;
    if (pendingResetReasons_ != NoReset) {
        frame.resetReasons = pendingResetReasons_;
        pendingResetReasons_ = NoReset;
        appendReleasesLocked(frame);
        // Reset is a neutral barrier. Inputs already accepted for the NEW epoch
        // stay queued until the next tick, so release and new DOWN cannot merge.
        return frame;
    }
    if (!armed_ || !inputAllowedLocked()) return frame;

    frame.moveX = moveX_;
    frame.moveY = moveY_;
    frame.yawDegrees = yawDegrees_;
    frame.pitchDegrees = pitchDegrees_;
    yawDegrees_ = pitchDegrees_ = 0; // Deltas, never rates and never dt-scaled.
    const auto appendEdge = [&](std::size_t i) {
        const auto edge = transitions_[i].pop();
        frame.actionEdges[frame.actionEdgeCount++] = edge;
        const auto bit = std::uint64_t{1} << i;
        if (edge.down) {
            guestHeldActions_ |= bit;
            guestActionEpochs_[i] = edge.sourceEpoch;
        } else {
            guestHeldActions_ &= ~bit;
            guestActionEpochs_[i] = 0;
        }
    };
    if ((context_.capabilities & UiNavigation) != 0 && !gameplayAllowedLocked()) {
        // UI commits completed gestures rather than holding a gameplay action
        // across ticks. Preserve the complete producer order: taking one edge
        // per action would turn A-down/A-up/B-down/B-up into overlapping DOWNs.
        // Stop at Frame capacity; all unselected edges remain in their rings.
        while (frame.actionEdgeCount < std::min(frame.actionEdges.size(), kUiEdgesPerFrame)) {
            std::size_t next = kActionCount;
            for (std::size_t i = 0; i < kActionCount; ++i) {
                const auto& queue = transitions_[i];
                if (queue.empty()) continue;
                if (next == kActionCount || queue.entries[queue.head].sequence <
                    transitions_[next].entries[transitions_[next].head].sequence) next = i;
            }
            if (next == kActionCount) break;
            appendEdge(next);
        }
    } else {
        // Preserve gameplay pulse/hold duration: at most one edge per action
        // per engine tick, even if a producer queued a full tap between ticks.
        for (std::size_t i = 0; i < kActionCount; ++i) {
            if (!transitions_[i].empty()) appendEdge(i);
        }
    }
    std::sort(frame.actionEdges.begin(), frame.actionEdges.begin() + frame.actionEdgeCount,
              [](const ActionEdge& a, const ActionEdge& b) { return a.sequence < b.sequence; });
    frame.heldActions = guestHeldActions_;
    while (!pointers_.empty()) {
        const auto event = pointers_.pop();
        frame.pointerEvents[frame.pointerEventCount++] = event;
        rememberPointerLocked(event);
    }
    return frame;
}

Mailbox& sharedMailbox() {
    static Mailbox mailbox;
    return mailbox;
}

std::uint64_t monotonicMilliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace bl2::touch
