#pragma once
#include <array>
#include <cmath>

// Isolated numerical candidate. No UObject layout, guest calls, or integration.
namespace wheeled_pawn_candidate {
struct DirectionRange {float DirDegreesPerSecond,SpeedRange;};
struct Config {
    DirectionRange DirChangeRateMin,DirChangeRateMax;
    float BlendTime,ReferenceSpeed,SpeedUpRate,SpeedDownRate,BrakeRate,IdleBlendTime;
};
struct State {float CurSpeed,DirAngle;std::array<float,4> Weight;bool bIsIdle;};
struct Vector2 {float x,y;};
struct Inputs {
    float DeltaTime,TargetSpeed;
    Vector2 DesiredPlanar,ActorPlanar;
    bool HasWillowPawnContext,MovementActive;
    std::array<bool,4> SequenceCast;
};
struct Result {State state;std::array<bool,4> SetPlayRate{};float PlayRate=0;};

inline float Clamp(float x,float low,float high) {return x<low?low:(x<high?x:high);}
inline Vector2 Normalize(Vector2 v) {
    const float xx=v.x*v.x,yy=v.y*v.y,length=xx+yy;
    if(length==1.0f)return v;
    if(length<1.0e-8f)return {0,0};
    // Preserve the source Newton step, with a portable reciprocal-sqrt seed.
    const float seed=1.0f/std::sqrt(length);
    const float a=length*seed,b=a*seed,c=3.0f-b,d=seed*c,inverse=0.5f*d;
    return {v.x*inverse,v.y*inverse};
}
inline float TargetAngle(const Inputs& in,bool idle) {
    if(idle||(std::fabs(in.DesiredPlanar.x)<1.0e-4f&&std::fabs(in.DesiredPlanar.y)<1.0e-4f))return 0;
    const auto desired=Normalize(in.DesiredPlanar),actor=Normalize(in.ActorPlanar);
    const auto right=Normalize({actor.y,-actor.x});
    const float a=actor.x*desired.x,b=actor.y*desired.y,dot=Clamp(a+b,-1,1);
    float angle=std::acos(dot);
    const float sideA=right.x*desired.x,sideB=right.y*desired.y;
    if(sideA+sideB>0)angle=-angle;
    return angle;
}
inline float DirectionRate(const Config& c,float speed) {
    float alpha=(speed-c.DirChangeRateMin.SpeedRange)/(c.DirChangeRateMax.SpeedRange-c.DirChangeRateMin.SpeedRange);
    float low=c.DirChangeRateMin.DirDegreesPerSecond,high=c.DirChangeRateMax.DirDegreesPerSecond;
    if(low>high){const auto old=low;low=high;high=old;alpha=1-alpha;}
    const float span=high-low,scaled=span*alpha;
    return Clamp(low+scaled,low,high);
}
inline float Ease2(float alpha) {
    if(alpha<0.5f){const float doubled=alpha*2.0f;return std::pow(doubled,2.0f)*0.5f;}
    const float reversed=1.0f-alpha,doubled=reversed*2.0f;
    return 1.0f-std::pow(doubled,2.0f)*0.5f;
}
inline Result Tick(State state,const Config& c,const Inputs& in) {
    Result out;out.state=state;auto& s=out.state;
    if(in.HasWillowPawnContext) {
        if(in.MovementActive) {
            if(in.TargetSpeed>s.CurSpeed){const float raised=s.CurSpeed+c.SpeedUpRate*in.DeltaTime;s.CurSpeed=raised<=in.TargetSpeed?raised:in.TargetSpeed;}
            else {const float reduced=s.CurSpeed-c.SpeedDownRate*in.DeltaTime;s.CurSpeed=reduced>=in.TargetSpeed?reduced:in.TargetSpeed;}
        } else {const float reduced=s.CurSpeed-c.BrakeRate*in.DeltaTime;s.CurSpeed=reduced>=0?reduced:0;}
        s.bIsIdle=s.CurSpeed<=0;
        if(c.ReferenceSpeed!=0) {
            out.PlayRate=Clamp(s.CurSpeed/c.ReferenceSpeed,0,10);
            for(unsigned i=1;i<4;++i)out.SetPlayRate[i]=in.SequenceCast[i];
        }
        const float target=TargetAngle(in,s.bIsIdle);
        float delta=target-s.DirAngle;
        if(double(delta)>3.141592653589793)delta=float(double(delta)-6.283185307179586);
        else if(double(delta)<-3.141592653589793)delta=float(double(delta)+6.283185307179586);
        if(delta!=0) {
            const float rate=DirectionRate(c,s.CurSpeed);
            if(rate>0) {
                const float degrees=rate*in.DeltaTime;
                const float step=float(double(degrees)*0.017453292519943295);
                s.DirAngle+=Clamp(delta,-step,step);
                while(double(s.DirAngle)>3.141592653589793)s.DirAngle-=6.2831854820251465f;
                while(double(s.DirAngle)<-3.141592653589793)s.DirAngle+=6.2831854820251465f;
            } else s.DirAngle=target;
        }
    }
    auto& w=s.Weight;
    if(s.bIsIdle) {
        if(w[0]<1&&c.IdleBlendTime>0) {
            const float changed=w[0]+in.DeltaTime/c.IdleBlendTime,alpha=changed<=1?changed:1;
            w[0]=Clamp(Ease2(alpha),0,1);
            const float first=w[2]+w[1],sum=first+w[3];
            if(sum>0){const float scale=(1-w[0])/sum;for(unsigned i=1;i<4;++i)w[i]*=scale;}
        } else w={1,0,0,0};
    } else {
        w[0]=0;s.DirAngle=Clamp(s.DirAngle,-1.5707963705062866f,1.5707963705062866f);
        const unsigned selected=s.DirAngle<0?2:3,opposite=selected==2?3:2;
        const float fraction=float(double(std::fabs(s.DirAngle))*0.6366197723675814);
        if(c.BlendTime>0) {
            const float step=in.DeltaTime/c.BlendTime;
            const float reduced=w[opposite]-step;w[opposite]=reduced>0?reduced:0;
            const float available=1-w[opposite],target=available*fraction;
            if(target>w[selected]){const float changed=w[selected]+step;w[selected]=changed>=target?target:changed;}
            else {const float changed=w[selected]-step;w[selected]=changed<=target?target:changed;}
            w[1]=available-w[selected];
        } else {w[selected]=fraction;w[1]=1-fraction;w[opposite]=0;}
    }
    return out;
}
} // namespace wheeled_pawn_candidate
