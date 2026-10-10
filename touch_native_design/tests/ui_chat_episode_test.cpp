// The generated method is the actual sink guard; metadata and Stop detection
// use the production reader. No unfocused AS calls or keyboard callbacks occur.
#include "ui_chat_model_fixture.inc"
#include "bl2_touch_text_chat.h"
using namespace bl2::touch;
struct ChatEpisodeConsumer {
    Fixture& f;
    const std::uint32_t base=Fixture::base;
    struct Keyboard {bool busy=false;bool pending() const {return busy;}} chatKeyboard;
    TextChatOwner chatOwner{};
    std::uint64_t chatObject=0,chatAttemptAt=0;
    unsigned chatAttempts=0;
    std::uint32_t read(std::uint32_t p) const {std::uint32_t value=0;Memory::read(&f.memory,p,&value,4);return value;}
    bool span(std::uint32_t p,std::size_t n) const {unsigned char out[256]{};return n<=sizeof(out)&&Memory::read(&f.memory,p,out,n);}
    bool array(std::uint32_t p,std::uint32_t limit,std::uint32_t& data,std::uint32_t& count) const {
        data=read(p);count=read(p+4);return count<=read(p+8)&&read(p+8)<=limit&&(!count||span(data,count*4));
    }
    std::uint64_t token(std::uint32_t p) const {
        const auto index=read(p+0x20);
        return p && index<read(base+0x02FBA268+4) && read(f.objects+index*4)==p ? (std::uint64_t(p)<<32)|index:0;
    }
    static bool copyMemory(const void* c,std::uint32_t p,void* out,std::size_t n) {
        return Memory::read(&static_cast<const ChatEpisodeConsumer*>(c)->f.memory,p,out,n);
    }
#include "ui_chat_episode_production.inc"
};
int main() {
    Fixture f;
    const auto movie=f.type("GFxMoviePlayer",0x300),chatType=f.type("TextChatGFxMovie",0x400,movie),
        gfxObject=f.type("GFxObject",0x100),owner=f.object("TextChat",chatType),chat=f.object("pcChat",gfxObject);
    const auto chatProperty=f.property(chatType,"ChatObj","ObjectProperty",0x300,4),
        capture=f.property(movie,"bCaptureInput","BoolProperty",0x200,4);
    f.memory.word(chatProperty+0x74,gfxObject);f.memory.word(chatProperty+0x48,0x2000);f.memory.word(owner+0x300,chat);
    f.memory.word(capture+0x74,0x40);
    const auto engine=f.object("Engine",f.type("Engine",0x600)),local=f.object("LocalPlayer",f.type("LocalPlayer",0x300)),
        controller=f.object("Controller",f.type("PlayerController",0x500)),world=f.object("World",f.type("World",0x400));
    const auto players=f.memory.allocate(16),vp=f.memory.allocate(128),gfx=f.memory.allocate(128);
    f.memory.word(f.base+0x03008D20,engine);f.memory.word(f.base+0x0300D940,world);f.memory.word(f.base+0x0301F840,gfx);
    f.memory.word(players,local);f.memory.array(engine+0x48C,players,1,4);f.memory.word(local+0x40,controller);
    f.memory.word(gfx+0x44,vp+4);f.memory.word(vp+0x3C,1200);f.memory.word(vp+0x40,540);
    ChatEpisodeConsumer state{f};
    TextChatOwner identity{5,50,10,state.token(owner),state.token(controller),state.token(world),
        0x67000000,0x68000000,vp,1200,540,0x69000000,{0x70000000,1,0,10},{0x70000000,2,0,10}};
    assert(identity);
    const auto accepted=[&] {state.chatOwner=identity;state.chatObject=state.token(chat);state.chatAttempts=3;state.chatAttemptAt=1000;};
    const TextChatKeyboardState active{identity.activity,true,true,false,true};
    accepted();f.memory.word(owner+0x200,0x40);state.stopChatEpisode(active);
    assert(state.chatOwner==identity && state.chatAttempts==3); // Touch epoch/OnPause did not Stop this input.
    f.memory.word(owner+0x200,0);state.stopChatEpisode(active);
    assert(!state.chatOwner && !state.chatObject && state.chatAttempts==0 && !state.chatAttemptAt);
    // Genuine Start reuses the SAME owner, ChatObj and input. The stopped
    // episode now permits a new Show; an unchanged active input never does.
    accepted();f.memory.word(owner+0x200,0x40);state.stopChatEpisode(active);assert(state.chatAttempts==3);
    f.memory.word(owner+0x200,0);
    for(unsigned fault=0;fault<11;++fault) {
        accepted();auto service=active;const auto before=f.memory.word(gfx+0x44);
        if(fault==0)service.known=false;
        if(fault==1)service.active=false;
        if(fault==2)service.activity+=4;
        if(fault==3)service.open=true;
        if(fault==4)state.chatKeyboard.busy=true;
        if(fault==5)state.chatOwner.world+=1;
        if(fault==6)state.chatOwner.controller+=1;
        if(fault==7)state.chatOwner.width+=1;
        if(fault==8)f.memory.word(gfx+0x44,vp+8);
        if(fault==9)f.memory.word(engine+0x48C+4,2);
        if(fault==10)state.chatObject+=1;
        state.stopChatEpisode(service);assert(state.chatOwner && state.chatAttempts==3 && state.chatAttemptAt==1000);
        f.memory.word(gfx+0x44,before);f.memory.word(engine+0x48C+4,1);state.chatKeyboard.busy=false;
    }
    accepted();state.stopChatEpisode(active);assert(!state.chatOwner && !state.chatAttempts);
    std::puts("PASS: production same-owner Start/Stop episode and11 Activity/lifetime/modal/geometry negatives; no callbacks");
}
