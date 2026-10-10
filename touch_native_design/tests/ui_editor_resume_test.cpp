// test_edit_resume.py injects the exact current production Reader and resume
// method. Metadata capture and paused-world predicates remain real production.
#include "ui_editor_model_fixture.inc"
#include "bl2_touch_ui_transactions.h"
#include <functional>
using namespace bl2::touch;
struct TestElf {std::uint32_t base=Fixture::base;std::uint32_t GetBaseAddress() const {return base;}};
struct DynarmicHost {
    Memory& memory;TestElf elf;
    TestElf& GetElfLoader() {return elf;}
    std::uint64_t GetGuestMemorySize() const {return 0x100000000ull;}
};
bool copyMemory(const void* h,std::uint32_t at,void* out,std::size_t bytes) {
    return Memory::read(&static_cast<const DynarmicHost*>(h)->memory,at,out,bytes);
}
#include "ui_editor_production.inc"
int main() {
    Fixture f;DynarmicHost host{f.memory,{}};ResumePredicate predicate(host);
    std::uint64_t generation=18;predicate.generation_=[&]{return generation;};
    const auto actor=f.type("Actor",0x300),cb=f.type("Controller",0x300,actor),ct=f.type("PlayerController",0x300,cb);
    const auto controller=f.object("Player",ct),pawn=f.object("Pawn",f.type("Pawn",0x300)),
        world=f.object("WorldInfo",f.type("WorldInfo",0x300));
    f.property(cb,"Pawn","ObjectProperty",0x120,4);f.property(actor,"WorldInfo","ObjectProperty",0x124,4);
    f.memory.word(controller+0x120,pawn);f.memory.word(controller+0x124,world);
    const auto engine=f.object("Engine",f.type("Engine",0x600)),local=f.object("LocalPlayer",f.type("LocalPlayer",0x300));
    const auto players=f.memory.allocate(16),movies=f.memory.allocate(16),gfx=f.memory.allocate(512),
        nativeWorld=f.memory.allocate(128),vp=f.memory.allocate(128),movie=f.memory.allocate(128),interface=f.memory.allocate(128);
    f.memory.word(f.base+0x03008D20,engine);f.memory.word(f.base+0x0300D940,nativeWorld);f.memory.word(f.base+0x0301F840,gfx);
    f.memory.word(local+0x40,controller);f.memory.word(players,local);f.memory.array(engine+0x48C,players,1,4);
    f.memory.word(gfx+0x44,vp+4);f.memory.word(vp+0x3C,1200);f.memory.word(vp+0x40,540);
    // The actual registered array contains ONLY Options; no old Pause token
    // or cached lifetime exists in the production predicate under test.
    const auto optionsType=f.type("OptionsGFxMovie",0x340,f.front),options=f.object("Options",optionsType);
    const auto optionsObj=f.object("OptionsObject",f.type("OptionsGFxObject",0x80));
    f.property(optionsType,"OptionsObj","ObjectProperty",0x280,4);f.memory.word(options+0x280,optionsObj);
    f.memory.word(options+0x260,f.list);f.memory.word(f.list+0x120,options);
    f.memory.word(options+0x3C,movie);f.memory.word(movie+0x64,options);f.memory.word(movie+0x34,interface);
    f.memory.word(movie+0x4C,1);f.memory.word(movie+0x5C,1);f.memory.word(movies,movie);f.memory.array(gfx+0x28,movies,1,4);
    constexpr std::array<std::uint32_t,9> producer{0xE59F3018,0xE79F3003,0xE5933000,0xE3530000,0x15D32294,
        0x17C02011,0x15C32294,0xE12FFF1E,0x00BA9C80};
    for(unsigned i=0;i<producer.size();++i) f.memory.word(f.base+0x0236A188+i*4,producer[i]);
    f.memory.word(engine+0x294,0x81);
    const Prepared focus{vp,controller,movie,options,1,true};
    const auto initial=predicate.resumeIdentity(focus);assert(initial && initial.owner==predicate.read_.token(options));
    UiEditResume edit;assert(edit.request(initial,1000));assert(edit.next(initial,true,false,true,1000));
    // Original Options Cancel invalidates the old scene, then a NEW original
    // Pause movie, owner, list and interface is published in the same game.
    ++generation;
    const auto newPause=f.object("NewPause",f.pause),newList=f.object("NewList",f.listType),
        newMovie=f.memory.allocate(128),newInterface=f.memory.allocate(128);
    f.memory.word(newPause+0x260,newList);f.memory.word(newList+0x120,newPause);
    f.memory.array(newList+0x178,f.ids,3,16);f.memory.array(newList+0x1C0,f.stack,1,4);
    f.memory.word(newPause+0x3C,newMovie);f.memory.word(newMovie+0x64,newPause);f.memory.word(newMovie+0x34,newInterface);
    f.memory.word(newMovie+0x4C,1);f.memory.word(newMovie+0x5C,1);f.memory.word(movies,newMovie);
    const Prepared resume{vp,controller,newMovie,newPause,2,true};
    const auto after=predicate.resumeIdentity(resume);assert(after && after!=initial && after.sameGame(initial));
    assert(edit.next(after,false,true,true,1016) && !edit.stage());assert(!edit.next(after,false,true,true,1032));
    const std::pair<std::uint32_t,std::uint32_t> faults[]{
        {newMovie+0x64,options},{newPause+0x3C,movie},{newPause+0x260,f.list},{newMovie+0x34,0},
        {newMovie+0x4C,0},{newMovie+0x5C,0},{movies,movie},{gfx+0x44,vp+8},
        {local+0x40,0},{engine+0x48C+4,2},{players,controller},{controller+0x120,0},{controller+0x124,0},
        {newPause+0xC,2},{newList+0xC,2},{engine+0xC,2},{local+0xC,2},{controller+0xC,2},{pawn+0xC,2},{world+0xC,2}};
    for(const auto [at,value]:faults) {
        const auto old=f.memory.word(at);f.memory.word(at,value);
        assert(!predicate.resumeIdentity(resume));f.memory.word(at,old);assert(predicate.resumeIdentity(resume));
    }
    f.memory.word(movies,movie);f.memory.word(f.list+0x120,options);
    assert(predicate.resumeIdentity(focus));
    f.memory.word(engine+0x294,0x80);assert(!predicate.resumeIdentity(focus)); // Frontend/unpaused Options with live pawn cannot resume.
    std::puts("PASS: production focused Options-only array, original Pause flag, CloseScene/new registered Pause,20 ownership negatives and exact-once Resume");
}
