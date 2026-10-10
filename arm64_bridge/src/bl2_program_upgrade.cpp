#include "bl2_program_upgrade.h"

#include <android/log.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr const char* kTag = "BL2ProgLink";
constexpr const char* kArrayNames[9] = {
    "VConstGlobal", "VConstFloat", "VConstBones", "VConstInt", "VConstBool",
    "PConstGlobal", "PConstFloat", "PConstInt", "PConstBool"};
// Pair binaries live beside the stage cache; hot pairs per session are a few
// thousand at most, so this only bounds a pathological session.
constexpr uint64_t kExportBudgetBytes = 192ull * 1024 * 1024;

struct Worker {
    uint64_t group = 0;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext share = EGL_NO_CONTEXT;
    const Bl2ShaderStageCache* cache = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Bl2ProgramUpgradeRequest> requests;
    std::vector<Bl2ProgramUpgradeResult> results;
    bool stop = false;
    bool failed = false;
    uint64_t exported_bytes = 0;
    uint32_t linked = 0, imported = 0, rejected = 0;
};

std::mutex g_mutex;               // Guards g_worker; ordered after the renderer's mutex.
Worker* g_worker = nullptr;
std::atomic<bool> g_has_results{false};

GLuint CompileShader(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    const GLchar* text = source.data();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    glDeleteShader(shader);
    return 0;
}

GLuint LinkPair(const Bl2ProgramUpgradeRequest& request) {
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, request.vertex_source);
    const GLuint fragment = vertex ? CompileShader(GL_FRAGMENT_SHADER, request.fragment_source) : 0;
    GLuint program = vertex && fragment ? glCreateProgram() : 0;
    if (program) {
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        glDetachShader(program, vertex);
        glDetachShader(program, fragment);
        if (linked != GL_TRUE) {
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vertex) glDeleteShader(vertex);
    if (fragment) glDeleteShader(fragment);
    return program;
}

// Same rule as the renderer's monolithic path: location by name, element
// count only when the array is actually active.
Bl2ProgramUniformArray FindArray(GLuint program, const char* name) {
    Bl2ProgramUniformArray result;
    result.location = glGetUniformLocation(program, name);
    if (result.location < 0) return result;
    GLint count = 0, max_name = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_name);
    if (count <= 0 || max_name <= 1) return result;
    std::vector<GLchar> buffer(static_cast<size_t>(max_name));
    const std::string wanted(name);
    const std::string wanted_array = wanted + "[0]";
    for (GLint index = 0; index < count; ++index) {
        GLsizei written = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, static_cast<GLuint>(index), max_name, &written,
                           &size, &type, buffer.data());
        if (written <= 0) continue;
        const std::string_view active(buffer.data(), static_cast<size_t>(written));
        if (active == wanted || active == wanted_array) {
            result.size = std::max(size, 0);
            break;
        }
    }
    return result;
}

EGLContext CreateWorkerContext(Worker& worker, EGLSurface& surface) {
    surface = EGL_NO_SURFACE;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return EGL_NO_CONTEXT;
    EGLint config_id = 0;
    if (!eglQueryContext(worker.display, worker.share, EGL_CONFIG_ID, &config_id))
        return EGL_NO_CONTEXT;
    EGLConfig config = nullptr;  // EGL_NO_CONFIG_KHR for a config-less renderer.
    if (config_id) {
        const EGLint wanted[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(worker.display, wanted, &config, 1, &count) || count != 1)
            return EGL_NO_CONTEXT;
    }
    const EGLint attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    const EGLContext context = eglCreateContext(worker.display, config, worker.share, attributes);
    if (context == EGL_NO_CONTEXT) return EGL_NO_CONTEXT;
    const char* extensions = eglQueryString(worker.display, EGL_EXTENSIONS);
    const bool surfaceless = extensions && std::strstr(extensions, "EGL_KHR_surfaceless_context");
    if (!surfaceless && config) {
        const EGLint pbuffer[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(worker.display, config, pbuffer);
    }
    if ((surfaceless || surface != EGL_NO_SURFACE) &&
        eglMakeCurrent(worker.display, surface, surface, context)) return context;
    if (surface != EGL_NO_SURFACE) eglDestroySurface(worker.display, surface);
    surface = EGL_NO_SURFACE;
    eglDestroyContext(worker.display, context);
    return EGL_NO_CONTEXT;
}

Bl2ProgramUpgradeResult Upgrade(Worker& worker, const Bl2ProgramUpgradeRequest& request) {
    Bl2ProgramUpgradeResult result;
    result.context = request.context;
    result.key = request.key;
    result.serial = request.serial;
    GLuint program = worker.cache
        ? worker.cache->ImportPair(request.vertex_digest, request.fragment_digest) : 0;
    result.imported = program != 0;
    if (!program) program = LinkPair(request);
    if (program && !result.imported && worker.cache &&
        worker.exported_bytes < kExportBudgetBytes) {
        uint64_t bytes = 0;
        if (worker.cache->ExportPair(program, request.vertex_digest,
                                     request.fragment_digest, bytes))
            worker.exported_bytes += bytes;
    }
    if (program) {
        for (size_t i = 0; i < result.arrays.size(); ++i)
            result.arrays[i] = FindArray(program, kArrayNames[i]);
        // The renderer may draw with it next: complete it before publishing.
        glFinish();
    }
    while (glGetError() != GL_NO_ERROR) {}
    result.program = program;
    return result;
}

void Run(Worker* worker) {
    pthread_setname_np(pthread_self(), "BL2ProgLink");
    // Background work: never compete with the game or render threads.
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
    EGLSurface surface = EGL_NO_SURFACE;
    const EGLContext context = CreateWorkerContext(*worker, surface);
    if (context == EGL_NO_CONTEXT) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "worker context unavailable error=0x%04X; pairs stay separable",
                            eglGetError());
    }
    for (;;) {
        Bl2ProgramUpgradeRequest request;
        {
            std::unique_lock lock(worker->mutex);
            if (context == EGL_NO_CONTEXT) worker->failed = true;
            worker->wake.wait(lock, [&] { return worker->stop || !worker->requests.empty(); });
            if (worker->stop) break;
            request = std::move(worker->requests.front());
            worker->requests.pop_front();
        }
        Bl2ProgramUpgradeResult result;
        if (context != EGL_NO_CONTEXT) result = Upgrade(*worker, request);
        else {
            result.context = request.context;
            result.key = request.key;
            result.serial = request.serial;
        }
        std::lock_guard lock(worker->mutex);
        if (!result.program) ++worker->rejected;
        else if (result.imported) ++worker->imported;
        else ++worker->linked;
        const uint32_t done = worker->linked + worker->imported + worker->rejected;
        if (done == 1 || done % 128 == 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "pair programs linked=%u imported=%u rejected=%u exported=%.1fMiB queued=%zu",
                worker->linked, worker->imported, worker->rejected,
                worker->exported_bytes / 1048576.0, worker->requests.size());
        }
        worker->results.push_back(std::move(result));
        g_has_results.store(true, std::memory_order_release);
    }
    if (context != EGL_NO_CONTEXT) {
        std::lock_guard lock(worker->mutex);
        for (const auto& result : worker->results)
            if (result.program) glDeleteProgram(result.program);
        worker->results.clear();
        eglMakeCurrent(worker->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(worker->display, surface);
        eglDestroyContext(worker->display, context);
    }
    eglReleaseThread();
}

}  // namespace

bool Bl2ProgramUpgradeEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return !(__system_property_get("debug.bl2.mono_programs", value) > 0 &&
                 value[0] == '0');
    }();
    return enabled;
}

bool Bl2ProgramUpgradeSubmit(uint64_t group, Bl2ProgramUpgradeRequest request,
                             const Bl2ShaderStageCache* cache) {
    if (!group || !Bl2ProgramUpgradeEnabled()) return false;
    std::lock_guard lock(g_mutex);
    if (!g_worker) {
        const EGLDisplay display = eglGetCurrentDisplay();
        const EGLContext share = eglGetCurrentContext();
        if (display == EGL_NO_DISPLAY || share == EGL_NO_CONTEXT) return false;
        auto* worker = new Worker;
        worker->group = group;
        worker->display = display;
        worker->share = share;
        worker->cache = cache && cache->PairStoreReady() ? cache : nullptr;
        try {
            worker->thread = std::thread(Run, worker);
        } catch (...) {
            delete worker;
            return false;
        }
        g_worker = worker;
    }
    if (g_worker->group != group) return false;
    std::lock_guard worker_lock(g_worker->mutex);
    if (g_worker->failed) return false;
    g_worker->requests.push_back(std::move(request));
    g_worker->wake.notify_one();
    return true;
}

bool Bl2ProgramUpgradeHasResults() {
    return g_has_results.load(std::memory_order_acquire);
}

std::vector<Bl2ProgramUpgradeResult> Bl2ProgramUpgradeTake(EGLContext context, size_t limit) {
    (void)context;
    std::vector<Bl2ProgramUpgradeResult> taken;
    std::lock_guard lock(g_mutex);
    if (!g_worker) {
        g_has_results.store(false, std::memory_order_release);
        return taken;
    }
    std::lock_guard worker_lock(g_worker->mutex);
    const size_t count = std::min(limit, g_worker->results.size());
    taken.assign(std::make_move_iterator(g_worker->results.begin()),
                 std::make_move_iterator(g_worker->results.begin() + static_cast<ptrdiff_t>(count)));
    g_worker->results.erase(g_worker->results.begin(),
                            g_worker->results.begin() + static_cast<ptrdiff_t>(count));
    g_has_results.store(!g_worker->results.empty(), std::memory_order_release);
    return taken;
}

uint64_t Bl2ProgramUpgradeGroup() {
    std::lock_guard lock(g_mutex);
    return g_worker ? g_worker->group : 0;
}

void Bl2ProgramUpgradeStop() {
    Worker* worker = nullptr;
    {
        std::lock_guard lock(g_mutex);
        worker = std::exchange(g_worker, nullptr);
        g_has_results.store(false, std::memory_order_release);
    }
    if (!worker) return;
    {
        std::lock_guard lock(worker->mutex);
        worker->stop = true;
        worker->requests.clear();
    }
    worker->wake.notify_one();
    if (worker->thread.joinable()) worker->thread.join();
    delete worker;
}
