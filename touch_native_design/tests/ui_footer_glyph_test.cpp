#include "gfx_widget_fixture.h"
#include "bl2_touch_settings.h"
#include <cstdio>
using namespace bl2::touch;
using namespace widget_fixture;

struct FooterVm final : GfxTransport {
    Vm vm;
    Raw parent{},field{};
    std::vector<unsigned> widths,indices;
    std::vector<WidgetPoint> positions;
    bool missingCharacter=false,changeInCharacter=false,missingBounds=true;
    bool nativeTransform=false;
    std::vector<std::string> transformWrites;
    unsigned fieldBoundsCalls=0,fieldProjectionCalls=0;
    explicit FooterVm(const char* fixture,const char* path="_root.missions.tooltips") : vm(fixture) {
        auto current=vm.root;const std::string full=path;std::size_t at=6;
        while(at<full.size()) {
            const auto end=full.find('.',at);const auto name=full.substr(at,end-at);
            const auto child=vm.make(10);vm.visible(child,current);vm.prop(current,name,child);
            parent=current;field=child;current=child;
            if(end==std::string::npos) break;at=end+1;
        }
        vm.object(parent).tx=350;vm.object(parent).ty=200;
        vm.prop(field,"_x",num(20));vm.prop(field,"_y",num(400));
        vm.object(field).bounds={-2,-2,598,35};
        vm.object(field).transformBounds=true;
    }
    Raw str(const std::string& text) {
        Raw value{};value[3]=6;value[4]=vm.next;
        vm.next+=static_cast<unsigned>((text.size()+1+255)/256*256);
        vm.write(value[4],text.c_str(),text.size()+1);return value;
    }
    void text(const std::string& caption,const std::string& html) {
        vm.prop(field,"text",str(caption));vm.prop(field,"htmlText",str(html));widths.clear();positions.clear();
        double x=0,y=0;
        for(std::size_t i=0;i<caption.size();) {
            const auto c=static_cast<unsigned char>(caption[i]);
            const unsigned n=c<128?1u:((c&0xE0)==0xC0?2u:((c&0xF0)==0xE0?3u:4u));
            const bool newline=c=='\r' || c=='\n';
            positions.push_back({x,y});widths.push_back(newline?0u:(n==3 && caption.compare(i,n,"\xEF\xBF\xBC")==0?26u:8u));
            x+=widths.back();if(newline) {x=0;y+=24;}
            if(n==4) {positions.push_back({x,y});widths.push_back(8);x+=8;}i+=n;
        }
    }
    WidgetPoint point(unsigned index) {
        const auto& p=vm.object(field).properties;
        return {(vm.object(parent).tx+number(p.at("_x"))+(positions.at(index).x+widths.at(index)*.5)*number(p.at("_xscale"))/100)/1280,
                (vm.object(parent).ty+number(p.at("_y"))+(positions.at(index).y+10)*number(p.at("_yscale"))/100)/720};
    }
    MapLegendTarget hit(GfxSession& s,unsigned index=0,MapLegendObservation* observation=nullptr) {
        const auto p=point(index);
        return CaptureBl2NativeFooter(s,NativeFooterFamily::Generic,2560,1440,static_cast<float>(p.x),static_cast<float>(p.y),observation);
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& a,std::uint32_t& result) override {
        if(nativeTransform && pc==set && a[1]==field[4]) {
            const auto key=vm.text(a[2]);
            if(key=="_x" || key=="_y" || key=="_xscale" || key=="_yscale") {
                const auto requested=number(vm.raw(a[3]));
                const auto previous=number(vm.object(field).properties.at(key));
                transformWrites.push_back(key);
                const bool okay=vm.call(pc,a,result);auto& props=vm.object(field).properties;
                // Model reproducing seed432: position setters quantize to
                // twips and scale changes shift a two-pixel local border.
                if(key=="_x" || key=="_y") props[key]=num(std::floor(requested*20+1e-8)/20);
                else {
                    const auto axis=key=="_xscale"?"_x":"_y";
                    props[axis]=num(std::floor((number(props.at(axis))-2*(requested-previous)/100)*20+1e-8)/20);
                }
                return okay;
            }
        }
        if(pc==invoke && a[1]==field[4] && vm.text(a[3])=="getBounds") {
            ++fieldBoundsCalls;if(missingBounds) {vm.output(a[2],{});result=1;return true;}
        }
        if(pc==get && a[1]==field[4] && vm.text(a[2])=="_bl2TouchFooterOriginal" &&
            !vm.object(field).properties.count("_bl2TouchFooterOriginal")) {
            // AVM can successfully return undefined for an absent dynamic
            // property. A valid Value handle is not an existing snapshot.
            vm.output(a[3],{});result=1;return true;
        }
        if(pc==base+GfxSession::kGetVariable) {
            const auto path=vm.text(a[2]);auto value=vm.root;
            if(path!="_root") for(std::size_t at=6;at<path.size();) {
                const auto end=path.find('.',at);const auto name=path.substr(at,end-at);
                const auto& props=vm.object(value).properties;const auto found=props.find(name);
                if(found==props.end()) {value={};break;}value=found->second;
                if(end==std::string::npos) break;at=end+1;
            }
            vm.output(a[1],value);result=value[3]?1u:0u;return true;
        }
        if(pc==invoke && a[1]==field[4] && vm.text(a[3])=="getCharBoundaries") {
            const auto index=static_cast<unsigned>(number(vm.raw(a[4])));indices.push_back(index);Raw rect{};
            if(!missingCharacter && index<widths.size()) {
                rect=vm.make(8);vm.prop(rect,"x",num(positions[index].x));vm.prop(rect,"y",num(positions[index].y));
                vm.prop(rect,"width",num(widths[index]));vm.prop(rect,"height",num(20));
            }
            vm.output(a[2],rect);result=1;if(changeInCharacter) vm.currentScene=false;return true;
        }
        if(pc==invoke && a[1]==field[4] && vm.text(a[3])=="localToGlobal") {
            ++fieldProjectionCalls;
            const auto point=vm.raw(a[4]);auto& p=vm.object(point).properties;const auto& t=vm.object(field).properties;
            p["x"]=num(vm.object(parent).tx+number(t.at("_x"))+number(p.at("x"))*number(t.at("_xscale"))/100);
            p["y"]=num(vm.object(parent).ty+number(t.at("_y"))+number(p.at("y"))*number(t.at("_yscale"))/100);
            vm.output(a[2],{});result=1;return true;
        }
        return vm.call(pc,a,result);
    }
};

int main(int argc,char** argv) {
    assert(argc==2);touchSettingsState.initialized=true;touchSettingsState.values[17]=100;
    const std::string icon="\xEF\xBF\xBC";
    {
        // Original Android Quest CompleteButtonBarGamepad aliases: A reward,
        // B exit and separate LT/RT description-scroll glyphs, in Russian.
        FooterVm vm(argv[1]);
        vm.text(icon+" Получить награду     "+icon+" Выход     "+icon+" / "+icon+" Пролистать",
            "<P><FONT FACE=\"WillowBody\"><IMG src='android_A' vspace='-3'> Получить награду     "
            "<IMG SRC=\"android_B\" vspace=\"-3\"> Выход     <IMG src='android_LT'> / "
            "<IMG src='android_RT'> Пролистать</FONT></P>");
        const auto original=vm.vm.object(vm.field).properties;
        const std::pair<unsigned,NativeFooterCommand> commands[]={{0,NativeFooterCommand::ButtonA},{23,NativeFooterCommand::ButtonB},
            {35,NativeFooterCommand::ButtonLT},{39,NativeFooterCommand::ButtonRT}};
        for(const auto [index,command]:commands) {
            {GfxSession s(vm,base,movie,scratch1,512,false);MapLegendObservation observation;const auto target=vm.hit(s,index,&observation);
             if(!target) std::fprintf(stderr,"Quest index=%u rejected=%s calls=%u measured=%u\n",index,observation.reason,s.calls(),observation.measured);
             assert(target && target.command==command && target.first==index && target.count && s.good() && s.calls()<512);
             assert(std::find(vm.indices.begin(),vm.indices.end(),index)!=vm.indices.end());}
            assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
        }
        assert(vm.vm.clicks==0 && vm.vm.object(vm.field).properties==original);
    }
    for(const auto* path:{"_root.reward.tooltips","_root.latentRewardOuter.missionReward.tooltips",
        "_root.itemInspect.tooltips.tooltips","_root.teleport.tooltips.tooltips","_root.lobby.tooltips",
        "_root.menu.tooltips","_root.menu.inner.tooltips","_root.menu.networkOptionsPopup.inner.tooltips",
        "_root.menu.playerInfoPopup.inner.tooltips","_root.gearboxAccount.tooltips","_root.serverBrowser.tooltips",
        "_root.marketplace.tooltips1","_root.marketplace.tooltips2"}) {
        FooterVm vm(argv[1],path);vm.text(icon+" Accept","<IMG src='xbox360_A'> Accept");
        {GfxSession s(vm,base,movie,scratch1,512,false);assert(vm.hit(s).command==NativeFooterCommand::ButtonA && s.calls()<512);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(const auto* glyph:{"android_A","xbox360_A","ps3_X"}) {
        FooterVm vm(argv[1]);vm.text(" Accept",std::string("<IMG src='")+glyph+"'>Accept");
        {GfxSession s(vm,base,movie,scratch1,512,false);assert(vm.hit(s).command==NativeFooterCommand::ButtonA);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(unsigned fault=0;fault<8;++fault) {
        FooterVm vm(argv[1]);vm.text(icon+" Accept","<IMG src='android_A'> Accept");
        if(fault==0) vm.text(icon+" Accept","<IMG src='android_LS'> Accept");
        if(fault==1) vm.text(icon+" Accept","<IMG src='android_Dpad'> Accept");
        if(fault==2) vm.text(" Accept","<IMG src='android_A'> Accept"); // omitted IMG is not a proven index
        if(fault==3) vm.vm.prop(vm.parent,"_visible",boolean(false));
        if(fault==4) vm.vm.prop(vm.field,"_parent",vm.vm.list); // unattached candidate has another parent
        if(fault==5) vm.missingCharacter=true;
        if(fault==6) vm.changeInCharacter=true;
        {GfxSession s(vm,base,movie,scratch1,fault==7?48u:512u,false);assert(!vm.hit(s));}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && vm.vm.clicks==0 && !vm.vm.abandoned);
    }
    {
        FooterVm vm(argv[1]);vm.text(icon+" Accept","<IMG src='android_A'> Accept");
        {GfxSession a(vm,base,movie,scratch1,512,false),b(vm,base,movie,scratch2,512,false);
         const auto before=vm.hit(a);vm.text(icon+" Accept","<IMG src='android_B'> Accept");
         const auto after=vm.hit(b);assert(before && after && !SameBl2MapLegend(before,after));}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    {
        // Exact Options htmlText/plain from abb seed429: one ASCII space for
        // each IMG, four authored spaces before it and one caption space.
        // TextField has getCharBoundaries, but neither MovieClip-only method.
        FooterVm vm(argv[1],"_root.menu.tooltips");
        vm.text("      Select      Back",
            "<TEXTFORMAT INDENT=\"0\" LEFTMARGIN=\"0\" RIGHTMARGIN=\"0\" LEADING=\"0\"><P ALIGN=\"RIGHT\">"
            "<FONT FACE=\"$WillowBody\" SIZE=\"15\" COLOR=\"#A4E8F3\" KERNING=\"0\">    "
            "<IMG SRC=\"android_A\" WIDTH=\"20\" HEIGHT=\"20\" VSPACE=\"-3\" ALIGN=\"baseline\"> Select    "
            "<IMG SRC=\"android_B\" WIDTH=\"20\" HEIGHT=\"20\" VSPACE=\"-3\" ALIGN=\"baseline\"> Back</FONT></P></TEXTFORMAT>");
        double width100=0,width130=0;
        for(const unsigned percent:{100u,130u,130u,200u,100u}) {
            touchSettingsState.values[17]=percent;
            {GfxSession s(vm,base,movie,scratch1,512,true);assert(ScaleBl2NativeFooter(s,NativeFooterFamily::Generic) && s.calls()<512);}
            for(const auto [i,command]:{std::pair{4u,NativeFooterCommand::ButtonA},std::pair{16u,NativeFooterCommand::ButtonB}}) {
                GfxSession s(vm,base,movie,scratch1,512,false);const auto target=vm.hit(s,i);
                assert(target && target.command==command && target.first==i && s.calls()<512);
                if(i==4 && percent==100) width100=target.bounds.right-target.bounds.left;
                if(i==4 && percent==130) width130=target.bounds.right-target.bounds.left;
            }
        }
        assert(std::abs(width130-width100*1.3)<1e-7);
        assert(vm.fieldBoundsCalls==0 && vm.fieldProjectionCalls==0);
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && vm.vm.clicks==0);
    }
    {
        // MainMenu's observed CR separator comes from two HTML paragraphs;
        // the final P contributes no trailing CR to native GetText.
        FooterVm vm(argv[1],"_root.menu.inner.tooltips");
        vm.text("      Network Options\r      Select Character      Select      Back",
            "<TEXTFORMAT><P ALIGN=\"RIGHT\"><FONT>    <IMG SRC=\"android_Back\"> Network Options</FONT></P></TEXTFORMAT>"
            "<TEXTFORMAT><P ALIGN=\"RIGHT\"><FONT>    <IMG SRC=\"android_Y\"> Select Character    "
            "<IMG SRC=\"android_A\"> Select    <IMG SRC=\"android_B\"> Back</FONT></P></TEXTFORMAT>");
        const std::pair<unsigned,NativeFooterCommand> commands[]={{4,NativeFooterCommand::ButtonBack},
            {26,NativeFooterCommand::ButtonY},{48,NativeFooterCommand::ButtonA},{60,NativeFooterCommand::ButtonB}};
        touchSettingsState.values[17]=130;
        {GfxSession s(vm,base,movie,scratch1,512,true);assert(ScaleBl2NativeFooter(s,NativeFooterFamily::Generic) && s.calls()<512);}
        for(const auto [i,command]:commands) {
            GfxSession s(vm,base,movie,scratch1,512,false);MapLegendObservation observation;
            const auto target=vm.hit(s,i,&observation);
            if(!target) std::fprintf(stderr,"MainMenu index=%u rejected=%s calls=%u measured=%u\n",i,observation.reason,s.calls(),observation.measured);
            assert(target && target.command==command && target.first==i && s.calls()<512);
        }
        assert(vm.fieldBoundsCalls==0 && vm.fieldProjectionCalls==0 && vm.vm.refs.empty() && vm.vm.movieRefs==1);
        touchSettingsState.values[17]=100;
    }
    {
        FooterVm vm(argv[1]);vm.text("     Accept","    <IMG src='android_A'>Accept");
        vm.vm.object(vm.parent).tx=1190;
        {GfxSession s(vm,base,movie,scratch1,512,false);const auto target=vm.hit(s,4);
         assert(target && target.bounds.right<=1 && target.bounds.left>=0);}
        assert(vm.fieldBoundsCalls==0 && vm.fieldProjectionCalls==0 && vm.vm.refs.empty());
    }
    for(unsigned fault=0;fault<3;++fault) {
        FooterVm vm(argv[1]);vm.text(icon+" Accept","<IMG src='android_A'> Accept");
        if(fault==0) vm.missingCharacter=true;
        if(fault==1) vm.changeInCharacter=true;
        if(fault==2) vm.vm.object(vm.parent).ty=2000;
        touchSettingsState.values[17]=130;
        {GfxSession s(vm,base,movie,scratch1,512,true);const auto original=vm.vm.object(vm.field).properties;
         assert(!ScaleBl2NativeFooter(s,NativeFooterFamily::Generic) && vm.vm.object(vm.field).properties==original);
         assert(!vm.vm.object(vm.field).properties.count("_bl2TouchFooterOriginal"));}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
        touchSettingsState.values[17]=100;
    }
    {
        // Exact native432 MainMenu transform witness. The model reproduces
        // its observed old-order delta (-.5375,-.5,0,0), then exercises the
        // corrected setter order and retained actual-transform ownership.
        FooterVm vm(argv[1],"_root.menu.inner.tooltips");
        vm.text("      Network Options\r      Select Character      Select      Back",
            "<TEXTFORMAT><P ALIGN=\"RIGHT\"><FONT>    <IMG SRC=\"android_Back\"> Network Options</FONT></P></TEXTFORMAT>"
            "<TEXTFORMAT><P ALIGN=\"RIGHT\"><FONT>    <IMG SRC=\"android_Y\"> Select Character    "
            "<IMG SRC=\"android_A\"> Select    <IMG SRC=\"android_B\"> Back</FONT></P></TEXTFORMAT>");
        vm.vm.prop(vm.field,"_x",num(-264.95));vm.vm.prop(vm.field,"_y",num(447.2));
        for(auto& p:vm.positions) {p.x+=740.05;p.y=p.y==0?2:22.4;}
        vm.nativeTransform=true;
        const auto original=vm.vm.object(vm.field).properties;
        {
            GfxSession s(vm,base,movie,scratch1,512,true);const auto field=s.variable("_root.menu.inner.tooltips");
            const auto write=[&](const char* key,double value) {
                const auto n=s.number(value);assert(n && s.setMember(field,key,n));s.release(n);
            };
            write("_x",-497.9625);write("_y",436.6);write("_xscale",125);write("_yscale",125);
            const auto& p=vm.vm.object(vm.field).properties;
            assert(std::abs(number(p.at("_x"))-(-498.5))<1e-9);
            assert(std::abs(number(p.at("_y"))-436.1)<1e-9);
            s.release(field);
        }
        vm.vm.object(vm.field).properties=original;vm.transformWrites.clear();
        for(const unsigned percent:{125u,125u,200u,100u}) {
            touchSettingsState.values[17]=percent;
            {
                GfxSession s(vm,base,movie,scratch1,512,true);
                assert(ScaleBl2NativeFooter(s,NativeFooterFamily::Generic) && s.good() && s.calls()<512);
                const auto& p=vm.vm.object(vm.field).properties;
                const auto& saved=vm.vm.object(p.at("_bl2TouchFooterOriginal")).properties;
                assert(number(saved.at("appliedX"))==number(p.at("_x")) && number(saved.at("appliedY"))==number(p.at("_y")));
                assert(number(saved.at("appliedSx"))==number(p.at("_xscale")) && number(saved.at("appliedSy"))==number(p.at("_yscale")));
                assert(std::abs(number(saved.at("cx"))-932.05)<1e-9 && std::abs(number(saved.at("bottom"))-42.4)<1e-9);
                if(vm.transformWrites.size()==4) {
                    assert((vm.transformWrites==std::vector<std::string>{"_xscale","_yscale","_x","_y"}));
                    assert(std::abs(number(p.at("_x"))-(-498))<1e-9 && std::abs(number(p.at("_y"))-436.6)<1e-9);
                    assert(std::abs(number(saved.at("appliedX"))-(-497.9625))>.03);
                }
                const auto writes=vm.vm.propertyWrites;s.beginInputBoundary();
                assert(ScaleBl2NativeFooter(s,NativeFooterFamily::Generic) && vm.vm.propertyWrites==writes);
            }
            {GfxSession s(vm,base,movie,scratch1,512,false);assert(vm.hit(s,48).command==NativeFooterCommand::ButtonA);}
        }
        for(const auto* key:{"_x","_y","_xscale","_yscale"})
            assert(std::abs(number(vm.vm.object(vm.field).properties.at(key))-number(original.at(key)))<1e-9);
        vm.vm.prop(vm.field,"_x",num(-264.75));touchSettingsState.values[17]=200;
        {GfxSession s(vm,base,movie,scratch1,512,true);const auto writes=vm.vm.propertyWrites;
         assert(!ScaleBl2NativeFooter(s,NativeFooterFamily::Generic) && vm.vm.propertyWrites==writes);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && vm.vm.clicks==0);
        touchSettingsState.values[17]=100;
    }
    {
        FooterVm vm(argv[1]);vm.text(icon+" Accept","<IMG src='android_A'> Accept");
        const auto original=vm.vm.object(vm.field).properties;
        double width100=0,width125=0,width200=0;
        for(const auto percent:{100u,125u,125u,200u,100u}) {
            touchSettingsState.values[17]=percent;
            {GfxSession scale(vm,base,movie,scratch1,512,true);
             assert(ScaleBl2NativeFooter(scale,NativeFooterFamily::Generic) && scale.good() && scale.calls()<512);
             const auto writes=vm.vm.propertyWrites;scale.beginInputBoundary();
             assert(ScaleBl2NativeFooter(scale,NativeFooterFamily::Generic) && vm.vm.propertyWrites==writes);}
            {GfxSession s(vm,base,movie,scratch1,512,false);const auto target=vm.hit(s);assert(target);
             const double width=target.bounds.right-target.bounds.left;
             if(percent==100) width100=width;if(percent==125) width125=width;if(percent==200) width200=width;}
        }
        assert(std::abs(width125-width100*1.25)<1e-7 && std::abs(width200-width100*2)<1e-7);
        for(const auto* name:{"_x","_y","_xscale","_yscale"})
            assert(number(vm.vm.object(vm.field).properties.at(name))==number(original.at(name)));
        touchSettingsState.values[17]=150;vm.vm.prop(vm.field,"_x",num(21));
        {GfxSession s(vm,base,movie,scratch1,512,true);const auto writes=vm.vm.propertyWrites;
         assert(!ScaleBl2NativeFooter(s,NativeFooterFamily::Generic) && vm.vm.propertyWrites==writes);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && vm.vm.clicks==0);
    }
    touchSettingsState.values[17]=100;
    std::puts("Native footer glyph, ownership, UTF-16, scale and restore checks passed");
}
