#include "gfx_widget_fixture.h"
#include "bl2_touch_widget_registry.h"
#include <iostream>
#include <limits>

using namespace bl2::touch;
namespace {
// The native localToGlobal ABI/math is separately executed against the pinned
// ARM guest in original-localToGlobal-math.json. This fixture checks our managed
// AS argument/output ownership, SELF bounds and Surface/viewport conversion.
struct ProjectionVm final : GfxTransport {
    widget_fixture::Vm vm;
    double scale = 1, rotation = .31;
    unsigned projections = 0, selfBounds = 0;
    unsigned invalidPoint = 0, stalePoint = 0;
    explicit ProjectionVm(const char* fixture) : vm(fixture) {
        vm.write(widget_fixture::wrapper+0x3C,vm.root.data(),32);
    }
    bool read(std::uint32_t p, void* out, std::size_t n) const override { return vm.read(p,out,n); }
    bool write(std::uint32_t p, const void* in, std::size_t n) override { return vm.write(p,in,n); }
    bool executable(std::uint32_t p) const override { return vm.executable(p); }
    bool current() const override { return vm.current(); }
    void abandonScratch() override { vm.abandonScratch(); }
    WidgetPoint fraction(double x, double y) const {
        const double a=std::cos(rotation), b=std::sin(rotation);
        const double worldX=770+scale*(x*a-y*b), worldY=510+scale*(x*b+y*a);
        const double w=1+.0003*scale*x-.0002*scale*y;
        return {worldX/(1280*w),worldY/(720*w)};
    }
    bool call(std::uint32_t pc, const std::vector<std::uint32_t>& args, std::uint32_t& result) override {
        if (pc==widget_fixture::invoke && vm.text(args[3])=="getBounds") {
            assert(args[5]==1 && vm.raw(args[4])[4]==args[1]); ++selfBounds;
        }
        if (pc==widget_fixture::invoke && vm.text(args[3])=="localToGlobal") {
            assert(args[5]==1); ++projections;
            const auto point=vm.raw(args[4]); auto& props=vm.object(point).properties;
            const auto projected=fraction(widget_fixture::number(props.at("x")),widget_fixture::number(props.at("y")));
            const auto& frame=vm.visibleFrame;
            props["x"]=widget_fixture::num(invalidPoint==projections ? std::numeric_limits<double>::quiet_NaN() :
                frame[0]+projected.x*(frame[2]-frame[0]));
            props["y"]=widget_fixture::num(frame[1]+projected.y*(frame[3]-frame[1]));
            vm.output(args[2],{}); result=1;
            if (stalePoint==projections) vm.currentScene=false;
            return true;
        }
        return vm.call(pc,args,result);
    }
};
struct ObjectivesVm final : GfxTransport {
    ProjectionVm projection;
    widget_fixture::Raw mission{};
    std::array<widget_fixture::Raw,6> modules{},inners{},backgrounds{},health{};
    unsigned projectedPoints=0,invalidPoint=0,stalePoint=0;
    explicit ObjectivesVm(const char* fixture) : projection(fixture) {
        auto& vm=projection.vm;
        mission=vm.make(10);vm.visible(mission,vm.root);
        vm.prop(mission,"_x",widget_fixture::num(1000));vm.prop(mission,"_y",widget_fixture::num(200));
        for (std::size_t i=0;i<6;++i) {
            modules[i]=vm.make(10);vm.visible(modules[i],mission);
            vm.prop(modules[i],"_x",widget_fixture::num(-289));vm.prop(modules[i],"_y",widget_fixture::num(22.0*i));
            vm.prop(mission,"Module"+std::to_string(i+1),modules[i]);
            inners[i]=vm.make(10);vm.visible(inners[i],modules[i]);vm.prop(modules[i],"inner",inners[i]);
            vm.prop(inners[i],"_visible",widget_fixture::boolean(false));
            vm.prop(inners[i],"_currentframe",widget_fixture::num(16));
            backgrounds[i]=vm.make(10);vm.visible(backgrounds[i],inners[i]);
            vm.prop(inners[i],"textbkgd",backgrounds[i]);vm.object(backgrounds[i]).bounds={0,-13,189,13};
            vm.prop(backgrounds[i],"_x",widget_fixture::num(326.45));vm.prop(backgrounds[i],"_y",widget_fixture::num(11.5));
            vm.prop(backgrounds[i],"_xscale",widget_fixture::num(-100));
            health[i]=vm.make(10);vm.visible(health[i],inners[i]);vm.prop(inners[i],"healthBar",health[i]);
            vm.object(health[i]).bounds={-1.5,-1.5,114.55,5.55};
            vm.prop(health[i],"_x",widget_fixture::num(172.35));vm.prop(health[i],"_y",widget_fixture::num(18.25));
        }
        vm.write(widget_fixture::wrapper+0x3C,mission.data(),32);
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return projection.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return projection.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return projection.executable(p);}
    bool current() const override {return projection.current();}
    void abandonScratch() override {projection.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {
        auto& vm=projection.vm;
        if (pc==widget_fixture::invoke && vm.text(args[3])=="localToGlobal") {
            ++projectedPoints;
            const auto point=vm.raw(args[4]);auto& props=vm.object(point).properties;
            double x=widget_fixture::number(props.at("x")),y=widget_fixture::number(props.at("y"));
            auto object=args[1];
            for (unsigned depth=0;object && depth<16;++depth) {
                const auto& p=vm.objects.at(object).properties;
                const double sx=widget_fixture::number(p.at("_xscale"))/100,
                    sy=widget_fixture::number(p.at("_yscale"))/100,
                    angle=widget_fixture::number(p.at("_rotation"))*3.141592653589793/180;
                const double nx=x*sx*std::cos(angle)-y*sy*std::sin(angle)+widget_fixture::number(p.at("_x"));
                y=x*sx*std::sin(angle)+y*sy*std::cos(angle)+widget_fixture::number(p.at("_y"));x=nx;
                object=p.count("_parent") ? p.at("_parent")[4] : 0;
            }
            props["x"]=widget_fixture::num(invalidPoint==projectedPoints ? std::numeric_limits<double>::quiet_NaN() : x);
            props["y"]=widget_fixture::num(y);vm.output(args[2],{});result=1;
            if (stalePoint==projectedPoints) vm.currentScene=false;
            return true;
        }
        return projection.call(pc,args,result);
    }
};
void closeCheck(ProjectionVm& vm) {
    assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
}
void expectNear(double actual, double expected) { assert(std::abs(actual-expected)<2e-6); }
HudObjectiveBounds observeObjectives(ObjectivesVm& vm,unsigned budget=512) {
    HudObjectiveBounds result;
    {
        GfxSession session(vm,widget_fixture::base,widget_fixture::movie,widget_fixture::scratch1,budget);
        const auto mission=session.pin(widget_fixture::wrapper+0x3C);
        result=ObserveBl2HudObjectives(session,mission);
        session.release(mission);session.close();assert(session.cleanupOkay());
    }
    closeCheck(vm.projection);
    return result;
}
void testObjectives(const char* fixture) {
    for (const auto scale:{1.0,1.25,2.0}) {
        ObjectivesVm vm(fixture);
        vm.projection.vm.prop(vm.projection.vm.root,"_xscale",widget_fixture::num(100*scale));
        vm.projection.vm.prop(vm.projection.vm.root,"_yscale",widget_fixture::num(100*scale));
        assert(observeObjectives(vm).state==HudObjectiveState::Empty && !vm.projectedPoints);
        vm.projection.vm.prop(vm.inners[0],"_visible",widget_fixture::boolean(true));
        vm.projection.vm.prop(vm.inners[0],"_currentframe",widget_fixture::num(29));
        vm.projection.vm.prop(vm.backgrounds[0],"_xscale",widget_fixture::num(-160)); // Dynamic text width.
        vm.projection.vm.prop(vm.inners[5],"_visible",widget_fixture::boolean(true));
        vm.projection.vm.prop(vm.inners[5],"_currentframe",widget_fixture::num(57));
        vm.projection.vm.prop(vm.backgrounds[5],"_xscale",widget_fixture::num(-75*100/189.0));
        auto result=observeObjectives(vm);
        assert(result.state==HudObjectiveState::Projected && vm.projectedPoints==16);
        expectNear(result.stage.left,735.05*scale);expectNear(result.stage.right,1037.45*scale);
        expectNear(result.stage.top,198.5*scale);expectNear(result.stage.bottom,334.5*scale);
        // A short health caption must include the native bar protruding left.
        vm.projection.vm.prop(vm.inners[0],"_visible",widget_fixture::boolean(false));
        result=observeObjectives(vm);expectNear(result.stage.left,881.85*scale);
        // Even a hidden whole movie is Empty only after six rows validate.
        vm.projection.vm.prop(vm.mission,"_visible",widget_fixture::boolean(false));
        assert(observeObjectives(vm).state==HudObjectiveState::Empty);
        vm.projection.vm.object(vm.mission).properties.erase("Module6");
        assert(observeObjectives(vm).state==HudObjectiveState::Unknown);
    }
    {
        ObjectivesVm vm(fixture);auto& raw=vm.projection.vm;
        raw.prop(vm.inners[0],"_visible",widget_fixture::boolean(true));
        raw.prop(vm.inners[0],"_currentframe",widget_fixture::num(29));
        raw.prop(vm.backgrounds[0],"_xscale",widget_fixture::num(-75*100/189.0));
        auto result=observeObjectives(vm);expectNear(result.stage.left,772.3);
        assert(result.state==HudObjectiveState::Projected); // Unnamed underline extends beyond short caption.
        raw.prop(vm.inners[0],"_currentframe",widget_fixture::num(39));
        result=observeObjectives(vm);expectNear(result.stage.left,962.45); // Timer replaced the underline.
    }
    for (unsigned mode=0;mode<8;++mode) {
        ObjectivesVm vm(fixture);auto& raw=vm.projection.vm;
        raw.prop(vm.inners[0],"_visible",widget_fixture::boolean(true));
        if (mode==0) raw.object(vm.mission).properties.erase("Module4");
        if (mode==1) raw.object(vm.modules[5]).properties.erase("inner");
        if (mode==2) raw.prop(vm.inners[1],"_alpha",widget_fixture::num(std::numeric_limits<double>::quiet_NaN()));
        if (mode==3) raw.object(vm.inners[0]).properties.erase("textbkgd");
        if (mode==4) vm.invalidPoint=3;
        if (mode==5) vm.stalePoint=2;
        if (mode==6) raw.prop(vm.inners[0],"_currentframe",widget_fixture::num(29.5));
        assert(observeObjectives(vm,mode==7 ? 15u : 512u).state==HudObjectiveState::Unknown);
    }
    std::cout<<"PASS: six actual objective rows, dynamic caption width, frame29 underline/frame57 health, 100/125/200 UI, hidden/unknown distinction, stale/budget ownership\n";
}
}
int main(int argc, char** argv) {
    assert(argc==2);
    testObjectives(argv[1]);
    const GfxRect local{-58,-48,58,48};
    const std::array<std::array<std::uint32_t,6>,4> surfaces{{
        {2560,1440,0,0,2560,1440},{2780,1264,0,0,2780,1264},
        {2424,1080,80,20,2200,1000},{3636,1620,120,30,3300,1500}}};
    for (const auto scale : {1.0,1.25,2.0}) for (const auto& surface : surfaces) {
        ProjectionVm vm(argv[1]); vm.scale=scale;
        vm.vm.visibleFrame={-151.77217f,0,1431.77217f,720};
        vm.vm.object(vm.vm.root).bounds=local;
        const auto original=vm.vm.object(vm.vm.root).properties;
        GfxRect projected;
        {
            GfxSession session(vm,widget_fixture::base,widget_fixture::movie,widget_fixture::scratch1,256);
            const auto clip=session.pin(widget_fixture::wrapper+0x3C);
            assert(session.projectedBounds(clip,projected) && vm.selfBounds==1 && vm.projections==4);
            GfxViewport viewport; std::copy(surface.begin(),surface.end(),viewport.words.begin());
            const GfxRect frame{vm.vm.visibleFrame[0],vm.vm.visibleFrame[1],vm.vm.visibleFrame[2],vm.vm.visibleFrame[3]};
            WidgetPoint a,b;
            assert(NormalizeWidgetPoint(frame,viewport,surface[0],surface[1],{projected.left,projected.top},a));
            assert(NormalizeWidgetPoint(frame,viewport,surface[0],surface[1],{projected.right,projected.bottom},b));
            GfxRect expected{1,1,0,0};
            for (const double x : {local.left,local.right}) for (const double y : {local.top,local.bottom}) {
                const auto p=vm.fraction(x,y);
                expected.left=std::min(expected.left,p.x); expected.top=std::min(expected.top,p.y);
                expected.right=std::max(expected.right,p.x); expected.bottom=std::max(expected.bottom,p.y);
            }
            expectNear(a.x,(surface[2]+expected.left*surface[4])/surface[0]);
            expectNear(a.y,(surface[3]+expected.top*surface[5])/surface[1]);
            expectNear(b.x,(surface[2]+expected.right*surface[4])/surface[0]);
            expectNear(b.y,(surface[3]+expected.bottom*surface[5])/surface[1]);
            assert(vm.vm.propertyWrites==8 && vm.vm.object(vm.vm.root).properties==original);
            viewport.words[0]++; assert(!NormalizeWidgetPoint(frame,viewport,surface[0],surface[1],{0,0},a));
            session.release(clip);session.close();assert(session.cleanupOkay());
        }
        closeCheck(vm);
    }
    for (unsigned mode=0;mode<4;++mode) {
        ProjectionVm vm(argv[1]);vm.vm.object(vm.vm.root).bounds=local;
        if (mode==0) vm.invalidPoint=3;
        if (mode==1) vm.stalePoint=2;
        if (mode==2) vm.vm.object(vm.vm.root).bounds.right=local.left;
        {
            GfxSession session(vm,widget_fixture::base,widget_fixture::movie,widget_fixture::scratch1,mode==3 ? 1u : 256u);
            const auto clip=session.pin(widget_fixture::wrapper+0x3C);GfxRect out{1,2,3,4};
            assert(!session.projectedBounds(clip,out));
            session.release(clip);session.close();assert(session.cleanupOkay());
        }
        closeCheck(vm);
    }
    {
        ProjectionVm vm(argv[1]);
        vm.vm.object(vm.vm.root).bounds={-10000,-10000,10000,10000}; // Animated/prototype bounds are irrelevant.
        {
            GfxSession session(vm,widget_fixture::base,widget_fixture::movie,widget_fixture::scratch1,256);
            const auto clip=session.pin(widget_fixture::wrapper+0x3C);GfxRect out;
            assert(session.projectedRect(clip,{-193.25,-9.05,-4.55,29.5},out));
            assert(!vm.selfBounds && vm.projections==4 && out.left>0 && out.right<1280);
            const auto calls=vm.projections;
            assert(!session.projectedRect(clip,{1,0,1,5},out) && vm.projections==calls);
            session.release(clip);session.close();assert(session.cleanupOkay());
        }
        closeCheck(vm);
    }
    std::cout<<"PASS: SELF bounds, four native AS projected corners, 100/125/200 UI, four Surface/viewport modes, immutable clips and invalid/stale/budget cleanup\n";
}
