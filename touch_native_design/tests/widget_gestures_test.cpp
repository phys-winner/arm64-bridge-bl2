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
}
int main() {
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
