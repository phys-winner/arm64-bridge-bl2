#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"

namespace bl2::touch {
// Fan out one already-drained frame. Each concrete sink enforces its mutually
// exclusive capability and owns only its own release/cancel state.
class CombinedSink final : public EngineSink {
public:
    CombinedSink(EngineSink& gameplay, EngineSink& ui) : gameplay_(gameplay), ui_(ui) {}
    void beginTouchFrame(const Frame& frame) override { gameplay_.beginTouchFrame(frame); ui_.beginTouchFrame(frame); }
    void action(const ActionEdge& edge) override { gameplay_.action(edge); ui_.action(edge); }
    void pointer(const PointerEvent& event) override { ui_.pointer(event); }
    void movement(float x, float y) override { gameplay_.movement(x,y); }
    void relativeLook(float yaw, float pitch) override { gameplay_.relativeLook(yaw,pitch); }
    void endTouchFrame(const Frame& frame) override { gameplay_.endTouchFrame(frame); ui_.endTouchFrame(frame); }
private:
    EngineSink& gameplay_;
    EngineSink& ui_;
};
} // namespace bl2::touch
