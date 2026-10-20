#pragma once
#include "bl2_dlc_material_trace.h"
#include <cstring>

namespace bl2::voice::trace {
constexpr std::array<bl2::dlc::trace::Hook,4> kHooks{{
    {0x01FA36E8,0xE5943018,0xE3A0B000,"bl2_qa_voice_duration"},
    {0x0103C9D0,0xED9C8A09,0xE7994003,"bl2_qa_voice_subtitle"},
    {0x0103BDA0,0xE2514000,0xE24DD01C,"bl2_qa_voice_end"},
    {0x01797620,0xE5DD31D8,0xE1530006,"bl2_qa_voice_wrap"},
}};
constexpr std::uint32_t kPlayRva=0x01ABFA50;
constexpr std::array<std::uint32_t,4> kPlayPrefix{0xE92D47F0,0xE2516000,0xE59F5138,0xE24DD028};
constexpr std::uint64_t kWindowMs=45000;
constexpr unsigned kRows=96;
constexpr unsigned kWrapRows=16;
inline bool HookMatches(const bl2::dlc::trace::Hook& hook,std::uint32_t first,std::uint32_t second) {
    return first==hook.first && second==hook.second;
}
inline float Float(std::uint32_t bits) { float value;std::memcpy(&value,&bits,4);return value; }
struct WrapSnapshot {
    bool valid=false;
    std::uint32_t bytes=0,units=0,lines=0,hash=2166136261u;
    std::array<std::uint32_t,16> lineUnits{};
    std::string previewHex;
};
template<class Reader> bool WideUnits(Reader& read,std::uint32_t p,std::uint32_t& units) {
    units=0;bool high=false;
    for(unsigned i=0;i<=4096;++i) {
        if(p>UINT32_MAX-i*2)return false;
        std::array<unsigned char,2> bytes{};
        if(!Reader::copyMemory(&read,p+i*2,bytes.data(),2))return false;
        const auto c=unsigned(bytes[0])|(unsigned(bytes[1])<<8);
        if(!c)return !high;
        if(i==4096 || (high && (c<0xDC00 || c>0xDFFF)) ||
           (!high && c>=0xDC00 && c<=0xDFFF))return false;
        high=!high && c>=0xD800 && c<=0xDBFF;++units;
    }
    return false;
}
template<class Reader> WrapSnapshot ReadWrap(Reader& read,std::uint32_t stack) {
    WrapSnapshot out;
    if(stack>UINT32_MAX-0x1DC || !read.span(stack,0x1DC))return out;
    const auto source=read.u32(stack+0x80),count=read.u32(stack+0x84),capacity=read.u32(stack+0x88);
    const auto lines=read.u32(stack+0x98),n=read.u32(stack+0x9C),cap=read.u32(stack+0xA0);
    out.bytes=count?count-1:0;out.lines=n;
    if(count<1 || count>4097 || count>capacity || capacity>8192 || n>16 || n>cap || cap>64 ||
       !read.span(lines,std::size_t(n)*20))return out;
    std::array<unsigned char,4097> bytes{};
    if(!Reader::copyMemory(&read,source,bytes.data(),count) || bytes[count-1])return out;
    constexpr char hex[]="0123456789abcdef";
    for(unsigned i=0;i+1<count;++i) {
        if(!bytes[i])return out;
        out.hash=(out.hash^bytes[i])*16777619u;
        if(i<32){out.previewHex+=hex[bytes[i]>>4];out.previewHex+=hex[bytes[i]&15];}
    }
    if(!WideUnits(read,read.u32(stack+0x1D4),out.units))return out;
    for(unsigned i=0;i<n;++i) {
        const auto p=read.u32(lines+i*20),size=read.u32(lines+i*20+4);
        if(size>8194 || (size && !WideUnits(read,p,out.lineUnits[i])))return out;
    }
    out.valid=true;return out;
}
inline bool ValidName(const std::string& name) {
    const unsigned prefix=name.rfind("Ak_Play_VO_",0)==0?11:
        name.rfind("Ak_Play_Orchid_VO_",0)==0?18:
        name.rfind("Ak_Play_Sage_VO_",0)==0?16:
        name.rfind("Ak_Play_Iris_VO_",0)==0?16:0;
    if(!prefix || name.size()<=prefix || name.size()>128)return false;
    for(unsigned char c:name)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))return false;
    return true;
}
struct Match {std::uint32_t event=0;unsigned count=0;};
template<class Reader> Match FindEvent(Reader& read,const std::string& name) {
    Match out;if(!ValidName(name))return out;
    std::uint32_t objects=0,count=0;
    if(!read.array(read.base+0x02FBA268,2000000,objects,count))return out;
    for(std::uint32_t i=0;i<count;++i) {
        const auto p=read.u32(objects+i*4);
        if(read.object(p) && read.span(p,0x50) && read.objectName(p)==name && read.className(p)=="AkEvent") {
            out.event=p;if(++out.count==2){out.event=0;return out;}
        }
    }
    return out;
}
} // namespace bl2::voice::trace
