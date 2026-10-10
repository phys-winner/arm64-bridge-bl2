#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>
#include <jni.h>

class DynarmicHost;

// Verify the APK's mapped input before ELF loading, then patch only loaded
// guest memory before any renderer entry can execute. No persisted ELF edits.
bool VerifyBl2NativeResolutionInput(const uint8_t* data, size_t size);
bool InstallBl2NativeResolutionPatch(DynarmicHost& host);

// SurfaceHolder dimensions are the coordinate space used by the Java UI.
// Recover an invalid pair from this Surface's native window; otherwise reject
// the callback and leave the last valid renderer state intact.
bool PrepareBl2NativeResolutionSurface(JNIEnv* env, jobject surface,
                                       jint& width, jint& height);
void LogBl2NativeResolutionState(DynarmicHost& host, const char* event);

// Call after a successful eglMakeCurrent with a non-null current context,
// before returning to guest code. A failing real GL limit stops that guest
// thread before it can allocate unsupported scene targets.
void CheckBl2NativeResolutionLimits(DynarmicHost& host);

// Bounded provenance (first and 60th presentation after each surface callback).
// Call before eglSwapBuffers, while the final viewport is still current.
// This proves presentation dimensions, not the dimensions of an earlier FBO.
void AuditBl2NativeResolutionPresent(DynarmicHost& host);

namespace bl2::ui {
struct HudElementLayout {
    float x=0,y=0,scaleX=100,scaleY=100;
    bool operator==(const HudElementLayout&) const = default;
};
inline bool ValidHudElementLayout(const HudElementLayout& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::abs(value.x)<=4096 &&
        std::abs(value.y)<=4096 && std::isfinite(value.scaleX) && std::isfinite(value.scaleY) &&
        value.scaleX>=25 && value.scaleX<=400 && value.scaleY>=25 && value.scaleY<=400;
}
// GetObject creates a fresh UObject wrapper on each call. The native GFx
// interface/display-object pair identifies the actual authored child instead.
struct HudElementBaseline {
    std::uint32_t interface=0,displayObject=0;
    std::uint64_t entry=0;
    HudElementLayout original;
    bool CaptureOrRestore(std::uint32_t nativeInterface,std::uint32_t nativeObject,
                          std::uint64_t entryIdentity,HudElementLayout& value) {
        if(!nativeInterface || !nativeObject || !entryIdentity || !ValidHudElementLayout(value))return false;
        if(interface!=nativeInterface || displayObject!=nativeObject || entry!=entryIdentity) {
            interface=nativeInterface;displayObject=nativeObject;entry=entryIdentity;original=value;
        } else value=original;
        return true;
    }
};
// UI_HUD.HUD's mapmask bottom is 49.7 authored pixels. ScaleY is a percentage;
// the tracker keeps eight of its own scaled pixels below the enlarged map.
inline float HudMissionY(float mapY,float mapScale,float trackerY,float trackerScale,float uiScale) {
    if(uiScale==1 || !std::isfinite(uiScale) || uiScale<1 || uiScale>2 ||
        !std::isfinite(mapY) || !std::isfinite(mapScale) || !std::isfinite(trackerY) ||
        !std::isfinite(trackerScale) || std::abs(mapY)>4096 || std::abs(trackerY)>4096 ||
        mapScale<25 || mapScale>400 || trackerScale<25 || trackerScale>400)return trackerY;
    const float target=std::max(trackerY,mapY+mapScale*.497f+trackerScale*.08f);
    return std::isfinite(target) && target<720 ? target : trackerY;
}
struct HudRect {
    float left=0,right=0,top=0,bottom=0;
};
inline HudRect HudDisplayRect(const HudElementLayout& value,const HudRect& authored) {
    return {value.x+authored.left*value.scaleX*.01f,value.x+authored.right*value.scaleX*.01f,
        value.y+authored.top*value.scaleY*.01f,value.y+authored.bottom*value.scaleY*.01f};
}
// UI_HUD.HUD's authored first-frame envelopes include the XP bar and its
// persistent level/class text. Move that group only when its enlarged native
// display rectangles overlap the health/shield corner; keep 100% pristine.
inline float HudExperienceY(const HudElementLayout& xp,const HudElementLayout* health,
                            const HudElementLayout* shield,float requested) {
    if(!ValidHudElementLayout(xp) || !std::isfinite(requested) || requested<=1 || requested>2)return xp.y;
    constexpr HudRect xpGroup{-200.5f,200.5f,-6.9f,31.35f};
    constexpr HudRect healthBounds{-51.2f,231.6f,-21.15f,27.05f};
    constexpr HudRect shieldBounds{-51.85f,171.4f,-15.15f,21.95f};
    float target=xp.y;
    for(int pass=0;pass<2;++pass)for(int i=0;i<2;++i) {
        const auto* corner=i==0 ? health : shield;
        if(!corner || !ValidHudElementLayout(*corner))continue;
        auto moved=xp;moved.y=target;
        const auto group=HudDisplayRect(moved,xpGroup),bar=HudDisplayRect(*corner,i==0 ? healthBounds : shieldBounds);
        if(group.left<bar.right && group.right>bar.left && group.top<bar.bottom && group.bottom>bar.top)
            target=std::min(target,bar.top-(xpGroup.bottom+8)*xp.scaleY*.01f);
    }
    return std::isfinite(target) && target>=xpGroup.top*-xp.scaleY*.01f ? target : xp.y;
}
// ApplyElementScaling consumes the original soft-float getter result. Keep
// that native player scalar at 100%; the interface setting is a multiplier.
inline float HudElementScale(float original,float requested) {
    if(requested==1 || !std::isfinite(original) || original<=0 ||
        !std::isfinite(requested) || requested<1 || requested>2)return original;
    const float result=original*requested;
    return std::isfinite(result) && result<=4 ? result : original;
}
struct HudLayoutFrame {
    std::uintptr_t core=0;
    std::uint64_t owner=0,definition=0,generation=0;
    std::uint32_t movie=0,entries=0,count=0,sp=0;
    bool operator==(const HudLayoutFrame&) const = default;
};
// One original Apply invocation owns the layout samples. The two native
// callbacks must refer to the same child before its SetDisplayInfo executes.
struct HudLayoutPass {
    HudLayoutFrame frame;
    float requested=0;
    std::uint64_t child=0,entry=0;
    std::uint32_t index=0,completed=0;
    bool Begin(const HudLayoutFrame& value,float scale) {
        if(frame.core || !value.core || !value.owner || !value.definition || !value.generation ||
            !value.movie || !value.entries || !value.count || value.count>128 ||
            !value.sp || (value.sp&7u) || !std::isfinite(scale) || scale<1 || scale>2)return false;
        frame=value;requested=scale;return true;
    }
    bool Matches(const HudLayoutFrame& value) const {return frame.core && frame==value;}
    bool Capture(const HudLayoutFrame& value,std::uint32_t currentIndex,
                 std::uint64_t currentChild,std::uint64_t currentEntry) {
        if(!Matches(value) || child || currentIndex>=frame.count || !currentChild || !currentEntry)return false;
        child=currentChild;entry=currentEntry;index=currentIndex;return true;
    }
    bool CompleteElement(const HudLayoutFrame& value,std::uint32_t currentIndex,
                         std::uint64_t currentChild,std::uint64_t currentEntry) {
        if(!Matches(value) || !child || currentIndex!=index || currentChild!=child || currentEntry!=entry)return false;
        child=entry=0;++completed;return true;
    }
    bool Finish(const HudLayoutFrame& value,std::uint32_t nextIndex,float& applied) {
        // An own epilogue clears even an invalidated lifetime; a foreign or
        // reentrant epilogue cannot discard the outer invocation's samples.
        if(!frame.core || value.core!=frame.core || value.sp!=frame.sp ||
            std::uint32_t(value.owner>>32)!=std::uint32_t(frame.owner>>32))return false;
        const bool complete=Matches(value) && !child && completed && nextIndex==frame.count;
        if(complete)applied=requested;
        *this={};return complete;
    }
};
struct HudA32Registers {
    std::array<std::uint32_t,16> regs{};
    std::array<std::uint32_t,64> ext{};
    std::uint32_t cpsr=0,fpscr=0;
    bool operator==(const HudA32Registers&) const = default;
};
// A pending same-core call is thread-local in the bridge. Refuse a second call
// or a foreign return instead of replacing its original Tick continuation.
struct HudReapplyCall {
    std::uintptr_t core=0;
    std::uint64_t owner=0,generation=0;
    std::uint32_t continuation=0;
    HudA32Registers saved;
    bool Begin(std::uintptr_t currentCore,std::uint32_t resume,const HudA32Registers& value,
               std::uint64_t currentOwner,std::uint64_t currentGeneration) {
        if(core || !currentCore || !resume || !currentOwner || !currentGeneration ||
            (value.cpsr&0x20u) || (value.regs[13]&7u))return false;
        core=currentCore;continuation=resume;saved=value;owner=currentOwner;generation=currentGeneration;return true;
    }
    bool Finish(std::uintptr_t currentCore,std::uint32_t sp,std::uint64_t currentOwner,
                std::uint64_t currentGeneration,HudA32Registers& value) {
        if(!core || core!=currentCore || sp!=saved.regs[13] || owner!=currentOwner || generation!=currentGeneration)return false;
        value=saved;value.regs[15]=continuation;core=0;continuation=0;owner=0;generation=0;return true;
    }
};
} // namespace bl2::ui
