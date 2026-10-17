#include "gfx_widget_fixture.h"
#include "bl2_touch_ui_transactions.h"
#include "ui_age_gate_model_fixture.inc"
#include <cstdio>
#include <memory>
using namespace bl2::touch;
using namespace widget_fixture;

struct AgeModelFixture : Fixture {
    std::uint32_t movieType=type("GearboxAccountGFxMovie",0x400),clipType=type("GearboxAccountGFxObject",0x200),
        account=object("Account",movieType),clip=object("AccountClip",clipType),
        stackField=property(movieType,"ScreenStack","ArrayProperty",0x280,12),
        clipField=property(movieType,"GearboxAccountObj","ObjectProperty",0x294,4),
        stackInner=0,screenData=memory.allocate(24),topText=memory.allocate(16),input=0,event=0,key=0,controller=0,result=0,
        email=0,password=0;
    AgeModelFixture() {
        for(auto kind:{"StrProperty","ByteProperty"}) {const auto p=type(kind,0x80);memory.word(p+0xC4,0x8000);}
        stackInner=inner(stackField,"StrProperty",12);
        memory.word(stackField+0x48,0x00402000);memory.word(stackInner+0x48,0x00400000);
        memory.word(clipField+0x48,0x2000);memory.word(clipField+0x74,clipType);memory.word(account+0x294,clip);
        std::memcpy(memory.blocks.at(topText).data(),"ageGate",8);
        memory.array(account+0x280,screenData,2,2);
        memory.array(screenData+12,topText,8,16); // Only the genuine active top matters.
        input=method(movieType,"HandleInputKey",0x00020002,
            {{"ControllerId","IntProperty"},{"ukey","NameProperty"},{"uevent","ByteProperty"},{"ReturnValue","BoolProperty"}});
        result=memory.word(input+0x44);event=memory.word(result+0x3C);key=memory.word(event+0x3C);controller=memory.word(key+0x3C);
        email=property(input,"EmailInputDesc","StrProperty",20,12);
        password=property(input,"PasswordInputDesc","StrProperty",32,12);
        memory.word(email+0x48,0x00400000);memory.word(password+0x48,0x00400000);
        memory.word(input+0x44,controller);memory.word(controller+0x3C,key);memory.word(key+0x3C,event);
        memory.word(event+0x3C,result);memory.word(result+0x3C,email);
        memory.word(email+0x3C,password);memory.word(password+0x3C,0);
        memory.word(input+0x48,(4u<<24)|44);memory.word(input+0x84,(20u<<16)|(4u<<8));memory.word(input+0x88,16);
        memory.word(key+0x44,8);memory.word(key+0x54,4);memory.word(event+0x44,1);memory.word(event+0x54,12);
        memory.word(result+0x54,16);memory.word(result+0x48,0x580);
        memory.word(event+0x74,object("EInputEvent",type("Enum",0x80)));
    }
    UiAgeGateModel capture(std::uint32_t image=Fixture::base) const {
        return CaptureBl2AgeGate({&memory,Memory::read},image,account);
    }
    void rebase(std::uint32_t image) {
        std::vector<std::pair<std::uint32_t,std::vector<unsigned char>>> blocks;
        for(const auto& pair:memory.blocks) if(pair.first<0x04000000) blocks.push_back(pair);
        for(const auto& [at,data]:blocks) memory.blocks[image+at-Fixture::base]=data;
    }
};

struct AgeVm final : GfxTransport {
    Vm vm;
    Raw content{},gate{};
    std::array<Raw,3> fields{};
    unsigned focusChanges=0,keyboardOpened=0;
    bool focusRejected=false,foreignFocus=false;
    std::array<bool,2> scratchUsed{};
    explicit AgeVm(const char* fixture):vm(fixture) {
        content=vm.make(10);gate=vm.make(10);vm.visible(content,vm.root);vm.visible(gate,content);
        vm.prop(content,"_root",vm.root);vm.prop(vm.root,"gearboxAccount",content);
        vm.prop(content,"_name",vm.str("gearboxAccount"));vm.prop(content,"_currentframe",num(4));
        vm.prop(content,"ageGate",gate);vm.prop(gate,"_name",vm.str("ageGate"));
        constexpr const char* names[]{"monthInput","dayInput","yearInput"};
        for(unsigned i=0;i<fields.size();++i) {
            fields[i]=vm.make(10);vm.visible(fields[i],gate);vm.prop(gate,names[i],fields[i]);
            vm.prop(fields[i],"_name",vm.str(names[i]));vm.prop(fields[i],"disabled",boolean(false));
            vm.prop(fields[i],"maxChars",num(i==2?4:2));vm.prop(fields[i],"text",vm.str(""));
            vm.prop(fields[i],"focused",num(i==0?1:0));
            vm.object(fields[i]).bounds={0,0,76.2,31};vm.object(fields[i]).tx=491.9+i*110;vm.object(fields[i]).ty=391.8;
        }
        vm.write(wrapper+0x3C,content.data(),32);
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {
        if(pc==invoke) for(unsigned i=0;i<fields.size();++i) if(args[1]==fields[i][4]) {
            const auto name=vm.text(args[3]);
            if(name=="__get__maxChars" || name=="__get__text" || name=="__get__focused") {
                assert(args[5]==0);vm.output(args[2],vm.object(fields[i]).properties.at(name.substr(7)));result=1;return true;
            }
            if(name=="__set__focused") {
                assert(args[5]==1 && number(vm.raw(args[4]))==1);++focusChanges;
                if(!focusRejected) for(unsigned j=0;j<fields.size();++j) vm.prop(fields[j],"focused",num(j==i || (foreignFocus && j==0)?1:0));
                vm.output(args[2],num(1));result=1;return true;
            }
        }
        return vm.call(pc,args,result);
    }
};
struct GfxHostTransport final : GfxTransport {
    AgeVm& host;std::function<bool()> retained;unsigned slot=0;
    GfxHostTransport(AgeVm& h,std::function<bool()> guard):host(h),retained(std::move(guard)) {
        if(host.scratchUsed[0]) slot=1;assert(!host.scratchUsed[slot]);host.scratchUsed[slot]=true;
    }
    ~GfxHostTransport() override {host.scratchUsed[slot]=false;}
    std::uint32_t scratch() const {return slot?scratch2:scratch1;}
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return host.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return host.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return host.executable(p);}
    bool current() const override {return host.current() && retained();}
    void abandonScratch() override {host.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {return host.call(pc,args,result);}
};
struct Consumer {
    struct Target {
        std::uint64_t ownerToken=0,controllerToken=7,scene=5;
        std::uint32_t owner=0,movie=widget_fixture::movie,interface=widget_fixture::movie,viewport=9,width=2560,height=1440,controllerId=0;
        struct {std::string movieClass="GearboxAccountGFxMovie";} direct;
    } frameTarget,ageGateOwner;
    static constexpr std::uint32_t base=widget_fixture::base;
    AgeModelFixture metadata;AgeVm host;
    std::unique_ptr<GfxHostTransport> ageGateTransport;
    std::unique_ptr<GfxSession> ageGateSession;
    AgeGateWidgetTarget ageGateTarget;UiAgeGateModel ageGateModel;
    bool ageGateGesture=false,alive=true,physical=false,loseOnPressed=false,blurOnPressed=false;
    bool keyWrong=false;
    std::vector<std::vector<std::uint32_t>> keys;
    UiTransactions transactions;
    explicit Consumer(const char* fixture):host(fixture) {
        metadata.rebase(base);frameTarget.owner=metadata.account;frameTarget.ownerToken=metadata.capture(base).owner;
        host.vm.write(metadata.clip+0x3C,host.content.data(),32);
        host.vm.put(base+0x0300A330,71u);host.vm.put(base+0x0300A334,0u);
        transactions.begin(3,5,true,false,2560,1440,false);
    }
    static bool copyMemory(const void* context,std::uint32_t p,void* out,std::size_t n) {
        return Memory::read(&static_cast<const Consumer*>(context)->metadata.memory,p,out,n);
    }
    bool current() const {return alive && !physical;}
    bool targetValid(const Target& target) const {return current() && target.ownerToken==frameTarget.ownerToken;}
    std::uint32_t read(std::uint32_t p) const {std::uint32_t word=0;host.read(p,&word,4);return word;}
    std::string name(std::uint32_t p) const {assert(p==base+0x0300A330);return keyWrong?"Enter":"XboxTypeS_A";}
    std::uint32_t originalQuery(std::uint32_t owner,const UiNativeMethod& method,std::initializer_list<std::uint32_t> params,std::uint32_t offset) {
        assert(owner==metadata.account && method.function==metadata.input && method.bytes==20 && offset==16);
        keys.emplace_back(params);assert(keys.back().size()==5 && keys.back()[1]==71 && keys.back()[2]==0 && keys.back()[4]==0xFFFFFFFFu);
        const auto event=keys.back()[3];assert(event<=1);
        if(!event && loseOnPressed) alive=false;
        if(!event && blurOnPressed) for(const auto& field:host.fields) host.vm.prop(field,"focused",num(0));
        if(event) ++host.keyboardOpened;
        return 1;
    }
#include "ui_age_gate_sink.inc"
    void pointer(PointerPhase phase,float x,float y,int id=1,std::uint64_t epoch=3) {
        const PointerEvent event{id,phase,x,y,epoch,1,10};
        const auto tap=transactions.pointer(event);
        if(transactions.ownsPointer(id)) {
            if(phase==PointerPhase::Down) ageGateDown(x,y);
            else if(phase==PointerPhase::Move && ageGateGesture) ageGateMove(x,y);
        }
        if(tap && ageGateGesture) ageGateUp(tap->x,tap->y,tap->scrollDirection!=0);
        if(!transactions.ownsPointer(id)) clearAgeGate();
    }
};

int main(int argc,char** argv) {
    assert(argc==2);unsigned positive=0,negative=0;
    {
        AgeModelFixture f;const auto good=f.capture();assert(good.valid && good.input.bytes==20 && good.clip>>32==f.clip);++positive;
        const std::pair<std::uint32_t,std::uint32_t> faults[]{
            {f.account+0xC,2},{f.account+0x34,f.pause},{f.stackField+0x28,f.front},{f.stackField+0x40,2},
            {f.stackField+0x44,4},{f.stackField+0x48,0x2000},{f.stackField+0x4C,1},
            {f.stackInner+0x28,f.clipField},{f.stackInner+0x44,4},{f.stackInner+0x40,2},{f.stackInner+0x48,0},
            {f.stackInner+0x4C,1},{f.account+0x284,0},{f.account+0x284,3},{f.screenData+16,7},
            {f.screenData+20,7},{f.topText,0x65676173},{f.clipField+0x48,0},{f.clipField+0x74,f.front},
            {f.clipField+0x28,f.front},{f.account+0x294,0},{f.clip+0xC,2},{f.input+0x7C,0x24400},
            {f.input+0x84,(24u<<16)|(4u<<8)},{f.input+0x88,12},{f.key+0x44,4},{f.event+0x54,16},
            {f.event+0x74,f.clip},{f.event+0x48,0x90},{f.result+0x48,0x80},{f.result+0x74,2},
            {f.result+0x3C,f.controller},{f.controller+0x3C,f.event},
            {f.email+0x48,0},{f.email+0x48,0x80},{f.email+0x4C,1},{f.email+0x40,2},
            {f.email+0x44,4},{f.email+0x54,16},{f.email+0x28,f.movieType},{f.email+0x3C,0},
            {f.password+0x48,0},{f.password+0x40,2},{f.password+0x44,4},{f.password+0x54,20},
            {f.password+0x28,f.movieType},{f.password+0x34,f.types.at("IntProperty")},{f.password+0x3C,f.controller},
            {f.result+0x3C,f.password},{f.result+0x3C,0},{f.input+0x48,32},{f.input+0x48,20},
            {f.input+0x84,(20u<<16)|(6u<<8)},{f.email+0x2C,f.name("OtherLocal")}};
        for(const auto& [at,value]:faults) {const auto old=f.memory.word(at);f.memory.word(at,value);assert(!f.capture().valid);f.memory.word(at,old);++negative;}
        const auto replacement=f.object("RecreatedClip",f.clipType);f.memory.word(f.account+0x294,replacement);
        assert(f.capture().valid && f.capture().structure!=good.structure);++negative;
    }
    for(unsigned selected=0;selected<3;++selected) {
        Consumer c(argv[1]);const float x=(530.f+selected*110)/1280,y=410.f/720;
        c.pointer(PointerPhase::Down,x,y);assert(c.ageGateTarget.index==static_cast<int>(selected) && c.host.focusChanges==1 && c.keys.empty());
        c.pointer(PointerPhase::Up,x,y);assert(c.keys.size()==2 && c.keys[0][3]==0 && c.keys[1][3]==1 && c.host.keyboardOpened==1);
        assert(c.host.vm.refs.empty() && c.host.vm.movieRefs==1 && !c.host.vm.abandoned);++positive;
    }
    for(unsigned fault=0;fault<20;++fault) {
        Consumer c(argv[1]);const float x=640.f/1280,y=410.f/720;
        c.pointer(PointerPhase::Down,x,y);assert(c.ageGateTarget && c.keys.empty());
        switch(fault) {
        case 0:c.pointer(PointerPhase::Cancel,x,y);break;
        case 1:c.pointer(PointerPhase::Down,x,y,2);break;
        case 2:c.pointer(PointerPhase::Move,x+.2f,y);break;
        case 3:c.transactions.begin(4,5,true,false,2560,1440);break;
        case 4:c.transactions.begin(3,6,true,false,2560,1440);break;
        case 5:c.transactions.begin(3,5,true,true,2560,1440);break;
        case 6:c.alive=false;break;
        case 7:c.physical=true;break;
        case 8:c.frameTarget.ownerToken++;break;
        case 9:c.metadata.memory.word(c.metadata.account+0x284,0);break;
        case 10:c.host.vm.prop(c.host.content,"_currentframe",num(3));break;
        case 11:c.host.vm.prop(c.host.gate,"_visible",boolean(false));break;
        case 12:c.host.vm.prop(c.host.fields[1],"focused",num(0));break;
        case 13:c.host.vm.prop(c.host.fields[0],"focused",num(1));break;
        case 14:c.host.vm.prop(c.host.fields[2],"disabled",boolean(true));break;
        case 15:c.host.vm.prop(c.host.fields[1],"_parent",c.host.content);break;
        case 16:c.host.vm.prop(c.host.fields[1],"maxChars",num(128));break;
        case 17:c.host.vm.prop(c.host.fields[1],"text",c.host.vm.str("12"));break;
        case 18:c.host.vm.object(c.host.fields[1]).tx+=10;break;
        case 19:c.keyWrong=true;break;
        }
        c.pointer(PointerPhase::Up,x,y);assert(c.keys.empty() && c.host.keyboardOpened==0);++negative;
        assert(c.host.vm.refs.empty() && c.host.vm.movieRefs==1 && !c.host.vm.abandoned);
    }
    for(bool blur:{false,true}) {
        Consumer c(argv[1]);c.pointer(PointerPhase::Down,.5f,410.f/720);
        c.loseOnPressed=!blur;c.blurOnPressed=blur;c.pointer(PointerPhase::Up,.5f,410.f/720);
        assert(c.keys.size()==1 && c.keys[0][3]==0 && !c.host.keyboardOpened);++negative;
    }
    for(unsigned fault=0;fault<10;++fault) {
        Consumer c(argv[1]);
        switch(fault) {
        case 0:c.frameTarget.direct.movieClass="TextChatGFxMovie";break;
        case 1:c.metadata.memory.word(c.metadata.account+0x284,0);break;
        case 2:c.host.vm.prop(c.host.content,"_currentframe",num(5));break;
        case 3:c.host.vm.prop(c.host.gate,"_name",c.host.vm.str("email"));break;
        case 4:c.host.vm.prop(c.host.fields[1],"_name",c.host.vm.str("passwordInput"));break;
        case 5:c.host.vm.prop(c.host.fields[1],"_visible",boolean(false));break;
        case 6:c.host.vm.prop(c.host.fields[1],"disabled",boolean(true));break;
        case 7:c.host.vm.prop(c.host.fields[2],"maxChars",num(2));break;
        case 8:c.host.focusRejected=true;break;
        case 9:c.host.foreignFocus=true;break;
        }
        c.pointer(PointerPhase::Down,.5f,410.f/720);c.pointer(PointerPhase::Up,.5f,410.f/720);
        assert(c.keys.empty() && !c.host.keyboardOpened);++negative;
    }
    {
        Consumer c(argv[1]);c.pointer(PointerPhase::Down,.5f,410.f/720);
        c.pointer(PointerPhase::Up,.5f,410.f/720,1,2);assert(c.keys.empty());
        c.pointer(PointerPhase::Cancel,.5f,410.f/720);assert(c.keys.empty());++negative;
    }
    std::printf("PASS: AgeGate original focus/20-byte XboxA phases %u positive/%u negative; no Submit/Enter/service replacement\n",positive,negative);
}
