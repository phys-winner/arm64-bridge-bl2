#include "bl2_graphics_settings.h"
#include "bl2_touch_settings.h"
#include "bl2_touch_guest_call.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <cerrno>

#if defined(BL2_NATIVE_ARM32)
const std::string& NativeSavedFilesRoot();
#endif

namespace {
using bl2::graphics::Data;
constexpr std::uint32_t kSystem=0x02FFEE6C,kApply=0x012792EC;
constexpr std::uint32_t kExec=0x0127942C,kLoad=0x023BE3EC,kSave=0x01269EC8,kView=0x0235F518;
constexpr std::uint32_t kInitializeRender=0x0126C700;
std::atomic<std::uint32_t> filtering{0},hostAnisotropy{0};
struct State {
    DynarmicHost* host=nullptr;
    std::uint32_t base=0,scratch=0,revision=0,player=0,playerIndex=0,playerClass=0;
    Data original{};
    bool ready=false,potato=false,applying=false,automaticAnisotropy=true,freshDefaults=false;
#if defined(BL2_NATIVE_ARM32)
    bool initialPreferencesApplied=false;
#endif
    unsigned originalDepth=0;
    std::recursive_mutex mutex;
} state;
enum class Kind { Exec,Load,Save,View };
struct Frame {
    std::uintptr_t core;
    std::uint32_t sp,lr;
    Kind kind;
    bool swapped;
    Data previous{};
};
thread_local std::vector<Frame> frames;
std::uintptr_t coreIdentity([[maybe_unused]] Dynarmic::A32::Jit& jit) {
#if defined(BL2_NATIVE_ARM32)
    return Dynarmic::A32::Jit::NativeCoreIdentity();
#else
    return reinterpret_cast<std::uintptr_t>(&jit);
#endif
}
bool span(DynarmicHost& host,std::uint32_t address,std::size_t bytes) {
    return address>=0x10000 && std::uint64_t(address)+bytes<=host.GetGuestMemorySize();
}
Data read(DynarmicHost& host) {
    Data out; for(std::size_t i=0;i<out.size();++i)out[i]=host.MemoryRead32(state.base+kSystem+4+std::uint32_t(i*4));
    return out;
}
void write(DynarmicHost& host,const Data& data,std::uint32_t address=0) {
    if(!address)address=state.base+kSystem+4;
    for(std::size_t i=0;i<data.size();++i)host.MemoryWrite32(address+std::uint32_t(i*4),i==52/4 ? 0u : data[i]);
}
bool valid(const Data& data) {
    float percent;std::memcpy(&percent,&data[1020/4],4);
    if(!std::isfinite(percent) || percent<1 || percent>200 || data[1000/4]<1 || data[1000/4]>16 ||
        data[1012/4]>2 || data[168/4]>3 || data[0]>2)return false;
    for(auto offset:bl2::graphics::hiddenOffsets)if(data[offset/4]>1)return false;
    for(std::uint32_t group=0;group<28;++group) {
        auto index=(212+group*28)/4;
        if(data[index]>16 || data[index+1]>16 || data[index]>data[index+1])return false;
    }
    return true;
}
void trace(const char* event,DynarmicHost& host) {
    char property[PROP_VALUE_MAX]{};
    if(__system_property_get("debug.bl2bridge.touch_diagnostics",property)<=0 || property[0]!='1')return;
    const auto actual=read(host);
    auto mask=[](const Data& data) {
        std::uint32_t bits=0;
        for(std::size_t i=0;i<bl2::graphics::hiddenOffsets.size();++i)
            if(data[bl2::graphics::hiddenOffsets[i]/4])bits|=1u<<i;
        return bits;
    };
    auto floating=[](const Data& data,std::uint32_t offset) {
        float value;std::memcpy(&value,&data[offset/4],4);return value;
    };
    __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
        "graphics event=%s potato=%u hidden baseline=%03x applied=%03x aniso baseline=%u applied=%u gl=%u "
        "texture baseline=%u applied=%u mip baseline=%u/%u/%u applied=%u/%u/%u "
        "detail baseline=%u applied=%u view baseline=%u applied=%u draw baseline=%.3f applied=%.3f "
        "ao=%u dof=%u fxaa=%u decals=%u population=%d foliage=%.3f "
        "render baseline=%.1f applied=%.1f res=%ux%u",
        event,state.potato,mask(state.original),mask(actual),state.original[1000/4],actual[1000/4],
        filtering.load(std::memory_order_relaxed),state.original[1012/4],actual[1012/4],
        state.original[212/4],state.original[216/4],state.original[220/4],actual[212/4],actual[216/4],actual[220/4],
        state.original[0],actual[0],state.original[168/4],actual[168/4],floating(state.original,4),floating(actual,4),
        actual[68/4],actual[60/4],actual[64/4],actual[192/4],static_cast<std::int32_t>(actual[156/4]),floating(actual,1008),
        floating(state.original,1020),floating(actual,1020),actual[1028/4],actual[1032/4]);
}
bool ready(DynarmicHost& host) {
    if(state.ready)return true;
    if(!span(host,state.base+kSystem,4+sizeof(Data)))return false;
    auto current=read(host);if(!valid(current))return false;
    state.original=current;state.ready=true;
    trace("loaded",host);
    return true;
}
void publishHidden() {
    for(std::size_t i=0;i<bl2::graphics::hiddenOffsets.size();++i)
        bl2::touch::SetBl2TouchSetting(21+i,state.original[bl2::graphics::hiddenOffsets[i]/4]);
    const auto maximum=hostAnisotropy.load(std::memory_order_acquire);
    if(!state.automaticAnisotropy || maximum)
        bl2::touch::SetBl2TouchSetting(31,bl2::graphics::AnisotropyIndex(state.automaticAnisotropy?maximum:state.original[1000/4]));
}
bool playerValid(DynarmicHost& host) {
    return state.player && span(host,state.player,0x38) &&
        host.MemoryRead32(state.player+0x20)==state.playerIndex &&
        host.MemoryRead32(state.player+0x34)==state.playerClass &&
        !(host.MemoryRead32(state.player+0x0C)&0x20000002u);
}
bool apply(DynarmicHost& host,bool updateView) {
    const auto previous=read(host);
    state.original=bl2::graphics::KeepRuntimeControls(state.original,previous);
    const auto desired=state.potato ? bl2::graphics::Potato(state.original) : state.original;
    write(host,desired,state.scratch);
    state.applying=true;
    struct Guard { ~Guard(){state.applying=false;} } guard;
    std::uint32_t result=0;
    if(!bl2::touch::Bl2TouchCallGuest(host,state.base+kApply,{state.base+kSystem,state.scratch,0},result) || !valid(read(host))) {
        write(host,previous);trace("apply-failed",host);return false;
    }
    filtering.store((state.potato?1u:state.automaticAnisotropy?0u:desired[1000/4])|(state.potato?0x100u:0u),std::memory_order_release);
    if(updateView && playerValid(host) &&
        !bl2::touch::Bl2TouchCallGuest(host,state.base+kView,{state.player,state.potato?0u:state.original[168/4]},result))
        HORN_LOGE("BL2Graphics","view-distance component refresh failed after graphics apply");
    trace("applied",host);
    return true;
}
std::uint32_t code(DynarmicHost& host,const std::array<std::uint32_t,4>& words) {
#if defined(BL2_NATIVE_ARM32)
    return host.CreateCodeVeneer(words);
#else
    const auto address=host.GuestAlloc(sizeof(words),4);if(!address)return 0;
    for(std::size_t i=0;i<words.size();++i)host.MemoryWrite32(address+std::uint32_t(i*4),words[i]);
    host.AddExecutableRange(address,sizeof(words));host.InvalidateGuestCode(address,sizeof(words));return address;
#endif
}
bool install(DynarmicHost& host,Kind kind,std::uint32_t rva,const std::array<std::uint32_t,2>& expected) {
    const auto entry=state.base+rva;
    if(host.MemoryRead32(entry)!=expected[0] || host.MemoryRead32(entry+4)!=expected[1])return false;
    const auto continuation=code(host,{expected[0],expected[1],0xE51FF004,entry+8});if(!continuation)return false;
    const auto returned=host.GetThunkManager().RegisterThunk("bl2_graphics_original_return_"+std::to_string(rva),
        [](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
            std::lock_guard<std::recursive_mutex> lock(state.mutex);
            if(frames.empty() || frames.back().core!=coreIdentity(jit) || frames.back().sp!=jit.Regs()[13]) {
                HORN_LOGE("BL2Graphics","original graphics return frame mismatch");
                jit.HaltExecution(Dynarmic::HaltReason::MemoryAbort);return;
            }
            auto frame=std::move(frames.back());frames.pop_back();jit.Regs()[14]=frame.lr;jit.Regs()[15]=frame.lr;
            if(frame.swapped)--state.originalDepth;
            if(frame.kind==Kind::View || (frame.kind==Kind::Exec && jit.Regs()[0])) {
                const auto selected=read(current);
                if(valid(selected)) { state.original=selected;publishHidden(); }
                else HORN_LOGE("BL2Graphics","original graphics setting rejected invalid data");
                if(!state.potato) {
                    filtering.store(state.automaticAnisotropy?0u:state.original[1000/4],std::memory_order_release);
                    trace("original-edit",current);
                }
                else if(!apply(current,true)) {
                    write(current,frame.previous);
                    HORN_LOGE("BL2Graphics","cannot reapply Potato after original setting");
                }
            } else write(current,frame.previous);
        });
    if(!returned)return false;
    const auto enter=host.GetThunkManager().RegisterThunk("bl2_graphics_original_entry_"+std::to_string(rva),
        [kind,continuation,returned](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
            std::lock_guard<std::recursive_mutex> lock(state.mutex);
            jit.Regs()[15]=continuation;
            if(state.applying || !ready(current))return;
            if(kind==Kind::View) {
                const auto player=jit.Regs()[0];
                if(span(current,player,0x38) && !(current.MemoryRead32(player+0x0C)&0x20000002u)) {
                    state.player=player;state.playerIndex=current.MemoryRead32(player+0x20);state.playerClass=current.MemoryRead32(player+0x34);
                }
                if(jit.Regs()[1]>3)return;
            }
            if(state.originalDepth || frames.size()>=8 || (!state.potato && kind!=Kind::Exec && kind!=Kind::View))return;
            const bool swapped=state.potato;
            frames.push_back({coreIdentity(jit),jit.Regs()[13],jit.Regs()[14],kind,swapped,read(current)});
            if(swapped) {
                ++state.originalDepth;
                state.original=bl2::graphics::KeepRuntimeControls(state.original,frames.back().previous);
                write(current,state.original);
            }
            jit.Regs()[14]=returned;
        });
    if(!enter)return false;
    host.MemoryWrite32(entry,0xE51FF004);host.MemoryWrite32(entry+4,enter);host.InvalidateGuestCode(entry,8);return true;
}
#if defined(BL2_NATIVE_ARM32)
bool initialTouchProjection(const Data& loaded,const std::array<std::uint32_t,12>& values,
    Data& original,Data& effective,bool& automaticAnisotropy) {
    if(values[0]>1 || (values[11]!=UINT32_MAX && values[11]>4))return false;
    for(std::size_t i=0;i<10;++i)if(values[i+1]!=UINT32_MAX && values[i+1]>1)return false;
    original=loaded;
    for(std::size_t i=0;i<10;++i)
        if(values[i+1]!=UINT32_MAX)original[bl2::graphics::hiddenOffsets[i]/4]=values[i+1];
    automaticAnisotropy=values[11]==UINT32_MAX;
    if(!automaticAnisotropy)original[1000/4]=bl2::graphics::anisotropyValues[values[11]];
    effective=values[0] ? bl2::graphics::Potato(original) : original;
    return true;
}

// shortcut: startup allocation only; runtime shadow enabling needs its own resource rebuild.
void initializeTouchGraphics(DynarmicHost& host) {
    if(state.initialPreferencesApplied)return;
    std::array<std::uint32_t,12> values{};
    {
        auto& settings=bl2::touch::touchSettingsState;
        std::lock_guard lock(settings.mutex);
        if(!settings.initialized)return;
        std::copy_n(settings.values.begin()+20,values.size(),values.begin());
    }
    const auto loaded=read(host);Data original{},effective{};bool automatic=true;
    if(!valid(loaded) || !initialTouchProjection(loaded,values,original,effective,automatic) || !valid(effective))return;
    state.original=original;state.potato=values[0]!=0;state.automaticAnisotropy=automatic;state.ready=true;
    write(host,effective);
    filtering.store((state.potato?1u:automatic?0u:original[1000/4])|(state.potato?0x100u:0u),std::memory_order_release);
    state.initialPreferencesApplied=true;
    // The original instruction at kInitializeRender propagates these settings
    // to its own render cache and creates resources later in the original RHI.
    // Keep revision unacknowledged: first Sync still resolves absent values.
    trace("initial-touch-preferences",host);
}
#endif

bool installFreshDefaults(DynarmicHost& host) {
    const auto entry=state.base+kInitializeRender;
    if(host.MemoryRead32(entry)!=0xE59D0010 || host.MemoryRead32(entry+4)!=0xE3A01000)return false;
    const auto continuation=code(host,{0xE59D0010,0xE3A01000,0xE51FF004,entry+8});if(!continuation)return false;
    const auto thunk=host.GetThunkManager().RegisterThunk("bl2_graphics_fresh_defaults",
        [continuation](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
            std::lock_guard<std::recursive_mutex> lock(state.mutex);
            jit.Regs()[15]=continuation;
#if defined(BL2_NATIVE_ARM32)
            if(!state.freshDefaults && state.initialPreferencesApplied)return;
#else
            // Always enforce MotionBlur before the original render initialization.
#endif
            current.MemoryWrite32(state.base+kSystem+4+52,0);
            const auto loaded=read(current);if(!valid(loaded))return;
            if(state.freshDefaults) {
                state.freshDefaults=false;state.original=bl2::graphics::MinimumDefaults(loaded);state.ready=true;
                write(current,state.original);
                // This seam is before the original render-thread propagation and
                // before WillowSystemSettings builds its original Options values.
                state.applying=true;std::uint32_t result=0;
                const bool saved=bl2::touch::Bl2TouchCallGuest(current,state.base+kSave,{state.base+kSystem},result);
                state.applying=false;
                if(!saved)HORN_LOGE("BL2Graphics","fresh graphics defaults could not be saved");
                trace("fresh-defaults",current);
            }
#if defined(BL2_NATIVE_ARM32)
            initializeTouchGraphics(current);
#endif
        });
    if(!thunk)return false;
    host.MemoryWrite32(entry,0xE51FF004);host.MemoryWrite32(entry+4,thunk);host.InvalidateGuestCode(entry,8);return true;
}
}

std::uint32_t Bl2GraphicsFiltering() { return filtering.load(std::memory_order_acquire); }
void Bl2GraphicsHostAnisotropy(std::uint32_t maximum) {
    hostAnisotropy.store(std::clamp(maximum,1u,16u),std::memory_order_release);
}
bool InstallBl2GraphicsSettings(DynarmicHost& host) {
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    state.host=&host;state.base=host.GetElfLoader().GetBaseAddress();
    if(host.FindSymbol("GSystemSettings")!=state.base+kSystem)return false;
    // Old INI values and original engine commands must never re-enable the effect.
    if(host.MemoryRead32(state.base+0x01266C04)!=0xE5983038 ||
        host.MemoryRead32(state.base+0x01266F6C)!=0xE5903038)return false;
    for(const auto offset:{0x01266C04u,0x01266F6Cu}) {
        host.MemoryWrite32(state.base+offset,0xE3A03000); // mov r3, #0: cached MotionBlur flag
        host.InvalidateGuestCode(state.base+offset,4);
    }
#if defined(BL2_NATIVE_ARM32)
    const auto& files=NativeSavedFilesRoot();
#else
    extern std::string g_saved_files_dir;
    const auto& files=g_saved_files_dir;
#endif
    struct stat file{};
    state.freshDefaults=!files.empty() &&
        stat((files+"/Config/Android-WillowEngine.ini").c_str(),&file)!=0 && errno==ENOENT;
    state.scratch=host.GuestAlloc(sizeof(Data),16);if(!state.scratch)return false;
    return install(host,Kind::Exec,kExec,{0xE92D4FF0,0xE24DDE8A}) &&
        install(host,Kind::Load,kLoad,{0xE92D4FF0,0xED2D8B02}) &&
        install(host,Kind::Save,kSave,{0xE92D40F0,0xE24DD014}) &&
        install(host,Kind::View,kView,{0xE92D4FF0,0xE3510000}) && installFreshDefaults(host);
}
void SyncBl2GraphicsSettings(DynarmicHost& host) {
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    if(state.host!=&host || state.applying || state.originalDepth || !ready(host))return;
    auto& settings=bl2::touch::touchSettingsState;
    std::array<std::uint32_t,12> values{};std::uint32_t revision;
    {
        std::lock_guard<std::mutex> settingLock(settings.mutex);
        if(!settings.initialized || bl2::touch::kTouchSettingCount<32)return;
        revision=settings.revision;
        if(revision==state.revision)return;
        std::copy_n(settings.values.begin()+20,values.size(),values.begin());
    }
    // Missing preferences inherit the original loaded runtime values, before
    // the Potato projection. Publishing these ACKs lets Java persist them.
    const auto maximum=hostAnisotropy.load(std::memory_order_acquire);
    const bool pendingAnisotropy=values[11]==UINT32_MAX && !maximum;
    for(std::size_t i=0;i<11;++i)if(values[i+1]==UINT32_MAX && (i!=10 || maximum)) {
        values[i+1]=i==10 ? bl2::graphics::AnisotropyIndex(maximum) : state.original[bl2::graphics::hiddenOffsets[i]/4];
        bl2::touch::SetBl2TouchSetting(21+i,values[i+1]);
    }
    auto selected=state.original;
    for(std::size_t i=0;i<10;++i) {
        if(values[i+1]>1)return;
        selected[bl2::graphics::hiddenOffsets[i]/4]=values[i+1];
    }
    if((!pendingAnisotropy && values[11]>4) || values[0]>1)return;
    const bool previousAutomatic=state.automaticAnisotropy;
    if(!pendingAnisotropy) {
        selected[1000/4]=bl2::graphics::anisotropyValues[values[11]];
        state.automaticAnisotropy=false;
    }
    const bool change=selected!=state.original || state.potato!=(values[0]!=0) || state.automaticAnisotropy!=previousAutomatic;
    const auto previous=state.original;const bool previousPotato=state.potato;
    state.original=selected;state.potato=values[0]!=0;
    if(change && !apply(host,true)) {
        state.original=previous;state.potato=previousPotato;state.automaticAnisotropy=previousAutomatic;
        HORN_LOGE("BL2Graphics","runtime graphics apply failed; retry at next input boundary");return;
    }
    // A host edit arriving during a synchronous engine call is consumed at
    // the next boundary; never ACK an unseen newer revision.
    if(!pendingAnisotropy)state.revision=revision;
}
