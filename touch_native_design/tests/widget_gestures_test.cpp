#include "bl2_touch_widget_gestures.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
using namespace bl2::touch;
namespace {
WidgetRegistry registry() {
    WidgetRegistry r;
    r.valid = true; r.route = 11; r.content = 22; r.model = 33;
    r.surfaceWidth = 2000; r.surfaceHeight = 1000;
    r.listBounds = {.1,.1,.9,.9}; r.rowCount = 10; r.totalRenderers = 3;
    for (int i = 0; i < 3; ++i) {
        WidgetTarget row;
        row.id = 100 + i; row.index = i; row.eventId = 6 + i;
        row.enabled = row.visible = true; row.kind = WidgetKind::Button; row.caption = "row";
        row.bounds = {.1,.2 + .2 * i,.9,.35 + .2 * i};
        row.origin={.1,.2+.2*i};
        row.renderer = {1,static_cast<std::uint32_t>(100 + i),0,10};
        row.data = {1,static_cast<std::uint32_t>(200 + i),0,8};
        r.rows.push_back(row);
    }
    return r;
}
PointerEvent e(int id, PointerPhase phase, float x, float y, std::uint64_t epoch = 5) {
    return {id,phase,x,y,epoch,0};
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
WidgetRegistry audioRegistry() {
    auto r=registry();r.surfaceWidth=3636;r.surfaceHeight=1620;
    r.listBounds={.121543,.066632,.384230,.772370};
    r.rowCount=9;r.totalRenderers=7;r.selectedIndex=0;
    // The selected Music artwork overlaps Effects. Effects' visible thumb
    // also lies just below its renderer's getBounds, in Dialogue's artwork.
    const GfxRect bounds[]{
        {.121543,.066632,.375472,.278819},
        {.150098,.204236,.375472,.285347},
        {.150098,.290278,.375472,.371389}};
    const double axisY[]{232.0/1080,317.0/1080,410.0/1080};
    for(std::size_t i=0;i<r.rows.size();++i) {
        auto& row=r.rows[i];row.kind=WidgetKind::Slider;
        row.bounds=bounds[i];row.origin={.150098,.118194+i*.086042};
        row.sliderBounds={.17,row.bounds.top,.375472,row.bounds.bottom};
        // Perspective makes the live track slightly diagonal. These fixture
        // axes model the visible endpoints, independent of decorative bounds.
        row.sliderStart={.17,axisY[i]-.02};row.sliderEnd={896.0/2424,axisY[i]};
        row.minimum=0;row.maximum=10;row.increment=1;row.value=10;
    }
    return r;
}
}
int main() {
    {
        auto r=registry();r.selectedIndex=1;
        auto& row=r.rows[0];row.kind=WidgetKind::Spinner;
        row.minimum=0;row.maximum=1;row.increment=1;row.value=0;
        WidgetGestures g(true);g.begin(5,9,true,false,r);
        auto command=g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        assert(command && command->kind==WidgetCommandKind::Select);
        // The real DOWN setter changes selection before UP and the next
        // engine frame may publish new selection artwork/content.
        r.selectedIndex=0;++r.content;row.bounds.left-=.02;
        assert(g.rebindSelection(r));
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
        assert(g.pointer(e(7,PointerPhase::Down,.4f,.25f),r)->kind==WidgetCommandKind::Select);
        command=g.pointer(e(7,PointerPhase::Up,.4f,.25f),r);
        assert(command && command->kind==WidgetCommandKind::Click);
        row.kind=WidgetKind::Slider;row.minimum=0;row.maximum=10;row.value=5;
        row.sliderBounds={.3,.2,.8,.35};row.sliderStart={.3,.275};row.sliderEnd={.8,.275};
        r.selectedIndex=1;
        assert(g.pointer(e(7,PointerPhase::Down,.4f,.275f),r)->kind==WidgetCommandKind::Select);
        r.selectedIndex=0;assert(g.rebindSelection(r));
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.275f),r));
        g.pointer(e(7,PointerPhase::Down,.4f,.275f),r);
        command=g.pointer(e(7,PointerPhase::Up,.4f,.275f),r);
        assert(command && command->kind==WidgetCommandKind::Slider && near(command->sliderFraction,.2));
        // Caption and axis taps share the first-selection rule. An explicit
        // horizontal drag retains the expanded axis and applies immediately.
        r.selectedIndex=1;g.pointer(e(7,PointerPhase::Down,.2f,.25f),r);
        r.selectedIndex=0;assert(g.rebindSelection(r));
        assert(!g.pointer(e(7,PointerPhase::Up,.2f,.25f),r));
        r.selectedIndex=1;g.pointer(e(7,PointerPhase::Down,.4f,.275f),r);
        r.selectedIndex=0;assert(g.rebindSelection(r));
        command=g.pointer(e(7,PointerPhase::Move,.7f,.275f),r);
        assert(command && command->kind==WidgetCommandKind::Slider && near(command->sliderFraction,.8));
        assert(!g.pointer(e(7,PointerPhase::Cancel,.7f,.275f),r));
        // Buttons still highlight on DOWN and commit once on the same UP,
        // including a button that was inactive when pressed.
        row.kind=WidgetKind::Button;r.selectedIndex=1;
        assert(g.pointer(e(7,PointerPhase::Down,.4f,.25f),r)->kind==WidgetCommandKind::Select);
        r.selectedIndex=0;assert(g.rebindSelection(r));
        command=g.pointer(e(7,PointerPhase::Up,.4f,.25f),r);
        assert(command && command->kind==WidgetCommandKind::Click);
        row.kind=WidgetKind::Spinner;
        for(int fault=0;fault<4;++fault) {
            r.selectedIndex=1;g.begin(5,9,true,false,r);
            g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);r.selectedIndex=0;
            if(fault==0) g.pointer(e(7,PointerPhase::Cancel,.4f,.25f),r);
            if(fault==1) g.pointer(e(8,PointerPhase::Down,.4f,.45f),r);
            if(fault==2) ++row.data.data;
            if(fault==3) g.begin(6,9,true,false,r);
            assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
            if(fault==1) assert(!g.pointer(e(8,PointerPhase::Up,.4f,.45f),r));
        }
    }
    {
        GfxViewport v; v.words = {2400,1080,240,0,1920,1080};
        WidgetPoint p;
        assert(NormalizeWidgetPoint({0,0,1280,720},v,2400,1080,{640,360},p));
        assert(near(p.x,.5) && near(p.y,.5));
        assert(NormalizeWidgetPoint({-160,0,1440,720},v,2400,1080,{0,0},p));
        assert(near(p.x,.18) && near(p.y,0));
        assert(!NormalizeWidgetPoint({0,0,1280,720},v,1920,1080,{640,360},p));
        v.words[2] = 800; assert(!NormalizeWidgetPoint({0,0,1280,720},v,2400,1080,{0,0},p));
        WidgetTarget slider; slider.sliderStart = {.25,.25}; slider.sliderEnd = {.75,.75};
        double fraction = 0;
        assert(ProjectWidgetSlider(slider,{.75,.25},2000,1000,fraction) && near(fraction,.8));
        assert(ProjectWidgetSlider(slider,{0,0},2000,1000,fraction) && fraction == 0);
        assert(ProjectWidgetSlider(slider,{1,1},2000,1000,fraction) && fraction == 1);
    }
    {
        auto r=registry();auto& row=r.rows[0];
        row.kind=WidgetKind::Spinner;row.valueBounds={.6,.2,.8,.35};row.minimum=0;row.maximum=4;row.increment=1;row.value=2;
        WidgetGestures g;g.begin(5,9,true,false,r);
        auto down=e(7,PointerPhase::Down,.7f,.25f);down.timestampMs=1000;g.pointer(down,r);
        auto move=e(7,PointerPhase::Move,.9f,.25f);move.timestampMs=1300;
        auto command=g.pointer(move,r);assert(command && command->kind==WidgetCommandKind::Spinner && command->spinnerIndex==4);
        row.value=4;
        ++row.id;++r.route;++row.renderer.data;
        assert(g.rebindSpinner(r));
        move.surfaceX=.67f;move.timestampMs=1400;command=g.pointer(move,r);assert(command && command->spinnerIndex==0);
        assert(!g.pointer(e(7,PointerPhase::Cancel,.67f,.25f),r));
        down.timestampMs=2000;g.pointer(down,r);move.timestampMs=2100;
        assert(!g.pointer(move,r));assert(!g.pointer(e(7,PointerPhase::Up,.67f,.25f),r));
        row.kind=WidgetKind::Slider;row.minimum=0;row.maximum=10;row.value=5;
        down.timestampMs=3000;g.pointer(down,r);move.surfaceX=.715f;move.timestampMs=3300;
        command=g.pointer(move,r);assert(command && command->kind==WidgetCommandKind::Slider && near(command->sliderFraction,.6));
        g.cancel();assert(!g.pointer(e(7,PointerPhase::Up,.715f,.25f),r));
    }
    {
        auto r = registry(); auto& row = r.rows[0];
        row.kind = WidgetKind::Slider; row.sliderBounds = {.3,.2,.8,.35};
        row.sliderStart = {.3,.275}; row.sliderEnd = {.8,.275};
        WidgetGestures g; g.begin(5,9,true,false,r);
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        auto command = g.pointer(e(7,PointerPhase::Up,.4f,.25f),r);
        assert(command && command->kind == WidgetCommandKind::Click);
        r.selectedIndex = 0;
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        command = g.pointer(e(7,PointerPhase::Up,.4f,.25f),r);
        assert(command && command->kind == WidgetCommandKind::Slider);
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        command = g.pointer(e(7,PointerPhase::Move,.4f,.1f),r);
        assert(command && command->kind == WidgetCommandKind::Scroll);
        assert(!g.pointer(e(7,PointerPhase::Cancel,.4f,.1f),r));
        row.kind = WidgetKind::Spinner;
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        command = g.pointer(e(7,PointerPhase::Up,.4f,.25f),r);
        assert(command && command->kind == WidgetCommandKind::Click);
    }
    {
        auto r = registry(); WidgetGestures g; g.begin(5,9,true,false,r);
        assert(!g.pointer(e(7,PointerPhase::Down,.4f,.25f),r));
        assert(g.captured() && g.target()->id == 100);
        auto cmd = g.pointer(e(7,PointerPhase::Up,.4f,.25f),r);
        assert(cmd && cmd->kind == WidgetCommandKind::Click && cmd->target.eventId == 6 && !g.captured());
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
    }
    for (int fault = 0; fault < 7; ++fault) {
        auto r = registry(); WidgetGestures g; g.begin(5,9,true,false,r);
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        if (fault == 0) ++r.route;
        if (fault == 1) r.rows[0].enabled = false;
        if (fault == 2) r.rows[0].caption = "changed";
        if (fault == 3) ++r.rows[0].data.data;
        if (fault == 4) g.cancel();
        if (fault == 5) g.begin(6,9,true,false,r);
        if (fault == 6) g.begin(5,9,true,true,r);
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
    }
    {
        auto r = registry(); WidgetGestures g; g.begin(5,9,true,false,r);
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        g.pointer(e(8,PointerPhase::Down,.4f,.45f),r);
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
        assert(!g.pointer(e(8,PointerPhase::Up,.4f,.45f),r));
        g.pointer(e(8,PointerPhase::Down,.4f,.45f),r);
        assert(g.pointer(e(8,PointerPhase::Up,.4f,.45f),r));
    }
    {
        auto r = registry(); WidgetGestures g; g.begin(5,9,true,false,r);
        r.rows[0].bounds.top=.08; // Selected artwork must not change row pitch.
        g.pointer(e(7,PointerPhase::Down,.4f,.65f),r);
        const auto scroll = g.pointer(e(7,PointerPhase::Move,.4f,.45f),r);
        assert(scroll && scroll->kind == WidgetCommandKind::Scroll && scroll->scrollPosition == 1);
        r.scrollPosition = 1; ++r.content;
        for (auto& row : r.rows) { ++row.index; ++row.id; ++row.data.data; }
        const auto next = g.pointer(e(7,PointerPhase::Move,.4f,.25f),r);
        assert(next && next->scrollPosition == 2); // Own scroll can recycle renderers.
        g.cancel(); assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
    }
    {
        auto r = registry(); auto& row = r.rows[0];
        row.kind = WidgetKind::Slider; row.sliderBounds = {.3,.2,.8,.35};
        row.sliderStart = {.3,.275}; row.sliderEnd = {.8,.275};
        row.minimum = 0; row.maximum = 10; row.increment = 1; row.slider = {1,300,0,10};
        WidgetGestures g; g.begin(5,9,true,false,r);
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        auto command = g.pointer(e(7,PointerPhase::Move,.7f,.1f),r);
        assert(command && command->kind == WidgetCommandKind::Slider && near(command->sliderFraction,.8));
        row.value = 8; // An applied setting is not a new widget/gesture.
        command = g.pointer(e(7,PointerPhase::Move,.9f,.1f),r);
        assert(command && command->sliderFraction == 1);
        assert(!g.pointer(e(7,PointerPhase::Move,.9f,.1f),r));
        assert(!g.pointer(e(7,PointerPhase::Cancel,.9f,.1f),r));
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        ++row.slider.data;
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
    }
    {
        // Actual Options geometry on Mali: selected Gamepad's decoration
        // shifts its x centre towards the tap on the preceding Video row.
        auto r=registry();r.surfaceWidth=2340;r.surfaceHeight=1080;
        r.rows[0].bounds={.137537,.228750,.371361,.286111};
        r.rows[1].bounds={.107958,.254514,.371361,.395972};
        r.rows[2].bounds={.137537,.395972,.371361,.453333};
        WidgetGestures g;g.begin(5,9,true,false,r);
        g.pointer(e(7,PointerPhase::Down,480.f/2340,275.f/1080),r);
        auto command=g.pointer(e(7,PointerPhase::Up,480.f/2340,275.f/1080),r);
        assert(command && command->target.id==r.rows[0].id);
        r.rows[0].bounds={.107958,.197327,.371361,.338785};
        r.rows[1].bounds={.137537,.286111,.371361,.343472};
        g.pointer(e(7,PointerPhase::Down,425.f/2340,332.f/1080),r);
        command=g.pointer(e(7,PointerPhase::Up,425.f/2340,332.f/1080),r);
        assert(command && command->target.id==r.rows[1].id);
    }
    {
        auto r = registry(); WidgetGestures g; g.begin(5,9,true,false,r);
        r.rows[0].bounds.bottom=.52; // Highlight artwork overlaps the next row.
        g.pointer(e(7,PointerPhase::Down,.4f,.475f),r);
        auto command=g.pointer(e(7,PointerPhase::Up,.4f,.475f),r);
        assert(command && command->target.id==r.rows[1].id);
    }
    {
        auto r=audioRegistry();WidgetGestures g;g.begin(5,9,true,false,r);
        for(int row=0;row<3;++row) {
            const auto& target=r.rows[static_cast<std::size_t>(row)];
            const auto x=static_cast<float>(target.sliderEnd.x), y=static_cast<float>(target.sliderEnd.y);
            assert(!g.pointer(e(7,PointerPhase::Down,x,y),r));
            assert(g.target() && g.target()->id==target.id);
            const auto command=g.pointer(e(7,PointerPhase::Move,.15f,y-.04f),r);
            assert(command && command->kind==WidgetCommandKind::Slider && command->target.id==target.id && command->sliderFraction==0);
            assert(!g.pointer(e(7,PointerPhase::Up,.15f,y-.04f),r));
        }
        // The adjacent Effects caption selects Effects on its first tap,
        // despite the selected Music artwork and its full-height sliderBounds.
        g.pointer(e(7,PointerPhase::Down,.23f,.248f),r);
        const auto caption=g.pointer(e(7,PointerPhase::Up,.23f,.248f),r);
        assert(caption && caption->kind==WidgetCommandKind::Click && caption->target.id==r.rows[1].id);
        // A close but vertically dominant gesture still scrolls; never adjusts
        // Music just because its native axis owned the initial DOWN.
        g.pointer(e(7,PointerPhase::Down,896.f/2424,232.f/1080),r);
        const auto scroll=g.pointer(e(7,PointerPhase::Move,896.f/2424,.07f),r);
        assert(scroll && scroll->kind==WidgetCommandKind::Scroll && scroll->scrollPosition==2);
        assert(!g.pointer(e(7,PointerPhase::Cancel,896.f/2424,.07f),r));
        for(int cancellation=0;cancellation<3;++cancellation) {
            g.pointer(e(7,PointerPhase::Down,896.f/2424,232.f/1080),r);
            if(cancellation==0) g.cancel();
            if(cancellation==1) g.pointer(e(8,PointerPhase::Down,896.f/2424,317.f/1080),r);
            if(cancellation==2) ++r.rows[0].slider.data;
            assert(!g.pointer(e(7,PointerPhase::Move,.15f,.17f),r));
            assert(!g.pointer(e(7,PointerPhase::Up,.15f,.17f),r));
            if(cancellation==1) assert(!g.pointer(e(8,PointerPhase::Up,896.f/2424,317.f/1080),r));
        }
        // Native axes cannot steal a tap outside the admitted list, and exact
        // overlapping axes stay ambiguous rather than choosing by row order.
        r.listBounds.right=.35;
        g.pointer(e(7,PointerPhase::Down,896.f/2424,232.f/1080),r);
        assert(!g.pointer(e(7,PointerPhase::Up,896.f/2424,232.f/1080),r));
        r=audioRegistry();g.begin(5,9,true,false,r);
        r.rows[1].sliderStart=r.rows[0].sliderStart;r.rows[1].sliderEnd=r.rows[0].sliderEnd;
        g.pointer(e(7,PointerPhase::Down,896.f/2424,232.f/1080),r);
        assert(!g.pointer(e(7,PointerPhase::Up,896.f/2424,232.f/1080),r));
    }
    {
        auto r = registry(); WidgetGestures g; g.begin(5,9,true,false,r);
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        assert(!g.pointer(e(7,PointerPhase::Move,std::numeric_limits<float>::quiet_NaN(),.25f),r));
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
        r.rows[1].bounds = r.rows[0].bounds;
        g.pointer(e(7,PointerPhase::Down,.4f,.25f),r);
        assert(!g.pointer(e(7,PointerPhase::Up,.4f,.25f),r));
    }
    std::cout << "PASS: native widget coordinates, captured taps, scroll, slider projection and cancel\n";
}
