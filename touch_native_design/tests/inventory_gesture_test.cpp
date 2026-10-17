#include "bl2_touch_inventory_gesture.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace bl2::touch;

// The original AS2 inventory requires bMouseDown before movement may start
// DragManager. Its cancelDrag/finishDrag(true) must not perform a drop. This
// small callback consumer makes the deferred-start regression observable.
struct NativeInventory {
    bool mouseDown = false, dragging = false;
    unsigned clicks = 0, drops = 0, cancels = 0, presses = 0, moves = 0;
    float pressedX = 0, pressedY = 0, mouseX = 0, mouseY = 0;
    void press(float x,float y) {
        assert(!mouseDown);mouseDown = true;++presses;pressedX=mouseX=x;pressedY=mouseY=y;
    }
    void move(float x,float y) {
        assert(mouseDown);dragging=true;++moves;mouseX=x;mouseY=y;
    }
    void up() {
        assert(mouseDown);if(dragging) ++drops;else ++clicks;
        mouseDown=dragging=false;
    }
    void dispatch(const std::optional<InventoryDragUpdate>& u) {
        if(!u) return;
        switch(u->command) {
        case InventoryDragCommand::Click:press(u->startX,u->startY);up();break;
        case InventoryDragCommand::StartMove:press(u->startX,u->startY);move(u->x,u->y);break;
        case InventoryDragCommand::Move:move(u->x,u->y);break;
        case InventoryDragCommand::Drop:mouseX=u->x;mouseY=u->y;up();break;
        case InventoryDragCommand::Cancel:
            assert(mouseDown);++cancels;dragging=false;mouseDown=false;break;
        }
    }
};

int main() {
    const InventoryDragOwner owner{InventoryDragFamily::Bank,10,20,30,40,50,60,70,2400,1080};
    const InventoryDragSource source{{100,101,0,8},{100,102,0,8},1000};
    const auto event = [](PointerPhase phase,float x,float y,std::uint64_t seq,
                          std::uint64_t time=1000,std::int32_t id=3,std::uint64_t epoch=10) {
        return PointerEvent{id,phase,x,y,epoch,seq,time};
    };
    {
        UiInventoryGesture g;NativeInventory native;
        native.dispatch(g.begin(owner,true,false,1000));
        native.dispatch(g.pointer(event(PointerPhase::Down,.2f,.3f,1),source));
        assert(g.captured() && !g.held() && !native.mouseDown && !native.clicks);
        native.dispatch(g.pointer(event(PointerPhase::Move,.202f,.302f,2),source));
        assert(!native.presses); // Finger slop remains local, not native DOWN.
        native.dispatch(g.pointer(event(PointerPhase::Up,.202f,.302f,3),source));
        assert(native.clicks==1 && !native.drops && !g.captured());
        native.dispatch(g.pointer(event(PointerPhase::Up,.202f,.302f,4),source));
        assert(native.clicks==1); // Duplicate UP never activates a second item.
    }
    for(const auto family:{InventoryDragFamily::Bank,InventoryDragFamily::Stash,
        InventoryDragFamily::Vending,InventoryDragFamily::Trading}) {
        auto current=owner;current.family=family;
        UiInventoryGesture g;NativeInventory native;
        g.begin(current,true,false,1000);g.pointer(event(PointerPhase::Down,.2f,.3f,1),source);
        native.dispatch(g.pointer(event(PointerPhase::Move,.35f,.32f,2),source));
        assert(g.held() && native.dragging && native.pressedX==.2f && native.pressedY==.3f);
        assert(native.mouseX==.35f && !native.clicks && !native.drops);
        g.begin(current,true,false,1016);
        native.dispatch(g.pointer(event(PointerPhase::Move,.55f,.5f,3,1016),source));
        native.dispatch(g.pointer(event(PointerPhase::Up,.6f,.6f,4,1016),source));
        assert(native.presses==1 && native.moves==2 && native.drops==1 && !native.clicks && !native.mouseDown);
    }
    {
        UiInventoryGesture g;NativeInventory native;g.begin(owner,true,false,1000);
        g.pointer(event(PointerPhase::Down,.2f,.3f,1),source);
        native.dispatch(g.pointer(event(PointerPhase::Up,.7f,.8f,2),source));
        assert(!g.captured() && !native.presses && !native.clicks && !native.drops);
    }
    for(int held=0;held<2;++held) {
        UiInventoryGesture g;NativeInventory native;g.begin(owner,true,false,1000);
        g.pointer(event(PointerPhase::Down,.2f,.3f,1),source);
        if(held) native.dispatch(g.pointer(event(PointerPhase::Move,.35f,.3f,2),source));
        native.dispatch(g.pointer(event(PointerPhase::Down,.8f,.8f,3,1000,4),source));
        assert(g.ownsPointer(3) && g.ownsPointer(4));
        native.dispatch(g.pointer(event(PointerPhase::Up,.35f,.3f,4),source));
        native.dispatch(g.pointer(event(PointerPhase::Up,.8f,.8f,5,1000,4),source));
        assert(!g.captured() && !native.clicks && !native.drops && native.cancels==unsigned(held));
    }
    // The same world/controller/scene/cell must survive every captured frame.
    for(int changed=0;changed<12;++changed) {
        UiInventoryGesture g;g.begin(owner,true,false,1000);
        g.pointer(event(PointerPhase::Down,.2f,.3f,1),source);
        assert(g.pointer(event(PointerPhase::Move,.35f,.3f,2),source));
        auto current=owner;
        if(changed==0) current.family=InventoryDragFamily::None;
        if(changed==1) ++current.epoch;
        if(changed==2) ++current.scene;
        if(changed==3) ++current.generation;
        if(changed==4) ++current.owner;
        if(changed==5) ++current.controller;
        if(changed==6) ++current.world;
        if(changed==7) ++current.viewport;
        if(changed==8) ++current.width;
        if(changed==9) ++current.height;
        const auto cancelled=g.begin(current,changed!=10,changed==11,1016);
        assert(cancelled && cancelled->command==InventoryDragCommand::Cancel && cancelled->owner==owner);
        assert(!g.captured() && !g.pointer(event(PointerPhase::Up,.35f,.3f,3,1016),source));
    }
    for(int fault=0;fault<9;++fault) {
        UiInventoryGesture g;NativeInventory native;g.begin(owner,true,false,1000);
        g.pointer(event(PointerPhase::Down,.2f,.3f,1),source);
        native.dispatch(g.pointer(event(PointerPhase::Move,.35f,.3f,2),source));
        auto fresh=source;auto e=event(PointerPhase::Up,.5f,.5f,3);
        if(fault==0) ++fresh.panel.data;
        if(fault==1) ++fresh.cell.data;
        if(fault==2) ++fresh.content;
        if(fault==3) fresh={};
        if(fault==4) e.sequence=2;
        if(fault==5) e.timestampMs=999;
        if(fault==6) e.timestampMs=1001;
        if(fault==7) e.surfaceX=std::numeric_limits<float>::quiet_NaN();
        if(fault==8) e.phase=PointerPhase::Cancel;
        native.dispatch(g.pointer(e,fresh));
        assert(native.cancels==1 && !native.drops && !native.clicks && !native.mouseDown);
    }
    {
        UiInventoryGesture g;NativeInventory native;g.begin(owner,true,false,1000);
        g.pointer(event(PointerPhase::Down,.2f,.3f,1),source);
        native.dispatch(g.pointer(event(PointerPhase::Move,.35f,.3f,2),source));
        assert(!g.pointer(event(PointerPhase::Up,.5f,.5f,3,1000,3,9),source));
        assert(g.held()); // A reused old-epoch pointer cannot affect this drag.
        native.dispatch(g.pointer(event(PointerPhase::Up,.5f,.5f,3),source));
        assert(native.drops==1);
        g.begin(owner,true,false,3000);
        assert(!g.pointer(event(PointerPhase::Down,.2f,.3f,4,1000),source));
        assert(!g.captured()); // Expired queued DOWN is not a fresh admission.
    }
    {
        UiInventoryGesture g;NativeInventory native;g.begin(owner,true,false,1000);
        g.pointer(event(PointerPhase::Down,.2f,.3f,1),source);
        native.dispatch(g.pointer(event(PointerPhase::Move,.35f,.3f,2),source));
        native.dispatch(g.cancel());native.dispatch(g.cancel());
        assert(native.cancels==1 && !g.captured() && !g.held());
    }
    assert(!InventoryDragOwner{} && !InventoryDragSource{});
    std::puts("PASS inventory deferred DOWN/click UP/drop/cancel/source/world/epoch/physical/time controls");
}
