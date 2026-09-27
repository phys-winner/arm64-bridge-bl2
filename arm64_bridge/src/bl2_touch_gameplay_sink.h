#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include <array>
#include <cstdint>
#include <memory>

class DynarmicHost;

namespace bl2::touch {

// Concrete semantic gameplay delivery for the immutable A0B06E... guest.
// Construction does not install hooks and does not enable capabilities. The
// runtime provider must independently validate the image and live context.
// All methods below run under the host engine-input mutex, never on Android UI.
class Ue3GameplaySink final : public EngineSink {
public:
    using LifetimeGeneration = std::uint64_t (*)();
    Ue3GameplaySink(DynarmicHost& host, LifetimeGeneration generation);
    ~Ue3GameplaySink() override;
    Ue3GameplaySink(const Ue3GameplaySink&) = delete;
    Ue3GameplaySink& operator=(const Ue3GameplaySink&) = delete;

    // Call before publishContext/consumeIntoEngine. Captures a bounded live
    // controller/input/world/focused-movie/context identity, and independently
    // resolves actions through InputActionDefinition + IsActionAvailable.
    // Returns semantic action bits, or zero on malformed/ambiguous data.
    std::uint64_t prepare(std::uint32_t controller, std::uint32_t input,
                          std::uint64_t sceneIdentity);
    bool ready() const;
    // No pending payload may survive an input tick without its native consumer.
    void expirePendingAxes();
    // Hooks inside the same input tick. They preserve the native processing:
    // PIProcessInput entry: normalized movement before game scaling.
    // PIPostAdjustInput +0x114: degrees to Rotator units before ignore clamps.
    void beforeAxisProcessing(std::uint32_t input);
    void beforeLookClamp(std::uint32_t input);
    // Destructor hook: never call methods on an object being destroyed.
    void retireInput(std::uint32_t input);

    void beginTouchFrame(const Frame& frame) override;
    void action(const ActionEdge& edge) override;
    void pointer(const PointerEvent& event) override;
    void movement(float right, float forward) override;
    void relativeLook(float yawDegrees, float pitchDegrees) override;
    void endTouchFrame(const Frame& frame) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bl2::touch
