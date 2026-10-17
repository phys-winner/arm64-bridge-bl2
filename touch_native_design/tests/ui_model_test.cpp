#include "bl2_touch_ui_model.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
using bl2::touch::CaptureBl2UiModel;
struct Memory {
    std::map<std::uint32_t,std::vector<unsigned char>> blocks;
    std::uint32_t next = 0x10000000;
    std::uint32_t allocate(std::size_t n = 1024) {
        const auto at = next; next += static_cast<std::uint32_t>((n + 255) & ~std::size_t(255));
        blocks[at] = std::vector<unsigned char>(n); return at;
    }
    static bool read(const void* context, std::uint32_t at, void* out, std::size_t n) {
        const auto& memory = *static_cast<const Memory*>(context);
        auto it = memory.blocks.upper_bound(at);
        if (it == memory.blocks.begin()) return false;
        --it;
        const auto offset = std::size_t(at - it->first);
        if (offset > it->second.size() || n > it->second.size() - offset) return false;
        std::memcpy(out,it->second.data() + offset,n); return true;
    }
    void word(std::uint32_t at, std::uint32_t value) {
        auto it = blocks.upper_bound(at);
        if (it != blocks.begin()) {
            --it;
            const auto offset = std::size_t(at - it->first);
            if (offset <= it->second.size() && sizeof(value) <= it->second.size() - offset) {
                std::memcpy(it->second.data() + offset,&value,sizeof(value)); return;
            }
        }
        blocks[at] = std::vector<unsigned char>(sizeof(value));
        std::memcpy(blocks[at].data(),&value,sizeof(value));
    }
    std::uint32_t word(std::uint32_t at) const {
        std::uint32_t value = 0; assert(read(this,at,&value,4)); return value;
    }
    void array(std::uint32_t at, std::uint32_t data, std::uint32_t count, std::uint32_t capacity) {
        unsigned char existing[12]{};
        if (!read(this,at,existing,sizeof(existing))) blocks[at] = std::vector<unsigned char>(12);
        word(at,data); word(at + 4,count); word(at + 8,capacity);
    }
};

struct Fixture {
    static constexpr std::uint32_t base = 0x10000;
    Memory memory;
    std::uint32_t objects = memory.allocate(4096), names = memory.allocate(4096);
    std::uint32_t count = 0;
    std::map<std::string,std::uint32_t> nameIds, types;
    std::uint32_t front = 0, pause = 0, listType = 0, owner = 0, list = 0, provider = 0;
    std::uint32_t ids = 0, stack = 0, infoType = 0, idsProperty = 0, stackProperty = 0;
    std::uint32_t dialog = 0, buttons = 0, noCancelProperty = 0;

    Fixture() {
        constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,16> abi{{
            {0x00FC57D0,0xE5954044},{0x00FC57E8,0xE594403C},
            {0x00FC57DC,0xE59330C4},{0x00FC57E0,0xE3130902},
            {0x00F7AE14,0xE5900040},{0x00F9738C,0xE5902054},
            {0x0101DE20,0xE2841040},{0x0101CDB0,0xE5803044},
            {0x0101D0C0,0xE3A03008},{0x0101D17C,0xE8920003},
            {0x0101D4D4,0xE5940074},{0x0101D4F0,0xE3A0300C},
            {0x0102A1CC,0xE5952074},{0x0102A1D8,0xE5924048},
            {0x0102A1E8,0xE7F74054},{0x0101CB30,0xE5903074}
        }};
        for (const auto& [rva,word] : abi) memory.word(base + rva,word);
        memory.array(base + 0x02FBA268,objects,0,1024);
        memory.array(base + 0x02FB00C8,names,0,1024);
        name("None");
        const auto meta = object("Class",0); types["Class"] = meta;
        memory.word(meta + 0x34,meta); memory.word(meta + 0x48,0x300);
        type("ScriptStruct",0x80);
        for (auto n : {"ObjectProperty","InterfaceProperty","StructProperty","ArrayProperty",
                       "IntProperty","NameProperty","BoolProperty"}) {
            const auto p = type(n,0x80); memory.word(p + 0xC4,0x8000);
        }
        front = type("FrontendGFxMovie",0x300);
        pause = type("PauseGFxMovie",0x340,front);
        property(front,"TheList","ObjectProperty",0x260,4);
        listType = type("WillowScrollingList",0x380);
        property(listType,"MyOwnerMovie","ObjectProperty",0x120,4);
        idsProperty = property(listType,"IndexToEventId","ArrayProperty",0x178,12);
        inner(idsProperty,"IntProperty",4);
        infoType = object("DataProviderInfo",types.at("ScriptStruct"));
        memory.word(infoType + 0x48,0x20);
        property(infoType,"DataProvider","InterfaceProperty",8,8);
        property(infoType,"SelectedIndex","IntProperty",0x18,4);
        stackProperty = property(listType,"DataProviderStack","ArrayProperty",0x1C0,12);
        inner(stackProperty,"StructProperty",0x20,infoType);
        owner = object("PauseInstance",pause);
        list = object("ActualList",listType);
        const auto providerType = type("WillowScrollingListDataProviderAudioOptions",0x100);
        provider = object("AudioProvider",providerType);
        memory.word(owner + 0x260,list); memory.word(list + 0x120,owner);
        ids = memory.allocate(64);
        memory.word(ids,107); memory.word(ids + 4,108); memory.word(ids + 8,127);
        memory.array(list + 0x178,ids,3,16);
        stack = memory.allocate(128);
        memory.word(stack + 8,provider); memory.word(stack + 0x18,1);
        memory.array(list + 0x1C0,stack,1,4);
    }
    std::uint32_t name(const std::string& text) {
        if (const auto it = nameIds.find(text); it != nameIds.end()) return it->second;
        const auto id = static_cast<std::uint32_t>(nameIds.size());
        const auto entry = memory.allocate(136);
        std::memcpy(memory.blocks.at(entry).data() + 8,text.c_str(),text.size() + 1);
        memory.word(names + id * 4,entry); nameIds[text] = id;
        memory.word(base + 0x02FB00C8 + 4,static_cast<std::uint32_t>(nameIds.size())); return id;
    }
    std::uint32_t object(const std::string& text, std::uint32_t cls) {
        const auto p = memory.allocate();
        memory.word(objects + count * 4,p); memory.word(p + 0x20,count++);
        memory.word(p + 0x2C,name(text)); memory.word(p + 0x34,cls);
        memory.word(base + 0x02FBA268 + 4,count); return p;
    }
    std::uint32_t type(const std::string& text, std::uint32_t size, std::uint32_t parent = 0) {
        const auto p = object(text,types.at("Class"));
        memory.word(p + 0x40,parent); memory.word(p + 0x48,size); types[text] = p; return p;
    }
    std::uint32_t property(std::uint32_t parent, const std::string& text, const char* kind,
                           std::uint32_t offset, std::uint32_t size) {
        const auto p = object(text,types.at(kind));
        memory.word(p + 0x28,parent); memory.word(p + 0x40,1);
        memory.word(p + 0x44,size); memory.word(p + 0x54,offset);
        memory.word(p + 0x3C,memory.word(parent + 0x44)); memory.word(parent + 0x44,p); return p;
    }
    std::uint32_t inner(std::uint32_t array, const char* kind, std::uint32_t size, std::uint32_t extra = 0) {
        const auto p = object("Inner",types.at(kind));
        memory.word(p + 0x28,array); memory.word(p + 0x40,1);
        memory.word(p + 0x44,size); memory.word(p + 0x74,extra); memory.word(array + 0x74,p); return p;
    }
    void addDialog() {
        const auto cls = type("WillowGFxDialogBox",0x340);
        noCancelProperty = property(cls,"bNoCancel","BoolProperty",0x200,4);
        memory.word(noCancelProperty + 0x74,0x20);
        property(cls,"CurrentSelection","IntProperty",0x230,4);
        const auto buttonType = object("DialogBoxButton",types.at("ScriptStruct"));
        memory.word(buttonType + 0x48,0x40);
        property(buttonType,"Tag","NameProperty",0x10,8);
        const auto keys = property(buttonType,"Keys","ArrayProperty",0x20,12);
        inner(keys,"NameProperty",8);
        const auto buttonProperty = property(cls,"Buttons","ArrayProperty",0x260,12);
        inner(buttonProperty,"StructProperty",0x40,buttonType);
        dialog = object("DialogInstance",cls); memory.word(dialog + 0x200,0x24);
        buttons = memory.allocate(128);
        memory.word(buttons + 0x10,name("Confirm")); memory.word(buttons + 0x50,name("Cancel"));
        const auto keys1 = memory.allocate(16), keys2 = memory.allocate(16);
        memory.word(keys1,name("Enter")); memory.word(keys2,name("Escape"));
        memory.array(buttons + 0x20,keys1,1,2); memory.array(buttons + 0x60,keys2,1,2);
        memory.array(dialog + 0x260,buttons,2,2);
    }
    bl2::touch::UiModelSnapshot capture(std::uint32_t target = 0) {
        return CaptureBl2UiModel({&memory,Memory::read},base,target ? target : owner);
    }
    std::uint32_t method(std::uint32_t declaringType,const char* name,std::uint32_t flags,
        std::initializer_list<std::pair<const char*,const char*>> params) {
        if (!types.count("Function")) type("Function",0xA0);
        const auto fn = object(name,types.at("Function"));
        memory.word(fn+0x28,declaringType);memory.word(fn+0x3C,memory.word(declaringType+0x44));memory.word(declaringType+0x44,fn);
        memory.word(fn+0x48,static_cast<std::uint32_t>(params.size()*4));memory.word(fn+0x7C,flags);
        memory.word(fn+0x84,static_cast<std::uint32_t>(params.size()<<8 | params.size()*4<<16));memory.word(fn+0x88,65535);
        unsigned at = 0;
        for (const auto& [label,kind] : params) {
            if (!types.count(kind)) { const auto t = type(kind,0x80);memory.word(t+0xC4,0x8000); }
            const auto p = property(fn,label,kind,at,4);memory.word(p+0x48,0x80);
            if (std::string(kind)=="BoolProperty") memory.word(p+0x74,1);
            at += 4;
        }
        return fn;
    }
};
struct TradingFixture : Fixture {
    std::uint32_t movieBase=type("WillowInventoryGFxMovie",0x380),movieType=type("TradingGFxMovie",0x380,movieBase),
        panelBase=type("BaseTopLevelPanelGFxObject",0x380),panelType=type("TradingPanelGFxObject",0x380,panelBase),
        backpackType=type("InventoryListPanelGFxObject",0x380),inventoryType=type("WillowInventory",0x100);
    std::uint32_t trading=object("Trading",movieType),panel=object("TradingPanel",panelType),
        backpack=object("Backpack",backpackType),item=object("Weapon",type("WillowWeapon",0x100,inventoryType));
    std::uint32_t panelField=property(movieType,"TradingPanel","ObjectProperty",0x100,4),
        backpackField=property(panelType,"BackpackPanel","ObjectProperty",0x104,4),
        parentField=property(panelBase,"ParentMovie","ObjectProperty",0x110,4),
        offeringField=property(panelType,"OfferingThing","ObjectProperty",0x118,4),
        frozenField=property(movieBase,"FrozenThing","ObjectProperty",0x108,4);
    std::uint32_t query=method(backpackType,"GetThingByIndex",0x00020002,
        {{"Index","IntProperty"},{"ReturnValue","ObjectProperty"}}),input=0,result=0;
    TradingFixture() {
        memory.word(panelField+0x74,panelType);memory.word(backpackField+0x74,backpackType);
        memory.word(parentField+0x74,movieBase);memory.word(offeringField+0x74,inventoryType);
        memory.word(frozenField+0x74,inventoryType);
        memory.word(trading+0x100,panel);memory.word(panel+0x104,backpack);memory.word(panel+0x110,trading);
        result=memory.word(query+0x44);input=memory.word(result+0x3C);
        memory.word(query+0x44,input);memory.word(input+0x3C,result);memory.word(result+0x3C,0);
        memory.word(result+0x48,0x580);memory.word(result+0x74,inventoryType);memory.word(query+0x88,4);
    }
    void acknowledge() {memory.word(panel+0x118,item);memory.word(trading+0x108,item);}
    std::uint64_t itemToken() const {return (std::uint64_t(item)<<32)|memory.word(item+0x20);}
};
}

int main() {
    {
        TradingFixture f;const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto capture=[&] {return bl2::touch::CaptureBl2TradingBackpack(view,f.base,f.trading,2);};
        const auto held=[&] {return bl2::touch::Bl2TouchTradingHeldItem(view,f.base,f.trading);};
        const auto source=capture();assert(source.valid && source.panel>>32==f.backpack &&
            source.inventory.function==f.query && source.inventory.bytes==8);
        assert(!bl2::touch::CaptureBl2TradingBackpack(view,f.base,f.trading,-1).valid);
        assert(!bl2::touch::CaptureBl2TradingBackpack(view,f.base,f.trading,4096).valid);
        const std::pair<std::uint32_t,std::uint32_t> getterFaults[]{
            {f.panelField+0x74,f.backpackType},{f.backpackField+0x28,f.panelBase},{f.backpackField+0x48,0x2000},{f.backpackField+0x40,2},
            {f.panel+0x104,f.panel},{f.query+0x88,0},{f.input+0x54,4},{f.input+0x48,0},
            {f.result+0x74,f.panelType},{f.result+0x48,0x80},{f.result+0x3C,f.input},{f.backpack+0xC,2}};
        for(const auto [at,value]:getterFaults) {
            const auto before=f.memory.word(at);f.memory.word(at,value);assert(!capture().valid);
            f.memory.word(at,before);assert(capture().valid);
        }
        assert(!held());f.acknowledge();assert(held()==f.itemToken());
        const auto other=f.object("OtherWeapon",f.types.at("WillowWeapon"));
        const std::pair<std::uint32_t,std::uint32_t> heldFaults[]{
            {f.panelField+0x74,f.backpackType},{f.parentField+0x28,f.panelType},{f.parentField+0x48,0x2000},
            {f.offeringField+0x74,f.panelType},{f.frozenField+0x4C,1},{f.frozenField+0x40,2},
            {f.panel+0x110,f.owner},{f.panel+0x118,0},{f.trading+0x108,other},
            {f.item+0xC,2},{f.panel+0xC,2},{f.trading+0xC,2},{f.item+0x34,f.panelType}};
        for(const auto [at,value]:heldFaults) {
            const auto before=f.memory.word(at);f.memory.word(at,value);assert(!held());
            f.memory.word(at,before);assert(held()==f.itemToken());
        }
    }
    {
        Fixture f;f.addDialog();const auto baseDialog=f.types.at("WillowGFxDialogBox");
        const auto training=f.type("WillowGFxTrainingDialogBox",0x340,baseDialog),
            noPause=f.type("WillowGFxTrainingDialogBox_NoPause",0x340,training);
        const auto floatType=f.type("FloatProperty",0x80),byteType=f.type("ByteProperty",0x80);
        f.memory.word(floatType+0xC4,0x8000);f.memory.word(byteType+0xC4,0x8000);
        f.memory.word(f.dialog+0x34,noPause);f.memory.array(f.dialog+0x260,0,0,0);
        const auto delay=f.property(training,"DelayUntilShowOk","FloatProperty",0x300,4),
            tab=f.property(training,"StatusMenuTab","ByteProperty",0x304,1);
        f.memory.word(delay+0x48,0x2000);f.memory.word(tab+0x48,0x2000);
        f.memory.word(f.dialog+0x300,0xBC23D70A);f.memory.word(f.dialog+0x304,0); // -0.01, tab0.
        const auto fn=f.method(training,"HandleInputKey",0x00020002,{});
        f.memory.word(fn+0x48,20);f.memory.word(fn+0x84,4u<<8|20u<<16);f.memory.word(fn+0x88,16);
        const auto id=f.property(fn,"ControllerId","IntProperty",0,4),key=f.property(fn,"ukey","NameProperty",4,8),
            event=f.property(fn,"uevent","ByteProperty",12,1),result=f.property(fn,"ReturnValue","BoolProperty",16,4);
        for(const auto p:{id,key,event}) f.memory.word(p+0x48,0x80);
        f.memory.word(result+0x48,0x580);f.memory.word(result+0x74,1);
        f.memory.word(fn+0x44,id);f.memory.word(id+0x3C,key);f.memory.word(key+0x3C,event);
        f.memory.word(event+0x3C,result);f.memory.word(result+0x3C,0);
        const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto capture=[&] {return bl2::touch::CaptureBl2DirectUi(view,f.base,f.dialog);};
        const auto ready=[&](const bl2::touch::UiDirectModel& m) {return bl2::touch::Bl2TouchTrainingPromptReady(view,f.base,m);};
        const auto m=capture();assert(m.valid && m.trainingPrompt && m.click.bytes==20 && ready(m));
        for(unsigned status=0;status<=5;++status) {
            f.memory.word(f.dialog+0x304,status);const auto current=capture();
            assert(current.valid && current.trainingPrompt && current.screen==static_cast<int>(status) && ready(current));
            if(status) assert(!bl2::touch::Bl2TouchDirectUiCurrent(view,f.base,m));
        }
        f.memory.word(f.dialog+0x304,0);
        for(const auto value:{0u,0x3F800000u}) {
            f.memory.word(f.dialog+0x300,value);assert(capture().valid && !ready(capture())); // EXACT0 still waits.
        }
        f.memory.word(f.dialog+0x300,0xBC23D70A);
        const std::pair<std::uint32_t,std::uint32_t> faults[]{
            {delay+0x48,0},{tab+0x44,4},{f.dialog+0x304,6},{f.dialog+0x300,0x7FC00000},
            {key+0x44,4},{event+0x44,4},{result+0x54,13},{result+0x74,2},{fn+0x88,12},
            {fn+0x7C,0x00040003},{key+0x3C,result},{result+0x48,0x80},{f.dialog+0x0C,2}};
        for(const auto [at,value]:faults) {
            const auto before=f.memory.word(at);f.memory.word(at,value);assert(!capture().valid && !ready(m));
            f.memory.word(at,before);assert(capture().valid && ready(m));
        }
        const auto stringType=f.type("StrProperty",0x80);f.memory.word(stringType+0xC4,0x8000);
        const auto local=f.property(fn,"ChatText","StrProperty",20,12);
        f.memory.word(local+0x48,0x00400000);f.memory.word(local+0x3C,0);
        f.memory.word(fn+0x44,id);f.memory.word(fn+0x48,32);f.memory.word(result+0x3C,local);
        assert(!capture().valid && capture().reason=="input-parameter-order"); // No generic local-tail exception.
        f.memory.word(fn+0x48,20);f.memory.word(result+0x3C,0);assert(capture().valid && ready(m));
        f.memory.word(f.dialog+0x34,baseDialog);assert(!capture().valid); // Never admit an ordinary empty Dialog.
    }
    {
        Fixture f;const auto movie=f.type("GFxMoviePlayer",0x300),
            cls=f.type("TextChatGFxMovie",0x400,movie),gfx=f.type("GFxObject",0x100);
        const auto owner=f.object("TextChat",cls),chat=f.object("pcChat",gfx);
        const auto field=f.property(cls,"ChatObj","ObjectProperty",0x300,4);
        f.memory.word(field+0x74,gfx);f.memory.word(field+0x48,0x2000);f.memory.word(owner+0x300,chat);
        const auto fn=f.method(cls,"HandleTextChatInput",0x00020002,{});
        f.memory.word(fn+0x48,4u<<24|32);f.memory.word(fn+0x84,4u<<8|20u<<16);f.memory.word(fn+0x88,16);
        const auto byte=f.type("ByteProperty",0x80);f.memory.word(byte+0xC4,0x8000);
        const auto id=f.property(fn,"ControllerId","IntProperty",0,4),key=f.property(fn,"ukey","NameProperty",4,8),
            event=f.property(fn,"uevent","ByteProperty",12,1),result=f.property(fn,"ReturnValue","BoolProperty",16,4);
        const auto string=f.type("StrProperty",0x80);f.memory.word(string+0xC4,0x8000);
        const auto local=f.property(fn,"ChatText","StrProperty",20,12);
        f.memory.word(local+0x48,0x00400000);f.memory.word(local+0x3C,0);
        for(const auto p:{id,key,event}) f.memory.word(p+0x48,0x80);
        f.memory.word(result+0x48,0x580);f.memory.word(result+0x74,1);
        f.memory.word(fn+0x44,id);f.memory.word(id+0x3C,key);f.memory.word(key+0x3C,event);
        f.memory.word(event+0x3C,result);f.memory.word(result+0x3C,local);
        const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto capture=[&] {return bl2::touch::CaptureBl2TextChat(view,f.base,owner);};
        const auto model=capture();assert(model.valid && model.chat>>32==chat && model.input.bytes==20 && model.input.function==fn);
        const std::pair<std::uint32_t,std::uint32_t> faults[]{
            {field+0x48,0},{field+0x74,cls},{owner+0x300,owner},{key+0x44,4},{key+0x54,8},
            {event+0x44,4},{event+0x54,16},{result+0x54,13},{result+0x74,2},{result+0x48,0x80},
            {fn+0x88,12},{fn+0x84,4u<<8|16u<<16},{key+0x3C,result},{id+0x40,2},{chat+0x0C,2},
            {local+0x28,cls},{local+0x34,f.types.at("IntProperty")},{local+0x40,2},{local+0x44,4},
            {local+0x54,16},{local+0x54,24},{local+0x48,0},{local+0x48,0x80},{local+0x4C,1},
            {local+0x3C,id},{local+0x2C,f.name("OtherLocal")},{result+0x3C,0},{fn+0x48,20},
            {fn+0x48,36},{fn+0x84,5u<<8|20u<<16}};
        for(const auto [at,value]:faults) {
            const auto before=f.memory.word(at);f.memory.word(at,value);assert(!capture().valid);
            f.memory.word(at,before);assert(capture().valid);
        }
        const auto captureInput=f.property(movie,"bCaptureInput","BoolProperty",0x200,4);
        f.memory.word(captureInput+0x74,0x40);f.memory.word(owner+0x200,0x40);
        const auto stopped=[&] {return bl2::touch::Bl2TouchTextChatStopped(view,f.base,model.owner,model.chat);};
        assert(!stopped());f.memory.word(owner+0x200,0x80);assert(stopped()); // Preserve unrelated Bool bits.
        const auto action=f.type("GFxAction_OpenMovie",0x300);
        const auto duplicate=f.property(action,"bCaptureInput","BoolProperty",0x200,4);
        f.memory.word(duplicate+0x48,1);f.memory.word(duplicate+0x74,0x40);
        assert(stopped()); // Same-named GFxUI export424 must not replace declaring export348.
        const std::pair<std::uint32_t,std::uint32_t> stopFaults[]{
            {captureInput+0x28,action},{captureInput+0x48,1},{captureInput+0x4C,1},{captureInput+0x40,2},
            {captureInput+0x44,1},{captureInput+0x74,3},{captureInput+0x54,0x2FF},{owner+0x300,owner},
            {field+0x74,cls},{field+0x48,0},{chat+0x0C,2},{owner+0x0C,2},{cls+0x40,action}};
        for(const auto [at,value]:stopFaults) {
            const auto before=f.memory.word(at);f.memory.word(at,value);assert(!stopped());
            f.memory.word(at,before);assert(stopped());
        }
        assert(!bl2::touch::Bl2TouchTextChatStopped(view,f.base,model.owner+1,model.chat));
        assert(!bl2::touch::Bl2TouchTextChatStopped(view,f.base,model.owner,model.chat+1));
    }
    {
        Fixture f;
        const auto movieType=f.type("TradingGFxMovie",0x300),panelType=f.type("TradingPanelGFxObject",0x300),
            offersType=f.type("TradingOffersPanelGFxObject",0x300),inventoryType=f.type("WillowInventory",0x100);
        const auto owner=f.object("Trading",movieType),panel=f.object("TradingPanel",panelType),offers=f.object("OffersPanel",offersType);
        const auto panelProperty=f.property(movieType,"TradingPanel","ObjectProperty",0x100,4),
            offersProperty=f.property(panelType,"OffersPanel","ObjectProperty",0x104,4);
        f.memory.word(panelProperty+0x74,panelType);f.memory.word(offersProperty+0x74,offersType);
        f.memory.word(owner+0x100,panel);f.memory.word(panel+0x104,offers);
        f.property(offersType,"SelectedCellSuffix","IntProperty",0x108,4);
        const auto browsing=f.property(offersType,"bBrowsingCells","BoolProperty",0x10C,4);
        f.memory.word(browsing+0x74,4);f.memory.word(offers+0x108,2);f.memory.word(offers+0x10C,4);
        const auto query=f.method(movieType,"GetOfferedCellInventory",0x00020002,
            {{"CellIndex","IntProperty"},{"ReturnValue","ObjectProperty"}});
        const auto result=f.memory.word(query+0x44),input=f.memory.word(result+0x3C);
        f.memory.word(query+0x44,input);f.memory.word(input+0x3C,result);f.memory.word(result+0x3C,0);
        f.memory.word(result+0x48,0x580);f.memory.word(result+0x74,inventoryType);f.memory.word(query+0x88,4);
        const auto allowed=f.method(offersType,"CanStartOffer",0x00020002,{{"ReturnValue","BoolProperty"}}),
            allowedResult=f.memory.word(allowed+0x44);
        f.memory.word(allowedResult+0x48,0x580);f.memory.word(allowed+0x88,0);
        const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto capture=[&] {return bl2::touch::CaptureBl2TradingOffer(view,f.base,owner,1);};
        const auto valid=capture();assert(valid.valid && valid.owner>>32==owner && valid.panel>>32==offers &&
            valid.inventory.function==query && valid.inventory.bytes==8 && valid.canStart.function==allowed && valid.canStart.bytes==4);
        assert(!bl2::touch::CaptureBl2TradingOffer(view,f.base,owner,0).valid);
        assert(!bl2::touch::CaptureBl2TradingOffer(view,f.base,owner,4).valid); // Partner receiving slots are not local drag sources.
        const std::pair<std::uint32_t,std::uint32_t> faults[]{
            {panelProperty+0x74,offersType},{offersProperty+0x74,panelType},{offers+0x10C,0},
            {input+0x54,4},{result+0x54,0},{query+0x86,4},{query+0x88,0},{input+0x48,0},
            {result+0x48,0x80},{result+0x74,panelType},{result+0x40,2},{input+0x3C,0},
            {allowedResult+0x48,0x80},{allowedResult+0x74,2},{allowed+0x7C,0},{offers+0xC,2}};
        for(const auto [at,value]:faults) {
            const auto before=f.memory.word(at);f.memory.word(at,value);assert(!capture().valid);
            f.memory.word(at,before);assert(capture().valid);
        }
        const auto item=f.object("OfferedWeapon",f.type("WillowWeapon",0x100,inventoryType)),foreign=f.object("Foreign",panelType);
        assert(bl2::touch::Bl2TouchInventoryToken(view,f.base,item)>>32==item);
        assert(!bl2::touch::Bl2TouchInventoryToken(view,f.base,foreign));f.memory.word(item+0xC,2);
        assert(!bl2::touch::Bl2TouchInventoryToken(view,f.base,item));
    }
    {
        Fixture f;
        const auto cls = f.type("CharacterSelectionReduxGFxMovie",0x500);
        const auto state = f.property(cls,"CurrentUIState","IntProperty",0x300,4);
        const auto selected = f.property(cls,"SelectedCharacterIndex","IntProperty",0x308,4);
        f.memory.word(state+0x40,2);f.memory.word(selected+0x40,2);
        const auto clipType = f.type("CharacterSelectionGFxObject",0x80), clip = f.object("CharacterSelect",clipType);
        f.property(cls,"CharacterSelectClip","ObjectProperty",0x320,4);
        f.method(cls,"HandleCharacterMouseOver",0x00040003,{{"PlayerIndex","IntProperty"},{"CharacterIndex","IntProperty"}});
        f.method(cls,"HandleCharacterClicked",0x00040003,{{"ControllerId","IntProperty"},{"CharacterIndex","IntProperty"}});
        f.method(cls,"SetSelectedCharacterIndex",0x00040003,{{"PlayerIndex","IntProperty"},{"NewSelectedIndex","IntProperty"}});
        const auto menuClick=f.method(cls,"HandleCustomizationMenuEntryClick",0x00020002,{{"PlayerIndex","IntProperty"},{"EntryIndex","IntProperty"}});
        const auto owner = f.object("CharacterMovie",cls);f.memory.word(owner+0x300,1);f.memory.word(owner+0x304,2);
        f.memory.word(owner+0x308,0);f.memory.word(owner+0x30C,3);f.memory.word(owner+0x320,clip);
        const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto a = bl2::touch::CaptureBl2DirectUi(view,f.base,owner,0);
        assert(a.valid && a.screen==1 && a.selected==0 && a.hover.bytes==8 && a.click.bytes==8 && a.select.bytes==8);
        const auto b = bl2::touch::CaptureBl2DirectUi(view,f.base,owner,1);
        assert(b.valid && b.screen==2 && b.selected==3 && b.structure!=a.structure && b.menuClick.bytes==8);
        const auto entry=f.memory.word(menuClick+0x44), player=f.memory.word(entry+0x3C);
        f.memory.word(player+0x54,4);f.memory.word(entry+0x54,0);
        assert(!bl2::touch::CaptureBl2DirectUi(view,f.base,owner,1).valid);
        f.memory.word(player+0x54,0);f.memory.word(entry+0x54,4);
        f.memory.word(owner+0x308,4);int selectedNow = -1;
        assert(bl2::touch::Bl2TouchDirectUiCurrent(view,f.base,a,&selectedNow) && selectedNow==4);
        f.memory.word(owner+0x300,2);assert(!bl2::touch::Bl2TouchDirectUiCurrent(view,f.base,a));
        f.memory.word(owner+0x300,1);f.memory.word(state+0x40,1);
        assert(!bl2::touch::CaptureBl2DirectUi(view,f.base,owner).valid); // Real source has two slots.
    }
    {
        Fixture f;const auto byte = f.type("ByteProperty",0x80);f.memory.word(byte+0xC4,0x8000);
        const auto cls = f.type("StatusMenuExGFxMovie",0x500), mapType = f.type("StatusMenuMapGFxObject",0x300);
        const auto gfxType = f.type("GFxObject",0x80), mask = f.object("NativeMapMask",gfxType), map = f.object("NativeMap",mapType);
        f.property(mapType,"MaskClip","ObjectProperty",0x60,4);f.memory.word(map+0x60,mask);
        f.property(cls,"CurrentScreen","ByteProperty",0x300,1);f.property(cls,"StatusMenuMap","ObjectProperty",0x308,4);
        f.method(cls,"HandleMouseDrag",0x00020802,{{"dx","IntProperty"},{"DY","IntProperty"}});
        f.method(cls,"StopMouseDrag",0x00020002,{});
        f.method(cls,"HandleMapZoom",0x00024002,{{"Delta","FloatProperty"},{"bLoopingAudio","BoolProperty"}});
        f.method(mapType,"PlaceCustomObjective",0x00020401,{});f.method(mapType,"ToggleLegend",0x00020002,{});
        f.method(mapType,"UpdateInfoBox",0x00020400,{});
        const auto owner=f.object("StatusMovie",cls);f.memory.word(owner+0x300,2);f.memory.word(owner+0x308,map);
        const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto d=bl2::touch::CaptureBl2DirectUi(view,f.base,owner);
        assert(d.valid && d.screen==2 && d.mask==mask && d.drag.bytes==8 && d.zoom.bytes==8);
        const auto fp=f.type("FloatProperty",0x80);f.memory.word(fp+0xC4,0x8000);
        const auto actor=f.type("Actor",0x400),controllerType=f.type("PlayerController",0x500,actor),worldType=f.type("WorldInfo",0x400);
        const auto controller=f.object("Player",controllerType),world=f.object("World",worldType);
        f.property(actor,"WorldInfo","ObjectProperty",0x200,4);f.memory.word(controller+0x200,world);
        f.property(mapType,"PlayerOwner","ObjectProperty",0x80,4);f.memory.word(map+0x80,controller);
        f.property(mapType,"ZoomSpeedFactor","FloatProperty",0x84,4);
        f.property(worldType,"DeltaSeconds","FloatProperty",0x204,4);
        const auto vector=f.object("Vector2D",f.types.at("ScriptStruct"));f.memory.word(vector+0x48,8);
        f.property(vector,"X","FloatProperty",0,4);f.property(vector,"Y","FloatProperty",4,4);
        const auto cursor=f.property(mapType,"CursorPos","StructProperty",0x214,8);
        const auto clip=f.property(mapType,"ClipSize","StructProperty",0x220,8);
        f.memory.word(cursor+0x74,vector);f.memory.word(clip+0x74,vector);
        const auto real=[&](std::uint32_t p,float value) {std::uint32_t bits=0;std::memcpy(&bits,&value,4);f.memory.word(p,bits);};
        real(map+0x84,.05f);real(world+0x204,1.f/60);real(map+0x214,12);real(map+0x218,-13);
        real(map+0x220,420);real(map+0x224,400);
        const auto state=bl2::touch::CaptureBl2MapState(view,f.base,d);
        assert(state.valid && state.map==d.clip && state.cursorProperty==cursor && state.cursorOffset==0x214 &&
            state.cursorX==12 && state.cursorY==-13 && state.clipWidth==420 && state.clipHeight==400 && state.zoomSpeed==.05f);
        f.memory.word(cursor+0x44,4);assert(!bl2::touch::CaptureBl2MapState(view,f.base,d).valid);f.memory.word(cursor+0x44,8);
        f.memory.word(vector+0x2C,f.name("Vector"));assert(!bl2::touch::CaptureBl2MapState(view,f.base,d).valid);
        f.memory.word(vector+0x2C,f.name("Vector2D"));real(world+0x204,0);
        assert(!bl2::touch::CaptureBl2MapState(view,f.base,d).valid);real(world+0x204,1.f/60);
        f.memory.word(owner+0x300,4);
        const auto skills=bl2::touch::CaptureBl2DirectUi(view,f.base,owner);
        assert(skills.valid && skills.screen==4 && !skills.clip && !skills.mask && skills.structure!=d.structure);
        assert(!bl2::touch::Bl2TouchDirectUiCurrent(view,f.base,d));
        assert(!bl2::touch::CaptureBl2MapState(view,f.base,d).valid);
        f.memory.word(owner+0x300,2);f.memory.word(d.zoom.function+0x86,0);
        assert(!bl2::touch::CaptureBl2DirectUi(view,f.base,owner).valid);
        for (const auto* name : {"BankGFxMovie","VendingMachineExGFxMovie","FastTravelStationGFxMovie","QuestAcceptGFxMovie","CustomizationGFxMovie"})
            assert(bl2::touch::Bl2TouchReviewedUiMovie(name));
        for (const auto* name : {"WillowHUDGFxMovie","ItemPickupGFxMovie","TwoPanelInterfaceGFxObject","UnknownGFxMovie"})
            assert(!bl2::touch::Bl2TouchReviewedUiMovie(name));
    }
    {
        Fixture f;
        const auto engineType = f.type("Engine",0x600), clientType = f.type("WillowGameViewportClient",0x300);
        f.property(engineType,"GameViewport","ObjectProperty",0x4A0,4);
        const auto engine = f.object("GameEngine",engineType), client = f.object("GameViewport",clientType);
        f.memory.word(f.base + 0x03008D20,engine);
        f.memory.word(engine + 0x490,1); // Adjacent GamePlayers Num is never a client.
        f.memory.word(engine + 0x4A0,client);
        f.property(clientType,"ViewportUI","ObjectProperty",0x120,4);
        const auto uiType = f.type("WillowUIInteraction",0x200);
        const auto opened = f.property(uiType,"bOpened","BoolProperty",0x180,4);
        f.memory.word(opened + 0x74,0x40);
        const auto ui = f.object("NativeDebug",uiType);
        f.memory.word(client + 0x120,ui); f.memory.word(ui + 0x180,0x40);
        const bl2::touch::UiModelMemory memory{&f.memory,Memory::read};
        assert(bl2::touch::Bl2TouchGameViewportClient(memory,f.base) == client);
        const auto menu = CaptureBl2UiModel(memory,f.base,bl2::touch::Bl2TouchGameViewportClient(memory,f.base));
        assert(menu.valid && menu.nativeMenuOpen);
        f.memory.word(engine + 0x4A0,1);
        assert(!bl2::touch::Bl2TouchGameViewportClient(memory,f.base));
    }
    {
        Fixture f;
        const auto actorType = f.type("Actor",0x300), controllerBase = f.type("Controller",0x300,actorType),
            controllerType = f.type("PlayerController",0x300,controllerBase), pawnType = f.type("Pawn",0x300);
        const auto worldType = f.type("WorldInfo",0x300), priType = f.type("PlayerReplicationInfo",0x200);
        f.property(controllerBase,"Pawn","ObjectProperty",0x120,4);
        f.property(actorType,"WorldInfo","ObjectProperty",0x124,4);
        f.property(worldType,"Pauser","ObjectProperty",0x130,4);
        const auto controller = f.object("LocalController",controllerType), pawn = f.object("LivePawn",pawnType);
        const auto world = f.object("LANWorld",worldType), pri = f.object("LocalPRI",priType);
        f.memory.word(controller + 0x120,pawn); f.memory.word(controller + 0x124,world);
        const bl2::touch::UiModelMemory memory{&f.memory,Memory::read};
        // A long game-specific child chain cannot hide inherited native fields.
        for (unsigned i = 0; i < 520; ++i) f.property(controllerType,"GameSpecific" + std::to_string(i),"IntProperty",0x200,4);
        assert(!bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller));
        assert(bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller,f.owner)); // LAN Pause, Pauser NULL.
        bl2::touch::UiPauseWorldIdentity identity;
        assert(bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller,f.owner,nullptr,&identity));
        assert((identity.controller>>32)==controller && (identity.pawn>>32)==pawn && (identity.world>>32)==world);
        const auto front = f.object("MainMenu",f.front);
        assert(!bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller,front));
        const auto engineType = f.type("Engine",0x600), engine = f.object("Engine",engineType);
        f.memory.word(f.base + 0x03008D20,engine);
        const std::array<std::uint32_t,9> words{0xE59F3018,0xE79F3003,0xE5933000,0xE3530000,0x15D32294,
            0x17C02011,0x15C32294,0xE12FFF1E,0x00BA9C80};
        for (unsigned i = 0; i < words.size(); ++i) f.memory.word(f.base + 0x0236A188 + i * 4,words[i]);
        f.memory.word(engine + 0x294,0x81);
        assert(bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller)); // Original LAN pause flag; no Pauser.
        f.memory.word(engine + 0x294,0x80);
        assert(!bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller));
        f.memory.word(world + 0x130,pri);
        assert(bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller));
        f.memory.word(controller + 0x120,0);
        assert(!bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller,f.owner));
        assert(!bl2::touch::Bl2TouchPausedWorld(memory,f.base,controller,f.owner,nullptr,&identity));
        assert(!identity.controller && !identity.pawn && !identity.world);
    }
    {
        Fixture f;
        const auto viewportType = f.type("WillowGameViewportClient",0x300);
        f.property(viewportType,"ViewportUI","ObjectProperty",0x120,4);
        const auto uiType = f.type("WillowUIInteraction",0x200);
        const auto opened = f.property(uiType,"bOpened","BoolProperty",0x180,4);
        f.memory.word(opened + 0x74,0x40);
        const auto viewport = f.object("Viewport",viewportType), ui = f.object("DebugUI",uiType);
        f.memory.word(viewport + 0x120,ui);
        const auto closed = f.capture(viewport);
        assert(closed.valid && !closed.nativeMenuOpen);
        f.memory.word(ui + 0x180,0xC0);
        const auto active = f.capture(viewport);
        assert(active.valid && active.nativeMenuOpen && active.structure != closed.structure);
        f.memory.word(opened + 0x74,3);
        assert(!f.capture(viewport).valid);
    }
    {
        Fixture f;
        const auto m = f.capture();
        if (!m.valid) std::cerr << m.reason << '\n';
        assert(m.valid && m.movieClass == "PauseGFxMovie" && m.providerDepth == 1);
        assert(m.eventIds == std::vector<std::int32_t>({107,108,127}));
        assert(m.selectedIndex == 1 && m.providerClass == "WillowScrollingListDataProviderAudioOptions");
        assert(m.list >> 32 == f.list && m.provider >> 32 == f.provider && m.bytesRead < 131072);
        f.memory.word(f.stack + 0x18,2);
        assert(f.capture().structure == m.structure); // selection is not structural replacement
        f.memory.word(f.ids + 4,127);
        assert(f.capture().structure != m.structure); // same movie/provider, new row mapping
        f.memory.word(f.ids + 4,108);
        const auto provider2 = f.object("OtherAudioProvider",f.types.at("WillowScrollingListDataProviderAudioOptions"));
        f.memory.word(f.stack + 8,provider2);
        assert(f.capture().structure != m.structure);
    }
    {
        Fixture f; f.addDialog(); const auto m = f.capture(f.dialog);
        assert(m.valid && m.noCancel && m.buttons.size() == 2 && m.buttons[1].tag == "Cancel");
        assert(m.buttons[0].keys == std::vector<std::string>({"Enter"}));
        f.memory.word(f.dialog + 0x200,4); assert(!f.capture(f.dialog).noCancel);
        f.memory.word(f.noCancelProperty + 0x74,3); assert(!f.capture(f.dialog).valid);
    }
    {
        Fixture f; f.addDialog();const auto cls=f.types.at("WillowGFxDialogBox");
        f.method(cls,"OnWidgetFocused",0x00020002,{{"ButtonIndex","IntProperty"}});
        const auto click=f.method(cls,"OnWidgetClicked",0x00020002,{{"ButtonIndex","IntProperty"},{"ControllerId","IntProperty"}});
        const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto dialog=bl2::touch::CaptureBl2DirectUi(view,f.base,f.dialog);
        assert(dialog.valid && dialog.screen==2 && dialog.click.bytes==8);
#if defined(BL2_NATIVE_ARM32)
        const auto training=f.type("WillowGFxTrainingDialogBox",0x340,cls);
        f.memory.word(f.dialog+0x34,training);
        assert(f.capture(f.dialog).valid && bl2::touch::CaptureBl2DirectUi(view,f.base,f.dialog).valid);
        // Original Willow export39127 derives from39125, with no input-method
        // override. The non-pausing training popup uses that same timed button
        // enablement and inherited DialogBox focus/click frame.
        const auto noPause=f.type("WillowGFxTrainingDialogBox_NoPause",0x340,training);
        f.memory.word(f.dialog+0x34,noPause);
        assert(bl2::touch::Bl2TouchReviewedUiMovie("WillowGFxTrainingDialogBox_NoPause") &&
            f.capture(f.dialog).valid && bl2::touch::CaptureBl2DirectUi(view,f.base,f.dialog).valid);
#endif
        const auto controller=f.memory.word(click+0x44), button=f.memory.word(controller+0x3C);
        f.memory.word(controller+0x54,0);f.memory.word(button+0x54,4);
        assert(!bl2::touch::CaptureBl2DirectUi(view,f.base,f.dialog).valid); // Signature order is not native layout order.
    }
    {
        Fixture f;const auto cls=f.type("ItemInspectionGFxMovie",0x400);
        const auto start=f.method(cls,"StartMouseDrag",0x00020400,{{"bRMB","BoolProperty"}});
        f.method(cls,"StopMouseDrag",0x00020400,{{"bRMB","BoolProperty"}});
        f.method(cls,"HandleMouseDrag",0x00020400,{{"dx","IntProperty"},{"DY","IntProperty"}});
        const auto owner=f.object("InspectItem",cls);const bl2::touch::UiModelMemory view{&f.memory,Memory::read};
        const auto d=bl2::touch::CaptureBl2DirectUi(view,f.base,owner);
        assert(d.valid && d.startDrag.bytes==4 && d.stopDrag.bytes==4 && d.drag.bytes==8);
        f.memory.word(f.memory.word(start+0x44)+0x74,2);
        assert(!bl2::touch::CaptureBl2DirectUi(view,f.base,owner).valid);
    }
    {
        Fixture f; f.memory.word(f.owner + 0x0C,0x20000000); assert(!f.capture().valid);
        f.memory.word(f.owner + 0x0C,2); assert(!f.capture().valid);
        f.memory.word(f.owner + 0x0C,0); f.memory.word(f.owner + 0x20,900); assert(!f.capture().valid);
    }
    {
        Fixture f; f.memory.word(f.list + 0x120,f.provider); assert(!f.capture().valid);
    }
    {
        Fixture f; f.memory.word(f.idsProperty + 0x54,0x37F); assert(!f.capture().valid);
    }
    {
        Fixture f; const auto p = f.property(f.front,"Other","IntProperty",0x100,4);
        f.memory.word(p + 0x3C,p); assert(!f.capture().valid);
    }
    {
        Fixture f; f.memory.word(f.idsProperty + 0x28,f.front); assert(!f.capture().valid);
    }
    {
        Fixture f; f.memory.array(f.list + 0x178,f.ids,17,16); assert(!f.capture().valid);
        f.memory.array(f.list + 0x178,0xFFFFFFF0,16,16); assert(!f.capture().valid);
    }
    {
        Fixture f; f.memory.word(f.memory.word(f.stackProperty + 0x74) + 0x34,f.types.at("ObjectProperty"));
        assert(!f.capture().valid); // DataProviderStack must not be treated as UObject**
    }
    {
        Fixture f; f.memory.word(Fixture::base + 0x00F9738C,0); assert(!f.capture().valid);
    }
    std::cout << "PASS: live-property relocation, stack struct, dialog masks/keys, structural changes, invalid lifetime/metadata/arrays/ABI\n";
}
