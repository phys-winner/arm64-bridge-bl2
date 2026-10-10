#include "gfx_widget_fixture.h"
#include "ui_inventory_model_fixture.inc"
#include <cstdio>
using namespace bl2::touch;
using namespace widget_fixture;

struct InventoryVm final : GfxTransport {
    Vm vm;
    Raw top{}, panel{}, list{}, renderer{}, cell{}, data{}, managerClass{}, manager{}, dragData{};
    unsigned hitQueries=0, cancellations=0, drops=0;
    bool selectable=true, arrow=false, offers=false,trading=false;
    std::uint64_t offeredItem=0x6800000000000017ull;
    unsigned offeredQueries=0;
    explicit InventoryVm(const char* fixture):vm(fixture) {
        top=vm.make(10);panel=vm.make(10);list=vm.make(10);renderer=vm.make(10);cell=vm.make(10);data=vm.make(8);
        managerClass=vm.make(8);manager=vm.make(8);dragData=vm.make(8);
        vm.visible(top,vm.root);vm.prop(vm.root,"main",top);
        vm.visible(panel,top);vm.prop(top,"storagePanel",panel);vm.prop(panel,"bCanDrag",boolean(true));
        vm.visible(list,panel);vm.prop(panel,"list",list);vm.prop(list,"owningPanel",panel);
        vm.visible(renderer,list);vm.prop(renderer,"cell",cell);vm.prop(renderer,"data",data);vm.prop(renderer,"index",num(2));
        vm.visible(cell,renderer);vm.prop(renderer,"label",vm.str("Actual weapon"));
        vm.prop(managerClass,"_instance",manager);vm.prop(manager,"cancelDrag",vm.make(8));
        Raw nil{};nil[3]=1;
        vm.prop(manager,"_inDrag",boolean(false));vm.prop(manager,"pressOrigin",panel);
        vm.prop(manager,"relatedObject",nil);vm.prop(manager,"_data",dragData);
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {return vm.read(p,out,n);}
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& a,std::uint32_t& result) override {
        if(pc==base+GfxSession::kGetVariable) {
            const auto path=vm.text(a[2]);assert(path=="_root" || path=="_global.gfx.managers.DragManager");
            vm.output(a[1],path=="_root"?vm.root:managerClass);result=1;return true;
        }
        if(pc==invoke) {
            const auto name=vm.text(a[3]);Raw out{};
            if(name=="GetHitCell") {
                assert(a[1]==list[4] && a[5]==1);const auto& p=vm.object(vm.raw(a[4])).properties;
                assert(std::abs(number(p.at("x"))-256)<.001 && std::abs(number(p.at("y"))-288)<.001);
                ++hitQueries;out=arrow?Raw{}:cell;
            } else if(name=="CanBeSelected") {
                assert(a[5]==0 && (offers || a[1]==cell[4]));out=boolean(a[1]==cell[4] && selectable);
            }
            else if(name=="hitTest") {
                assert(offers && a[5]==2);
                out=boolean(a[1]==cell[4]);
            }
            else if(name=="__get__label") {
                assert(a[5]==0);
                if(a[1]!=renderer[4]) {vm.output(a[2],{});result=0;return true;}
                out=vm.object(renderer).properties.at("label");
            }
            else if(name=="__get__instance") {assert(a[1]==managerClass[4] && a[5]==0);out=vm.object(managerClass).properties.at("_instance");}
            else if(name=="cancelDrag") {
                assert(a[1]==manager[4] && a[5]==0);++cancellations;
                vm.prop(manager,"_inDrag",boolean(false)); // Original finishDrag(true), never stopDrag(false).
            } else if(name=="stopDrag") {++drops;assert(false);}
            else return vm.call(pc,a,result);
            vm.output(a[2],out);result=1;return true;
        }
        return vm.call(pc,a,result);
    }
    InventoryWidgetTarget hit(GfxSession& s) {
        return CaptureBl2InventoryWidget(s,trading?InventoryDragFamily::Trading:InventoryDragFamily::Bank,
            2560,1440,.2f,.4f,[this](std::int32_t index,bool fromOffers) {
                assert(trading && fromOffers==offers && (offers?index==1:index>=0&&index<=4095));
                ++offeredQueries;return offeredItem;
            });
    }
    void makeBackpack() {trading=true;vm.prop(vm.root,"trading",top);vm.prop(top,"backpack",panel);}
    void makeOffers() {
        offers=trading=true;vm.prop(vm.root,"trading",top);vm.prop(top,"offers",panel);
        // Original four local slots; partner receiving slots are not sources.
        for(unsigned i=0;i<4;++i) {
            const auto c=i==1?cell:vm.make(10);vm.visible(c,panel);
            vm.prop(panel,"cell"+std::to_string(i+1),c);
            vm.prop(c,"_name",vm.str(("cell"+std::to_string(i+1)).c_str()));
        }
        vm.prop(panel,"currentSelectedCell",cell);
    }
};
struct InventoryConsumer {
    struct Target {unsigned width=2560,height=1440;std::uint32_t owner=0;UiDirectModel direct;};
    TradingFixture model;
    static constexpr auto base=Fixture::base;
    InventoryVm& vm;
    UiInventoryGesture inventoryGesture;
    std::unique_ptr<GfxSession> inventorySession;
    InventoryWidgetTarget inventoryTarget;
    GfxIdentity inventoryDragData{};
    Target inventoryOwner{},frameTarget{};
    std::optional<InventoryDragUpdate> inventoryFinal;
    std::uint64_t inventoryPressedAt=100,now=100;
    bool inventoryPressed=true,inventoryCleanup=false,alive=true,allowed=true;
    unsigned moves=0,releases=0,drops=0;
    explicit InventoryConsumer(InventoryVm& transport):vm(transport) {
        if(vm.trading) vm.offeredItem=model.itemToken();
        frameTarget.owner=model.trading;inventoryOwner=frameTarget;
        inventorySession=std::make_unique<GfxSession>(vm,widget_fixture::base,movie,scratch1,512,true);
        inventoryTarget=vm.hit(*inventorySession);assert(inventoryTarget);
        const InventoryDragOwner owner{inventoryFamily(),10,20,30,40,50,60,70,2560,1440};
        inventoryGesture.begin(owner,true,false,100);
        inventoryGesture.pointer({3,PointerPhase::Down,.2f,.4f,10,1,100},inventoryTarget.source);
    }
    std::uint64_t monotonicMilliseconds() const {return now;}
    bool targetValid(const Target&,bool) const {return alive;}
    bool current() const {return allowed && alive;}
    bool inventoryDiagnosticEnabled() const {return false;}
    template<class... T> void inventoryDiagnostic(T&&...) {}
    bool statusInventoryItemCurrent() {assert(false);return false;}
    InventoryDragFamily inventoryFamily() const {return vm.trading?InventoryDragFamily::Trading:InventoryDragFamily::Bank;}
    static bool copyMemory(const void* context,std::uint32_t p,void* out,std::size_t n) {
        return Memory::read(&static_cast<const InventoryConsumer*>(context)->model.memory,p,out,n);
    }
    InventoryWidgetTarget inventoryHit(float x,float y) {
        if(!current()) return {};
        assert(x==.2f && y==.4f);
        GfxSession fresh(vm,widget_fixture::base,movie,scratch2,512,true);return vm.hit(fresh);
    }
    void clearInventory() {inventoryPressed=false;inventoryFinal.reset();inventorySession.reset();}
    bool mouse(std::uint32_t type,float,float) {
        assert(current());if(type==1) ++moves;else if(type==3) {++releases;++drops;}return true;
    }
    bool mouseFor(const Target&,std::uint32_t type,float x,float y,bool cleanup) {
        assert(alive && cleanup && x==-1 && y==-1);
        if(type==1) ++moves;else if(type==3) ++releases;return true;
    }
    // Native phase/coordinates are exercised with the real extracted method
    // in ui_status_inventory_test; this fixture substitutes that transport.
    bool inventoryMouseUpFor(const Target& target,float x,float y,bool cleanup=false) {
        return cleanup?mouseFor(target,3,x,y,true):mouse(3,x,y);
    }
#include "ui_inventory_completion.inc"
};
int main(int argc,char** argv) {
    assert(argc==2);
    {
        InventoryVm vm(argv[1]);
        {
            GfxSession retained(vm,base,movie,scratch1,512,true),fresh(vm,base,movie,scratch2,512,true);
            const auto down=vm.hit(retained),up=vm.hit(fresh);assert(down && up && SameBl2InventoryWidget(down,up));
            assert(down.index==2 && down.label=="Actual weapon" && vm.cancellations==0 && vm.drops==0);
            assert(!Bl2InventoryDragStarted(retained,down) && !CancelBl2InventoryDrag(retained,down));
            vm.vm.prop(vm.manager,"_inDrag",boolean(true));retained.beginInputBoundary();GfxIdentity actual{};
            assert(Bl2InventoryDragStarted(retained,down,&actual) && actual.data==vm.dragData[4]);
            assert(CancelBl2InventoryDrag(retained,down) && vm.cancellations==1 && vm.drops==0);
            assert(!CancelBl2InventoryDrag(retained,down) && vm.cancellations==1);
            vm.vm.prop(vm.renderer,"data",vm.vm.make(8));fresh.beginInputBoundary();
            assert(!SameBl2InventoryWidget(down,vm.hit(fresh))); // Real native item binding changed.
        }
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    for(unsigned fault=0;fault<13;++fault) {
        InventoryVm vm(argv[1]);
        switch(fault) {
        case 0:vm.vm.prop(vm.panel,"bCanDrag",boolean(false));break;
        case 1:vm.vm.prop(vm.list,"owningPanel",vm.top);break;
        case 2:vm.vm.prop(vm.cell,"_parent",vm.top);break;
        case 3:vm.vm.prop(vm.renderer,"cell",vm.vm.make(10));break;
        case 4:vm.selectable=false;break;
        case 5:vm.vm.prop(vm.data,"frozen",boolean(true));break;
        case 6:vm.vm.prop(vm.data,"bSoldOut",boolean(true));break;
        case 7:vm.vm.prop(vm.data,"bUnloadable",num(0));break;
        case 8:vm.vm.prop(vm.renderer,"index",num(.5));break;
        case 9:vm.vm.prop(vm.renderer,"label",vm.vm.str(""));break;
        case 10:vm.vm.prop(vm.managerClass,"_instance",Raw{});break;
        case 11:vm.arrow=true;break;
        case 12:vm.vm.prop(vm.panel,"_visible",boolean(false));break;
        }
        {GfxSession s(vm,base,movie,scratch1,512,true);assert(!vm.hit(s));assert(vm.cancellations==0 && vm.drops==0);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
    }
    {
        InventoryVm vm(argv[1]);
        {GfxSession s(vm,base,movie,scratch1,512,true);const auto down=vm.hit(s);assert(down);
         vm.vm.prop(vm.manager,"_inDrag",boolean(true));vm.vm.prop(vm.manager,"pressOrigin",vm.top);
         s.beginInputBoundary();assert(!CancelBl2InventoryDrag(s,down) && vm.cancellations==0);}
        assert(vm.vm.refs.empty());
    }
    for(const auto type:{0u,8u,10u}) {
        InventoryVm vm(argv[1]);
        {GfxSession s(vm,base,movie,scratch1,512,true);const auto down=vm.hit(s);assert(down);
         vm.vm.prop(vm.manager,"_inDrag",boolean(true));
         const auto related=type?vm.vm.make(type):Raw{};vm.vm.prop(vm.manager,"relatedObject",related);
         s.beginInputBoundary();assert(!Bl2InventoryDragStarted(s,down) && !CancelBl2InventoryDrag(s,down) && !vm.cancellations);}
        assert(vm.vm.refs.empty());
    }
    {
        InventoryVm vm(argv[1]);vm.makeOffers();
        {GfxSession down(vm,base,movie,scratch1,512,true),fresh(vm,base,movie,scratch2,512,true);
         const auto target=vm.hit(down);assert(target && target.index==1 && target.source.content==vm.offeredItem);
         assert(!target.data && target.dataId.data==0 && vm.offeredQueries==1);
         assert(SameBl2InventoryWidget(target,vm.hit(fresh)) && vm.offeredQueries==2);
         vm.offeredItem=0x6800000000000018ull;fresh.beginInputBoundary();
         assert(!SameBl2InventoryWidget(target,vm.hit(fresh))); // Same rarity/art, different native offered item.
         vm.vm.prop(vm.manager,"_inDrag",boolean(true));down.beginInputBoundary();
         assert(CancelBl2InventoryDrag(down,target) && vm.cancellations==1 && vm.drops==0);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(unsigned fault=0;fault<4;++fault) {
        InventoryVm vm(argv[1]);vm.makeOffers();
        if(fault==0) vm.offeredItem=0;
        else if(fault==1) vm.vm.prop(vm.panel,"currentSelectedCell",vm.top);
        else if(fault==2) vm.vm.prop(vm.cell,"_name",vm.vm.str("cell6"));
        else vm.vm.prop(vm.cell,"_parent",vm.top);
        {GfxSession s(vm,base,movie,scratch1,512,true);assert(!vm.hit(s));}
        assert(vm.vm.refs.empty() && vm.cancellations==0 && vm.drops==0);
    }
    {
        InventoryVm vm(argv[1]);vm.makeBackpack();
        {InventoryConsumer original(vm);const auto source=original.inventoryTarget.source;
         assert(original.inventoryTarget.nativeItem==original.model.itemToken());
         assert(original.freshInventorySource()==source && vm.offeredQueries==2);
         // Genuine StartOffer freezes/reorders the source. Before the original
         // manager acknowledges this panel, that source change must cancel.
         vm.vm.prop(vm.data,"frozen",boolean(true));
         assert(!original.freshInventorySource());
         vm.vm.prop(vm.manager,"_inDrag",boolean(true));original.model.acknowledge();
         assert(original.freshInventorySource()==source && original.inventoryDragData.data==vm.dragData[4]);
         const auto hitQueries=vm.hitQueries;
         vm.vm.prop(vm.renderer,"index",num(3));vm.vm.prop(vm.renderer,"data",vm.vm.make(8));
         vm.vm.prop(vm.renderer,"label",vm.vm.str("Reordered different weapon"));
         assert(original.freshInventorySource()==source && vm.hitQueries==hitQueries);
         const auto other=original.model.object("Foreign item",original.model.types.at("WillowWeapon"));
         original.model.memory.word(original.model.trading+0x108,other);assert(!original.freshInventorySource());
         original.model.acknowledge();assert(original.freshInventorySource()==source);
         original.model.memory.word(original.model.item+0xC,2);assert(!original.freshInventorySource());
         original.model.memory.word(original.model.item+0xC,0);assert(original.freshInventorySource()==source);
         original.inventoryDragData={interface,123,0,8};assert(!original.freshInventorySource());
         original.inventoryDragData={};vm.vm.prop(vm.manager,"pressOrigin",vm.top);assert(!original.freshInventorySource());
         vm.vm.prop(vm.manager,"pressOrigin",vm.panel);original.allowed=false;assert(!original.freshInventorySource());
         assert(!vm.cancellations && !vm.drops && !original.releases);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    for(const bool canceled:{false,true}) {
        InventoryVm vm(argv[1]);
        {InventoryConsumer original(vm);InventoryDragUpdate update;
         update.command=canceled?InventoryDragCommand::Cancel:InventoryDragCommand::Drop;update.x=.6f;update.y=.5f;
         original.finishInventory(update);
         assert(original.inventoryFinal && original.inventoryPressed && original.releases==0 && vm.cancellations==0);
         // The genuine AS consumer sees queued Down/Move during the normal
         // Advance. Completion from that same Android batch must wait for it.
         vm.vm.prop(vm.manager,"_inDrag",boolean(true));original.now=116;
         original.finishInventory(*original.inventoryFinal);
         assert(!original.inventoryPressed && original.releases==1 && original.drops==unsigned(!canceled));
         assert(vm.cancellations==unsigned(canceled) && vm.drops==0);}
        assert(vm.vm.refs.empty() && vm.vm.movieRefs==1);
    }
    {
        InventoryVm vm(argv[1]);InventoryConsumer original(vm);InventoryDragUpdate update;
        update.command=InventoryDragCommand::Drop;original.now=351;
        original.finishInventory(update);assert(original.releases==1 && original.drops==0 && vm.cancellations==0);
    }
    {
        InventoryVm vm(argv[1]);InventoryConsumer original(vm);InventoryDragUpdate update;
        vm.vm.prop(vm.manager,"_inDrag",boolean(true));original.allowed=false;
        original.finishInventory(update);assert(original.releases==1 && original.drops==0 && vm.cancellations==1);
    }
    {
        InventoryVm vm(argv[1]);InventoryConsumer original(vm);InventoryDragUpdate update;
        vm.vm.prop(vm.manager,"_inDrag",boolean(true));original.inventoryDragData={interface,123,0,8};
        original.finishInventory(update);assert(original.releases==0 && original.moves==0 && vm.cancellations==0);
    }
    {
        InventoryVm vm(argv[1]);InventoryConsumer original(vm);InventoryDragUpdate update;
        vm.vm.prop(vm.manager,"_inDrag",boolean(true));original.alive=false;
        original.finishInventory(update);assert(original.releases==0 && original.moves==0 && vm.cancellations==0);
    }
    std::puts("PASS: bound item/Offers identity, original Trading freeze/reorder ACK, admission/lifetime negatives and scoped cancelDrag");
}
