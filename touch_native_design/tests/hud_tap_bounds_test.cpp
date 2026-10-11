#include "gfx_widget_fixture.h"
#include "bl2_touch_gameplay_sink.h"
#include <iostream>
#include <limits>
using namespace bl2::touch;
namespace {
// UI_HUD p1 children for the protocol 5 tap targets, with the authored
// placements from the shipped HUD SwfMovie (Startup.xxx export 42641).
struct TapVm final : GfxTransport {
    widget_fixture::Vm vm;
    widget_fixture::Raw p1{},character{},cooldown{},icon{},xpbar{},challenges{},inner{},background{};
    unsigned projections=0,calls=0,stalePoint=0;
    explicit TapVm(const char* fixture) : vm(fixture) {
        p1=vm.make(10);vm.visible(p1,vm.root);place(p1,216.05,30.45,100);
        character=vm.make(10);vm.visible(character,p1);place(character,-113.4,535.6,103.5);
        cooldown=vm.make(10);vm.visible(cooldown,character);place(cooldown,-2.75,-2.9,100);
        icon=vm.make(10);vm.visible(icon,cooldown);place(icon,16.7,17.95,100);
        // loadMovie replaces the placeholder's content, never icon's own placement.
        vm.prop(icon,"_width",widget_fixture::num(64));vm.prop(icon,"_height",widget_fixture::num(64));
        vm.prop(character,"cooldown",cooldown);vm.prop(cooldown,"icon",icon);
        xpbar=vm.make(10);vm.visible(xpbar,p1);place(xpbar,422.9,608.75,100);
        vm.prop(xpbar,"_width",widget_fixture::num(401));vm.prop(xpbar,"_height",widget_fixture::num(27));
        challenges=vm.make(10);vm.visible(challenges,p1);place(challenges,-120,459.1,100);
        vm.prop(challenges,"_currentframe",widget_fixture::num(13));
        inner=vm.make(10);vm.visible(inner,challenges);place(inner,0,49.9,100);
        background=vm.make(10);vm.visible(background,inner);place(background,-16.75,-27,100);
        vm.prop(background,"_width",widget_fixture::num(254));vm.prop(background,"_height",widget_fixture::num(53.5));
        vm.prop(challenges,"inner",inner);vm.prop(inner,"bkgd",background);
        for (const auto& clip:{character,cooldown,xpbar,challenges,inner,background,icon})
            vm.object(clip).bounds={-100000,-100000,100000,100000}; // getBounds must never be the target.
    }
    void place(const widget_fixture::Raw& clip,double x,double y,double scale) {
        vm.prop(clip,"_x",widget_fixture::num(x));vm.prop(clip,"_y",widget_fixture::num(y));
        vm.prop(clip,"_xscale",widget_fixture::num(scale));vm.prop(clip,"_yscale",widget_fixture::num(scale));
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {
        ++calls;
        if (pc==widget_fixture::invoke && vm.text(args[3])=="getBounds") assert(false);
        if (pc==widget_fixture::invoke && vm.text(args[3])=="localToGlobal") {
            ++projections;
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
            return true;
        }
        return vm.call(pc,args,result);
    }
};
using Observer=bool (*)(GfxSession&,GfxHandle,GfxRect&);
bool observe(TapVm& vm,const widget_fixture::Raw& clip,Observer observer,GfxRect& out,unsigned budget=256) {
    vm.vm.write(widget_fixture::wrapper+0x3C,clip.data(),32);
    GfxSession session(vm,widget_fixture::base,widget_fixture::movie,widget_fixture::scratch1,budget);
    const auto handle=session.pin(widget_fixture::wrapper+0x3C);
    const bool okay=observer(session,handle,out);
    session.release(handle);session.close();assert(session.cleanupOkay());
    assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    return okay;
}
void near(double actual,double expected) {
    if (std::abs(actual-expected)>=1e-6) std::cerr<<"near "<<actual<<" != "<<expected<<'\n';
    assert(std::abs(actual-expected)<1e-6);
}
}
int main(int argc,char** argv) {
    assert(argc==2);
    {
        // Skill icon: the authored circle in cooldown space, through the
        // character's 1.035 scale and p1's placement on the 16:9 frame.
        TapVm vm(argv[1]);GfxRect out;
        assert(observe(vm,vm.character,ObserveBl2SkillIcon,out) && vm.projections==4);
        near(out.left,216.05-113.4+1.035*(-2.75-2.55));near(out.right,216.05-113.4+1.035*(-2.75+37.3));
        near(out.top,30.45+535.6+1.035*(-2.9-1.55));near(out.bottom,30.45+535.6+1.035*(-2.9+38.15));
    }
    for (unsigned bad=0;bad<9;++bad) {
        TapVm vm(argv[1]);GfxRect out{1,2,3,4};
        if (bad==0) vm.vm.prop(vm.character,"_visible",widget_fixture::boolean(false));
        if (bad==1) vm.vm.object(vm.character).properties.erase("cooldown"); // "_none" and vehicle frames.
        if (bad==2) vm.vm.prop(vm.cooldown,"_alpha",widget_fixture::num(0));
        if (bad==3) vm.vm.prop(vm.icon,"_x",widget_fixture::num(17));
        if (bad==4) vm.vm.prop(vm.icon,"_xscale",widget_fixture::num(150));
        if (bad==5) vm.vm.prop(vm.icon,"_rotation",widget_fixture::num(.5));
        if (bad==6) vm.vm.object(vm.cooldown).properties.erase("icon");
        if (bad==7) vm.stalePoint=2;
        assert(!observe(vm,vm.character,ObserveBl2SkillIcon,out,bad==8 ? 6u : 256u) && out.left==0 && out.right==0);
        if (bad<7) assert(!vm.projections);
    }
    for (const double scale:{100.0,125.0,80.0}) {
        // XP bar: the persistent background, whatever the HUD element scale.
        TapVm vm(argv[1]);vm.place(vm.xpbar,422.9,608.75,scale);
        vm.vm.prop(vm.xpbar,"_width",widget_fixture::num(401*scale/100));
        vm.vm.prop(vm.xpbar,"_height",widget_fixture::num(28.1*scale/100)); // The "go" flash reaches y=-8.
        GfxRect out;
        assert(observe(vm,vm.xpbar,ObserveBl2ExperienceBar,out));
        near(out.left,216.05+422.9-200.5*scale/100);near(out.right,216.05+422.9+200.5*scale/100);
        near(out.top,30.45+608.75-6.9*scale/100);near(out.bottom,30.45+608.75+20.1*scale/100);
    }
    for (unsigned bad=0;bad<5;++bad) {
        TapVm vm(argv[1]);GfxRect out{1,2,3,4};
        if (bad==0) vm.vm.prop(vm.xpbar,"_width",widget_fixture::num(398));
        if (bad==1) vm.vm.prop(vm.xpbar,"_height",widget_fixture::num(40));
        if (bad==2) vm.vm.prop(vm.xpbar,"_rotation",widget_fixture::num(90));
        if (bad==3) vm.vm.prop(vm.xpbar,"_visible",widget_fixture::boolean(false));
        if (bad==4) vm.vm.prop(vm.xpbar,"_width",widget_fixture::num(std::numeric_limits<double>::quiet_NaN()));
        assert(!observe(vm,vm.xpbar,ObserveBl2ExperienceBar,out) && out.left==0 && !vm.projections);
    }
    {
        // Challenge notice at rest (frame 13) and through frames 8..12.
        for (const double frame:{8.0,10.0,13.0}) {
            TapVm vm(argv[1]);vm.vm.prop(vm.challenges,"_currentframe",widget_fixture::num(frame));GfxRect out;
            assert(observe(vm,vm.challenges,ObserveBl2ChallengeNotice,out));
            const double x=216.05-120-16.75,y=30.45+459.1+49.9-27;
            near(out.left,x);near(out.right,x+254);near(out.top,y-26);near(out.bottom,y+27.5);
        }
        // A long caption: the subtext script set bkgd._width, i.e. _xscale.
        for (const double scale:{180.0,62.5}) {
            TapVm vm(argv[1]);vm.vm.prop(vm.background,"_xscale",widget_fixture::num(scale));
            vm.vm.prop(vm.background,"_width",widget_fixture::num(254*scale/100));GfxRect out;
            assert(observe(vm,vm.challenges,ObserveBl2ChallengeNotice,out));
            const double x=216.05-120-16.75;
            near(out.left,x);near(out.right,x+254*scale/100);
        }
    }
    for (const double frame:{0.0,1.0,2.0,7.0,14.0,24.0,12.5,std::numeric_limits<double>::quiet_NaN()}) {
        // "off", sliding in, sliding out or malformed: no target.
        TapVm vm(argv[1]);vm.vm.prop(vm.challenges,"_currentframe",widget_fixture::num(frame));GfxRect out{1,2,3,4};
        assert(!observe(vm,vm.challenges,ObserveBl2ChallengeNotice,out) && out.left==0 && !vm.projections);
    }
    for (unsigned bad=0;bad<7;++bad) {
        TapVm vm(argv[1]);GfxRect out{1,2,3,4};
        if (bad==0) vm.vm.prop(vm.inner,"_visible",widget_fixture::boolean(false));
        if (bad==1) vm.vm.prop(vm.background,"_x",widget_fixture::num(0));
        if (bad==2) vm.vm.prop(vm.background,"_height",widget_fixture::num(80));
        if (bad==3) vm.vm.object(vm.inner).properties.erase("bkgd");
        if (bad==4) vm.vm.prop(vm.challenges,"_alpha",widget_fixture::num(0));
        if (bad==5) vm.vm.prop(vm.background,"_width",widget_fixture::num(300)); // Not the authored art.
        if (bad==6) vm.vm.prop(vm.background,"_yscale",widget_fixture::num(150));
        assert(!observe(vm,vm.challenges,ObserveBl2ChallengeNotice,out) && out.left==0 && !vm.projections);
    }
    {
        // Weapon slot word: QuickSelectSlot 1..4 with EWeaponType + 1.
        using Slots=std::vector<WeaponSlotEntry>;
        const Slots three{WeaponSlotEntry{1,3,false},WeaponSlotEntry{2,1,true},WeaponSlotEntry{4,6,false}};
        assert(Bl2WeaponSlotsWord(three)==(kWeaponSlotsKnown|0xBu|(2u<<8)|(3u<<16)|(1u<<20)|(6u<<28)));
        assert(Bl2WeaponSlotsWord(Slots{})==kWeaponSlotsKnown);
        assert(Bl2WeaponSlotsWord(Slots{WeaponSlotEntry{1,0,false},WeaponSlotEntry{3,0,false}})==(kWeaponSlotsKnown|0x5u));
        const std::vector<Slots> rejected{Slots{WeaponSlotEntry{0,1,false}},Slots{WeaponSlotEntry{5,1,false}},
            Slots{WeaponSlotEntry{1,7,false}},Slots{WeaponSlotEntry{2,1,false},WeaponSlotEntry{2,3,false}},
            Slots{WeaponSlotEntry{1,1,true},WeaponSlotEntry{2,2,true}}};
        for (const auto& bad:rejected) assert(Bl2WeaponSlotsWord(bad)==0);
    }
    std::cout<<"PASS: skill icon circle via cooldown, XP bar background at 80/100/125 scale, challenge notice frames 8-13 and stretched backgrounds\n";
    std::cout<<"PASS: hidden/_none/moved/scaled/stale/budget guards, sliding and off frames, getBounds never used\n";
    std::cout<<"PASS: weapon slot word with types, equipped slot, duplicates and range rejection\n";
}
