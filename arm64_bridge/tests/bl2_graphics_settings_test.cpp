#include "bl2_graphics_settings.h"
#include <cassert>
#include <cstdio>

int main() {
    bl2::graphics::Data original{};
    for(std::size_t i=0;i<original.size();++i)original[i]=static_cast<std::uint32_t>(i*17+3);
    bl2::graphics::Float(original,4,1);bl2::graphics::Float(original,172,.5f);
    original[112/4]=0;original[116/4]=0;original[1000/4]=8;
    for(std::uint32_t group=0;group<28;++group) {
        const auto index=(212+group*28)/4;
        original[index]=5;original[index+1]=10;original[index+2]=0;
    }
    original[(212+3*28)/4]=8; // An authored floor above the 128px target.
    original[(212+4*28)/4+1]=6; // Already smaller than the target.
    const auto minimum=bl2::graphics::MinimumDefaults(original);
    for(std::size_t i=0;i<original.size();++i) {
        const auto offset=i*4;
        if(offset==192 || offset==24 || offset==1008 || offset==1012 || offset==156 ||
            offset==68 || offset==60 || offset==64 || offset==168 || offset==4)continue;
        assert(minimum[i]==original[i]);
    }
    assert(minimum[192/4]==0 && minimum[24/4]==0 && minimum[4/4]==original[172/4]);
    assert(minimum[1012/4]==2 && static_cast<std::int32_t>(minimum[156/4])==-40);
    const auto low=bl2::graphics::Potato(original);
    for(const auto offset:{36u,40u,44u,48u,144u,148u,160u})
        assert(low[offset/4]==original[offset/4]);
    // Resolution/surface/window, frame-rate policy, VSync and streaming policy
    // are unchanged even when their values are distinct non-default sentinels.
    for(const auto offset:{196u,996u,1016u,1020u,1024u,1028u,1032u,1036u,1040u,108u})
        assert(low[offset/4]==original[offset/4]);
    for(std::uint32_t group=16;group<28;++group)for(std::uint32_t word=0;word<7;++word)
        assert(low[(212+group*28)/4+word]==original[(212+group*28)/4+word]);
    for(std::uint32_t group=0;group<16;++group) {
        const auto index=(212+group*28)/4;
        assert(low[index]==original[index] && low[index+1]>=low[index]);
        assert(low[index+1]<=original[index+1] && low[index+2]==1);
        for(std::uint32_t word=3;word<7;++word)assert(low[index+word]==original[index+word]);
    }
    assert(low[216/4]==7 && low[(212+3*28)/4+1]==8 && low[(212+4*28)/4+1]==6);
    float distance,foliage;
    std::memcpy(&distance,&low[4/4],4);std::memcpy(&foliage,&low[1008/4],4);
    assert(distance==.75f && foliage==.5f && low[4/4]==low[172/4]);
    for(const auto filter:{0u,0x2600u,0x2601u,0x2700u,0x2701u,0x2702u,0x2703u,UINT32_MAX}) {
        assert(bl2::graphics::MinificationFilter(filter,0x101u)==filter);
        assert(bl2::graphics::MinificationFilter(filter,0)==(filter==0x2701u?0x2703u:filter));
    }
    assert(low[1000/4]==1 && low[1012/4]==2 && low[168/4]==0);
    assert(low[88/4]==1 && low[1064/4]==0 && low[1068/4]==0);
    for(std::size_t i=0;i<bl2::graphics::anisotropyValues.size();++i)
        assert(bl2::graphics::AnisotropyIndex(bl2::graphics::anisotropyValues[i])==i);
    // Selecting another original option while Potato is active modifies the
    // saved baseline; projection stays low and disabling uses that new value.
    auto edited=original;edited[72/4]=1;edited[1000/4]=16;edited[168/4]=3;
    auto editedLow=bl2::graphics::Potato(edited);
    assert(editedLow[72/4]==0 && editedLow[1000/4]==1 && editedLow[168/4]==0);
    assert(edited[72/4]==1 && edited[1000/4]==16 && edited[168/4]==3);
    auto resized=low;resized[1028/4]=2400;resized[1032/4]=1080;
    bl2::graphics::Float(resized,1020,150);resized[196/4]=6;resized[1016/4]=1;
    const auto retained=bl2::graphics::KeepRuntimeControls(edited,resized);
    assert(retained[1028/4]==2400 && retained[1032/4]==1080 && retained[1020/4]==resized[1020/4]);
    assert(retained[196/4]==6 && retained[1016/4]==1 && retained[72/4]==edited[72/4]);
    const auto retainedLow=bl2::graphics::Potato(retained);
    assert(retainedLow[1028/4]==2400 && retainedLow[1020/4]==resized[1020/4]);
    assert(bl2::graphics::Potato(low)==low);
    std::puts("PASS: BL2 graphics projection, visible near objects, authored data-texture layout/filtering, bounded mips, surface/pacing and baseline restore");
}
