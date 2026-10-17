#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include <memory>

class DynarmicHost;
namespace bl2::touch {
// Experimental Frontend/Pause/Options semantic widget sink. Keeps stage3 key
// navigation as its fallback; never synthesizes mouse capture or a gamepad.
// Caller must already have passed the canonical whole-ELF SHA/size gate.
// Construct after the SHA gate. Prepare/deliver and destruction of a live sink
// run only at the input boundary; the Host outlives all retained sessions.
class Ue3WidgetSink final : public EngineSink {
public:
    using LifetimeGeneration = std::uint64_t (*)();
    Ue3WidgetSink(DynarmicHost&, LifetimeGeneration);
    ~Ue3WidgetSink() override;
    bool verified() const;
    std::uint32_t prepare(std::uint32_t completeViewport, std::uint32_t controller,
        std::uint32_t movie, std::uint32_t owner, std::uint64_t sceneIdentity);
    void beginTouchFrame(const Frame&) override;
    void action(const ActionEdge&) override;
    void pointer(const PointerEvent&) override;
    void movement(float, float) override {}
    void relativeLook(float, float) override {}
    void endTouchFrame(const Frame&) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
