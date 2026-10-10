#include "gfx_widget_fixture.h"
#include "bl2_touch_ui_transactions.h"
#include "ui_status_inventory_model_fixture.inc"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <span>
using namespace bl2::touch;
using namespace widget_fixture;
namespace bl2::touch {
std::string CaptureBl2StatusInventoryGate(const UiModelMemory&,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
}
#include "ui_status_inventory_transport.inc"

// Only Android logging/property access is substituted. The extracted logger
// below runs with its real finite budget/dedup. The DropTarget observer uses
// its own bounded read-only GFx transport, never the functional session.
static std::string diagnosticsProperty="0";
static std::vector<std::string> diagnosticsLines;
static constexpr unsigned PROP_VALUE_MAX=92,ANDROID_LOG_INFO=4;
static int __system_property_get(const char* name,char* value) {
    assert(std::strcmp(name,"debug.bl2bridge.touch_diagnostics")==0);
    assert(diagnosticsProperty.size()<PROP_VALUE_MAX);
    std::memcpy(value,diagnosticsProperty.c_str(),diagnosticsProperty.size()+1);return static_cast<int>(diagnosticsProperty.size());
}
static int __android_log_print(unsigned priority,const char* tag,const char* format,...) {
    assert(priority==ANDROID_LOG_INFO && std::strcmp(tag,"BL2TouchDiagnostics")==0);
    char line[4096]{};va_list args;va_start(args,format);std::vsnprintf(line,sizeof(line),format,args);va_end(args);
    diagnosticsLines.emplace_back(line);return 0;
}
#include "ui_key_return_scope.inc"
#include "ui_native_release_scope.inc"
static constexpr std::uint32_t kMovieKey=0x01CC5E7C;
static std::uint32_t floatWord(float value) {std::uint32_t word=0;std::memcpy(&word,&value,4);return word;}
namespace Dynarmic::A32 {struct Jit {std::array<std::uint32_t,16> r{};auto& Regs() {return r;}};}
struct DynarmicHost {
    static constexpr std::uint32_t base=0x40000000;
    std::map<std::uint32_t,std::uint32_t> words;
    std::vector<std::vector<std::uint32_t>> replays;
    unsigned writes=0,invalidations=0,registrations=0,refuseRegistration=0;
    bool imageValid=true,boundsValid=true,symbolValid=true;
    struct Elf {
        DynarmicHost& host;
        std::uint32_t GetImageSize() const {return host.imageValid?0x03089000:0;}
        bool IsReadOnlyRange(std::uint32_t,std::size_t) const {return host.boundsValid;}
        bool IsCodeRange(std::uint32_t,std::size_t) const {return host.boundsValid;}
    } elf{*this};
    auto& GetElfLoader() {return elf;}
    auto& GetThunkManager() {return *this;}
    std::uint32_t FindSymbol(const char* s) {
        return symbolValid?base+(std::strcmp(s,"KEY_LeftMouseButton")==0?0x03009F68:
            std::strstr(s,"FGFxEngine")?0x01CC5E7C:0x01CB7410):0;
    }
    std::uint32_t MemoryRead32(std::uint32_t p) {return words.at(p);}
    void MemoryWrite32(std::uint32_t p,std::uint32_t v) {++writes;words[p]=v;}
    void InvalidateGuestCode(std::uint32_t,std::size_t n) {assert(n==8);++invalidations;}
    std::uint32_t CreateCodeVeneer(std::span<const std::uint32_t> w) {replays.emplace_back(w.begin(),w.end());return 0x20000+static_cast<std::uint32_t>(replays.size()*16);}
    template<class Handler> std::uint32_t RegisterThunk(const char*,Handler) {++registrations;return registrations==refuseRegistration?0:0x30000+registrations*32;}
    DynarmicHost() {
        words={{base+0x01CC5FE0,0xE59DA02C},{base+0x01CC5FE4,0xE35A0000},{base+0x01CC5FE8,0x1AFFFFC6},
            {base+0x01CB74E0,0xE59D0020},{base+0x01CB74E4,0xE2900000},{base+0x01CB74E8,0x13A00001},
            {base+0x01CC6348,0xE5923000},{base+0x01CC634C,0xE5933090},{base+0x01CC6350,0xE12FFF33}};
    }
};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4459)
#endif
#include "ui_key_return_install.inc"
#include "ui_native_release_install.inc"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

struct StatusFixture : Fixture {
    std::uint32_t movieBase=type("GFxMoviePlayer",0x380),movieType=type("StatusMenuExGFxMovie",0x380,movieBase),
        baseTopType=type("BaseTopLevelPanelGFxObject",0x380),
        topType=type("StatusMenuInventoryPanelGFxObject",0x380,baseTopType),
        backpackType=type("InventoryListPanelGFxObject",0x380),equippedType=type("StatusMenuEquippedPanelGFxObject",0x380),
        inventoryType=type("WillowInventory",0x100),weaponType=type("WillowWeapon",0x100,inventoryType);
    std::uint32_t status=object("Status",movieType),top=object("Inventory",topType),
        backpack=object("Backpack",backpackType),equipped=object("Equipped",equippedType),item=object("Actual weapon",weaponType);
    std::uint32_t screenField=0,topField=0,backpackField=0,equippedField=0,screenEnum=0;
    std::array<std::uint32_t,2> query{},input{},result{};
    std::array<std::uint32_t,5> inputFields{};
    std::array<std::uint32_t,4> movieInputFields{};
    StatusFixture() {
        const auto byte=type("ByteProperty",0x80);memory.word(byte+0xC4,0x8000);
        const auto enumeration=type("Enum",0x100);screenEnum=object("EBackButtonScreen",enumeration);
        screenField=property(movieType,"CurrentScreen","ByteProperty",0x104,1);memory.word(screenField+0x74,screenEnum);
        topField=property(movieType,"InventoryPanel","ObjectProperty",0x108,4);memory.word(topField+0x74,topType);
        backpackField=property(topType,"BackpackPanel","ObjectProperty",0x100,4);memory.word(backpackField+0x74,backpackType);
        equippedField=property(topType,"EquippedPanel","ObjectProperty",0x104,4);memory.word(equippedField+0x74,equippedType);
        memory.word(status+0x104,3);memory.word(status+0x108,top);
        memory.word(top+0x100,backpack);memory.word(top+0x104,equipped);
        const std::array panelTypes{backpackType,equippedType};
        for(unsigned i=0;i<2;++i) {
            query[i]=method(panelTypes[i],"GetThingByIndex",0x00020002,{{"Index","IntProperty"},{"ReturnValue","ObjectProperty"}});
            result[i]=memory.word(query[i]+0x44);input[i]=memory.word(result[i]+0x3C);
            memory.word(query[i]+0x44,input[i]);memory.word(input[i]+0x3C,result[i]);memory.word(result[i]+0x3C,0);
            memory.word(result[i]+0x48,0x580);memory.word(result[i]+0x74,inventoryType);memory.word(query[i]+0x88,4);
        }
        const auto delegate=type("DelegateProperty",0x80);memory.word(delegate+0xC4,0x8000);
        inputFields={property(baseTopType,"__OnInputKey__Delegate","DelegateProperty",0x180,12),
            property(baseTopType,"QueuedInputKey","DelegateProperty",0x18C,12),
            property(baseTopType,"QueuedTriggerKey","NameProperty",0x198,8),
            property(topType,"bIsDoingEquip","BoolProperty",0x1A0,4),
            property(topType,"bInitialSetupFinished","BoolProperty",0x1A0,4)};
        memory.word(inputFields[0]+0x48,0x00400000);memory.word(inputFields[1]+0x48,0x00400000);
        memory.word(inputFields[3]+0x74,1);memory.word(inputFields[4]+0x74,2);
        memory.word(top+0x180,top);memory.word(top+0x184,name("NormalInputKey"));
        memory.word(top+0x18C,top);memory.word(top+0x190,name("EquipInputKey"));
        memory.word(top+0x198,name("None"));memory.word(top+0x1A0,2);
        movieInputFields={property(movieBase,"__OnInputKey__Delegate","DelegateProperty",0x200,12),
            property(movieBase,"LocalPlayerOwnerIndex","IntProperty",0x20C,4),
            property(movieBase,"bCaptureInput","BoolProperty",0x210,4),
            property(movieBase,"bDiscardNonOwnerInput","BoolProperty",0x210,4)};
        memory.word(movieInputFields[0]+0x48,0x00402000);memory.word(movieInputFields[1]+0x48,0x2000);
        memory.word(movieInputFields[2]+0x74,4);memory.word(movieInputFields[3]+0x74,8);
        memory.word(status+0x200,status);memory.word(status+0x204,name("InventoryPanelInputKey"));
        memory.word(status+0x20C,0);memory.word(status+0x210,4);
    }
    std::uint64_t token(std::uint32_t p) const {return (std::uint64_t(p)<<32)|memory.word(p+0x20);}
};

// Genuine declaring classes/CPF and reflected offsets for the two native
// controller gates. These are separate from the old Top/Movie-state fixture.
struct GateFixture : StatusFixture {
    std::uint32_t willowMovie=type("WillowGFxMovie",0x380,movieBase),
        movie3D=type("WillowGFxMovie3D",0x380,willowMovie),
        inventoryMovie=type("WillowInventoryGFxMovie",0x380,movie3D),
        playerType=type("Player",0x100),localType=type("LocalPlayer",0x100,playerType),
        controllerBase=type("PlayerController",0x1000),controllerType=type("WillowPlayerController",0x1000,controllerBase),
        controller=object("Controller",controllerType),local=object("LocalPlayer",localType);
    std::array<std::uint32_t,6> gateFields{};
    GateFixture() {
        memory.word(movieType+0x40,inventoryMovie);
        gateFields={property(baseTopType,"ParentMovie","ObjectProperty",0x214,4),
            property(willowMovie,"WPCOwner","ObjectProperty",0x218,4),
            property(movie3D,"bAlwaysHideInCinematicMode","BoolProperty",0x21C,4),
            property(controllerBase,"Player","ObjectProperty",0x66C,4),
            property(controllerBase,"bCinematicMode","BoolProperty",0x670,4),
            property(localType,"ControllerId","IntProperty",0x64,4)};
        memory.word(gateFields[0]+0x74,inventoryMovie);memory.word(gateFields[1]+0x74,controllerType);
        memory.word(gateFields[1]+0x48,0x2000);memory.word(gateFields[2]+0x48,0x2000);
        memory.word(gateFields[2]+0x74,0x20);memory.word(gateFields[3]+0x48,2);memory.word(gateFields[3]+0x74,playerType);
        memory.word(gateFields[4]+0x74,0x40);
        memory.word(top+0x214,status);memory.word(status+0x218,controller);memory.word(controller+0x66C,local);
        memory.word(base+0x01B427B0,0xE590566C);memory.word(base+0x01B427F4,0xE5D50064);
    }
    std::string gate(std::uint32_t sent=0) {return CaptureBl2StatusInventoryGate({&memory,Memory::read},base,status,controller,sent);}
    void growTables(std::uint32_t capacity) {
        const auto newObjects=memory.allocate(capacity*4),newNames=memory.allocate(capacity*4);
        for(std::uint32_t i=0;i<count;++i) memory.word(newObjects+i*4,memory.word(objects+i*4));
        for(std::uint32_t i=0;i<nameIds.size();++i) memory.word(newNames+i*4,memory.word(names+i*4));
        objects=newObjects;names=newNames;
        memory.array(base+0x02FBA268,objects,count,capacity);
        memory.array(base+0x02FB00C8,names,static_cast<std::uint32_t>(nameIds.size()),capacity);
    }
};

struct StatusVm final : InventoryVmBase {
    Raw equipped{};
    bool retired=false,originalQueue=false,pendingMove=false,originalMouseDown=false;
    bool rolloverOnly=false,moveInstalled=false;
    float pendingX=0,pendingY=0,lastX=-1,lastY=-1;
    struct Button {std::uint32_t type;float x,y;};
    std::vector<Button> buttons,events;
    mutable unsigned reads=0;
    unsigned writes=0,calls=0;
    std::uint32_t nextScratch=0x23100000;
    unsigned scriptInvokes=0,memberSets=0,dropReads=0;
    bool failDrop=false,replaceDrop=false,replacePayload=false;
    std::vector<std::vector<std::uint32_t>> keyCalls;
    std::array<std::uint32_t,2> keyHandled{{0,1}};
    std::function<void(std::uint32_t)> keySideEffect;
    std::function<void(float,float)> nativeMouseUp;
    std::uint32_t nativeMouseIndex=0;
    unsigned nativeReleases=0;
    bool nativeFiltered=false,nativeDiscarded=false,nativeHandled=false;
    bool queuedMouseRelease=true,equipDelegate=false;
    std::vector<std::string> nativeOrder;
    bool originalSwitch(std::uint32_t index,std::uint32_t event) {
        // Exact original13202: unrelated keys remain consumed. Only the
        // matching Released clears the queue; LeftMouse returns false.
        if(!queuedMouseRelease) return false;
        if(index==nativeMouseIndex && event==1) {
            queuedMouseRelease=false;equipDelegate=true;return false;
        }
        return true;
    }
    std::uint32_t RunGuestCallback(std::uint32_t pc,const std::vector<std::uint32_t>& args) {
        assert(pc==Fixture::base+0x01CC5E7C && args.size()==6 && args[5]<=1);
        keyCalls.push_back(args);if(keySideEffect) keySideEffect(args[5]);
        if(args[3]!=nativeMouseIndex || args[5]!=1) return keyHandled[args[5]];
        ++nativeReleases;nativeOrder.push_back("native-release");
        if(nativeFiltered || nativeDiscarded || originalSwitch(args[3],args[5]) || nativeHandled) return 1;
        std::array<std::uint32_t,16> regs{};const auto sp=0x001F0000;
        regs[0]=regs[2]=widget_fixture::movie;regs[1]=sp+0x1C;
        regs[4]=args[3];regs[5]=args[1];regs[6]=uiNativeReleaseScope->owner;
        regs[7]=args[0];regs[9]=args[2];regs[13]=sp;
        std::map<std::uint32_t,std::uint32_t> stack{{sp+0x64,args[3]},{sp+0x68,args[4]},
            {sp+0x6C,args[5]},{sp+0x1C,3},{sp+0x30,0},{sp+0x24,0},{sp+0x28,0}};
        if(UiAdaptNativeRelease(regs,[&](auto p){return stack.at(p);},[&](auto p,auto v){stack[p]=v;})) {
            float x=0,y=0;std::memcpy(&x,&stack.at(sp+0x24),4);std::memcpy(&y,&stack.at(sp+0x28),4);
            assert(nativeMouseUp);nativeMouseUp(x,y);nativeOrder.push_back("gfx-up-queued");queue(3,x,y);
        }
        return 1;
    }
    explicit StatusVm(const char* fixture):InventoryVmBase(fixture) {
        vm.prop(vm.root,"inventory",top);equipped=vm.make(10);vm.visible(equipped,top);vm.prop(top,"equippedPanel",equipped);
        for(unsigned i=0;i<8;++i) {const auto c=vm.make(10);vm.visible(c,equipped);vm.prop(equipped,"cell"+std::to_string(i+1),c);}
    }
    void equipSource() {
        offers=true;vm.prop(panel,"_visible",boolean(false));vm.visible(cell,equipped);
        vm.prop(cell,"_name",vm.str("cell5"));vm.prop(equipped,"cell5",cell);vm.prop(manager,"pressOrigin",equipped);
    }
    void queue(std::uint32_t type,float x,float y) {
        events.push_back({type,x,y});
        if(!originalQueue) return;
        // Original AddMouseMove28B36E4 sets pending position/dirtyMask.
        // AddMouseButton28B3724 clears that bit for ordinary explicit X.
        if(type==1) {pendingMove=true;pendingX=x;pendingY=y;}
        else {pendingMove=false;buttons.push_back({type,x,y});}
    }
    static bool sourcePoint(double x,double y) {
        return x>=.18*1280 && x<=.23*1280 && y>=.37*720 && y<=.43*720;
    }
    void advanceOriginal() {
        if(!originalQueue) return;
        const auto source=offers?equipped:panel;
        // GetEntry28B38F4 consumes queued buttons before flushing dirty moves.
        for(const auto& button:buttons) {
            if(button.type==2) {
                const auto found=vm.object(source).properties.find("currentOverClip");
                // Authored handleStorageOnPress forwards only currentOverClip;
                // onCellPressed alone sets bMouseDown, never the event's XY.
                originalMouseDown=found!=vm.object(source).properties.end() && found->second==cell;
            } else {
                originalMouseDown=false;equipDelegate=false;nativeOrder.push_back("original-finish-equip");
            }
            if(rolloverOnly) {lastX=button.x;lastY=button.y;}
        }
        buttons.clear();
        if(pendingMove) {
            if(rolloverOnly) {
                // Native MouseState28B30FC suppresses unchanged integer XY.
                // First entry's RollOver196939 only installs onMouseMove;
                // subsequent real motion invokes StorageMove197739/GetHitCell.
                if(std::floor(pendingX)!=std::floor(lastX) || std::floor(pendingY)!=std::floor(lastY)) {
                    Raw nil{};nil[3]=1;
                    const bool inside=sourcePoint(pendingX/2559*1280,pendingY/1439*720);
                    if(!inside) {moveInstalled=false;vm.prop(source,"currentOverClip",nil);}
                    else if(!moveInstalled) {moveInstalled=true;vm.prop(source,"currentOverClip",nil);}
                    else {
                        if(originalMouseDown) vm.prop(manager,"_inDrag",boolean(true));
                        vm.prop(source,"currentOverClip",cell);
                    }
                }
                lastX=pendingX;lastY=pendingY;
            } else {
                if(originalMouseDown) vm.prop(manager,"_inDrag",boolean(true));
                const bool origin=pendingX==std::floor(.2f*2559) && pendingY==std::floor(.4f*1439);
                vm.prop(source,"currentOverClip",origin?cell:Raw{});
            }
            pendingMove=false;
        }
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {++reads;return InventoryVmBase::read(p,out,n);}
    bool write(std::uint32_t p,const void* bytes,std::size_t n) override {++writes;return InventoryVmBase::write(p,bytes,n);}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {
        ++calls;
        if(pc==set) ++memberSets;
        if(pc==get && vm.text(args[2])=="dropTarget") {
            ++dropReads;
            if(failDrop) return false;
            const bool ran=InventoryVmBase::call(pc,args,result);
            if(dropReads==1 && replaceDrop) {Raw nil{};nil[3]=1;vm.prop(manager,"dropTarget",nil);}
            if(dropReads==1 && replacePayload) vm.prop(manager,"_data",vm.make(8));
            return ran;
        }
        if(pc==invoke) {
            ++scriptInvokes;
            const auto method=vm.text(args[3]);
            if(method=="cancelDrag") {
                equipDelegate=false;nativeOrder.push_back("original-cancel-finish-equip");
            }
            if(method=="GetHitCell") {
                assert(args[1]==list[4] && args[5]==1);++hitQueries;
                const auto& point=vm.object(vm.raw(args[4])).properties;
                const bool inside=!rolloverOnly || sourcePoint(number(point.at("x")),number(point.at("y")));
                vm.output(args[2],!retired && inside && !arrow?cell:Raw{});result=1;return true;
            }
            if(method=="hitTest" || method=="CanBeSelected") {
                assert(args[5]==(method=="hitTest"?2u:0u));
                const bool inside=method!="hitTest" || !rolloverOnly ||
                    sourcePoint(number(vm.raw(args[4])),number(vm.raw(args[4]+32)));
                vm.output(args[2],boolean(args[1]==cell[4] && (method=="hitTest"?(offers && !retired && inside):selectable)));
                result=1;return true;
            }
        }
        return InventoryVmBase::call(pc,args,result);
    }
};

// The host fixture replaces only the callback transport. The actual sink's
// capture/DOWN/ACK/completion methods below compile without edits.
struct GfxHostTransport final : GfxTransport {
    StatusVm& vm;
    std::function<bool()> live;
    std::uint32_t ownScratch;
    GfxHostTransport(StatusVm& transport,std::function<bool()> current):vm(transport),live(std::move(current)),ownScratch(transport.nextScratch) {
        transport.nextScratch+=0x4000;
    }
    std::uint32_t scratch() const {return ownScratch;}
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* data,std::size_t n) override {return vm.write(p,data,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return live() && vm.current();}
    bool call(std::uint32_t p,const std::vector<std::uint32_t>& args,std::uint32_t& result) override {return vm.call(p,args,result);}
    void abandonScratch() override {vm.abandonScratch();}
};

struct StatusConsumer {
    struct Target {
        unsigned width=2560,height=1440;
        std::uint32_t owner=0,movie=widget_fixture::movie,interface=widget_fixture::movie,capabilities=96;
        std::uint32_t gfx=0x22000000,controller=0,controllerId=0;
        std::uint64_t ownerToken=0,scene=20,generation=30;
        UiDirectModel direct;
    };
    GateFixture metadata;
    static constexpr auto base=Fixture::base;
    StatusVm& vm;
    StatusVm& host;
    UiInventoryGesture inventoryGesture;
    UiTransactions transactions;
    MapLegendTarget footerTarget;
    StatusConsumer* state_=this;
    std::unique_ptr<GfxHostTransport> inventoryTransport;
    std::unique_ptr<GfxSession> inventorySession;
    InventoryWidgetTarget inventoryTarget;
    InventoryDragSource inventoryFresh;
    GfxIdentity inventoryDragData{};
    UiStatusInventoryModel inventoryStatus;
    std::array<GfxIdentity,3> inventoryStatusPanels{};
    Target inventoryOwner{},frameTarget{};
    std::optional<InventoryDragUpdate> inventoryStart,inventoryFinal;
    std::uint64_t inventoryPressedAt=100,now=100;
    bool inventoryPressed=false,inventoryCleanup=false,inventoryHoverBoundary=false,alive=true,allowed=true,staleOnQuery=false;
    bool inventoryReleaseReady=true;
    unsigned queries=0,clicks=0,moves=0,releases=0,drops=0,presses=0;
    mutable unsigned currentChecks=0,modelReads=0;
    unsigned inventoryDiagnostics=0;
    std::array<std::uint64_t,32> inventoryDiagnosticKeys{};
    unsigned inventoryTerminalDiagnostics=0;
    std::array<std::uint64_t,8> inventoryTerminalDiagnosticKeys{};
    unsigned inventoryInputDiagnostics=0;
    std::array<std::uint64_t,12> inventoryInputDiagnosticKeys{};
    unsigned inventoryGateDiagnostics=0;
    unsigned keyReturnDiagnostics=0;
    std::array<unsigned,2> inventoryDropDiagnostics{};
    std::array<std::array<std::uint64_t,4>,2> inventoryDropDiagnosticKeys{};
    std::uint64_t epoch=10;
    bool preparedValid=true,frameValid=true;
    unsigned logs=0,keyRejects=0;
    static constexpr std::uint32_t kMovieKey=0x01CC5E7C;
    explicit StatusConsumer(StatusVm& transport):vm(transport),host(transport) {
        frameTarget.owner=metadata.status;frameTarget.ownerToken=metadata.token(metadata.status);
        frameTarget.controller=metadata.controller;
        frameTarget.direct.valid=true;frameTarget.direct.movieClass="StatusMenuExGFxMovie";frameTarget.direct.screen=3;
        inventoryOwner=frameTarget;
        vm.vm.put(metadata.top+0x3C,vm.top);vm.vm.put(metadata.backpack+0x3C,vm.panel);vm.vm.put(metadata.equipped+0x3C,vm.equipped);
        inventorySession=std::make_unique<GfxSession>(vm,widget_fixture::base,movie,scratch1,512,true);
        const auto index=metadata.name("LeftMouseButton");
        metadata.memory.word(base+0x03009F68,index);metadata.memory.word(base+0x03009F6C,0);
        vm.nativeMouseIndex=index;vm.nativeMouseUp=[this](float x,float y){++releases;if(x>=0 && y>=0) ++drops;};
    }
    bool current() const {++currentChecks;return alive && allowed;}
    bool targetValid(const Target&,bool) const {return alive;}
    template<class T=std::uint32_t> T read(std::uint32_t p) const {
        T value{};assert(Memory::read(&metadata.memory,p,&value,sizeof(value)));return value;
    }
    std::string name(std::uint32_t p) const {
        const auto id=read<std::uint32_t>(p);
        for(const auto& entry:metadata.nameIds) if(entry.second==id) return entry.first;
        return {};
    }
    void networkDiagnostic(const Target&,const char*) {}
    InventoryDragOwner inventoryIdentity() const {return {inventoryFamily(),10,20,30,40,50,60,70,2560,1440};}
    std::uint64_t monotonicMilliseconds() const {return now;}
    static bool copyMemory(const void* context,std::uint32_t p,void* out,std::size_t n) {
        const auto& sink=*static_cast<const StatusConsumer*>(context);++sink.modelReads;
        return Memory::read(&sink.metadata.memory,p,out,n);
    }
    std::uint64_t tradingInventory(std::int32_t,bool) {assert(false);return 0;}
    std::optional<std::uint32_t> originalQuery(std::uint32_t object,const UiNativeMethod& fn,
        std::initializer_list<std::uint32_t> args,std::uint32_t offset) {
        const bool equipped=object==metadata.equipped;assert(object==metadata.backpack || equipped);
        assert(fn.function==metadata.query[equipped?1:0] && fn.bytes==8 && offset==4 && args.size()==2);
        assert(*args.begin()==(equipped?4u:2u) && *std::next(args.begin())==0xFFFFFFFFu);++queries;
        if(staleOnQuery) metadata.memory.word(metadata.status+0x104,2);
        return metadata.item;
    }
    InventoryWidgetTarget inventoryHit(float x,float y) {
        if(!current()) return {};
        GfxSession fresh(vm,widget_fixture::base,movie,scratch2,512,true);return captureInventory(fresh,x,y);
    }
    void clearInventory() {inventoryPressed=false;inventoryHoverBoundary=false;inventoryStart.reset();inventoryFinal.reset();inventoryStatus={};inventoryStatusPanels={};inventoryTarget={};inventorySession.reset();inventoryTransport.reset();}
    void consumeHover() {
        vm.advanceOriginal();inventoryHoverBoundary=true;
        if(!vm.originalQueue) vm.vm.prop(vm.offers?vm.equipped:vm.panel,"currentOverClip",vm.cell);
    }
    void originalFrame() {
        consumeHover();now+=16;
        const auto owner=inventoryIdentity();
        if(const auto update=inventoryGesture.begin(owner,current(),false,now)) inventoryUpdate(*update);
        if(inventoryGesture.captured() && inventoryTarget && current()) {
            inventoryFresh=freshInventorySource();
            if(!inventoryFresh) {if(const auto update=inventoryGesture.cancel()) inventoryUpdate(*update);else clearInventory();}
        }
        // Same production endTouchFrame ordering: input cancellation is already
        // processed before any deferred native press can be emitted.
        if(inventoryStart && inventoryGesture.captured()) {const auto update=*inventoryStart;inventoryUpdate(update);}
    }
    bool mouse(std::uint32_t type,float x,float y) {
        assert(current());vm.queue(type,x,y);if(type==1) ++moves;else if(type==2) ++presses;else if(type==3) {++releases;++drops;}return true;
    }
    bool mouseFor(const Target&,std::uint32_t type,float x,float y,bool cleanup) {
        assert(alive && cleanup && x==-1 && y==-1);if(type==1) ++moves;else if(type==3) ++releases;return true;
    }
    void tap(const UiTap&) {++clicks;}
#define __ANDROID__ 1
#define HORN_ENABLE_LOGGING 1
#define HORN_LOGI(...) ((void)0)
#include "ui_status_inventory_consumer.inc"
#include "ui_status_inventory_hover.inc"
#undef HORN_ENABLE_LOGGING
#undef HORN_LOGI
#undef __ANDROID__
    void pointer(const PointerEvent& event) {
        auto& s=*this;
#include "ui_status_inventory_pointer.inc"
    }
};

int main(int argc,char** argv) {
    assert(argc==2);
    for(unsigned fault=0;fault<8;++fault) {
        DynarmicHost host;uiNativeReleaseBase=0;
        if(fault<3) host.words[DynarmicHost::base+0x01CC6348+fault*4]^=1;
        else if(fault==3) host.imageValid=false;
        else if(fault==4) host.boundsValid=false;
        else if(fault==5) host.symbolValid=false;
        else if(fault==6) host.refuseRegistration=1;
        assert(InstallUiNativeRelease(host,DynarmicHost::base)==(fault==7));
        assert(host.writes==(fault==7?2u:0u) && host.invalidations==unsigned(fault==7));
        if(fault==7) {
            assert((host.replays==std::vector<std::vector<std::uint32_t>>{{0xE5923000,0xE5933090,0xE51FF004,DynarmicHost::base+0x01CC6350}}));
            assert(InstallUiNativeRelease(host,DynarmicHost::base) && host.writes==2);
            assert(!InstallUiNativeRelease(host,DynarmicHost::base+4) && host.writes==2);
        }
    }
    uiNativeReleaseBase=0;
    for(unsigned fault=0;fault<19;++fault) {
        constexpr std::uint32_t sp=0x100000;
        UiNativeReleaseScope nativeRelease{1,2,3,4,0,5,0,floatWord(1250),floatWord(760),[]{return true;},0,0};
        std::array<std::uint32_t,16> regs{};
        regs[0]=regs[2]=4;regs[1]=sp+0x1C;regs[4]=5;regs[6]=1;regs[7]=3;regs[9]=2;regs[13]=sp;
        std::map<std::uint32_t,std::uint32_t> stack{{sp+0x64,5},{sp+0x68,0},{sp+0x6C,1},
            {sp+0x1C,3},{sp+0x30,0},{sp+0x24,floatWord(0)},{sp+0x28,floatWord(0)}};
        unsigned reads=0,writes=0;
        if(fault==1) regs[13]+=4;
        else if(fault==2) regs[13]=0xFFFF;
        else if(fault==3) regs[13]=0xFFFFFFF8;
        else if(fault==4) regs[6]++;
        else if(fault==5) regs[5]++;
        else if(fault==6) regs[4]++;
        else if(fault==7) regs[0]++;
        else if(fault==8) regs[2]++;
        else if(fault==9) regs[1]++;
        else if(fault>=10 && fault<=14) ++stack[sp+std::array{0x64u,0x68u,0x6Cu,0x1Cu,0x30u}[fault-10]];
        else if(fault==15) nativeRelease.current=[] {return false;};
        else if(fault==16) nativeRelease.current={};
        else if(fault==17) nativeRelease.delivered=1;
        else if(fault==18) nativeRelease.rejected=1;
        const auto original=stack;
        UiNativeReleaseActivation active(nativeRelease);
        const bool sent=UiAdaptNativeRelease(regs,[&](auto p){++reads;return stack.at(p);},[&](auto p,auto v){++writes;stack[p]=v;});
        assert(sent==(fault==0) && writes==(fault==0?2u:0u));
        if(!fault) {
            assert(nativeRelease.delivered==1 && stack.at(sp+0x24)==floatWord(1250) && stack.at(sp+0x28)==floatWord(760));
            assert(!UiAdaptNativeRelease(regs,[&](auto p){return stack.at(p);},[&](auto,auto){assert(false);}));
        } else assert(stack==original && nativeRelease.rejected>=1);
    }
    {
        unsigned reads=0,writes=0;std::array<std::uint32_t,16> regs{};
        assert(UiAdaptNativeRelease(regs,[&](auto){++reads;return 0u;},[&](auto,auto){++writes;}));
        UiNativeReleaseScope outer{};outer.movie=2;outer.gfx=3;
        {UiNativeReleaseActivation active(outer);
         assert(UiAdaptNativeRelease(regs,[&](auto){++reads;return 0u;},[&](auto,auto){++writes;}));
         UiNativeReleaseScope inner{};{UiNativeReleaseActivation nested(inner);assert(uiNativeReleaseScope==&inner);}
         assert(uiNativeReleaseScope==&outer);}
        assert(!uiNativeReleaseScope && !reads && !writes && !outer.delivered && !outer.rejected);
    }
    for(const bool cancel:{false,true}) for(unsigned gate=0;gate<4;++gate) {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);vm.originalQueue=true;
        sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));sink.inventoryPressed=true;
        vm.vm.prop(vm.manager,"_inDrag",boolean(true));
        vm.nativeFiltered=gate==1;vm.nativeDiscarded=gate==2;vm.nativeHandled=gate==3;
        InventoryDragUpdate finish;finish.command=cancel?InventoryDragCommand::Cancel:InventoryDragCommand::Drop;
        finish.source=sink.inventoryTarget.source;finish.x=.6f;finish.y=.5f;
        sink.finishInventory(finish);
        assert(vm.nativeReleases==1 && sink.releases==unsigned(gate==0));
        assert(sink.drops==unsigned(!cancel && gate==0) && vm.cancellations==unsigned(cancel));
        assert(vm.queuedMouseRelease==(gate==1 || gate==2));
        if(!gate) {
            assert(vm.nativeOrder[0]=="native-release" && vm.nativeOrder[1]=="gfx-up-queued");
            if(cancel) assert(vm.nativeOrder[2]=="original-cancel-finish-equip" && !vm.equipDelegate);
            assert(vm.events.back().type==3 && vm.events.back().x==(cancel?-1.f:std::floor(.6f*2559)) &&
                vm.events.back().y==(cancel?-1.f:std::floor(.5f*1439)));
            vm.advanceOriginal();assert(!vm.equipDelegate && !vm.queuedMouseRelease);
            assert(!vm.originalSwitch(vm.nativeMouseIndex+1,0)); // Escape/Left now reach Normal's ordinary route.
        }
    }
    {
        // Exact old-route discriminator: GFx alone completes transfer but
        // FinishEquip(None) leaves the genuine queued trigger untouched.
        StatusVm vm(argv[1]);vm.originalQueue=true;vm.queue(3,1570,370);vm.advanceOriginal();
        assert(vm.queuedMouseRelease && !vm.equipDelegate && vm.originalSwitch(21962,0));
    }
    for(unsigned fault=0;fault<11;++fault) {
        DynarmicHost host;uiKeyReturnProbeBase=0;
        if(fault<6) {auto it=host.words.begin();std::advance(it,fault);it->second^=1;}
        if(fault==6) host.imageValid=false;
        if(fault==7) host.boundsValid=false;
        if(fault==8) host.symbolValid=false;
        if(fault==9 || fault==10) host.refuseRegistration=fault-8;
        assert(!InstallUiKeyReturnProbe(host,DynarmicHost::base));
        assert(!uiKeyReturnProbeBase && !host.writes && !host.invalidations);
    }
    {
        DynarmicHost host;uiKeyReturnProbeBase=0;
        assert(InstallUiKeyReturnProbe(host,DynarmicHost::base));
        assert(host.writes==4 && host.invalidations==2 && host.replays.size()==2);
        assert(host.replays[0]==std::vector<std::uint32_t>({0xE59DA02C,0xE35A0000,0xE51FF004,DynarmicHost::base+0x01CC5FE8}));
        assert(host.replays[1]==std::vector<std::uint32_t>({0xE59D0020,0xE2900000,0xE51FF004,DynarmicHost::base+0x01CB74E8}));
        assert(InstallUiKeyReturnProbe(host,DynarmicHost::base) && host.writes==4);
        assert(!InstallUiKeyReturnProbe(host,DynarmicHost::base+4) && host.writes==4);
        uiKeyReturnProbeBase=0;
    }
    // Actual observer extraction: foreign callbacks, nested scopes and input
    // stack mismatches cannot supply another callback's filter/delegate return.
    {
        std::array<std::uint32_t,16> regs{};regs[13]=0x10000;
        std::array<std::uint32_t,12> stack{};unsigned reads=0;
        const auto read=[&](std::uint32_t p) {++reads;assert(p>=0x10000 && p<0x10030);return stack[(p-0x10000)/4];};
        UiKeyReturnScope scope{0x20000,0x30000,0,21962,0,0,{},{},{}};
        regs[6]=scope.owner;regs[9]=scope.movie;regs[5]=scope.controllerId;regs[4]=scope.index;
        stack[7]=scope.controllerId;stack[8]=scope.index;stack[9]=scope.number;stack[10]=scope.event;stack[11]=0;
        UiObserveKeyReturn(0,regs,read);assert(reads==0);
        {
            UiKeyReturnActivation active(&scope);
            UiObserveKeyReturn(0,regs,read);assert(scope.visits[0]==1 && scope.values[0]==0);
            const auto saved=regs;regs[6]^=4;UiObserveKeyReturn(0,regs,read);regs=saved;
            stack[8]^=1;UiObserveKeyReturn(0,regs,read);stack[8]^=1;
            assert(scope.rejected[0]==2 && scope.visits[0]==1);
            for(unsigned field=0;field<4;++field) {
                stack[7+field]^=1;UiObserveKeyReturn(0,regs,read);stack[7+field]^=1;
            }
            assert(scope.rejected[0]==6 && scope.visits[0]==1);
            UiKeyReturnScope nested=scope;nested.visits={};nested.values={};nested.rejected={};
            {UiKeyReturnActivation activeNested(&nested);UiObserveKeyReturn(0,regs,read);assert(nested.visits[0]==1);}
            assert(uiKeyReturnScope==&scope && scope.visits[0]==1);
            regs[4]=scope.owner;stack[4]=scope.controllerId;stack[5]=scope.index;stack[6]=scope.number;stack[7]=scope.event;stack[8]=1;
            UiObserveKeyReturn(1,regs,read);assert(scope.visits[1]==1 && scope.values[1]==1);
            regs[9]^=4;UiObserveKeyReturn(1,regs,read);regs[9]^=4;assert(scope.rejected[1]==1);
            scope.values[0]=1;UiObserveKeyReturn(1,regs,read);assert(scope.rejected[1]==2 && scope.visits[1]==1);
            scope.values[0]=0;regs[13]+=4;UiObserveKeyReturn(1,regs,read);assert(scope.rejected[1]==3);
        }
        assert(!uiKeyReturnScope);
        // Missing filter visit and wrong delegate owner remain unknown.
        scope.visits={};regs[13]=0x10000;
        {UiKeyReturnActivation active(&scope);UiObserveKeyReturn(1,regs,read);assert(scope.visits[1]==0);}
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);
        diagnosticsProperty="1";diagnosticsLines.clear();uiKeyReturnProbeBase=Fixture::base;
        vm.keySideEffect=[&](std::uint32_t event) {
            assert(uiKeyReturnScope && uiKeyReturnScope->event==event);
            const auto& scope=*uiKeyReturnScope;
            std::array<std::uint32_t,16> regs{};std::array<std::uint32_t,12> stack{};regs[13]=0x10000;
            const auto read=[&](std::uint32_t p) {return stack[(p-0x10000)/4];};
            regs[6]=scope.owner;regs[9]=scope.movie;regs[5]=scope.controllerId;regs[4]=scope.index;
            stack[7]=scope.controllerId;stack[8]=scope.index;stack[9]=scope.number;stack[10]=scope.event;stack[11]=0;
            UiObserveKeyReturn(0,regs,read);
            regs[4]=scope.owner;stack[4]=scope.controllerId;stack[5]=scope.index;stack[6]=scope.number;stack[7]=scope.event;stack[8]=1;
            UiObserveKeyReturn(1,regs,read);
        };
        for(unsigned i=0;i<8;++i) assert(sink.keyReturnCallback(sink.frameTarget,21962,0,i&1)==vm.keyHandled[i&1]);
        assert(vm.keyCalls.size()==8 && diagnosticsLines.size()==8 && sink.keyReturnDiagnostics==8 && !uiKeyReturnScope);
        assert(diagnosticsLines[0].find("filter=(1,00000000,0) delegate=(1,00000001,0)")!=std::string::npos);
        vm.keySideEffect=[&](std::uint32_t) {assert(!uiKeyReturnScope);};
        assert(sink.keyReturnCallback(sink.frameTarget,21962,0,0)==0 && diagnosticsLines.size()==8 && vm.keyCalls.size()==9);
        sink.keyReturnDiagnostics=0;diagnosticsProperty="0";
        sink.keyReturnCallback(sink.frameTarget,21962,0,0);assert(diagnosticsLines.size()==8);
        diagnosticsProperty="1";sink.frameTarget.direct.screen=2;
        sink.keyReturnCallback(sink.frameTarget,21962,0,0);assert(diagnosticsLines.size()==8);
        uiKeyReturnProbeBase=0;diagnosticsProperty="0";diagnosticsLines.clear();
    }
    for(unsigned bits=0;bits<8;++bits) {
        GateFixture f;
        f.memory.word(f.status+0x218,(bits&1)?f.controller:0);
        f.memory.word(f.status+0x21C,(bits&2)?0x20:0);
        f.memory.word(f.controller+0x670,(bits&4)?0x40:0);
        const auto state=f.gate();
        assert(state.find("known=1 reason=gate-owned")!=std::string::npos);
        assert(state.find(std::string("derivedFilter=")+(bits==7?"1":"0"))!=std::string::npos);
        assert(state.find("controllerMatch=1")!=std::string::npos);
    }
    for(const std::uint32_t id:{0u,27u,256u,511u}) {
        GateFixture f;f.memory.word(f.local+0x64,id);
        const auto state=f.gate(id&255u);
        assert(state.find("known=1")!=std::string::npos && state.find("controllerMatch=1")!=std::string::npos);
        assert(state.find("localId="+std::to_string(id)+" getMyId="+std::to_string(id&255u))!=std::string::npos);
        assert(f.gate((id&255u)+1).find("controllerMatch=0")!=std::string::npos);
    }
    for(unsigned field=0;field<6;++field) for(unsigned fault=0;fault<8;++fault) {
        GateFixture f;const auto p=f.gateFields[field];
        if(fault==0) f.memory.word(p+0x40,2);
        else if(fault==1) f.memory.word(p+0x44,8);
        else if(fault==2) f.memory.word(p+0x48,f.memory.word(p+0x48)^1u);
        else if(fault==3) f.memory.word(p+0x4C,1);
        else if(fault==4) f.memory.word(p+0x54,0xFFFFF);
        else if(fault==5) f.memory.word(p+0x28,f.movieBase);
        else if(fault==6) f.memory.word(p+0x34,f.types.at("NameProperty"));
        else f.memory.word(p+0x0C,0x20000002);
        assert(f.gate().find("known=0")!=std::string::npos);
    }
    for(unsigned fault=0;fault<14;++fault) {
        GateFixture f;
        if(fault==0) f.memory.word(f.gateFields[0]+0x74,f.controllerType);
        else if(fault==1) f.memory.word(f.gateFields[1]+0x74,f.playerType);
        else if(fault==2) f.memory.word(f.gateFields[3]+0x74,f.controllerType);
        else if(fault==3) f.memory.word(f.gateFields[2]+0x74,0);
        else if(fault==4) f.memory.word(f.gateFields[4]+0x74,3);
        else if(fault==5) f.memory.word(f.status+0x108,f.backpack);
        else if(fault==6) f.memory.word(f.top+0x214,f.object("Foreign movie",f.inventoryMovie));
        else if(fault==7) f.memory.word(f.status+0x218,f.object("Foreign controller",f.controllerType));
        else if(fault==8) f.memory.word(f.controller+0x66C,f.object("Remote Player",f.playerType));
        else if(fault==9) f.memory.word(f.base+0x01B427B0,0);
        else if(fault==10) f.memory.word(f.base+0x01B427F4,0);
        else if(fault==11) f.memory.word(f.controller+0x0C,0x20000002);
        else if(fault==12) f.memory.word(f.objects+f.memory.word(f.local+0x20)*4,0);
        else {
            const auto shadow=f.property(f.movieType,"WPCOwner","ObjectProperty",0x220,4);
            f.memory.word(shadow+0x48,0x2000);f.memory.word(shadow+0x74,f.controllerType);f.memory.word(f.status+0x220,f.controller);
        }
        assert(f.gate().find("known=0")!=std::string::npos);
    }
    struct GateChangingMemory {
        Memory& memory;std::uint32_t address,value;unsigned reads=0;
        static bool copy(const void* context,std::uint32_t p,void* out,std::size_t n) {
            auto& self=*const_cast<GateChangingMemory*>(static_cast<const GateChangingMemory*>(context));
            if(p==self.address && n==4 && ++self.reads==2) self.memory.word(p,self.value);
            return Memory::read(&self.memory,p,out,n);
        }
    };
    for(unsigned fault=0;fault<7;++fault) {
        GateFixture f;
        const std::array addresses{f.status+0x108,f.top+0x214,f.status+0x218,f.status+0x21C,f.controller+0x66C,f.controller+0x670,f.local+0x64};
        GateChangingMemory changed{f.memory,addresses[fault],fault==3?0x20u:fault==5?0x40u:fault==6?27u:0u};
        const auto state=CaptureBl2StatusInventoryGate({&changed,GateChangingMemory::copy},f.base,f.status,f.controller,0);
        assert(state.find("known=0 reason=gate-changed")!=std::string::npos);
    }
    {
        GateFixture f;
        for(unsigned i=0;i<400;++i) f.property(f.topType,"UnusedGateTop_"+std::string(80,'x')+std::to_string(i),"IntProperty",0x240,4);
        const auto prior=CaptureBl2StatusInventoryInputState({&f.memory,Memory::read},f.base,f.status);
        assert(!prior.valid && prior.reason.find("read-budget")!=std::string::npos);
        assert(f.gate().find("known=1 reason=gate-owned")!=std::string::npos); // Fresh Gate budget, one walk.
    }
    {
        GateFixture f;f.growTables(8192);
        // Real cooked WillowPlayerController has2,387 direct UFields. Long
        // legal identifiers also prove that the new diagnostic budget is
        // adequate without changing the old functional reader's limits.
        for(unsigned i=0;i<2387;++i) f.property(f.controllerType,"OriginalControllerChild_"+std::string(80,'x')+std::to_string(i),"IntProperty",0x680,4);
        const auto state=f.gate();assert(state.find("known=1 reason=gate-owned")!=std::string::npos);
        const auto bytes=std::stoul(state.substr(state.rfind(',')+1));assert(bytes>131072 && bytes<=524288);
    }
    {
        GateFixture f;f.growTables(8192);
        for(unsigned i=0;i<4097;++i) f.property(f.controllerType,"OverLimit_"+std::to_string(i),"IntProperty",0x680,4);
        const auto state=f.gate();assert(state.find("known=0")!=std::string::npos && state.find("field-cycle-or-limit")!=std::string::npos);
    }
    {
        GateFixture f;f.growTables(8192);
        for(unsigned i=0;i<4090;++i) f.property(f.controllerType,"BudgetBound_"+std::string(100,'x')+std::to_string(i),"IntProperty",0x680,4);
        const auto state=f.gate();assert(state.find("known=0")!=std::string::npos && state.find("read-budget")!=std::string::npos);
    }
    {
        GateFixture f;f.memory.blocks.erase(f.local);
        const auto state=f.gate();assert(state.find("known=0")!=std::string::npos && state.find("unreadable")!=std::string::npos);
    }
    // The real keyName method is extracted, including its original callback
    // pair. Logging must reflect both return values without an added call.
    for(const bool diagnostic:{false,true}) for(const bool lost:{false,true}) {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);diagnosticsProperty=diagnostic?"1":"0";diagnosticsLines.clear();
        sink.metadata.memory.word(sink.base+0x03009FB8,sink.metadata.name("Escape"));
        sink.metadata.memory.word(sink.base+0x03009FB8+4,0);
        vm.keyHandled={7,0};if(lost) vm.keySideEffect=[&](std::uint32_t event){if(event==0) sink.alive=false;};
        assert(sink.keyName(0x03009FB8,"Escape")==!lost);
        assert(vm.keyCalls.size()==(lost?1u:2u) && vm.keyCalls[0][5]==0);
        assert(vm.keyCalls[0][1]==sink.frameTarget.controllerId);
        if(!lost) assert(vm.keyCalls[1][5]==1);
        const auto gateLines=std::count_if(diagnosticsLines.begin(),diagnosticsLines.end(),[](const auto& line) {
            return line.find("inventory-key-gate ")==0;
        });
        assert(gateLines==(diagnostic?(lost?1:2):0));
        if(diagnostic) {
            assert(std::any_of(diagnosticsLines.begin(),diagnosticsLines.end(),[](const auto& line) {
                return line.find("event=0 handled=7")!=std::string::npos && line.find("known=1 reason=gate-owned")!=std::string::npos;
            }));
            if(!lost) assert(std::any_of(diagnosticsLines.begin(),diagnosticsLines.end(),[](const auto& line) {
                return line.find("event=1 handled=0")!=std::string::npos;
            }));
        }
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);diagnosticsProperty="1";diagnosticsLines.clear();
        sink.metadata.memory.word(sink.base+0x03009FB8,sink.metadata.name("Escape"));sink.metadata.memory.word(sink.base+0x03009FB8+4,0);
        for(unsigned i=0;i<20;++i) assert(sink.keyName(0x03009FB8,"Escape"));
        assert(vm.keyCalls.size()==40 && sink.inventoryGateDiagnostics==8);
        assert(std::count_if(diagnosticsLines.begin(),diagnosticsLines.end(),[](const auto& line) {return line.find("inventory-key-gate ")==0;})==8);
        const auto reads=sink.modelReads;sink.inventoryGateObservation(sink.frameTarget);assert(reads==sink.modelReads);
    }
    diagnosticsProperty="0";diagnosticsLines.clear();
    // First-enter BP hover is genuinely NULL. The extracted production pointer
    // route must forward accepted real MOVE, not retry the original origin.
    for(const bool continuous:{false,true}) {
        StatusVm vm(argv[1]);vm.originalQueue=vm.rolloverOnly=true;
        {StatusConsumer sink(vm);sink.inventorySession.reset();
         sink.inventoryGesture.begin(sink.inventoryIdentity(),true,false,sink.now);
         sink.pointer({3,PointerPhase::Down,.2f,.4f,10,1,sink.now});
         assert(sink.inventoryGesture.captured() && !sink.presses);
         sink.originalFrame();
         assert(vm.moveInstalled && vm.vm.object(vm.panel).properties.at("currentOverClip")[3]==1);
         sink.pointer({3,PointerPhase::Move,.2f,.4f,10,2,sink.now});
         sink.originalFrame();
         assert(vm.vm.object(vm.panel).properties.at("currentOverClip")[3]==1 && !sink.presses);
         const auto idleMoves=sink.moves;
         for(unsigned i=0;i<4;++i) sink.originalFrame();
         assert(sink.moves==idleMoves && !sink.presses); // No frame-generated replay.
         sink.pointer({3,PointerPhase::Move,.212f,.4f,10,3,sink.now});
         assert(vm.pendingMove && sink.moves==idleMoves+1 && !sink.presses && !sink.inventoryHoverBoundary);
         assert(vm.events.back().type==1 && vm.events.back().x==std::floor(.212f*2559) && vm.events.back().y==std::floor(.4f*1439));
         sink.pointer({3,PointerPhase::Move,.22f,.4f,9,100,sink.now}); // Old epoch cannot forward or steal the pending point.
         assert(sink.moves==idleMoves+1 && vm.pendingX==std::floor(.212f*2559));
         const auto pending=*sink.inventoryStart;sink.inventoryUpdate(pending);
         assert(sink.moves==idleMoves+1 && !sink.presses); // Same-frame retry is silent.
         if(continuous) {
             sink.consumeHover();sink.now+=16;
             sink.inventoryGesture.begin(sink.inventoryIdentity(),true,false,sink.now);
             sink.inventoryFresh=sink.freshInventorySource();
             sink.pointer({3,PointerPhase::Move,.214f,.4f,10,4,sink.now});
             assert(sink.presses==1); // Next MOVE does not starve a valid fresh hover.
         } else sink.originalFrame();
         assert(sink.presses==1 && sink.inventoryPressed);
         sink.originalFrame();
         assert(vm.vm.object(vm.manager).properties.at("_inDrag")==boolean(true) && sink.inventoryDragData.data==vm.dragData[4]);
         const auto accepted=vm.events.size();
         for(unsigned i=0;i<3;++i) sink.originalFrame();
         assert(vm.events.size()==accepted && sink.presses==1);
         sink.pointer({3,PointerPhase::Cancel,.214f,.4f,10,5,sink.now});
         assert(vm.cancellations==1 && sink.releases==1 && !sink.drops && !sink.clicks);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    // A genuine source hover must not authorize a press after the latest real
    // sample leaves that source. The new hit guard uses actual native capture.
    {
        StatusVm vm(argv[1]);vm.originalQueue=vm.rolloverOnly=true;
        {StatusConsumer sink(vm);sink.inventorySession.reset();
         sink.inventoryGesture.begin(sink.inventoryIdentity(),true,false,sink.now);
         sink.pointer({3,PointerPhase::Down,.2f,.4f,10,1,sink.now});sink.originalFrame();
         sink.pointer({3,PointerPhase::Move,.212f,.4f,10,2,sink.now});
         sink.consumeHover();sink.now+=16;
         sink.inventoryGesture.begin(sink.inventoryIdentity(),true,false,sink.now);
         sink.inventoryFresh=sink.freshInventorySource();
         assert(vm.vm.object(vm.panel).properties.at("currentOverClip")==vm.cell);
         sink.pointer({3,PointerPhase::Move,.6f,.4f,10,3,sink.now});
         assert(!sink.presses && !sink.inventoryHoverBoundary && vm.pendingX==std::floor(.6f*2559));
         sink.originalFrame();
         assert(vm.vm.object(vm.panel).properties.at("currentOverClip")[3]==1 && !sink.presses);
         sink.pointer({3,PointerPhase::Cancel,.6f,.4f,10,4,sink.now});
         assert(!sink.presses && !sink.releases && !vm.cancellations && !sink.clicks);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    for(unsigned fault=0;fault<12;++fault) {
        StatusVm vm(argv[1]);vm.originalQueue=vm.rolloverOnly=true;
        {StatusConsumer sink(vm);sink.inventorySession.reset();
         sink.inventoryGesture.begin(sink.inventoryIdentity(),true,false,sink.now);
         sink.pointer({3,PointerPhase::Down,.2f,.4f,10,1,sink.now});sink.originalFrame();
         sink.pointer({3,PointerPhase::Move,.212f,.4f,10,2,sink.now});
         assert(sink.inventoryStart && !sink.presses);
         const auto forwarded=sink.moves;
         if(fault<3) sink.pointer({fault==1?4:3,
             fault==1?PointerPhase::Down:fault==2?PointerPhase::Up:PointerPhase::Cancel,.212f,.4f,10,3,sink.now});
         else if(fault==3) {
             auto owner=sink.inventoryIdentity();++owner.generation;
             const auto cancel=sink.inventoryGesture.begin(owner,true,false,sink.now);assert(cancel);sink.inventoryUpdate(*cancel);
         } else if(fault==4) sink.alive=false;
         else if(fault==5) sink.allowed=false;
         else if(fault==6) vm.vm.prop(vm.cell,"_visible",boolean(false));
         else if(fault==7) sink.metadata.item=sink.metadata.object("Replacement before press",sink.metadata.weaponType);
         else if(fault==8) {vm.vm.prop(vm.manager,"_inDrag",boolean(true));vm.vm.prop(vm.manager,"pressOrigin",vm.top);}
         else if(fault==9) vm.vm.prop(vm.manager,"_inDrag",num(1));
         else if(fault==10) sink.metadata.memory.word(sink.metadata.status+0x104,2);
         else sink.pointer({3,PointerPhase::Move,.214f,.4f,10,2,sink.now}); // Replay cancels, not forwards.
         sink.originalFrame();
         assert(!sink.presses && sink.moves==forwarded && !sink.releases && !sink.drops && !sink.clicks && !vm.cancellations);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    // Genuine BP press depends on a consumed hover, not the mouse button's
    // position. Same-batch DOWN/MOVE must wait; no forced original Advance.
    for(const bool equipped:{false,true}) {
        StatusVm vm(argv[1]);if(equipped) vm.equipSource();vm.originalQueue=true;
        {StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
         const auto source=sink.inventoryTarget.source;const auto owner=sink.inventoryIdentity();
         sink.inventoryGesture.begin(owner,true,false,100);
         sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
         const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
         assert(move);sink.inventoryUpdate(*move);
#ifndef BL2_NATIVE_HOVER_NEGATIVE
         assert(!sink.presses && sink.inventoryStart && vm.pendingMove && vm.buttons.empty());
#endif
         sink.originalFrame();sink.originalFrame();
         assert(vm.vm.object(vm.manager).properties.at("_inDrag")==boolean(true));
         assert(sink.inventoryPressed && sink.presses==1 && sink.inventoryDragData.data==vm.dragData[4]);
         assert(sink.freshInventorySource()==source); // Real own ACK after original queue consumption.
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
#ifdef BL2_NATIVE_HOVER_NEGATIVE
    return 0;
#endif
    // Live25 BP waits before native DOWN. Observe the already queried hover's
    // type/identity without another GFx query or altering that strict gate.
    const auto hoverTrace=[&](bool diagnostic,unsigned sample) {
        diagnosticsProperty=diagnostic?"1":"0";diagnosticsLines.clear();
        StatusVm vm(argv[1]);std::array<unsigned,10> counts{};
        {
            StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
            const auto source=sink.inventoryTarget.source;const auto owner=sink.inventoryIdentity();
            sink.inventoryGesture.begin(owner,true,false,100);
            sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
            sink.consumeHover();Raw observed{};
            if(sample==1) observed[3]=1; // Original NULL, not an AS display object.
            else if(sample==2) observed=num(1);
            else if(sample==3) observed=vm.renderer;
            else if(sample==4) observed=vm.top;
            else if(sample==5) observed=vm.cell;
            vm.vm.prop(vm.panel,"currentOverClip",observed);
            diagnosticsLines.clear();
            const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
            assert(move);sink.inventoryUpdate(*move);
            assert(sink.presses==unsigned(sample==5) && sink.inventoryPressed==(sample==5));
            assert(!sink.releases && !sink.drops && !sink.clicks && !vm.cancellations);
            if(diagnostic) {
                const auto found=std::find_if(diagnosticsLines.begin(),diagnosticsLines.end(),[&](const auto& entry) {
                    return entry.find(sample==5?"reason=original-down-queued":"reason=waiting-original-hover")!=std::string::npos;
                });
                assert(found!=diagnosticsLines.end());const auto& line=*found;
                char hover[80]{},expected[80]{},renderer[80]{};
                std::snprintf(hover,sizeof(hover),"hover=(%u,%08X,%08X)",observed[3]&0x8F,
                    sample>=3?observed[2]:0,sample>=3?observed[4]:0);
                std::snprintf(expected,sizeof(expected),"expectedCell=(%u,%08X,%08X)",source.cell.type,source.cell.interface,source.cell.data);
                std::snprintf(renderer,sizeof(renderer),"renderer=(%u,%08X,%08X)",
                    sink.inventoryTarget.rendererId.type,sink.inventoryTarget.rendererId.interface,sink.inventoryTarget.rendererId.data);
                assert(line.find(hover)!=std::string::npos && line.find(expected)!=std::string::npos && line.find(renderer)!=std::string::npos);
                assert(line.find(sample==5?"reason=original-down-queued":"reason=waiting-original-hover")!=std::string::npos);
            } else assert(diagnosticsLines.empty());
            counts={vm.reads,vm.writes,vm.calls,sink.modelReads,sink.currentChecks,sink.queries,
                sink.presses,sink.moves,sink.releases,sink.drops};
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);return counts;
    };
    for(unsigned sample=0;sample<6;++sample) assert(hoverTrace(false,sample)==hoverTrace(true,sample));
    diagnosticsProperty="0";diagnosticsLines.clear();
    // A consumed foreign/unknown hover, source loss, manager takeover or ended
    // physical intent never becomes a delayed original mouse press.
    for(unsigned fault=0;fault<10;++fault) {
        StatusVm vm(argv[1]);vm.originalQueue=true;
        {StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
         const auto source=sink.inventoryTarget.source;auto owner=sink.inventoryIdentity();
         sink.inventoryGesture.begin(owner,true,false,100);
         sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
         const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
         assert(move);sink.inventoryUpdate(*move);assert(sink.inventoryStart && !sink.presses);
         if(fault<3) {
             const auto end=sink.inventoryGesture.pointer({fault==1?4:3,
                 fault==1?PointerPhase::Down:fault==2?PointerPhase::Up:PointerPhase::Cancel,.5f,.5f,10,3,100},source);
             assert(end);sink.inventoryUpdate(*end);
         } else if(fault==3) {++owner.epoch;const auto end=sink.inventoryGesture.begin(owner,true,false,100);assert(end);sink.inventoryUpdate(*end);}
         else if(fault==4) sink.allowed=false;
         else if(fault==5) vm.vm.prop(vm.cell,"_visible",boolean(false));
         else if(fault==6) {vm.vm.prop(vm.manager,"_inDrag",boolean(true));vm.vm.prop(vm.manager,"pressOrigin",vm.top);}
         else if(fault==7) vm.vm.prop(vm.manager,"_inDrag",num(1));
         else if(fault==8) {
             sink.consumeHover();vm.vm.prop(vm.panel,"currentOverClip",vm.top);
             const auto pending=*sink.inventoryStart;sink.inventoryUpdate(pending);
             assert(sink.inventoryStart && !sink.presses);
             const auto end=sink.inventoryGesture.cancel();assert(end);sink.inventoryUpdate(*end);
         } else sink.metadata.item=sink.metadata.object("Replaced before hover",sink.metadata.weaponType);
         sink.originalFrame();
         assert(!sink.presses && !sink.releases && !sink.drops && !sink.clicks && !vm.cancellations);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    // Reflection uses registered declaring fields, 12-byte delegates, 8-byte
    // names and actual one-bit Bool masks. It never changes their values.
    for(unsigned fault=0;fault<24;++fault) {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);auto& m=sink.metadata;
        const auto calls=vm.calls,writes=vm.writes;
        if(fault>=1 && fault<=5) m.memory.word(m.inputFields[fault-1]+0x28,m.backpackType);
        else if(fault>=6 && fault<=10) m.memory.word(m.inputFields[fault-6]+0x40,2);
        else if(fault==11) m.memory.word(m.inputFields[0]+0x34,m.types.at("ObjectProperty"));
        else if(fault==12) m.memory.word(m.inputFields[1]+0x44,8);
        else if(fault==13) m.memory.word(m.inputFields[2]+0x44,4);
        else if(fault==14) m.memory.word(m.inputFields[0]+0x48,0);
        else if(fault==15) m.memory.word(m.inputFields[1]+0x4C,1);
        else if(fault==16) m.memory.word(m.inputFields[2]+0x48,0x80);
        else if(fault==17) m.memory.word(m.inputFields[3]+0x74,0);
        else if(fault==18) m.memory.word(m.inputFields[4]+0x74,3);
        else if(fault==19) m.memory.word(m.inputFields[0]+0x54,0x181);
        else if(fault==20) m.memory.word(m.top+0x180,m.item+4);
        else if(fault==21) m.memory.word(m.top+0x184,0xFFFFFFFFu);
        else if(fault==22) m.memory.word(m.top+0x34,m.backpackType);
        else if(fault==23) m.memory.word(m.status+0x0C,2);
        const auto state=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
        assert(state.valid==(fault==0));
        if(!fault) {
            assert(state.owner==m.token(m.status) && state.top==m.token(m.top));
            assert(state.delegateObject==state.top && state.queuedObject==state.top);
            assert(state.delegateName=="NormalInputKey" && state.queuedName=="EquipInputKey" && state.triggerName=="None");
            assert(!state.doingEquip && state.setupFinished);
            assert(state.movieValid && state.movieDelegateObject==state.owner && state.movieDelegateName=="InventoryPanelInputKey" &&
                state.movieDelegateClass=="StatusMenuExGFxMovie" && state.localPlayerOwnerIndex==0 && state.captureInput && !state.discardNonOwnerInput);
            m.memory.word(m.top+0x1A0,1);
            const auto changed=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
            assert(changed.valid && changed.doingEquip && !changed.setupFinished);
        }
        assert(!sink.presses && !sink.releases && !sink.drops && !sink.clicks && !vm.cancellations && vm.calls==calls && vm.writes==writes);
    }
    // The outer Movie gate is distinct from Top's Normal/equip=false state.
    // Read exact declaring metadata and report NULL/off/filter states honestly.
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);auto& m=sink.metadata;
        // Real linked metadata, not an injected reader failure: repeated Top
        // property scans exhaust its131072-byte budget before Movie fields.
        for(unsigned i=0;i<400;++i)
            m.property(m.topType,"UnusedTop_"+std::string(80,'x')+std::to_string(i),"IntProperty",0x240,4);
        const auto calls=vm.calls,writes=vm.writes;
        const auto state=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
        assert(!state.valid && state.reason.find("read-budget/bytes=")!=std::string::npos);
        assert(state.movieValid && state.movieDelegateObject==m.token(m.status) &&
            state.movieDelegateName=="InventoryPanelInputKey" && state.localPlayerOwnerIndex==0 && state.captureInput && !state.discardNonOwnerInput);
        assert(state.movieReason.find("movie-input-owned/bytes=")==0 && state.movieReason.find("/top-bytes=")!=std::string::npos);
        assert(vm.calls==calls && vm.writes==writes && !sink.presses && !sink.releases && !sink.clicks);
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);auto& m=sink.metadata;
        // Native32: four inherited Movie lookups exhausted131072, even with
        // Top separately known. A single targeted walk must preserve the exact
        // metadata/declaring guards and stay inside the same original budget.
        for(unsigned i=0;i<400;++i)
            m.property(m.movieType,"UnusedMovie_"+std::string(80,'x')+std::to_string(i),"IntProperty",0x240,4);
        const auto calls=vm.calls,writes=vm.writes;
        const auto state=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
        assert(state.valid && state.movieValid && state.movieDelegateName=="InventoryPanelInputKey" &&
            state.localPlayerOwnerIndex==0 && state.captureInput && !state.discardNonOwnerInput);
        const auto at=state.movieReason.find("/bytes=");assert(at!=std::string::npos);
        assert(std::stoul(state.movieReason.substr(at+7))<131072);
        assert(vm.calls==calls && vm.writes==writes && !sink.presses && !sink.releases && !sink.clicks);
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);auto& m=sink.metadata;
        // Independent readers remain bounded on two legitimate long linked
        // class chains; neither reader accepts a partially observed default.
        const auto parent=m.type("MovieIntermediate",0x380,m.movieBase);
        m.memory.word(m.movieType+0x40,parent);
        for(const auto type:{m.movieType,parent}) for(unsigned i=0;i<400;++i)
            m.property(type,"UnusedMovie_"+std::string(110,'x')+std::to_string(i),"IntProperty",0x240,4);
        const auto state=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
        assert(!state.movieValid && state.movieReason.find("read-budget/bytes=")!=std::string::npos &&
            state.localPlayerOwnerIndex==-1);
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);auto& m=sink.metadata;
        // An inaccessible Movie metadata object is distinct from the Top's
        // byte budget. Its default field values remain explicitly unknown.
        m.memory.blocks.erase(m.movieInputFields[2]);
        const auto state=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
        assert(state.valid && !state.movieValid && state.movieReason.find("/unreadable/bytes=")!=std::string::npos &&
            state.movieReason.find("read-budget")==std::string::npos && state.localPlayerOwnerIndex==-1);
    }
    for(unsigned fault=0;fault<26;++fault) {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);auto& m=sink.metadata;
        const auto calls=vm.calls,writes=vm.writes;
        if(fault>=1 && fault<=4) m.memory.word(m.movieInputFields[fault-1]+0x28,m.topType);
        else if(fault>=5 && fault<=8) m.memory.word(m.movieInputFields[fault-5]+0x40,2);
        else if(fault==9) m.memory.word(m.movieInputFields[0]+0x34,m.types.at("ObjectProperty"));
        else if(fault==10) m.memory.word(m.movieInputFields[1]+0x34,m.types.at("ByteProperty"));
        else if(fault==11) m.memory.word(m.movieInputFields[0]+0x44,8);
        else if(fault==12) m.memory.word(m.movieInputFields[1]+0x44,1);
        else if(fault==13) m.memory.word(m.movieInputFields[0]+0x48,0x00400000);
        else if(fault==14) m.memory.word(m.movieInputFields[1]+0x48,0);
        else if(fault==15) m.memory.word(m.movieInputFields[2]+0x48,0x2000);
        else if(fault==16) m.memory.word(m.movieInputFields[3]+0x4C,1);
        else if(fault==17) m.memory.word(m.movieInputFields[2]+0x74,0);
        else if(fault==18) m.memory.word(m.movieInputFields[3]+0x74,3);
        else if(fault==19) m.memory.word(m.movieInputFields[0]+0x54,0x201);
        else if(fault==20) m.memory.word(m.status+0x200,m.item+4);
        else if(fault==21) m.memory.word(m.status+0x204,0xFFFFFFFFu);
        else if(fault==22) {
            const auto duplicate=m.property(m.movieType,"__OnInputKey__Delegate","DelegateProperty",0x200,12);
            m.memory.word(duplicate+0x48,0x00402000); // Same name on the wrong declaring class is not GFxUI312.
        } else if(fault==23) m.memory.word(m.movieBase+0x2C,m.name("GFxAction_OpenMovie"));
        else if(fault==24) m.memory.word(m.movieInputFields[3]+0x54,0x37F);
        else if(fault==25) {m.memory.word(m.status+0x200,m.item);m.memory.word(m.item+0xC,2);}
        const auto state=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
        assert(state.valid && state.movieValid==(fault==0));
        assert(state.delegateName=="NormalInputKey" && !state.doingEquip && state.setupFinished);
        assert(!sink.presses && !sink.releases && !sink.drops && !sink.clicks && !vm.cancellations && vm.calls==calls && vm.writes==writes);
    }
    for(unsigned sample=0;sample<4;++sample) {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);auto& m=sink.metadata;
        if(sample==0) {m.memory.word(m.status+0x200,0);m.memory.word(m.status+0x204,m.name("None"));}
        else if(sample==1) m.memory.word(m.status+0x210,8);
        else if(sample==2) m.memory.word(m.status+0x20C,1);
        else {m.memory.word(m.status+0x200,m.top);m.memory.word(m.status+0x204,m.name("NormalInputKey"));}
        const auto state=CaptureBl2StatusInventoryInputState({&m.memory,Memory::read},StatusConsumer::base,m.status);
        assert(state.valid && state.movieValid);
        if(sample==0) assert(!state.movieDelegateObject && state.movieDelegateName=="None" && state.movieDelegateClass=="None");
        else if(sample==1) assert(!state.captureInput && state.discardNonOwnerInput);
        else if(sample==2) assert(state.localPlayerOwnerIndex==1);
        else assert(state.movieDelegateObject==state.top && state.movieDelegateClass=="StatusMenuInventoryPanelGFxObject");
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);diagnosticsLines.clear();diagnosticsProperty="0";
        const auto reads=sink.modelReads,calls=vm.calls,writes=vm.writes;
        sink.inventoryInputDiagnostic("disabled",sink.frameTarget);assert(sink.modelReads==reads && diagnosticsLines.empty());
        diagnosticsProperty="1";
        for(unsigned i=0;i<100;++i) sink.inventoryInputDiagnostic("dedup",sink.frameTarget);
        assert(diagnosticsLines.size()==1);
        assert(diagnosticsLines.front().find("movieInput=(1,movie-input-owned/bytes=")!=std::string::npos &&
            diagnosticsLines.front().find(",InventoryPanelInputKey,StatusMenuExGFxMovie)")!=std::string::npos &&
            diagnosticsLines.front().find("localOwner=0 capture=1 discard=0")!=std::string::npos);
        for(unsigned i=0;i<20;++i) sink.inventoryInputDiagnostic(std::to_string(i).c_str(),sink.frameTarget);
        assert(sink.inventoryInputDiagnostics==12 && diagnosticsLines.size()==12);
        const auto exhausted=sink.modelReads;sink.inventoryInputDiagnostic("exhausted",sink.frameTarget);
        assert(sink.modelReads==exhausted && vm.calls==calls && vm.writes==writes && !sink.presses && !sink.releases && !sink.clicks);
    }
    // Original DropTarget is read in an isolated session. No setters, lazy
    // instance getter, transfer/cancel callback, functional scratch or budget.
    for(unsigned mode=0;mode<12;++mode) {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);diagnosticsProperty="0";diagnosticsLines.clear();
        sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
        vm.vm.prop(vm.manager,"_inDrag",boolean(true));
        InventoryDragObservation observed{};
        assert(Bl2InventoryDragStarted(*sink.inventorySession,sink.inventoryTarget,nullptr,nullptr,&observed));
        Raw nil{};nil[3]=1;Raw drop=nil;
        if(mode==1 || mode==7 || mode==8) {
            drop=vm.vm.object(vm.equipped).properties.at("cell2");vm.vm.prop(drop,"_name",vm.vm.str("cell2"));
        } else if(mode==2) {drop=vm.panel;vm.vm.prop(drop,"_name",vm.vm.str("storagePanel"));}
        else if(mode==3 || mode==4) {
            drop=vm.vm.make(10);vm.vm.visible(drop,mode==3?vm.panel:vm.vm.root);vm.vm.prop(drop,"_name",vm.vm.str("entry4"));
        } else if(mode==5) drop=num(5);
        vm.vm.prop(vm.manager,"dropTarget",drop);
        vm.failDrop=mode==6;vm.replaceDrop=mode==7;vm.replacePayload=mode==8;
        if(mode==9) vm.vm.prop(vm.managerClass,"_instance",Raw{});
        if(mode==10) vm.vm.prop(vm.manager,"pressOrigin",vm.vm.root);
        if(mode==11) sink.alive=false;
        const auto actions=std::array{sink.presses,sink.moves,sink.releases,sink.drops,sink.queries,vm.cancellations,vm.scriptInvokes,vm.memberSets};
        const auto functionalCalls=sink.inventorySession->calls();const auto movieRefs=vm.vm.movieRefs;
        const bool functionalGood=sink.inventorySession->good();
        const auto scratch=sink.inventoryTransport->scratch();
        const auto scratchImage=[&] {
            std::vector<std::pair<std::uint32_t,std::uint8_t>> image;
            for(const auto& byte:vm.vm.memory) if(byte.first>=scratch && byte.first-scratch<GfxSession::kScratchBytes) image.push_back(byte);
            std::sort(image.begin(),image.end());return image;
        };
        const auto beforeScratch=scratchImage();
        const auto calls=vm.calls; sink.inventoryDropDiagnostic("own-ack",observed);
        assert(vm.calls==calls && diagnosticsLines.empty() && !sink.inventoryDropDiagnostics[0]);
        diagnosticsProperty="1";
        for(unsigned i=0;i<100;++i) sink.inventoryDropDiagnostic("own-ack",observed);
        assert(diagnosticsLines.size()==1 && sink.inventoryDropDiagnostics[0]==1);
        const auto& line=diagnosticsLines.front();
        assert(line.find("inventory-drop stage=own-ack")!=std::string::npos && line.find("boundary=before-native-up")!=std::string::npos);
        assert((line.find("known=1")!=std::string::npos)==(mode<=4));
        if(mode==0) assert(line.find("reason=drop-null")!=std::string::npos && line.find("relation=null")!=std::string::npos);
        else if(mode==1) assert(line.find("name=cell2 relation=equipped-cell")!=std::string::npos);
        else if(mode==2) assert(line.find("relation=storage-panel")!=std::string::npos);
        else if(mode==3) assert(line.find("relation=storage-child")!=std::string::npos);
        else if(mode==4) assert(line.find("relation=other")!=std::string::npos);
        assert(sink.inventorySession->calls()==functionalCalls && sink.inventorySession->good()==functionalGood && scratchImage()==beforeScratch && vm.vm.movieRefs==movieRefs);
        assert((actions==std::array{sink.presses,sink.moves,sink.releases,sink.drops,sink.queries,vm.cancellations,vm.scriptInvokes,vm.memberSets}));
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);diagnosticsProperty="0";diagnosticsLines.clear();
        sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));vm.vm.prop(vm.manager,"_inDrag",boolean(true));
        Raw nil{};nil[3]=1;vm.vm.prop(vm.manager,"dropTarget",nil);diagnosticsProperty="1";
        InventoryDragObservation observed{};
        for(unsigned i=0;i<12;++i) {
            vm.vm.prop(vm.manager,"_data",vm.vm.make(8));
            assert(Bl2InventoryDragStarted(*sink.inventorySession,sink.inventoryTarget,nullptr,nullptr,&observed));
            sink.inventoryDropDiagnostic("own-ack",observed);
        }
        assert(sink.inventoryDropDiagnostics[0]==4 && diagnosticsLines.size()==4 && !sink.inventoryDropDiagnostics[1]);
        for(unsigned i=0;i<12;++i) {
            vm.vm.prop(vm.manager,"_data",vm.vm.make(8));
            assert(Bl2InventoryDragStarted(*sink.inventorySession,sink.inventoryTarget,nullptr,nullptr,&observed));
            sink.inventoryDropDiagnostic("pre-up",observed);
        }
        assert(sink.inventoryDropDiagnostics[1]==4 && diagnosticsLines.size()==8 && !sink.inventoryDropDiagnosticEnabled());
        const auto calls=vm.calls,reads=vm.reads,writes=vm.writes;
        sink.inventoryDropDiagnostic("pre-up",observed);
        assert(vm.calls==calls && vm.reads==reads && vm.writes==writes && diagnosticsLines.size()==8);
    }
    // Diagnostics may observe the same failing lazy-instance gate, but must
    // never construct a manager, add a query, mutate a cell or release a key.
    const auto trace=[&](bool diagnostic,unsigned fault,bool equipped) {
        diagnosticsProperty=diagnostic?"1":"0";diagnosticsLines.clear();
        StatusVm vm(argv[1]);if(equipped) vm.equipSource();
        std::array<unsigned,10> counts{};
        {
            StatusConsumer sink(vm);sink.inventorySession.reset();
            sink.inventoryDropDiagnostics={4,4}; // Isolate the retained scalar-only logger parity check.
            if(fault==1) vm.vm.prop(vm.managerClass,"_instance",Raw{});
            else if(fault==2) vm.vm.prop(vm.managerClass,"_instance",num(1));
            else if(fault==3) vm.vm.prop(vm.top,"_visible",boolean(false));
            else if(fault==4) vm.selectable=false;
            else if(fault==5) sink.staleOnQuery=true;
            else if(fault==6) vm.vm.prop(vm.manager,"_inDrag",num(1));
            const bool down=sink.inventoryDown(.2f,.4f);
            assert(down==(fault==0));
            if(down) {
                const auto source=sink.inventoryTarget.source;
                const InventoryDragOwner owner{sink.inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
                sink.inventoryGesture.begin(owner,true,false,100);
                sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
                sink.consumeHover();const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
                assert(move);sink.inventoryUpdate(*move);
                vm.vm.prop(vm.manager,"_inDrag",boolean(true));sink.now=116;
                assert(sink.freshInventorySource()==source);
                const auto up=sink.inventoryGesture.pointer({3,PointerPhase::Up,.6f,.5f,10,3,100},source);
                assert(up);sink.inventoryUpdate(*up);
                assert(sink.drops==1 && !vm.cancellations && !sink.clicks);
            }
            counts={vm.reads,vm.writes,vm.calls,sink.modelReads,sink.currentChecks,sink.queries,
                sink.presses,sink.moves,sink.releases,sink.drops};
            if(diagnostic) {
                assert(!diagnosticsLines.empty() && diagnosticsLines.size()<=32);
                if(fault==1) {
                    bool lazy=false;
                    for(const auto& line:diagnosticsLines)
                        lazy|=line.find("reason=manager-instance")!=std::string::npos && line.find("managerTypes=(8,0,0,0)")!=std::string::npos;
                    assert(lazy);
                }
            } else assert(diagnosticsLines.empty());
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
        return counts;
    };
    for(const bool equipped:{false,true}) for(unsigned fault=0;fault<7;++fault) {
        const auto plain=trace(false,fault,equipped),observed=trace(true,fault,equipped);
        auto withoutDiagnosticReads=observed;
        assert(observed[3]>=plain[3]);withoutDiagnosticReads[3]=plain[3];
        // The new state log deliberately adds only bounded native metadata
        // reads; all other queries/GFx writes/current/action counts stay exact.
        if(plain!=withoutDiagnosticReads) {
            std::fprintf(stderr,"diagnostic parity equipped=%u fault=%u plain=",equipped?1u:0u,fault);
            for(const auto n:plain) std::fprintf(stderr,"%u,",n);
            std::fprintf(stderr," observed=");for(const auto n:observed) std::fprintf(stderr,"%u,",n);
            std::fprintf(stderr,"\n");
        }
        assert(plain==withoutDiagnosticReads);
    }
    {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);diagnosticsProperty="1";diagnosticsLines.clear();
        const std::array before{vm.reads,vm.writes,vm.calls,sink.modelReads,sink.currentChecks};
        for(unsigned i=0;i<100;++i) sink.inventoryDiagnostic("dedup","same-result");
        assert(diagnosticsLines.size()==1);
        for(unsigned i=0;i<100;++i) sink.inventoryDiagnostic("budget","distinct-result",nullptr,nullptr,nullptr,nullptr,-1,-1,static_cast<int>(i));
        assert(diagnosticsLines.size()==32 && sink.inventoryDiagnostics==32);
        assert((std::array{vm.reads,vm.writes,vm.calls,sink.modelReads,sink.currentChecks}==before));
        // Ordinary routes cannot spend the eight reserved terminal slots.
        for(unsigned i=0;i<100;++i) sink.inventoryDiagnostic("update","waiting-original-hover",nullptr,nullptr,nullptr,nullptr,-1,-1,static_cast<int>(i));
        assert(diagnosticsLines.size()==32 && !sink.inventoryTerminalDiagnostics);
        InventoryDragObservation observed{};observed.known=observed.active=observed.started=true;
        for(unsigned i=0;i<100;++i) sink.inventoryDiagnostic("fresh-source","native-held-item-owned",nullptr,nullptr,&observed);
        assert(diagnosticsLines.size()==33 && sink.inventoryTerminalDiagnostics==1);
        for(unsigned i=0;i<100;++i) sink.inventoryDiagnostic("finish","original-drop-queued",nullptr,nullptr,&observed,nullptr,-1,-1,static_cast<int>(i));
        assert(diagnosticsLines.size()==40 && sink.inventoryTerminalDiagnostics==8 && !sink.inventoryDiagnosticEnabled());
        assert((std::array{vm.reads,vm.writes,vm.calls,sink.modelReads,sink.currentChecks}==before));
    }
    // Native28 consumed all32 general lines at ownACK. Run real production
    // continuation/completion with that exact budget state; no new GFx calls.
    for(unsigned sample=0;sample<3;++sample) {
        StatusVm vm(argv[1]);StatusConsumer sink(vm);diagnosticsProperty="1";diagnosticsLines.clear();
        for(unsigned i=0;i<32;++i) sink.inventoryDiagnostic("budget","distinct",nullptr,nullptr,nullptr,nullptr,-1,-1,static_cast<int>(i));
        sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
        const auto source=sink.inventoryTarget.source;const auto owner=sink.inventoryIdentity();
        sink.inventoryGesture.begin(owner,true,false,100);
        sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
        sink.consumeHover();const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
        assert(move);sink.inventoryUpdate(*move);vm.vm.prop(vm.manager,"_inDrag",boolean(true));
        if(sample==2) sink.metadata.item=sink.metadata.object("Changed native item",sink.metadata.weaponType);
        const auto currentSource=sink.freshInventorySource();assert(bool(currentSource)==(sample!=2));
        if(sample==2) {
            assert(std::any_of(diagnosticsLines.begin(),diagnosticsLines.end(),[](const auto& line) {
                return line.find("reason=native-held-item-changed")!=std::string::npos;
            }));
        }
        const auto finish=sink.inventoryGesture.pointer({3,sample?PointerPhase::Cancel:PointerPhase::Up,.6f,.5f,10,3,100},source);
        assert(finish);sink.inventoryUpdate(*finish);
        assert(sink.drops==unsigned(sample==0) && vm.cancellations==unsigned(sample!=0) && sink.releases==1 && !sink.clicks);
        assert(std::any_of(diagnosticsLines.begin(),diagnosticsLines.end(),[&](const auto& line) {
            return line.find(sample?"reason=cancel-release-outside":"reason=original-drop-queued")!=std::string::npos;
        }));
        assert(sink.inventoryDiagnostics==32 && sink.inventoryTerminalDiagnostics>0 && sink.inventoryTerminalDiagnostics<=8);
    }
    diagnosticsProperty="0";diagnosticsLines.clear();
    for(const bool equipped:{false,true}) for(const bool cancel:{false,true}) {
        StatusVm vm(argv[1]);if(equipped) vm.equipSource();
        {
            StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
            const auto source=sink.inventoryTarget.source;
            const InventoryDragOwner owner{sink.inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
            sink.inventoryGesture.begin(owner,true,false,100);
            sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
            sink.consumeHover();const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
            assert(move);sink.inventoryUpdate(*move);
            vm.retired=true; // Actual20: original origin hit retires after queued native press.
            assert(!sink.freshInventorySource()); // Without own ACK geometry is still mandatory.
            vm.vm.prop(vm.manager,"_inDrag",boolean(true));sink.now=116;
            const auto hits=vm.hitQueries,queries=sink.queries;
            assert(sink.freshInventorySource()==source && sink.inventoryDragData.data==vm.dragData[4]);
            assert(vm.hitQueries==hits && sink.queries==queries+1);
            // Even hidden/replaced source art may retire while original native
            // panel/index/item and the acknowledged payload remain unchanged.
            vm.vm.prop(vm.cell,"_visible",boolean(false));
            assert(sink.freshInventorySource()==source && vm.hitQueries==hits);
            const auto end=cancel?sink.inventoryGesture.cancel():sink.inventoryGesture.pointer({3,PointerPhase::Up,.6f,.5f,10,3,100},source);
            assert(end);sink.inventoryUpdate(*end);
            assert(sink.presses==1 && sink.releases==1 && sink.drops==unsigned(!cancel) && vm.cancellations==unsigned(cancel) && !sink.clicks);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    // Same retired origin with changed native metadata/identity is never an
    // excuse to retain the drag. Direct UP also revalidates before dropping.
    for(const bool equipped:{false,true}) for(unsigned fault=0;fault<13;++fault) {
        StatusVm vm(argv[1]);if(equipped) vm.equipSource();
        {
            StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
            const auto source=sink.inventoryTarget.source;
            const InventoryDragOwner owner{sink.inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
            sink.inventoryGesture.begin(owner,true,false,100);
            sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
            sink.consumeHover();const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
            assert(move);sink.inventoryUpdate(*move);vm.vm.prop(vm.manager,"_inDrag",boolean(true));sink.now=116;
            vm.retired=true;assert(sink.freshInventorySource()==source);
            auto& m=sink.metadata;
            if(fault==0) m.item=m.object("Replaced held weapon",m.weaponType);
            else if(fault==1) vm.vm.put(m.top+0x3C,vm.vm.make(10));
            else if(fault==2) vm.vm.put(m.backpack+0x3C,vm.vm.make(10));
            else if(fault==3) vm.vm.put(m.equipped+0x3C,vm.vm.make(10));
            else if(fault==4) m.memory.word(m.query[equipped?1:0]+0x7C,0x00020003);
            else if(fault==5) m.memory.word(m.input[equipped?1:0]+0x48,0);
            else if(fault==6) m.memory.word(m.screenField+0x48,0x2000);
            else if(fault==7) vm.vm.prop(equipped?vm.equipped:vm.panel,"_parent",vm.vm.root);
            else if(fault==8) vm.vm.prop(vm.top,"_visible",boolean(false));
            else if(fault==9) sink.staleOnQuery=true;
            else if(fault==10) m.memory.word(m.item+0xC,2);
            else if(fault==11) sink.allowed=false;
            else sink.alive=false;
            assert(!sink.freshInventorySource());
            InventoryDragUpdate up;up.command=InventoryDragCommand::Drop;up.source=source;up.x=.6f;up.y=.5f;
            sink.finishInventory(up);
            assert(!sink.drops && !sink.clicks);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(const bool equipped:{false,true}) {
        StatusVm vm(argv[1]);if(equipped) vm.equipSource();
        {StatusConsumer sink(vm);
         sink.inventorySession.reset(); // A real DOWN starts without a retained session.
         assert(sink.inventoryDown(.2f,.4f));assert(sink.inventoryTarget);
         assert(sink.queries==1 && sink.inventoryTarget.nativeItem==sink.metadata.token(sink.metadata.item));
         assert(sink.inventoryTarget.source.panel.data==(equipped?vm.equipped[4]:vm.panel[4]));
         assert(sink.inventoryTarget.index==(equipped?4:2));
         const auto source=sink.inventoryTarget.source;
         const InventoryDragOwner owner{sink.inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
         sink.inventoryGesture.begin(owner,true,false,100);
         assert(!sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source));
         assert(sink.presses==0 && sink.drops==0); // DOWN only highlights.
         auto click=sink.inventoryGesture.pointer({3,PointerPhase::Up,.2f,.4f,10,2,100},source);
         assert(click && click->command==InventoryDragCommand::Click);sink.inventoryUpdate(*click);
         assert(sink.clicks==1 && sink.presses==0 && sink.drops==0);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    for(const bool equipped:{false,true}) for(const bool cancel:{false,true}) {
        StatusVm vm(argv[1]);if(equipped) vm.equipSource();
        {StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
         const auto source=sink.inventoryTarget.source;
         const InventoryDragOwner owner{sink.inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
         sink.inventoryGesture.begin(owner,true,false,100);
         sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
         sink.consumeHover();const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
         assert(move && move->command==InventoryDragCommand::StartMove);sink.inventoryUpdate(*move);
         assert(sink.presses==1 && sink.moves==2 && !sink.drops);
         const auto end=cancel?sink.inventoryGesture.cancel():sink.inventoryGesture.pointer({3,PointerPhase::Up,.6f,.5f,10,3,100},source);
         assert(end);sink.inventoryUpdate(*end);assert(sink.inventoryFinal && sink.releases==0);
         vm.vm.prop(vm.manager,"_inDrag",boolean(true));sink.now=116;
         assert(sink.freshInventorySource()==source && sink.inventoryDragData.data==vm.dragData[4]);
         sink.finishInventory(*sink.inventoryFinal);
         assert(sink.releases==1 && sink.drops==unsigned(!cancel) && vm.cancellations==unsigned(cancel) && !sink.clicks);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(unsigned fault=0;fault<24;++fault) {
        StatusVm vm(argv[1]);
        {StatusConsumer sink(vm);auto& m=sink.metadata;
         switch(fault) {
         case 0:m.memory.word(m.status+0x104,2);break;
         case 1:m.memory.word(m.status+0x104,4);break;
         case 2:m.memory.word(m.screenField+0x74,m.topType);break;
         case 3:m.memory.word(m.screenField+0x48,0x2000);break;
         case 4:m.memory.word(m.topField+0x28,m.topType);break;
         case 5:m.memory.word(m.topField+0x74,m.backpackType);break;
         case 6:m.memory.word(m.top+0x100,m.top);break;
         case 7:m.memory.word(m.equippedField+0x40,2);break;
         case 8:m.memory.word(m.query[0]+0x84,2u<<8|12u<<16);break;
         case 9:m.memory.word(m.input[0]+0x48,0);break;
         case 10:m.memory.word(m.result[1]+0x74,m.topType);break;
         case 11:m.memory.word(m.result[1]+0x3C,m.input[1]);break;
         case 12:vm.vm.prop(vm.panel,"bCanDrag",boolean(false));break;
         case 13:vm.vm.prop(vm.vm.root,"inventory",vm.vm.make(10));break;
         case 14:vm.vm.put(m.backpack+0x3C,vm.equipped);break;
         case 15:vm.vm.prop(vm.cell,"_parent",vm.top);break;
         case 16:vm.vm.prop(vm.data,"frozen",boolean(true));break;
         case 17:vm.selectable=false;break;
         case 18:m.memory.word(m.item+0xC,2);break;
         case 19:sink.staleOnQuery=true;break;
         case 20:sink.allowed=false;break;
         case 21:vm.vm.prop(vm.top,"_visible",boolean(false));break;
         case 22:vm.vm.prop(vm.managerClass,"_instance",Raw{});break;
         case 23:sink.frameTarget.direct.screen=5;break;
         }
         assert(!sink.captureInventory(*sink.inventorySession,.2f,.4f));
         assert(!sink.clicks && !sink.presses && !sink.releases && !vm.cancellations);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    for(unsigned fault=0;fault<5;++fault) {
        StatusVm vm(argv[1]);
        {StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
         const InventoryDragOwner owner{sink.inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
         sink.inventoryGesture.begin(owner,true,false,100);
         sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},sink.inventoryTarget.source);
         sink.inventoryPressed=true;vm.vm.prop(vm.manager,"_inDrag",boolean(true));
         assert(sink.freshInventorySource());const auto own=sink.inventoryDragData;
         if(fault==0) vm.vm.prop(vm.manager,"_data",vm.vm.make(8));
         else if(fault==1) vm.vm.prop(vm.manager,"pressOrigin",vm.top);
         else if(fault==2) sink.alive=false;
         else if(fault==3) sink.metadata.memory.word(sink.metadata.status+0x104,2);
         else vm.vm.prop(vm.manager,"relatedObject",vm.panel);
         assert(!sink.freshInventorySource());assert(sink.inventoryDragData==own);
         InventoryDragUpdate cancel;cancel.command=InventoryDragCommand::Cancel;
         if(fault!=3) {sink.finishInventory(cancel);assert(!sink.releases && !vm.cancellations);}
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(unsigned fault=0;fault<3;++fault) {
        StatusVm vm(argv[1]);
        {StatusConsumer sink(vm);
         sink.inventorySession.reset();
         vm.vm.prop(vm.manager,"_inDrag",fault==2?num(1):boolean(true));
         if(fault==0) vm.vm.prop(vm.manager,"pressOrigin",vm.top);
         assert(!sink.inventoryDown(.2f,.4f));
         assert(!sink.presses && !sink.releases && !sink.drops && !sink.clicks && !vm.cancellations);
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(unsigned fault=0;fault<10;++fault) {
        StatusVm vm(argv[1]);
        {StatusConsumer sink(vm);sink.inventorySession.reset();assert(sink.inventoryDown(.2f,.4f));
         const auto source=sink.inventoryTarget.source;
         InventoryDragOwner owner{sink.inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
         sink.inventoryGesture.begin(owner,true,false,100);
         sink.inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},source);
         sink.consumeHover();const auto move=sink.inventoryGesture.pointer({3,PointerPhase::Move,.5f,.5f,10,2,100},source);
         assert(move);sink.inventoryUpdate(*move);vm.vm.prop(vm.manager,"_inDrag",boolean(true));
         assert(sink.freshInventorySource()==source);
         std::optional<InventoryDragUpdate> cancel;
         if(fault==0) cancel=sink.inventoryGesture.pointer({4,PointerPhase::Down,.6f,.4f,10,3,100},source);
         else if(fault==1) cancel=sink.inventoryGesture.pointer({3,PointerPhase::Cancel,.5f,.5f,10,3,100},source);
         else if(fault==7) cancel=sink.inventoryGesture.pointer({3,PointerPhase::Move,.6f,.4f,10,2,100},source);
         else if(fault>=8) {
             if(fault==8) sink.metadata.item=sink.metadata.object("Replaced weapon",sink.metadata.weaponType);
             else vm.vm.prop(vm.panel,"bCanDrag",boolean(false));
             const auto fresh=sink.freshInventorySource();assert(!fresh);
             cancel=sink.inventoryGesture.pointer({3,PointerPhase::Move,.6f,.4f,10,3,100},fresh);
         } else {
             if(fault==2) ++owner.epoch;else if(fault==3) ++owner.scene;else if(fault==6) sink.alive=false;
             cancel=sink.inventoryGesture.begin(owner,fault!=4,fault==5 || fault==6,100);
         }
         assert(cancel && cancel->command==InventoryDragCommand::Cancel);sink.inventoryUpdate(*cancel);
         assert(!sink.drops && !sink.clicks && vm.cancellations==unsigned(fault!=6) && sink.releases==unsigned(fault!=6));
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    std::puts("PASS: Gate35 original filter8/controller4 positives, 62 metadata/link +7 lifetime negatives, independent Top budget +2387-field positive/4097-field refusal/524288-byte cap/unreadable distinction; extracted keyName handled/0-1/owner-loss/budget controls; prior real MOVE, typed Top/Movie, item/drop/cancel/foreign controls retained");
}
