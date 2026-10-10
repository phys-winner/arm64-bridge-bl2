#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include "bl2_touch_ui_transactions.h"
#include <cstdint>
#include <functional>
#include <memory>

class DynarmicHost;
namespace bl2::touch {

// Reviewed-image UI probe. Construction/admission never imply hardware
// acceptance. ui-test admits six release-committed navigation commands;
// ui-tap-test additionally probes absolute GFx taps. It does not admit drag,
// scroll, pinch, inventory/map, or DirectUi.
class Ue3UiSink final : public EngineSink {
public:
    using LifetimeGeneration = std::uint64_t (*)();
    Ue3UiSink(DynarmicHost& host, LifetimeGeneration generation, bool tapTest);
    ~Ue3UiSink() override;
    std::uint32_t prepare(std::uint32_t completeViewport, std::uint32_t controller,
        std::uint32_t movie, std::uint32_t owner, std::uint64_t sceneIdentity);
    bool verified() const;
    void beginTouchFrame(const Frame& frame) override;
    void action(const ActionEdge& edge) override;
    void pointer(const PointerEvent& event) override;
    // Widget screens give their passive footer first refusal, retaining list drags.
    bool footerPointer(const PointerEvent&,bool allowCapture);
    void setFooterActionHandler(std::function<bool(const ActionEdge&)>);
    // Hover is original MouseEvent::MouseMove only; it never presses/activates.
    void hover(const PointerEvent&);
    void movement(float, float) override {}
    void relativeLook(float, float) override {}
    void endTouchFrame(const Frame& frame) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bl2::touch
