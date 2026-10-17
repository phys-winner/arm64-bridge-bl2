#include "gfx_widget_fixture.h"
#include "bl2_touch_settings.h"
#include <iostream>
#include <limits>
using namespace widget_fixture;

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(Bl2InterfaceScale() == 1.25);
    touchSettingsState.initialized = true; touchSettingsState.values[17] = 150;
    {
        // TitleMenusClik CFX F2FD08B7F660226BC5C765C928082E8BB9A836B5A01B5535BBB5062FA2F717F1:
        // ScrollingList sprite191 __get__/__set__rowHeight and draw body128..218
        // use automatic renderer _height when _rowHeight is undefined; sprite196
        // setText/initSlider use inner.textField/currentValue TextFields.
        Vm vm(argv[1]); touchPage(vm);
        vm.prop(vm.list,"__height",num(550));
        std::vector<Raw> captions, values;
        const auto textField = [&](const Raw& field, double width, double glyphWidth) {
            vm.prop(field,"_fontSize",num(16)); vm.prop(field,"_defaultFontSize",num(16));
            vm.prop(field,"_measuredTextWidth",num(glyphWidth));
            vm.prop(field,"_measuredTextHeight",num(18));
            vm.prop(field,"_width",num(width)); vm.prop(field,"_height",num(40));
        };
        for (const auto& renderer : vm.object(vm.renderers).array) {
            vm.prop(renderer,"_height",num(50));
            const auto inner = vm.object(renderer).properties.at("inner"), caption = vm.make(10);
            vm.visible(caption,inner); vm.prop(inner,"textField",caption);
            vm.prop(caption,"_x",num(0)); vm.prop(caption,"_y",num(0));
            const auto value = vm.object(inner).properties.at("currentValue");
            textField(caption,400,200); textField(value,100,48);
            captions.push_back(caption); values.push_back(value);
        }
        textField(captions[1],120,110); // A long localized label must fit its original column.
        // Authored TextField boxes can span the value column. The glyphs and
        // original spinner arrow determine the true free caption width.
        textField(captions[4],1000,230);
        vm.prop(values[4],"_x",num(300)); vm.prop(values[4],"_textAlign",vm.str("right"));
        const auto spinnerInner = vm.object(vm.object(vm.renderers).array[4]).properties.at("inner");
        const auto arrow = vm.make(10); vm.visible(arrow,spinnerInner);
        vm.prop(spinnerInner,"spinnerLeftArrow",arrow); vm.prop(spinnerInner,"initialSpinnerLeftArrowX",num(380));
        vm.prop(arrow,"_x",num(380)); vm.object(arrow).bounds = {380,0,390,10};
        // A real slider track below18px glyphs must move below27px glyphs,
        // while staying within the enlarged75px row.
        vm.prop(vm.slider,"_y",num(22));
        const auto track = vm.object(vm.slider).properties.at("track"); vm.object(track).bounds = {10,0,490,4};
        const int settingIds[]{1,2,3,4,5,6,7,9,10,11,12};
        for (unsigned i = 0; i < 11; ++i) vm.model.eventIds[i] = kTouchSettingsValue + settingIds[i];
        const auto currentEvents = vm.model.eventIds;
        vm.model.eventIds.resize(4); // Native Options had4rows; our committed provider has18.
        assert(!vm.object(vm.list).properties.count("_rowHeight"));
        const auto root = vm.object(vm.root).properties;
        const auto data = vm.object(vm.data).array;
        GfxIdentity identity;
        const auto present = [&](bool formatValues = false) {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), provider = s.member(list,"data");
            assert(s.identity(provider,identity));
            s.release(provider);
            // Production SettingsUi retains13 values across frames, including
            // this list. Exercise that exact32-slot pool pressure, not an empty
            // helper session; persistent pin setup isn't a per-frame call cost.
            std::vector<GfxHandle> persistent;
            for (unsigned i = 0; i < 12; ++i) persistent.push_back(s.member(list,"_root"));
            s.beginInputBoundary();
            if (formatValues) FormatBl2TouchSettingsList(s,list,identity,currentEvents);
            const auto typography = ScaleBl2WidgetTypography(s,list,identity,vm.model,currentEvents);
            assert(s.good() && s.calls() < 1024);
            for (const auto value : persistent) s.release(value);
            s.release(list); s.close(); assert(s.cleanupOkay() && vm.refs.empty());
            return typography;
        };
        auto result = present();
        assert(result.originalRowHeight == 50 && result.rowHeight == 75 && result.fields == 0);
        assert(vm.rowHeightChanges == 1 && vm.layouts == 1 && vm.object(vm.renderers).array.size() == 7);
        result = present();
        assert(result.renderers == 7 && result.fields > 0 && result.fields <= 14);
        // Original scale9Grid reads share the same unchanged1024-call budget.
        // A presentation may resume its final rows on the following boundary;
        // every visible glyph must converge, without poisoning the session.
        for (unsigned pass = 0; pass < 3; ++pass) present();
        for (unsigned i = 0; i < 7; ++i) {
            assert(number(vm.object(values[i]).properties.at("_fontSize")) == 24);
            const double labelSize = number(vm.object(captions[i]).properties.at("_fontSize"));
            if (i != 1 && i != 4) assert(labelSize == 24);
            else {
                assert(labelSize >= 16 && labelSize < 24);
                const double right = i == 1 ? 116 : number(vm.object(arrow).properties.at("_x")) - 4;
                assert(number(vm.object(captions[i]).properties.at("_measuredTextWidth")) <= right + .001);
            }
            assert(number(vm.object(values[i]).properties.at("_height")) == 60);
            const auto renderer = vm.object(vm.renderers).array[i], inner = vm.object(renderer).properties.at("inner");
            assert(number(vm.object(renderer).properties.at("_yscale")) == 150);
            assert(std::abs(number(vm.object(inner).properties.at("_yscale")) - 100.0 / 1.5) < 1e-8);
            // Font24 and parent150%/inner66.67% mean1.5x glyphs, not2.25x Y.
            assert(std::abs(number(vm.object(renderer).properties.at("_yscale")) *
                number(vm.object(inner).properties.at("_yscale")) / 10000 - 1) < 1e-8);
        }
        assert(number(vm.object(vm.slider).properties.at("_y")) == 33);
        assert(number(vm.object(vm.slider).properties.at("value")) == 5 &&
            number(vm.object(vm.sliderData).properties.at("currentValue")) == 5);
        // The production presentation does value formatting before typography
        // in the same1024-call boundary; every visible field must still fit.
        assert(present(true).fields > 0);
        const auto formats = vm.textFormats, writes = vm.propertyWrites;
        present(); present();
        assert(vm.textFormats == formats && vm.propertyWrites == writes && vm.rowHeightChanges == 1);
        assert(vm.object(vm.root).properties == root && vm.object(vm.data).array == data &&
            number(vm.object(vm.list).properties.at("selectedIndex")) == 0 &&
            number(vm.object(vm.list).properties.at("__height")) == 550);
        assert(!vm.clicks && !vm.sliderNotifications && !vm.spinnerNotifications && !vm.scrolls);
        // An original delayed init may reset TextFormat while our baseline
        // property survives. Correct it to24, never36, on the next pass.
        textField(values[0],100,48); present();
        assert(number(vm.object(values[0]).properties.at("_fontSize")) == 24 && vm.rowHeightChanges == 1);
        const auto oldProvider = vm.object(vm.list).properties.at("dataProvider");
        vm.prop(vm.list,"dataProvider",vm.make(9));
        const auto savedWrites = vm.propertyWrites, savedFormats = vm.textFormats;
        assert(present().rowHeight == 0 && vm.propertyWrites == savedWrites && vm.textFormats == savedFormats);
        vm.prop(vm.list,"dataProvider",oldProvider);
        vm.prop(vm.list,"externalRenderers",boolean(true));
        assert(present().rowHeight == 0 && vm.propertyWrites == savedWrites && vm.textFormats == savedFormats);
        vm.prop(vm.list,"externalRenderers",boolean(false));
        // A limited input boundary stops before its budget is exhausted; it
        // cannot poison later scrolling with a sticky guest-call-budget error.
        GfxSession limited(vm,base,movie,scratch1,120,true);
        const auto list = limited.pin(wrapper + 0x3C);
        ScaleBl2WidgetTypography(limited,list,identity,vm.model,currentEvents);
        assert(limited.good()); limited.release(list); limited.close(); assert(vm.refs.empty());
        // Live125→200→100 reverses every host-owned font/box/inner/track
        // change. At smaller scales the original list exposes more rows; four
        // bounded presentation passes must visit all of them under13pins.
        for (const auto percent : {125,200,100,150,100,125}) {
            assert(SetBl2TouchSetting(17,percent));
            for (unsigned pass = 0; pass < 4; ++pass) present(true);
            const auto visible = vm.object(vm.renderers).array.size();
            const double scale = percent / 100.0;
            assert(std::abs(number(vm.object(vm.list).properties.at("_rowHeight")) - 50 * scale) < .001);
            for (unsigned i = 0; i < visible; ++i) {
                assert(std::abs(number(vm.object(values[i]).properties.at("_fontSize")) - 16 * scale) < .001);
                assert(std::abs(number(vm.object(values[i]).properties.at("_height")) - 40 * scale) < .001);
                const auto renderer = vm.object(vm.renderers).array[i], inner = vm.object(renderer).properties.at("inner");
                assert(std::abs(number(vm.object(renderer).properties.at("_yscale")) *
                    number(vm.object(inner).properties.at("_yscale")) / 10000 - 1) < .001);
                if (percent == 100) assert(std::abs(number(vm.object(captions[i]).properties.at("_fontSize")) - 16) < .001);
            }
            if (percent == 100) assert(number(vm.object(vm.slider).properties.at("_y")) == 22);
            assert(vm.object(vm.data).array == data && !vm.sliderNotifications && !vm.spinnerNotifications && !vm.clicks);
        }
        assert(SetBl2TouchSetting(17,150));
    }
    {
        // Actual Options renderer38 / TitleMenusClik renderer64 grid and the
        // selected raw Shape2 circle. It has no separately scalable MovieClip.
        // The left slice preserves most of X while the middle slice stretches
        // Y. Removing the original grid is necessary before affine X/Y works.
        Vm vm(argv[1]);touchPage(vm);
        vm.object(vm.data).array.resize(1);vm.object(vm.renderers).array.resize(1);
        vm.model.eventIds.resize(1);vm.prop(vm.list,"totalRenderers",num(1));vm.prop(vm.list,"__height",num(500));
        const auto renderer=vm.object(vm.renderers).array[0], inner=vm.object(renderer).properties.at("inner");
        vm.prop(renderer,"_height",num(50));
        const auto grid=vm.make(8);
        vm.prop(grid,"x",num(-9));vm.prop(grid,"y",num(-12));vm.prop(grid,"width",num(190));vm.prop(grid,"height",num(74));
        vm.prop(renderer,"scale9Grid",grid);
        const auto caption=vm.make(10);vm.visible(caption,inner);vm.prop(inner,"textField",caption);
        const auto value=vm.object(inner).properties.at("currentValue");
        for (const auto field:{caption,value}) {
            vm.prop(field,"_fontSize",num(16));vm.prop(field,"_width",num(400));vm.prop(field,"_height",num(40));
            vm.prop(field,"_x",num(0));vm.prop(field,"_y",num(0));
            vm.prop(field,"_measuredTextWidth",num(100));vm.prop(field,"_measuredTextHeight",num(18));
        }
        const auto present=[&] {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list=s.pin(wrapper+0x3C), data=s.member(list,"data");GfxIdentity identity;
            assert(s.identity(data,identity));s.release(data);
            std::vector<GfxHandle> pins;for(unsigned i=0;i<12;++i)pins.push_back(s.member(list,"_root"));
            s.beginInputBoundary();ScaleBl2WidgetTypography(s,list,identity,vm.model,vm.model.eventIds);
            assert(s.good() && s.calls()<1024);
            for(const auto p:pins)s.release(p);
            s.release(list);s.close();assert(s.cleanupOkay() && vm.refs.empty());
        };
        const auto savedGrid=vm.object(grid).properties;
        for(const auto percent:{125,200,100,200,125,100}) {
            assert(SetBl2TouchSetting(17,percent));for(unsigned i=0;i<3;++i)present();
            const auto& props=vm.object(renderer).properties;
            assert(props.at("_bl2TouchScale9Grid")==grid && vm.object(grid).properties==savedGrid);
            if(percent==100) assert(props.at("scale9Grid")==grid);
            else {
                assert(props.at("scale9Grid")[3]==0);
                const auto sx=number(props.at("_xscale")),sy=number(props.at("_yscale"));
                // Full authored contour bounds X[-36.1,-5.6],Y[3.5,33.9].
                // Without nine-slice, the physical footprint must scale BOTH.
                assert(std::abs((30.5*sx/100)/(30.4*sy/100)-30.5/30.4)<1e-8);
                assert(std::abs(sx/100-percent/100.0)<1e-8);
                assert(std::abs(sx*number(vm.object(inner).properties.at("_xscale"))/10000-1)<1e-8);
            }
            assert(!vm.clicks && !vm.sliderNotifications && !vm.spinnerNotifications);
        }
        // A foreign grid must not be erased or replaced with this asset's grid.
        const auto foreign=vm.make(8);vm.object(foreign).properties=savedGrid;vm.prop(foreign,"width",num(240));
        vm.prop(renderer,"scale9Grid",foreign);assert(SetBl2TouchSetting(17,200));present();present();
        assert(vm.object(renderer).properties.at("scale9Grid")==foreign && vm.object(grid).properties==savedGrid);
        assert(SetBl2TouchSetting(17,150));
    }
    {
        // Original Options inner.default hides both native controls. Its
        // selected Button seek updates caption only; original setData restores
        // the owned slider frame after200->100, without activating a setting.
        Vm vm(argv[1]);touchPage(vm);
        vm.object(vm.data).array.resize(1);vm.object(vm.renderers).array.resize(1);vm.model.eventIds.resize(1);
        vm.prop(vm.list,"totalRenderers",num(1));vm.prop(vm.list,"__height",num(500));
        const auto renderer=vm.object(vm.renderers).array[0],inner=vm.object(renderer).properties.at("inner");
        const auto slider=vm.object(inner).properties.at("slider"),value=vm.object(inner).properties.at("currentValue");
        vm.prop(renderer,"_height",num(50));vm.prop(renderer,"state",vm.str("selected_over"));
        vm.prop(inner,"_currentframe",num(3));vm.resetInnerOnState=true;
        const auto caption=vm.make(10);vm.visible(caption,inner);vm.prop(inner,"textField",caption);
        for(const auto field:{caption,value}) {
            vm.prop(field,"_fontSize",num(16));vm.prop(field,"_height",num(40));vm.prop(field,"_width",num(400));
            vm.prop(field,"_x",num(0));vm.prop(field,"_y",num(0));
            vm.prop(field,"_measuredTextWidth",num(100));vm.prop(field,"_measuredTextHeight",num(18));
        }
        const auto listeners=vm.make(8),sliderFn=vm.make(8),spinnerFn=vm.make(8),prototype=vm.make(8);
        vm.prop(vm.list,"_listeners",listeners);vm.prop(vm.list,"onSliderValueChange",sliderFn);
        vm.prop(vm.list,"onSpinnerValueChange",spinnerFn);vm.prop(vm.list,"__proto__",prototype);
        vm.prop(prototype,"onSliderValueChange",vm.make(8));vm.prop(prototype,"onSpinnerValueChange",vm.make(8));
        const auto present=[&](bool allow=true) {
            GfxSession s(vm,base,movie,scratch1,1024,true);const auto list=s.pin(wrapper+0x3C);
            const auto data=s.member(list,"data");GfxIdentity identity;assert(s.identity(data,identity));s.release(data);
            std::vector<GfxHandle> pins;for(unsigned i=0;i<12;++i)pins.push_back(s.member(list,"_root"));
            s.beginInputBoundary();ScaleBl2WidgetTypography(s,list,identity,vm.model,vm.model.eventIds,allow);
            assert(s.good() && s.calls()<1024);
            for(const auto p:pins)s.release(p);s.release(list);s.close();assert(s.cleanupOkay() && vm.refs.empty());
        };
        const auto items=vm.object(vm.data).array;
        for(const auto percent:{200,100,200,100}) {
            assert(SetBl2TouchSetting(17,percent));for(unsigned pass=0;pass<4;++pass)present();
            assert(number(vm.object(inner).properties.at("_currentframe"))==3);
            assert(vm.object(value).properties.at("_visible")[4]==1 && vm.object(slider).properties.at("_visible")[4]==1);
            assert(vm.object(vm.list).properties.at("_listeners")==listeners);
            assert(vm.object(vm.list).properties.at("onSliderValueChange")==sliderFn &&
                vm.object(vm.list).properties.at("onSpinnerValueChange")==spinnerFn);
            assert(vm.object(vm.data).array==items && !vm.clicks && !vm.sliderNotifications && !vm.spinnerNotifications);
        }
        assert(vm.rendererRebinds==4);
        vm.prop(inner,"_currentframe",num(1));vm.prop(value,"_visible",boolean(false));vm.prop(slider,"_visible",boolean(false));
        const auto rebinds=vm.rendererRebinds;present(false);assert(vm.rendererRebinds==rebinds);
        present();assert(vm.rendererRebinds==rebinds+1); // Repair waits until captured drag has released.
        assert(SetBl2TouchSetting(17,150));
    }
    {
        // Exact public ScrollingList scrollToIndex (sprite191 body0..159)
        // restores selected row6 after totalRenderers shrinks7→5. Same-index
        // __set__selectedIndex cannot do this: its original body exits at21.
        Vm vm(argv[1]); touchPage(vm);
        vm.object(vm.data).array.resize(7); vm.object(vm.renderers).array.resize(7);
        vm.model.eventIds.resize(5); vm.prop(vm.list,"totalRenderers",num(7));
        vm.prop(vm.list,"__height",num(500)); vm.prop(vm.list,"_rowHeight",num(65));
        vm.prop(vm.list,"_bl2TouchRowHeight",num(50)); vm.prop(vm.list,"selectedIndex",num(6));
        std::vector<std::int32_t> events{6,1,2,3,5,kTouchSettingsOpen,kTouchSettingsValue + 17};
        const auto data = vm.object(vm.data).array;
        for (unsigned i = 0; i < 7; ++i) {
            const auto renderer = vm.object(vm.renderers).array[i], inner = vm.object(renderer).properties.at("inner");
            vm.prop(renderer,"_height",num(65)); vm.prop(renderer,"_yscale",num(130));
            vm.prop(renderer,"_bl2TouchRendererScaleY",num(100));
            vm.prop(inner,"_yscale",num(100.0 / 1.3)); vm.prop(inner,"_bl2TouchInnerScaleY",num(100));
            const auto caption = vm.make(10); vm.visible(caption,inner); vm.prop(inner,"textField",caption);
            for (const auto field : {caption,vm.object(inner).properties.at("currentValue")}) {
                vm.prop(field,"_fontSize",num(20.8)); vm.prop(field,"_bl2TouchFontSize",num(16));
                vm.prop(field,"_width",num(400)); vm.prop(field,"_height",num(52));
                vm.prop(field,"_bl2TouchTextHeight",num(40)); vm.prop(field,"_measuredTextWidth",num(100 * 1.3));
                vm.prop(field,"_measuredTextHeight",num(18 * 1.3));
            }
            // Button.setState's native same-state gotoAndPlay refreshes the
            // authored selection art instead of dispatching handleClick.
            vm.prop(renderer,"state",vm.str("over"));
            const auto art = vm.make(10); vm.visible(art,renderer); vm.prop(renderer,"selectionArt",art);
            vm.prop(art,"authoredScaleY",num(100)); vm.prop(art,"_yscale",num(i == 6 ? 8 : 100));
        }
        const auto present = [&] {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), provider = s.member(list,"data"); GfxIdentity identity;
            assert(s.identity(provider,identity)); s.release(provider);
            std::vector<GfxHandle> persistent;
            for (unsigned i = 0; i < 12; ++i) persistent.push_back(s.member(list,"_root"));
            s.beginInputBoundary(); ScaleBl2WidgetTypography(s,list,identity,vm.model,events);
            assert(s.good() && s.calls() < 1024);
            for (const auto value : persistent) s.release(value);
            s.release(list); s.close(); assert(s.cleanupOkay() && vm.refs.empty());
        };
        for (const auto percent : {100,200,125,200,100}) {
            assert(SetBl2TouchSetting(17,percent));
            const auto refreshes = vm.stateRefreshes;
            for (unsigned pass = 0; pass < 4; ++pass) present();
            const double top = number(vm.object(vm.list).properties.at("scrollPosition"));
            const double capacity = number(vm.object(vm.list).properties.at("totalRenderers"));
            assert(top <= 6 && 6 < top + capacity && number(vm.object(vm.list).properties.at("selectedIndex")) == 6);
            const auto selectedRenderer = vm.object(vm.renderers).array[static_cast<std::size_t>(6 - top)];
            const auto art = vm.object(selectedRenderer).properties.at("selectionArt");
            assert(vm.stateRefreshes == refreshes + 1 && number(vm.object(art).properties.at("_yscale")) == 100);
            assert(vm.object(vm.data).array == data && !vm.clicks && !vm.selections && !vm.sliderNotifications && !vm.spinnerNotifications);
        }
        assert(SetBl2TouchSetting(17,150));
    }
    for (const auto& page : std::vector<std::pair<const char*,unsigned>>{{"FrontendGFxMovie",9},{"PauseGFxMovie",5},{"PauseGFxMovie",4}}) {
        // These pages deliberately have no owned Settings events. The captured
        // native Main has9rows, but selected animation bounds are101.85px vs
        // actual41.3px pitch. Preserve every row, native sizing and animation.
        Vm vm(argv[1]); vm.model.movieClass = page.first;
        const unsigned rows = page.second;
        vm.object(vm.data).array.clear(); vm.object(vm.renderers).array.clear(); vm.model.eventIds.clear();
        vm.prop(vm.list,"__height",num(rows * 41.3)); vm.prop(vm.list,"totalRenderers",num(rows));
        std::vector<Raw> captions;
        for (unsigned i = 0; i < rows; ++i) {
            const auto item = vm.make(8), renderer = vm.make(10), inner = vm.make(10), caption = vm.make(10);
            vm.prop(item,"rendererType",vm.str("default")); vm.prop(item,"label",vm.str("Main entry"));
            vm.object(vm.data).array.push_back(item); vm.model.eventIds.push_back(static_cast<int>(i));
            vm.visible(renderer,vm.list); vm.visible(inner,renderer); vm.visible(caption,inner);
            vm.prop(renderer,"owner",vm.list); vm.prop(renderer,"data",item); vm.prop(renderer,"index",num(i));
            vm.prop(renderer,"_height",num(i == 0 ? 101.85 : 41.3)); vm.prop(renderer,"_y",num(i * 41.3));
            vm.object(vm.renderers).array.push_back(renderer);
            vm.prop(renderer,"inner",inner); vm.prop(inner,"textField",caption);
            vm.prop(caption,"_x",num(0)); vm.prop(caption,"_y",num(0));
            vm.prop(caption,"_width",num(400)); vm.prop(caption,"_height",num(40));
            vm.prop(caption,"_fontSize",num(16)); vm.prop(caption,"_measuredTextWidth",num(160));
            vm.prop(caption,"_measuredTextHeight",num(18)); captions.push_back(caption);
            // A default-button frame retains this authored object but never
            // displays a numeric setting. It must not constrain the caption.
            const auto hiddenValue = vm.make(10); vm.visible(hiddenValue,inner);
            vm.prop(inner,"currentValue",hiddenValue); vm.prop(hiddenValue,"_visible",boolean(false));
            vm.prop(hiddenValue,"_fontSize",num(16)); vm.prop(hiddenValue,"_x",num(1));
        }
        const auto savedData = vm.object(vm.data).array;
        // Native draw creates the viewport's renderer capacity even when the
        // provider is shorter. Those last two blank rows are not data entries.
        for (unsigned i = rows; i < rows + 2; ++i) {
            const auto renderer = vm.make(10); vm.visible(renderer,vm.list);
            vm.prop(renderer,"owner",vm.list); vm.prop(renderer,"index",num(i));
            vm.prop(renderer,"data",Raw{}); vm.prop(renderer,"_y",num(i * 41.3));
            vm.object(vm.renderers).array.push_back(renderer);
        }
        vm.prop(vm.list,"totalRenderers",num(rows + 2));
        const auto viewportHeight = vm.object(vm.list).properties.at("__height");
        for (unsigned i = 0; i < 3; ++i) {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), provider = s.member(list,"data"); GfxIdentity identity;
            assert(s.identity(provider,identity));
            s.release(provider);
            std::vector<GfxHandle> persistent;
            for (unsigned j = 0; j < 12; ++j) persistent.push_back(s.member(list,"_root"));
            s.beginInputBoundary();
            const auto typography = ScaleBl2WidgetTypography(s,list,identity,vm.model,{});
            assert(s.good() && std::abs(typography.rowHeight - 41.3) < .001 && typography.fields == rows);
            for (const auto value : persistent) s.release(value);
            s.release(list); s.close(); assert(s.cleanupOkay() && vm.refs.empty());
        }
        for (const auto& caption : captions) assert(number(vm.object(caption).properties.at("_fontSize")) == 24);
        assert(!vm.rowHeightChanges && !vm.layouts && !vm.object(vm.list).properties.count("_rowHeight") &&
            vm.object(vm.list).properties.at("__height") == viewportHeight && vm.object(vm.data).array == savedData &&
            vm.object(vm.renderers).array.size() == rows + 2 && !vm.clicks && !vm.selections);
        for (unsigned i = 0; i < rows; ++i) {
            const auto renderer = vm.object(vm.renderers).array[i], inner = vm.object(renderer).properties.at("inner");
            assert(number(vm.object(renderer).properties.at("_height")) == (i == 0 ? 101.85 : 41.3));
            assert(number(vm.object(renderer).properties.at("_y")) == i * 41.3 &&
                number(vm.object(renderer).properties.at("_yscale")) == 100 && number(vm.object(inner).properties.at("_yscale")) == 100);
            const auto hiddenValue = vm.object(inner).properties.at("currentValue");
            assert(number(vm.object(hiddenValue).properties.at("_fontSize")) == 16);
            assert(number(vm.object(captions[i]).properties.at("_measuredTextHeight")) + 4 <= 41.3);
        }
        for (const auto percent : {125,200,100,150}) {
            assert(SetBl2TouchSetting(17,percent));
            for (unsigned pass = 0; pass < 4; ++pass) {
                GfxSession s(vm,base,movie,scratch1,1024,true);
                const auto list = s.pin(wrapper + 0x3C), provider = s.member(list,"data"); GfxIdentity identity;
                assert(s.identity(provider,identity)); s.release(provider);
                std::vector<GfxHandle> persistent;
                for (unsigned i = 0; i < 12; ++i) persistent.push_back(s.member(list,"_root"));
                s.beginInputBoundary(); ScaleBl2WidgetTypography(s,list,identity,vm.model,{});
                assert(s.good() && s.calls() < 1024);
                for (const auto value : persistent) s.release(value);
                s.release(list); s.close(); assert(s.cleanupOkay() && vm.refs.empty());
            }
            for (const auto& caption : captions) {
                const auto& props = vm.object(caption).properties;
                const double size = number(props.at("_fontSize"));
                if (percent <= 150) assert(std::abs(size - 16 * percent / 100.0) < .001);
                else assert(size > 24 && size <= 32 && number(props.at("_measuredTextHeight")) + 4 <= 41.3);
            }
            assert(!vm.rowHeightChanges && !vm.layouts && !vm.object(vm.list).properties.count("_rowHeight") &&
                vm.object(vm.data).array == savedData && vm.object(vm.renderers).array.size() == rows + 2);
        }
    }
    for (const bool customPage : {false,true}) {
        // A poisoned persistent session cannot restore its AS data. Reattach
        // must use the exact native owner's proven baseline, including when a
        // completely different Touch page remained attached. Prefix truncation
        // would recover the wrong rows and lose the original callbacks here.
        static_assert(GfxSession::kSlots == 32);
        Vm vm(argv[1]); vm.model.movieClass = "OptionsGFxMovie";
        const auto header = vm.make(10), description = vm.make(10);
        const auto listeners = vm.make(8), sliderHandler = vm.make(8), spinnerHandler = vm.make(8);
        vm.prop(vm.list,"headerText",header); vm.prop(vm.list,"description",description);
        vm.prop(header,"htmlText",vm.str("Original options"));
        vm.prop(description,"htmlText",vm.str("Original hint"));
        vm.prop(vm.list,"_listeners",listeners); vm.prop(vm.list,"onSliderValueChange",sliderHandler);
        vm.prop(vm.list,"onSpinnerValueChange",spinnerHandler);
        const auto nativeData = vm.object(vm.data).array;
        const auto synthetic = vm.make(8); vm.prop(synthetic,"label",vm.str("Touch controls"));
        vm.prop(synthetic,"rendererType",vm.str("default")); vm.prop(synthetic,"unselectable",boolean(false));
        vm.prop(vm.list,"synthetic",synthetic);
        const auto interfaceRow = vm.make(8); vm.prop(interfaceRow,"label",vm.str("Interface scale (%)"));
        vm.prop(interfaceRow,"rendererType",vm.str("slider")); vm.prop(interfaceRow,"unselectable",boolean(false));
        vm.prop(vm.list,"interface",interfaceRow);
        const auto append = [&](GfxSession& s, GfxHandle list, GfxHandle baseline) {
            const auto original = s.member(baseline,"data"), data = s.invoke(original,"slice");
            const auto row = s.member(list,"synthetic"), pushed = s.invoke(data,"push",{row},true);
            const auto ui = s.member(list,"interface"), pushedUi = s.invoke(data,"push",{ui},true);
            assert(pushed && pushedUi && s.setMember(list,"data",data));
            const auto committed = s.invoke(list,"CommitListItems",{},true); assert(committed && s.good());
            s.release(committed); s.release(pushedUi); s.release(ui); s.release(pushed); s.release(row); s.release(data); s.release(original);
        };
        {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), h = s.member(list,"headerText"), d = s.member(list,"description");
            const auto saved = CaptureBl2OptionsOriginal(s,list,h,d,vm.model); assert(saved && s.good());
            append(s,list,saved);
            if (customPage) {
                const auto replacement = vm.make(9);
                for (unsigned i = 0; i < 18; ++i) vm.object(replacement).array.push_back(vm.make(8));
                vm.prop(vm.list,"data",replacement); vm.prop(vm.list,"dataProvider",replacement);
                vm.prop(header,"htmlText",vm.str("Touch page"));
                vm.prop(description,"htmlText",vm.str("Touch hint"));
                vm.prop(vm.list,"_listeners",vm.make(8)); vm.prop(vm.list,"onSliderValueChange",vm.make(8));
                vm.prop(vm.list,"onSpinnerValueChange",vm.make(8));
            }
            std::vector<GfxHandle> exhausted;
            while (s.good()) {
                const auto value = s.undefined(); if (value) exhausted.push_back(value);
            }
            assert(std::string(s.reason()) == "value-slot-budget" && exhausted.size() == 28);
            s.close(); assert(s.cleanupOkay() && vm.refs.empty());
        }
        const auto retained = vm.object(vm.list).properties.at("_bl2TouchNativeOriginal");
        assert(vm.object(vm.object(vm.list).properties.at("data")).array.size() == (customPage ? 18 : 6));
        for (unsigned pass = 0; pass < 2; ++pass) {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), h = s.member(list,"headerText"), d = s.member(list,"description");
            const auto saved = CaptureBl2OptionsOriginal(s,list,h,d,vm.model); assert(saved && s.good());
            const auto restore = [&](GfxHandle target, const char* field, const char* name) {
                const auto value = s.member(saved,name); assert(value && s.setMember(target,field,value)); s.release(value);
            };
            restore(h,"htmlText","header"); restore(d,"htmlText","description");
            restore(list,"_listeners","listeners"); restore(list,"onSliderValueChange","slider");
            restore(list,"onSpinnerValueChange","spinner"); append(s,list,saved);
            auto model = vm.model; model.eventIds.push_back(kTouchSettingsOpen); model.eventIds.push_back(kTouchSettingsValue + 17);
            s.beginInputBoundary();
            const auto registry = CaptureBl2Widgets(s,model,2560,1440);
            assert(registry.valid && registry.rowCount == model.eventIds.size());
            s.release(saved); s.release(d); s.release(h); s.release(list); s.close();
            assert(s.cleanupOkay() && vm.refs.empty());
            const auto& actual = vm.object(vm.object(vm.list).properties.at("data")).array;
            assert(actual.size() == nativeData.size() + 2 && std::equal(nativeData.begin(),nativeData.end(),actual.begin()));
            assert(actual[actual.size() - 2] == synthetic && actual.back() == interfaceRow &&
                std::count(actual.begin(),actual.end(),synthetic) == 1 && std::count(actual.begin(),actual.end(),interfaceRow) == 1);
            assert(vm.object(vm.list).properties.at("_bl2TouchNativeOriginal") == retained &&
                vm.object(vm.list).properties.at("_listeners") == listeners &&
                vm.object(vm.list).properties.at("onSliderValueChange") == sliderHandler &&
                vm.object(vm.list).properties.at("onSpinnerValueChange") == spinnerHandler);
            assert(vm.text(vm.object(header).properties.at("htmlText")[4]) == "Original options" &&
                vm.text(vm.object(description).properties.at("htmlText")[4]) == "Original hint");
        }
        for (unsigned changed = 0; changed < 4; ++changed) {
            auto model = vm.model;
            if (changed == 0) ++model.owner;
            else if (changed == 1) ++model.list;
            else if (changed == 2) ++model.provider;
            else ++model.structure;
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), h = s.member(list,"headerText"), d = s.member(list,"description");
            const auto writes = vm.propertyWrites;
            assert(!CaptureBl2OptionsOriginal(s,list,h,d,model) && s.good() && vm.propertyWrites == writes);
            s.release(d); s.release(h); s.release(list); s.close(); assert(s.cleanupOkay() && vm.refs.empty());
        }
    }
    {
        // Selected Audio content has a real inner Y offset and a thumb taller
        // than the thin track. With a native120% selected-content scale,1.5x
        // font would push the thumb into the following caption's logical row.
        Vm vm(argv[1]); touchPage(vm); vm.prop(vm.list,"__height",num(550));
        for (const auto& renderer : vm.object(vm.renderers).array) {
            vm.prop(renderer,"_height",num(50)); const auto inner = vm.object(renderer).properties.at("inner");
            const auto caption = vm.make(10); vm.visible(caption,inner); vm.prop(inner,"textField",caption);
            for (const auto field : {caption,vm.object(inner).properties.at("currentValue")}) {
                vm.prop(field,"_fontSize",num(16)); vm.prop(field,"_width",num(400)); vm.prop(field,"_height",num(40));
                vm.prop(field,"_measuredTextWidth",num(160)); vm.prop(field,"_measuredTextHeight",num(18));
            }
        }
        vm.prop(vm.inner,"_y",num(8)); vm.prop(vm.inner,"_yscale",num(120)); vm.prop(vm.slider,"_y",num(22));
        const auto track = vm.object(vm.slider).properties.at("track"), thumb = vm.object(vm.slider).properties.at("thumb");
        vm.object(track).bounds = {10,0,490,4}; vm.object(thumb).bounds = {0,-3,15,17};
        const auto savedData = vm.object(vm.data).array;
        const auto present = [&] {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), data = s.member(list,"data"); GfxIdentity identity;
            assert(s.identity(data,identity)); ScaleBl2WidgetTypography(s,list,identity,vm.model,vm.model.eventIds);
            assert(s.good()); s.release(data); s.release(list); s.close(); assert(s.cleanupOkay() && vm.refs.empty());
        };
        present(); present();
        const auto renderer = vm.object(vm.renderers).array.front(), caption = vm.object(vm.inner).properties.at("textField");
        const double font = number(vm.object(caption).properties.at("_fontSize"));
        assert(font > 16 && font < 24);
        const double outerSy = number(vm.object(renderer).properties.at("_yscale")) / 100;
        const double innerSy = number(vm.object(vm.inner).properties.at("_yscale")) / 100;
        const double innerY = number(vm.object(vm.inner).properties.at("_y"));
        const double sliderY = number(vm.object(vm.slider).properties.at("_y"));
        const double glyphBottom = outerSy * (innerY + innerSy *
            (number(vm.object(caption).properties.at("_measuredTextHeight")) + 4));
        const double controlTop = outerSy * (innerY + innerSy * (sliderY - 3));
        const double controlBottom = outerSy * (innerY + innerSy * (sliderY + 17));
        assert(controlTop >= glyphBottom + 2.399 && controlBottom <= 75 - 2.399);
        const auto writes = vm.propertyWrites, formats = vm.textFormats;
        present(); present(); assert(vm.propertyWrites == writes && vm.textFormats == formats);
        assert(vm.object(vm.data).array == savedData && number(vm.object(vm.slider).properties.at("value")) == 5 &&
            number(vm.object(vm.sliderData).properties.at("currentValue")) == 5 && !vm.sliderNotifications && !vm.clicks);
    }
    {
        Vm vm(argv[1]); touchPage(vm); vm.prop(vm.list,"__height",num(550));
        const auto renderer = vm.object(vm.renderers).array.front(); vm.prop(renderer,"_height",num(50));
        vm.object(vm.list).transformBounds = true; vm.changeOnPropertyWrite = true;
        GfxSession s(vm,base,movie,scratch1,1024,true);
        const auto list = s.pin(wrapper + 0x3C), data = s.member(list,"data"); GfxIdentity identity;
        assert(s.identity(data,identity)); ScaleBl2WidgetTypography(s,list,identity,vm.model,vm.model.eventIds);
        assert(!s.good() && vm.propertyWrites == 1 && !vm.rowHeightChanges && !vm.textFormats);
        s.release(data); s.release(list); s.close(); assert(vm.refs.empty() && s.cleanupOkay());
    }
    {
        // Original Startup.xxx TitleMenusClik CFX SHA256:
        // F2FD08B7F660226BC5C765C928082E8BB9A836B5A01B5535BBB5062FA2F717F1.
        // sprite172(_root.menu)=(361.6,179.2); child155(pauseMenuDeco)=
        // (278,181.6), only shape154 [-760,-380,760,380], black alpha128.
        const auto authored = [](Vm& vm) {
            const auto menu = vm.make(10), scrim = vm.make(10), lobby = vm.make(10);
            vm.visible(menu,vm.root); vm.visible(scrim,menu); vm.visible(lobby,menu);
            vm.prop(vm.root,"menu",menu); vm.prop(menu,"pauseMenuDeco",scrim); vm.prop(menu,"lobbyMenuDeco",lobby);
            for (const auto clip : {menu,scrim,lobby}) {
                vm.prop(clip,"_xscale",num(100)); vm.prop(clip,"_yscale",num(100)); vm.prop(clip,"_rotation",num(0));
            }
            vm.prop(menu,"_x",num(361.6)); vm.prop(menu,"_y",num(179.2));
            vm.object(menu).tx = 361.6; vm.object(menu).ty = 179.2;
            vm.prop(scrim,"_x",num(278)); vm.prop(scrim,"_y",num(181.6));
            vm.object(scrim).bounds = {-760,-380,760,380}; vm.object(scrim).transformBounds = true;
            vm.prop(scrim,"authoredFill",num(128));
            return scrim;
        };
        for (const auto stageRect : {std::array<float,4>{-168,0,1448,720},std::array<float,4>{0,0,1280,720},
                                 std::array<float,4>{0,-448,1280,1168}}) {
            Vm vm(argv[1]); const auto scrim = authored(vm);
            vm.visibleFrame = stageRect;
            const auto menu = vm.object(vm.root).properties.at("menu"), lobby = vm.object(menu).properties.at("lobbyMenuDeco");
            const auto savedMenu = vm.object(menu).properties, savedLobby = vm.object(lobby).properties;
            const auto savedList = vm.object(vm.list).properties;
            const auto savedData = vm.object(vm.data).array;
            auto fit = [&] {
                GfxSession s(vm,base,movie,scratch1,256,true);
                const auto list = s.pin(wrapper + 0x3C); GfxRect visible, before, after;
                assert(FitBl2PauseBackdrop(s,list,visible,before,after));
                assert(before.left <= -120.4 + .001 && before.right >= 1399.6 - .001);
                assert(after.left <= stageRect[0] + .1 && after.top <= stageRect[1] + .1 &&
                    after.right >= stageRect[2] - .1 && after.bottom >= stageRect[3] - .1);
                assert(s.good() && s.calls() < 100); s.release(list); s.close(); assert(s.cleanupOkay());
            };
            fit();
            const auto written = vm.propertyWrites;
            const auto savedScrim = vm.object(scrim).properties;
            fit(); // Reopening/polling cannot enlarge the clip again or compound alpha.
            assert(vm.propertyWrites == written && vm.object(scrim).properties == savedScrim);
            assert(vm.object(menu).properties == savedMenu && vm.object(lobby).properties == savedLobby);
            assert(vm.object(vm.list).properties == savedList && vm.object(vm.data).array == savedData);
            assert(number(vm.object(scrim).properties.at("_alpha")) == 100 && number(vm.object(scrim).properties.at("authoredFill")) == 128);
            if (stageRect[0] == -168) {
                assert(std::abs(number(vm.object(scrim).properties.at("_xscale")) - 1616.0 / 1520 * 100) < 1e-8);
                assert(std::abs(number(vm.object(scrim).properties.at("_x")) - 278.4) < 1e-8);
            }
            if (stageRect[0] == 0 && stageRect[1] == 0) assert(!written); // Original16:9 overscan already covers the screen.
            assert(vm.refs.empty() && vm.movieRefs == 1 && !vm.clicks && !vm.sliderNotifications && !vm.scrolls);
        }
        for (int fault = 0; fault < 4; ++fault) {
            Vm vm(argv[1]); const auto scrim = authored(vm); vm.visibleFrame = {-168,0,1448,720};
            if (fault == 0) vm.prop(scrim,"_visible",boolean(false));
            if (fault == 1) vm.prop(scrim,"_parent",vm.root);
            if (fault == 2) vm.object(scrim).bounds.left = -640;
            if (fault == 3) vm.changeOnPropertyWrite = true;
            GfxSession s(vm,base,movie,scratch1,256,true);
            const auto list = s.pin(wrapper + 0x3C); GfxRect visible, before, after;
            assert(!FitBl2PauseBackdrop(s,list,visible,before,after));
            if (fault < 3) assert(!vm.propertyWrites);
            else {
                assert(!s.good()); // Ownership loss after one setter prevents every later write.
                assert(number(vm.object(scrim).properties.at("_x")) == 278);
                assert(number(vm.object(scrim).properties.at("_y")) == 181.6);
                assert(number(vm.object(scrim).properties.at("_yscale")) == 100);
            }
            s.release(list); s.close(); assert(vm.refs.empty() && vm.movieRefs == 1 && s.cleanupOkay());
        }
    }
    {
        Vm vm(argv[1]); touchPage(vm);
        // Real UI_Options.Options initSlider writes data.currentValue.toString()
        // into inner.currentValue.text. No private _rowHeight exists here.
        const std::vector<int> ids{1,2,3,4,5,6,7,9,10,11,12};
        const double values[]{double(.42f),double(.9985f),double(.22239998f),double(.55f),0,0,0,0,0,1,double(.4f)};
        const char* expected[]{"42%","100%","0.22","0.55","Off","Off","Off","Off","Off","1.00","0.40"};
        std::vector<Raw> fields, saved;
        for (unsigned i = 0; i < ids.size(); ++i) {
            vm.model.eventIds[i] = kTouchSettingsValue + ids[i];
            const auto row = vm.object(vm.data).array[i], renderer = vm.object(vm.renderers).array[i];
            const auto inner = vm.object(renderer).properties.at("inner");
            const auto field = vm.object(inner).properties.at("currentValue"); fields.push_back(field);
            vm.prop(row,"currentValue",num(values[i])); saved.push_back(vm.object(row).properties.at("currentValue"));
            vm.prop(field,"text",vm.str(touchSettingSpecs[ids[i]].toggle ? "Off" : "0.420000016689"));
        }
        const auto preferences = touchSettingsState.values;
        auto present = [&] {
            GfxSession s(vm,base,movie,scratch1,1024,true);
            const auto list = s.pin(wrapper + 0x3C), data = s.member(list,"data");
            GfxIdentity identity; assert(s.identity(data,identity));
            FormatBl2TouchSettingsList(s,list,identity,vm.model.eventIds);
            assert(s.good() && s.calls() < 400); s.release(data); s.release(list);
        };
        present();
        for (unsigned i = 0; i < ids.size(); ++i) {
            assert(vm.text(vm.object(fields[i]).properties.at("text")[4]) == expected[i]);
            assert(vm.object(vm.object(vm.data).array[i]).properties.at("currentValue") == saved[i]);
        }
        // An asynchronous renderer init after a first input-boundary pass is
        // corrected by the same presentation operation after GFx advance.
        vm.prop(fields[10],"text",vm.str("0.400000005960")); present();
        assert(vm.text(vm.object(fields[10]).properties.at("text")[4]) == "0.40");
        const auto roundedWrites = vm.propertyWrites;
        present(); present();
        assert(vm.propertyWrites == roundedWrites);
        // Recycled rows can be initialized repeatedly while a scrolling finger
        // remains down. Each new raw value is corrected without changing the
        // data or invoking the native slider/spinner change callbacks.
        for (unsigned pass = 0; pass < 8; ++pass) {
            vm.prop(fields[2],"text",vm.str("0.222399979829788"));
            present();
            assert(vm.text(vm.object(fields[2]).properties.at("text")[4]) == "0.22");
        }
        // Do not paint a renderer while it still belongs to its previous row.
        vm.prop(fields[0],"text",vm.str("pending"));
        vm.prop(vm.object(vm.renderers).array[0],"data",vm.object(vm.data).array[1]); present();
        assert(vm.text(vm.object(fields[0]).properties.at("text")[4]) == "pending");
        vm.prop(vm.list,"dataProvider",vm.make(9));
        vm.prop(fields[10],"text",vm.str("uncommitted")); present();
        assert(vm.text(vm.object(fields[10]).properties.at("text")[4]) == "uncommitted");
        assert(touchSettingsState.values == preferences && !vm.sliderNotifications && !vm.spinnerNotifications);
        assert(vm.refs.empty() && vm.movieRefs == 1);
    }
    // The earlier presentation tests initialized only their scale. Bootstrap
    // protocol tests below deliberately begin from the real empty state.
    touchSettingsState.initialized = false; touchSettingsState.revision = 0;
    touchSettingsState.commands = 0; touchSettingsState.uiRequest = 0; touchSettingsState.values.fill(0);
    {
        assert(!touchSettingsState.initialized && Bl2InterfaceScale() == 1.25f);
        std::vector<std::uint32_t> host{kTouchSettingsVersion,0};
        for (std::size_t i = 0; i < kTouchSettingCount; ++i)
            host.push_back(i == 0 ? 100u : i == 17 ? 125u : touchSettingSpecs[i].toggle ? 0u :
                TouchFloatBits(static_cast<float>(touchSettingSpecs[i].minimum)));
        host[2+16] = 0xFFFFFFFFu;
        for (std::size_t i=21;i<32;++i) host[2+i]=0xFFFFFFFFu;
        host[2+32]=1;host[2+33]=1;
        auto invalidPerformance=host;invalidPerformance[2+32]=0xFFFFFFFFu;
        assert(ExchangeBl2TouchSettings(invalidPerformance)[1]==0 && !touchSettingsState.initialized);
        auto legacy = host; legacy[0] = 1; legacy.pop_back();
        assert(ExchangeBl2TouchSettings(legacy)[1] == 0 && !touchSettingsState.initialized);
        legacy = host; legacy[0] = 1;
        assert(ExchangeBl2TouchSettings(legacy)[1] == 0 && !touchSettingsState.initialized);
        legacy = host; legacy.push_back(125);
        assert(ExchangeBl2TouchSettings(legacy)[1] == 0 && !touchSettingsState.initialized && Bl2InterfaceScale() == 1.25f);
        host[2+kTouchMotionBlurSetting]=1; // Older builds could save ON.
        const auto initial = ExchangeBl2TouchSettings(host);
        assert(initial[3+kTouchMotionBlurSetting]==0 && !SetBl2TouchSetting(kTouchMotionBlurSetting,1));
        assert(initial.size() == kTouchSettingCount+3 && initial[0] == kTouchSettingsVersion && initial[1] == 1 && initial[2] == 0 &&
            initial[3+16] == (BL2_ENABLE_DEBUG_MENU_DEFAULT ? 1u : 0u) && initial[3+17] == 125 && Bl2InterfaceScale() == 1.25f &&
            initial[3+7] == 0 && initial[3+18] == 0 && initial[3+19] == 0);
        assert(SetBl2TouchSetting(0,60) && !SetBl2TouchSetting(0,65));
        auto changed = ExchangeBl2TouchSettings(host);
        assert(changed[1] == 2 && changed[3] == 60); // final-close stale host cannot undo native edit
        host[1] = changed[1]; host[2] = 60;
        assert(ExchangeBl2TouchSettings(host)[1] == changed[1]);
        CommandBl2TouchSettings(4);
        assert(ExchangeBl2TouchSettings(host)[2] == 4 && ExchangeBl2TouchSettings(host)[2] == 0);
        assert(SetBl2TouchSetting(16,1) && Bl2DebugMenuVisible());
        const auto debugEdit = ExchangeBl2TouchSettings(host);
        assert(debugEdit[2] == 8 && ExchangeBl2TouchSettings(host)[2] == 0);
        host[1] = debugEdit[1]; host[2+16] = 1; host[2 + 8] = 1;
        assert(ExchangeBl2TouchSettings(host)[3 + 8] == 1 && (touchSettingsState.uiRequest & 4u));
        assert(!SetBl2TouchSetting(16,2));
        for(int percent=100;percent<=200;percent+=10) {
            assert(SetBl2TouchSetting(17,percent));
            assert(std::abs(Bl2InterfaceScale()-percent/100.f) < .00001f);
        }
        const auto staleUi = ExchangeBl2TouchSettings(host);
        assert(staleUi[3+17] == 200 && Bl2InterfaceScale() == 2.f && staleUi[3] == 60);
        host[1] = staleUi[1]; host[2+17] = 200;
        assert(ExchangeBl2TouchSettings(host)[3+17] == 200);
        assert(SetBl2TouchSetting(17,125) && Bl2InterfaceScale() == 1.25f);
        assert(!SetBl2TouchSetting(17,99) && !SetBl2TouchSetting(17,201) && !SetBl2TouchSetting(17,126));
        assert(!SetBl2TouchSetting(17,125.5) && !SetBl2TouchSetting(17,110.0001));
        const auto currentUi = ExchangeBl2TouchSettings(host);
        assert(currentUi[3+17] == 125 && currentUi[3+16] == 1);
        host[1] = currentUi[1]; host[2+17] = 126;
        assert(ExchangeBl2TouchSettings(host)[3+17] == 125); // Invalid host grid cannot alter native UI state.
        host[2+17] = 125;
        assert(ExchangeBl2TouchSettings(host)[3+17] == 125);
        assert(!Bl2ShowFps() && Bl2FrameRateLimit() == 0);
        assert(SetBl2TouchSetting(18,1) && Bl2ShowFps());
        for (const auto limit : {0,30,60,90}) assert(SetBl2TouchSetting(19,limit) && Bl2FrameRateLimit() == static_cast<unsigned>(limit));
        assert(!SetBl2TouchSetting(19,45) && !SetBl2TouchSetting(19,120) && !SetBl2TouchSetting(19,30.5));
        const auto saved = touchSettingsState.values;
        assert(!SetBl2TouchSetting(32,2) && !SetBl2TouchSetting(33,-1));
        assert(SetBl2TouchSetting(32,0) && SetBl2TouchSetting(33,0));
        const auto disabledPerformance=ExchangeBl2TouchSettings(host);
        assert(disabledPerformance[3+32]==0 && disabledPerformance[3+33]==0 &&
            disabledPerformance[3]==saved[0] && disabledPerformance[3+17]==saved[17] &&
            disabledPerformance[1]!=host[1]);
        assert(SetBl2TouchSetting(32,1) && SetBl2TouchSetting(33,1));
        assert(TouchSettingValueText(1,TouchSettingNumber(1,TouchFloatBits(.42f))) == "42%");
        assert(TouchSettingValueText(2,.9985) == "100%" && TouchSettingValueText(0,150) == "150%");
        assert(TouchSettingNumber(17,125) == 125 && TouchSettingValueText(17,125) == "125%");
        assert(TouchSettingValueText(3,.223999798) == "0.22" && TouchSettingValueText(4,.55000001192) == "0.55");
        assert(TouchSettingValueText(11,1) == "1.00" && TouchSettingValueText(12,.400000059) == "0.40");
        assert(touchSettingsState.values == saved); // Display rounding cannot quantize saved preferences.
        // v7 appended the Reload button toggle after frame pacing, v8 the tap radial, v9 the
        // inset margin, button icons and hidden gamepad images, v10 look acceleration;
        // ids 0-36 keep their slots.
        assert(kTouchSettingsVersion == 10 && kTouchSettingCount == 41 && kTouchReloadButtonSetting == 35);
        assert(touchSettingSpecs[kTouchReloadButtonSetting].toggle &&
            std::string(TouchSettingCaption(kTouchReloadButtonSetting,false)) == "RELOAD BUTTON" &&
            std::string(TouchSettingCaption(kTouchReloadButtonSetting,true)) == "КНОПКА ПЕРЕЗАРЯДКИ" &&
            TouchSettingHint(kTouchReloadButtonSetting,true) && TouchSettingHint(kTouchReloadButtonSetting,false));
        assert(!SetBl2TouchSetting(kTouchReloadButtonSetting,2) && !SetBl2TouchSetting(kTouchReloadButtonSetting,.5));
        assert(SetBl2TouchSetting(kTouchReloadButtonSetting,1));
        const auto reloadOn = ExchangeBl2TouchSettings(host);
        assert(reloadOn.size() == kTouchSettingCount+3 && reloadOn[3+kTouchReloadButtonSetting] == 1 &&
            reloadOn[3+kTouchFramePacingSetting] == saved[kTouchFramePacingSetting]);
        auto oldSize = host; oldSize.pop_back(); oldSize[0] = 7; oldSize[1] = reloadOn[1];
        assert(ExchangeBl2TouchSettings(oldSize)[3+kTouchReloadButtonSetting] == 1); // A v7 host cannot clear it.
        host[1] = reloadOn[1]; host[2+kTouchReloadButtonSetting] = 2;
        assert(ExchangeBl2TouchSettings(host)[3+kTouchReloadButtonSetting] == 1); // Non-boolean host value rejected.
        assert(kTouchRadialTapSetting == 36 && touchSettingSpecs[kTouchRadialTapSetting].toggle &&
            std::string(TouchSettingCaption(kTouchRadialTapSetting,false)) == "RADIAL BY TAP" &&
            std::string(TouchSettingCaption(kTouchRadialTapSetting,true)) == "РАДИАЛ ПО ТАПУ" &&
            TouchSettingHint(kTouchRadialTapSetting,true) && TouchSettingHint(kTouchRadialTapSetting,false));
        assert(touchSettingsState.values[kTouchRadialTapSetting] == 0); // OFF unless chosen.
        assert(!SetBl2TouchSetting(kTouchRadialTapSetting,2) && !SetBl2TouchSetting(kTouchRadialTapSetting,.5));
        assert(SetBl2TouchSetting(kTouchRadialTapSetting,1));
        const auto radialOn = ExchangeBl2TouchSettings(host);
        assert(radialOn.size() == kTouchSettingCount+3 && radialOn[3+kTouchRadialTapSetting] == 1 &&
            radialOn[3+kTouchReloadButtonSetting] == 1);
        assert(kTouchScreenInsetsSetting == 37 && kTouchButtonIconsSetting == 38 && kTouchHideGamepadGlyphsSetting == 39);
        for (const auto id : {kTouchScreenInsetsSetting,kTouchButtonIconsSetting,kTouchHideGamepadGlyphsSetting})
            assert(touchSettingSpecs[id].toggle && TouchSettingHint(id,true) && TouchSettingHint(id,false) &&
                !SetBl2TouchSetting(id,2) && !SetBl2TouchSetting(id,.5));
        assert(std::string(TouchSettingCaption(kTouchScreenInsetsSetting,true)) == "ОТСТУП ОТ ВЫРЕЗА ЭКРАНА" &&
            std::string(TouchSettingCaption(kTouchButtonIconsSetting,true)) == "ЗНАЧКИ НА КНОПКАХ" &&
            std::string(TouchSettingCaption(kTouchHideGamepadGlyphsSetting,true)) == "СКРЫТЬ КНОПКИ ГЕЙМПАДА" &&
            std::string(TouchSettingCaption(kTouchHideGamepadGlyphsSetting,false)) == "HIDE GAMEPAD BUTTONS");
        assert(!Bl2HideGamepadGlyphs() && SetBl2TouchSetting(kTouchHideGamepadGlyphsSetting,1) && Bl2HideGamepadGlyphs());
        const auto v9 = ExchangeBl2TouchSettings(host);
        assert(v9[3+kTouchHideGamepadGlyphsSetting] == 1 && v9[3+kTouchScreenInsetsSetting] == 0);
        assert(SetBl2TouchSetting(kTouchHideGamepadGlyphsSetting,0) && !Bl2HideGamepadGlyphs());
        // v10: look acceleration follows the v9 fields; look and aim sensitivity reach 3.00.
        assert(kTouchLookAccelerationSetting == 40 && touchSettingSpecs[kTouchLookAccelerationSetting].toggle &&
            std::string(TouchSettingCaption(kTouchLookAccelerationSetting,true)) == "УСКОРЕНИЕ ОБЗОРА" &&
            std::string(TouchSettingCaption(kTouchLookAccelerationSetting,false)) == "LOOK ACCELERATION" &&
            TouchSettingHint(kTouchLookAccelerationSetting,true) && TouchSettingHint(kTouchLookAccelerationSetting,false));
        assert(!SetBl2TouchSetting(kTouchLookAccelerationSetting,2) && !SetBl2TouchSetting(kTouchLookAccelerationSetting,.5) &&
            SetBl2TouchSetting(kTouchLookAccelerationSetting,1));
        assert(SetBl2TouchSetting(3,3) && SetBl2TouchSetting(4,2.5) && !SetBl2TouchSetting(3,3.01) && !SetBl2TouchSetting(4,3.01));
        assert(TouchSettingValueText(3,3) == "3.00" && TouchSettingValueText(4,2.5) == "2.50");
        const auto v10 = ExchangeBl2TouchSettings(host);
        assert(v10.size() == kTouchSettingCount+3 && v10[3+kTouchLookAccelerationSetting] == 1 &&
            TouchSettingNumber(3,v10[3+3]) == 3 && TouchSettingNumber(4,v10[3+4]) == 2.5);
        auto v9Host = host; v9Host[0] = 9; v9Host.pop_back(); v9Host[1] = v10[1];
        assert(ExchangeBl2TouchSettings(v9Host)[3+kTouchLookAccelerationSetting] == 1); // A v9 host cannot clear it.
        // GfxSession::stringValue refuses 256 bytes or more: a longer UTF-8 hint never shows.
        for (std::size_t id = 0; id < kTouchSettingCount; ++id)
            for (const bool russian : {false, true}) {
                const char* hint = TouchSettingHint(id,russian);
                assert((!hint || std::strlen(hint) < 256) && std::strlen(TouchSettingCaption(id,russian)) < 256);
            }
    }
    {
        // Overflowing original descriptions scroll by whole lines under a vertical drag.
        const auto missions = Bl2TextScrollTargets("StatusMenuExGFxMovie",1);
        assert(missions.size() == 1 && std::string(missions[0]->field) == "_root.missions.missionSummary.description" &&
            missions[0]->parentOwnsSwipe && std::string(missions[0]->arrows) == "SetScrollArrows");
        const auto challenges = Bl2TextScrollTargets("StatusMenuExGFxMovie",5);
        assert(challenges.size() == 2 && !challenges[0]->parentOwnsSwipe && !challenges[1]->arrows);
        assert(Bl2TextScrollTargets("QuestAcceptGFxMovie",-1).size() == 1);
        for (const auto screen : {0,2,3,4}) assert(Bl2TextScrollTargets("StatusMenuExGFxMovie",screen).empty());
        assert(Bl2TextScrollTargets("PauseGFxMovie",1).empty() && Bl2TextScrollTargets("OptionsGFxMovie",5).empty());
        // UI_StatusMenu EditText526 at its 101.6% layout scale: Scaleform reports
        // _x/_y at the box's own top-left, so its [-86.15,-2] origin is not re-added.
        const std::array<double,7> summary{-52.65,-109.35,276.75*1.016,258.45*1.016,101.6,101.6,0};
        GfxRect local;
        assert(Bl2TextScrollLocalRect(*missions[0],summary,local));
        assert(std::abs(local.left-(-52.65)) < 1e-9 && std::abs(local.top-(-109.35)) < 1e-9);
        assert(std::abs(local.right-(-52.65+276.75*1.016)) < 1e-9 && std::abs(local.bottom-(-109.35+258.45*1.016)) < 1e-9);
        for (const unsigned bad : {2u,3u,4u,6u}) {
            auto metrics = summary; metrics[bad] += bad == 6 ? .5 : 5;
            assert(!Bl2TextScrollLocalRect(*missions[0],metrics,local) && local.right == 0);
        }
        // Device read of the live field at its own _z=-600: placement (-52.65,
        // -109.35) plus the scaled origin, both sides reduced by one depth factor.
        const std::array<double,7> deep{-140.20,-111.40,271.35,253.40,101.6,101.6,0};
        assert(Bl2TextScrollLocalRect(*missions[0],deep,local));
        assert(std::abs(local.left-(-140.20)) < 1e-9 && std::abs(local.right-(-140.20+276.75*1.016)) < 1e-9);
        assert(std::abs(local.top-(-111.40)) < 1e-9 && std::abs(local.bottom-(-111.40+258.45*1.016)) < 1e-9);
        auto uneven = deep; uneven[3] = 258.45*1.016; // One reduced side is another shape.
        assert(!Bl2TextScrollLocalRect(*missions[0],uneven,local) && local.right == 0);
        auto shrunk = summary; shrunk[2] *= .85; shrunk[3] *= .85; // Beyond any depth factor.
        assert(!Bl2TextScrollLocalRect(*missions[0],shrunk,local) && local.right == 0);
        auto dead = summary; dead[0] = std::numeric_limits<double>::quiet_NaN();
        assert(!Bl2TextScrollLocalRect(*missions[0],dead,local));
        assert(Bl2TextScrollLine(1,9,-100,20) == 6 && Bl2TextScrollLine(6,9,100,20) == 1);
        assert(Bl2TextScrollLine(1,9,-1000,20) == 9 && Bl2TextScrollLine(9,9,1000,20) == 1);
        assert(Bl2TextScrollLine(3,9,-9,20) == 3 && Bl2TextScrollLine(3,9,-11,20) == 4); // Rounded whole lines.
        assert(Bl2TextScrollLine(3,9,std::numeric_limits<double>::quiet_NaN(),20) == 3);
        assert(Bl2TextScrollLine(3,9,-50,0) == 3 && Bl2TextScrollLine(0,9,-50,20) == 0 && Bl2TextScrollLine(10,9,-50,20) == 10);
        std::cout << "PASS: settings v10 Reload, tap radial, inset margin, icon, gamepad image and look acceleration toggles, 3x look/aim sensitivity; mission/challenge/quest description targets, authored boxes and line mapping\n";
    }
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
        assert(widgets.selectedIndex == 0 && widgets.rows[1].sliderBounds.top == widgets.rows[1].bounds.top);
        session.close(); assert(vm.refs.empty() && vm.movieRefs == 1 && session.cleanupOkay());
    }
    {
        Vm vm(argv[1]);
        vm.model.movieClass = "OptionsGFxMovie";
        vm.object(vm.renderers).array.clear();
        vm.object(vm.data).array.clear(); vm.model.eventIds.clear();
        for (unsigned i = 0; i < 18; ++i) {
            auto item = vm.make(8), renderer = vm.make(10);
            vm.prop(item,"unselectable",boolean(false)); vm.prop(item,"label",vm.str("Touch setting"));
            vm.prop(item,"rendererType",vm.str("default"));
            vm.visible(renderer,vm.list); vm.prop(renderer,"owner",vm.list); vm.prop(renderer,"data",item);
            vm.prop(renderer,"index",num(i)); vm.prop(renderer,"disabled",boolean(false));
            vm.object(renderer).bounds = {100,100.0 + 25*i,1100,125.0 + 25*i};
            vm.object(renderer).ty = 100.0 + 25*i;
            vm.object(vm.renderers).array.push_back(renderer); vm.object(vm.data).array.push_back(item);
            vm.model.eventIds.push_back(kTouchSettingsValue + static_cast<std::int32_t>(i));
        }
        vm.prop(vm.list,"totalRenderers",num(18));
        {
            GfxSession s(vm,base,movie,scratch1,1024);
            const auto widgets = CaptureBl2Widgets(s,vm.model,2560,1440);
            assert(widgets.valid && widgets.rowCount == 18 && widgets.totalRenderers == 18 && widgets.rows.size() == 18);
        }
        vm.model.eventIds.pop_back();
        {
            GfxSession s(vm,base,movie,scratch1,1024);
            assert(CaptureBl2Widgets(s,vm.model,2560,1440).reason == "list-event-count-AS-18-model-17");
        }
        vm.model.eventIds.push_back(kTouchSettingsBack); vm.prop(vm.list,"totalRenderers",num(17));
        {
            GfxSession s(vm,base,movie,scratch1,1024);
            assert(!CaptureBl2Widgets(s,vm.model,2560,1440).valid); // Array/count mismatch still rejects.
        }
        assert(vm.refs.empty() && vm.movieRefs == 1);
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
    {
        // Default125 joins the10% grid on the first directional/repeat edit.
        // Stock initSlider may snap its thumb to130 while data still says125;
        // a second caption tap must advance125→130, never skip to140.
        assert(StepBl2InterfacePercent(125,false) == 120 && StepBl2InterfacePercent(125,true) == 130);
        assert(StepBl2InterfacePercent(120,false) == 110 && StepBl2InterfacePercent(130,true) == 140);
        assert(StepBl2InterfacePercent(100,false) == 100 && StepBl2InterfacePercent(200,true) == 200);
        Vm vm(argv[1]); vm.model.movieClass = "OptionsGFxMovie";
        vm.model.eventIds[1] = kTouchSettingsValue + 17;
        vm.prop(vm.sliderData,"minValue",num(100)); vm.prop(vm.sliderData,"maxValue",num(200));
        vm.prop(vm.sliderData,"sliderIncrement",num(10)); vm.prop(vm.sliderData,"currentValue",num(125));
        vm.prop(vm.slider,"minimum",num(100)); vm.prop(vm.slider,"maximum",num(200));
        vm.prop(vm.slider,"snapInterval",num(10)); vm.prop(vm.slider,"value",num(130));
        const auto prototype = vm.make(8), listeners = vm.make(8), handler = vm.make(8);
        vm.prop(vm.list,"__proto__",prototype); vm.prop(vm.list,"_listeners",listeners);
        for (const char* name : {"onSliderValueChange","onSpinnerValueChange"}) {
            vm.prop(vm.list,name,handler); vm.prop(prototype,name,handler);
        }
        assert(SetBl2TouchSetting(17,125));
        WidgetRegistry widgets;
        { GfxSession capture(vm,base,movie,scratch1,768); widgets = CaptureBl2Widgets(capture,vm.model,2560,1440); }
        if (!widgets.valid) std::cerr << "Interface registry: " << widgets.reason << '\n';
        assert(widgets.valid && widgets.rows[1].value == 130 && widgets.rows[1].increment == 10 &&
            number(vm.object(vm.sliderData).properties.at("currentValue")) == 125);
        {
            GfxSession hold(vm,base,movie,scratch1,128);
            WidgetBinding binding(hold,vm.model,widgets,&widgets.rows[1]); assert(binding.valid());
            WidgetCommand command; command.kind = WidgetCommandKind::Click;
            command.route = widgets.route; command.target = widgets.rows[1];
            for (unsigned pass = 0; pass < 3; ++pass) {
                GfxSession operation(vm,base,movie,scratch2,384,true);
                const auto delivered = ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; });
                assert(delivered.state == WidgetDeliveryState::Dispatched && operation.good() && vm.selections == 1);
                assert(touchSettingsState.values[17] == (pass == 0 ? 125u : pass == 1 ? 130u : 140u));
                assert(vm.sliderNotifications == static_cast<int>(pass));
            }
            command.kind = WidgetCommandKind::Slider;
            for (const auto fraction : {.04,.26,.93,1.0,0.0}) {
                command.sliderFraction = fraction;
                GfxSession operation(vm,base,movie,scratch2,384,true);
                const auto delivered = ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; });
                assert(delivered.state == WidgetDeliveryState::Dispatched && operation.good());
                assert(delivered.actualValue >= 100 && delivered.actualValue <= 200 && std::fmod(delivered.actualValue,10) == 0);
                assert(touchSettingsState.values[17] == delivered.actualValue);
            }
            const auto saved = touchSettingsState.values;
            GfxSession stale(vm,base,movie,scratch2,384,true);
            assert(ApplyBl2WidgetCommand(stale,binding,vm.model,widgets,command,0,[] { return false; }).state == WidgetDeliveryState::Rejected);
            assert(touchSettingsState.values == saved);
        }
        assert(vm.refs.empty() && vm.movieRefs == 1 && vm.object(vm.list).properties.at("_listeners") == listeners);
    }
    for(const auto [event,request]:std::array<std::pair<std::int32_t,unsigned>,3>{{
        {kTouchSettingsOpen,1u},{kTouchSettingsGraphicsOpen,32u},{kTouchSettingsNativeBack,16u}}}) {
        Vm vm(argv[1]);
        vm.model.movieClass = "OptionsGFxMovie"; vm.model.eventIds[1] = event;
        vm.prop(vm.sliderData,"rendererType",vm.str("default"));
        auto prototype = vm.make(8), listeners = vm.make(8), handler = vm.make(8);
        vm.prop(vm.list,"__proto__",prototype); vm.prop(vm.list,"_listeners",listeners);
        for (const char* name : {"onSliderValueChange","onSpinnerValueChange"}) {
            vm.prop(vm.list,name,handler); vm.prop(prototype,name,handler);
        }
        WidgetRegistry widgets;
        { GfxSession capture(vm,base,movie,scratch1,768); widgets = CaptureBl2Widgets(capture,vm.model,2560,1440); }
        assert(widgets.valid && widgets.rows[1].kind == WidgetKind::Button);
        GfxSession hold(vm,base,movie,scratch1,128);
        WidgetBinding binding(hold,vm.model,widgets,&widgets.rows[1]); assert(binding.valid());
        WidgetCommand command; command.kind = WidgetCommandKind::Click; command.route = widgets.route; command.target = widgets.rows[1];
        touchSettingsState.uiRequest = 0;
        for (int i = 0; i < 2; ++i) {
            GfxSession operation(vm,base,movie,scratch2,256,true);
            assert(ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; }).state == WidgetDeliveryState::Dispatched);
            assert(vm.selections == 1 && touchSettingsState.uiRequest == (i?request:0u) && vm.clicks == 0);
            for (const char* name : {"_listeners","onSliderValueChange","onSpinnerValueChange"}) {
                const auto restored = vm.object(vm.list).properties.at(name);
                assert(restored == (std::strcmp(name,"_listeners") == 0 ? listeners : handler));
            }
        }
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
        WidgetCommand command; command.kind = WidgetCommandKind::Click; command.route = widgets.route; command.target = widgets.rows[1];
        for (int i = 0; i < 2; ++i) {
            GfxSession operation(vm,base,movie,scratch2,256,true);
            const auto result = ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[] { return true; });
            assert(result.state == WidgetDeliveryState::Dispatched && vm.selections == 1);
            assert(vm.sliderNotifications == i && number(vm.object(vm.slider).properties.at("value")) == 5 + i);
        }
    }
    {
        Vm vm(argv[1]);
        const auto renderer=vm.object(vm.renderers).array[1];
        vm.prop(vm.sliderData,"rendererType",vm.str("spinner"));
        auto choices=vm.make(9);vm.object(choices).array={vm.str("Off"),vm.str("2x"),vm.str("4x"),vm.str("8x"),vm.str("16x")};
        vm.prop(vm.sliderData,"spinnerChoices",choices);vm.prop(vm.sliderData,"currentValue",num(2));
        WidgetRegistry widgets;
        {GfxSession capture(vm,base,movie,scratch1,768);widgets=CaptureBl2Widgets(capture,vm.model,2560,1440);}
        assert(widgets.valid && widgets.rows[1].kind==WidgetKind::Spinner && widgets.rows[1].maximum==4);
        GfxSession hold(vm,base,movie,scratch1,128);WidgetBinding binding(hold,vm.model,widgets,&widgets.rows[1]);assert(binding.valid());
        WidgetCommand command;command.kind=WidgetCommandKind::Spinner;command.route=widgets.route;command.target=widgets.rows[1];command.spinnerIndex=4;
        {GfxSession operation(vm,base,movie,scratch2,256,true);
            auto result=ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[]{return true;});
            assert(result.state==WidgetDeliveryState::Dispatched && vm.spinnerNotifications==1 && number(vm.object(vm.sliderData).properties.at("currentValue"))==4);}
        {GfxSession operation(vm,base,movie,scratch2,256,true);command.spinnerIndex=5;
            assert(ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[]{return true;}).state==WidgetDeliveryState::Rejected);
            assert(vm.spinnerNotifications==1);}
        command.kind = WidgetCommandKind::Click;
        for (int i = 0; i < 2; ++i) {
            GfxSession operation(vm,base,movie,scratch2,256,true);
            assert(ApplyBl2WidgetCommand(operation,binding,vm.model,widgets,command,0,[]{return true;}).state == WidgetDeliveryState::Dispatched);
            assert(vm.selections == 1 && vm.spinnerNotifications == 1 + i);
            assert(number(vm.object(vm.sliderData).properties.at("currentValue")) == (i ? 0 : 4));
        }
        (void)renderer;
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
