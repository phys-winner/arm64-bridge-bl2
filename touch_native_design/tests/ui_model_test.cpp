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
};
}

int main() {
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
