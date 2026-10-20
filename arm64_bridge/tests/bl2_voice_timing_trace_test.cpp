#include "bl2_voice_timing_trace.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

struct WrapReader {
    static constexpr unsigned base=0x300000,stack=base+0x100,source=base+0x800,wide=base+0x1000,lines=base+0x2000;
    std::vector<unsigned char> bytes=std::vector<unsigned char>(0x5000);
    bool span(unsigned p,std::size_t n) const{return p>=base&&p-base<=bytes.size()&&n<=bytes.size()-(p-base);}
    static bool copyMemory(const void* context,unsigned p,void* out,std::size_t n) {
        const auto& r=*static_cast<const WrapReader*>(context);
        if(!r.span(p,n))return false;std::memcpy(out,r.bytes.data()+p-base,n);return true;
    }
    unsigned u32(unsigned p) const {
        assert(span(p,4));const auto* b=bytes.data()+p-base;
        return unsigned(b[0])|(unsigned(b[1])<<8)|(unsigned(b[2])<<16)|(unsigned(b[3])<<24);
    }
    void put(unsigned p,unsigned value){assert(span(p,4));for(unsigned i=0;i<4;++i)bytes[p-base+i]=static_cast<unsigned char>(value>>(i*8));}
    void string(unsigned p,const std::u16string& s) {
        assert(span(p,(s.size()+1)*2));unsigned at=p-base;
        for(auto c:s){bytes[at++]=static_cast<unsigned char>(c);bytes[at++]=static_cast<unsigned char>(c>>8);}
        bytes[at++]=0;bytes[at]=0;
    }
    WrapReader() {
        bytes[source-base]='a';bytes[source-base+1]='b';bytes[source-base+2]='c';
        put(stack+0x80,source);put(stack+0x84,4);put(stack+0x88,4);put(stack+0x1D4,wide);
        put(stack+0x98,lines);put(stack+0x9C,2);put(stack+0xA0,2);
        string(wide,u"abc");string(lines+0x100,u"a");string(lines+0x200,u"bc");
        put(lines,lines+0x100);put(lines+4,2);put(lines+20,lines+0x200);put(lines+24,3);
    }
};

struct Reader {
    unsigned base=0x40000000;
    std::vector<unsigned> objects;
    std::map<unsigned,std::pair<std::string,std::string>> names;
    bool validArray=true;
    bool array(unsigned p,unsigned limit,unsigned& data,unsigned& count) {
        assert(p==base+0x02FBA268&&limit==2000000);data=0x10000;count=unsigned(objects.size());return validArray;
    }
    unsigned u32(unsigned p) {assert(p>=0x10000&&(p-0x10000)/4<objects.size());return objects[(p-0x10000)/4];}
    bool object(unsigned p) {return names.contains(p);}
    bool span(unsigned p,unsigned n) {return p!=0xDEAD&&n==0x50;}
    std::string objectName(unsigned p){return names.at(p).first;}
    std::string className(unsigned p){return names.at(p).second;}
};
int main() {
    using namespace bl2::voice::trace;
    assert(ValidName("Ak_Play_Orchid_VO_Chapter3_0030_echo_Scarlett"));
    assert(!ValidName("Ak_Play_Orchid_VO_"));
    assert(!ValidName("Ak_Play_Orchid_SFX_Anything"));
    assert(ValidName("Ak_Play_Iris_VO_Ep4_15_echo_TinyTina"));
    assert(ValidName("Ak_Play_Iris_VO_"+std::string(112,'a')));
    assert(!ValidName("Ak_Play_Iris_VO_"+std::string(113,'a')));
    for(const std::string bad:{"Ak_Play_Iris_VO_","Ak_Play_Iris_SFX_X","Ak_Play_Iris_VO_bad;quit",
        "Ak_Play_Iris_VO_bad\nname","Ak_Play_Iris_VO_\xFF"})assert(!ValidName(bad));
    const std::string sage="Ak_Play_Sage_VO_Ep1_01_echoX_Hammerlock";
    assert(ValidName(sage) && ValidName("Ak_Play_Sage_VO_x"));
    assert(ValidName("Ak_Play_Sage_VO_"+std::string(112,'a')));
    for(const std::string bad:{"Ak_Play_Sage_VO_","Ak_Play_Sage_SFX_X","Ak_Play_Sage_VO_bad;quit",
        "Ak_Play_Sage_VO_bad\nname","Ak_Play_Sage_VO_\xFF"})assert(!ValidName(bad));
    assert(!ValidName("Ak_Play_Sage_VO_"+std::string(113,'a')));
    const std::string name="Ak_Play_VO_Ep13_Pt3_18g_Live_Angel";
    assert(ValidName(name));
    for(const std::string bad:{std::string{},std::string("Ak_Play_VO_"),std::string("Ak_Play_VO_bad;quit"),
        std::string("Ak_Play_VO_bad\nname"),std::string("Ak_Play_VO_\xFF"),std::string(129,'A'),std::string("Ak_Play_Music_X")})assert(!ValidName(bad));
    Reader r;r.objects={0,0xDEAD,0x20000,0x21000};
    r.names={{0xDEAD,{name,"AkEvent"}},{0x20000,{name,"AkEvent"}},{0x21000,{name,"Object"}}};
    auto match=FindEvent(r,name);assert(match.event==0x20000&&match.count==1);
    assert(!FindEvent(r,"Ak_Play_VO_Missing").count&&!FindEvent(r,"quit").count);
    r.names[0x21000].second="AkEvent";match=FindEvent(r,name);assert(!match.event&&match.count==2);
    r.validArray=false;assert(!FindEvent(r,name).count);
    r.validArray=true;r.names[0x20000].first=sage;r.names[0x21000].second="Object";
    match=FindEvent(r,sage);assert(match.event==0x20000&&match.count==1);
    r.names[0x21000]={sage,"AkEvent"};match=FindEvent(r,sage);assert(!match.event&&match.count==2);
    static_assert(bl2::dlc::trace::Veneer(kHooks[0],0x40000000)==std::array<unsigned,8>{
        0xE5943018,0xE3A0B000,0xE51FF004,0x41FA36F0,0xE5943018,0xE3A0B000,0xE51FF004,0xF036FA41});
    static_assert(bl2::dlc::trace::Veneer(kHooks[1],0x40000000)[3]==0x4103C9D8);
    static_assert(bl2::dlc::trace::Veneer(kHooks[2],0x40000000)[3]==0x4103BDA8);
    static_assert(bl2::dlc::trace::Veneer(kHooks[3],0x40000000)==std::array<unsigned,8>{
        0xE5DD31D8,0xE1530006,0xE51FF004,0x41797628,0xE5DD31D8,0xE1530006,0xE51FF004,0x28767941});
    for(const auto& hook:kHooks) {
        assert(HookMatches(hook,hook.first,hook.second));
        assert(!HookMatches(hook,hook.first^1u,hook.second)&&!HookMatches(hook,hook.first,hook.second^1u));
    }
    WrapReader wr;auto snapshot=ReadWrap(wr,wr.stack);
    assert(snapshot.valid&&snapshot.bytes==3&&snapshot.units==3&&snapshot.lines==2&&
           snapshot.lineUnits[0]==1&&snapshot.lineUnits[1]==2&&snapshot.hash==0x1A47E90Bu&&snapshot.previewHex=="616263");
    auto rejected=[](auto change){WrapReader bad;change(bad);assert(!ReadWrap(bad,bad.stack).valid);};
    rejected([](auto& b){b.put(b.stack+0x84,0);});
    rejected([](auto& b){b.put(b.stack+0x84,4098);b.put(b.stack+0x88,4098);});
    rejected([](auto& b){b.put(b.stack+0x88,3);});
    rejected([](auto& b){b.put(b.stack+0x88,8193);});
    rejected([](auto& b){b.put(b.stack+0x80,0xDEAD);});
    rejected([](auto& b){b.bytes[b.source-b.base+1]=0;});
    rejected([](auto& b){b.bytes[b.source-b.base+3]=1;});
    rejected([](auto& b){b.put(b.stack+0x1D4,0xDEAD);});
    rejected([](auto& b){b.string(b.wide,std::u16string(1,char16_t(0xD800)));});
    rejected([](auto& b){b.string(b.wide,std::u16string(1,char16_t(0xDC00)));});
    rejected([](auto& b){b.string(b.wide,std::u16string(4097,u'x'));});
    rejected([](auto& b){b.put(b.stack+0x9C,17);b.put(b.stack+0xA0,17);});
    rejected([](auto& b){b.put(b.stack+0xA0,1);});
    rejected([](auto& b){b.put(b.stack+0x98,0xDEAD);});
    rejected([](auto& b){b.put(b.lines,0xDEAD);});
    rejected([](auto& b){b.put(b.lines+4,8195);});
    assert(!ReadWrap(wr,0xFFFFFEFF).valid&&!ReadWrap(wr,0xDEAD).valid);
    wr.string(wr.wide,u"\u041F\u0440\u0438\u0432\u0435\u0442 \u043C\u0438\u0440");
    const std::string utf8="\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80";
    std::memcpy(wr.bytes.data()+wr.source-wr.base,utf8.c_str(),utf8.size()+1);
    wr.put(wr.stack+0x84,unsigned(utf8.size()+1));wr.put(wr.stack+0x88,unsigned(utf8.size()+1));
    snapshot=ReadWrap(wr,wr.stack);assert(snapshot.valid&&snapshot.bytes==19&&snapshot.units==10);
    assert(Float(0x3F800000)==1.f&&Float(0)==0.f&&Float(0xC0000000)==-2.f);
    std::puts("PASS: 4 exact voice veneers/preimage negatives, bounded wrap byte/UTF-16/line/hash snapshot and malformed/unmapped/overflow/surrogate rejection, loaded AkEvent lookup, bit-exact float read");
}
