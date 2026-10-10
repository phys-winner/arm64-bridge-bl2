#include "gfx_widget_fixture.h"
#include "bl2_touch_ui_transactions.h"
#include <cstdio>
using namespace bl2::touch;
using namespace widget_fixture;

struct CharacterVm final : GfxTransport {
    Vm vm;
    Raw content{}, grid{}, cells{}, menu{}, entries{}, panel{}, scrolling{}, items{}, rows{};
    Raw getterOverride{};
    unsigned previews = 0, commits = 0, transportCalls = 0;
    explicit CharacterVm(const char* fixture) : vm(fixture) {
        content=vm.make(10); grid=vm.make(10); cells=vm.make(9);
        menu=vm.make(10);entries=vm.make(9);panel=vm.make(10);scrolling=vm.make(10);items=vm.make(9);rows=vm.make(9);
        vm.visible(content,vm.root);vm.prop(content,"_root",vm.root);
        vm.visible(grid,content);vm.prop(content,"characterNavigationGrid",grid);vm.prop(grid,"cells",cells);
        vm.visible(menu,content);vm.prop(content,"p1CustomizationMenu",menu);vm.prop(menu,"menuEntries",entries);
        vm.visible(panel,content);vm.prop(content,"p1CustomizationList",panel);
        vm.visible(scrolling,panel);vm.prop(panel,"scrollingList",scrolling);
        vm.prop(scrolling,"_dataProvider",items);vm.prop(scrolling,"renderers",rows);
        vm.prop(scrolling,"disabled",boolean(false));vm.prop(scrolling,"_scrollPosition",num(0));
        // Real WillowStagePlacedScrollingList addProperty installs an empty
        // public getter; the inherited __get__scrollPosition reads its private
        // offset. A numeric public alias would hide the hardware failure.
        vm.prop(scrolling,"scrollPosition",Raw{});
        for (unsigned i=0;i<4;++i) {
            const auto cell=vm.make(10);vm.visible(cell,grid);vm.prop(cell,"_name",vm.str(("cell"+std::to_string(i)).c_str()));
            vm.prop(cell,"characterIndex",num(i));vm.prop(cell,"disabled",boolean(false));
            // Original cell art is96px; placement is live and may be3D projected.
            vm.object(cell).bounds={0,0,96,96};vm.object(cell).tx=100+i*150;vm.object(cell).ty=500;
            vm.object(cells).array.push_back(cell);
            const auto entry=vm.make(10);vm.visible(entry,menu);vm.prop(entry,"index",num(i));vm.prop(entry,"disabled",boolean(false));
            vm.object(entry).bounds={0,0,240,80};vm.object(entry).tx=100;vm.object(entry).ty=20+i*90;
            vm.object(entries).array.push_back(entry);
            const auto label="Customization"+std::to_string(i);
            const auto data=vm.str((label+"####NEW####EQUIPPED").c_str());vm.object(items).array.push_back(data);
            const auto row=vm.make(10);vm.visible(row,scrolling);vm.prop(row,"owner",scrolling);vm.prop(row,"index",num(i));
            vm.prop(row,"data",vm.str(label.c_str()));vm.prop(row,"disabled",boolean(false));
            vm.object(row).bounds={0,0,260,30};vm.object(row).tx=100;vm.object(row).ty=50+i*40;
            vm.object(rows).array.push_back(row);
        }
        vm.write(wrapper+0x3C,content.data(),32);
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {
        ++transportCalls;
        if (pc==invoke) {
            const auto name=vm.text(args[3]);
            if (name=="__get__dataProvider") {
                assert(args[1]==scrolling[4] && args[5]==0);
                vm.output(args[2],getterOverride[3] ? getterOverride : vm.object(scrolling).properties.at("_dataProvider"));result=1;return true;
            }
            if(name=="__get__scrollPosition") {
                assert(args[1]==scrolling[4] && args[5]==0);
                vm.output(args[2],vm.object(scrolling).properties.at("_scrollPosition"));result=1;return true;
            }
            if (name=="SetSelectedEntry" || name=="__set__selectedIndex") {
                assert(args[5]==1);assert(args[1]==menu[4] || args[1]==scrolling[4]);
                ++previews;vm.prop(args[1]==menu[4]?menu:scrolling,"selectedIndex",vm.raw(args[4]));
                vm.output(args[2],{});result=1;return true;
            }
            if (name=="HandleCustomizationSelected") {
                assert(args[1]==panel[4] && args[5]==3);
                assert(number(vm.raw(args[4]))==0 && vm.text(vm.raw(args[4]+32)[4])=="click" &&
                    number(vm.raw(args[4]+64))==1);
                ++commits;vm.output(args[2],{});result=1;return true;
            }
        }
        return vm.call(pc,args,result);
    }
    CharacterWidgetTarget hit(GfxSession& s,int screen,float x,float y,CharacterWidgetObservation* observation=nullptr) {
        const auto clip=s.pin(wrapper+0x3C);
        return CaptureBl2CharacterWidget(s,clip,screen,0,2560,1440,x,y,observation);
    }
};

int main(int argc,char** argv) {
    assert(argc==2);
    {
        CharacterVm vm(argv[1]);
        GfxSession retained(vm,base,movie,scratch1,512,true), fresh(vm,base,movie,scratch2,512,false);
        const auto down=vm.hit(retained,1,550.f/1280,540.f/720);assert(down && down.index==3);
        const auto up=vm.hit(fresh,1,550.f/1280,540.f/720);assert(SameBl2CharacterWidget(down,up));
        assert(!ConfirmBl2CharacterPortrait(down,up,false,3)); // First inactive tap only selects.
        assert(ConfirmBl2CharacterPortrait(down,up,true,3)); // Active portrait confirms with the original handler.
        assert(!ConfirmBl2CharacterPortrait(down,up,true,2));
        assert(!SelectBl2CharacterWidget(retained,down) && !ClickBl2CharacterCustomization(retained,down));
        vm.vm.prop(vm.vm.object(vm.cells).array[3],"characterIndex",num(2));
        fresh.beginInputBoundary();assert(!vm.hit(fresh,1,550.f/1280,540.f/720));
        assert(vm.previews==0 && vm.commits==0);
    }
    {
        CharacterVm vm(argv[1]);
        GfxSession s(vm,base,movie,scratch1,512,true);
        const auto name=vm.hit(s,2,200.f/1280,240.f/720);assert(name && name.index==2 && name.kind==CharacterWidgetKind::Menu);
        assert(SelectBl2CharacterWidget(s,name) && vm.previews==1 && vm.commits==0);
        assert(!ClickBl2CharacterCustomization(s,name)); // Name stays the original native MenuEntryClick/keyboard path.
        vm.vm.prop(vm.menu,"_visible",boolean(false));s.beginInputBoundary();assert(!vm.hit(s,2,200.f/1280,240.f/720));
    }
    {
        CharacterVm vm(argv[1]);
        GfxSession retained(vm,base,movie,scratch1,512,true),fresh(vm,base,movie,scratch2,512,false);
        const auto down=vm.hit(retained,3,200.f/1280,100.f/720);assert(down && down.index==1);
        assert(SelectBl2CharacterWidget(retained,down) && vm.previews==1 && vm.commits==0);
        const auto up=vm.hit(fresh,3,200.f/1280,100.f/720);assert(SameBl2CharacterWidget(down,up));
        assert(ClickBl2CharacterCustomization(retained,down) && vm.commits==1);
        const auto row=vm.vm.object(vm.rows).array[1];vm.vm.prop(row,"data",vm.vm.str("Replaced label"));
        fresh.beginInputBoundary();assert(!vm.hit(fresh,3,200.f/1280,100.f/720));
        vm.vm.prop(row,"data",vm.vm.str("Customization1"));
        // Native NEW/EQUIPPED changes leave the same displayed caption. They
        // still replace the semantic item and must invalidate a captured UP.
        vm.vm.object(vm.items).array[1]=vm.vm.str("Customization1####EQUIPPED");
        fresh.beginInputBoundary();const auto replaced=vm.hit(fresh,3,200.f/1280,100.f/720);
        assert(replaced && !SameBl2CharacterWidget(down,replaced)); // Same renderer/array, changed data must cancel UP.
        vm.vm.prop(vm.scrolling,"_dataProvider",vm.vm.make(9));
        fresh.beginInputBoundary();assert(!vm.hit(fresh,3,200.f/1280,100.f/720));
        vm.vm.prop(vm.scrolling,"_dataProvider",vm.items);
        vm.vm.prop(row,"owner",vm.grid);fresh.beginInputBoundary();assert(!vm.hit(fresh,3,200.f/1280,100.f/720));
    }
    {
        CharacterVm vm(argv[1]);
        GfxSession s(vm,base,movie,scratch1,512,true);
        assert(!vm.hit(s,0,.2f,.2f));assert(!vm.hit(s,4,.2f,.2f));assert(!vm.hit(s,1,-1,.2f));
        vm.vm.prop(vm.content,"_alpha",num(0));assert(!vm.hit(s,1,.2f,.75f));
    }
    {
        CharacterVm vm(argv[1]);
        { GfxSession s(vm,base,movie,scratch1,512,true);assert(vm.hit(s,1,.12f,.75f));vm.vm.currentScene=false;assert(!SelectBl2CharacterWidget(s,{})); }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    {
        // Actual Head list: three native items, eight allocated renderers,
        // undefined public scrollPosition and inherited private offset zero.
        CharacterVm vm(argv[1]);
        vm.vm.object(vm.items).array.resize(3);
        while(vm.vm.object(vm.rows).array.size()<8) {
            const auto unused=vm.vm.make(10);vm.vm.visible(unused,vm.scrolling);
            vm.vm.prop(unused,"_visible",boolean(false));vm.vm.object(vm.rows).array.push_back(unused);
        }
        GfxSession s(vm,base,movie,scratch1,512,false);CharacterWidgetObservation report;
        auto hit=vm.hit(s,3,200.f/1280,100.f/720,&report);
        assert(hit && hit.index==1 && report.count==3 && report.rendered==8 && report.scroll==0 && std::string(report.reason)=="hit");
        // Native getter is authoritative even if an unrelated/public value is
        // materialized. Invalid private offset must never fall back to it.
        vm.vm.prop(vm.scrolling,"scrollPosition",num(7));s.beginInputBoundary();
        assert(vm.hit(s,3,200.f/1280,100.f/720));
        for(const auto offset:std::array<Raw,5>{Raw{},num(-1),num(.5),num(1),num(NAN)}) {
            vm.vm.prop(vm.scrolling,"_scrollPosition",offset);vm.vm.prop(vm.scrolling,"scrollPosition",num(0));
            s.beginInputBoundary();assert(!vm.hit(s,3,200.f/1280,100.f/720,&report));
            assert(std::string(report.reason)=="scroll-value" || std::string(report.reason)=="scroll-range");
        }
        vm.vm.prop(vm.scrolling,"_scrollPosition",num(0));s.beginInputBoundary();
        assert(vm.hit(s,3,200.f/1280,100.f/720));
        assert(vm.previews==0 && vm.commits==0);
    }
    {
        // Diagnostics are a copy of the real capture, including early guards.
        // Compare complete managed transport/cleanup counts with diagnostics
        // absent; observing cannot change the target, activate it or add calls.
        for(unsigned fault=0;fault<18;++fault) {
            CharacterVm plain(argv[1]),observed(argv[1]);
            const auto change=[&](CharacterVm& vm) {
                const auto row=vm.vm.object(vm.rows).array[1];
                switch(fault) {
                case 1: vm.vm.prop(vm.scrolling,"disabled",boolean(true));break;
                case 2: while(vm.vm.object(vm.rows).array.size()<9) vm.vm.object(vm.rows).array.push_back(row);break;
                case 3: vm.vm.prop(vm.scrolling,"_scrollPosition",num(1));break;
                case 4: vm.vm.prop(row,"owner",vm.grid);break;
                case 5: vm.vm.prop(row,"data",vm.vm.str("Foreign label"));break;
                case 6: vm.vm.prop(row,"index",num(3));break;
                case 7: vm.vm.prop(row,"_visible",boolean(false));break;
                case 8: vm.vm.prop(row,"disabled",boolean(true));break;
                case 9: vm.vm.object(row).ty=500;break;
                case 10: vm.vm.object(vm.items).array.clear();break;
                case 11: vm.vm.prop(vm.scrolling,"_dataProvider",vm.vm.str("Not an array"));break;
                case 12: vm.vm.prop(vm.panel,"_parent",vm.grid);break;
                case 13: vm.vm.prop(vm.content,"_visible",boolean(false));break;
                case 14: break; // Same strict guest-call budget on both captures.
                case 15: vm.vm.object(vm.items).array[1]=num(42);break;
                case 16: vm.getterOverride=vm.vm.make(9);vm.vm.object(vm.getterOverride).array=vm.vm.object(vm.items).array;break;
                case 17: vm.vm.prop(vm.scrolling,"disabled",num(0));break;
                default: break;
                }
            };
            change(plain);change(observed);
            CharacterWidgetObservation report;
            {
                const auto budget=fault==14?30u:512u;
                GfxSession a(plain,base,movie,scratch1,budget,false),b(observed,base,movie,scratch1,budget,false);
                const auto before=plain.transportCalls,otherBefore=observed.transportCalls;
                const auto expected=plain.hit(a,3,200.f/1280,100.f/720);
                const auto actual=observed.hit(b,3,200.f/1280,100.f/720,&report);
                assert(bool(expected)==bool(actual));
                if(expected) assert(SameBl2CharacterWidget(expected,actual));
                assert(a.calls()==b.calls() && a.bytesRead()==b.bytesRead());
                assert(plain.transportCalls-before==observed.transportCalls-otherBefore);
                assert(a.cleanupOkay() && b.cleanupOkay());
                if(fault==0) {
                    assert(actual && std::string(report.reason)=="hit" && report.count==4 && report.rendered==4 && report.scroll==0);
                    assert(report.committed && report.disabledKnown && !report.disabled && report.rowCount==2);
                    const auto& hit=report.rows[1];
                    assert(hit.hit && hit.owner && hit.shown && hit.projected && hit.normalized && hit.index==1);
                    assert(hit.label=="Customization1####NEW####EQUIPPED" && hit.displayed=="Customization1");
                } else {
                    assert(!actual && std::string(report.reason)!="unobserved");
                    const char* reason=fault==1||fault==17 ? "container-disabled" : fault==2 ? "renderer-count" :
                        fault==3 ? "scroll-range" : fault==10 ? "provider-count" : fault==11 ? "provider-array" :
                        fault==12 ? "panel-parent" : fault==13 ? "content-root" : fault==14 ? "session-failed" :
                        fault==16 ? "provider-commit" : "no-row-hit";
                    // A budget may exhaust before the row loop; the session
                    // reason remains authoritative even for an earlier stage.
                    if(fault==14) assert(!b.good());
                    else assert(std::string(report.reason)==reason);
                    if(fault==4||fault==5||fault==6||fault==7||fault==8||fault==9||fault==15) {
                        assert(report.rowCount==4);
                        const char* rowReason=fault==4?"widget-owner":fault==5||fault==15?"widget-label":
                            fault==6?"widget-index":fault==7?"widget-hidden":fault==8?"widget-disabled":"widget-outside";
                        assert(std::string(report.rows[1].reason)==rowReason);
                    }
                }
            }
            assert(plain.transportCalls==observed.transportCalls && plain.previews==0 && observed.previews==0 && plain.commits==0 && observed.commits==0);
            assert(plain.vm.refs.empty() && observed.vm.refs.empty() && plain.vm.movieRefs==1 && observed.vm.movieRefs==1);
            assert(!plain.vm.abandoned && !observed.vm.abandoned);
        }
    }
    {
        // Use the actual shared transaction state that the native sink uses:
        // selection is allowed on DOWN, activation only after its matching UP.
        CharacterVm vm(argv[1]);
        GfxSession retained(vm,base,movie,scratch1,512,true),fresh(vm,base,movie,scratch2,512,false);
        UiTransactions input;
        const auto down=vm.hit(retained,1,.12f,.75f);assert(down && down.index==0);
        input.begin(7,123,true,false,2560,1440);
        const auto event=[&](int id,PointerPhase phase,float x=.12f,float y=.75f) {
            return input.pointer({id,phase,x,y,7,0,100});
        };
        assert(!event(1,PointerPhase::Down));
        assert(input.ownsPointer(1));
        event(1,PointerPhase::Cancel);assert(!event(1,PointerPhase::Up));
        event(1,PointerPhase::Down);event(2,PointerPhase::Down);
        assert(!event(1,PointerPhase::Up) && !event(2,PointerPhase::Up));
        event(1,PointerPhase::Down);event(1,PointerPhase::Move,.3f,.75f);
        assert(!event(1,PointerPhase::Up));
        event(1,PointerPhase::Down);input.begin(7,124,true,false,2560,1440);
        assert(!event(1,PointerPhase::Up)); // Original movie replaced within the same epoch.
        event(1,PointerPhase::Down);const auto tap=event(1,PointerPhase::Up);assert(tap);
        const auto up=vm.hit(fresh,1,tap->x,tap->y);
        assert(!ConfirmBl2CharacterPortrait(down,up,false,0));
        assert(ConfirmBl2CharacterPortrait(down,up,true,0));
        assert(vm.previews==0 && vm.commits==0); // No synthetic Confirm/Enter on the first portrait tap.
    }
    std::puts("PASS: original CharacterSelect capture/confirm, 18 observed-vs-unobserved call/guard controls, label/owner/visibility replacement and managed cleanup");
}
