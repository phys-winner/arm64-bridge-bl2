// Actual A64 observation body; JNI token resolver/table calls are explicit substitutes.
#define __ANDROID__ 1
#if defined(_MSC_VER)
#define __attribute__(value)
#endif
#include "bl2_touch_text_chat.h"
#include "jni_fake_env.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
using namespace bl2::touch;
static bool exception=false,missingEnv=false,missingObject=false,missingField=false,changeActivity=false;
static unsigned resolves=0,objectRefs=0,classRefs=0;
static _jobject activityObject;
static _jclass activityClass;
static JNINativeInterface jniTable{};
static JNIEnv jniEnv{&jniTable};
static jboolean exceptionCheck(JNIEnv*){return exception?JNI_TRUE:JNI_FALSE;}
static void exceptionClear(JNIEnv*){exception=false;}
static jclass objectClass(JNIEnv*,jobject o){assert(o==&activityObject);return &activityClass;}
static jfieldID fieldId(JNIEnv*,jclass c,const char* name,const char* sig) {
    assert(c==&activityClass && std::strcmp(sig,"Z")==0);
    return missingField?nullptr:reinterpret_cast<jfieldID>(std::uintptr_t{std::strcmp(name,"bKeyboardOpen")==0?1u:2u});
}
static jboolean boolField(JNIEnv*,jobject o,jfieldID f) {
    assert(o==&activityObject);return reinterpret_cast<std::uintptr_t>(f)==2?JNI_TRUE:JNI_FALSE;
}
static void deleteLocal(JNIEnv*,jobject o){if(o==&activityObject)++objectRefs;else {assert(o==&activityClass);++classRefs;}}
FakeJniEnv& FakeJniEnv::Instance(){static FakeJniEnv fake;fake.m_env_table=0x20000;return fake;}
JNIEnv* FakeJniEnv::GetEnv() const{return missingEnv?nullptr:&jniEnv;}
jobject FakeJniEnv::ResolveObject(std::uint32_t handle) const {
    assert(handle==0x2F2A);++resolves;return missingObject?nullptr:&activityObject;
}
struct Vm final:GfxTransport {
    static constexpr std::uint32_t base=0x40000000;
    std::map<std::uint32_t,std::uint32_t> words;
    std::uint32_t rawEnv=0x20000;bool live=true;
    bool read(std::uint32_t p,void* out,std::size_t n) const override {
        const auto it=words.find(p);if(it==words.end() || n!=4)return false;std::memcpy(out,&it->second,4);return true;
    }
    bool write(std::uint32_t,const void*,std::size_t) override{return false;}
    bool executable(std::uint32_t) const override{return true;}
    bool current() const override{return live;}
    void abandonScratch() override{}
    bool call(std::uint32_t p,const std::vector<std::uint32_t>& args,std::uint32_t& out) override {
        assert(p==base+0x01E18554 && args.empty());out=rawEnv;
        if(changeActivity)words[base+0x03057590+0x28]=0x2F2B;
        return true;
    }
};
namespace bl2::touch {
namespace {
#include "arm64_chat_observation_helpers.inc"
}
#include "arm64_chat_observation_production.inc"
}
int main() {
    jniTable.ExceptionCheck=exceptionCheck;jniTable.ExceptionClear=exceptionClear;jniTable.GetObjectClass=objectClass;
    jniTable.GetFieldID=fieldId;jniTable.GetBooleanField=boolField;jniTable.DeleteLocalRef=deleteLocal;
    for(unsigned test=0;test<8;++test) {
        Vm vm;for(const auto& site:keyboardSites())for(unsigned i=0;i<site.count;++i)vm.words[vm.base+site.rva+i*4]=site.words[i];
        vm.words[vm.base+0x03057590+0x28]=0x2F2A;vm.words[vm.base+0x03057590+0x120]=0;
        exception=false;resolves=objectRefs=classRefs=0;missingEnv=test==2;missingObject=test==3;missingField=test==4;changeActivity=test==5;
        if(test==1)vm.rawEnv=0x21000;if(test==6)vm.words[vm.base+0x03057590+0x120]=2;if(test==7)vm.live=false;
        const auto state=ObserveBl2TextChatKeyboard(vm,vm.base);
        assert(state.known==(test==0));
        if(test==0)assert(state.active && !state.open && !state.done && state.activity==0x2F2A);
        assert(objectRefs==(test==0 || test==4 || test==5?1u:0u));
        assert(classRefs==objectRefs);
        if(test==1 || test==2 || test==6 || test==7)assert(resolves==0);
    }
    std::puts("PASS: A64 production Activity observation resolves handles, rejects stale/foreign service, releases JNI refs");
}
