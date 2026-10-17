#include "bl2_touch_ui_transactions.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace bl2::touch;

int main() {
    {
        const UiPauseResumeIdentity owned{10,11,12,13,14,15,16,100,101,102,103,1200,540};
        UiEditResume edit;
        assert(!edit.request({},1000));
        assert(edit.request(owned,1000) && edit.stage()==1);
        assert(edit.next(owned,true,false,true,1000) && edit.stage()==2);
        // Original Options Cancel changes generation and can create a new
        // Pause native movie/list. Only the game identity must be unchanged.
        auto pause=owned;pause.owner=115;pause.list=116;pause.movie=201;pause.interface=202;
        assert(!edit.next(owned,true,false,true,1016) && edit.stage()==2);
        assert(edit.next(pause,false,true,true,1032) && !edit.stage());
        assert(!edit.next(pause,false,true,true,1048)); // Exactly one Resume.
        for(int changed=0;changed<13;++changed) {
            auto fresh=owned;
            if(changed==0) ++fresh.engine;
            if(changed==1) ++fresh.local;
            if(changed==2) ++fresh.controller;
            if(changed==3) ++fresh.pawn;
            if(changed==4) ++fresh.world;
            if(changed==5) ++fresh.owner;
            if(changed==6) ++fresh.list;
            if(changed==7) ++fresh.viewport;
            if(changed==8) ++fresh.movie;
            if(changed==9) ++fresh.interface;
            if(changed==10) ++fresh.nativeWorld;
            if(changed==11) ++fresh.width;
            if(changed==12) ++fresh.height;
            assert(edit.request(owned,1000));assert(edit.next(owned,true,false,true,1000));
            const bool nativeReplacement=changed==5 || changed==6 || changed==8 || changed==9;
            assert(edit.next(fresh,false,true,true,1032)==nativeReplacement && !edit.stage());
        }
        assert(edit.request(owned,1000));assert(edit.next(owned,true,false,true,1000));
        assert(!edit.next(pause,true,false,true,1032) && !edit.stage()); // Replaced Options is not expected Pause.
        for(int fault=0;fault<6;++fault) {
            assert(edit.request(owned,1000));assert(edit.next(owned,true,false,true,1000));
            assert(!edit.next(fault==0 ? UiPauseResumeIdentity{} : owned,fault==1,false,
                fault!=2,fault==3 ? 999 : fault==4 ? 3001 : 1016));
            if(fault==1) assert(edit.stage()==2); // Still the old Options, no second Cancel.
            else assert(!edit.stage());
            edit.clear();assert(!edit.next(owned,false,true,true,1032));
        }
        assert(edit.request(owned,1000));
        assert(!edit.next(owned,false,true,true,1000)); // Cannot start from another page.
        assert(edit.request(owned,1000));
        assert(!edit.next(owned,true,true,true,1000)); // Ambiguous focus.
    }
    // Invert the actual cooked AnalogScale rate at different native frame
    // deltas; the same pinch ratio must produce the same original scale.
    for (const float dt : {1.f/30,1.f/60,1.f/90,.007f}) for (const float r : {.5f,.8f,1.f,1.25f,1.9375f,2.f}) {
        const auto delta=Bl2MapZoomDelta(2*std::log2(r),.05f,dt);assert(delta);
        const double k=.05f*dt*60,actual=*delta>=0 ? 1+*delta*k : 1/(1-*delta*k);
        assert(std::abs(actual-r)<.000001);
    }
    assert(!Bl2MapZoomDelta(1,0,.016f));assert(!Bl2MapZoomDelta(1,.05f,0));
    assert(!Bl2MapZoomDelta(3,.05f,.016f));
    {
        UiMapDeferredPoint queued;
        queued.store({.5f,.6f,0},7,70,1000);assert(queued.pending());
        const auto point=queued.take(7,70,1016,true);assert(point && point->x==.5f && point->y==.6f);
        assert(!queued.pending() && !queued.take(7,70,1017,true));
        for(unsigned i=0;i<5;++i) {
            queued.store({.5f,.6f,0},7,70,1000);
            assert(!queued.take(i==0 ? 8 : 7,i==1 ? 71 : 70,i==2 ? 999 : i==3 ? 1251 : 1016,i!=4));
            assert(!queued.take(7,70,1016,true)); // Failed ownership never leaves a retry.
        }
        queued.store({.5f,.6f,0},7,70,1000);queued.clear();assert(!queued.take(7,70,1016,true));
        queued.store({-1,.6f,0},7,70,1000);assert(!queued.pending());
    }
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
    state.begin(3,12,true,false,2400,1080,true);
    // Continuous list scroll: one step per kUiScrollPitch (6% of 1080 = 64.8 px)
    // while the finger moves, the remainder carried; never a tap afterwards.
    pointer(1,PointerPhase::Down,.3f,.6f);
    assert(!state.dragging());
    assert(!pointer(1,PointerPhase::Move,.3f,.59f) && !state.dragging()); // Inside the 1.5% slop.
    assert(!pointer(1,PointerPhase::Move,.3f,.55f) && state.dragging()); // 54 px: vertical, no whole step yet.
    auto step = pointer(1,PointerPhase::Move,.3f,.53f);
    assert(step && step->scrollDirection == 1 && !step->tabDirection && step->x == .3f && step->y == .6f);
    assert(!pointer(1,PointerPhase::Move,.3f,.50f)); // 1.67 pitches from the start: the carried remainder.
    step = pointer(1,PointerPhase::Move,.3f,.29f);
    assert(step && step->scrollDirection == 4); // 334.8 px = 5.17 pitches: 4 more after the first.
    assert(state.dragging() && state.ownsPointer(1));
    step = pointer(1,PointerPhase::Move,.4f,.40f); // Locked vertical even when it drifts sideways.
    assert(step && step->scrollDirection == -1);
    assert(!pointer(1,PointerPhase::Up,.4f,.41f) && !state.dragging()); // No tap after a scroll.
    pointer(1,PointerPhase::Down,.3f,.3f);
    step = pointer(1,PointerPhase::Up,.3f,.62f); // A fast flick reports its whole distance at UP.
    assert(step && step->scrollDirection == -5);
    pointer(1,PointerPhase::Down,.3f,.95f);
    // 982.8 px is 15.2 pitches; one sample yields at most 8, the next sample the rest.
    step = pointer(1,PointerPhase::Move,.3f,.04f);
    assert(step && step->scrollDirection == 8);
    step = pointer(1,PointerPhase::Move,.3f,.04f);
    assert(step && step->scrollDirection == 7);
    pointer(1,PointerPhase::Up,.3f,.04f);
    // Horizontal swipe: one tab at UP, + when the finger moves left.
    pointer(1,PointerPhase::Down,.5f,.5f);
    assert(!pointer(1,PointerPhase::Move,.42f,.51f) && state.dragging());
    const auto nextTab = pointer(1,PointerPhase::Up,.35f,.52f); // 360 px left.
    assert(nextTab && nextTab->tabDirection == 1 && !nextTab->scrollDirection && nextTab->x == .5f && nextTab->y == .5f);
    pointer(1,PointerPhase::Down,.3f,.3f); pointer(1,PointerPhase::Move,.5f,.4f);
    const auto previousTab = pointer(1,PointerPhase::Up,.5f,.4f);
    assert(previousTab && previousTab->tabDirection == -1);
    pointer(1,PointerPhase::Down,.5f,.5f);
    assert(!pointer(1,PointerPhase::Up,.47f,.5f)); // 72 px < 8% of 1080: neither a tab nor a tap.
    pointer(1,PointerPhase::Down,.3f,.3f); pointer(1,PointerPhase::Move,.4f,.31f);
    assert(!pointer(1,PointerPhase::Up,.5f,.6f)); // Ends steeper than 1.5:1: no tab, never a scroll.
    pointer(1,PointerPhase::Down,.3f,.6f); pointer(2,PointerPhase::Down,.4f,.6f);
    assert(!pointer(1,PointerPhase::Move,.3f,.3f)); // A second finger cancels steps too.
    assert(!pointer(1,PointerPhase::Up,.3f,.3f) && !pointer(2,PointerPhase::Up));
    pointer(1,PointerPhase::Down,.3f,.6f); state.action({Action::UiAction,true,3,21});
    assert(!pointer(1,PointerPhase::Move,.3f,.3f) && !pointer(1,PointerPhase::Up,.3f,.3f)); // Key + drag cancel.
    assert(!state.action({Action::UiAction,false,3,22}));
    pointer(1,PointerPhase::Down,.3f,.6f); pointer(1,PointerPhase::Move,.3f,.5f);
    assert(!pointer(1,PointerPhase::Cancel,.3f,.3f)); // Cancel commits nothing more.
    pointer(1,PointerPhase::Down,.3f,.6f);
    state.begin(3,13,true,false,2400,1080,true);
    assert(!pointer(1,PointerPhase::Up,.3f,.3f)); // No swipe into a replacement movie.
    state.begin(3,13,true,false,2400,1080,false);
    pointer(1,PointerPhase::Down,.3f,.6f);
    assert(!pointer(1,PointerPhase::Move,.3f,.3f) && !state.dragging()); // Dialogs: no swipe navigation.
    assert(!pointer(1,PointerPhase::Up,.3f,.3f));
    for (Action action : {Action::UiAction,Action::UiOther,Action::UiPreviousTab,Action::UiNextTab}) {
        assert(!state.action({action,true,3,30}));
        assert(state.action({action,false,3,31}) == action); // Protocol 6 menu keys commit like Confirm.
    }
    {
        UiMapGesture map;
        const auto start = [&] { map.begin(7,70,true,false,2400,1080); };
        start();
        const auto event = [&](int id,PointerPhase phase,float x,float y,std::uint64_t time=100,bool inside=true) {
            return map.pointer({id,phase,x,y,7,0,time},inside);
        };
        assert(!event(1,PointerPhase::Down,.1f,.1f,100,false));
        auto u = event(1,PointerPhase::Down,.5f,.5f);assert(u && u->start && !u->waypoint && map.captured());
        u = event(1,PointerPhase::Move,.6f,.4f);assert(u && u->dx==240 && u->dy==-108 && !u->zoom);
        u = event(1,PointerPhase::Up,.6f,.4f,900);assert(u && u->stop && !u->waypoint && !map.captured());
        event(1,PointerPhase::Down,.5f,.5f);
        u = event(1,PointerPhase::Up,.5f,.5f,650);assert(u && u->stop && u->waypoint);
        event(1,PointerPhase::Down,.4f,.5f);u=event(2,PointerPhase::Down,.6f,.5f);
        assert(u && u->stop && !u->start);
        u=event(2,PointerPhase::Move,.8f,.5f);assert(u && u->zoom>1.99f && u->zoom<=2 && !u->dx && !u->dy);
        event(2,PointerPhase::Up,.8f,.5f);u=event(1,PointerPhase::Up,.4f,.5f,950);
        assert(u && !u->waypoint && !u->start && !map.captured());
        for (const auto invalid : {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-1.f,2.f}) {
            event(1,PointerPhase::Down,.5f,.5f);u=event(1,PointerPhase::Move,invalid,.5f);
            assert(u && u->stop && !u->dx && !u->dy && !u->zoom && !u->waypoint);
            u=event(1,PointerPhase::Up,.5f,.5f,950);assert(u && !u->waypoint && !map.captured());
        }
        event(1,PointerPhase::Down,.4f,.5f);event(2,PointerPhase::Down,.6f,.5f);
        event(3,PointerPhase::Down,.5f,.6f);
        u=event(2,PointerPhase::Move,.8f,.5f);assert(u && !u->zoom && !u->dx && !u->waypoint);
        event(1,PointerPhase::Up,.4f,.5f);event(2,PointerPhase::Up,.8f,.5f);event(3,PointerPhase::Up,.5f,.6f);
        assert(!map.captured());
        event(1,PointerPhase::Down,.5f,.5f);map.begin(7,71,true,false,2400,1080);
        assert(!event(1,PointerPhase::Up,.5f,.5f,950));
        map.begin(7,70,true,false,2400,1080);event(1,PointerPhase::Down,.5f,.5f);
        map.begin(7,70,false,false,2400,1080);assert(!event(1,PointerPhase::Up,.5f,.5f,950));
    }
    std::puts("PASS: UI commit, reset/no-activation, scene, takeover, tap/slop, cancel, multitouch, resize, stale epoch");
}
