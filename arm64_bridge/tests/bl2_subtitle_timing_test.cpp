#include "../src/bl2_subtitle_timing.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
unsigned checks=0;
void Check(bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}++checks;}
std::uint32_t Bits(float f){std::uint32_t n;std::memcpy(&n,&f,4);return n;}
struct Reader {
    static constexpr unsigned base=0x300000,pair=base+0x100,stack=base+0x200,cues=base+0x400,source=base+0x1000;
    std::vector<unsigned char> bytes=std::vector<unsigned char>(0x4000);
    bool span(unsigned p,std::size_t n)const{return p>=base&&p-base<=bytes.size()&&n<=bytes.size()-(p-base);}
    static bool copyMemory(const void* context,unsigned p,void* out,std::size_t n){
        const auto& r=*static_cast<const Reader*>(context);
        if(!r.span(p,n))return false;std::memcpy(out,r.bytes.data()+p-base,n);return true;
    }
    unsigned u32(unsigned p)const{
        Check(span(p,4),"bounded u32 read");const auto* b=bytes.data()+p-base;
        return unsigned(b[0])|(unsigned(b[1])<<8)|(unsigned(b[2])<<16)|(unsigned(b[3])<<24);
    }
    void put(unsigned p,unsigned n){Check(span(p,4),"bounded fixture write");for(unsigned i=0;i<4;++i)bytes[p-base+i]=static_cast<unsigned char>(n>>(i*8));}
    explicit Reader(const std::string& text){
        Check(text.size()+1<=bytes.size()-(source-base),"fixture source bounds");
        std::memcpy(bytes.data()+source-base,text.c_str(),text.size()+1);
        put(pair+0x1C,cues);put(pair+0x20,2);put(pair+0x24,2);
        put(cues,source);put(cues+4,unsigned(text.size()+1));put(cues+8,unsigned(text.size()+1));
        put(cues+12,Bits(101.215622f));put(cues+28,Bits(109.098122f));
        put(stack+0x80,source);put(stack+0x84,unsigned(text.size()+1));put(stack+0x88,unsigned(text.size()+1));
    }
};
unsigned Decide(Reader& r,unsigned original,unsigned duration=Bits(7.8825f)){
    const auto before=r.bytes;
    const auto result=bl2::subtitle::Denominator(r,r.pair,r.stack,original,duration);
    Check(r.bytes==before,"decision never writes guest memory or authored absolute starts");return result;
}
void Reject(const char* label,auto modify){
    Reader r("\xD0\xAF");modify(r);
    Check(Decide(r,Bits(2.f))==Bits(2.f),label);
}
void Case(const char* label,const std::string& text,unsigned units,bool changed){
    Reader r(text);const auto original=Bits(float(text.size()));const auto actual=Decide(r,original);
    Check(actual==(changed?Bits(float(units)):original),label);
    std::printf("DECISION %s %zu %u %u %u %u\n",label,text.size(),units,unsigned(changed),original,actual);
}
} // namespace
int main(){
    using namespace bl2::subtitle;
    static_assert(Replay(0x40000000)==std::array<unsigned,8>{
        0xE3A035FE,0xE58D00A8,0xE51FF004,0x417975B0,0xE3A035FE,0xE58D00A8,0xE51FF004,0xB0757941});
    Check(Matches(0xE3A035FE,0xE58D00A8)&&!Matches(0xE3A035FF,0xE58D00A8)&&!Matches(0xE3A035FE,0xE58D00A9),"exact preimage guards");
    for(unsigned n=0;n<=4096;++n)Check(IntegerFloatBits(n)==Bits(float(n)),"integer-only exact float32 count bits");
    Case("ASCII","Hello world",11,false);
    Case("CYRILLIC","\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80",10,true);
    Case("PUNCTUATION","A \xE2\x80\x94 \xE2\x80\xA6 \xC2\xA0",7,true);
    Case("NONBMP","\xF0\x9F\x98\x80",2,false);
    Case("INVALID_UTF8","\xD0",1,false);
    std::string mixed;for(unsigned i=0;i<71;++i)mixed+="\xD0\xAF";mixed+=std::string(30,'x');
    Case("LONG172",mixed,101,true);
    for(const std::string invalid:{std::string("\xC0\xAF"),std::string("\xD0"),std::string("\x80"),
        std::string("\xED\xA0\x80"),std::string("\xE0\x80\x80"),std::string("\xE2\x80"),
        std::string("\xF5\x80\x80\x80"),std::string("\xD0\x7F"),std::string("A\0B",3)}){
        Reader r(invalid);const auto original=Bits(float(invalid.size()));
        Check(Decide(r,original)==original,"invalid UTF-8 preserves original denominator");
    }
    Reject("authored multi-cue sequence",[](auto& r){r.put(r.pair+0x20,3);r.put(r.pair+0x24,3);});
    Reader authored("\xD0\xAF");authored.put(authored.pair+0x20,3);authored.put(authored.pair+0x24,3);
    const auto authoredResult=Decide(authored,Bits(2.f));
    std::printf("DECISION MULTICUE 2 1 0 %u %u\n",Bits(2.f),authoredResult);
    Reject("wrong cue count",[](auto& r){r.put(r.pair+0x20,1);});
    Reject("wrong cue capacity",[](auto& r){r.put(r.pair+0x24,1);});
    Reject("unbounded cue capacity",[](auto& r){r.put(r.pair+0x24,513);});
    Reject("unmapped cue array",[](auto& r){r.put(r.pair+0x1C,0xDEAD);});
    Reject("empty first cue",[](auto& r){r.put(r.cues+4,1);});
    Reject("nonempty authored end cue",[](auto& r){r.put(r.cues+20,3);});
    Reject("end cue terminator",[](auto& r){r.put(r.cues+16,r.source);r.put(r.cues+20,1);});
    Reject("unmapped source",[](auto& r){r.put(r.stack+0x80,0xDEAD);});
    Reject("empty source",[](auto& r){r.put(r.stack+0x84,1);});
    Reject("source count overflow",[](auto& r){r.put(r.stack+0x84,0xFFFFFFFF);});
    Reject("source byte budget",[](auto& r){r.put(r.stack+0x84,4098);r.put(r.stack+0x88,4098);});
    Reject("source capacity",[](auto& r){r.put(r.stack+0x88,2);});
    Reject("source capacity budget",[](auto& r){r.put(r.stack+0x88,8193);});
    Reject("missing source terminator",[](auto& r){r.bytes[r.source-r.base+2]=1;});
    Reader r("\xD0\xAF");
    for(unsigned duration:{0u,0x80000000u,0xBF800000u,0x7F800000u,0xFF800000u,0x7FC00000u})
        Check(Decide(r,Bits(2.f),duration)==Bits(2.f),"invalid time window unchanged");
    Check(Decide(r,Bits(3.f))==Bits(3.f),"unexpected current S18 unchanged");
    Check(Denominator(r,0xFFFFFFFE,r.stack,Bits(2.f),Bits(7.f))==Bits(2.f)&&
          Denominator(r,r.pair,0xFFFFFFFE,Bits(2.f),Bits(7.f))==Bits(2.f)&&
          Denominator(r,0xDEAD,r.stack,Bits(2.f),Bits(7.f))==Bits(2.f),"frame bounds/overflow unchanged");
    Reader end("\xD0\xAF");end.put(end.cues+16,end.source+10);end.put(end.cues+20,2);
    Check(Decide(end,Bits(2.f))==Bits(1.f),"wide empty end cue accepted");
    std::string maximum;for(unsigned i=0;i<2048;++i)maximum+="\xD0\xAF";
    Reader max(maximum);Check(Decide(max,Bits(4096.f))==Bits(2048.f),"exact maximum source bound accepted");
    std::printf("PASS: %u integer denominator, frame, authored-cue, UTF-8 and preimage checks\n",checks);
}
