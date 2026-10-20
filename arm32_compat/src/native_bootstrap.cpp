#include "native_host.h"
#include "native_jni.h"
#include "native_vfs.h"
#include "native_perf.h"
#include "native_vulkan_renderer.h"
#include "native_render_nvsi.h"
#include "bl2_native_resolution.h"
#include "obb_sha256.h"
#include "bl2_pause_debug_menu.h"
#include "bl2_movie_skip.h"
#include "bl2_startup_screens.h"
#include "bl2_touch_runtime.h"
#include "bl2_iris_wheeled_runtime.h"
#include "bl2_locale.h"
#include "bl2_lan_discovery_compat.h"
#include "bl2_lan_runtime_adapter.h"
#include "bl2_fresh_buffer_names.h"
#include "bl2_graphics_settings.h"
#include "thunk_manager.h"
#include "thunks_tegra.h"
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstring>
#include <sys/system_properties.h>

// Match the current ARM64 local-session lifecycle at the same original
// Steamworks seams. Native thunks use a real register-preserving continuation,
// rather than the interpreter's SVC instruction.
static bool InstallNativeLocalSession(DynarmicHost& host) {
    auto& loader = host.GetElfLoader();
    const auto base = loader.GetBaseAddress();
    const auto sign_in = loader.GetSymbolAddress("_ZN26UOnlineSubsystemSteamworks13SignInLocallyEv");
    const auto signed_in = loader.GetSymbolAddress("_Z23AndroidSteam_IsSignedInv");
    const auto connected = loader.GetSymbolAddress("_Z24AndroidSteam_IsConnectedv");
    if (loader.GetImageSize() != 0x03089000 ||
        loader.GetSymbolAddress("_ZN26UOnlineSubsystemSteamworks4InitEv") != base + 0x01F5AEE4 ||
        loader.GetSymbolAddress("_ZN26UOnlineSubsystemSteamworks26OnSteamServersDisconnectedEP26SteamServersDisconnected_t") != base + 0x01F55990 ||
        sign_in != base + 0x01F55934 || signed_in != base + 0x01F529AC || connected != base + 0x01F52948)
        return false;
    constexpr std::pair<std::uint32_t,std::uint32_t> preimage[] = {
        {0x01F5B4E4,0xE59440F0}, {0x01F5B4E8,0xE59D0040},
        {0x01F5B4EC,0xE2944000}, {0x01F5B4F0,0x13A04001},
        {0x01F5B500,0xE1A00004}, {0x01F5B504,0xE28DD084}, {0x01F5B508,0xE8BD8FF0},
        {0x01F55A94,0xE2509000}, {0x01F55A98,0x1A000039},
        {0x01F55A9C,0xE5D83111}, {0x01F55AA0,0xE3530000}, {0x01F55AA4,0x0A000038},
        {0x01F55AA8,0xE5941064}, {0x01F55B84,0xE28DD040}, {0x01F55B88,0xE8BD87F0},
        {0x01F55B8C,0xE1A00008}, {0x01F55B90,0xEBFFFF67},
    };
    for (const auto& [rva, word] : preimage)
        if (!loader.IsReadOnlyRange(base + rva,4) || host.MemoryRead32(base + rva) != word) return false;
    const std::uint32_t continuation_words[]{0xE59440F0,0xE59D0040,0xE51FF004,base + 0x01F5B4EC};
    const auto continuation = host.CreateCodeVeneer(continuation_words);
    if (!continuation) return false;
    const auto initialize = host.GetThunkManager().RegisterThunk("bl2_initialize_local_session",
        [sign_in,signed_in,connected,continuation](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
            jit.Regs()[15] = continuation;
            const auto owner = jit.Regs()[4];
            if (!NativeLocalIdentityPrepared() || owner < 0x10000 ||
                std::uint64_t(owner) + 0x4D8 > current.GetGuestMemorySize() ||
                current.MemoryRead8(owner + 0x111) != 0 ||
                !current.RunFunction(signed_in,{}) || current.RunFunction(connected,{})) return;
            current.RunFunction(sign_in,{owner});
            __android_log_print(ANDROID_LOG_INFO,"BL2Arm32",
                "Local session baseline initialized before viewport: status=%u remote=0",
                unsigned(current.MemoryRead8(owner + 0x111)));
        });
    const std::uint32_t disconnect_words[]{
        0xE5D83111,0xE3530001,0x059FF00C,0xE3530000,0x059FF008,0xE51FF004,
        base + 0x01F55AA8,base + 0x01F55B84,base + 0x01F55B8C};
    const auto disconnected = host.CreateCodeVeneer(disconnect_words);
    if (!initialize || !disconnected) return false;
    for (const auto& [site,veneer] : {std::pair{base + 0x01F5B4E4,initialize},
                                    std::pair{base + 0x01F55A9C,disconnected}}) {
        host.MemoryWrite32(site,0xE51FF004);host.MemoryWrite32(site + 4,veneer);
        host.InvalidateGuestCode(site,8);
        if (host.MemoryRead32(site) != 0xE51FF004 || host.MemoryRead32(site + 4) != veneer) return false;
    }
    return true;
}

// Diagnostic entry observers reuse the native register-preserving thunk path.
static bool InstallNativeShadowAllocationObserver(DynarmicHost& host) {
    char property[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.bl2.vk_gfx_capture",property)<=0 ||
        std::strcmp(property,"1")!=0 || !bl2vk::IsInitialized()) return true;
    auto& loader=host.GetElfLoader();const auto base=loader.GetBaseAddress();
    if (loader.GetImageSize()!=0x03089000 || base>UINT32_MAX-0x03089000) return false;
    for (const auto& [symbol,rva]:{std::pair{"GSystemSettings",0x02FFEE6Cu},
            std::pair{"GSceneRenderTargets",0x03013560u},
            std::pair{"GScreenWidth",0x02F3C2D4u},std::pair{"GScreenHeight",0x02F3C2D0u}})
        if (host.FindSymbol(symbol)!=base+rva) return false;
    struct Entry { const char* symbol;const char* label;std::uint32_t rva;
                   std::array<std::uint32_t,2> words; };
    static constexpr std::array entries{
        Entry{"_ZN19FSceneRenderTargets14InitDynamicRHIEv","InitDynamicRHI",0x019608B8,{0xE92D4FF0,0xE1A04000}},
        Entry{"_ZN19FSceneRenderTargets8AllocateEjjj","Allocate",0x019544C0,{0xE92D41F0,0xE1A04000}},
        Entry{"_ZN28TShadowProjectionPixelShaderI13F4SampleHwPCFE13SetParametersEPK10FSceneViewPK20FProjectedShadowInfo",
              "HardwarePCF.SetParameters",0x01976250,{0xE92D4FF0,0xED2D8B02}}
    };
    for (const auto& entry:entries) {
        const auto site=base+entry.rva;
        if (host.FindSymbol(entry.symbol)!=site || !loader.IsReadOnlyRange(site,8) ||
            host.MemoryRead32(site)!=entry.words[0] || host.MemoryRead32(site+4)!=entry.words[1]) return false;
    }
    static std::array<std::atomic<std::uint32_t>,entries.size()> counts{};
    static std::atomic<std::uint32_t> sequence{};
    std::array<std::uint32_t,entries.size()> observers{};
    for (std::size_t i=0;i<entries.size();++i) {
        const auto site=base+entries[i].rva;
        const std::array replayWords{entries[i].words[0],entries[i].words[1],0xE51FF004u,site+8};
        const auto replay=host.CreateCodeVeneer(replayWords);if (!replay) return false;
        observers[i]=host.GetThunkManager().RegisterThunk(std::string("bl2_vk_shadow_entry_")+entries[i].label,
            [i,base,replay](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                jit.Regs()[15]=replay;
                auto report=counts[i].load(std::memory_order_relaxed);
                while (report<8 && !counts[i].compare_exchange_weak(report,report+1,
                    std::memory_order_relaxed,std::memory_order_relaxed)) {}
                if (report>=8) return;
                const auto ordinal=sequence.fetch_add(1,std::memory_order_relaxed)+1;
                const auto system=base+0x02FFEE6Cu,scene=base+0x03013560u;
                const auto readSystem=[&](std::uint32_t offset) { return current.MemoryRead32(system+offset); };
                const auto readScene=[&](std::uint32_t offset) { return current.MemoryRead32(scene+offset); };
                const auto span=[&](std::uint32_t address,std::size_t bytes) {
                    return address>=0x10000 && std::uint64_t(address)+bytes<=current.GetGuestMemorySize();
                };
                const auto width=readScene(0xE0),height=readScene(0xE4);
                const auto screenWidth=current.MemoryRead32(base+0x02F3C2D4u);
                const auto screenHeight=current.MemoryRead32(base+0x02F3C2D0u);
                const auto a1=jit.Regs()[1],a2=jit.Regs()[2],a3=jit.Regs()[3];
                const int wouldUpdate=i==1 ? int(screenWidth>width || screenHeight>height ||
                    (a3 && (screenWidth!=width || screenHeight!=height))) : -1;
                // Shader fields are serialized resource parameters; flag0 means
                // these are entry snapshots before InitializePixelShader runs.
                unsigned shaderInitialized=UINT32_MAX,paramBase=UINT32_MAX,paramCount=UINT32_MAX,samplerIndex=UINT32_MAX;
                std::uint32_t selectedOffset=0,selectedRef=0;
                int primary=-1;unsigned infoCascade=UINT32_MAX,infoWhole=UINT32_MAX,lightType=UINT32_MAX;
                if (i==2) {
                    const auto shader=jit.Regs()[0];
                    if (span(shader,198)) {
                        shaderInitialized=current.MemoryRead8(shader+0x10)&1u;
                        paramBase=current.MemoryRead16(shader+192);
                        paramCount=current.MemoryRead16(shader+194);
                        samplerIndex=current.MemoryRead16(shader+196);
                    }
                    if (span(a2,0x139)) {
                        infoCascade=current.MemoryRead32(a2+0x130);
                        infoWhole=current.MemoryRead8(a2+0x138);
                        if (infoCascade || !infoWhole) primary=0;
                        else {
                            const auto light=current.MemoryRead32(a2+8);
                            if (span(light,0x13D)) { lightType=current.MemoryRead8(light+0x13C);primary=lightType==3; }
                        }
                        if (primary>=0) { selectedOffset=primary?0x1D0:0x1C0;selectedRef=readScene(selectedOffset); }
                    }
                }
                __android_log_print(ANDROID_LOG_INFO,"BL2Vulkan",
                    "Shadow allocation entry: sequence=%u function=%s report=%u this=%08x guestLR=%08x "
                    "dynamicShadows=%u/%u hardwarePCF=%u/%u scene=%08x sceneInitialized=%u sceneSize=%ux%u "
                    "screenSize=%ux%u args=%08x,%08x,%08x wouldUpdateRHI=%d "
                    "hardwareTexture=%08x,%08x hardwareSurface=%08x,%08x manualTexture=%08x,%08x manualSurface=%08x,%08x "
                    "shaderInitialized=%u paramBase=%u paramCount=%u samplerIndex=%u primary=%d "
                    "infoCascade=%u infoWhole=%u lightType=%u selectedOffset=%03x selectedRef=%08x",
                    ordinal,entries[i].label,report+1,jit.Regs()[0],jit.Regs()[14],
                    readSystem(0x42C),readSystem(0x4D4),readSystem(0x454),readSystem(0x4D8),scene,
                    unsigned(current.MemoryRead8(scene+0x10)&1u),width,height,screenWidth,screenHeight,a1,a2,a3,wouldUpdate,
                    readScene(0x1C0),readScene(0x1D0),readScene(0x1C8),readScene(0x1D8),
                    readScene(0x1E0),readScene(0x1F0),readScene(0x1E8),readScene(0x1F8),
                    shaderInitialized,paramBase,paramCount,samplerIndex,primary,infoCascade,infoWhole,lightType,selectedOffset,selectedRef);
            });
        if (!observers[i]) return false;
    }
    // Validate and prepare all three observers before changing any entry.
    for (std::size_t i=0;i<entries.size();++i) {
        const auto site=base+entries[i].rva;
        host.MemoryWrite32(site,0xE51FF004);host.MemoryWrite32(site+4,observers[i]);host.InvalidateGuestCode(site,8);
        if (host.MemoryRead32(site)!=0xE51FF004 || host.MemoryRead32(site+4)!=observers[i]) return false;
    }
    return true;
}

static bool InstallNativeRenderWithoutNvsi(DynarmicHost& host) {
    using namespace bl2::native_render;
    const auto& loader=host.GetElfLoader();
    const auto base=loader.GetBaseAddress();
    if (loader.GetImageSize()!=kImageBytes || base>UINT32_MAX-kImageBytes ||
        host.FindSymbol(kSymbol)!=base+kFunctionRva ||
        !loader.IsReadOnlyRange(base+kFunctionRva,kFunctionBytes)) return false;
    for (const auto& binding:kBindings)
        if (host.FindSymbol(binding.symbol)!=base+binding.targetRva) return false;
    return Apply(base,[&](std::uint32_t a) { return host.MemoryRead32(a); },
        [&](std::uint32_t a,std::uint32_t value) { host.MemoryWrite32(a,value); },
        [&](std::uint32_t a,std::size_t bytes) { host.InvalidateGuestCode(a,bytes); });
}

static JavaVM* javaVm=nullptr;
JavaVM* GetHostJavaVM() { return javaVm; }
static std::string JavaString(JNIEnv* env,jstring value) {
    if(!value) return {};
    const auto* chars=env->GetStringUTFChars(value,nullptr);
    if(!chars) return {};
    std::string result(chars); env->ReleaseStringUTFChars(value,chars); return result;
}
static bool VerifyEngine(const std::string& path) {
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if(fd<0) return false;
    struct stat info{};
    if(fstat(fd,&info) || info.st_size!=49885152) { close(fd); return false; }
    void* bytes=mmap(nullptr,info.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(bytes==MAP_FAILED) return false;
    const auto* input=static_cast<const std::uint8_t*>(bytes);
    bool valid=false;
    {
        const ObbSha256::MappingScope guestDigest(input,info.st_size);
        valid=VerifyBl2TouchInput(input,info.st_size) && VerifyBl2PauseDebugMenuInput(input,info.st_size) && VerifyBl2LocaleGuest(input,info.st_size) && VerifyBl2NativeResolutionInput(input,info.st_size);
    }
    munmap(bytes,info.st_size); return valid;
}
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,void*) {
    javaVm=vm;
    JNIEnv* env=nullptr;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK) return JNI_ERR;
    jclass thread=env->FindClass("android/app/ActivityThread");
    jmethodID current=thread?env->GetStaticMethodID(thread,"currentApplication","()Landroid/app/Application;"):nullptr;
    jobject context=current?env->CallStaticObjectMethod(thread,current):nullptr;
    if(!context || env->ExceptionCheck()) return JNI_ERR;
    jclass contextClass=env->GetObjectClass(context);
    jmethodID getInfo=env->GetMethodID(contextClass,"getApplicationInfo","()Landroid/content/pm/ApplicationInfo;");
    jobject info=env->CallObjectMethod(context,getInfo);
    jclass infoClass=env->GetObjectClass(info);
    const auto field=[&](const char* name) { return JavaString(env,static_cast<jstring>(env->GetObjectField(info,env->GetFieldID(infoClass,name,"Ljava/lang/String;")))); };
    const std::string dir=field("nativeLibraryDir"),apk=field("sourceDir");
    if(env->ExceptionCheck() || dir.empty() || apk.empty()) return JNI_ERR;
    const auto enginePath=dir+"/libUE3Runtime.so";
    if(!InitializeNativeVfs(env,context,apk.c_str()) || !VerifyEngine(enginePath)) return JNI_ERR;
    if(!dlopen((dir+"/libtsec_wrapper.so").c_str(),RTLD_NOW|RTLD_GLOBAL)) {
        __android_log_print(ANDROID_LOG_ERROR,"BL2Arm32","TSec loader: %s",dlerror()); return JNI_ERR;
    }
    void* engine=dlopen(enginePath.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!engine) { __android_log_print(ANDROID_LOG_ERROR,"BL2Arm32","Engine loader: %s",dlerror()); return JNI_ERR; }
    auto& host=GetNativeHost();
    if(!host.InitializeNative(engine) || !host.GetElfLoader().RedirectImports(host.GetThunkManager()) ||
       !InstallNativeJniRuntimePatches() || !InstallBl2NativeResolutionPatch(host) ||
       !InstallBl2FreshPersistentBufferNames(host) || !InstallBl2GraphicsSettings(host) || !InstallNativeLocalSession(host) || !InstallBl2LanDiscoveryCompat(host) ||
       !InstallBorderlandsNvsiLifecycle(host) || !InstallNativeRenderWithoutNvsi(host) || !InstallBl2TouchRuntime(host) || !InstallBl2PauseDebugMenu(host) || !InstallBl2MovieSkip(host) || !InstallBl2StartupScreens(host) || !InstallBl2LocaleOverlay(host) || !InstallNativePerfProbe(host) || !InstallNativeShadowAllocationObserver(host) || !InstallBl2WeaponAmmo(host) || !InstallBl2LanPacketAdapter(host) || !InstallBl2SubtitleTiming(host) || !InstallBl2OrchidOwnership(host)) {
        __android_log_print(ANDROID_LOG_ERROR,"BL2Arm32","Native compatibility initialization failed"); return JNI_ERR;
    }
    if(!bl2::iris::wheeled::InstallCallbacks(host))return JNI_ERR;
    auto onLoad=reinterpret_cast<jint(*)(JavaVM*,void*)>(dlsym(engine,"JNI_OnLoad"));
    const auto result=onLoad?onLoad(vm,nullptr):JNI_ERR;
    if(result<0 || env->ExceptionCheck() || !RegisterNativeActivityHooks(env)) return JNI_ERR;
    __android_log_print(ANDROID_LOG_INFO,"BL2Arm32","Native ARM32 compatibility ready; engine executes directly");
    return result;
}
