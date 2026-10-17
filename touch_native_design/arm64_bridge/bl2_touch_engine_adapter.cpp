#include "bl2_touch_engine_adapter.h"

namespace bl2::touch {

bool consumeIntoEngine(Mailbox& mailbox, std::uint64_t engineTick,
                       std::uint64_t nowMs, EngineSink& sink) {
    const Frame frame = mailbox.consumeFrame(engineTick, nowMs);
    if (!frame.fresh) return false;
    sink.beginTouchFrame(frame);
    for (std::size_t i = 0; i < frame.actionEdgeCount; ++i) sink.action(frame.actionEdges[i]);
    for (std::size_t i = 0; i < frame.pointerEventCount; ++i) sink.pointer(frame.pointerEvents[i]);
    sink.movement(frame.moveX, frame.moveY);
    if (frame.yawDegrees != 0 || frame.pitchDegrees != 0) {
        sink.relativeLook(frame.yawDegrees, frame.pitchDegrees);
    }
    sink.endTouchFrame(frame);
    return true;
}

} // namespace bl2::touch
