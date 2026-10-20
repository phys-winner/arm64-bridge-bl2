#include "native_host.h"
#include "native_jni.h"
#include "native_perf.h"
#include "perf_probe.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include "bl2_native_resolution.h"
#include "bl2_perf_hint.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

extern void RefreshGlBindings();
extern void GlStateProbePresent();
extern void Bl2AutoFlushReset();
extern void Bl2ShaderWarmupContextCreated(EGLDisplay,EGLContext,EGLContext);
extern void Bl2ShaderWarmupContextDestroyed(EGLContext);
extern void Bl2ShaderWarmupDisplayTerminated(EGLDisplay);
extern void Bl2LegacyStagePrewarmFrame(uint64_t next_start_ns);

namespace {
struct WindowSurface { EGLDisplay display; EGLConfig config; EGLSurface surface; };
std::mutex windowsMutex;
std::unordered_map<ANativeWindow*,WindowSurface> windows;
template<class T> T Pointer(std::uint32_t address) { return reinterpret_cast<T>(address); }
std::uint32_t Address(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }
}

void RegisterAndroidThunks(ThunkManager& manager,DynarmicHost&) {
    using Jit=Dynarmic::A32::Jit;
    using Host=DynarmicHost;
    manager.RegisterThunk("eglBindAPI",[](Jit& j,Host&) {
        j.Regs()[0]=eglBindAPI(j.Regs()[0]==EGL_OPENGL_API?EGL_OPENGL_ES_API:j.Regs()[0]);
    });
    manager.RegisterThunk("eglGetConfigAttrib",[](Jit& j,Host&) {
        auto* output=Pointer<EGLint*>(j.Regs()[3]);
        j.Regs()[0]=eglGetConfigAttrib(Pointer<EGLDisplay>(j.Regs()[0]),Pointer<EGLConfig>(j.Regs()[1]),j.Regs()[2],output);
        if(j.Regs()[0] && j.Regs()[2]==EGL_RENDERABLE_TYPE && (*output&EGL_OPENGL_ES3_BIT)) *output|=EGL_OPENGL_BIT;
    });
    manager.RegisterThunk("eglCreateContext",[](Jit& j,Host&) {
        EGLDisplay display=Pointer<EGLDisplay>(j.Regs()[0]);
        EGLContext share=Pointer<EGLContext>(j.Regs()[2]);
        const EGLint attributes[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
        EGLContext context=eglCreateContext(display,Pointer<EGLConfig>(j.Regs()[1]),share,attributes);
        if(context!=EGL_NO_CONTEXT) Bl2ShaderWarmupContextCreated(display,context,share);
        __android_log_print(ANDROID_LOG_INFO,"BL2Arm32","GLES3.2 context=%p",context);
        j.Regs()[0]=Address(context);
    });
    manager.RegisterThunk("eglCreateWindowSurface",[](Jit& j,Host&) {
        auto display=Pointer<EGLDisplay>(j.Regs()[0]);
        auto config=Pointer<EGLConfig>(j.Regs()[1]);
        auto* window=Pointer<ANativeWindow*>(j.Regs()[2]);
        std::lock_guard lock(windowsMutex);
        auto prior=windows.find(window);
        if(prior!=windows.end() && prior->second.display==display && prior->second.config==config) {
            j.Regs()[0]=Address(prior->second.surface); return;
        }
        auto surface=eglCreateWindowSurface(display,config,window,Pointer<const EGLint*>(j.Regs()[3]));
        if(surface!=EGL_NO_SURFACE) windows[window]={display,config,surface};
        j.Regs()[0]=Address(surface);
    });
    manager.RegisterThunk("eglDestroySurface",[](Jit& j,Host&) {
        auto surface=Pointer<EGLSurface>(j.Regs()[1]);
        std::lock_guard lock(windowsMutex);
        for(auto it=windows.begin();it!=windows.end();) it=it->second.surface==surface?windows.erase(it):std::next(it);
        j.Regs()[0]=eglDestroySurface(Pointer<EGLDisplay>(j.Regs()[0]),surface);
    });
    manager.RegisterThunk("eglDestroyContext",[](Jit& j,Host&) {
        auto context=Pointer<EGLContext>(j.Regs()[1]);
        const auto result=eglDestroyContext(Pointer<EGLDisplay>(j.Regs()[0]),context);
        if(result) Bl2ShaderWarmupContextDestroyed(context);
        j.Regs()[0]=result;
    });
    manager.RegisterThunk("eglTerminate",[](Jit& j,Host&) {
        auto display=Pointer<EGLDisplay>(j.Regs()[0]);
        const auto result=eglTerminate(display);
        if(result) { Bl2ShaderWarmupDisplayTerminated(display); std::lock_guard lock(windowsMutex); windows.clear(); }
        j.Regs()[0]=result;
    });
    manager.RegisterThunk("eglMakeCurrent",[](Jit& j,Host& host) {
        j.Regs()[0]=eglMakeCurrent(Pointer<EGLDisplay>(j.Regs()[0]),Pointer<EGLSurface>(j.Regs()[1]),Pointer<EGLSurface>(j.Regs()[2]),Pointer<EGLContext>(j.Regs()[3]));
        if(j.Regs()[0] && j.Regs()[3]) {
            static std::atomic_flag reported=ATOMIC_FLAG_INIT;
            if(!reported.test_and_set(std::memory_order_relaxed)) {
                const auto* vendor=reinterpret_cast<const char*>(glGetString(GL_VENDOR));
                const auto* renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
                const auto* version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
                __android_log_print(ANDROID_LOG_INFO,"BL2Arm32",
                    "Driver GL_VENDOR='%s' GL_RENDERER='%s' GL_VERSION='%s'",
                    vendor?vendor:"",renderer?renderer:"",version?version:"");
            }
        }
        if(j.Regs()[0]) { RefreshGlBindings(); CheckBl2NativeResolutionLimits(host); }
    });
    manager.RegisterThunk("eglSwapBuffers",[](Jit& j,Host& host) {
        AuditBl2NativeResolutionPresent(host);
        NativeApplyFrameRateLimit();
        const auto start=NativePerfSwapStart();
        {
            HornPerfScope swap(HornPerfCategory::Swap);
            j.Regs()[0]=eglSwapBuffers(Pointer<EGLDisplay>(j.Regs()[0]),Pointer<EGLSurface>(j.Regs()[1]));
            Bl2AutoFlushReset();
        }
        NativePerfPresent(host,start,j.Regs()[0]!=0);
        if(j.Regs()[0]) HornPerfFrameBoundary();
        if(j.Regs()[0]) Bl2PerfHintFrame(NativeCurrentFrameRateLimit());
        if(j.Regs()[0]) GlStateProbePresent();
        if(j.Regs()[0]) Bl2LegacyStagePrewarmFrame(0);
#if HORN_ENABLE_LOGGING
        static std::atomic<std::uint32_t> frames{0};
        const auto frame=frames.fetch_add(1)+1;
        if(frame<=3 || frame%300==0) __android_log_print(ANDROID_LOG_INFO,"BL2Arm32","frame=%u swap=%u",frame,j.Regs()[0]);
#endif
    });
    manager.RegisterThunk("eglGetProcAddress",[](Jit& j,Host& host) {
        const auto* name=Pointer<const char*>(j.Regs()[0]);
        const auto shim=host.GetThunkManager().FindRegisteredSymbol(name);
        j.Regs()[0]=shim?shim:reinterpret_cast<std::uintptr_t>(eglGetProcAddress(name));
    });
    manager.RegisterThunk("dlsym",[](Jit& j,Host& host) {
        const auto* name=Pointer<const char*>(j.Regs()[1]);
        const auto shim=host.GetThunkManager().FindRegisteredSymbol(name);
        j.Regs()[0]=shim?shim:Address(dlsym(Pointer<void*>(j.Regs()[0]),name));
    });
}
