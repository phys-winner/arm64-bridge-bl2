#include "gfx_widget_fixture.h"
#include "bl2_touch_text_chat.h"
#include "bl2_touch_input.h"
#include <iostream>
using namespace bl2::touch;
namespace {
using widget_fixture::Raw;
constexpr auto base=widget_fixture::base;
constexpr std::uint32_t globals=base+0x03057590,resultAt=0x26000000;
struct ChatVm final : GfxTransport {
    widget_fixture::Vm vm;
    Raw chat{},input{};
    unsigned setterCalls=0,parts=0;
    std::size_t largestPart=0;
    bool rejectSetter=false,replaceDuringProjection=false,unfocusDuringProjection=false;
    mutable bool mutateResult=false;
    explicit ChatVm(const char* fixture,const char* keyboard) : vm(fixture) {
        std::ifstream in(keyboard);assert(in.good());std::uint32_t address=0,word=0;
        while(in>>std::hex>>address>>word) vm.put(base+address,word);
        chat=vm.make(10);input=vm.make(10);vm.visible(chat,vm.root);vm.visible(input,chat);
        vm.prop(chat,"_name",vm.str("pcChat"));vm.prop(chat,"chatInput",input);
        vm.prop(input,"_name",vm.str("chatInput"));vm.prop(input,"focused",widget_fixture::num(1));
        vm.prop(input,"maxChars",widget_fixture::num(128));vm.prop(input,"value",vm.str(""));
        vm.object(input).bounds={10,20,1200,65};vm.object(input).tx=40;vm.object(input).ty=200;
        vm.write(widget_fixture::wrapper+0x3C,chat.data(),32);
        vm.put(globals+0x120,0u);vm.put(globals+0x124,0u);vm.put(globals+0x28,0x1200u);
    }
    std::string full(std::uint32_t p) const {
        std::string out;char c=0;
        for(unsigned i=0;i<1024 && vm.read(p+i,&c,1) && c;++i) out+=c;
        return out;
    }
    std::string valueText(const Raw& r) const {
        assert((r[3]&15)==6);std::uint32_t p=r[4];
        if(r[3]&0x40) assert(vm.read(p,&p,4));
        return full(p);
    }
    Raw str(const std::string& text,bool managed=false) {
        Raw r{};r[3]=6;r[4]=vm.next;vm.next+=0x400;
        vm.write(r[4],text.c_str(),text.size()+1);
        if(managed) {
            const auto p=vm.next;vm.next+=0x100;vm.put(p,r[4]);r[4]=p;r[2]=widget_fixture::interface;r[3]|=0x40;
        }
        return r;
    }
    bool read(std::uint32_t p,void* out,std::size_t n) const override {
        const bool okay=vm.read(p,out,n);
        if(p==resultAt && mutateResult) {
            mutateResult=false;const_cast<ChatVm*>(this)->vm.put(globals+0x118,0u);
        }
        return okay;
    }
    bool write(std::uint32_t p,const void* in,std::size_t n) override {return vm.write(p,in,n);}
    bool executable(std::uint32_t p) const override {return vm.executable(p);}
    bool current() const override {return vm.current();}
    void abandonScratch() override {vm.abandonScratch();}
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& a,std::uint32_t& result) override {
        if(pc==base+GfxSession::kCreateObject && vm.text(a[2])=="Array") {
            vm.output(a[1],vm.make(9));result=0;return true;
        }
        if(pc==widget_fixture::invoke) {
            const auto method=vm.text(a[3]);
            if(method=="__get__focused" || method=="__get__maxChars" || method=="__get__text") {
                assert(a[1]==input[4] && a[5]==0);
                const char* key=method=="__get__focused" ? "focused" : method=="__get__maxChars" ? "maxChars" : "value";
                vm.output(a[2],vm.object(input).properties.at(key));result=1;return true;
            }
            if(method=="__set__text") {
                assert(a[1]==input[4] && a[5]==1);++setterCalls;
                if(rejectSetter) {vm.output(a[2],{});result=0;return true;}
                const auto value=str(valueText(vm.raw(a[4])));vm.prop(input,"value",value);
                vm.output(a[2],value);result=1;return true;
            }
            if(method=="push") {
                assert(vm.objects.at(a[1]).type==9 && a[5]==1);
                const auto text=valueText(vm.raw(a[4]));++parts;largestPart=std::max(largestPart,text.size());
                assert(text.size()<256 && Bl2TextChatUtf8(text).has_value());
                vm.objects.at(a[1]).array.push_back(str(text));
                vm.output(a[2],widget_fixture::num(static_cast<double>(vm.objects.at(a[1]).array.size())));result=1;return true;
            }
            if(method=="join") {
                assert(vm.objects.at(a[1]).type==9 && a[5]==1 && valueText(vm.raw(a[4])).empty());
                std::string text;for(const auto& part:vm.objects.at(a[1]).array) text+=valueText(part);
                vm.output(a[2],str(text,true));result=1;return true;
            }
            if(method=="localToGlobal") {
                const bool okay=vm.call(pc,a,result);
                if(replaceDuringProjection) {replaceDuringProjection=false;vm.prop(chat,"chatInput",vm.make(10));}
                if(unfocusDuringProjection) vm.prop(input,"focused",widget_fixture::num(0));
                return okay;
            }
        }
        return vm.call(pc,a,result);
    }
    TextChatOwner owner() const {
        return {1,20,30,0x1111,0x2222,0x3333,widget_fixture::movie,widget_fixture::interface,
            0x23456000,2560,1440,0x1200,
            {chat[2],chat[4],0,10},{input[2],input[4],0,10}};
    }
    TextChatKeyboardState service() const {
        std::uint32_t done=0;assert(vm.read(globals+0x120,&done,4));
        return {owner().activity,true,true,done==0,done==1};
    }
    void finish(const std::string& text,bool canceled=false) {
        vm.write(resultAt,text.c_str(),text.size()+1);
        const std::array<std::uint32_t,3> header{resultAt,static_cast<std::uint32_t>(text.size()+1),static_cast<std::uint32_t>(text.size()+1)};
        vm.write(globals+0x114,header.data(),12);vm.put(globals+0x120,1u);vm.put(globals+0x124,canceled ? 1u : 0u);
    }
};
void closed(ChatVm& vm,GfxSession& s,GfxHandle chat) {
    s.release(chat);s.close();assert(s.cleanupOkay() && vm.vm.refs.empty() && vm.vm.movieRefs==1 && !vm.vm.abandoned);
}
std::string repeated(const char* one,unsigned count) {std::string s;while(count--) s+=one;return s;}
}
int main(int argc,char** argv) {
    assert(argc==3);
    const std::string cyrillic=repeated("\xD0\x96",128),bmp=repeated("\xE6\xBC\xA2",128);
    const std::string emoji="\xF0\x9F\x98\x80",modified="\xED\xA0\xBD\xED\xB8\x80";
    assert(Bl2TextChatUtf8(cyrillic)==cyrillic && Bl2TextChatUtf8(bmp)==bmp);
    assert(Bl2TextChatUtf8(modified)==emoji && Bl2TextChatUtf8(repeated(modified.c_str(),64))==repeated(emoji.c_str(),64));
    for(const auto& bad:std::vector<std::string>{repeated("a",129),repeated(emoji.c_str(),65),"\xED\xA0\xBD","\xED\xB8\x80",
        "\xED\xA0\xBDx","\xC0\x80","\xF4\x90\x80\x80","\xE2\x82",std::string("a\0b",3),"x\ny"}) assert(!Bl2TextChatUtf8(bad));
    for(const auto& text:{std::string{},std::string("hello"),cyrillic,bmp,repeated(emoji.c_str(),64)}) {
        ChatVm vm(argv[1],argv[2]);GfxSession s(vm,base,widget_fixture::movie,widget_fixture::scratch1,384,true);
        const auto chat=s.pin(widget_fixture::wrapper+0x3C);TextChatInput captured;
        assert(CaptureBl2TextChatInput(s,chat,captured));assert(captured.bounds.left==50 && captured.bounds.top==220);
        assert(AssignBl2TextChatInput(s,chat,captured,text) && vm.valueText(vm.vm.object(vm.input).properties.at("value"))==text);
        assert(vm.setterCalls==1 && vm.largestPart<256 && vm.parts==(text.empty() ? 0u : text.size()<=255 ? 1u : 2u));
        closed(vm,s,chat);
    }
    for(unsigned bad=0;bad<9;++bad) {
        ChatVm vm(argv[1],argv[2]);
        if(bad==0) vm.vm.prop(vm.chat,"_name",vm.vm.str("foreign"));
        if(bad==1) vm.vm.prop(vm.input,"_parent",vm.vm.root);
        if(bad==2) vm.vm.prop(vm.input,"_visible",widget_fixture::boolean(false));
        if(bad==3) vm.vm.prop(vm.input,"focused",widget_fixture::num(0));
        if(bad==4) vm.vm.prop(vm.input,"maxChars",widget_fixture::num(256));
        if(bad==5) vm.vm.prop(vm.input,"value",vm.str(repeated("a",129)));
        if(bad==6) vm.replaceDuringProjection=true;
        if(bad==7) vm.unfocusDuringProjection=true;
        GfxSession s(vm,base,widget_fixture::movie,widget_fixture::scratch1,bad==8 ? 8u : 192u);
        const auto chat=s.pin(widget_fixture::wrapper+0x3C);TextChatInput captured;
        assert(!CaptureBl2TextChatInput(s,chat,captured) && !captured.input.data);closed(vm,s,chat);
    }
    {
        ChatVm vm(argv[1],argv[2]);GfxSession s(vm,base,widget_fixture::movie,widget_fixture::scratch1,384,true);
        const auto chat=s.pin(widget_fixture::wrapper+0x3C);TextChatInput captured;assert(CaptureBl2TextChatInput(s,chat,captured));
        vm.rejectSetter=true;assert(!AssignBl2TextChatInput(s,chat,captured,"") && vm.setterCalls==1);closed(vm,s,chat);
    }
    {
        ChatVm vm(argv[1],argv[2]);UiTextChatKeyboard keyboard;auto owner=vm.owner();
        assert(keyboard.opened(owner,100) && !keyboard.opened(owner,101));
        const TextChatKeyboardState modal{owner.activity,true,true,true,false};
        auto focused=owner;focused.epoch=2;
        assert(keyboard.acknowledgeModalEpoch(focused,modal,ManualReset,101));
        assert(!keyboard.poll(vm,base,focused,vm.service(),true,false,102));
        vm.finish(modified);auto result=keyboard.poll(vm,base,focused,vm.service(),true,false,103);
        assert(result && result->owner==focused && !result->canceled && result->text==emoji && !keyboard.pending());
        assert(!keyboard.poll(vm,base,focused,vm.service(),true,false,104));
    }
    for(unsigned bad=0;bad<17;++bad) {
        ChatVm vm(argv[1],argv[2]);UiTextChatKeyboard keyboard;const auto owner=vm.owner();auto current=owner;
        TextChatKeyboardState modal{owner.activity,true,true,true,false};assert(keyboard.opened(owner,100));
        std::uint32_t reset=ManualReset;std::uint64_t now=101;current.epoch=2;
        if(bad==0) ++current.owner;if(bad==1) ++current.controller;if(bad==2) ++current.world;
        if(bad==3) ++current.movie;if(bad==4) ++current.interface;if(bad==5) ++current.generation;
        if(bad==6) ++current.scene;if(bad==7) ++current.width;if(bad==8) ++current.viewport;
        if(bad==9) ++current.input.data;if(bad==10) ++current.chat.data;if(bad==11) modal.active=false;
        if(bad==12) modal.known=false;if(bad==13) ++modal.activity;if(bad==14) modal.open=false;
        if(bad==15) reset=ContextChanged;if(bad==16) now=99;
        assert(!keyboard.acknowledgeModalEpoch(current,modal,reset,now));
        vm.finish("late");assert(!keyboard.poll(vm,base,current,vm.service(),true,true,102) && !keyboard.pending());
    }
    for(unsigned bad=0;bad<9;++bad) {
        ChatVm vm(argv[1],argv[2]);UiTextChatKeyboard keyboard;const auto owner=vm.owner();assert(keyboard.opened(owner,100));
        vm.finish("hello",bad==0);
        if(bad==1) vm.finish("");if(bad==2) vm.finish("\xED\xA0\xBD");
        if(bad==3) vm.vm.put(globals+0x118,386u);
        if(bad==4) vm.mutateResult=true;
        if(bad==5) keyboard.retire();
        if(bad==6) vm.vm.currentScene=false;
        if(bad==7) vm.vm.put(base+0x1E18450,0u);
        if(bad==8) vm.vm.put(globals+0x28,0x1300u);
        const auto result=keyboard.poll(vm,base,owner,vm.service(),true,false,101);
        if(bad<=3) assert(result && result->canceled && result->text.empty());else assert(!result);
    }
    {
        ChatVm vm(argv[1],argv[2]);UiTextChatKeyboard keyboard;auto owner=vm.owner();assert(keyboard.opened(owner,100));
        const TextChatKeyboardState modal{owner.activity,true,true,true,false};
        assert(keyboard.acknowledgeModalEpoch(owner,modal,NoReset,101));
        ++owner.epoch;const TextChatKeyboardState completed{owner.activity,true,true,false,true};
        assert(keyboard.acknowledgeModalEpoch(owner,completed,ManualReset,102));
        vm.finish("typed");auto result=keyboard.poll(vm,base,owner,vm.service(),true,false,103);assert(result && result->text=="typed");
    }
    {
        ChatVm vm(argv[1],argv[2]);UiTextChatKeyboard keyboard;const auto owner=vm.owner();assert(keyboard.opened(owner,100));
        assert(!keyboard.poll(vm,base,owner,vm.service(),false,false,101) && keyboard.pending());
        // A physical controller taking ownership retires this keyboard request.
        // Later release cannot revive its message in the same menu.
        vm.finish("do not send");assert(!keyboard.poll(vm,base,owner,vm.service(),true,false,102) && !keyboard.pending());
    }
    {
        ChatVm vm(argv[1],argv[2]);UiTextChatKeyboard keyboard;const auto owner=vm.owner();assert(keyboard.opened(owner,100));
        vm.finish("home canceled");auto inactive=vm.service();inactive.active=false;
        assert(!keyboard.poll(vm,base,owner,inactive,true,false,101) && !keyboard.pending());
    }
    std::cout<<"PASS: original pcChat128 Unicode units, Array.join managed string and exact setter/readback/field guards\n";
    std::cout<<"PASS: explicit own-keyboard focus reset, strict world/owner/generation/surface/physical cancellation and late-result discard\n";
}
