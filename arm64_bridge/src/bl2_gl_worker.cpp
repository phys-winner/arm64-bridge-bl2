// GL worker: the GL/EGL calls of BL2's rendering thread run on one host thread.
//
// Measured on Adreno 650 (Outwash, slow view after the camera turn): the guest
// rendering thread is ~97% on-CPU and issues ~8,200 GL calls per frame; about
// half of its time is the Adreno driver and the bridge's GL translation, while
// the GPU is ~41% busy. Unless debug.bl2.gl_worker=0 (read once), the guest thread
// that makes a window surface current hands every GL/EGL thunk to one host
// thread, which owns that context from then on, and keeps running guest code.
//
// Calls without results whose inputs are their arguments run asynchronously
// from a command ring in guest memory: registers, stack arguments and the data
// behind pointer arguments are copied there and the unchanged handlers run on a
// shadow JIT whose SP points at the copy. Every other GL/EGL call waits for the
// worker, which runs its handler on the guest thread's own JIT state; the guest
// thread is blocked meanwhile, so guest memory and registers behave as before.
// Draws stay synchronous while an enabled vertex attribute or the index data
// is client memory (the driver reads it during the draw), tracked from the
// guest's own binding calls after the worker queried the bound state.
//
// Frame boundaries do not drain the queue either: glFenceSync reserves its
// guest handle and is created by the worker in order, glClientWaitSync waits on
// an EGL fence placed right after that GL fence, eglSwapBuffers is queued (one
// frame in flight) and occlusion results polled with GL_QUERY_RESULT_NO_WAIT
// come from results the worker collected. Outwash after the camera turn, thermal
// status 2: 42 -> 60 FPS (the frame limit), the same as before the turn.
//
// Buffer uploads do not drain it either (debug.bl2.gl_worker_buffers=0 keeps
// them synchronous): glBufferData copies its data into the ring, names for
// bl2_fresh_persistent_* come from a cache the worker fills in advance, and a
// write-only map that invalidates its range returns zeroed guest staging
// memory at once (GL leaves an invalidated range undefined until written);
// glUnmapBuffer queues the upload, which the worker replays in order through
// the unchanged map and unmap handlers.
//
// Ring protocol: records are published with a release store of `head`. The
// position the worker sleeps on is announced (a full fence) only every 32
// records, before the guest thread waits for anything and before thunks that
// may block it, so the ~8,000 records of a frame do not each pay a store-load
// fence; the worker returns ring space in batches. Fields written by each side
// sit on separate cache lines.
//
// A thread stays bound until it makes a non-window surface or no context
// current; the worker releases the context first, so the thread runs directly
// afterwards. The ADPF session also covers the guest thread (bl2_perf_hint.cpp).
#include "dynarmic_host.h"
#include "thunk_manager.h"
#include "bl2_gl_worker_wake.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

bool Bl2EglIsWindowSurfaceHandle(uint32_t handle);  // thunks_android.cpp
void Bl2PerfHintSetProducer(pid_t tid);              // bl2_perf_hint.cpp
uint32_t Bl2GuestGlSyncReserve();                    // thunks_gl_gles2_core.cpp
void Bl2GuestGlSyncAttach(uint32_t handle, GLsync sync);
bool Bl2GlWorkerTakeBufferName(uint32_t* name);     // used by bl2_fresh_buffer_names.cpp

namespace {
constexpr const char* kTag = "BL2GlWorker";
constexpr uint32_t kRingBytes = 8u << 20;
constexpr uint32_t kWrap = 0xFFFFFFFFu;
constexpr uint32_t kInternal = 0xFFFFFFFEu;
constexpr uint32_t kMaxIds = 4096;
constexpr unsigned kAttribs = 16;

enum Kind : uint8_t {
    kUnknown = 0, kLocal, kSync, kAsync, kPayload, kDrawElements, kDrawArrays, kBindBuffer,
    kAttribPointer, kEnableAttrib, kDisableAttrib, kBindVertexArray, kDeleteBuffers,
    kMakeCurrent, kReleaseThread, kFenceSync, kClientWaitSync, kDeleteSync, kSwapBuffers,
    kBeginQuery, kEndQuery, kQueryResult, kDeleteQueries, kBufferData, kMapRange, kUnmap,
    kBufferPointer, kFreshName, kBlocking,
};

struct Spec {
    uint8_t kind = kUnknown;
    uint8_t words = 0;       // argument words copied (r0-r3, then the stack)
    uint8_t pointer = 0;     // kPayload: argument word holding the data pointer
    uint8_t unit = 0;        // kPayload: bytes per counted element
    uint8_t count = 0;       // kPayload: argument word holding the element count
    uint32_t max_count = 0;  // kPayload: larger counts run synchronously
};

struct NamedSpec {
    const char* name;
    Spec spec;
};

// Hot calls of the rendering thread (per frame in Outwash: 1,670 draws, 2,000
// attribute pointers, 1,870 local parameter uploads, 1,000 buffer binds) and
// the state calls between them. Handlers audited: no results, no guest writes.
constexpr NamedSpec kSpecs[] = {
    {"glNamedProgramLocalParametersI4uivEXT", {kPayload, 5, 4, 16, 3, 4096}},
    {"glDrawBuffers", {kPayload, 2, 1, 4, 0, 16}},
    {"glDrawElementsBaseVertex", {kDrawElements, 5}},
    {"glDrawElementsBaseVertexOES", {kDrawElements, 5}},
    {"glDrawElementsBaseVertexEXT", {kDrawElements, 5}},
    {"glDrawArrays", {kDrawArrays, 3}},
    {"glDrawElementsInstanced", {kDrawElements, 5}},
    {"glDrawElementsInstancedBaseVertex", {kDrawElements, 6}},
    {"glDrawElementsInstancedBaseVertexOES", {kDrawElements, 6}},
    {"glDrawElementsInstancedBaseVertexEXT", {kDrawElements, 6}},
    // An ignored hint in the bridge (no GL call): runs on the guest thread.
    {"glInvalidateBufferData", {kLocal}},
    {"glVertexAttribPointer", {kAttribPointer, 6}},
    {"glBindBuffer", {kBindBuffer, 2}},
    {"glEnableVertexAttribArray", {kEnableAttrib, 1}},
    {"glDisableVertexAttribArray", {kDisableAttrib, 1}},
    {"glBindVertexArray", {kBindVertexArray, 1}},
    {"glDeleteBuffers", {kDeleteBuffers, 2}},
    {"eglMakeCurrent", {kMakeCurrent, 4}},
    {"eglReleaseThread", {kReleaseThread, 0}},
    {"glFenceSync", {kFenceSync, 2}},
    {"glClientWaitSync", {kClientWaitSync, 4}},
    {"glDeleteSync", {kDeleteSync, 1}},
    {"eglSwapBuffers", {kSwapBuffers, 2}},
    {"glBindProgramARB", {kAsync, 2}},
    {"glBeginQuery", {kBeginQuery, 2}},
    {"glEndQuery", {kEndQuery, 1}},
    {"glGetQueryObjectuiv", {kQueryResult, 3}},
    {"glDeleteQueries", {kDeleteQueries, 2}},
    {"glCullFace", {kAsync, 1}},
    {"glVertexAttribDivisor", {kAsync, 2}},
    {"glDisable", {kAsync, 1}},
    {"glEnable", {kAsync, 1}},
    {"glViewport", {kAsync, 4}},
    {"glBindFramebuffer", {kAsync, 2}},
    {"glReadBuffer", {kAsync, 1}},
    {"glColorMask", {kAsync, 4}},
    {"glScissor", {kAsync, 4}},
    {"glStencilFunc", {kAsync, 3}},
    {"glDepthMask", {kAsync, 1}},
    {"glClear", {kAsync, 1}},
    {"glStencilOp", {kAsync, 3}},
    {"glBlendFuncSeparate", {kAsync, 4}},
    {"glStencilMask", {kAsync, 1}},
    {"glDepthRangef", {kAsync, 2}},
    {"glStencilOpSeparate", {kAsync, 4}},
    {"glStencilFuncSeparate", {kAsync, 4}},
    {"glDepthFunc", {kAsync, 1}},
    {"glBlitFramebuffer", {kAsync, 10}},
    {"glClearStencil", {kAsync, 1}},
    {"glClearColor", {kAsync, 4}},
    {"glPolygonOffset", {kAsync, 2}},
    {"glClearDepthf", {kAsync, 1}},
    {"glBlendFunc", {kAsync, 2}},
    {"glFlush", {kAsync, 0}},
    // Per frame in Outwash: two new vertex buffers, each named, sized, mapped
    // (write | invalidate buffer | unsynchronized), written and unmapped.
    {"glBufferData", {kBufferData, 4, 2, 1, 1, 256u << 10}},
    {"glMapBufferRange", {kMapRange, 4}},
    {"glUnmapBuffer", {kUnmap, 1}},
    {"glGetBufferPointerv", {kBufferPointer, 3}},
    {"bl2_fresh_persistent_vertex_name", {kFreshName}},
    {"bl2_fresh_persistent_index_name", {kFreshName}},
    {"bl2_fresh_persistent_pixel_name", {kFreshName}},
};

struct alignas(16) Record {
    uint32_t bytes;               // whole record, a multiple of 16
    uint32_t id;                  // thunk id, kWrap or kInternal
    const ThunkHandler* handler;  // async and sync thunk records
    Dynarmic::A32::Jit* live;     // sync: the guest thread's JIT; async: null
    uint32_t seq;                 // sync: completion sequence
    uint32_t cpsr;
    uint32_t regs[16];
    uint32_t stack_words;         // copied stack arguments follow the record
    uint32_t op;                  // kInternal: operation
};
static_assert(sizeof(Record) % 16 == 0);

enum InternalOp : uint32_t {
    kOpRelease = 1, kOpQueryVertexState = 2, kOpFenceSync = 3, kOpGenNames = 4, kOpMapUpload = 5,
};

struct VertexState {
    uint32_t vertex_array = 0, array_buffer = 0, element_buffer = 0;
    uint32_t enabled = 0;
    uint32_t buffer[kAttribs] = {};
};

// Producer-side view of the guest's vertex input bindings (bound thread only).
struct Shadow {
    bool valid = false;
    bool vertex_array = false;  // a non-default VAO: draws stay synchronous
    uint32_t array_buffer = 0, element_buffer = 0;
    uint32_t enabled = 0, client = 0;
    uint32_t buffer[kAttribs] = {};
};

constexpr uint32_t kAnnounceEvery = 32;  // records between announcements
constexpr uint32_t kShadowCpsr = 0x10;   // ARM state, user mode
constexpr uint32_t kTailEvery = 64;      // records between returns of ring space
constexpr uint32_t kReturnBytes = 1u << 20;

struct Worker {
    // Set up once.
    DynarmicHost* host = nullptr;
    uint8_t* memory = nullptr;     // host view of guest memory
    uint32_t ring = 0;             // guest address of the ring
    VertexState query;             // kOpQueryVertexState result
    std::atomic<bool> swap_failed{false};
    std::unique_ptr<Dynarmic::A32::Jit> shadow;
    // Guest thread: written per record.
    alignas(128) std::atomic<uint64_t> head{0};  // bytes published by the guest thread
    std::atomic<uint32_t> head_word{0};          // announced position (the worker sleeps on it)
    uint64_t write = 0;            // next free byte (== head after publish)
    uint64_t tail_seen = 0;        // last observed tail (ring space)
    uint32_t unannounced = 0;      // records published since the last announcement
    uint32_t sync_seq = 0;
    uint32_t swaps_issued = 0;
    // Guest thread: written when it waits.
    alignas(128) std::atomic<uint32_t> producer_waiting{0};
    std::atomic<uint64_t> wake_ns{0};     // diagnostics: last FutexWake of a sleeping worker
    // Worker: written when it sleeps or wakes.
    alignas(128) std::atomic<uint32_t> worker_sleeping{0};
    // Worker: ring space and completions.
    alignas(128) std::atomic<uint64_t> tail{0};  // bytes consumed by the worker
    std::atomic<uint32_t> tail_word{0}, done_word{0};
    std::atomic<uint32_t> swaps_done{0};  // swap records the worker finished
    // Worker: diagnostics, written per record and read by the watchdog.
    alignas(128) std::atomic<uint32_t> executing{0};  // id + 1 of the running record, 0 idle
    std::atomic<uint64_t> executed{0};
};

std::atomic<uint8_t> g_kind[kMaxIds];
Spec g_spec[kMaxIds];
std::string g_names[kMaxIds];  // diagnostics
std::mutex g_spec_mutex;
bool g_enabled = false;
Worker* g_worker = nullptr;
std::mutex g_bind_mutex;
std::atomic<bool> g_bound{false};
pthread_t g_bound_thread{};
Shadow g_shadow;  // bound thread only
std::atomic<uint32_t> g_logged_errors{0};
std::atomic<uint32_t> g_missed_wakeups{0};  // diagnostics
std::atomic<uint32_t> g_raced_wakeups{0};   // diagnostics
std::atomic<uint32_t> g_quiet_pickups{0};   // diagnostics

uint64_t NowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
}

// Write-only maps of the bound thread that the worker performs at unmap time.
struct DeferredMap {
    uint32_t buffer = 0, target = 0, offset = 0, length = 0, access = 0, staging = 0;
};
constexpr unsigned kDeferredMaps = 8;
constexpr uint32_t kMaxDeferredMap = 1u << 20;
DeferredMap g_maps[kDeferredMaps];            // bound thread only
const ThunkHandler* g_map_handler = nullptr;  // glMapBufferRange, set on the bound thread
bool g_buffers_enabled = false;               // debug.bl2.gl_worker_buffers, read once

// Buffer names for bl2_fresh_persistent_*: generated by the worker in its
// context, taken by the bound thread (one producer, one consumer).
struct NameCache {
    static constexpr uint32_t kSize = 64;
    uint32_t names[kSize] = {};
    std::atomic<uint32_t> pushed{0}, taken{0};
    std::atomic<bool> refill{false};  // a kOpGenNames record is queued
};
NameCache g_fresh;

// Diagnostics (bound thread): synchronous GL calls per frame, logged every 600 swaps.
struct SyncStats {
    uint32_t frame = 0, max = 0, frames = 0;
    uint64_t total = 0;
    uint16_t by_id[kMaxIds] = {};
};
SyncStats* g_sync_stats = nullptr;

// Occlusion results without a queue drain. UE3 polls with
// GL_QUERY_RESULT_NO_WAIT, which may legitimately report "not ready", so the
// guest thread answers from results the worker collected for the same query
// instance (generation = number of glBeginQuery calls on that name).
// debug.bl2.gl_worker_queries=0 (read once) keeps these calls synchronous.
constexpr uint32_t kQueryNames = 4096;
constexpr GLenum kDesktopSamplesPassed = 0x8914;
constexpr GLenum kQueryResultNoWait = 0x9194;
struct QueryCache {
    std::atomic<uint32_t> generation[kQueryNames];  // instance the value belongs to
    std::atomic<uint32_t> value[kQueryNames];        // translated as the handler does
    uint32_t issued[kQueryNames] = {};               // guest thread: glBeginQuery count
    uint32_t begun[kQueryNames] = {};                // worker: glBeginQuery count
    GLuint active[2] = {};                           // worker: id per target slot
    bool active_desktop[2] = {};
    struct Pending {
        GLuint id;
        uint32_t generation;
        uint32_t swap;  // `swaps` when the query ended
        bool desktop;
    };
    // A frame's queries wait for its swap before they are polled.
    Pending pending[kQueryNames];
    unsigned pending_count = 0;                      // worker only
    uint32_t swaps = 0;                              // worker: successful eglSwapBuffers
};
QueryCache* g_queries = nullptr;
bool g_queries_enabled = false;

// Occlusion targets only; timer and other queries keep the synchronous path.
unsigned QuerySlot(GLenum target) {
    if (target == kDesktopSamplesPassed || target == GL_ANY_SAMPLES_PASSED) return 0;
    return target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE ? 1u : 2u;
}

// The visible-count convention of the glGetQueryObjectuiv handler
// (debug.horn.occlusion_probe, re-read every 32 calls).
int OcclusionProbeMode() {
    static uint32_t calls = 0;  // worker thread only
    static int mode = 0;
    if ((++calls & 31u) == 1u) {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.horn.occlusion_probe", value);
        mode = std::strcmp(value, "log") == 0 ? 1 : std::strcmp(value, "visible") == 0 ? 2
             : std::strcmp(value, "count65535") == 0 ? 3 : std::strcmp(value, "count262144") == 0 ? 4
             : std::strcmp(value, "count1048576") == 0 ? 5 : 0;
    }
    return mode;
}

// Worker: results that became available since the last poll.
//
// Only queries that ended before the last executed eglSwapBuffers are asked
// about, as the guest itself only reads queries of earlier frames. Mali-G715
// (r54p3, Pixel 10a) crashes in gles_fb_object_flush_graphics_nx when asked
// about a query whose framebuffer has not been flushed yet while the default
// framebuffer has draws pending; the swap flushes every pending frame. A name
// begun again since its entry was recorded is skipped too: GL discarded that
// result, and the name now belongs to the current frame (or is active).
void PollQueries() {
    QueryCache* q = g_queries;
    if (!q || !q->pending_count) return;
    // Results complete in submission order: stop at the first one still pending.
    unsigned done = 0;
    for (; done < q->pending_count; ++done) {
        const auto pending = q->pending[done];
        if (pending.swap == q->swaps) break;  // This frame's queries, and all after them.
        if (q->begun[pending.id] != pending.generation) continue;
        GLuint available = GL_FALSE;
        glGetQueryObjectuiv(pending.id, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) break;
        GLuint native = 0;
        glGetQueryObjectuiv(pending.id, GL_QUERY_RESULT, &native);
        uint32_t value = native;
        if (pending.desktop && native != 0) {
            const int mode = OcclusionProbeMode();
            value = mode == 3 ? 65535u : mode == 4 ? 262144u : mode == 5 ? 1048576u : 0xFFFFFFFEu;
        }
        q->value[pending.id].store(value, std::memory_order_relaxed);
        q->generation[pending.id].store(pending.generation, std::memory_order_release);
    }
    if (!done) return;
    for (unsigned i = done; i < q->pending_count; ++i) q->pending[i - done] = q->pending[i];
    q->pending_count -= done;
}

// Worker, after glDeleteQueries: deleted names are no longer polled (a query
// on a deleted name would leave GL_INVALID_OPERATION for the guest).
void ForgetQueries(DynarmicHost& host, uint32_t count, uint32_t names) {
    QueryCache* q = g_queries;
    if (!q || !q->pending_count || static_cast<int32_t>(count) <= 0 || count > 65536 ||
        uint64_t(names) + 4ull * count > host.GetGuestMemorySize())
        return;
    unsigned kept = 0;
    for (unsigned i = 0; i < q->pending_count; ++i) {
        bool deleted = false;
        for (uint32_t n = 0; n < count && !deleted; ++n) deleted = host.MemoryRead32(names + 4 * n) == q->pending[i].id;
        if (!deleted) q->pending[kept++] = q->pending[i];
    }
    q->pending_count = kept;
}

// A guest GLsync handle -> an EGL fence inserted right after its GL fence, so
// the guest thread can wait for it without a current context (no queue drain).
struct EglFences {
    PFNEGLCREATESYNCKHRPROC create = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroy = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC wait = nullptr;
    PFNEGLGETSYNCATTRIBKHRPROC attrib = nullptr;
    std::mutex mutex;
    std::unordered_map<uint32_t, std::pair<EGLDisplay, EGLSyncKHR>> by_handle;
    bool Ready() const { return create && destroy && wait && attrib; }
};
EglFences g_fences;

void LoadEglFences() {
    g_fences.create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    g_fences.destroy = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    g_fences.wait = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    g_fences.attrib = reinterpret_cast<PFNEGLGETSYNCATTRIBKHRPROC>(eglGetProcAddress("eglGetSyncAttribKHR"));
}

// Worker: after the guest's glFenceSync handler returned `handle` in r0.
void AddEglFence(uint32_t handle) {
    if (!handle || !g_fences.Ready()) return;
    const EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) return;
    const EGLSyncKHR sync = g_fences.create(display, EGL_SYNC_FENCE_KHR, nullptr);
    if (sync == EGL_NO_SYNC_KHR) return;
    glFlush();  // Both fences reach the GPU even if the queue then idles.
    std::lock_guard lock(g_fences.mutex);
    auto& slot = g_fences.by_handle[handle];
    if (slot.second != EGL_NO_SYNC_KHR) g_fences.destroy(slot.first, slot.second);
    slot = {display, sync};
}

// Worker: after the guest's glDeleteSync handler.
void RemoveEglFence(uint32_t handle) {
    std::lock_guard lock(g_fences.mutex);
    const auto found = g_fences.by_handle.find(handle);
    if (found == g_fences.by_handle.end()) return;
    g_fences.destroy(found->second.first, found->second.second);
    g_fences.by_handle.erase(found);
}

// Guest thread: glClientWaitSync on a fence the worker already placed.
bool WaitEglFence(uint32_t handle, uint64_t timeout, uint32_t& result) {
    EGLDisplay display;
    EGLSyncKHR sync;
    {
        std::lock_guard lock(g_fences.mutex);
        const auto found = g_fences.by_handle.find(handle);
        if (found == g_fences.by_handle.end()) return false;
        display = found->second.first;
        sync = found->second.second;
        EGLint status = 0;
        if (g_fences.attrib(display, sync, EGL_SYNC_STATUS_KHR, &status) && status == EGL_SIGNALED_KHR) {
            result = GL_ALREADY_SIGNALED;
            return true;
        }
    }
    // The entry lives until the guest's own glDeleteSync, which follows this wait.
    const EGLint waited = g_fences.wait(display, sync, 0, timeout);
    result = waited == EGL_CONDITION_SATISFIED_KHR ? GL_CONDITION_SATISFIED
           : waited == EGL_TIMEOUT_EXPIRED_KHR ? GL_TIMEOUT_EXPIRED : GL_WAIT_FAILED;
    return true;
}

void FutexWait(std::atomic<uint32_t>* word, uint32_t expected, const timespec* timeout = nullptr) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT_PRIVATE, expected, timeout, nullptr, 0);
}

long FutexWake(std::atomic<uint32_t>* word) {
    return syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

inline void CpuRelax() { asm volatile("yield" ::: "memory"); }

bool ReadEnabled() {
    char value[PROP_VALUE_MAX] = {};
    return !(__system_property_get("debug.bl2.gl_worker", value) > 0 && value[0] == '0');
}

// Thunks that may put the guest thread to sleep: pending records are announced first.
constexpr const char* kBlockingThunks[] = {
    "pthread_cond_wait", "pthread_cond_timedwait", "pthread_mutex_lock", "pthread_join", "pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock", "sem_wait", "sem_timedwait", "sched_yield", "usleep", "nanosleep", "sleep", "poll",
    "select", "epoll_wait", "read", "recvfrom", "syscall", "futex", "__futex_wait",
};

Spec Classify(const std::string& name) {
    for (const auto& entry : kSpecs)
        if (name == entry.name) return entry.spec;
    for (const char* blocking : kBlockingThunks)
        if (name == blocking) return Spec{kBlocking};
    Spec spec;
    const bool gl = name.rfind("gl", 0) == 0 || name.rfind("egl", 0) == 0 ||
                    name.rfind("bl2_fresh_persistent_", 0) == 0;
    spec.kind = gl ? kSync : kLocal;
    return spec;
}

const Spec& SpecFor(uint32_t id, const std::string& name) {
    static const Spec local{kLocal};
    if (id >= kMaxIds) return local;
    if (g_kind[id].load(std::memory_order_acquire) == kUnknown) {
        std::lock_guard lock(g_spec_mutex);
        if (g_kind[id].load(std::memory_order_relaxed) == kUnknown) {
            g_spec[id] = Classify(name);
            g_names[id] = name;
            g_kind[id].store(g_spec[id].kind, std::memory_order_release);
        }
    }
    return g_spec[id];
}

void LogHandlerError(const char* what, uint32_t id) {
    if (g_logged_errors.fetch_add(1, std::memory_order_relaxed) < 16)
        __android_log_print(ANDROID_LOG_ERROR, kTag, "handler id=%u threw: %s", id, what);
}

// ---- worker thread ----

void CallHandler(const ThunkHandler& handler, Dynarmic::A32::Jit& jit, DynarmicHost& host, uint32_t id) {
    try {
        handler(jit, host);
    } catch (const std::exception& error) {
        LogHandlerError(error.what(), id);
    } catch (...) {
        LogHandlerError("unknown", id);
    }
}

void RunInternal(Worker& w, Record& record, uint32_t guest_address) {
    if (record.op == kOpRelease) {
        if (g_queries) g_queries->pending_count = 0;  // Query names belong to the released context.
        const EGLDisplay display = eglGetCurrentDisplay();
        if (display != EGL_NO_DISPLAY && eglGetCurrentContext() != EGL_NO_CONTEXT)
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else if (record.op == kOpQueryVertexState) {
        VertexState state;
        GLint value = 0;
        if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &value);
            state.vertex_array = static_cast<uint32_t>(value);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &value);
            state.array_buffer = static_cast<uint32_t>(value);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &value);
            state.element_buffer = static_cast<uint32_t>(value);
            GLint attribs = 0;
            glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attribs);
            for (unsigned i = 0; i < kAttribs && i < static_cast<unsigned>(attribs); ++i) {
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &value);
                if (value) state.enabled |= 1u << i;
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &value);
                state.buffer[i] = static_cast<uint32_t>(value);
            }
        }
        w.query = state;
    } else if (record.op == kOpFenceSync) {
        // regs: condition, flags, the guest handle reserved by the guest thread.
        const GLsync sync = eglGetCurrentContext() != EGL_NO_CONTEXT
            ? glFenceSync(record.regs[0], record.regs[1]) : nullptr;
        Bl2GuestGlSyncAttach(record.regs[2], sync);
        if (sync) AddEglFence(record.regs[2]);
    } else if (record.op == kOpGenNames) {
        NameCache& cache = g_fresh;
        uint32_t pushed = cache.pushed.load(std::memory_order_relaxed);
        const uint32_t room = NameCache::kSize - (pushed - cache.taken.load(std::memory_order_acquire));
        if (room && eglGetCurrentContext() != EGL_NO_CONTEXT) {
            GLuint names[NameCache::kSize] = {};
            glGenBuffers(static_cast<GLsizei>(room), names);
            for (uint32_t i = 0; i < room; ++i)
                if (names[i]) cache.names[pushed++ % NameCache::kSize] = names[i];
            cache.pushed.store(pushed, std::memory_order_release);
        }
        cache.refill.store(false, std::memory_order_release);
    } else if (record.op == kOpMapUpload) {
        // regs: target, offset, length, access, staging; handler: glUnmapBuffer. The
        // bridge's own map handler fills its staging, which receives the guest's bytes.
        auto& regs = w.shadow->Regs();
        const uint32_t target = record.regs[0], length = record.regs[2], staging = record.regs[4];
        for (unsigned i = 0; i < 4; ++i) regs[i] = record.regs[i];
        regs[13] = guest_address + sizeof(Record);
        bool uploaded = false;
        if (g_map_handler && record.handler) {
            CallHandler(*g_map_handler, *w.shadow, *w.host, kMaxIds);
            const uint32_t mapped = regs[0];
            if (mapped && uint64_t(mapped) + length <= w.host->GetGuestMemorySize()) {
                std::memcpy(w.memory + mapped, w.memory + staging, length);
                regs[0] = target;
                CallHandler(*record.handler, *w.shadow, *w.host, kMaxIds);
                uploaded = regs[0] == GL_TRUE;
            }
        }
        static std::atomic<uint32_t> failures{0};
        if (!uploaded && failures.fetch_add(1, std::memory_order_relaxed) < 16)
            __android_log_print(ANDROID_LOG_WARN, kTag, "deferred map upload failed: target=%04X offset=%u length=%u access=%04X",
                                target, record.regs[1], length, record.regs[3]);
        w.host->GuestFree(staging);
    }
}

void Execute(Worker& w, Record& record, uint32_t guest_address) {
    if (record.id == kInternal) {
        RunInternal(w, record, guest_address);
    } else if (record.live) {
        const uint32_t argument = record.live->Regs()[0];
        const uint32_t argument1 = record.live->Regs()[1];
        CallHandler(*record.handler, *record.live, *w.host, record.id);
        if (record.id < kMaxIds) {
            const uint8_t kind = g_kind[record.id].load(std::memory_order_relaxed);
            if (kind == kFenceSync) AddEglFence(record.live->Regs()[0]);
            else if (kind == kDeleteSync) RemoveEglFence(argument);
            else if (kind == kDeleteQueries) ForgetQueries(*w.host, argument, argument1);
            else if ((kind == kMakeCurrent || kind == kReleaseThread) && g_queries) g_queries->pending_count = 0;
            else if (kind == kSwapBuffers && g_queries && record.live->Regs()[0] == EGL_TRUE) {
                ++g_queries->swaps;
                PollQueries();
            }
        }
    } else {
        auto& regs = w.shadow->Regs();
        for (unsigned i = 0; i < 16; ++i) regs[i] = record.regs[i];
        regs[13] = guest_address + sizeof(Record);
        CallHandler(*record.handler, *w.shadow, *w.host, record.id);
        if (record.id < kMaxIds) {
            const uint8_t kind = g_kind[record.id].load(std::memory_order_relaxed);
            if (kind == kDeleteSync) {
                RemoveEglFence(record.regs[0]);
            } else if (kind == kBeginQuery && g_queries) {
                const GLuint id = record.regs[1];
                const unsigned slot = QuerySlot(record.regs[0]);
                if (id < kQueryNames && slot < 2) {
                    g_queries->active[slot] = id;
                    g_queries->active_desktop[slot] = record.regs[0] == kDesktopSamplesPassed;
                    ++g_queries->begun[id];
                }
            } else if (kind == kEndQuery && g_queries) {
                const unsigned slot = QuerySlot(record.regs[0]);
                const GLuint id = slot < 2 ? g_queries->active[slot] : 0;
                if (id && id < kQueryNames && g_queries->pending_count < kQueryNames)
                    g_queries->pending[g_queries->pending_count++] = {id, g_queries->begun[id], g_queries->swaps,
                                                                      g_queries->active_desktop[slot]};
                if (slot < 2) g_queries->active[slot] = 0;
            } else if (kind == kSwapBuffers) {
                if (regs[0] != EGL_TRUE) {
                    w.swap_failed.store(true, std::memory_order_relaxed);
                    __android_log_print(ANDROID_LOG_WARN, kTag, "queued eglSwapBuffers failed: %u", regs[0]);
                } else if (g_queries) {
                    ++g_queries->swaps;  // Queries ended before it may now be polled.
                }
                w.swaps_done.fetch_add(1, std::memory_order_seq_cst);
                if (w.producer_waiting.load(std::memory_order_seq_cst)) FutexWake(&w.swaps_done);
                if (regs[0] == EGL_TRUE) PollQueries();  // After the wake: the guest paces on swaps.
            }
        }
    }
    if (record.live || (record.id == kInternal && record.seq)) {
        w.done_word.store(record.seq, std::memory_order_seq_cst);
        if (w.producer_waiting.load(std::memory_order_seq_cst)) FutexWake(&w.done_word);
    }
}

// Worker: publishes consumed ring space and wakes a guest thread waiting for it.
void ReturnSpace(Worker& w, uint64_t tail) {
    w.tail.store(tail, std::memory_order_seq_cst);
    w.tail_word.store(static_cast<uint32_t>(tail), std::memory_order_seq_cst);
    if (w.producer_waiting.load(std::memory_order_seq_cst)) FutexWake(&w.tail_word);
}

void WorkerMain(Worker* worker) {
    Worker& w = *worker;
    pthread_setname_np(pthread_self(), "BL2GlWorker");
    __android_log_print(ANDROID_LOG_INFO, kTag, "worker thread tid=%d", gettid());
    // Never runs guest code: async handlers only read their copied registers.
    Dynarmic::A32::UserConfig config;
    config.callbacks = w.host;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.code_cache_size = 4u << 20;
    w.shadow = std::make_unique<Dynarmic::A32::Jit>(config);
    w.shadow->SetCpsr(kShadowCpsr);
    // Ring space goes back every kTailEvery records or kReturnBytes, before the
    // worker sleeps, and at once to a guest thread waiting for it.
    uint64_t tail = 0, returned = 0, executed = 0;
    uint32_t unreturned = 0;
    const auto give_back = [&] {
        ReturnSpace(w, tail);
        returned = tail;
        unreturned = 0;
    };
    for (;;) {
        uint64_t head = w.head.load(std::memory_order_acquire);
        if (head == tail) {
            PollQueries();
            for (unsigned spin = 0; spin < 4000 && head == tail; ++spin) {
                CpuRelax();
                head = w.head.load(std::memory_order_acquire);
                if (returned != tail && w.producer_waiting.load(std::memory_order_relaxed)) give_back();
            }
            if (head == tail) {
                if (returned != tail) give_back();
                // Pairs with Announce: if this load misses an announcement, Announce
                // claims Armed -> WakePending and wakes the futex below. An announced
                // head_word synchronizes with the guest thread, so `head` is current.
                w.worker_sleeping.store(bl2::gl_worker::kArmed, std::memory_order_seq_cst);
                const uint32_t announced = w.head_word.load(std::memory_order_seq_cst);
                if (w.head.load(std::memory_order_acquire) == tail) {
                    // Bounded: work published after the last announcement waits at most 50 ms.
                    const uint64_t slept_at = NowNs();
                    const timespec timeout{0, 50000000};
                    const long slept = syscall(SYS_futex, reinterpret_cast<uint32_t*>(&w.head_word),
                                               FUTEX_WAIT_PRIVATE, announced, &timeout, nullptr, 0);
                    if (slept != 0 && errno == ETIMEDOUT && w.head.load(std::memory_order_seq_cst) != tail) {
                        const uint64_t now = NowNs(), woke = w.wake_ns.load(std::memory_order_relaxed);
                        if (w.head_word.load(std::memory_order_seq_cst) == announced) {
                            // Published but never announced: the guest thread went quiet
                            // without waiting on the worker. Expected to be rare.
                            if (g_quiet_pickups.fetch_add(1, std::memory_order_relaxed) < 4)
                                __android_log_print(ANDROID_LOG_INFO, kTag, "picked up unannounced records after %.1f ms",
                                                    (now - slept_at) / 1e6);
                        } else if (woke >= slept_at && now - woke < 2000000ull) {
                            // The wake-up raced with the timeout: the work is seen at once.
                            if (g_raced_wakeups.fetch_add(1, std::memory_order_relaxed) < 4)
                                __android_log_print(ANDROID_LOG_INFO, kTag,
                                                    "idle timeout raced with a wake-up %.3f ms earlier (harmless)",
                                                    (now - woke) / 1e6);
                        } else if (g_missed_wakeups.fetch_add(1, std::memory_order_relaxed) < 16) {
                            __android_log_print(ANDROID_LOG_WARN, kTag,
                                                "missed wake-up: announced while asleep, last wake %.1f ms ago, slept %.1f ms",
                                                woke ? (now - woke) / 1e6 : -1.0, (now - slept_at) / 1e6);
                        }
                    }
                }
                w.worker_sleeping.store(bl2::gl_worker::kAwake, std::memory_order_relaxed);
                continue;
            }
        }
        while (tail != head) {
            if ((executed & 255u) == 0) PollQueries();
            const uint32_t offset = static_cast<uint32_t>(tail % kRingBytes);
            auto* record = reinterpret_cast<Record*>(w.memory + w.ring + offset);
            const uint32_t bytes = record->bytes;
            if (record->id != kWrap) {
                w.executing.store(record->id + 1, std::memory_order_relaxed);
                Execute(w, *record, w.ring + offset);
                w.executing.store(0, std::memory_order_relaxed);
                w.executed.store(++executed, std::memory_order_relaxed);
            }
            tail += bytes;
            if (++unreturned >= kTailEvery || tail - returned >= kReturnBytes ||
                w.producer_waiting.load(std::memory_order_relaxed))
                give_back();
        }
    }
}

// ---- guest thread (producer) ----

// Makes everything published so far visible to a sleeping worker: the
// sequentially consistent store of head_word followed by the load of
// worker_sleeping pairs with the worker's store of worker_sleeping followed by
// its load of head_word, so at least one side sees the other.
void Announce(Worker& w) {
    w.unannounced = 0;
    w.head_word.store(static_cast<uint32_t>(w.write), std::memory_order_seq_cst);
    if (bl2::gl_worker::ClaimWake(w.worker_sleeping)) {
        w.wake_ns.store(NowNs(), std::memory_order_relaxed);
        bl2::gl_worker::CompleteWake(w.worker_sleeping, FutexWake(&w.head_word));
    }
}

// Waits until `ready()` holds: a short spin, then futex sleeps on `word`.
template <class Ready>
void ProducerWait(Worker& w, std::atomic<uint32_t>& word, Ready ready, uint32_t waiting_id) {
    if (w.unannounced) Announce(w);
    for (unsigned spin = 0; spin < 20000; ++spin) {
        if (ready()) return;
        CpuRelax();
    }
    timespec began{};
    clock_gettime(CLOCK_MONOTONIC, &began);
    unsigned reports = 0;
    while (!ready()) {
        const uint32_t observed = word.load(std::memory_order_seq_cst);
        w.producer_waiting.store(1, std::memory_order_seq_cst);
        const timespec timeout{1, 0};
        if (!ready()) FutexWait(&word, observed, &timeout);
        w.producer_waiting.store(0, std::memory_order_relaxed);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double waited = (now.tv_sec - began.tv_sec) + (now.tv_nsec - began.tv_nsec) / 1e9;
        if (waited >= 0.9 && w.worker_sleeping.load(std::memory_order_seq_cst) &&
            w.head.load(std::memory_order_seq_cst) != w.tail.load(std::memory_order_seq_cst)) {
            FutexWake(&w.head_word);
            if (g_missed_wakeups.fetch_add(1, std::memory_order_relaxed) < 16)
                __android_log_print(ANDROID_LOG_WARN, kTag, "guest woke a sleeping worker with queued work");
        } else if (ready() && waited >= 0.9 && g_missed_wakeups.fetch_add(1, std::memory_order_relaxed) < 16) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "guest wait for %s ended by timeout (missed wake-up)",
                                waiting_id < kMaxIds ? g_names[waiting_id].c_str() : "space/internal");
        }
        if (!ready() && waited >= 2.0 * (reports + 1) && reports < 8) {
            ++reports;
            const uint32_t running = w.executing.load(std::memory_order_relaxed);
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "guest waits %.1fs for %s: worker runs %s, executed=%llu head=%llu tail=%llu sleeping=%u",
                waited, waiting_id < kMaxIds ? g_names[waiting_id].c_str() : "space/internal",
                running ? (running - 1 < kMaxIds ? g_names[running - 1].c_str() : "internal") : "nothing",
                static_cast<unsigned long long>(w.executed.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(w.head.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(w.tail.load(std::memory_order_relaxed)),
                w.worker_sleeping.load(std::memory_order_relaxed));
        }
    }
}

// Returns space for `bytes` (a multiple of 16) at the producer position.
Record* Reserve(Worker& w, uint32_t bytes) {
    uint32_t offset = static_cast<uint32_t>(w.write % kRingBytes);
    const uint32_t before_end = kRingBytes - offset;
    const uint32_t needed = bytes > before_end ? bytes + before_end : bytes;
    if (w.write + needed - w.tail_seen > kRingBytes) {
        ProducerWait(w, w.tail_word, [&] {
            w.tail_seen = w.tail.load(std::memory_order_seq_cst);
            return w.write + needed - w.tail_seen <= kRingBytes;
        }, kMaxIds);
    }
    // The ring is larger than the caches: start owning the next lines early.
    __builtin_prefetch(w.memory + w.ring + ((offset + 256u) & (kRingBytes - 1u)), 1, 3);
    if (bytes > before_end) {
        auto* wrap = reinterpret_cast<Record*>(w.memory + w.ring + offset);
        wrap->bytes = before_end;
        wrap->id = kWrap;
        w.write += before_end;
        offset = 0;
    }
    return reinterpret_cast<Record*>(w.memory + w.ring + offset);
}

// A worker that is awake sees `head` on its own; one that went to sleep is
// woken by the next announcement (`announce`: now, for records the guest thread
// will wait for). A plain look at worker_sleeping catches most sleeps early.
void Publish(Worker& w, uint32_t bytes, bool announce = false) {
    w.write += bytes;
    w.head.store(w.write, std::memory_order_release);
    if (announce || ++w.unannounced >= kAnnounceEvery ||
        bl2::gl_worker::NeedsAnnouncement(w.worker_sleeping.load(std::memory_order_relaxed)))
        Announce(w);
}

void WaitDone(Worker& w, uint32_t seq, uint32_t id) {
    ProducerWait(w, w.done_word, [&] { return w.done_word.load(std::memory_order_seq_cst) == seq; }, id);
}

void RunSync(Worker& w, uint32_t id, const ThunkHandler& handler, Dynarmic::A32::Jit& jit) {
    auto* record = Reserve(w, sizeof(Record));
    record->bytes = sizeof(Record);
    record->id = id;
    record->handler = &handler;
    record->live = &jit;
    record->seq = ++w.sync_seq;
    record->stack_words = 0;
    record->op = 0;
    Publish(w, sizeof(Record), true);
    WaitDone(w, w.sync_seq, id);
}

void RunInternalSync(Worker& w, uint32_t op) {
    auto* record = Reserve(w, sizeof(Record));
    record->bytes = sizeof(Record);
    record->id = kInternal;
    record->handler = nullptr;
    record->live = nullptr;
    record->seq = ++w.sync_seq;
    record->stack_words = 0;
    record->op = op;
    Publish(w, sizeof(Record), true);
    WaitDone(w, w.sync_seq, kMaxIds);
}

void RunInternalAsync(Worker& w, uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2, bool announce = false) {
    auto* record = Reserve(w, sizeof(Record));
    record->bytes = sizeof(Record);
    record->id = kInternal;
    record->handler = nullptr;
    record->live = nullptr;
    record->seq = 0;
    record->regs[0] = a0;
    record->regs[1] = a1;
    record->regs[2] = a2;
    record->stack_words = 0;
    record->op = op;
    Publish(w, sizeof(Record), announce);
}

uint32_t ArgWord(Dynarmic::A32::Jit& jit, DynarmicHost& host, unsigned index) {
    if (index < 4) return jit.Regs()[index];
    const uint64_t address = uint64_t(jit.Regs()[13]) + 4ull * (index - 4);
    if (address + 4 > host.GetGuestMemorySize()) return 0;
    return host.MemoryRead32(static_cast<uint32_t>(address));
}

// Async record: registers, stack arguments and an optional payload copied for
// the handler. Returns false (run synchronously) when the inputs are unusable.
bool RunAsync(Worker& w, uint32_t id, const ThunkHandler& handler, Dynarmic::A32::Jit& jit,
              DynarmicHost& host, const Spec& spec, bool announce = false) {
    const auto& regs = jit.Regs();
    const uint32_t stack_words = spec.words > 4 ? spec.words - 4u : 0u;
    if (stack_words && uint64_t(regs[13]) + 4ull * stack_words > host.GetGuestMemorySize()) return false;
    uint32_t payload_address = 0, payload_bytes = 0;
    if (spec.kind == kPayload || spec.kind == kBufferData) {
        const uint32_t count = ArgWord(jit, host, spec.count);
        payload_address = ArgWord(jit, host, spec.pointer);
        // glBufferData(NULL) only allocates: no payload.
        if (spec.kind != kBufferData || payload_address) {
            if (static_cast<int32_t>(count) <= 0 || count > spec.max_count) return false;
            payload_bytes = count * spec.unit;
            if (payload_address < 0x10000 ||
                uint64_t(payload_address) + payload_bytes > host.GetGuestMemorySize()) return false;
        }
    }
    const uint32_t bytes = (sizeof(Record) + 4 * stack_words + payload_bytes + 15u) & ~15u;
    auto* record = Reserve(w, bytes);
    const uint32_t guest = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(record) - w.memory);
    record->bytes = bytes;
    record->id = id;
    record->handler = &handler;
    record->live = nullptr;
    record->seq = 0;
    // Thunks are called from ARM code in user mode; the shadow JIT keeps that CPSR.
    record->cpsr = kShadowCpsr;
    std::memcpy(record->regs, regs.data(), sizeof(record->regs));
    record->stack_words = stack_words;
    record->op = 0;
    auto* stack = reinterpret_cast<uint32_t*>(record + 1);
    if (stack_words) std::memcpy(stack, w.memory + regs[13], 4 * stack_words);
    if (payload_bytes) {
        const uint32_t copy = guest + sizeof(Record) + 4 * stack_words;
        std::memcpy(w.memory + copy, w.memory + payload_address, payload_bytes);
        if (spec.pointer < 4) record->regs[spec.pointer] = copy;
        else stack[spec.pointer - 4] = copy;
    }
    Publish(w, bytes, announce);
    return true;
}

void LoadShadow(const VertexState& state) {
    g_shadow = Shadow{};
    g_shadow.valid = true;
    g_shadow.vertex_array = state.vertex_array != 0;
    g_shadow.array_buffer = state.array_buffer;
    g_shadow.element_buffer = state.element_buffer;
    g_shadow.enabled = state.enabled;
    for (unsigned i = 0; i < kAttribs; ++i) {
        g_shadow.buffer[i] = state.buffer[i];
        if (!state.buffer[i]) g_shadow.client |= 1u << i;
    }
}

bool DrawsReadGuestMemory(bool indexed) {
    if (!g_shadow.valid || g_shadow.vertex_array) return true;
    if (indexed && !g_shadow.element_buffer) return true;
    return (g_shadow.enabled & g_shadow.client) != 0;
}

void UpdateShadow(const Spec& spec, Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    if (!g_shadow.valid) return;
    const auto& r = jit.Regs();
    switch (spec.kind) {
    case kBindBuffer:
        if (r[0] == GL_ARRAY_BUFFER) g_shadow.array_buffer = r[1];
        else if (r[0] == GL_ELEMENT_ARRAY_BUFFER && !g_shadow.vertex_array) g_shadow.element_buffer = r[1];
        break;
    case kAttribPointer:
        if (r[0] < kAttribs && !g_shadow.vertex_array) {
            g_shadow.buffer[r[0]] = g_shadow.array_buffer;
            if (g_shadow.array_buffer) g_shadow.client &= ~(1u << r[0]);
            else g_shadow.client |= 1u << r[0];
        }
        break;
    case kEnableAttrib:
        if (r[0] < kAttribs && !g_shadow.vertex_array) g_shadow.enabled |= 1u << r[0];
        break;
    case kDisableAttrib:
        if (r[0] < kAttribs && !g_shadow.vertex_array) g_shadow.enabled &= ~(1u << r[0]);
        break;
    case kBindVertexArray:
        g_shadow.vertex_array = r[0] != 0;
        break;
    case kDeleteBuffers: {
        // Deleting a bound buffer unbinds it; attributes that used it read client memory.
        const int32_t count = static_cast<int32_t>(r[0]);
        const uint32_t names = r[1];
        if (count <= 0 || count > 65536 || uint64_t(names) + 4ull * count > host.GetGuestMemorySize()) {
            g_shadow.valid = false;
            break;
        }
        for (int32_t i = 0; i < count; ++i) {
            const uint32_t name = host.MemoryRead32(names + 4 * i);
            if (!name) continue;
            if (g_shadow.array_buffer == name) g_shadow.array_buffer = 0;
            if (g_shadow.element_buffer == name && !g_shadow.vertex_array) g_shadow.element_buffer = 0;
            for (unsigned a = 0; a < kAttribs; ++a)
                if (g_shadow.buffer[a] == name && !g_shadow.vertex_array) {
                    g_shadow.buffer[a] = 0;
                    g_shadow.client |= 1u << a;
                }
        }
        break;
    }
    default:
        break;
    }
}

// The buffer the guest's binding calls left on `target` (0: unknown or none).
uint32_t ShadowBuffer(uint32_t target) {
    if (!g_shadow.valid) return 0;
    if (target == GL_ARRAY_BUFFER) return g_shadow.array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER && !g_shadow.vertex_array) return g_shadow.element_buffer;
    return 0;
}

DeferredMap* FindDeferredMap(uint32_t buffer) {
    if (!buffer) return nullptr;
    for (auto& map : g_maps)
        if (map.buffer == buffer) return &map;
    return nullptr;
}

// glBufferData and glDeleteBuffers end a mapping; so do context changes.
void DropDeferredMap(DynarmicHost& host, uint32_t buffer) {
    if (DeferredMap* map = FindDeferredMap(buffer)) {
        host.GuestFree(map->staging);
        *map = DeferredMap{};
    }
}

void DropDeferredMaps(DynarmicHost& host) {
    for (auto& map : g_maps)
        if (map.buffer) DropDeferredMap(host, map.buffer);
}

// glMapBufferRange: write-only, invalidating, not persistent, at most 1 MiB.
bool MapDeferred(Worker& w, const ThunkHandler& handler, Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    g_map_handler = &handler;
    const uint32_t target = jit.Regs()[0], access = jit.Regs()[3];
    const int32_t offset = static_cast<int32_t>(jit.Regs()[1]);
    const int32_t length = static_cast<int32_t>(jit.Regs()[2]);
    constexpr uint32_t kRefused = GL_MAP_READ_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | 0x0040u /* persistent */ |
                                  0x0080u /* coherent */;
    const uint32_t buffer = ShadowBuffer(target);
    if (!g_buffers_enabled || !buffer || offset < 0 || length <= 0 || uint32_t(length) > kMaxDeferredMap ||
        !(access & GL_MAP_WRITE_BIT) || (access & kRefused) ||
        !(access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)) || FindDeferredMap(buffer))
        return false;
    DeferredMap* slot = nullptr;
    for (auto& map : g_maps)
        if (!map.buffer) {
            slot = &map;
            break;
        }
    if (!slot) return false;
    const uint32_t staging = host.GuestAlloc(static_cast<size_t>(length), 64);
    if (!staging) return false;
    std::memset(w.memory + staging, 0, static_cast<size_t>(length));
    *slot = {buffer, target, static_cast<uint32_t>(offset), static_cast<uint32_t>(length), access, staging};
    jit.Regs()[0] = staging;
    return true;
}

// glUnmapBuffer of a deferred map: the worker maps, copies and unmaps in order.
bool UnmapDeferred(Worker& w, const ThunkHandler& handler, Dynarmic::A32::Jit& jit) {
    const uint32_t target = jit.Regs()[0];
    DeferredMap* map = FindDeferredMap(ShadowBuffer(target));
    if (!map || map->target != target) return false;
    auto* record = Reserve(w, sizeof(Record));
    record->bytes = sizeof(Record);
    record->id = kInternal;
    record->handler = &handler;
    record->live = nullptr;
    record->seq = 0;
    record->cpsr = kShadowCpsr;
    record->regs[0] = map->target;
    record->regs[1] = map->offset;
    record->regs[2] = map->length;
    record->regs[3] = map->access;
    record->regs[4] = map->staging;
    record->stack_words = 0;
    record->op = kOpMapUpload;
    Publish(w, sizeof(Record));
    *map = DeferredMap{};
    jit.Regs()[0] = GL_TRUE;
    return true;
}

uint32_t CachedNames() {
    return g_fresh.pushed.load(std::memory_order_acquire) - g_fresh.taken.load(std::memory_order_relaxed);
}

// Keeps names ready: a refill is queued once fewer than a quarter remain.
void RequestNames(Worker& w) {
    if (CachedNames() >= NameCache::kSize / 4 || g_fresh.refill.exchange(true, std::memory_order_acq_rel)) return;
    RunInternalAsync(w, kOpGenNames, 0, 0, 0);
}

// Context changes: cached names belong to the old context (no refill is in flight
// after a synchronous call returned).
void DiscardNames() {
    g_fresh.taken.store(g_fresh.pushed.load(std::memory_order_acquire), std::memory_order_release);
}

void NoteSyncCall(uint32_t id) {
    if (!g_sync_stats) return;
    ++g_sync_stats->frame;
    if (id < kMaxIds && g_sync_stats->by_id[id] < 0xFFFF) ++g_sync_stats->by_id[id];
}

void NoteFrame() {
    SyncStats* s = g_sync_stats;
    if (!s) return;
    s->total += s->frame;
    if (s->frame > s->max) s->max = s->frame;
    s->frame = 0;
    if (++s->frames < 600) return;
    uint32_t top[3] = {};
    for (uint32_t id = 1; id < kMaxIds; ++id) {
        if (s->by_id[id] > s->by_id[top[2]]) {
            top[2] = id;
            for (int i = 2; i > 0 && s->by_id[top[i]] > s->by_id[top[i - 1]]; --i) std::swap(top[i], top[i - 1]);
        }
    }
    char names[192] = {};
    size_t used = 0;
    for (uint32_t id : top) {
        if (!id || !s->by_id[id] || used >= sizeof(names)) continue;
        const int wrote = snprintf(names + used, sizeof(names) - used, " %s=%u", g_names[id].c_str(), s->by_id[id]);
        if (wrote > 0) used += static_cast<size_t>(wrote);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "sync calls per frame avg=%.2f max=%u over %u frames;%s",
                        double(s->total) / s->frames, s->max, s->frames, used ? names : " none");
    std::memset(s->by_id, 0, sizeof(s->by_id));
    s->total = s->max = s->frames = 0;
}

// Diagnostics: a record that keeps the worker busy for seconds (the same record
// running at consecutive one-second checks, nothing executed between) is logged.
void WatchdogMain(Worker* worker) {
    pthread_setname_np(pthread_self(), "BL2GlWatchdog");
    uint32_t last_running = 0;
    uint64_t last_executed = 0;
    unsigned stuck = 0;
    for (;;) {
        sleep(1);
        const uint32_t running = worker->executing.load(std::memory_order_relaxed);
        const uint64_t executed = worker->executed.load(std::memory_order_relaxed);
        if (running && running == last_running && executed == last_executed) {
            if (++stuck == 3 || stuck % 30 == 0)
                __android_log_print(ANDROID_LOG_WARN, kTag, "worker busy %us in %s", stuck,
                                    running - 1 < kMaxIds ? g_names[running - 1].c_str() : "internal");
        } else {
            stuck = 0;
        }
        last_running = running;
        last_executed = executed;
    }
}

Worker* StartWorker(DynarmicHost& host) {
    if (g_worker) return g_worker;
    const uint32_t ring = host.GuestAlloc(kRingBytes, 64);
    if (!ring) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "off: no guest memory for the command ring");
        g_enabled = false;
        return nullptr;
    }
    LoadEglFences();
    {
        char value[PROP_VALUE_MAX] = {};
        g_queries_enabled = !(__system_property_get("debug.bl2.gl_worker_queries", value) > 0 && value[0] == '0');
        if (g_queries_enabled) g_queries = new QueryCache();
        g_buffers_enabled = !(__system_property_get("debug.bl2.gl_worker_buffers", value) > 0 && value[0] == '0');
        g_sync_stats = new SyncStats();
    }
    auto* worker = new Worker;
    worker->host = &host;
    worker->memory = host.GetGuestMemoryBase();
    worker->ring = ring;
    try {
        std::thread(WorkerMain, worker).detach();
        std::thread(WatchdogMain, worker).detach();
    } catch (...) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "off: worker thread not started");
        g_enabled = false;
        delete worker;
        return nullptr;
    }
    g_worker = worker;
    __android_log_print(ANDROID_LOG_INFO, kTag, "started ring=%08X bytes=%u buffers=%d queries=%d", ring, kRingBytes,
                        g_buffers_enabled, g_queries_enabled);
    return worker;
}

bool IsBoundThread() {
    return g_bound.load(std::memory_order_acquire) && pthread_equal(g_bound_thread, pthread_self());
}

// eglMakeCurrent: binds the calling thread when it makes a window surface current.
bool MakeCurrent(uint32_t id, const ThunkHandler& handler, Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    const uint32_t draw = jit.Regs()[1], context = jit.Regs()[3];
    const bool window = context && Bl2EglIsWindowSurfaceHandle(draw);
    std::unique_lock lock(g_bind_mutex);
    if (IsBoundThread()) {
        DropDeferredMaps(host);
        if (window) {
            RunSync(*g_worker, id, handler, jit);
            DiscardNames();
            if (jit.Regs()[0] == EGL_TRUE) {
                RunInternalSync(*g_worker, kOpQueryVertexState);
                LoadShadow(g_worker->query);
            }
            return true;
        }
        // Another surface or none: the worker lets go and the thread runs directly.
        RunInternalSync(*g_worker, kOpRelease);
        DiscardNames();
        g_bound.store(false, std::memory_order_release);
        g_shadow = Shadow{};
        __android_log_print(ANDROID_LOG_INFO, kTag, "unbound tid=%d draw=%08X context=%08X", gettid(), draw, context);
        return false;
    }
    if (!window) return false;
    if (g_bound.load(std::memory_order_acquire)) {
        static std::atomic<unsigned> conflicts{0};
        if (conflicts.fetch_add(1, std::memory_order_relaxed) < 8)
            __android_log_print(ANDROID_LOG_WARN, kTag, "window current on tid=%d while another thread is bound: runs directly",
                                gettid());
        return false;
    }
    Worker* worker = StartWorker(host);
    if (!worker) return false;
    // The thread's own current context (if any) is released as eglMakeCurrent would.
    const EGLDisplay display = eglGetCurrentDisplay();
    if (display != EGL_NO_DISPLAY && eglGetCurrentContext() != EGL_NO_CONTEXT)
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    g_bound_thread = pthread_self();
    g_bound.store(true, std::memory_order_release);
    RunSync(*worker, id, handler, jit);
    DiscardNames();
    if (jit.Regs()[0] == EGL_TRUE) {
        RunInternalSync(*worker, kOpQueryVertexState);
        LoadShadow(worker->query);
    } else {
        g_shadow = Shadow{};
    }
    Bl2PerfHintSetProducer(gettid());
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "bound tid=%d draw=%08X context=%08X result=%u vao=%u array=%u element=%u enabled=%04X client=%04X",
                        gettid(), draw, context, jit.Regs()[0], worker->query.vertex_array, g_shadow.array_buffer,
                        g_shadow.element_buffer, g_shadow.enabled, g_shadow.client);
    return true;
}
}  // namespace

// bl2_fresh_persistent_* handler on the bound thread: a name the worker made in advance.
bool Bl2GlWorkerTakeBufferName(uint32_t* name) {
    if (!IsBoundThread()) return false;
    const uint32_t taken = g_fresh.taken.load(std::memory_order_relaxed);
    if (g_fresh.pushed.load(std::memory_order_acquire) == taken) return false;
    *name = g_fresh.names[taken % NameCache::kSize];
    g_fresh.taken.store(taken + 1, std::memory_order_release);
    return true;
}

// ThunkManager::DispatchThunk: true when the worker ran or queued the call.
bool Bl2GlWorkerDispatch(uint32_t id, const std::string& name, const ThunkHandler& handler,
                         Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    static const bool enabled = [] {
        g_enabled = ReadEnabled();
        __android_log_print(ANDROID_LOG_INFO, kTag, g_enabled ? "enabled (debug.bl2.gl_worker=0 disables)"
                                                              : "disabled by debug.bl2.gl_worker=0");
        return g_enabled;
    }();
    if (!enabled || !g_enabled) return false;
    const Spec& spec = SpecFor(id, name);
    if (spec.kind == kLocal) return false;
    if (spec.kind == kBlocking) {
        if (IsBoundThread() && g_worker->unannounced) Announce(*g_worker);
        return false;
    }
    if (spec.kind == kMakeCurrent) return MakeCurrent(id, handler, jit, host);
    if (!IsBoundThread()) return false;
    Worker& w = *g_worker;
    switch (spec.kind) {
    case kReleaseThread:
        DropDeferredMaps(host);
        RunSync(w, id, handler, jit);
        DiscardNames();
        {
            std::lock_guard lock(g_bind_mutex);
            g_bound.store(false, std::memory_order_release);
            g_shadow = Shadow{};
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "released tid=%d", gettid());
        return true;
    case kBindBuffer:
    case kAttribPointer:
    case kEnableAttrib:
    case kDisableAttrib:
        UpdateShadow(spec, jit, host);
        if (RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    case kBindVertexArray:
        UpdateShadow(spec, jit, host);
        break;
    case kDeleteBuffers: {
        // Deleting a mapped buffer unmaps it.
        const int32_t count = static_cast<int32_t>(jit.Regs()[0]);
        const uint32_t names = jit.Regs()[1];
        if (count > 0 && count <= 65536 && uint64_t(names) + 4ull * count <= host.GetGuestMemorySize())
            for (int32_t i = 0; i < count; ++i) DropDeferredMap(host, host.MemoryRead32(names + 4 * i));
        UpdateShadow(spec, jit, host);
        break;
    }
    case kBufferData:
        if (!g_buffers_enabled) break;
        DropDeferredMap(host, ShadowBuffer(jit.Regs()[0]));  // New storage unmaps the old one.
        if (RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    case kMapRange:
        if (MapDeferred(w, handler, jit, host)) return true;
        break;
    case kUnmap:
        if (UnmapDeferred(w, handler, jit)) return true;
        break;
    case kBufferPointer: {
        // r0 target, r1 pname, r2 params.
        const DeferredMap* map = FindDeferredMap(ShadowBuffer(jit.Regs()[0]));
        const uint32_t params = jit.Regs()[2];
        if (map && jit.Regs()[1] == GL_BUFFER_MAP_POINTER && params >= 0x10000 &&
            uint64_t(params) + 4 <= host.GetGuestMemorySize()) {
            host.MemoryWrite32(params, map->staging);
            return true;
        }
        break;
    }
    case kFreshName:
        if (!g_buffers_enabled) break;
        RequestNames(w);
        // With a cached name ready, the handler runs on this thread and takes it.
        if (CachedNames()) return false;
        break;
    case kClientWaitSync: {
        // r0 handle, r1 flags, r2:r3 the 64-bit timeout.
        uint32_t result = 0;
        const uint64_t timeout = uint64_t(jit.Regs()[2]) | (uint64_t(jit.Regs()[3]) << 32);
        if (WaitEglFence(jit.Regs()[0], timeout, result)) {
            jit.Regs()[0] = result;
            return true;
        }
        break;
    }
    case kDeleteSync:
        if (RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    case kBeginQuery:
        if (g_queries && jit.Regs()[1] < kQueryNames) ++g_queries->issued[jit.Regs()[1]];
        if (RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    case kEndQuery:
        if (RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    case kQueryResult: {
        // r0 query, r1 pname, r2 params.
        const GLuint query = jit.Regs()[0];
        const GLenum pname = jit.Regs()[1];
        const uint32_t params = jit.Regs()[2];
        if (!g_queries || query >= kQueryNames || params < 0x10000 ||
            uint64_t(params) + 4 > host.GetGuestMemorySize())
            break;
        const bool ready = g_queries->generation[query].load(std::memory_order_acquire) ==
                           g_queries->issued[query] && g_queries->issued[query] != 0;
        if (pname == kQueryResultNoWait) {
            if (ready) host.MemoryWrite32(params, g_queries->value[query].load(std::memory_order_relaxed));
            return true;  // Not ready: the caller's memory stays untouched, as NO_WAIT specifies.
        }
        if (pname == GL_QUERY_RESULT_AVAILABLE) {
            host.MemoryWrite32(params, ready ? GL_TRUE : GL_FALSE);
            return true;
        }
        if (pname == GL_QUERY_RESULT && ready) {
            host.MemoryWrite32(params, g_queries->value[query].load(std::memory_order_relaxed));
            return true;
        }
        break;
    }
    case kFenceSync:
        // The handle is reserved now; the worker creates the GL and EGL fences in order.
        if (const uint32_t handle = Bl2GuestGlSyncReserve()) {
            RunInternalAsync(w, kOpFenceSync, jit.Regs()[0], jit.Regs()[1], handle, true);
            jit.Regs()[0] = handle;
            return true;
        }
        break;
    case kSwapBuffers:
        // At most one queued swap: the guest may run one frame ahead of the worker.
        if (w.swap_failed.exchange(false, std::memory_order_relaxed)) break;
        ProducerWait(w, w.swaps_done, [&] {
            return w.swaps_done.load(std::memory_order_seq_cst) == w.swaps_issued;
        }, id);
        NoteFrame();
        if (RunAsync(w, id, handler, jit, host, spec, true)) {
            ++w.swaps_issued;
            jit.Regs()[0] = EGL_TRUE;
            return true;
        }
        break;
    case kDrawElements:
        if (!DrawsReadGuestMemory(true) && RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    case kDrawArrays:
        if (!DrawsReadGuestMemory(false) && RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    case kAsync:
    case kPayload:
        if (RunAsync(w, id, handler, jit, host, spec)) return true;
        break;
    default:
        break;
    }
    NoteSyncCall(id);
    RunSync(w, id, handler, jit);
    return true;
}
