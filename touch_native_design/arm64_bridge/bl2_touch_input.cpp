#include "bl2_touch_input.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bl2::touch {
namespace {
constexpr std::uint32_t kKnownCapabilities = GameplayInput | RelativeLook | DirectUi |
                                             PhysicalOwnsInput | CanSkip | UiNavigation | UiTapTest;
bool sameSceneContext(const Context& a, const Context& b) {
    return a.mode == b.mode && a.capabilities == b.capabilities &&
           a.sceneIdentity == b.sceneIdentity;
}
float limitedLook(float pending, float next) {
    return std::clamp(pending + std::clamp(next, -kMaxLookDegreesPerPacket,
                                         kMaxLookDegreesPerPacket),
                      -kMaxPendingLookDegrees, kMaxPendingLookDegrees);
}
} // namespace

std::array<std::uint64_t, 8> Snapshot::wire() const {
    return {kProtocolVersion, epoch, static_cast<std::uint64_t>(context.mode),
            context.capabilities, context.availableActions, frameSerial, 0, 0};
}

std::uint64_t Mailbox::publishContext(Context context, bool forceNewScene) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<unsigned>(context.mode) > static_cast<unsigned>(Mode::Dead)) {
        context.mode = Mode::Unknown;
    }
    context.capabilities &= kKnownCapabilities;
    context.availableActions &= kKnownActions;
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
        (context_.capabilities & (DirectUi | UiTapTest)) == 0) return false;
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
    const PointerEvent event{pointerId, phase, surfaceX, surfaceY, epoch_, ++sequence_};
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
        while (frame.actionEdgeCount < frame.actionEdges.size()) {
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
