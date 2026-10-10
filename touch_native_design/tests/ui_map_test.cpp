#include "gfx_widget_fixture.h"
#include "bl2_touch_ui_transactions.h"
#include <cstdio>
using namespace bl2::touch;
using namespace widget_fixture;

struct MapVm final : GfxTransport {
    Vm vm;
    Raw parent{},field{},map{},mask{};
    std::uint32_t first=30,count=6;
    unsigned transportCalls=0;
    double lineAlignment=147;
    std::vector<unsigned> indices;
    bool charMissing=false,changeInCharacter=false,changeInProjection=false;
    bool inventoryMode=false;
    explicit MapVm(const char* fixture) : vm(fixture) {
        parent=vm.make(10);field=vm.make(10);
        map=vm.make(10);mask=vm.make(10);vm.visible(map,vm.root);vm.visible(mask,map);
        vm.object(mask).bounds={-366.9,-270,366.9,270};mouse(0,0);
        vm.visible(parent,vm.root);vm.visible(field,parent);
        vm.prop(vm.root,"tooltips",parent);vm.prop(parent,"tooltips",field);
        vm.object(parent).tx=500.45;vm.object(parent).ty=614.95;
        vm.prop(field,"_x",num(-378.55));vm.prop(field,"_y",num(-15.95));
        text("(A) Waypoint  (LT/RT) Zoom  (Y) Legend  (B) Close");
    }
    void text(const char* caption) {
        vm.prop(field,"text",vm.str(caption));
        const std::string s=caption;first=static_cast<std::uint32_t>(s.find("Legend"));count=6;
    }
    WidgetPoint footer(NativeFooterFamily family) {
        inventoryMode=true;
        if(family==NativeFooterFamily::TwoPanel || family==NativeFooterFamily::Vending) {
            const auto main=vm.make(10);vm.visible(main,vm.root);vm.prop(vm.root,"main",main);
            vm.visible(parent,main);vm.prop(main,"tooltipsWrapper",parent);
            vm.object(parent).tx=738-73.55;vm.object(parent).ty=21.9+630.95;
            vm.prop(field,"_x",num(-392.1));vm.prop(field,"_y",num(-15.45));
        } else {
            assert(family==NativeFooterFamily::Trading);
            vm.object(parent).tx=647.45;vm.object(parent).ty=647.45;
            vm.prop(field,"_x",num(-391.55));vm.prop(field,"_y",num(-12.1));
        }
        return {vm.object(parent).tx+number(vm.object(field).properties.at("_x"))+100+lineAlignment,
            vm.object(parent).ty+number(vm.object(field).properties.at("_y"))+12.5};
    }
    void mouse(double x,double y) {vm.prop(map,"_xmouse",num(x));vm.prop(map,"_ymouse",num(y));}
    static WidgetPoint projected(double x,double y) {
        // Controlled inherited3D plane: perspective, translation and skew.
        // The production code must call the native projection, not infer a
        // linear fullscreen or rectangular-mask coordinate ratio.
        const double w=1+.0003*x+.0002*y;
        return {(640+x+.15*y)/w,(360+.05*x+y)/w};
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& a,std::uint32_t& result) override {
        ++transportCalls;
        if (pc==base+GfxSession::kGetVariable) {
            assert(vm.text(a[2])=="_root");vm.output(a[1],vm.root);result=1;return true;
        }
        if (pc==invoke && (vm.text(a[3])=="getCharBoundaries" || vm.text(a[3])=="getExactCharBoundaries")) {
            assert(a[1]==field[4] && a[5]==1);
            const auto i=static_cast<unsigned>(number(vm.raw(a[4])));indices.push_back(i);
            assert(inventoryMode || (i>=first && i<first+count));
            Raw out{};
            if (!charMissing) {
                // Original helper02863FC8 loads line+0x0C before glyph
                // advances. Exact-ink helper0286424C omits it entirely.
                const double aligned=vm.text(a[3])=="getCharBoundaries"?lineAlignment:0;
                out=vm.make(8);vm.prop(out,"x",num((inventoryMode?100+i*8:253+(i-first)*8)+aligned));vm.prop(out,"y",num(5));
                vm.prop(out,"width",num(8));vm.prop(out,"height",num(15));
            }
            vm.output(a[2],out);result=1;if(changeInCharacter) vm.currentScene=false;return true;
        }
        if(pc==invoke && a[1]==map[4] && vm.text(a[3])=="localToGlobal") {
            assert(a[5]==1);const auto p=vm.raw(a[4]);auto& props=vm.object(p).properties;
            const auto q=projected(number(props.at("x")),number(props.at("y")));
            props["x"]=num(q.x);props["y"]=num(q.y);
            vm.output(a[2],{});result=1;if(changeInProjection)vm.currentScene=false;return true;
        }
        return vm.call(pc,a,result);
    }
    MapLegendTarget hit(GfxSession& s,float x=530.f/1280,float y=610.f/720,MapLegendObservation* observation=nullptr) {
        return CaptureBl2MapLegend(s,2560,1440,x,y,observation);
    }
    MapCursorTarget cursor(GfxSession& s,double localX=120,double localY=-90) {
        vm.write(wrapper+0x3C,map.data(),32);vm.write(wrapper+0x7C,mask.data(),32);
        const auto m=s.pin(wrapper+0x3C),k=s.pin(wrapper+0x7C);const auto q=projected(localX,localY);
        const auto out=CaptureBl2MapCursor(s,m,k,2560,1440,static_cast<float>(q.x/1280),static_cast<float>(q.y/720));
        s.release(k);s.release(m);return out;
    }
};

int main(int argc,char** argv) {
    assert(argc==2);
    for(const auto family:{NativeFooterFamily::TwoPanel,NativeFooterFamily::Trading}) {
        const char* caption=family==NativeFooterFamily::TwoPanel?"Take   Inspect   Trash/Favourite   Sort   Cancel":"Sort   Inspect   Close";
        const std::vector<std::pair<const char*,NativeFooterCommand>> commands=family==NativeFooterFamily::TwoPanel?
            std::vector<std::pair<const char*,NativeFooterCommand>>{{"Take",NativeFooterCommand::Accept},{"Inspect",NativeFooterCommand::Inspect},
                {"Trash/Favourite",NativeFooterCommand::Mark},{"Sort",NativeFooterCommand::Sort},{"Cancel",NativeFooterCommand::Close}}:
            std::vector<std::pair<const char*,NativeFooterCommand>>{{"Sort",NativeFooterCommand::Sort},{"Inspect",NativeFooterCommand::Inspect},
                {"Close",NativeFooterCommand::Close}};
        for(const auto& [word,command]:commands) {
            MapVm vm(argv[1]);const auto origin=vm.footer(family);vm.text(caption);
            const auto start=std::string(caption).find(word),count=std::strlen(word);
            const auto x=static_cast<float>((origin.x+(start+count*.5)*8)/1280),y=static_cast<float>(origin.y/720);
            {GfxSession down(vm,base,movie,scratch1,512,false),up(vm,base,movie,scratch2,512,false);
             const auto a=CaptureBl2NativeFooter(down,family,2560,1440,x,y),b=CaptureBl2NativeFooter(up,family,2560,1440,x,y);
             assert(a && b && a.command==command && a.first==start && a.count==count && SameBl2MapLegend(a,b));
             assert(down.calls()<512 && up.calls()<512 && vm.vm.clicks==0);
             vm.vm.prop(vm.parent,"_parent",vm.vm.root);
             if(family==NativeFooterFamily::Trading) vm.vm.prop(vm.field,"_parent",vm.vm.root);
             up.beginInputBoundary();assert(!CaptureBl2NativeFooter(up,family,2560,1440,x,y));}
            assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
        }
    }
    for(const auto* accept:{"Purchase","Sell","Buyback"}) {
        MapVm vm(argv[1]);const auto origin=vm.footer(NativeFooterFamily::Vending);
        const std::string caption=std::string(accept)+"   Sell Trash   Inspect   Sort   Cancel";vm.text(caption.c_str());
        {GfxSession s(vm,base,movie,scratch1,512,false);
         const auto a=CaptureBl2NativeFooter(s,NativeFooterFamily::Vending,2560,1440,
             static_cast<float>((origin.x+16)/1280),static_cast<float>(origin.y/720));
         assert(a && a.command==NativeFooterCommand::Accept && a.count==std::strlen(accept));
         s.beginInputBoundary();const auto first=caption.find("Sell Trash");
         const auto b=CaptureBl2NativeFooter(s,NativeFooterFamily::Vending,2560,1440,
             static_cast<float>((origin.x+(first+5)*8)/1280),static_cast<float>(origin.y/720));
         assert(b && b.command==NativeFooterCommand::SellTrash && b.count==10 && b.first==first && vm.vm.clicks==0);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(const auto [accept,count]:std::array<std::pair<const char*,unsigned>,3>{{{"Приобрести",10},{"Продать",7},{"Выкупить",8}}}) {
        MapVm vm(argv[1]);const auto origin=vm.footer(NativeFooterFamily::Vending);
        const std::string caption=std::string(accept)+"   Продать весь хлам   Осмотреть   Отсортировать   Отмена";vm.text(caption.c_str());
        {GfxSession s(vm,base,movie,scratch1,512,false);
         const auto a=CaptureBl2NativeFooter(s,NativeFooterFamily::Vending,2560,1440,
             static_cast<float>((origin.x+16)/1280),static_cast<float>(origin.y/720));
         assert(a && a.command==NativeFooterCommand::Accept && a.first==0 && a.count==count);
         s.beginInputBoundary();const auto first=count+3;
         const auto b=CaptureBl2NativeFooter(s,NativeFooterFamily::Vending,2560,1440,
             static_cast<float>((origin.x+(first+8.5)*8)/1280),static_cast<float>(origin.y/720));
         assert(b && b.command==NativeFooterCommand::SellTrash && b.count==17 && b.first==first && vm.vm.clicks==0);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    {
        MapVm vm(argv[1]);const auto origin=vm.footer(NativeFooterFamily::TwoPanel);
        const char* caption="\xF0\x9F\x8E\xAE  Осмотреть   Хлам/любимое   Отсортировать   Отмена";
        vm.text(caption);
        const std::pair<unsigned,NativeFooterCommand> commands[]={{4,NativeFooterCommand::Inspect},{16,NativeFooterCommand::Mark},
            {31,NativeFooterCommand::Sort},{47,NativeFooterCommand::Close}};
        for(const auto [first,command]:commands) {
            GfxSession s(vm,base,movie,scratch1,512,false);
            const auto t=CaptureBl2NativeFooter(s,NativeFooterFamily::TwoPanel,2560,1440,
                static_cast<float>((origin.x+(first+2)*8)/1280),static_cast<float>(origin.y/720));
            assert(t && t.first==first && t.command==command && vm.vm.clicks==0);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    {
        const char* caption="Select/Compare   Drop   Close   Inspect   Trash/Favourite   Sort";
        const std::pair<const char*,NativeFooterCommand> commands[]={{"Select/Compare",NativeFooterCommand::Select},
            {"Drop",NativeFooterCommand::Drop},{"Close",NativeFooterCommand::Close},{"Inspect",NativeFooterCommand::Inspect},
            {"Trash/Favourite",NativeFooterCommand::Mark},{"Sort",NativeFooterCommand::Sort}};
        for(const auto& [word,command]:commands) {
            MapVm vm(argv[1]);vm.inventoryMode=true;vm.text(caption);
            const auto start=std::string(caption).find(word),count=std::strlen(word);
            const auto x=static_cast<float>((368.9+(start+count*.5)*8)/1280);
            {
                GfxSession down(vm,base,movie,scratch1,512,false),up(vm,base,movie,scratch2,512,false);
                const auto a=CaptureBl2NativeFooter(down,NativeFooterFamily::StatusInventory,2560,1440,x,611.5f/720),
                    b=CaptureBl2NativeFooter(up,NativeFooterFamily::StatusInventory,2560,1440,x,611.5f/720);
                assert(a && b && a.command==command && a.first==start && a.count==count && SameBl2MapLegend(a,b));
                assert(down.good() && down.calls()<512 && up.good() && vm.vm.clicks==0);
                UiTransactions intent;intent.begin(8,80,true,false,2560,1440);
                assert(!intent.pointer({1,PointerPhase::Down,x,611.5f/720,8,0,100}));
                assert(!intent.pointer({1,PointerPhase::Cancel,x,611.5f/720,8,0,101}));
                assert(!intent.pointer({1,PointerPhase::Up,x,611.5f/720,8,0,102}));
                vm.text("Inspect Close");up.beginInputBoundary();
                const auto changed=CaptureBl2NativeFooter(up,NativeFooterFamily::StatusInventory,2560,1440,x,611.5f/720);
                assert(!SameBl2MapLegend(a,changed)); // A delayed caption change cannot commit the old key.
            }
            assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
        }
    }
    for(unsigned negative=0;negative<7;++negative) {
        MapVm vm(argv[1]);vm.inventoryMode=true;vm.text("Inspect   Trash/Favourite");
        if(negative==0) vm.text("Inspect Inspect");
        if(negative==1) vm.text("Sell Trash"); // Never map a sale hint to favourite/mark.
        if(negative==2) vm.vm.prop(vm.field,"_parent",vm.vm.root);
        if(negative==3) vm.vm.prop(vm.field,"_visible",boolean(false));
        if(negative==4) vm.charMissing=true;
        if(negative==5) vm.changeInCharacter=true;
        {
            GfxSession s(vm,base,movie,scratch1,negative==6?32u:512u,false);
            assert(!CaptureBl2NativeFooter(s,NativeFooterFamily::StatusInventory,2560,1440,397.f/1280,611.5f/720));
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    for(unsigned negative=0;negative<7;++negative) {
        MapVm vm(argv[1]);vm.vm.prop(vm.vm.root,"tooltips",vm.field);vm.vm.visible(vm.field,vm.vm.root);
        vm.vm.prop(vm.vm.root,"_currentframe",num(2));vm.vm.prop(vm.field,"_name",vm.vm.str("tooltips"));
        vm.vm.prop(vm.field,"_x",num(376.95));vm.vm.prop(vm.field,"_y",num(430.6));
        vm.vm.prop(vm.field,"_width",num(531));vm.vm.prop(vm.field,"_height",num(37.35));
        vm.text("(A) OK");
        if(negative==1) vm.vm.prop(vm.vm.root,"_currentframe",num(1));
        if(negative==2) vm.vm.prop(vm.field,"_width",num(529));
        if(negative==3) vm.vm.prop(vm.field,"_name",vm.vm.str("other"));
        if(negative==4) vm.vm.prop(vm.field,"_parent",vm.parent);
        if(negative==5) vm.vm.prop(vm.field,"text",vm.vm.str(""));
        {
            GfxSession s(vm,base,movie,scratch1,512,false);
            const auto t=CaptureBl2NativeFooter(s,NativeFooterFamily::Training,2560,1440,
                negative==6?.05f:640.f/1280,447.f/720);
            assert(bool(t)==(negative==0));if(t) assert(t.command==NativeFooterCommand::TrainingAccept);
            assert(vm.indices.empty() && vm.vm.clicks==0); // Genuine fixed field box; no guessed word or click.
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    {
        MapVm vm(argv[1]);vm.lineAlignment=0;
        GfxSession s(vm,base,movie,scratch1,512,false);
        assert(!vm.hit(s)); // The actual centred word is outside unaligned exact-ink rectangles.
        vm.lineAlignment=147;s.beginInputBoundary();MapLegendObservation observation;
        assert(vm.hit(s,530.f/1280,610.f/720,&observation) && observation.character.left>=400);
    }
    {
        MapVm vm(argv[1]);
        GfxSession down(vm,base,movie,scratch1,512,false),up(vm,base,movie,scratch2,512,false);
        const auto a=vm.hit(down),b=vm.hit(up);
        assert(a && b && SameBl2MapLegend(a,b) && a.count==6 && a.first==vm.first);
        assert(a.bounds.left<530.f/1280 && a.bounds.right>530.f/1280 && vm.indices.size()==12);
        assert(vm.vm.clicks==0 && vm.vm.sliderNotifications==0); // Capturing never activates the footer.
        UiTransactions intent;intent.begin(8,80,true,false,2560,1440);
        const auto event=[&](PointerPhase phase,float x=530.f/1280) {return intent.pointer({1,phase,x,610.f/720,8,0,100});};
        assert(!event(PointerPhase::Down));assert(event(PointerPhase::Up));
        assert(!event(PointerPhase::Down));assert(!event(PointerPhase::Cancel));assert(!event(PointerPhase::Up));
        vm.text("(A) Different Waypoint  (Y) Legend  (B) Close");up.beginInputBoundary();
        const auto changed=vm.hit(up);assert(changed && !SameBl2MapLegend(a,changed));
    }
    {
        MapVm vm(argv[1]);
        // Native AS indices are UTF-16, even though GFx::Value strings are UTF-8.
        // One supplementary glyph consumes two units; each Russian letter one.
        const char* caption="\xF0\x9F\x8E\xAE \xD0\x9A\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0  \xD0\x9B\xD0\xB5\xD0\xB3\xD0\xB5\xD0\xBD\xD0\xB4\xD0\xB0  Close";
        vm.vm.prop(vm.field,"text",vm.vm.str(caption));vm.first=10;vm.count=7;
        GfxSession s(vm,base,movie,scratch1,512,false);const auto t=vm.hit(s);
        assert(t && t.first==10 && t.count==7 && vm.indices.front()==10 && vm.indices.back()==16);
    }
    for (unsigned negative=0;negative<9;++negative) {
        MapVm vm(argv[1]);
        if(negative==0) vm.vm.prop(vm.field,"_visible",boolean(false));
        if(negative==1) vm.vm.prop(vm.parent,"_alpha",num(0));
        if(negative==2) vm.vm.prop(vm.field,"_parent",vm.vm.root);
        if(negative==3) vm.text("(Y) Legend Legend (B) Close");
        if(negative==4) vm.text("Legendary weapon");
        if(negative==5) vm.charMissing=true;
        if(negative==6) vm.changeInCharacter=true;
        if(negative==7) vm.vm.prop(vm.field,"_rotation",num(15));
        {GfxSession s(vm,base,movie,scratch1,negative==8 ? 32u : 512u,false);assert(!vm.hit(s));}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    {
        MapVm vm(argv[1]);GfxSession s(vm,base,movie,scratch1,512,false);
        assert(!vm.hit(s,.1f,.1f)); // Other footer words and the map itself cannot become Legend.
    }
    {
        // The optional diagnostic must neither query extra characters nor
        // change exact caption bounds, managed cleanup or original actions.
        for(unsigned fault=0;fault<11;++fault) {
            MapVm plain(argv[1]),observed(argv[1]);
            const auto change=[&](MapVm& vm) {
                if(fault==1)vm.vm.prop(vm.field,"_visible",boolean(false));
                if(fault==2)vm.vm.prop(vm.field,"_parent",vm.vm.root);
                if(fault==3)vm.text("(Y) Legend Legend (B) Close");
                if(fault==4)vm.text("Legendary weapon");
                if(fault==5)vm.charMissing=true;
                if(fault==6)vm.changeInCharacter=true;
                if(fault==7)vm.vm.prop(vm.field,"_rotation",num(15));
                if(fault==10)vm.text("(A) Waypoint (B) Close");
            };
            change(plain);change(observed);
            {
                const auto budget=fault==8?32u:512u;
                GfxSession a(plain,base,movie,scratch1,budget,false),b(observed,base,movie,scratch1,budget,false);
                MapLegendObservation report;
                const auto x=fault==9?.1f:530.f/1280,y=fault==9?.1f:610.f/720;
                const auto expected=plain.hit(a,x,y),actual=observed.hit(b,x,y,&report);
                assert(bool(expected)==bool(actual));if(expected)assert(SameBl2MapLegend(expected,actual));
                assert(a.calls()==b.calls() && a.bytesRead()==b.bytesRead() && plain.transportCalls==observed.transportCalls);
                assert(plain.indices==observed.indices && std::string(report.reason)!="unobserved");
                if(!fault) assert(actual && report.hit && report.projected && report.measured==6 && report.first==plain.first && report.count==6);
                if(fault==5)assert(report.rectType==0 && report.measured==0 && std::string(report.reason)=="legend-character-rectangle");
                if(fault==9)assert(!report.hit && report.projected && report.measured==6 && std::string(report.reason)=="legend-outside");
            }
            assert(plain.transportCalls==observed.transportCalls && plain.vm.clicks==0 && observed.vm.clicks==0);
            assert(plain.vm.refs.empty() && observed.vm.refs.empty() && plain.vm.movieRefs==1 && observed.vm.movieRefs==1);
            assert(!plain.vm.abandoned && !observed.vm.abandoned);
        }
    }
    {
        MapVm vm(argv[1]);GfxSession s(vm,base,movie,scratch1,192,false);
        // HandleEvent queues MouseMove: its immediate native getters still
        // expose the old position. It must never become the requested waypoint.
        const auto pending=vm.cursor(s);assert(!pending.valid && pending.mousePending);
        const auto q=MapVm::projected(120,-90);UiMapDeferredPoint intent;
        intent.store({static_cast<float>(q.x/1280),static_cast<float>(q.y/720),0},7,70,1000);
        vm.mouse(120,-90);s.beginInputBoundary();const auto next=intent.take(7,70,1016,true);
        assert(next && !intent.take(7,70,1017,true));
        const auto current=vm.cursor(s);assert(current.valid && !current.mousePending);
        assert(current.local.x==120 && current.local.y==-90 && vm.vm.clicks==0);
        vm.mouse(450,0);s.beginInputBoundary();const auto outside=vm.cursor(s,450,0);
        assert(!outside.valid && !outside.mousePending); // Same projected point outside actual local mask.
    }
    for(unsigned negative=0;negative<6;++negative) {
        MapVm vm(argv[1]);vm.mouse(120,-90);
        if(negative==0)vm.vm.prop(vm.mask,"_parent",vm.vm.root);
        if(negative==1)vm.vm.prop(vm.map,"_visible",boolean(false));
        if(negative==2)vm.vm.prop(vm.mask,"_alpha",num(0));
        if(negative==3)vm.vm.object(vm.mask).bounds={0,0,0,0};
        if(negative==4)vm.changeInProjection=true;
        {GfxSession s(vm,base,movie,scratch1,negative==5 ? 16u : 192u,false);assert(!vm.cursor(s).valid);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    std::puts("Native Map footer/UTF-16/11 observed/plain controls/3D inverse/queued mouse/ownership/cancel/budget controls PASS");
}
