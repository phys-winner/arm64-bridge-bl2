#include "bl2_touch_ui_transactions.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace bl2::touch;

int main() {
    UiTransactions state;
    state.begin(1,10,true,false,2400,1080);
    assert(!state.action({Action::Back,true,1,1}));
    assert(state.action({Action::Back,false,1,2}) == Action::Back);
    assert(!state.action({Action::Back,false,1,3}));
    assert(!state.action({Action::Fire,true,1,4}));
    assert(!state.action({Action::Fire,false,1,5}));
    state.action({Action::Back,true,1,6}); state.action({Action::Confirm,true,1,7});
    assert(!state.action({Action::Back,false,1,8}));
    assert(!state.action({Action::Confirm,false,1,9}));
    for (Action action : {Action::Back,Action::Confirm,Action::UiUp,Action::UiDown,Action::UiLeft,Action::UiRight}) {
        assert(!state.action({action,true,1,10}));
        state.begin(2,10,true,true,2400,1080);
        assert(!state.action({action,false,1,11})); // Reset release must never commit.
        assert(!state.action({action,false,2,12}));
        state.begin(1,10,true,false,2400,1080);
    }
    state.action({Action::Confirm,true,1,13});
    state.begin(1,11,true,false,2400,1080);
    assert(!state.action({Action::Confirm,false,1,14})); // Same epoch, new target.
    state.action({Action::Confirm,true,1,15});
    state.begin(1,11,false,false,2400,1080);
    assert(!state.action({Action::Confirm,false,1,16})); // Physical takeover.
    state.begin(3,12,true,false,2400,1080);
    auto pointer = [&](int id, PointerPhase phase, float x=.25f, float y=.5f) {
        return state.pointer({id,phase,x,y,3,1});
    };
    assert(!pointer(2,PointerPhase::Down));
    assert(!pointer(2,PointerPhase::Move,.251f,.5f));
    const auto tap = pointer(2,PointerPhase::Up,.252f,.5f);
    assert(tap && tap->x == .25f && tap->y == .5f);
    assert(!pointer(2,PointerPhase::Up));
    pointer(3,PointerPhase::Down); pointer(3,PointerPhase::Move,.4f,.5f);
    assert(!pointer(3,PointerPhase::Up)); // Drag back to origin is still cancelled.
    pointer(3,PointerPhase::Down); assert(!pointer(3,PointerPhase::Cancel));
    assert(!pointer(3,PointerPhase::Up));
    pointer(3,PointerPhase::Down); pointer(4,PointerPhase::Down);
    assert(!pointer(3,PointerPhase::Up));
    assert(!pointer(4,PointerPhase::Up)); // No ownership transfer to remaining finger.
    pointer(3,PointerPhase::Down); state.action({Action::Confirm,true,3,17});
    assert(!pointer(3,PointerPhase::Up));
    assert(!state.action({Action::Confirm,false,3,18})); // Pointer + button cancel each other.
    state.action({Action::Confirm,true,3,19}); pointer(3,PointerPhase::Down);
    assert(!state.action({Action::Confirm,false,3,20}));
    assert(!pointer(3,PointerPhase::Up));
    pointer(5,PointerPhase::Down);
    state.begin(3,12,true,false,1080,2400);
    assert(!pointer(5,PointerPhase::Up)); // Rotation/resize.
    pointer(6,PointerPhase::Down); pointer(6,PointerPhase::Move,std::numeric_limits<float>::quiet_NaN(),.5f);
    assert(!pointer(6,PointerPhase::Up));
    pointer(7,PointerPhase::Down);
    state.begin(4,12,true,false,1080,2400);
    assert(!pointer(7,PointerPhase::Up)); // Epoch replacement.
    state.begin(3,12,true,false,2400,1080);
    for (int i=0;i<10;++i) { pointer(i,PointerPhase::Down); assert(pointer(i,PointerPhase::Up)); }
    std::puts("PASS: UI commit, reset/no-activation, scene, takeover, tap/slop, cancel, multitouch, resize, stale epoch");
}
