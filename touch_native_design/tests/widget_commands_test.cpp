#include "gfx_widget_fixture.h"
#include <iostream>
using namespace widget_fixture;

int main(int argc, char** argv) {
    assert(argc == 2);
    {
        Vm vm(argv[1]);
        GfxSession session(vm,base,movie,scratch1,768);
        const auto widgets = CaptureBl2Widgets(session,vm.model,2560,1440);
        if (!widgets.valid) std::cerr << widgets.reason << " calls=" << session.calls() << '\n';
        assert(widgets.valid && widgets.rows.size() == 2);
        assert(widgets.rows[0].caption == "Menu" && widgets.rows[1].kind == WidgetKind::Slider);
        assert(std::abs(widgets.rows[1].sliderStart.x - 510.0 / 1280) < 1e-8);
        assert(std::abs(widgets.rows[1].sliderEnd.x - 990.0 / 1280) < 1e-8);
        assert(widgets.rows[1].eventId == 107 && widgets.rows[1].value == 5);
        session.close(); assert(vm.refs.empty() && vm.movieRefs == 1 && session.cleanupOkay());
    }
    for (int fault = 0; fault < 4; ++fault) {
        Vm vm(argv[1]);
        if (fault == 0) vm.prop(vm.root,"_rotation",num(15));
        if (fault == 1) vm.prop(vm.list,"dataProvider",vm.make(9));
        if (fault == 2) vm.prop(vm.object(vm.renderers).array[0],"data",vm.sliderData);
        if (fault == 3) vm.prop(vm.slider,"maximum",num(11));
        GfxSession s(vm,base,movie,scratch1,768);
        assert(!CaptureBl2Widgets(s,vm.model,2560,1440).valid);
    }
    for (const bool transition : {false,true}) {
        Vm vm(argv[1]); WidgetRegistry widgets;
        { GfxSession capture(vm,base,movie,scratch1,768); widgets = CaptureBl2Widgets(capture,vm.model,2560,1440); }
        GfxSession hold(vm,base,movie,scratch1,128);
        WidgetBinding binding(hold,vm.model,widgets,&widgets.rows[0]); assert(binding.valid());
        const auto retainedRefs = vm.refs.size(); assert(retainedRefs >= 4);
        vm.changeInClick = transition;
        { GfxSession operation(vm,base,movie,scratch2,256,true);
          WidgetCommand command; command.kind = WidgetCommandKind::Click; command.route = widgets.route; command.target = widgets.rows[0];
          auto result = ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; });
          assert(result.state == (transition ? WidgetDeliveryState::SceneChanged : WidgetDeliveryState::Dispatched));
          assert(vm.clicks == 1); }
        assert(vm.refs.size() == retainedRefs); // Old objects remain pinned between input frames.
    }
    {
        Vm vm(argv[1]); WidgetRegistry widgets;
        { GfxSession capture(vm,base,movie,scratch1,768); widgets = CaptureBl2Widgets(capture,vm.model,2560,1440); }
        GfxSession hold(vm,base,movie,scratch1,128);
        WidgetBinding binding(hold,vm.model,widgets,&widgets.rows[1]); assert(binding.valid());
        WidgetCommand command; command.kind = WidgetCommandKind::Slider; command.route = widgets.route;
        command.target = widgets.rows[1]; command.sliderFraction = .74;
        for (int i = 0; i < 2; ++i) {
            GfxSession operation(vm,base,movie,scratch2,256,true);
            const auto result = ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; });
            assert(result.state == (i == 0 ? WidgetDeliveryState::Dispatched : WidgetDeliveryState::Unchanged));
            assert(result.actualValue == 7 && vm.sliderNotifications == 1);
            assert(number(vm.object(vm.sliderData).properties.at("currentValue")) == 7);
        }
        { GfxSession operation(vm,base,movie,scratch2,256,true);
          const auto result = ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return false; });
          assert(result.state == WidgetDeliveryState::Rejected && vm.sliderNotifications == 1); }
        ++widgets.rows[1].data.data;
        { GfxSession operation(vm,base,movie,scratch2,256,true);
          assert(ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; }).state == WidgetDeliveryState::Rejected); }
    }
    {
        Vm vm(argv[1]); WidgetRegistry widgets;
        { GfxSession capture(vm,base,movie,scratch1,768); widgets = CaptureBl2Widgets(capture,vm.model,2560,1440); }
        GfxSession hold(vm,base,movie,scratch1,128);
        WidgetBinding binding(hold,vm.model,widgets,nullptr); assert(binding.valid());
        GfxSession operation(vm,base,movie,scratch2,256,true);
        WidgetCommand command; command.kind = WidgetCommandKind::Scroll; command.route = widgets.route; command.scrollPosition = 1;
        const auto result = ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; });
        assert(result.state == WidgetDeliveryState::Dispatched && vm.scrolls == 1 && vm.clicks == 0);
    }
    std::cout << "PASS: live widget registry, retained identities, click/scroll/slider semantics and cleanup\n";
}
