#pragma once

#include <string_view>

// Registration-time allowlist, not a symbol-prefix heuristic. Every other
// handler, including future imports and dynamically installed JNI/HLE thunks,
// remains UNKNOWN. The audited implementations either only read guest bytes,
// or publish every write through MemoryWrite*, GuestAlloc/Free, or the bulk
// guard. glGetQueryObjectuiv has an explicit final 4-byte notification.
inline bool GuestNativeWritesObserved(std::string_view name) {
    static constexpr std::string_view observed[] = {
        // Exact draw consumers and guest-read-only GL state setters.
        "glDrawArrays", "glDrawElements", "glDrawArraysInstanced",
        "glDrawElementsInstanced", "glDrawElementsBaseVertex",
        "glDrawElementsBaseVertexOES", "glDrawElementsBaseVertexEXT",
        "glDrawElementsInstancedBaseVertex",
        "glDrawElementsInstancedBaseVertexOES",
        "glDrawElementsInstancedBaseVertexEXT",
        "glNamedProgramLocalParametersI4uivEXT", "glBindProgramARB",
        "glBeginQuery", "glGetQueryObjectuiv", "glBindBuffer",
        "glBindFramebuffer", "glBindRenderbuffer", "glBindTexture",
        "glColorMask", "glCullFace", "glDepthFunc", "glDepthMask",
        "glFrontFace", "glPolygonOffset", "glStencilFunc",
        "glStencilFuncSeparate", "glStencilMask", "glStencilOp",
        "glStencilOpSeparate", "glVertexAttribPointer", "glViewport",
        "glDisableVertexAttribArray", "glBlendFuncSeparate", "glBlitFramebuffer",
        "glClear", "glFlush", "glDisable", "glEnableVertexAttribArray",
        // Additional exact guest-read-only GL state setters.
        "glReadBuffer", "glDrawBuffers", "glScissor", "glEnable",
        "glClearColor", "glVertexAttribDivisor", "glDepthRangef",
        "eglSwapBuffers", "bl2_convex_fastpath", "bl2_convex_outcode_fastpath",
        // Exact pinned limiter peak scan: reads guest history, changes JIT registers.
        "bl2_limiter_peak_scan",
        // Pinned visibility predicates are read-only; contained publishes its
        // output through MemoryWrite32. RHI replacements notify every write.
        "bl2_convex_contained_fastpath", "bl2_convex_sphere_fastpath",
        "bl2_convex_selftest", "bl2_setup_arrays_fastpath",
        "bl2_program_locals_fastpath",
        "bl2_audio_codebook_fastpath", "bl2_audio_floor_fastpath", "bl2_audio_check_fpscr",
        // The existing guard covers all actual bulk write destinations.
        "memcpy", "memmove", "memset",
        "__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8",
        "__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8",
        "__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8",
        "__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8",
        "memcmp", "strlen", "strcmp", "strncmp", "strcasecmp", "rand",
        // Allocation headers, recycled zeroing and successful free CAS notify.
        "malloc", "free", "memalign", "_Znwj", "_Znaj", "_ZdlPv", "_ZdaPv",
        // Host synchronization state; guest outputs are typed MemoryWrite*.
        // In particular, an idle sem/cond wait must not force every draw full.
        "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock",
        "pthread_cond_signal", "pthread_cond_broadcast", "pthread_cond_wait",
        "pthread_cond_timedwait", "pthread_cond_timedwait_relative_np",
        "pthread_join", "pthread_detach", "pthread_once", "pthread_self",
        "pthread_setspecific", "pthread_getspecific", "sem_wait", "sem_trywait",
        "sem_post", "gettimeofday", "clock_gettime", "clock", "sleep", "usleep", "sched_yield",
        // syscall dispatch delegates outputs to typed/native classified paths;
        // accepted futex operations only wait/wake and never modify guest bytes.
        "syscall",
        // OpenSL owns a PCM copy; state outputs are typed, callbacks execute JIT.
        "SLBufferQueue_Enqueue", "SLBufferQueue_GetState", "SLPlay_GetPlayState",
        // These complete scalar implementations modify only JIT registers.
        // modf, scanf and output-pointer math functions stay UNKNOWN.
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "ceil",
        "exp", "log", "log10", "pow", "sqrt", "floor", "rint",
        "sinf", "cosf", "tanf", "asinf", "acosf", "atanf", "atan2f",
        "ceilf", "expf", "floorf", "logf", "roundf", "log10f", "sqrtf",
        "fmod", "fmodf", "__isfinitef", "__isinff",
        "__isinf"
    };
    for (const auto observed_name : observed) {
        if (name == observed_name) return true;
    }
    return false;
}
