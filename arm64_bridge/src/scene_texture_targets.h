#pragma once

#include <cstddef>
#include <cstring>

struct SceneTextureTarget {
    const char* sha256;
    int width;
    int height;
};

// Exact CPU-upload payloads from the eight-package ASTC4 material inventory.
// Indices 0-4 are Tex_MetalDent01_Normal source mips 2-6; 5 is StubFlat_Nrm.
inline constexpr SceneTextureTarget kSceneTextureTargets[] = {
    {"088FF9DB74BB924ADA5ECFF563433DEF7C6FB7A8E91F752376ACD70CBC30BD7C", 64, 64},
    {"437977BC1E6FC921BBFF3FBA26A3986A058144936F113E91D81E12264EF1C6CC", 32, 32},
    {"B67F95C95F19EF368404BB2D1AA3F690F6870F4887E0861E41134B5510F34A7C", 16, 16},
    {"6252C8B9774FFB80EBA765DBA16A94BA024C69CFDCBC84E9F7F905CF32DDDB2D", 8, 8},
    {"A8EB053CB289C7E897BA8D8FF2DD60A3E30260AF93B8DD34828C18D6716EA4EB", 4, 4},
    {"442F4F8C2C46D39124A06DAF6C12258110AD9B668F78BD082C2275F343F784D5", 4, 4},
};

inline int SceneTextureTargetIndex(const char* sha256, int width, int height) {
    if (!sha256) return -1;
    for (std::size_t i = 0; i < sizeof(kSceneTextureTargets) / sizeof(kSceneTextureTargets[0]); ++i) {
        const auto& target = kSceneTextureTargets[i];
        if (width == target.width && height == target.height &&
            std::strcmp(sha256, target.sha256) == 0) return static_cast<int>(i);
    }
    return -1;
}
