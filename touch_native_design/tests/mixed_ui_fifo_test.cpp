#include "bl2_touch_engine_adapter.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
using namespace bl2::touch;
namespace {
struct Event { std::uint64_t sequence; std::string name; };
struct Sink final : EngineSink {
    std::vector<Event> events;
    std::vector<Frame> frames;
    Mailbox* mailbox = nullptr;
    bool changed = false;
    void beginTouchFrame(const Frame& f) override { frames.push_back(f); }
    void action(const ActionEdge& e) override {
        if (mailbox) { (void)mailbox->snapshot(); changed = true; } // Mutex is not held.
        events.push_back({e.sequence,"A" + std::to_string(static_cast<unsigned>(e.action)) + (e.down ? "+" : "-")});
    }
    void pointer(const PointerEvent& e) override { events.push_back({e.sequence,"P" + std::to_string(static_cast<unsigned>(e.phase))}); }
    void movement(float,float) override {}
    void relativeLook(float,float) override {}
    void endTouchFrame(const Frame&) override {}
};
std::uint64_t start(Mailbox& mailbox, std::uint32_t caps = UiNavigation | UiWidgetsTest) {
    Context c; c.mode = Mode::Menu; c.capabilities = caps; c.sceneIdentity = 10;
    c.availableActions = actionBit(Action::Back) | actionBit(Action::Confirm) | actionBit(Action::UiDown);
    const auto epoch = mailbox.publishContext(c);
    mailbox.consumeFrame(1,100); // Retire context reset before arming a new stream.
    assert(mailbox.submit(epoch,0,0,0,0,0,100)); return epoch;
}
void action(Mailbox& m, std::uint64_t epoch, Action a, bool down) {
    assert(m.submit(epoch,0,0,0,0,down ? actionBit(a) : 0,101));
}
void pointer(Mailbox& m, std::uint64_t epoch, PointerPhase phase, float x = .4f) {
    assert(m.uiPointer(epoch,7,phase,x,.5f,101));
}
void increasing(const std::vector<Event>& events) {
    for (std::size_t i = 1; i < events.size(); ++i) assert(events[i].sequence > events[i - 1].sequence);
}
}
int main() {
    {
        Mailbox m; const auto epoch = start(m); Sink s; s.mailbox = &m;
        pointer(m,epoch,PointerPhase::Down);
        pointer(m,epoch,PointerPhase::Move,.45f);
        action(m,epoch,Action::Back,true);
        pointer(m,epoch,PointerPhase::Move,.5f);
        action(m,epoch,Action::Back,false);
        pointer(m,epoch,PointerPhase::Up);
        assert(consumeIntoEngine(m,2,102,s));
        const std::vector<std::string> expected{"P0","P1","A15+","P1","A15-","P2"};
        assert(s.events.size() == expected.size() && s.changed);
        for (std::size_t i = 0; i < expected.size(); ++i) assert(s.events[i].name == expected[i]);
        increasing(s.events);
    }
    {
        Mailbox m; const auto epoch = start(m); Sink s;
        pointer(m,epoch,PointerPhase::Down);
        pointer(m,epoch,PointerPhase::Move,.45f);
        pointer(m,epoch,PointerPhase::Move,.5f); // Adjacent moves still coalesce.
        pointer(m,epoch,PointerPhase::Up);
        consumeIntoEngine(m,2,102,s);
        assert(s.events.size() == 3 && s.frames[0].pointerEvents[1].surfaceX == .5f);
        increasing(s.events);
    }
    {
        Mailbox m; const auto epoch = start(m); Sink s;
        // 26 action edges fit the per-action producer rings, but exceed Frame25.
        for (int i = 0; i < 7; ++i) { action(m,epoch,Action::Confirm,true); action(m,epoch,Action::Confirm,false); }
        for (int i = 0; i < 6; ++i) { action(m,epoch,Action::Back,true); action(m,epoch,Action::Back,false); }
        pointer(m,epoch,PointerPhase::Down); pointer(m,epoch,PointerPhase::Up);
        consumeIntoEngine(m,2,102,s);
        assert(s.frames[0].actionEdgeCount == 25 && s.frames[0].pointerEventCount == 0);
        assert(s.frames[0].heldActions == actionBit(Action::Back));
        consumeIntoEngine(m,3,103,s);
        assert(s.frames[1].actionEdgeCount == 1 && s.frames[1].pointerEventCount == 2 && s.frames[1].heldActions == 0);
        assert(s.events.size() == 28); increasing(s.events);
        assert(!consumeIntoEngine(m,3,103,s));
    }
    {
        Mailbox m; const auto epoch = start(m); Sink s;
        for (int i = 0; i < 7; ++i) { action(m,epoch,Action::Confirm,true); action(m,epoch,Action::Confirm,false); }
        for (int i = 0; i < 5; ++i) { action(m,epoch,Action::Back,true); action(m,epoch,Action::Back,false); }
        action(m,epoch,Action::Back,true); // Exactly Frame25 now.
        pointer(m,epoch,PointerPhase::Down); pointer(m,epoch,PointerPhase::Up);
        action(m,epoch,Action::Back,false); // This edge is deferred, earlier pointers aren't.
        consumeIntoEngine(m,2,102,s);
        assert(s.frames[0].actionEdgeCount == 25 && s.frames[0].pointerEventCount == 2);
        consumeIntoEngine(m,3,103,s); assert(s.frames[1].actionEdgeCount == 1);
        increasing(s.events);
    }
    {
        Mailbox m; Context c; c.mode = Mode::Gameplay; c.capabilities = GameplayInput | RelativeLook;
        c.availableActions = actionBit(Action::Fire); c.sceneIdentity = 3;
        const auto epoch = m.publishContext(c); m.consumeFrame(1,100);
        assert(m.submit(epoch,0,0,0,0,0,100));
        action(m,epoch,Action::Fire,true); action(m,epoch,Action::Fire,false);
        Sink s; consumeIntoEngine(m,2,102,s); assert(s.events.size() == 1 && s.frames[0].heldActions == actionBit(Action::Fire));
        consumeIntoEngine(m,3,103,s); assert(s.events.size() == 2 && s.frames[1].heldActions == 0);
    }
    {
        Mailbox m; const auto epoch = start(m); Sink s;
        pointer(m,epoch,PointerPhase::Down); action(m,epoch,Action::Back,true);
        consumeIntoEngine(m,2,102,s); m.reset(ManualReset);
        assert(!m.uiPointer(epoch,7,PointerPhase::Up,.4f,.5f,103));
        consumeIntoEngine(m,3,103,s);
        assert(s.frames.back().resetReasons & ManualReset);
        assert(s.frames.back().actionEdgeCount == 1 && !s.frames.back().actionEdges[0].down);
        assert(s.frames.back().pointerEventCount == 1 && s.frames.back().pointerEvents[0].phase == PointerPhase::Cancel);
        increasing(s.events);
    }
    std::cout << "PASS: mixed UI FIFO, interleaved MOVE barrier, Frame capacity, reset and gameplay duration\n";
}
