#include "bl2_touch_input.h"
#include <cassert>
#include <cstdio>
using namespace bl2::touch;
int main() {
    Mailbox box;
    Context context{Mode::Menu,UiNavigation,actionBit(Action::UiDown),42};
    auto epoch = box.publishContext(context);
    assert(box.snapshot().context.capabilities == UiNavigation);
    assert(box.submit(epoch,0,0,0,0,0,100)); box.consumeFrame(1,100);
    assert(!box.uiPointer(epoch,0,PointerPhase::Down,.2f,.3f,100));
    assert(box.submit(epoch,1,1,30,30,actionBit(Action::UiDown),101));
    assert(box.submit(epoch,0,0,0,0,0,102));
    auto frame = box.consumeFrame(2,102);
    assert(frame.actionEdgeCount == 2 && frame.actionEdges[0].down && !frame.actionEdges[1].down);
    assert(frame.actionEdges[0].sequence < frame.actionEdges[1].sequence && frame.heldActions == 0);
    assert(frame.moveX == 0 && frame.moveY == 0 && frame.yawDegrees == 0 && frame.pitchDegrees == 0);
    frame = box.consumeFrame(3,102);
    assert(frame.actionEdgeCount == 0);
    context.capabilities |= UiTapTest;
    epoch = box.publishContext(context);
    assert(box.snapshot().context.capabilities == (UiNavigation | UiTapTest));
    assert(!(box.snapshot().context.capabilities & DirectUi));
    assert(box.submit(epoch,0,0,0,0,0,103)); box.consumeFrame(4,103);
    assert(box.uiPointer(epoch,3,PointerPhase::Down,.2f,.3f,104));
    assert(box.uiPointer(epoch,3,PointerPhase::Up,.2f,.3f,105));
    frame = box.consumeFrame(5,105);
    assert(frame.pointerEventCount == 2);
    assert(frame.pointerEvents[0].phase == PointerPhase::Down && frame.pointerEvents[1].phase == PointerPhase::Up);
    std::puts("PASS: additive UI capabilities, navigation-only pointer refusal, short edges, menu axis isolation, tap FIFO");
}
