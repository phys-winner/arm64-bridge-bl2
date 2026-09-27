#pragma once
#include "bl2_touch_input.h"

namespace bl2::touch {

// Implement this in the canonical bridge only after the guest seam is verified.
// These callbacks execute on the engine input thread, without Mailbox's mutex.
class EngineSink {
public:
    virtual ~EngineSink() = default;
    // If frame.resetReasons != 0, idempotently clear the entire touch-owned sink
    // ledger, including partial prior dispatch. Keep physical sources intact.
    virtual void beginTouchFrame(const Frame& frame) = 0;
    // On a reset, release ONLY touch-owned engine state. Retire old UI capture
    // safely; sourceEpoch may refer to a scene whose UObject was already freed.
    // Validate target lifetime/sourceEpoch for each actual delivery. A prior
    // callback may have opened a new scene during this same input frame.
    virtual void action(const ActionEdge& edge) = 0;
    // Event delivery must be immediate and ordered, not a final-state write.
    // CANCEL must abort capture without activating the newly selected item.
    virtual void pointer(const PointerEvent& event) = 0;
    virtual void movement(float right, float forward) = 0;
    // Positive yaw = look right; positive pitch = look up. Degrees per frame.
    // Convert sign/units at the proven engine seam; never multiply by dt here.
    virtual void relativeLook(float yawDegrees, float pitchDegrees) = 0;
    virtual void endTouchFrame(const Frame& frame) = 0;
};

// Call exactly once at the input boundary, before the original input processing.
// engineTick must increase across scene transitions; it is NOT a render frame.
// Physical/keyboard/mouse sources remain separate until the engine combines them.
bool consumeIntoEngine(Mailbox& mailbox, std::uint64_t engineTick,
                       std::uint64_t nowMs, EngineSink& sink);

} // namespace bl2::touch
