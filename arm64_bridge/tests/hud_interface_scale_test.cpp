#include "../src/bl2_native_resolution.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace bl2::ui;
static void require(bool value,const char* message) {
    if(!value) {std::cerr<<message<<'\n';std::exit(1);}
}
int main() {
    require(HudElementScale(1.125f,1)==1.125f && HudElementScale(1.125f,2)==2.25f &&
        HudElementScale(1.125f,1.25f)==1.40625f,
        "The Apply-only multiplier must preserve the native player scalar, including at 100%");
    require(HudElementScale(1,0)==1 && HudElementScale(1,std::numeric_limits<float>::infinity())==1,
        "Invalid interface multipliers must retain the native scalar");
    const HudElementLayout health{190,630,200,200},shield{190,585,200,200},xp{640,625,200,200};
    const auto healthCopy=health,shieldCopy=shield,xpCopy=xp;
    const float xpY=HudExperienceY(xp,&health,&shield,2);
    require(xpY<xp.y && xpY+31.35f*2+8*2<=shield.y-15.15f*2 &&
        health==healthCopy && shield==shieldCopy && xp==xpCopy,
        "An overlapping XP/level group must clear both native corner bars without resizing or moving them");
    auto xpFar=xp;xpFar.x=1500;
    require(HudExperienceY(xp,&health,&shield,1)==xp.y && HudExperienceY(xpFar,&health,&shield,2)==xpFar.y &&
        HudExperienceY(xp,nullptr,nullptr,2)==xp.y && HudExperienceY(xp,&health,&shield,0)==xp.y,
        "100%, horizontally separate groups, unavailable corner clips and invalid requests must retain native XP geometry");
    auto highCorner=health;highCorner.y=10;
    auto highXp=xp;highXp.y=30;highXp.x=health.x;
    require(HudExperienceY(highXp,&highCorner,nullptr,2)==highXp.y,
        "Clearance must never push a group above the authored stage");
    HudElementBaseline xpBaseline,levelBaseline;
    const HudElementLayout originalXp{640,625,100,100},originalLevel{530,646.05f,100,100};
    auto xpInput=originalXp,levelInput=originalLevel;
    for(float requested:{2.f,1.f,1.25f,2.f,1.f}) {
        require(xpBaseline.CaptureOrRestore(0x10000,0x30000,4,xpInput) &&
            levelBaseline.CaptureOrRestore(0x10000,0x31000,5,levelInput),
            "Persistent XP and level clips must restore their separate authored inputs");
        xpInput.scaleX*=requested;xpInput.scaleY*=requested;
        levelInput.scaleX*=requested;levelInput.scaleY*=requested;
        const float delta=HudExperienceY(xpInput,&health,&shield,requested)-xpInput.y;
        xpInput.y+=delta;levelInput.y+=delta;
        if(requested==1)require(xpInput==originalXp && levelInput==originalLevel,
            "100% must remove both the XP clearance and matching persistent level-text offset");
    }
    // Use the real 418 frame's owner/entries/SP and child values. Tokens,
    // native instance and core IDs stand in for the separately guarded UE3
    // metadata; they are independently varied to exercise each admission key.
    const HudLayoutFrame frame{1,0x56777A4000000012ull,0x5100000000000013ull,3,
        0x62000000,0x5FDF8640,39,0x4477F6A8};
    constexpr std::uint64_t child=0x671A78A000000014ull,entry=0x12345678;
    HudLayoutPass pass;
    require(pass.Begin(frame,2) && !pass.Begin(frame,1),
        "A second Apply invocation must not overwrite its outer layout scope");
    require(pass.Capture(frame,0,child,entry) && !pass.Capture(frame,1,child,entry),
        "A captured GetDisplayInfo child must finish before another child can be sampled");
    auto foreignFrame=frame;foreignFrame.core=2;float applied=0;
    require(!pass.CompleteElement(foreignFrame,0,child,entry) && !pass.Finish(foreignFrame,39,applied) &&
        pass.frame==frame && applied==0,
        "A foreign core's element or epilogue must leave the original invocation intact");
    for(int field=0;field<6;++field) {
        auto changed=frame;
        switch(field) {
            case 0:changed.owner^=1;break;
            case 1:changed.definition^=1;break;
            case 2:++changed.generation;break;
            case 3:changed.movie+=4;break;
            case 4:changed.entries+=16;break;
            default:changed.sp+=8;break;
        }
        require(!pass.Matches(changed) && !pass.CompleteElement(changed,0,child,entry),
            "Changed owner/definition/lifetime/native movie/array/stack must reject a final element");
    }
    require(!pass.CompleteElement(frame,1,child,entry) && !pass.CompleteElement(frame,0,child+1,entry) &&
        !pass.CompleteElement(frame,0,child,entry+1) && pass.CompleteElement(frame,0,child,entry),
        "SetDisplayInfo must match the exact input index, registered child and authored entry");
    require(pass.Finish(frame,39,applied) && applied==2 && !pass.frame.core && !pass.Finish(frame,39,applied),
        "Only a completed original loop may ACK the requested setting exactly once");
    for(float requested:{1.f,1.25f,2.f,1.f}) {
        applied=0;
        require(pass.Begin(frame,requested) && pass.Capture(frame,0,child,entry) &&
            pass.CompleteElement(frame,0,child,entry) && pass.Finish(frame,39,applied) && applied==requested,
            "Native effective scalar and Tick's acknowledged interface percentage must remain separate");
    }
    applied=0;
    require(pass.Begin(frame,2) && !pass.Finish(frame,39,applied) && !pass.frame.core && applied==0,
        "A root-null/empty-input early epilogue must clear its own scope without ACK");
    require(pass.Begin(frame,2) && pass.Capture(frame,0,child,entry) &&
        !pass.Finish(frame,39,applied) && !pass.frame.core && applied==0,
        "An unfinished child must never ACK the applied setting");
    require(pass.Begin(frame,2) && pass.Capture(frame,0,child,entry) &&
        pass.CompleteElement(frame,0,child,entry) && !pass.Finish(frame,1,applied) && !pass.frame.core,
        "A partial native loop must not ACK the whole HUD");
    auto changedLifetime=frame;++changedLifetime.generation;
    require(pass.Begin(frame,2) && pass.Capture(frame,0,child,entry) &&
        pass.CompleteElement(frame,0,child,entry) && !pass.Finish(changedLifetime,39,applied) && !pass.frame.core,
        "An own epilogue must retire an invalidated pass without ACKing a stale movie lifetime");
    auto invalidFrame=frame;invalidFrame.sp+=4;
    require(!pass.Begin(invalidFrame,2),"Only an aligned original Apply stack may own a layout pass");
    invalidFrame=frame;invalidFrame.count=129;
    require(!pass.Begin(invalidFrame,2),"The authored entry bound must apply to layout scopes as well as guest reads");
    HudElementBaseline map,mission;
    const HudElementLayout authoredMap{1230,20,100,100},authoredMission{1150,78,100,100};
    HudElementLayout mapInput=authoredMap,missionInput=authoredMission;
    for(int cycle=0;cycle<10;++cycle)for(float requested:{1.25f,2.f,1.f,1.25f}) {
        require(map.CaptureOrRestore(0x10000,0x20000,1,mapInput) &&
            mission.CaptureOrRestore(0x10000,0x21000,2,missionInput),"Valid live GFx children must retain authored geometry");
        require(mapInput==authoredMap && missionInput==authoredMission,
            "A preceding scaled frame must never become the next ApplyElementScaling input");
        // The original HUD scales positions about their distinct authored
        // anchors and uses percent ScaleY. Feed its resulting display geometry
        // through the production tracker rule before the next original pass.
        mapInput.y=10+(mapInput.y-10)*requested;mapInput.scaleY*=requested;mapInput.scaleX*=requested;
        missionInput.y=10+(missionInput.y-10)*requested;missionInput.scaleY*=requested;missionInput.scaleX*=requested;
        missionInput.y=HudMissionY(mapInput.y,mapInput.scaleY,missionInput.y,missionInput.scaleY,requested);
        if(requested==1)require(mapInput==authoredMap && missionInput==authoredMission,
            "100% must restore the authored map/tracker after a 200% pass, including the tracker offset");
        else require(missionInput.y>=mapInput.y+.497f*mapInput.scaleY+.08f*missionInput.scaleY,
            "125% and 200% must leave scaled tracker spacing below the enlarged map");
    }
    HudElementLayout reloaded{100,30,90,110};
    require(map.CaptureOrRestore(0x10000,0x22000,1,reloaded) && map.original==reloaded,
        "A new native display object must capture its own layout instead of an earlier wrapper's baseline");
    reloaded.y=90;
    require(map.CaptureOrRestore(0x10000,0x22000,3,reloaded) && map.original.y==90,
        "Changed definition/entry identity must invalidate an earlier clip baseline");
    const auto before=map.original;
    reloaded.scaleY=std::numeric_limits<float>::quiet_NaN();
    require(!map.CaptureOrRestore(0x10000,0x22000,3,reloaded) && map.original==before,
        "Invalid display inputs must neither poison nor apply a previous clip layout");
    require(HudMissionY(20,200,250,200,2)==250 && HudMissionY(20,200,78,200,1)==78 &&
        HudMissionY(700,200,78,200,2)==78,
        "Clear layouts, 100% and an out-of-stage result must retain the original tracker position");
    HudA32Registers initial;
    for(std::size_t i=0;i<initial.regs.size();++i)initial.regs[i]=0x10200000u+static_cast<std::uint32_t>(i);
    for(std::size_t i=0;i<initial.ext.size();++i)initial.ext[i]=0x7F800000u+static_cast<std::uint32_t>(i);
    initial.regs[13]=0x20001000;initial.cpsr=0xA80A0010;initial.fpscr=0x01000000;
    HudReapplyCall call;
    require(call.Begin(1,0x022B3168,initial,3,4),"An aligned ARM-mode same-core reapply must retain its original Tick state");
    HudA32Registers foreign=initial;foreign.regs.fill(0);foreign.ext.fill(0);foreign.cpsr=foreign.fpscr=0;
    const auto blank=foreign;
    require(!call.Begin(2,0x10000,foreign,3,4) && !call.Finish(2,initial.regs[13],3,4,foreign) &&
        !call.Finish(1,initial.regs[13]+8,3,4,foreign) && !call.Finish(1,initial.regs[13],5,4,foreign) &&
        !call.Finish(1,initial.regs[13],3,5,foreign) && foreign==blank && call.saved==initial,
        "A competing core or wrong-stack return must not overwrite the pending continuation or another core's registers");
    require(call.Finish(1,initial.regs[13],3,4,foreign),"The original core must complete exactly one reapply");
    initial.regs[15]=0x022B3168;
    require(foreign==initial && !call.Finish(1,initial.regs[13],3,4,foreign),
        "Return must restore every general/VFP/CPSR/FPSCR value and execute the Tick continuation exactly once");
    initial.cpsr|=0x20;
    require(!call.Begin(1,0x022B3168,initial,3,4),"A Thumb caller must not enter an ARM-only native HUD continuation");
    initial.cpsr&=~0x20u;initial.regs[13]+=4;
    require(!call.Begin(1,0x022B3168,initial,3,4),"An unaligned caller must not invoke the original AAPCS HUD function");
    std::cout<<"HUD interface scale/baseline/layout-pass/reapply tests passed\n";
}
