#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

class DynarmicHost;
bool InstallBl2GraphicsSettings(DynarmicHost&);
// Owned game/input-thread boundary; no work after unchanged settings.
void SyncBl2GraphicsSettings(DynarmicHost&);
// Render-thread read: low bits are anisotropy, bit8 preserves authored filtering.
// Zero anisotropy keeps the bridge's original automatic driver maximum.
std::uint32_t Bl2GraphicsFiltering();
void Bl2GraphicsHostAnisotropy(std::uint32_t maximum);

namespace bl2::graphics {
// Shipped BL2 FSystemSettingsData, recovered from its LoadFromIni descriptor
// tables. Horn's similarly named data has a different layout.
using Data = std::array<std::uint32_t,0x4C8 / 4>;
inline constexpr std::array<std::uint32_t,10> hiddenOffsets{
    72,52,96,1064,1068,20,28,76,80,128};
inline constexpr std::uint32_t anisotropyOffset=1000;
inline constexpr std::array<std::uint32_t,5> anisotropyValues{1,2,4,8,16};
inline std::uint32_t AnisotropyIndex(std::uint32_t value) {
    for(std::uint32_t i=0;i<anisotropyValues.size();++i)
        if(value<=anisotropyValues[i])return i;
    return 4;
}
inline std::uint32_t MinificationFilter(std::uint32_t original,std::uint32_t control) {
    // GL_LINEAR_MIPMAP_NEAREST -> GL_LINEAR_MIPMAP_LINEAR is a bridge upgrade.
    // Potato skips that upgrade; data textures still need their authored
    // bilinear/point sampling, rather than a blanket nearest override.
    return !(control&0x100u) && original==0x2701u ? 0x2703u : original;
}
inline void Float(Data& data,std::uint32_t offset,float value) {
    std::memcpy(&data[offset/4],&value,4);
}
inline Data KeepRuntimeControls(Data quality,const Data& live) {
    // Surface callbacks and frame-policy edits can arrive after our baseline
    // capture. Neither Potato nor restore may put back an older surface.
    for(const auto offset:{108u,196u,996u,1016u,1020u,1024u,1028u,1032u,1036u,1040u})
        quality[offset/4]=live[offset/4];
    return quality;
}
inline Data MinimumDefaults(const Data& original) {
    auto minimum=original;
    // Exactly the eight rows populated by VideoOptions on EGBXPlatform_Android.
    minimum[192/4]=0;minimum[24/4]=0; // NumberOfDecals Off also disables DynamicDecals.
    Float(minimum,1008,.5f);
    minimum[1012/4]=2;minimum[156/4]=static_cast<std::uint32_t>(-40);
    minimum[68/4]=0;minimum[60/4]=0;minimum[64/4]=0;
    minimum[168/4]=0;minimum[4/4]=original[172/4]; // Original Low distance scale.
    return minimum;
}
inline Data Potato(const Data& original) {
    auto low=original;
    // Keep the authored lighting, cooked lightmap/GBuffer and translucency
    // paths. Disabling those paths is not a supported lower quality level.
    // Surface, screen percentage, pacing and user preferences stay intact.
    for(const auto offset : {0u,8u,12u,16u,20u,24u,28u,52u,56u,
        60u,64u,68u,72u,76u,80u,84u,96u,100u,128u,132u,136u,
        192u,204u,1064u,1068u,1100u,1104u,1108u,1112u,1116u,1196u})low[offset/4]=0;
    low[88/4]=1; // DropParticleDistortion: true is cheaper.
    low[1000/4]=1;
    low[1012/4]=2; // UI Low is engine TextureQuality=2.
    low[156/4]=static_cast<std::uint32_t>(-40); // Original GameDetail Low.
    low[168/4]=0;
    low[112/4]=std::max(original[112/4],2u);
    low[116/4]=std::max(original[116/4],3u);
    Float(low,4,.75f); Float(low,172,.75f); Float(low,1008,.5f);
    Float(low,32,.125f); Float(low,1188,.125f);
    Float(low,1200,0); Float(low,1204,0); Float(low,1208,0); Float(low,1212,.125f);
    // Only ordinary color/normal/specular groups (World through Skybox).
    // Lightmaps, render targets, terrain height AND weights, LUTs and other
    // data textures retain their layout. Keep the authored minimum and use
    // a bounded 128px cap rather than forcing every resource down to 32px.
    for(std::uint32_t group=0;group<16;++group) {
        const auto start=(212+group*28)/4;
        low[start+1]=std::max(original[start],std::min(original[start+1],7u));
        low[start+2]=static_cast<std::uint32_t>(std::max(static_cast<std::int32_t>(original[start+2]),1));
    }
    return low;
}
}
