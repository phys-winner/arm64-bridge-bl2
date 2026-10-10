#include "gfx_widget_fixture.h"
#include "bl2_touch_gameplay_sink.h"
#include <iostream>
#include <limits>
using namespace bl2::touch;
namespace {
struct HintVm final : GfxTransport {
    widget_fixture::Vm vm;
    widget_fixture::Raw tools{},desc{};
    unsigned projections=0,boundsCalls=0,stalePoint=0,changedFramePoint=0,calls=0;
    unsigned depthReads=0,replaceAfter=0;
    bool missingDepth=false;
    explicit HintVm(const char* fixture) : vm(fixture) {
        tools=vm.make(10);vm.visible(tools,vm.root);
        vm.prop(tools,"_currentframe",widget_fixture::num(2));
        vm.prop(tools,"_x",widget_fixture::num(100));vm.prop(tools,"_y",widget_fixture::num(180));
        // Whole tools bounds include other frames/prototypes. They must never
        // become a hit region, even when much larger than the visible caption.
        vm.object(tools).bounds={-10000,-10000,10000,10000};
        desc=vm.make(8);vm.visible(desc,tools);
        // Actual EditText572 has both instanceName and VariableName="desc".
        // A named GetMember therefore returns the bound STRING, while the
        // original display-list getter still returns the native field.
        vm.prop(tools,"desc",vm.str("Press X to open"));
        vm.prop(desc,"_name",vm.str("desc"));vm.prop(desc,"_depth",widget_fixture::num(3));
        vm.prop(desc,"_x",widget_fixture::num(37.8));vm.prop(desc,"_y",widget_fixture::num(5.35));
        vm.prop(desc,"_width",widget_fixture::num(351.2));vm.prop(desc,"_height",widget_fixture::num(30.65));
        vm.prop(desc,"text",vm.str("Press X to open"));vm.write(widget_fixture::wrapper+0x3C,tools.data(),32);
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {
        ++calls;
        if (pc==widget_fixture::invoke && vm.text(args[3])=="getBounds") ++boundsCalls;
        if (pc==widget_fixture::invoke && vm.text(args[3])=="getInstanceAtDepth") {
            assert(args[1]==tools[4] && args[5]==1);
            // Match the ORIGINAL native callback, not the requested helper
            // argument: SWF depth3 is addressed with AS2 depth-16381. Passing
            // positive3 instead looks for dynamic depth16387 and returns NULL.
            const auto argument=widget_fixture::number(vm.raw(args[4]));
            assert(std::isfinite(argument) && argument==std::floor(argument));
            const auto nativeDepth=static_cast<int>(argument)+0x4000;
            ++depthReads;auto field=desc;
            if (missingDepth || nativeDepth!=3) field={};
            else if (replaceAfter && depthReads>replaceAfter) field=vm.make(8);
            vm.output(args[2],field);result=1;return true;
        }
        if (pc==widget_fixture::invoke && vm.text(args[3])=="getDepth") {
            assert(args[1]==desc[4] && args[5]==0);
            vm.output(args[2],widget_fixture::num(widget_fixture::number(vm.object(desc).properties.at("_depth"))-0x4000));
            result=1;return true;
        }
        if (pc==widget_fixture::invoke && vm.text(args[3])=="localToGlobal") {
            assert(args[1]==tools[4] && args[5]==1);++projections;
            const auto point=vm.raw(args[4]);auto& fields=vm.object(point).properties;
            double x=widget_fixture::number(fields.at("x")),y=widget_fixture::number(fields.at("y"));
            auto clip=args[1];
            for (unsigned depth=0;clip && depth<16;++depth) {
                const auto& values=vm.objects.at(clip).properties;
                x=x*widget_fixture::number(values.at("_xscale"))/100+widget_fixture::number(values.at("_x"));
                y=y*widget_fixture::number(values.at("_yscale"))/100+widget_fixture::number(values.at("_y"));
                clip=values.count("_parent") ? values.at("_parent")[4] : 0;
            }
            fields["x"]=widget_fixture::num(x);fields["y"]=widget_fixture::num(y);
            vm.output(args[2],{});result=1;
            if (stalePoint==projections) vm.currentScene=false;
            if (changedFramePoint==projections) vm.prop(tools,"_currentframe",widget_fixture::num(14));
            return true;
        }
        return vm.call(pc,args,result);
    }
};
bool observe(HintVm& vm,GfxRect& out,unsigned budget=192,UseHintObservation* detail=nullptr) {
    GfxSession session(vm,widget_fixture::base,widget_fixture::movie,widget_fixture::scratch1,budget);
    const auto tools=session.pin(widget_fixture::wrapper+0x3C);
    const bool okay=ObserveBl2UseHint(session,tools,out,detail);
    session.release(tools);session.close();assert(session.cleanupOkay());
    assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned && !vm.boundsCalls);
    return okay;
}
void near(double actual,double expected) {assert(std::abs(actual-expected)<1e-6);}
}
int main(int argc,char** argv) {
    assert(argc==2);
    {
        // Reproduce the native15 failure through the actual GfxSession route:
        // the authored placement is unchanged, only the AS2 argument differs.
        HintVm vm(argv[1]);
        GfxSession session(vm,widget_fixture::base,widget_fixture::movie,widget_fixture::scratch1,192);
        const auto tools=session.pin(widget_fixture::wrapper+0x3C),wrong=session.number(3);
        const auto absent=session.invoke(tools,"getInstanceAtDepth",{wrong});
        assert(session.type(absent)==0);
        const auto staticDepth=session.number(-16381);
        const auto field=session.invoke(tools,"getInstanceAtDepth",{staticDepth});
        GfxIdentity actual;
        assert(session.identity(field,actual) && actual.type==8 && actual.data==vm.desc[4]);
        const auto depth=session.invoke(field,"getDepth");double value=0;
        assert(session.number(depth,value) && value==-16381);
        session.release(depth);session.release(field);session.release(staticDepth);
        session.release(absent);session.release(wrong);session.release(tools);session.close();
        assert(session.cleanupOkay() && vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    {
        HintVm plain(argv[1]),observed(argv[1]);GfxRect a,b;UseHintObservation detail;
        const bool plainOkay=observe(plain,a),observedOkay=observe(observed,b,192,&detail);
        if (!plainOkay || !observedOkay) std::cerr<<"hint admission reason="<<detail.reason<<" frame="<<detail.frame
            <<" type="<<detail.toolsType<<" metrics="<<detail.metrics[2]<<","<<detail.metrics[3]
            <<" projections="<<observed.projections<<" calls="<<observed.calls<<'\n';
        assert(plainOkay && observedOkay);
        // Diagnostic data comes from the same reads/projection, and must not
        // add guest calls or change the admitted native target.
        assert(plain.calls==observed.calls && plain.projections==observed.projections);
        assert(detail.toolsType==10 && detail.descType==8 && detail.tools==detail.parent && detail.depth==-16381 &&
            detail.frame==2 && detail.currentFrame==2 && std::strcmp(detail.reason,"projected")==0);
        assert(plain.depthReads==2 && plain.vm.object(plain.tools).properties.at("desc")[3]==6);
        near(a.left,b.left);near(detail.stage.left,b.left);near(detail.stage.bottom,b.bottom);
        near(detail.metrics[2],351.2);near(detail.local.left,35.8);
    }
    for (const double scale:{1.0,1.25,2.0}) {
        HintVm vm(argv[1]);vm.vm.prop(vm.tools,"_xscale",widget_fixture::num(scale*100));
        vm.vm.prop(vm.tools,"_yscale",widget_fixture::num(scale*100));
        for (const int frame:{2,4,5,6,7,8,9,10,11,12,13,55}) {
            vm.vm.prop(vm.tools,"_currentframe",widget_fixture::num(frame));GfxRect out;
            assert(observe(vm,out));near(out.left,100+35.8*scale);near(out.top,180+3.35*scale);
            near(out.right,100+387*scale);near(out.bottom,180+34*scale);
        }
    }
    for (const double frame:{0.0,1.0,3.0,14.0,15.0,40.0,54.0,56.0,57.0,58.0,59.0,2.5,999.0}) {
        HintVm vm(argv[1]);vm.vm.prop(vm.tools,"_currentframe",widget_fixture::num(frame));GfxRect out{1,2,3,4};
        assert(!observe(vm,out) && !vm.projections && out.left==0 && out.right==0);
    }
    for (unsigned bad=0;bad<14;++bad) {
        HintVm vm(argv[1]);
        if (bad==0) vm.vm.prop(vm.tools,"_visible",widget_fixture::boolean(false));
        if (bad==1) vm.vm.prop(vm.desc,"_alpha",widget_fixture::num(0));
        if (bad==2) vm.vm.prop(vm.desc,"text",vm.vm.str(" \t\n"));
        if (bad==3) vm.vm.prop(vm.desc,"_parent",vm.vm.root);
        if (bad==4) vm.vm.prop(vm.desc,"_width",widget_fixture::num(351));
        if (bad==5) vm.vm.prop(vm.desc,"_rotation",widget_fixture::num(.01));
        if (bad==6) vm.vm.prop(vm.desc,"_x",widget_fixture::num(std::numeric_limits<double>::quiet_NaN()));
        if (bad==7) vm.stalePoint=2;
        if (bad==8) vm.changedFramePoint=3;
        if (bad==10) vm.missingDepth=true;
        if (bad==11) vm.vm.prop(vm.desc,"_name",vm.vm.str("trade"));
        if (bad==12) vm.vm.prop(vm.desc,"_depth",widget_fixture::num(2));
        if (bad==13) vm.replaceAfter=1;
        GfxRect out{1,2,3,4};UseHintObservation detail;
        assert(!observe(vm,out,bad==9 ? 4u : 192u,&detail) && out.left==0 && out.right==0);
        if (bad==3) assert(std::strcmp(detail.reason,"desc-parent-text-visibility")==0 && detail.tools!=detail.parent);
        if (bad==4) assert(std::strcmp(detail.reason,"desc-shape")==0 && detail.metrics[2]==351 && !vm.projections);
        if (bad==8) assert(std::strcmp(detail.reason,"frame-visibility-recheck")==0 && detail.currentFrame==14);
        if (bad==10) assert(std::strcmp(detail.reason,"desc-display-depth")==0 && !vm.projections);
        if (bad==13) assert(std::strcmp(detail.reason,"desc-display-replaced")==0 && vm.projections==4);
    }
    std::cout<<"PASS: original AS2 +16384/-16384 depth conversion, positive3 NULL regression, authored depth3 bound desc field and 100/125/200 projection\n";
    std::cout<<"PASS: melee/full/cannotUse/trade exclusion, empty/hidden/foreign/changed/stale/budget guards and managed cleanup\n";
}
