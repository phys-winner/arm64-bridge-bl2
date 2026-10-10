#pragma once
#include "bl2_touch_widget_commands.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace widget_fixture {
using namespace bl2::touch;
using Raw = std::array<std::uint32_t,8>;
constexpr std::uint32_t base = 0x10000000, movie = 0x23000000, interface = 0x23001000;
constexpr std::uint32_t movieTable = 0x23002000, valueTable = 0x23003000;
constexpr std::uint32_t wrapper = 0x23004000, scratch1 = 0x23005000, scratch2 = 0x23008000;
constexpr std::uint32_t add = base + 0x2000000, release = add + 4, get = add + 8;
constexpr std::uint32_t set = add + 12, invoke = add + 16, element = add + 20, frame = add + 24, viewport = add + 28;
inline Raw num(double n) { Raw value{}; value[3] = 5; std::memcpy(&value[4],&n,8); return value; }
inline Raw boolean(bool b) { Raw value{}; value[3] = 2; value[4] = b ? 1 : 0; return value; }
inline double number(const Raw& raw) { assert(raw[3] == 5); double v = 0; std::memcpy(&v,&raw[4],8); return v; }
struct Object {
    std::uint32_t type = 8;
    std::unordered_map<std::string,Raw> properties;
    std::vector<Raw> array;
    GfxRect bounds;
    double tx = 0, ty = 0;
    bool transformBounds = false;
};
struct Vm final : GfxTransport {
    std::unordered_map<std::uint32_t,std::uint8_t> memory;
    std::unordered_map<std::uint32_t,Object> objects;
    std::unordered_map<std::uint32_t,std::uint32_t> refs;
    std::uint32_t next = 0x24000000;
    bool currentScene = true, abandoned = false, changeInClick = false;
    int movieRefs = 1, clicks = 0, sliderNotifications = 0, spinnerNotifications = 0, scrolls = 0, selections = 0;
    unsigned propertyWrites = 0;
    unsigned rowHeightChanges = 0, layouts = 0, textFormats = 0;
    unsigned stateRefreshes = 0;
    unsigned rendererRebinds = 0;
    bool resetInnerOnState = false;
    bool changeOnPropertyWrite = false;
    bool layoutPending = false;
    std::array<float,4> visibleFrame{0,0,1280,720};
    Raw root{}, list{}, data{}, renderers{}, slider{}, inner{}, sliderData{};
    std::vector<Raw> rendererCapacity;
    UiModelSnapshot model;
    explicit Vm(const char* fixture) {
        std::ifstream in(fixture); assert(in.good());
        std::uint32_t p = 0, word = 0;
        while (in >> std::hex >> p >> word) put(base + p,word);
        put(movie,movieTable); put(movie + 4,1u); put(movieTable + 4,add + 32);
        put(interface,valueTable);
        for (const auto pair : std::vector<std::pair<std::uint32_t,std::uint32_t>>{
            {8,add},{12,release},{20,get},{24,set},{28,invoke},{52,element}}) put(valueTable + pair.first,pair.second);
        put(movieTable + 0x4C,frame); put(movieTable + 0x38,viewport);
        root = make(10); list = make(10); data = make(9); renderers = make(9);
        visible(root,{}); visible(list,root);
        prop(root,"_x",num(0)); prop(root,"_y",num(0)); prop(root,"_xscale",num(100));
        prop(root,"_yscale",num(100)); prop(root,"_rotation",num(0));
        prop(list,"_root",root); prop(list,"data",data); prop(list,"dataProvider",data); prop(list,"renderers",renderers);
        prop(list,"totalRenderers",num(2)); prop(list,"scrollPosition",num(0)); prop(list,"disabled",boolean(false));
        prop(list,"externalRenderers",boolean(false)); // Original CoreList prototype default.
        prop(list,"selectedIndex",num(0));
        object(list).bounds = {100,100,1100,500};
        for (unsigned i = 0; i < 4; ++i) {
            auto item = make(8);
            prop(item,"unselectable",boolean(false)); prop(item,"label",str(i == 1 ? "Sound" : "Menu"));
            prop(item,"rendererType",str(i == 1 ? "slider" : "default"));
            object(data).array.push_back(item);
            if (i >= 2) continue;
            auto renderer = make(10); visible(renderer,list);
            prop(renderer,"owner",list); prop(renderer,"data",item); prop(renderer,"index",num(i));
            prop(renderer,"disabled",Raw{}); // Stock CLIK's initial undefined _disabled.
            object(renderer).bounds = {100,100 + 200.0 * i,1100,250 + 200.0 * i};
            object(renderer).tx=100;object(renderer).ty=100+200.0*i;
            object(renderers).array.push_back(renderer);
            if (i == 1) {
                sliderData = item; inner = make(10); slider = make(10);
                auto track = make(10); visible(inner,renderer); visible(slider,inner); visible(track,slider);
                prop(renderer,"inner",inner); prop(inner,"slider",slider); prop(inner,"data",item);
                auto valueText=make(10);visible(valueText,inner);prop(inner,"currentValue",valueText);
                prop(valueText,"_x",num(1010));prop(valueText,"_y",num(300));prop(valueText,"_width",num(70));prop(valueText,"_height",num(40));
                prop(valueText,"_rotation",num(0));prop(valueText,"_xscale",num(100));prop(valueText,"_yscale",num(100));
                prop(slider,"track",track); prop(slider,"disabled",boolean(false));
                prop(slider,"minimum",num(0)); prop(slider,"maximum",num(10)); prop(slider,"snapInterval",num(1));
                prop(slider,"value",num(5)); prop(slider,"__width",num(500));
                prop(slider,"offsetLeft",num(10)); prop(slider,"offsetRight",num(10));
                object(slider).bounds = {500,300,1000,450}; object(slider).tx = 500; object(slider).ty = 300;
                object(track).bounds = {10,30,490,50};
                prop(item,"minValue",num(0)); prop(item,"maxValue",num(10));
                prop(item,"sliderIncrement",num(1)); prop(item,"currentValue",num(5));
            }
        }
        write(wrapper + 0x3C,list.data(),32);
        model.valid = true; model.movieClass = "FrontendGFxMovie"; model.structure = 100;
        model.owner = 0x6300000000000001ull;
        model.list = (std::uint64_t(wrapper) << 32) | 2;
        model.provider = 0x6300000100000003ull; model.eventIds = {6,107,2,5};
    }
    Raw make(std::uint32_t type) {
        const auto p = next; next += 0x100;
        objects[p].type = type;
        Raw value{}; value[2] = interface; value[3] = type | 0x40; value[4] = p;
        return value;
    }
    Raw str(const char* text) {
        Raw value{}; value[3] = 6; value[4] = next; next += 0x100;
        write(value[4],text,std::strlen(text) + 1); return value;
    }
    Object& object(const Raw& value) { return objects.at(value[4]); }
    void prop(const Raw& o, const std::string& name, const Raw& v) { object(o).properties[name] = v; }
    void visible(const Raw& o, const Raw& parent) {
        prop(o,"_visible",boolean(true)); prop(o,"_alpha",num(100));
        prop(o,"_xscale",num(100)); prop(o,"_yscale",num(100)); prop(o,"_rotation",num(0));
        prop(o,"_x",num(0)); prop(o,"_y",num(0));
        if (parent[3]) prop(o,"_parent",parent);
    }
    template<class T> void put(std::uint32_t p, const T& v) { write(p,&v,sizeof(v)); }
    bool read(std::uint32_t p, void* out, std::size_t count) const override {
        auto* dst = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < count; ++i) {
            auto it = memory.find(p + static_cast<std::uint32_t>(i)); if (it == memory.end()) return false;
            dst[i] = it->second;
        }
        return true;
    }
    bool write(std::uint32_t p, const void* in, std::size_t count) override {
        const auto* src = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < count; ++i) memory[p + static_cast<std::uint32_t>(i)] = src[i];
        return true;
    }
    std::string text(std::uint32_t p) const {
        std::string out; char c = 0;
        while (out.size() < 128 && read(p++,&c,1) && c) out += c;
        return out;
    }
    Raw raw(std::uint32_t p) const { Raw r{}; assert(read(p,r.data(),32)); return r; }
    bool executable(std::uint32_t p) const override { return p >= base + 0xE823B0 && p < base + 0x2BB91EC; }
    bool current() const override { return currentScene && !abandoned; }
    void abandonScratch() override { abandoned = true; }
    void addref(std::uint32_t p) {
        const auto v = raw(p); assert(!v[0] && !v[1] && !refs.count(p)); refs[p] = v[4];
        put(p,0xA1100001u); put(p + 4,0xA1100002u);
    }
    void output(std::uint32_t p, const Raw& value) { write(p,value.data(),32); if (value[3] & 0x40) addref(p); }
    void bindWindow() {
        const auto& items = object(object(list).properties.at("data")).array;
        const auto scroll = static_cast<unsigned>(number(object(list).properties.at("scrollPosition")));
        const auto selected = static_cast<unsigned>(number(object(list).properties.at("selectedIndex")));
        for (unsigned i = 0; i < object(renderers).array.size(); ++i) {
            const auto renderer = object(renderers).array[i];
            prop(renderer,"index",num(scroll + i));
            prop(renderer,"data",scroll + i < items.size() ? items[scroll + i] : Raw{});
            prop(renderer,"selected",boolean(scroll + i == selected));
            if (object(renderer).properties.count("inner"))
                prop(object(renderer).properties.at("inner"),"data",object(renderer).properties.at("data"));
        }
    }
    bool call(std::uint32_t pc, const std::vector<std::uint32_t>& a, std::uint32_t& result) override {
        result = 1;
        if (pc == base + GfxSession::kMovieAddRef) { ++movieRefs; put(movie + 4,static_cast<std::uint32_t>(movieRefs)); }
        else if (pc == base + GfxSession::kMovieRelease) {
            assert(movieRefs > 1); --movieRefs; put(movie + 4,static_cast<std::uint32_t>(movieRefs));
            if (movieRefs == 1) assert(refs.empty());
        } else if (pc == add) { assert(movieRefs >= 2); addref(a[1]); }
        else if (pc == release) {
            assert(movieRefs >= 2 && refs.at(a[1]) == a[2]); refs.erase(a[1]);
        } else if (pc == base + GfxSession::kCreateObject) { output(a[1],make(8)); result = 0; }
        else if (pc == get) {
            auto& o = objects.at(a[1]); const auto key = text(a[2]);
            if (key == "length" && o.type == 9) output(a[3],num(static_cast<double>(o.array.size())));
            else if (key == "textWidth" && o.properties.count("_measuredTextWidth")) output(a[3],o.properties.at("_measuredTextWidth"));
            else if (key == "textHeight" && o.properties.count("_measuredTextHeight")) output(a[3],o.properties.at("_measuredTextHeight"));
            else if (o.properties.count(key)) output(a[3],o.properties.at(key));
            else { output(a[3],{}); result = 0; }
        } else if (pc == element) {
            auto& values = objects.at(a[1]).array;
            if (a[2] < values.size()) output(a[3],values[a[2]]);
            else { output(a[3],{}); result = 0; }
        } else if (pc == set) {
            auto value = raw(a[3]);
            // An AS property stores the payload, not the caller's local
            // GFx::Value intrusive ownership links.
            value[0] = value[1] = 0;
            // SetMember copies an unmanaged string into the AVM property; the
            // caller's scratch string is reusable after its GFx::Value dies.
            if (value[3] == 6) value = str(text(value[4]).c_str());
            objects.at(a[1]).properties[text(a[2])] = value;
            ++propertyWrites;
            if (changeOnPropertyWrite && objects.at(a[1]).transformBounds) currentScene = false;
        }
        else if (pc == frame) { write(a[0],visibleFrame.data(),16); }
        else if (pc == viewport) { const std::array<std::uint32_t,13> v{2560,1440,0,0,2560,1440}; write(a[1],v.data(),52); }
        else if (pc == invoke) {
            const auto key = text(a[3]); Raw out{};
            if (key == "slice") {
                assert(objects.at(a[1]).type == 9 && a[5] == 0);
                out = make(9); object(out).array = objects.at(a[1]).array;
            } else if (key == "push") {
                assert(objects.at(a[1]).type == 9);
                for (unsigned i = 0; i < a[5]; ++i) {
                    auto value = raw(a[4] + i * 32); value[0] = value[1] = 0;
                    if (value[3] == 6) value = str(text(value[4]).c_str());
                    objects.at(a[1]).array.push_back(value);
                }
                out = num(static_cast<double>(objects.at(a[1]).array.size()));
            } else if (key == "CommitListItems") {
                prop(list,"dataProvider",object(list).properties.at("data"));
            } else if (key == "__get__rowHeight") {
                const auto& props = objects.at(a[1]).properties;
                if (props.count("_rowHeight")) out = props.at("_rowHeight");
            } else if (key == "__set__rowHeight") {
                assert(a[5] == 1 && number(raw(a[4])) > 0);
                out = raw(a[4]); prop(list,"_rowHeight",out); ++rowHeightChanges; layoutPending = true;
            } else if (key == "validateNow") {
                if (layoutPending) {
                layoutPending = false;
                // Original ScrollingList.draw uses floor(availableHeight /
                // rendererHeight + .05), then drawLayout sets each row's Y and
                // setSize. Its AS data/provider and selection remain intact.
                ++layouts;
                auto& rows = object(renderers).array;
                const double height = number(object(list).properties.at("_rowHeight"));
                const double available = number(object(list).properties.at("__height"));
                const auto count = static_cast<std::size_t>(std::floor(available / height + .05));
                if (rendererCapacity.empty()) rendererCapacity = rows;
                if (count > rows.size()) rows.assign(rendererCapacity.begin(),rendererCapacity.begin() + std::min(count,rendererCapacity.size()));
                if (rows.size() > count) rows.resize(count);
                prop(list,"totalRenderers",num(static_cast<double>(rows.size())));
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    auto& renderer = object(rows[i]); const double y = object(list).bounds.top + i * height;
                    renderer.bounds.top = y; renderer.bounds.bottom = y + height; renderer.ty = y;
                    // Original Button.draw assigns _height=__height. A MovieClip
                    // scales its children when the authored row height changes.
                    const double previous = number(renderer.properties.at("_height"));
                    prop(rows[i],"_yscale",num(number(renderer.properties.at("_yscale")) * height / previous));
                    prop(rows[i],"_height",num(height));
                }
                const auto& items = object(object(list).properties.at("data")).array;
                const double maximumScroll = std::max(0.0,static_cast<double>(items.size()) - rows.size());
                prop(list,"scrollPosition",num(std::clamp(number(object(list).properties.at("scrollPosition")),0.0,maximumScroll)));
                bindWindow();
                }
            } else if (key == "setState") {
                assert(a[5] == 1);
                auto& props = objects.at(a[1]).properties;
                assert(text(raw(a[4])[4]) == text(props.at("state")[4]));
                ++stateRefreshes;
                if (props.count("selectionArt")) {
                    const auto art = props.at("selectionArt");
                    prop(art,"_yscale",object(art).properties.at("authoredScaleY"));
                }
                if (resetInnerOnState && props.count("inner")) {
                    const auto content=props.at("inner");
                    prop(content,"_currentframe",num(1));
                    prop(object(content).properties.at("currentValue"),"_visible",boolean(false));
                    if (object(content).properties.count("slider"))
                        prop(object(content).properties.at("slider"),"_visible",boolean(false));
                }
            } else if (key == "setData") {
                assert(a[5] == 1);
                const auto item=raw(a[4]);auto& props=objects.at(a[1]).properties;
                assert(props.count("inner") && item==props.at("data"));
                const auto content=props.at("inner");prop(content,"data",item);++rendererRebinds;
                const auto type=text(object(item).properties.at("rendererType")[4]);
                prop(content,"_currentframe",num(type=="slider" ? 3 : type=="spinner" ? 2 : 1));
                prop(object(content).properties.at("currentValue"),"_visible",boolean(type=="slider" || type=="spinner"));
                if(object(content).properties.count("slider"))
                    prop(object(content).properties.at("slider"),"_visible",boolean(type=="slider"));
            } else if (key == "scrollToIndex") {
                assert(a[5] == 1);
                const double selected = number(raw(a[4]));
                const double capacity = number(object(list).properties.at("totalRenderers"));
                const double previous = number(object(list).properties.at("scrollPosition"));
                if (capacity > 0 && (selected < previous || selected >= previous + capacity)) {
                    const double last = std::max(0.0,static_cast<double>(object(object(list).properties.at("data")).array.size()) - capacity);
                    const double scroll = std::clamp(selected < previous ? selected : selected - (capacity - 1),0.0,last);
                    prop(list,"scrollPosition",num(scroll)); ++scrolls; bindWindow();
                }
            } else if (key == "getTextFormat") {
                const auto& props = objects.at(a[1]).properties;
                if (props.count("_fontSize")) {
                    out = make(8); prop(out,"size",props.at("_fontSize"));
                    prop(out,"align",props.count("_textAlign") ? props.at("_textAlign") : str("left"));
                }
            } else if (key == "setTextFormat" || key == "setNewTextFormat") {
                assert(a[5] == 1);
                const double requested = number(object(raw(a[4])).properties.at("size"));
                auto& props = objects.at(a[1]).properties;
                if (key == "setNewTextFormat") props["_defaultFontSize"] = num(requested);
                else {
                    const double previous = number(props.at("_fontSize"));
                    props["_measuredTextWidth"] = num(number(props.at("_measuredTextWidth")) * requested / previous);
                    if (props.count("_measuredTextHeight"))
                        props["_measuredTextHeight"] = num(number(props.at("_measuredTextHeight")) * requested / previous);
                    props["_fontSize"] = num(requested); ++textFormats;
                }
            } else if (key == "updateSpinnerArrows") {
                // UI_Options.Options uses the measured value width, not the
                // TextField box, to lay out the original left spinner arrow.
                auto& props = objects.at(a[1]).properties;
                const auto arrow = props.at("spinnerLeftArrow"), field = props.at("currentValue");
                const double x = number(props.at("initialSpinnerLeftArrowX")) -
                    number(object(field).properties.at("_measuredTextWidth")) * .94;
                prop(arrow,"_x",num(x)); object(arrow).bounds.left = x;
                object(arrow).bounds.right = x + 10;
            } else if (key == "getBounds") {
                const auto& o = objects.at(a[1]); auto r = o.bounds; out = make(8);
                if (o.transformBounds && raw(a[4])[4] != a[1]) {
                    const auto sx = number(o.properties.at("_xscale")) / 100;
                    const auto sy = number(o.properties.at("_yscale")) / 100;
                    const auto& parent = object(o.properties.at("_parent"));
                    const auto x = number(o.properties.at("_x")) + parent.tx;
                    const auto y = number(o.properties.at("_y")) + parent.ty;
                    r = {r.left * sx + x,r.top * sy + y,r.right * sx + x,r.bottom * sy + y};
                }
                prop(out,"xMin",num(r.left)); prop(out,"yMin",num(r.top)); prop(out,"xMax",num(r.right)); prop(out,"yMax",num(r.bottom));
            } else if (key == "localToGlobal") {
                const auto p = raw(a[4]); const auto& from = objects.at(a[1]);
                auto& props = object(p).properties;
                props["x"] = num(number(props.at("x")) + from.tx);
                props["y"] = num(number(props.at("y")) + from.ty);
            } else if (key == "globalToLocal") {
                const auto p = raw(a[4]); const auto& from = objects.at(a[1]);
                auto& props = object(p).properties;
                props["x"] = num(number(props.at("x")) - from.tx);
                props["y"] = num(number(props.at("y")) - from.ty);
            } else if (key == "handleClick") {
                assert(a[5] == 2 && number(raw(a[4])) == 0 && number(raw(a[4] + 32)) == 0);
                ++clicks; if (changeInClick) currentScene = false;
            } else if (key == "__set__scrollPosition") {
                prop(list,"scrollPosition",num(number(raw(a[4])))); ++scrolls;
                bindWindow();
            } else if (key == "__set__selectedIndex") {
                prop(list,"selectedIndex",num(number(raw(a[4])))); ++selections;
            } else if (key == "__set__value") {
                const auto requested = number(raw(a[4]));
                const auto& props = object(slider).properties;
                const double low = number(props.at("minimum")), high = number(props.at("maximum"));
                const double step = number(props.at("snapInterval"));
                prop(slider,"value",num(std::clamp(std::round(std::clamp(requested,low,high) / step) * step,low,high)));
            } else if (key == "onSliderChange") {
                assert(a[5] == 0); ++sliderNotifications;
                prop(sliderData,"currentValue",object(slider).properties.at("value"));
            } else if(key=="onSpinnerChange") {
                assert(a[5]==0);++spinnerNotifications;
            } else assert(false);
            output(a[2],out);
        } else assert(false);
        return true;
    }
};
inline void touchPage(Vm& vm) {
    // The device's real custom page has18data rows and11visible renderers;
    // the first11 contain6numeric sliders and5two-choice spinners.
    vm.model.movieClass = "OptionsGFxMovie";
    vm.object(vm.data).array.clear(); vm.object(vm.renderers).array.clear(); vm.model.eventIds.clear();
    vm.object(vm.list).bounds = {100,100,1100,650};
    vm.prop(vm.list,"totalRenderers",num(11));
    for (unsigned i = 0; i < 18; ++i) {
        const bool slider = i < 4 || i == 9 || i == 10;
        auto data = vm.make(8);
        vm.prop(data,"unselectable",boolean(false)); vm.prop(data,"label",vm.str("Touch setting"));
        vm.prop(data,"rendererType",vm.str(slider ? "slider" : i < 11 ? "spinner" : "default"));
        vm.object(vm.data).array.push_back(data); vm.model.eventIds.push_back(107 + static_cast<int>(i));
        if (i >= 11) continue;
        const double y = 100 + i * 50.0;
        auto renderer = vm.make(10), inner = vm.make(10), field = vm.make(10);
        vm.visible(renderer,vm.list); vm.visible(inner,renderer); vm.visible(field,inner);
        vm.prop(renderer,"owner",vm.list); vm.prop(renderer,"data",data); vm.prop(renderer,"index",num(i));
        vm.prop(renderer,"disabled",boolean(false)); vm.prop(renderer,"inner",inner); vm.prop(inner,"data",data);
        vm.object(renderer).bounds = {100,y,1100,y + 45}; vm.object(renderer).ty = y;
        vm.object(inner).ty = y;
        vm.prop(inner,"currentValue",field); vm.prop(field,"_x",num(950)); vm.prop(field,"_y",num(0));
        vm.prop(field,"_width",num(100)); vm.prop(field,"_height",num(40));
        vm.prop(field,"_rotation",num(0)); vm.prop(field,"_xscale",num(100)); vm.prop(field,"_yscale",num(100));
        if (slider) {
            auto control = vm.make(10), track = vm.make(10), thumb = vm.make(10);
            vm.visible(control,inner); vm.visible(track,control); vm.visible(thumb,control);
            vm.prop(inner,"slider",control); vm.prop(control,"track",track); vm.prop(control,"thumb",thumb); vm.prop(control,"disabled",boolean(false));
            vm.object(thumb).bounds = {0,10,15,30};
            vm.prop(control,"minimum",num(0)); vm.prop(control,"maximum",num(10)); vm.prop(control,"snapInterval",num(1));
            vm.prop(control,"value",num(5)); vm.prop(control,"__width",num(500));
            vm.prop(control,"offsetLeft",num(10)); vm.prop(control,"offsetRight",num(10));
            vm.object(control).bounds = {500,y + 10,1000,y + 35}; vm.object(control).tx = 500; vm.object(control).ty = y;
            vm.object(track).bounds = {10,10,490,30};
            vm.prop(data,"minValue",num(0)); vm.prop(data,"maxValue",num(10));
            vm.prop(data,"sliderIncrement",num(1)); vm.prop(data,"currentValue",num(5));
            if (!i) { vm.slider = control; vm.sliderData = data; vm.inner = inner; }
        } else {
            auto choices = vm.make(9); vm.object(choices).array = {vm.str("Off"),vm.str("On")};
            vm.prop(data,"spinnerChoices",choices); vm.prop(data,"currentValue",num(0));
        }
        vm.object(vm.renderers).array.push_back(renderer);
    }
}
}
