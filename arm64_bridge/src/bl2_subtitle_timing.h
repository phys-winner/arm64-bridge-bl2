#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace bl2::subtitle {
constexpr std::uint32_t kRva=0x017975A8;
constexpr std::uint32_t kFunctionRva=0x01796B3C;
constexpr std::array<std::uint32_t,2> kPreimage{0xE3A035FE,0xE58D00A8};
constexpr bool Matches(std::uint32_t first,std::uint32_t second) {
    return first==kPreimage[0] && second==kPreimage[1];
}
constexpr std::uint32_t Swap(std::uint32_t n) {
    return (n>>24)|((n>>8)&0xFF00)|((n<<8)&0xFF0000)|(n<<24);
}
constexpr std::array<std::uint32_t,8> Replay(std::uint32_t base) {
    return {kPreimage[0],kPreimage[1],0xE51FF004,base+kRva+8,
            kPreimage[0],kPreimage[1],0xE51FF004,Swap(base+kRva+8)};
}
// Every supported count is <=4096, hence exactly representable. No host FP
// operation or host rounding environment participates in the guest division.
constexpr std::uint32_t IntegerFloatBits(std::uint32_t n) {
    if(!n)return 0;
    unsigned power=0;for(auto value=n;value>1;value>>=1)++power;
    return ((power+127u)<<23)|((n-(1u<<power))<<(23-power));
}
inline bool BmpUnits(const unsigned char* bytes,std::size_t size,std::uint32_t& units,bool& nonAscii) {
    units=0;nonAscii=false;
    for(std::size_t i=0;i<size;++i) {
        const auto c=bytes[i];
        if(!c)return false;
        if(c<128){++units;continue;}
        nonAscii=true;
        if(c>=0xC2 && c<=0xDF) {
            if(i+1>=size || bytes[i+1]<0x80 || bytes[i+1]>0xBF)return false;
            ++i;
        } else if(c>=0xE0 && c<=0xEF) {
            if(i+2>=size || bytes[i+1]<0x80 || bytes[i+1]>0xBF ||
               bytes[i+2]<0x80 || bytes[i+2]>0xBF ||
               (c==0xE0 && bytes[i+1]<0xA0) || (c==0xED && bytes[i+1]>=0xA0))return false;
            i+=2;
        } else return false; // shortcut: preserve old native behavior for unverified non-BMP input.
        ++units;
    }
    return true;
}

// Return the original register bits on every rejected/unsupported frame. The
// original VDIV follows unchanged and retains its own FPSCR/VFP semantics.
template<class Reader> std::uint32_t Denominator(Reader& read,std::uint32_t pair,
        std::uint32_t stack,std::uint32_t original,std::uint32_t duration) {
    if(pair>UINT32_MAX-0x28 || stack>UINT32_MAX-0x8C || !read.span(pair,0x28) ||
       !read.span(stack,0x8C) || !duration || (duration&0x80000000u) ||
       (duration&0x7F800000u)==0x7F800000u)return original;
    const auto cues=read.u32(pair+0x1C),n=read.u32(pair+0x20),cap=read.u32(pair+0x24);
    if(n!=2 || cap<n || cap>512 || !read.span(cues,32) || read.u32(cues+4)<2)return original;
    // The final native cue must be empty. Authored multi-cue sequences are not
    // redistributed; the first cue's absolute start and the end remain intact.
    const auto endBytes=read.u32(cues+20);
    if(endBytes>2)return original;
    if(endBytes) {
        std::array<unsigned char,2> end{};
        if(!Reader::copyMemory(&read,read.u32(cues+16),end.data(),endBytes) || end[0] || end[1])return original;
    }
    const auto source=read.u32(stack+0x80),count=read.u32(stack+0x84),capacity=read.u32(stack+0x88);
    if(count<2 || count>4097 || count>capacity || capacity>8192 ||
       original!=IntegerFloatBits(count-1))return original;
    std::array<unsigned char,4097> bytes{};
    if(!Reader::copyMemory(&read,source,bytes.data(),count) || bytes[count-1])return original;
    std::uint32_t units=0;bool nonAscii=false;
    if(!BmpUnits(bytes.data(),count-1,units,nonAscii) || !nonAscii || !units)return original;
    return IntegerFloatBits(units);
}
} // namespace bl2::subtitle
