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
constexpr std::uint32_t wrapper = 0x23004000, scratch1 = 0x23005000, scratch2 = 0x23006000;
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
};
struct Vm final : GfxTransport {
    std::unordered_map<std::uint32_t,std::uint8_t> memory;
    std::unordered_map<std::uint32_t,Object> objects;
    std::unordered_map<std::uint32_t,std::uint32_t> refs;
    std::uint32_t next = 0x24000000;
    bool currentScene = true, abandoned = false, changeInClick = false;
    int movieRefs = 1, clicks = 0, sliderNotifications = 0, scrolls = 0;
    Raw root{}, list{}, data{}, renderers{}, slider{}, inner{}, sliderData{};
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
        object(list).bounds = {100,100,1100,500};
        for (unsigned i = 0; i < 4; ++i) {
            auto item = make(8);
            prop(item,"unselectable",boolean(false)); prop(item,"label",str(i == 1 ? "Sound" : "Menu"));
            prop(item,"rendererType",str(i == 1 ? "slider" : "default"));
            object(data).array.push_back(item);
            if (i >= 2) continue;
            auto renderer = make(10); visible(renderer,list);
            prop(renderer,"owner",list); prop(renderer,"data",item); prop(renderer,"index",num(i));
            prop(renderer,"disabled",boolean(false));
            object(renderer).bounds = {100,100 + 200.0 * i,1100,250 + 200.0 * i};
            object(renderers).array.push_back(renderer);
            if (i == 1) {
                sliderData = item; inner = make(10); slider = make(10);
                auto track = make(10); visible(inner,renderer); visible(slider,inner); visible(track,slider);
                prop(renderer,"inner",inner); prop(inner,"slider",slider); prop(inner,"data",item);
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
            else if (o.properties.count(key)) output(a[3],o.properties.at(key));
            else { output(a[3],{}); result = 0; }
        } else if (pc == element) {
            auto& values = objects.at(a[1]).array;
            if (a[2] < values.size()) output(a[3],values[a[2]]);
            else { output(a[3],{}); result = 0; }
        } else if (pc == set) { objects.at(a[1]).properties[text(a[2])] = raw(a[3]); }
        else if (pc == frame) { const float r[]{0,0,1280,720}; write(a[0],r,16); }
        else if (pc == viewport) { const std::array<std::uint32_t,13> v{2560,1440,0,0,2560,1440}; write(a[1],v.data(),52); }
        else if (pc == invoke) {
            const auto key = text(a[3]); Raw out{};
            if (key == "getBounds") {
                const auto r = objects.at(a[1]).bounds; out = make(8);
                prop(out,"xMin",num(r.left)); prop(out,"yMin",num(r.top)); prop(out,"xMax",num(r.right)); prop(out,"yMax",num(r.bottom));
            } else if (key == "localToGlobal") {
                const auto p = raw(a[4]); const auto& from = objects.at(a[1]);
                auto& props = object(p).properties;
                props["x"] = num(number(props.at("x")) + from.tx);
                props["y"] = num(number(props.at("y")) + from.ty);
            } else if (key == "handleClick") {
                assert(a[5] == 2 && number(raw(a[4])) == 0 && number(raw(a[4] + 32)) == 0);
                ++clicks; if (changeInClick) currentScene = false;
            } else if (key == "__set__scrollPosition") {
                prop(list,"scrollPosition",num(number(raw(a[4])))); ++scrolls;
            } else if (key == "__set__value") {
                const auto requested = number(raw(a[4]));
                prop(slider,"value",num(std::round(std::clamp(requested,0.0,10.0))));
            } else if (key == "onSliderChange") {
                assert(a[5] == 0); ++sliderNotifications;
                prop(sliderData,"currentValue",object(slider).properties.at("value"));
            } else assert(false);
            output(a[2],out);
        } else assert(false);
        return true;
    }
};
}
