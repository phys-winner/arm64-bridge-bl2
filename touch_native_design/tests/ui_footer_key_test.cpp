#include "bl2_touch_widget_commands.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#define HORN_LOGI(...) ((void)0)
using namespace bl2::touch;

namespace {
constexpr std::uint32_t kMovieKey=0x01CC5E7C;
struct KeyCase {NativeFooterCommand command;std::uint32_t rva;const char* spelling;};
// Original KEY_* ELF symbols, independently checked against the pinned image.
constexpr KeyCase cases[] = {
    {NativeFooterCommand::Inspect,0x0300A318,"XboxTypeS_RightThumbstick"},
    {NativeFooterCommand::Mark,0x0300A310,"XboxTypeS_LeftThumbstick"},
    {NativeFooterCommand::Sort,0x03009FD0,"PageDown"},
    {NativeFooterCommand::Select,0x0300A340,"XboxTypeS_X"},
    {NativeFooterCommand::Drop,0x0300A348,"XboxTypeS_Y"},
    {NativeFooterCommand::Close,0x03009FB8,"Escape"},
    {NativeFooterCommand::Accept,0x03009FA0,"Enter"},
    {NativeFooterCommand::SellTrash,0x0300A310,"XboxTypeS_LeftThumbstick"},
    {NativeFooterCommand::ButtonA,0x0300A330,"XboxTypeS_A"},
    {NativeFooterCommand::ButtonB,0x0300A338,"XboxTypeS_B"},
    {NativeFooterCommand::ButtonX,0x0300A340,"XboxTypeS_X"},
    {NativeFooterCommand::ButtonY,0x0300A348,"XboxTypeS_Y"},
    {NativeFooterCommand::ButtonBack,0x0300A320,"XboxTypeS_Back"},
    {NativeFooterCommand::ButtonStart,0x0300A328,"XboxTypeS_Start"},
    {NativeFooterCommand::ButtonLSB,0x0300A310,"XboxTypeS_LeftThumbstick"},
    {NativeFooterCommand::ButtonRSB,0x0300A318,"XboxTypeS_RightThumbstick"},
    {NativeFooterCommand::ButtonLB,0x0300A350,"XboxTypeS_LeftShoulder"},
    {NativeFooterCommand::ButtonRB,0x0300A358,"XboxTypeS_RightShoulder"},
    {NativeFooterCommand::ButtonLT,0x0300A360,"XboxTypeS_LeftTrigger"},
    {NativeFooterCommand::ButtonRT,0x0300A368,"XboxTypeS_RightTrigger"}
};
struct Host {
    struct Call {std::uint32_t pc;std::vector<std::uint32_t> args;};
    std::unordered_map<std::uint32_t,std::uint8_t> bytes;
    std::vector<Call> calls;
    bool* alive=nullptr;
    bool loseAfterDown=false;
    mutable unsigned reads=0;
    std::uint32_t GetGuestMemorySize() const {return 0x10000000;}
    std::uint8_t MemoryRead8(std::uint32_t p) const {
        ++reads;const auto found=bytes.find(p);return found==bytes.end()?0:found->second;
    }
    std::uint32_t MemoryRead32(std::uint32_t p) const {
        std::uint32_t value=0;
        for(unsigned i=0;i<4;++i) value|=std::uint32_t(MemoryRead8(p+i))<<(i*8);
        return value;
    }
    void word(std::uint32_t p,std::uint32_t value) {
        for(unsigned i=0;i<4;++i) bytes[p+i]=static_cast<std::uint8_t>(value>>(i*8));
    }
    void text(std::uint32_t p,const std::string& value) {
        for(unsigned i=0;i<128;++i) bytes[p+i]=i<value.size()?static_cast<std::uint8_t>(value[i]):0;
    }
    void RunGuestCallback(std::uint32_t pc,const std::vector<std::uint32_t>& args) {
        calls.push_back({pc,args});
        if(loseAfterDown && calls.back().args.back()==0) *alive=false;
    }
};
struct Consumer {
    struct Target {
        std::uint32_t gfx=0x30000,controllerId=0,movie=0x40000,owner=0x50000;
        struct Direct {std::string movieClass="StatusMenuExGFxMovie";} direct;
    };
    static constexpr std::uint32_t base=0x00100000,nameData=0x04000000,entries=0x04200000;
    Host host;
    Target frameTarget;
    std::function<bool(const ActionEdge&)> footerAction;
    std::uint64_t epoch=37;
    unsigned logs=0,keyRejects=0,diagnostics=0;
    bool alive=true,physical=false;
    Consumer() {
        host.alive=&alive;
        host.word(base+0x02FB00C8,nameData);host.word(base+0x02FB00CC,64);host.word(base+0x02FB00D0,64);
        for(unsigned i=0;i<std::size(cases);++i) {
            const auto index=cases[i].command==NativeFooterCommand::SellTrash?2u:i+1;
            host.word(base+cases[i].rva,index);host.word(base+cases[i].rva+4,0);
            host.word(nameData+index*4,entries+index*0x100);host.text(entries+index*0x100+8,cases[i].spelling);
        }
    }
    bool span(std::uint32_t p,std::size_t size) const {
        return p>=0x10000 && p<=host.GetGuestMemorySize() && size<=host.GetGuestMemorySize()-p;
    }
    std::uint32_t read(std::uint32_t p) const {return span(p,4)?host.MemoryRead32(p):0;}
    bool current() const {return alive && !physical;}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args) {host.RunGuestCallback(pc,args);return true;}
    template<class... Args> void inventoryDiagnostic(Args&&...) {}
    void networkDiagnostic(const Target&,const char*) {++diagnostics;}
    void spelling(std::uint32_t rva,const std::string& value) {
        const auto entry=read(nameData+read(base+rva)*4);host.text(entry+8,value);
    }
#include "ui_footer_keys.inc"
};
void dispatched(const Consumer& consumer,std::uint32_t rva) {
    assert(consumer.host.calls.size()==2 && consumer.diagnostics==1);
    const auto index=consumer.read(Consumer::base+rva);
    for(unsigned event=0;event<2;++event) {
        const auto& call=consumer.host.calls[event];
        assert(call.pc==Consumer::base+kMovieKey);
        assert((call.args==std::vector<std::uint32_t>{consumer.frameTarget.gfx,consumer.frameTarget.controllerId,
            consumer.frameTarget.movie,index,0,event}));
    }
}
}

int main() {
    unsigned positives=0,negatives=0;
    for(const auto& key:cases) {
        Consumer c;if(key.command==NativeFooterCommand::SellTrash) c.frameTarget.direct.movieClass="VendingMachineExGFxMovie";
        assert(c.footerKey(key.command));dispatched(c,key.rva);++positives;
        std::string cooked=key.spelling;
        if(const auto at=cooked.find("Thumbstick");at!=std::string::npos) cooked[at+5]='S';
        else for(auto& ch:cooked) if(ch>='A' && ch<='Z') ch=static_cast<char>(ch+('a'-'A'));
        Consumer mixed;if(key.command==NativeFooterCommand::SellTrash) mixed.frameTarget.direct.movieClass="VendingMachineExGFxMovie";
        mixed.spelling(key.rva,cooked);
        assert(mixed.footerKey(key.command));dispatched(mixed,key.rva);++positives;
        for(unsigned fault=0;fault<12;++fault) {
            Consumer wrong;if(key.command==NativeFooterCommand::SellTrash) wrong.frameTarget.direct.movieClass="VendingMachineExGFxMovie";
            switch(fault) {
            case 0:wrong.spelling(key.rva,std::string(key.spelling)+"Extra");break;
            case 1:wrong.spelling(key.rva,std::string(key.spelling).substr(0,std::strlen(key.spelling)-1));break;
            case 2:wrong.host.word(Consumer::base+key.rva+4,1);break;
            case 3:wrong.host.word(Consumer::base+key.rva,64);break;
            case 4:wrong.host.word(Consumer::base+0x02FB00D0,63);break;
            case 5:wrong.host.word(Consumer::nameData+wrong.read(Consumer::base+key.rva)*4,0xFFFF);break;
            case 6:wrong.spelling(key.rva,"");break;
            case 7:wrong.spelling(key.rva,std::string(128,'x'));break;
            case 8:wrong.spelling(key.rva,std::string(key.spelling)+static_cast<char>(0x80));break;
            case 9:wrong.spelling(key.rva,std::string(key.spelling)+"\n");break;
            case 10:wrong.alive=false;break;
            case 11:wrong.physical=true;break;
            }
            assert(!wrong.footerKey(key.command) && wrong.host.calls.empty());
            if(fault>=10) assert(wrong.host.reads==0);
            ++negatives;
        }
    }
    {
        Consumer c;c.spelling(0x03009FD0,"PageDown[");
        assert(!c.keyName(0x03009FD0,"PageDown{"));assert(c.host.calls.empty());++negatives;
#if defined(BL2_NATIVE_ARM32)
        assert(!c.keyName(0x03009FD0,nullptr));assert(c.host.calls.empty());++negatives;
#endif
    }
    for(auto command:{NativeFooterCommand::SellTrash,NativeFooterCommand::Legend,NativeFooterCommand::TrainingAccept}) {
        Consumer c;assert(!c.footerKey(command) && c.host.calls.empty());++negatives;
    }
    for(const auto& key:cases) {
        Consumer c;if(key.command==NativeFooterCommand::SellTrash) c.frameTarget.direct.movieClass="VendingMachineExGFxMovie";
        c.host.loseAfterDown=true;assert(!c.footerKey(key.command));
        assert(c.host.calls.size()==1 && c.host.calls[0].args.back()==0 && c.diagnostics==1);++negatives;
    }
    for(auto command:{NativeFooterCommand::ButtonA,NativeFooterCommand::ButtonB}) {
        for(bool handled:{false,true}) {
            Consumer c;std::vector<ActionEdge> events;
            c.footerAction=[&](const ActionEdge& edge) {events.push_back(edge);return handled;};
            assert(c.footerKey(command));
            assert(events.size()==(handled?2u:1u));
            assert(events[0].down && events[0].sourceEpoch==c.epoch &&
                events[0].action==(command==NativeFooterCommand::ButtonA?Action::Confirm:Action::Back));
            if(handled) assert(!events[1].down && c.host.calls.empty());
            else assert(c.host.calls.size()==2);
            ++positives;
            c.alive=false;events.clear();assert(!c.footerKey(command) && events.empty());++negatives;
        }
    }
    std::printf("PASS: production footer key dispatch %u positive/%u negative; original FName storage and pressed/released ABI\n",positives,negatives);
}
