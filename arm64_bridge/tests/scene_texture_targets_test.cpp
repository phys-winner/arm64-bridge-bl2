#include "scene_texture_targets.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    for (int i = 0; i < 6; ++i) {
        const auto& target = kSceneTextureTargets[i];
        assert(SceneTextureTargetIndex(target.sha256, target.width, target.height) == i);
        assert(SceneTextureTargetIndex(target.sha256, target.width + 1, target.height) == -1);
        assert(SceneTextureTargetIndex(target.sha256, target.width, target.height + 1) == -1);
        std::string changed = target.sha256;
        changed.back() = changed.back() == '0' ? '1' : '0';
        assert(SceneTextureTargetIndex(changed.c_str(), target.width, target.height) == -1);
        changed = target.sha256;
        changed.pop_back();
        assert(SceneTextureTargetIndex(changed.c_str(), target.width, target.height) == -1);
    }
    assert(SceneTextureTargetIndex(nullptr, 4, 4) == -1);
    assert(SceneTextureTargetIndex("", 4, 4) == -1);
    assert(SceneTextureTargetIndex(kSceneTextureTargets[4].sha256, 4, 4) !=
           SceneTextureTargetIndex(kSceneTextureTargets[5].sha256, 4, 4));
    std::puts("PASS exact-six-targets wrong-sha wrong-dimensions truncated-sha distinct-4x4");
}
