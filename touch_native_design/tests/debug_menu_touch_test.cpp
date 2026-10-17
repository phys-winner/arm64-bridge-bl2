#include "../../arm64_bridge/src/bl2_pause_debug_menu.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <cstring>

using namespace bl2::touch;
static void require(bool value,const char* message) {
    if(!value) {std::cerr<<message<<'\n';std::exit(1);}
}
static void originalGearBuilderFooter() {
    // Pinned original WillowGame.xxx, InputKey export47115, serialized offsets
    // 0x1EB..0x234. NameConst Y/XboxTypeS_Y -> ProcessCmd, StructMember tertiary.
    constexpr std::uint8_t originalY[]{
        0x07,0xA2,0x02,0x84,0xFE,0x00,0x09,0xB8,0x00,0x00,0x21,0x73,0x82,0x00,0x00,0x00,
        0x00,0x00,0x00,0x16,0x18,0x15,0x00,0xFE,0x00,0x09,0xB8,0x00,0x00,0x21,0x67,0x82,
        0x00,0x00,0x00,0x00,0x00,0x00,0x16,0x16,0x1B,0xCF,0x5E,0x00,0x00,0x00,0x00,0x00,
        0x00,0x35,0x14,0xB7,0x00,0x00,0x19,0xB7,0x00,0x00,0x00,0x00,0x10,0x01,0x5C,0xCD,
        0x00,0x00,0x01,0x7F,0xCD,0x00,0x00,0x16,0x04,0x27};
    const auto word=[](const std::uint8_t* p) {std::uint32_t value;std::memcpy(&value,p,4);return value;};
    require(originalY[10]==0x21 && word(originalY+11)==33395 && originalY[29]==0x21 &&
        word(originalY+30)==33383 && originalY[40]==0x1B && word(originalY+41)==24271 &&
        originalY[49]==0x35 && word(originalY+50)==46868 && kDebugBuildFooterAction==Action::NextWeapon,
        "The captured original branch must map Y, not Enter, to ProcessCmd(TertiaryActionCmd)");
    const std::string_view hint="(X) Back up a step    (Y) Build this Gun";
    DebugBuildFooterRange range;
    require(debugBuildFooterRange("Gear Builder","BuildWeapon",hint,range) &&
        hint.substr(range.prefix,range.label)=="(Y) Build this Gun",
        "Only the original ready Gear Builder hint may expose its Y build command");
    DebugBuildFooterRect rect;
    require(debugBuildFooterRect(200,1450,720,30,720,30,400,320,1400,3636,1620,rect) &&
        rect.left==600 && rect.right==920 && rect.top<1450 && rect.bottom>1480 && rect.top>1400,
        "The real proportional-font prefix must isolate Y from X, enlarge its height and leave every row untouched");
    DebugMenuGesture gesture;
    const auto pointer=[&](PointerPhase phase,float x,float y) {
        return gesture.pointer({0,phase,x/3636,y/1620,7,1,100},3636,1620,30);
    };
    unsigned primary=0,spawn=0,initialize=0,thrown=0,clear=0,closed=0;
    bool opened=true;
    // Decoded original delegate: only event0/2 dispatches commands; event1
    // cannot execute a second Build. BuildWeapon throws loot; no backpack write.
    const auto original=[&](const DebugInputParameters& parameters) {
        if(!opened || (parameters.event!=0 && parameters.event!=2))return;
        if(parameters.keyIndex==33395) {++spawn;++initialize;++thrown;++clear;++closed;opened=false;}
        else if(parameters.keyIndex==12002)++primary; // Enter selects its primary slot.
    };
    pointer(PointerPhase::Down,760,1465);
    require(spawn==0,"Merely touching the ready footer must not create loot");
    const auto tap=pointer(PointerPhase::Up,760,1465);
    require(tap.kind==DebugTouchCommand::Tap && debugBuildFooterRange("Gear Builder","BuildWeapon",hint,range),
        "A completed unchanged ready-footer tap must retain the genuine build intent");
    DebugInputParameters press;press.controllerId=3;press.keyIndex=33395;original(press);
    auto release=press;release.event=1;original(release);
    require(primary==0 && spawn==1 && initialize==1 && thrown==1 && clear==1 && closed==1,
        "Ready-footer activation must use only original Y, create/initialize/throw once, clear parts and close the native menu");
    for(const auto command:{std::string_view{},std::string_view{"SetWeaponPart"},std::string_view{"BuildWeapon 1"}})
        require(!debugBuildFooterRange("Gear Builder",command,hint,range),
            "An unready row or a different tertiary command must never activate Build");
    require(!debugBuildFooterRange("Inventory","BuildWeapon",hint,range) &&
        !debugBuildFooterRange("Gear Builder","BuildWeapon","(A) Select Weapon Family",range) &&
        !debugBuildFooterRange("Gear Builder","BuildWeapon","(X) Back up a step    (Y) Build this Shield",range) &&
        debugBuildFooterRange("Gear Builder","BuildItem","(X) Back up a step    (Y) Build this Shield",range),
        "Other menus, a stale/mismatched hint and weapon/item disagreement must fail closed");
    require(!debugBuildFooterRect(200,1450,400,60,720,30,400,320,1400,3636,1620,rect) &&
        !debugBuildFooterRect(200,1450,720,30,720,30,400,320,1600,3636,1620,rect) &&
        !debugBuildFooterRect(200,1450,720,30,720,30,std::numeric_limits<float>::quiet_NaN(),320,1400,3636,1620,rect),
        "Wrapped/clipped or unreadable geometry and overlapping authored rows must not offer a guessed build target");
    pointer(PointerPhase::Down,760,1465);gesture.cancel();
    require(pointer(PointerPhase::Up,760,1465).kind==DebugTouchCommand::None,
        "Cancel, competing input or content invalidation must discard an old build intent");
    pointer(PointerPhase::Down,760,1465);gesture.clear();
    require(pointer(PointerPhase::Up,760,1465).kind==DebugTouchCommand::None,
        "A stale scene/epoch must not activate its old ready footer");
    pointer(PointerPhase::Down,760,1465);
    require(pointer(PointerPhase::Move,760,1265).kind==DebugTouchCommand::Scroll &&
        pointer(PointerPhase::Up,760,1265).kind!=DebugTouchCommand::Tap,
        "Paging over the footer must keep ordinary navigation and never create an item");
}
// Consume byte offsets read in the guest's real PlayerManager call site. This
// deliberately has no knowledge of DebugInputParameters' field names/layout.
struct OriginalInputDelegateFixture {
    int selected=0,primary=0,alternate[4]{},main=0,sub=0;
    bool opened=true;
    void call(void* bytes) {
        auto* raw=static_cast<std::uint8_t*>(bytes);
        const auto word=[&](std::size_t at) {std::uint32_t value;std::memcpy(&value,raw+at,4);return value;};
        require(word(0)==3 && word(8)==0 && word(16)==0x3F800000 && word(20)==0,
            "Delegate must receive ControllerId, both FName words, Amount and keyboard/gamepad flag at original offsets");
        require(raw[13]==0 && raw[14]==0 && raw[15]==0 && word(24)==0,
            "Optional event padding and script return must be initialized for every press and release");
        std::uint32_t handled=0;
        if(raw[12]==0 && opened) {
            switch(word(4)) {
            case 1:++selected;handled=1;break;
            case 2:opened=false;handled=1;break;
            case 3:++primary;handled=1;break;
            case 4:case 5:case 6:case 7:++alternate[word(4)-4];handled=1;break;
            case 8:++main;handled=1;break;
            case 9:++sub;handled=1;break;
            }
        }
        std::memcpy(raw+24,&handled,4);
    }
};
// Model the decoded original InputKey page branches and selectable-only arrow
// traversal. Page keys call UpdateLeave/UpdateOver but never ProcessCmd.
struct OriginalPagedMenuFixture {
    std::array<bool,39> selectable{};
    int selected=0,top=0,visible=23,leave=0,over=0,commands=0,pageDown=0,pageUp=0;
    void call(DebugInputParameters& parameters) {
        const auto* raw=reinterpret_cast<const std::uint8_t*>(&parameters);
        require(parameters.controllerId==3 && parameters.keyNumber==0 && raw[12]<=1 &&
            raw[13]==0 && raw[14]==0 && raw[15]==0 && parameters.amountBits==0x3F800000 &&
            parameters.gamepad==0 && parameters.handled==0,
            "Arrow and page inputs must preserve the original fresh press/release delegate block");
        if(raw[12])return;
        const int last=static_cast<int>(selectable.size())-1;
        if(parameters.keyIndex==3) {++commands;parameters.handled=1;return;}
        ++leave;
        switch(parameters.keyIndex) {
        case 1:
            for(int i=selected+1;i<=last;++i)if(selectable[i]) {selected=i;break;}
            while(selected>=top+visible)++top;
            break;
        case 10:
            for(int i=selected-1;i>=0;--i)if(selectable[i]) {selected=i;break;}
            while(selected<top)--top;
            break;
        case 11:
            ++pageDown;selected=std::min(selected+visible,last);
            while(selected>=top+visible)++top;
            break;
        case 12:
            ++pageUp;selected=std::max(selected-visible,0);
            while(selected<top)--top;
            break;
        default:require(false,"Paging must dispatch only original Up/Down/PageUp/PageDown keys");
        }
        ++over;parameters.handled=1;
    }
    void send(std::uint32_t key) {
        DebugInputParameters press;press.controllerId=3;press.keyIndex=key;call(press);
        require(press.handled==1,"Original arrow/page press must retain its script bool result");
        DebugInputParameters release;release.controllerId=3;release.keyIndex=key;release.event=1;call(release);
        require(release.handled==0,"A page-key release must not repeat pagination");
    }
    void move(Action action) {
        const auto before=static_cast<std::uint32_t>(selected);
        send(action==Action::UiDown?1u:10u);
        if(debugPageFallback(action,static_cast<std::uint32_t>(selectable.size()),before,
            static_cast<std::uint32_t>(selected),static_cast<std::uint32_t>(top),static_cast<std::uint32_t>(visible)))
            send(action==Action::UiDown?11u:12u);
    }
};
static void originalInformationalPagination() {
    OriginalPagedMenuFixture menu;
    std::fill_n(menu.selectable.begin(),23,true);
    menu.move(Action::UiDown);
    require(menu.selected==1 && menu.pageDown==0 && menu.leave==1 && menu.over==1,
        "Ordinary changing Down must run exactly one original arrow and no unnecessary page key");
    menu.selected=22;menu.move(Action::UiDown);
    require(menu.selected==38 && menu.top==16 && !menu.selectable[menu.selected] && menu.pageDown==1 &&
        menu.leave==3 && menu.over==3 && menu.commands==0 && debugVisibleTop(39,38,16,23)==16,
        "The 414 boundary at selectable row22 must reveal the informational tail through original PageDown without activation");
    menu.move(Action::UiDown);
    require(menu.selected==38 && menu.top==16 && menu.pageDown==1 && menu.commands==0,
        "The already visible last row must not issue another page or execute its informational entry");
    menu.move(Action::UiUp);
    require(menu.selected==22 && menu.top==16 && menu.pageUp==0 && menu.commands==0,
        "Reverse Up must retain original selectable traversal across the tail without an unnecessary PageUp");
    OriginalPagedMenuFixture info;info.visible=12;
    std::array<bool,39> seen{};
    const auto view=[&] {for(int i=info.top;i<std::min(info.top+info.visible,39);++i)seen[i]=true;};
    view();
    for(int gesture=0;gesture<8 && info.selected<38;++gesture) {info.move(Action::UiDown);view();}
    require(info.selected==38 && info.top==27 && info.pageDown==4 && info.commands==0 &&
        std::all_of(seen.begin(),seen.end(),[](bool value){return value;}),
        "A fully informational category must make every native row readable with bounded forward pages");
    for(int gesture=0;gesture<8 && info.selected>0;++gesture)info.move(Action::UiUp);
    require(info.selected==0 && info.top==0 && info.pageUp==4 && info.commands==0 && info.leave==info.over,
        "An informational prefix must support reverse PageUp and preserve paired UpdateLeave/UpdateOver");
    require(!debugPageFallback(Action::UiDown,39,22,23,0,23) &&
        !debugPageFallback(Action::UiDown,39,22,22,16,23) &&
        !debugPageFallback(Action::UiUp,39,0,0,0,23) &&
        !debugPageFallback(Action::Confirm,39,22,22,0,23) &&
        !debugPageFallback(Action::UiDown,0,0,0,0,23) &&
        !debugPageFallback(Action::UiDown,39,39,39,0,23) &&
        !debugPageFallback(Action::UiDown,39,22,22,39,23) &&
        !debugPageFallback(Action::UiDown,39,22,22,0,0) &&
        !debugPageFallback(Action::UiDown,39,22,22,0,513) &&
        !debugPageFallback(Action::UiDown,65537,22,22,0,23),
        "Changed selection, complete windows, unrelated actions and stale bounds must never request pagination");
}
// The authored renderer measures headers/columns before placing them, then
// derives Menu_IPP from its remaining body height and native StrLen("Q").
// Check the resulting visible layout, including fixed gaps/icons, rather than
// asserting a particular fit implementation or multiplying the viewport.
struct OriginalCanvasLayoutFixture {
    float clipX=1200,clipY=600,headerX=0,columnX[2]{},rowPitch=0;
    unsigned visible=0;
    float measure(float glyphs,float scale) const {return glyphs*10*scale;}
    float draw(float glyphs,float scale) const {return glyphs*10*scale;}
    void render(float scale) {
        const float titleA=measure(18,scale),titleB=measure(20,scale),icon=36,gap=18;
        headerX=std::max(10.f,(clipX-titleA-titleB-icon-gap)/2);
        require(headerX+draw(18,scale)<=headerX+titleA &&
            headerX+titleA+icon+gap+draw(20,scale)<=clipX-10,
            "Both measured tab widths and painted glyphs must fit with original icon/gap sizes");
        columnX[0]=10;columnX[1]=columnX[0]+measure(30,scale)+24;
        require(columnX[0]+draw(30,scale)<=columnX[1]-24 && columnX[1]+draw(24,scale)<=clipX-10,
            "Actual enlarged column text must remain inside independently measured column boundaries");
        rowPitch=20*scale;
        visible=static_cast<unsigned>((clipY-100*scale)/rowPitch);
        require(visible>0 && 100*scale+visible*rowPitch<=clipY,
            "The authored page calculation must retain visible rows without drawing beyond its body area");
    }
};
static void originalStrLenFrameAbi() {
    std::array<std::uint32_t,16> registers{};
    std::array<std::uint32_t,32> canvas{};
    std::array<std::uint32_t,DebugStrLenFrameAbi::localBytes/4> stack{};
    for(std::size_t i=0;i<stack.size();++i)stack[i]=0xAA000000u+static_cast<std::uint32_t>(i);
    const auto before=stack;
    constexpr std::uint32_t frame=0x00112000,canvasAddress=0x00224000;
    registers[4]=canvasAddress;registers[5]=frame;registers[3]=0;
    canvas[0x40/4]=0x3F000000;canvas[0x50/4]=0x42800000;
    // Preserve the caller while R5 still means FFrame, in verified unused
    // local padding. These are the real displaced ARM load/store words.
    stack[DebugStrLenFrameAbi::frameSlot/4]=registers[5];
    const auto execute=[&](std::uint32_t word) {
        const auto op=word&0xFFF00000u;
        require((op==0xE5900000 || op==0xE5800000) && ((word>>16)&15)==4 && (word&0xFFF)<canvas.size()*4,
            "Frame preservation must replay original ARM instructions with only an R4-relative operand");
        const auto destination=(word>>12)&15,index=(word&0xFFF)/4;
        if(op==0xE5900000)registers[destination]=canvas[index];
        else canvas[index]=registers[destination];
    };
    execute(DebugStrLenFrameAbi::saveWord0);execute(DebugStrLenFrameAbi::saveWord1);
    require(registers[5]==0x3F000000 && registers[5]!=frame && canvas[0x50/4]==0 &&
        stack[DebugStrLenFrameAbi::frameSlot/4]==frame,
        "The original StrLen must reuse R5 for OrgX while the late measurement still recovers the real FFrame");
    for(std::size_t i=0;i<stack.size();++i)if(i!=DebugStrLenFrameAbi::frameSlot/4)
        require(stack[i]==before[i],"Frame preservation must not overwrite FString, output locals or outgoing argument words");
    const std::array<std::uint32_t,4> continuation{DebugStrLenFrameAbi::saveWord0,
        DebugStrLenFrameAbi::saveWord1,0xE51FF004,DebugStrLenFrameAbi::saveSite+8};
    require(continuation[2]==0xE51FF004 && continuation[3]==0x018511DC &&
        DebugStrLenFrameAbi::frameSlot+4<=DebugStrLenFrameAbi::localBytes,
        "The verified no-PC-relative continuation must resume after the displaced words inside the original local allocation");
    // The real parser's dummy references point to +14/+10, and its FString to
    // +20/+24/+28. Finishing those operations cannot destroy the saved frame.
    for(unsigned offset:{0x10u,0x14u,0x20u,0x24u,0x28u})stack[offset/4]=0;
    require(stack[DebugStrLenFrameAbi::frameSlot/4]==frame,
        "Native output/parameter handling must leave the retained caller alive until the scale-input seam");
    const auto foreignCanvas=canvas;
    stack[DebugStrLenFrameAbi::frameSlot/4]=0;
    require(stack[DebugStrLenFrameAbi::frameSlot/4]!=frame && canvas==foreignCanvas,
        "A later foreign caller must discard the old UI frame without changing another Canvas");
}
static void originalHalfSecondRefresh() {
    DebugFontRefreshKey measured{{11,12,13,14},15,16,17,18,1,0,1,1,3636,1620,3636,1620,2};
    const float baseline[]{638};
    // TickImp refreshes the same one-row Memory warning every .5s. FString,
    // menu/header arrays and ColumnLayout may all get fresh native storage.
    // The rendered fit must survive their resets, while pointer identities
    // remain different for the separate input/geometry validator.
    std::uintptr_t menu=0x10000,columns=0x20000;
    OriginalCanvasLayoutFixture paint;
    for(unsigned refresh=0;refresh<30;++refresh) {
        const auto oldMenu=menu,oldColumns=columns;
        menu+=0x1000;columns+=0x100;
        const float width[]{refresh%3==0?0.f:refresh%3==1?394.f:1276.f};
        const auto plan=debugFontFramePlan(measured,measured,false,width,baseline,1,2);
        const float factor=plan==DebugFontFramePlan::ReuseRefresh?2.f:1.f;
        paint.render(factor);
        require(menu!=oldMenu && columns!=oldColumns && paint.rowPitch==40 && paint.columnX[1]==634,
            "Every native .5s array reset must keep the measured 200% paint; no alternating original-size frame");
    }
    const float owned[]{1276},wider[]{1800};
    require(debugFontFramePlan(measured,measured,true,owned,baseline,1,2)==DebugFontFramePlan::Continue,
        "Unchanged metrics and storage must keep the ordinary per-frame fit without replaying original measurement");
    auto newCaption=measured;newCaption.metrics^=0xC4A971u;
    require(debugFontFramePlan(measured,newCaption,true,owned,baseline,1,2)==DebugFontFramePlan::MeasureOriginal,
        "An edited wider caption/header/icon with the same addresses, row count and category must remeasure before any cached paint");
    require(debugReuseFontMeasurements(measured,measured,owned,baseline,1,2) &&
        !debugReuseFontMeasurements(measured,measured,wider,baseline,1,2),
        "Only a native reset or the exact owned measured width may reuse the fit; wider metrics must remeasure");
    for(unsigned field=0;field<15;++field) {
        auto changed=measured;
        switch(field) {
        case 0:++changed.owner.ui;break;case 1:++changed.owner.engine;break;
        case 2:++changed.owner.client;break;case 3:++changed.owner.world;break;
        case 4:++changed.canvas;break;case 5:++changed.generation;break;case 6:++changed.scene;break;
        case 7:++changed.metrics;break; // Changed caption/header/font/layout, same category and row count.
        case 8:++changed.main;break;case 9:++changed.sub;break;case 10:++changed.rows;break;
        case 11:++changed.columns;break;case 12:++changed.width;break;case 13:++changed.clipY;break;
        case 14:changed.requested=1;break;
        }
        require(!debugReuseFontMeasurements(measured,changed,owned,baseline,1,2),
            "A real content, owner, lifecycle, font, viewport or requested-scale change must measure its own layout");
    }
    auto absent=measured;absent.metrics=0;
    const float invalid[]{std::numeric_limits<float>::quiet_NaN()};
    require(!debugReuseFontMeasurements(absent,absent,owned,baseline,1,2) &&
        !debugReuseFontMeasurements(measured,measured,invalid,baseline,1,2) &&
        !debugReuseFontMeasurements(measured,measured,owned,baseline,2,2) &&
        !debugReuseFontMeasurements(measured,measured,owned,baseline,1,2.1f),
        "Unbounded/unreadable signatures, malformed widths/count and invalid scales retain the original fallback");
}
static void originalRowRefreshCapture() {
    // native14: row0 was already selected, so DOWN ran no guest callback;
    // TickImp's original RefreshCurrentMenu(false) rebuilt the same Gear list
    // during the 35ms touch, cancelling UP solely on array allocation identity.
    DebugRowCaptureKey key;
    key.owner={11,12,13,14};key.controller=15;key.schema=16;key.input=17;
    key.scene=18;key.generation=19;key.epoch=74;key.menu=20;key.commands=21;
    key.viewport=22;key.width=1200;key.height=540;key.kind=0;key.index=0;key.selected=0;
    key.count=250;key.top=0;key.ipp=12;key.main=3;key.sub=0;key.requested=1.3f;
    DebugMenuGesture native(true);
    std::uint64_t allocation=100;
    unsigned activations=0;
    for(unsigned refresh=0;refresh<20;++refresh) {
        require(native.pointer({0,PointerPhase::Down,.15f,.252f,74,0,100},1200,540,16).kind==DebugTouchCommand::Select,
            "DOWN on an already-selected Gear family must remain a harmless selection");
        const auto oldAllocation=allocation;allocation+=32;
        require(allocation!=oldAllocation && debugPreserveRowCapture(key,key),
            "The identical original row/commands/window must survive only allocation changes at a native refresh");
        if(native.pointer({0,PointerPhase::Up,.15f,.252f,74,0,135},1200,540,16).kind==DebugTouchCommand::Tap)++activations;
    }
    require(activations==20,"A periodic allocation refresh must not swallow any original row UP or repeat its command");
    // An original refresh may also reuse the same arrays and label storage.
    // Exercise the production captured-frame/UP gate without an allocation
    // change: a changed original command must retire the pending UP.
    const auto sameAllocation=allocation;
    require(native.pointer({0,PointerPhase::Down,.15f,.252f,74,0,200},1200,540,16).kind==DebugTouchCommand::Select,
        "An in-place refresh starts with the same harmless native DOWN");
    auto reused=key;++reused.commands;
    if(!debugCapturedRowCurrent(native.captured(),0,key,reused))native.clear();
    require(allocation==sameAllocation &&
        native.pointer({0,PointerPhase::Up,.15f,.252f,74,0,235},1200,540,16).kind==DebugTouchCommand::None,
        "Unchanged array addresses/caption must not permit UP to activate a newly refreshed original command");
    require(native.pointer({0,PointerPhase::Down,.15f,.252f,74,0,300},1200,540,16).kind==DebugTouchCommand::Select &&
        debugCapturedRowCurrent(native.captured(),0,key,key) &&
        native.pointer({0,PointerPhase::Up,.15f,.252f,74,0,335},1200,540,16).kind==DebugTouchCommand::Tap,
        "Unchanged original semantics and addresses retain exactly one native UP");
    auto next=key;next.index=3;
    auto selected=next;selected.selected=3;
    require(debugPreserveRowCapture(next,selected,true) && !debugPreserveRowCapture(next,selected),
        "Only our original DOWN arrows may rebase selection to the exact captured row");
    auto overshot=selected;overshot.selected=4;
    require(!debugPreserveRowCapture(next,overshot,true),"Original arrows must not adopt a skipped or different selected row");
    for(unsigned field=0;field<22;++field) {
        auto changed=key;
        switch(field) {
            case 0:++changed.owner.ui;break;case 1:++changed.owner.engine;break;case 2:++changed.owner.client;break;
            case 3:++changed.owner.world;break;case 4:++changed.controller;break;case 5:++changed.schema;break;
            case 6:++changed.input;break;case 7:++changed.scene;break;case 8:++changed.generation;break;
            case 9:++changed.epoch;break;case 10:++changed.menu;break;case 11:++changed.commands;break;
            case 12:++changed.viewport;break;case 13:++changed.controllerId;break;case 14:++changed.width;break;
            case 15:++changed.height;break;case 16:++changed.index;break;case 17:++changed.selected;break;
            case 18:++changed.top;break;case 19:++changed.ipp;break;case 20:++changed.count;break;case 21:changed.requested=2;break;
        }
        require(!debugPreserveRowCapture(key,changed),
            "A changed row payload/command/caption, routing, schema, owner, lifecycle or display window must cancel the old UP");
    }
    auto invalid=key;invalid.commands=0;
    require(!debugPreserveRowCapture(invalid,invalid) && !debugPreserveRowCapture({},{}),
        "Unknown metadata or an unreadable original command may not acquire semantic capture ownership");
    auto footer=key;footer.kind=3;footer.footer=27;
    require(debugPreserveRowCapture(footer,footer) && !debugPreserveRowCapture(footer,footer,true),
        "An identical ready Y footer may survive refresh but cannot inherit a row-selection transition");
    auto unready=footer;++unready.commands;
    auto hint=footer;++hint.footer;
    require(!debugPreserveRowCapture(footer,unready) && !debugPreserveRowCapture(footer,hint),
        "A changed BuildWeapon/BuildItem command or ready hint must discard the captured footer");
    auto header=key;header.kind=1;
    require(!debugPreserveRowCapture(header,header),"Category transition capture retains its original allocation/lifecycle cancellation");
}
int main() {
    originalRowRefreshCapture();
    originalGearBuilderFooter();
    originalStrLenFrameAbi();
    originalHalfSecondRefresh();
    originalInformationalPagination();
    {
        DebugMenuGesture native(true);
        const auto pointer = [&](PointerPhase phase,float y=.3f) {
            return native.pointer({0,phase,.2f,y,7,0,100},2400,1080,30);
        };
        require(pointer(PointerPhase::Down).kind==DebugTouchCommand::Select &&
            pointer(PointerPhase::Up).kind==DebugTouchCommand::Tap,
            "Native DOWN highlights without a command; the same original row activates only on UP");
        require(pointer(PointerPhase::Down).kind==DebugTouchCommand::Select &&
            pointer(PointerPhase::Cancel).kind==DebugTouchCommand::None &&
            pointer(PointerPhase::Up).kind==DebugTouchCommand::None,
            "Cancellation leaves a harmless selection and cannot perform its command");
        pointer(PointerPhase::Down);
        require(pointer(PointerPhase::Move,.55f).kind==DebugTouchCommand::Scroll &&
            pointer(PointerPhase::Up,.55f).kind!=DebugTouchCommand::Tap,
            "Paging after native DOWN selection never activates that row on release");
    }
    DebugTextExtent widths[]={{380,54},{540,24}};
    const float wide=debugFontScale(1180,widths,2,1.25f);
    require(wide==1.25f,"Wide Debug layouts must get the default 125% text enlargement");
    OriginalCanvasLayoutFixture canvas;
    canvas.render(wide);
    require(canvas.clipX==1200 && canvas.clipY==600 && canvas.rowPitch==25 && canvas.visible==19,
        "Text enlargement must change original line metrics/page capacity independently of viewport/render quality");
    const auto column=canvas.columnX[1];
    for(int frame=0;frame<20;++frame)canvas.render(debugFontScale(1180,widths,2,1.25f));
    require(canvas.columnX[1]==column && canvas.rowPitch==25 && canvas.visible==19,
        "Repeated native measure/draw frames must keep stable column and row geometry");
    canvas.render(debugFontScale(1180,widths,2,2));
    require(canvas.rowPitch==40 && canvas.visible==10 && canvas.clipX==1200 && canvas.clipY==600,
        "200% must grow native text/page metrics without resizing the render viewport");
    canvas.render(debugFontScale(1180,widths,2,1));
    require(canvas.rowPitch==20 && canvas.visible==25 && canvas.columnX[1]==334,
        "Returning to 100% must restore pristine native measurements and pagination");
    canvas.render(debugFontScale(1180,widths,2,1.25f));
    require(canvas.columnX[1]==column && canvas.rowPitch==25 && canvas.visible==19,
        "125 to 200 to 100 to 125 must restore the first layout without accumulated scaling");
    float cache=380;
    const DebugColumnCacheOwner originalOwner{11,12,13,14};
    auto currentOwner=originalOwner;
    unsigned inputEpoch=1;
    for(int reopen=0;reopen<12;++reopen) {
        for(float scale:{1.25f,2.f,1.f}) {
            const auto baseline=cache;
            cache=baseline*scale;
            ++inputEpoch; // Original CloseScene/StartScene advance this epoch.
            if(originalOwner==currentOwner)cache=debugRestoreColumnWidth(cache,baseline,scale);
            require(cache==380,"Reopening a still-live column allocation must recover only its owned width cache");
        }
    }
    require(inputEpoch==37,"Display restoration must support every real scene-close/open epoch change");
    for(unsigned identity=0;identity<4;++identity) {
        currentOwner=originalOwner;
        if(identity==0)++currentOwner.ui;
        if(identity==1)++currentOwner.engine;
        if(identity==2)++currentOwner.client;
        if(identity==3)++currentOwner.world;
        cache=760;
        if(originalOwner==currentOwner)cache=debugRestoreColumnWidth(cache,380,2);
        require(cache==760,"A foreign registered UI, engine, client or world must not inherit a previous width baseline");
    }
    require(debugRestoreColumnWidth(901,380,2)==901 && debugRestoreColumnWidth(0,380,2)==0 &&
        debugRestoreColumnWidth(760,380,1)==760,
        "New authored widths, native resets and original-scale layouts must not be overwritten by stale bridge metrics");
    canvas.clipX=700;
    const float compact=debugFontScale(680,widths,2,2);
    require(compact>1 && compact<2,"Narrow layouts must fit their real tab/column metrics instead of clipping at requested 200%");
    canvas.render(compact);
    const DebugTextExtent sidePane[]={{540,244}};
    const float withPane=debugFontScale(880,sidePane,1,2);
    require(withPane>1 && withPane<2 && 540*withPane+244<=880.01f,
        "A side description pane must retain its original available width while body columns grow");
    require(debugFontScale(500,widths,2,2)==1 && debugFontScale(1180,widths,2,1)==1,
        "A dense original layout and a 100% request must preserve the original text geometry");
    const DebugTextExtent invalid[]={{std::numeric_limits<float>::quiet_NaN(),0}},fixed[]={{0,100}};
    require(debugFontScale(100,invalid,1,2)==1 && debugFontScale(90,fixed,1,2)==1 &&
        debugFontScale(std::numeric_limits<float>::infinity(),widths,2,2)==1 &&
        debugFontScale(1180,widths,2,std::numeric_limits<float>::quiet_NaN())==1 &&
        debugFontScale(1180,widths,2,.9f)==1 && debugFontScale(1180,widths,2,2.1f)==1 &&
        debugFontScale(1180,nullptr,1,1.25f)==1,
        "Invalid metrics and oversized fixed icons must leave the original text contract untouched");
    require(debugVisibleTop(100,42,8,22)==21 && debugVisibleTop(100,5,21,22)==5 &&
        debugVisibleTop(100,99,96,10)==90 && debugVisibleTop(8,7,6,20)==0,
        "Enlarged-font pagination must keep the original selected row visible at either end of the list");
    require(debugVisibleTop(100,42,8,0)==8 && debugVisibleTop(100,100,8,22)==8 &&
        debugVisibleTop(100,42,8,513)==8,
        "Uninitialized/stale pagination must not rewrite a display window");
    OriginalInputDelegateFixture original;
    for(std::uint32_t key:{1u,3u,4u,5u,6u,7u,8u,9u,2u}) {
        DebugInputParameters press;
        press.controllerId=3;press.keyIndex=key;
        original.call(&press);
        require(press.handled==1,"Original delegate bool return must come back from byte24");
        DebugInputParameters release;
        release.controllerId=3;release.keyIndex=key;release.event=1;
        original.call(&release);
        require(release.handled==0,"Release must not repeat a native Debug command");
    }
    require(original.selected==1 && original.primary==1 && original.main==1 && original.sub==1 && !original.opened &&
        std::all_of(std::begin(original.alternate),std::end(original.alternate),[](int n){return n==1;}),
        "The original delegate route must select, execute all five command slots, change categories and close");
    DebugMenuGesture gesture;
    const auto input=[&](int id,PointerPhase phase,float x,float y) {
        return gesture.pointer({id,phase,x,y,7,1,100},2424,1080,30);
    };
    input(1,PointerPhase::Down,.2f,.3f);
    auto tap=input(1,PointerPhase::Up,.202f,.302f);
    require(tap.kind==DebugTouchCommand::Tap && tap.x==.2f && tap.y==.3f,
        "Small slip must commit at the captured row, not a neighbour");
    // Exact 412 regression: Select button completed before the next independent
    // Memory header tap. Idle cancellation must not swallow that tap's UP.
    gesture.cancel();
    input(0,PointerPhase::Down,850.f/2424,100.f/1080);
    auto header=input(0,PointerPhase::Up,850.f/2424,100.f/1080);
    require(header.kind==DebugTouchCommand::Tap && header.x==850.f/2424 && header.y==100.f/1080,
        "Completed Select must leave the first main-header tap actionable");
    for(Action button:{Action::Confirm,Action::UiUp,Action::UiDown,Action::UiLeft,Action::UiRight,
            Action::Reload,Action::NextWeapon,Action::Skill,Action::Sprint,Action::PreviousWeapon,Action::Inventory}) {
        require((kDebugMenuActions&actionBit(button))!=0,"Expanded Debug action must be admitted");
        gesture.cancel();
        input(0,PointerPhase::Down,.35f,.12f);
        require(input(0,PointerPhase::Up,.35f,.12f).kind==DebugTouchCommand::Tap,
            "Every completed More/action button must preserve the next independent sub-header tap");
    }
    input(2,PointerPhase::Down,.3f,.7f);
    auto scroll=input(2,PointerPhase::Move,.3f,.5f);
    require(scroll.kind==DebugTouchCommand::Scroll && scroll.steps>0 && scroll.steps<=8,
        "Upward swipe must advance the original list with bounded work");
    require(input(2,PointerPhase::Up,.3f,.5f).kind!=DebugTouchCommand::Tap,
        "Scrolling must never activate a debug command on release");
    input(0,PointerPhase::Down,.3f,.7f);
    require(input(0,PointerPhase::Move,.3f,.5f).kind==DebugTouchCommand::Scroll,
        "A paging gesture must start as original vertical scrolling");
    gesture.cancel();
    require(input(0,PointerPhase::Move,.3f,.2f).kind==DebugTouchCommand::None &&
        input(0,PointerPhase::Up,.3f,.2f).kind==DebugTouchCommand::None,
        "A native Page window change must discard the old captured scroll and its row activation on release");
    input(3,PointerPhase::Down,.6f,.5f);
    require(input(3,PointerPhase::Up,.4f,.5f).kind==DebugTouchCommand::NextTab,
        "Leftward swipe must open the next original category");
    gesture.cancel();input(0,PointerPhase::Down,.4f,.5f);
    require(input(0,PointerPhase::Up,.6f,.5f).kind==DebugTouchCommand::PreviousTab,
        "Rightward swipe after a completed category button must navigate backwards");
    gesture.cancel();input(0,PointerPhase::Down,.3f,.3f);
    auto previousRows=input(0,PointerPhase::Move,.3f,.6f);
    require(previousRows.kind==DebugTouchCommand::Scroll && previousRows.steps<0 && previousRows.steps>=-8,
        "Downward swipe after a completed More action must select previous rows with bounded work");
    require(input(0,PointerPhase::Up,.3f,.6f).kind!=DebugTouchCommand::Tap,
        "Downward scrolling must not invoke a command on release");
    input(4,PointerPhase::Down,.2f,.3f);input(5,PointerPhase::Down,.5f,.3f);
    require(input(4,PointerPhase::Up,.2f,.3f).kind==DebugTouchCommand::None,
        "Second finger must cancel the first intent");
    require(input(5,PointerPhase::Up,.5f,.3f).kind==DebugTouchCommand::None && !gesture.captured(),
        "Ownership must never transfer to the second finger");
    input(6,PointerPhase::Down,.2f,.3f);
    require(input(6,PointerPhase::Cancel,.2f,.3f).kind==DebugTouchCommand::None,
        "CANCEL must release without running a debug action");
    input(7,PointerPhase::Down,.2f,.3f);gesture.cancel();
    require(input(7,PointerPhase::Up,.2f,.3f).kind==DebugTouchCommand::None,
        "Competing key action must cancel captured pointer intent");
    input(0,PointerPhase::Down,.35f,.12f);
    require(input(0,PointerPhase::Up,.35f,.12f).kind==DebugTouchCommand::Tap,
        "After the conflicting stream finishes, its cancellation must not leak to another header tap");
    input(8,PointerPhase::Down,.2f,.3f);gesture.clear();
    require(input(8,PointerPhase::Up,.2f,.3f).kind==DebugTouchCommand::None,
        "Scene/epoch reset must discard an old pointer UP");
    input(9,PointerPhase::Down,std::numeric_limits<float>::quiet_NaN(),.3f);
    require(input(9,PointerPhase::Up,.2f,.3f).kind==DebugTouchCommand::None,
        "Nonfinite pointer must not reach a guest command");
    input(10,PointerPhase::Down,.2f,.3f);input(10,static_cast<PointerPhase>(99),.2f,.3f);
    require(input(10,PointerPhase::Up,.2f,.3f).kind==DebugTouchCommand::None,
        "Unknown pointer phase must cancel intent");
    require((kDebugMenuActions&actionBit(Action::Reload)) && !(kDebugMenuActions&actionBit(Action::Fire)),
        "Debug uses its native alternate commands without offering gameplay Fire");
    require(debugCategoryNavigation(Action::Inventory) && debugCategoryNavigation(Action::PreviousWeapon) &&
        debugCategoryNavigation(Action::UiLeft) && debugCategoryNavigation(Action::UiRight) &&
        !debugCategoryNavigation(Action::Confirm) && !debugCategoryNavigation(Action::Reload) &&
        !debugCategoryNavigation(Action::Back),
        "Only original category navigation may adopt the content change caused by its command");
    std::cout<<"DebugMenuTouchTest: original StrLen frame/padding/continuation ABI, paired native text fit/pagination, original delegate ABI/commands, informational PageUp/PageDown boundaries/reverse/no activation, row capture, swipe, category, cancellation, multitouch and admission passed\n";
}
