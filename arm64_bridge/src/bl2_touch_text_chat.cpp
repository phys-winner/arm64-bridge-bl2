#include "bl2_touch_text_chat.h"
#include "bl2_touch_input.h"
#include <algorithm>
#include <array>
#include <cmath>
#if defined(__ANDROID__)
#include <jni.h>
#if !defined(BL2_NATIVE_ARM32)
#include "jni_fake_env.h"
#endif
#endif

namespace bl2::touch {
namespace {
constexpr std::uint32_t kKeyboard=0x01E19790,kDone=0x01E18450,kString=0x01E1846C,kEnv=0x01E18554;
// Original IsVirtualKeyboardDone/GetVirtualKeyboardString instructions resolve
// the same globals: +114 TArray<char>, +120 done, +124 cancel, +28 Activity.
constexpr std::uint32_t kGlobals=0x03057590;
bool word(const GfxTransport& vm,std::uint32_t p,std::uint32_t& value) { return vm.read(p,&value,4); }
bool keyboardAbi(const GfxTransport& vm,std::uint32_t base) {
    if(!base || std::uint64_t(base)+0x03089000>UINT32_MAX) return false;
    struct Site {std::uint32_t rva,count;std::array<std::uint32_t,7> words;};
    constexpr Site sites[]={{kKeyboard,2,{0xE92D47F0,0xE24DD010}},
        {kDone,7,{0xE59F3010,0xE08F3003,0xE5932124,0xE5802000,0xE5930120,0xE12FFF1E,0x0123F134}},
        {kString,5,{0xE59F0008,0xE08F0000,0xE2800F45,0xE12FFF1E,0x0123F118}},
        {kEnv,2,{0xE92D40F0,0xE24DD014}}};
    for(const auto& site:sites) {
        if(!vm.executable(base+site.rva)) return false;
        for(unsigned i=0;i<site.count;++i) {
            std::uint32_t actual=0;
            if(!word(vm,base+site.rva+4*i,actual) || actual!=site.words[i]) return false;
        }
    }
    return true;
}
bool number(GfxSession& s,GfxHandle object,const char* key,double& out) {
    const auto member=s.member(object,key);const bool okay=s.number(member,out);s.release(member);return okay;
}
bool visible(GfxSession& s,GfxHandle object) {
    const auto value=s.member(object,"_visible");bool enabled=false;double alpha=0;
    const bool okay=s.boolean(value,enabled) && enabled;s.release(value);
    return okay && number(s,object,"_alpha",alpha) && alpha>0 && alpha<=100;
}
bool string(GfxSession& s,GfxHandle object,const char* key,std::string& out,std::size_t limit=1024) {
    const auto value=s.member(object,key);const bool okay=s.string(value,out,limit);s.release(value);return okay;
}
bool scalar(const std::string& text,std::size_t& at,std::uint32_t& cp) {
    if(at>=text.size()) return false;
    const auto first=static_cast<unsigned char>(text[at++]);unsigned bytes=0;std::uint32_t minimum=0;
    if(first<0x80) cp=first;
    else if(first>=0xC2 && first<=0xDF) {cp=first&31;bytes=1;minimum=0x80;}
    else if(first>=0xE0 && first<=0xEF) {cp=first&15;bytes=2;minimum=0x800;}
    else if(first>=0xF0 && first<=0xF4) {cp=first&7;bytes=3;minimum=0x10000;}
    else return false;
    for(unsigned i=0;i<bytes;++i) {
        if(at>=text.size()) return false;
        const auto c=static_cast<unsigned char>(text[at++]);if((c&0xC0)!=0x80) return false;
        cp=(cp<<6)|(c&63);
    }
    return cp>=minimum && cp<=0x10FFFF && cp>=32 && cp!=127;
}
void append(std::string& out,std::uint32_t cp) {
    if(cp<0x80) out+=static_cast<char>(cp);
    else if(cp<0x800) {out+=static_cast<char>(0xC0|(cp>>6));out+=static_cast<char>(0x80|(cp&63));}
    else if(cp<0x10000) {
        out+=static_cast<char>(0xE0|(cp>>12));out+=static_cast<char>(0x80|((cp>>6)&63));out+=static_cast<char>(0x80|(cp&63));
    } else {
        out+=static_cast<char>(0xF0|(cp>>18));out+=static_cast<char>(0x80|((cp>>12)&63));
        out+=static_cast<char>(0x80|((cp>>6)&63));out+=static_cast<char>(0x80|(cp&63));
    }
}
[[maybe_unused]] std::string modified(const std::string& text) {
    std::string out;std::size_t at=0;std::uint32_t cp=0;
    while(scalar(text,at,cp)) {
        if(cp<0x10000) append(out,cp);
        else {cp-=0x10000;append(out,0xD800+(cp>>10));append(out,0xDC00+(cp&1023));}
    }
    return out;
}
}

std::optional<std::string> Bl2TextChatUtf8(const std::string& text) {
    if(text.size()>512) return {};
    std::string out;std::size_t at=0;unsigned units=0;
    while(at<text.size()) {
        std::uint32_t cp=0;if(!scalar(text,at,cp)) return {};
        if(cp>=0xD800 && cp<=0xDBFF) {
            std::uint32_t low=0;if(!scalar(text,at,low) || low<0xDC00 || low>0xDFFF) return {};
            cp=0x10000+((cp-0xD800)<<10)+(low-0xDC00);
        } else if(cp>=0xDC00 && cp<=0xDFFF) return {};
        units+=cp>0xFFFF ? 2u : 1u;if(units>128) return {};
        append(out,cp);
    }
    return out;
}

bool CaptureBl2TextChatInput(GfxSession& s,GfxHandle chat,TextChatInput& out) {
    out={};std::string name;
    if(!visible(s,chat) || !string(s,chat,"_name",name,32) || name!="pcChat") return false;
    const auto input=s.member(chat,"chatInput"),parent=s.member(input,"_parent");
    const auto focus=s.invoke(input,"__get__focused"),maximum=s.invoke(input,"__get__maxChars");
    const auto text=s.invoke(input,"__get__text");TextChatInput candidate;GfxIdentity expected;
    double focused=0,maxChars=0;
    bool okay=visible(s,input) && string(s,input,"_name",name,32) && name=="chatInput" &&
        s.identity(chat,candidate.chat) && s.identity(parent,expected) && expected==candidate.chat &&
        s.identity(input,candidate.input) && s.number(focus,focused) && focused==1 &&
        s.number(maximum,maxChars) && maxChars==128 && s.string(text,candidate.text,512) &&
        Bl2TextChatUtf8(candidate.text).has_value() && s.projectedBounds(input,candidate.bounds);
    GfxIdentity final;const auto currentInput=s.member(chat,"chatInput");
    const auto currentFocus=s.invoke(input,"__get__focused");double finalFocus=0;
    okay=okay && s.identity(currentInput,final) && final==candidate.input && visible(s,chat) && visible(s,input) &&
        s.number(currentFocus,finalFocus) && finalFocus==1;
    s.release(currentFocus);
    s.release(currentInput);
    s.release(text);s.release(maximum);s.release(focus);s.release(parent);s.release(input);
    if(okay && s.good()) out=std::move(candidate);
    return okay && s.good();
}
bool AssignBl2TextChatInput(GfxSession& s,GfxHandle chat,const TextChatInput& captured,const std::string& text) {
    const auto normal=Bl2TextChatUtf8(text);TextChatInput fresh;
    if(!normal || !CaptureBl2TextChatInput(s,chat,fresh) || fresh.chat!=captured.chat || fresh.input!=captured.input) return false;
    const auto input=s.member(chat,"chatInput"),parts=s.array();bool okay=input && parts;
    // Original AS Array.join owns the resulting complete string; per-slot
    // stringValue remains below256 bytes and never truncates Cyrillic/emoji.
    for(std::size_t at=0;okay && at<normal->size();) {
        std::size_t end=std::min(at+255,normal->size());
        while(end<normal->size() && (static_cast<unsigned char>((*normal)[end])&0xC0)==0x80) --end;
        const auto part=s.stringValue(normal->substr(at,end-at));
        const auto pushed=part ? s.invoke(parts,"push",{part},true) : GfxHandle{};
        okay=static_cast<bool>(pushed);s.release(pushed);s.release(part);at=end;
    }
    const auto separator=s.stringValue("");
    const auto whole=okay && separator ? s.invoke(parts,"join",{separator}) : GfxHandle{};
    const auto set=whole ? s.invoke(input,"__set__text",{whole},true) : GfxHandle{};
    okay=okay && whole && set && s.good();
    s.release(set);s.release(whole);s.release(separator);s.release(parts);
    const auto readback=s.invoke(input,"__get__text");std::string actual;
    okay=okay && readback && s.string(readback,actual,512) && actual==*normal && s.good();
    GfxIdentity final;const auto currentInput=s.member(chat,"chatInput");
    okay=okay && s.identity(currentInput,final) && final==fresh.input;
    s.release(currentInput);s.release(readback);s.release(input);return okay && s.good();
}

TextChatOwner::operator bool() const {
    return epoch && scene && generation && owner && controller && world && movie && interface && viewport && activity &&
        width && height && width<=16384 && height<=16384 && chat.interface && chat.data && input.interface && input.data && chat!=input;
}
TextChatKeyboardState ObserveBl2TextChatKeyboard(GfxTransport& vm,std::uint32_t base) {
    TextChatKeyboardState state;
#if defined(__ANDROID__)
    std::uint32_t rawEnv=0,done=0;
    if(!vm.current() || !keyboardAbi(vm,base) || !word(vm,base+kGlobals+0x28,state.activity) || !state.activity ||
        !word(vm,base+kGlobals+0x120,done) || done>1 || !vm.call(base+kEnv,{},rawEnv) || !rawEnv) return {};
#if defined(BL2_NATIVE_ARM32)
    auto* env=reinterpret_cast<JNIEnv*>(static_cast<std::uintptr_t>(rawEnv));
    if(env->ExceptionCheck()) return {};
    const auto object=reinterpret_cast<jobject>(static_cast<std::uintptr_t>(state.activity));
#else
    auto& fake=FakeJniEnv::Instance();
    if(rawEnv!=fake.EnvTableGuestAddr()) return {};
    auto* env=fake.GetEnv();if(!env || env->ExceptionCheck()) return {};
    const auto object=fake.ResolveObject(state.activity);if(!object) return {};
    struct ActivityRef {JNIEnv* env;jobject object;~ActivityRef(){env->DeleteLocalRef(object);}} owned{env,object};
#endif
    const auto cls=env->GetObjectClass(object);
    if(!cls) {if(env->ExceptionCheck()) env->ExceptionClear();return {};}
    const auto open=env->GetFieldID(cls,"bKeyboardOpen","Z");
    const auto active=!env->ExceptionCheck() ? env->GetFieldID(cls,"bAppActive","Z") : nullptr;
    if(open && active && !env->ExceptionCheck()) {
        state.open=env->GetBooleanField(object,open)!=JNI_FALSE;
        state.active=env->GetBooleanField(object,active)!=JNI_FALSE;
        state.known=!env->ExceptionCheck();state.done=done!=0;
    }
    env->DeleteLocalRef(cls);
    if(env->ExceptionCheck()) env->ExceptionClear();
    std::uint32_t currentActivity=0;
    if(!vm.current() || !word(vm,base+kGlobals+0x28,currentActivity) || currentActivity!=state.activity ||
        !word(vm,base+kGlobals+0x120,done) || done>1) return {};
    state.done=done!=0;
#else
    (void)vm;(void)base;
#endif
    return state;
}
bool ShowBl2TextChatKeyboard(GfxTransport& vm,std::uint32_t base,std::uint32_t scratch,
                            const std::string& title,const std::string& initial) {
#if defined(__ANDROID__)
    const auto text=Bl2TextChatUtf8(initial),caption=Bl2TextChatUtf8(title);
    if(!text || !caption || title.size()>128 || !scratch || std::uint64_t(scratch)+1024>UINT32_MAX) return false;
    const auto service=ObserveBl2TextChatKeyboard(vm,base);
    if(!service.known || !service.active || service.open || !vm.current()) return false;
    const auto captionBytes=modified(*caption),textBytes=modified(*text);
    const std::uint32_t titleAt=scratch,emptyAt=scratch+512,textAt=scratch+528;
    const char empty=0;std::uint32_t shown=0;
    return vm.write(titleAt,captionBytes.c_str(),captionBytes.size()+1) && vm.write(emptyAt,&empty,1) &&
        vm.write(textAt,textBytes.c_str(),textBytes.size()+1) && vm.current() &&
        vm.call(base+kKeyboard,{titleAt,emptyAt,textAt,128,0},shown) && shown==1;
#else
    (void)vm;(void)base;(void)scratch;(void)title;(void)initial;return false;
#endif
}
bool UiTextChatKeyboard::opened(const TextChatOwner& owner,std::uint64_t now) {
    if(pending_ || !owner) return false;
    owner_=owner;time_=now;pending_=true;retired_=false;observedModal_=false;return true;
}
bool UiTextChatKeyboard::acknowledgeModalEpoch(const TextChatOwner& current,const TextChatKeyboardState& service,
                                               std::uint32_t resets,std::uint64_t now) {
    auto originalEpoch=current;originalEpoch.epoch=owner_.epoch;
    const bool okay=pending_ && !retired_ && current && originalEpoch==owner_ && now>=time_ &&
        current.epoch>=owner_.epoch && !(resets&~(ManualReset|HeartbeatLost)) &&
        service.known && service.active && service.activity==owner_.activity &&
        (service.open || (observedModal_ && service.done));
    if(!okay) return false;
    observedModal_|=service.open;owner_.epoch=current.epoch;time_=now;return true;
}
std::optional<TextChatResult> UiTextChatKeyboard::poll(GfxTransport& vm,std::uint32_t base,const TextChatOwner& current,
                                                      const TextChatKeyboardState& service,bool allowed,bool reset,std::uint64_t now) {
    if(!pending_) return {};
    retired_|=reset || !allowed || current!=owner_ || now<time_ || !vm.current() ||
        !service.known || !service.active || service.activity!=owner_.activity;time_=now;
    if(!keyboardAbi(vm,base)) {retired_=true;return {};}
    std::uint32_t done=0,canceled=0;
    if(!word(vm,base+kGlobals+0x120,done) || !done) return {};
    pending_=false;
    if(retired_ || done!=1 || !word(vm,base+kGlobals+0x124,canceled) || canceled>1) return {};
    const auto stillCurrent=[&] {
        std::uint32_t finalDone=0,finalCancel=0,activity=0;
        return word(vm,base+kGlobals+0x120,finalDone) && finalDone==done &&
            word(vm,base+kGlobals+0x124,finalCancel) && finalCancel==canceled &&
            word(vm,base+kGlobals+0x28,activity) && activity==owner_.activity && vm.current();
    };
    const auto cancel=[&]() -> std::optional<TextChatResult> {
        return stillCurrent() ? std::optional<TextChatResult>{TextChatResult{owner_,{},true}} : std::nullopt;
    };
    if(canceled) return cancel();
    std::array<std::uint32_t,3> array{};
    if(!vm.read(base+kGlobals+0x114,array.data(),12) || array[1]>array[2] || array[1]>385 ||
        (!array[0] && array[1])) return cancel();
    std::string bytes(array[1],'\0');
    if(array[1] && (!vm.read(array[0],bytes.data(),bytes.size()) || bytes.back()!=0)) return cancel();
    if(!bytes.empty()) bytes.pop_back();
    const auto text=Bl2TextChatUtf8(bytes);std::array<std::uint32_t,3> final{};
    if(!vm.read(base+kGlobals+0x114,final.data(),12) || final!=array || !stillCurrent()) return {};
    if(!text || text->empty()) return TextChatResult{owner_,{},true};
    return TextChatResult{owner_,*text,false};
}
} // namespace bl2::touch
