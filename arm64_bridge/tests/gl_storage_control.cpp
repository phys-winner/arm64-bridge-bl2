#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {
constexpr GLsizeiptr kBytes = 16384;
constexpr GLbitfield kAccess = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT |
                               GL_MAP_COHERENT_BIT_EXT;
unsigned failures = 0;
bool reuse_complete = true;
PFNGLBUFFERSTORAGEEXTPROC storage = nullptr;

bool check(bool condition, const char* label) {
    std::printf("CHECK %s %s\n", condition ? "PASS" : "FAIL", label);
    failures += !condition;
    return condition;
}

bool error(const char* operation, GLenum expected = GL_NO_ERROR) {
    const GLenum actual = glGetError();
    std::printf("GL_ERROR operation=%s actual=%04X expected=%04X\n",
                operation, actual, expected);
    return check(actual == expected, operation);
}

struct Metadata {
    GLint size = -1, immutable = -1, flags = -1, mapped = -1, access = -1;
};

Metadata metadata(GLenum target, const char* label) {
    Metadata result;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &result.size);
    error("GetBufferParameteriv(SIZE)");
    glGetBufferParameteriv(target, GL_BUFFER_IMMUTABLE_STORAGE_EXT, &result.immutable);
    error("GetBufferParameteriv(IMMUTABLE)");
    glGetBufferParameteriv(target, GL_BUFFER_STORAGE_FLAGS_EXT, &result.flags);
    error("GetBufferParameteriv(STORAGE_FLAGS)");
    glGetBufferParameteriv(target, GL_BUFFER_MAPPED, &result.mapped);
    error("GetBufferParameteriv(MAPPED)");
    glGetBufferParameteriv(target, GL_BUFFER_ACCESS_FLAGS, &result.access);
    error("GetBufferParameteriv(ACCESS_FLAGS)");
    std::printf("METADATA label=%s target=%04X size=%d immutable=%d flags=%04X "
                "mapped=%d access=%04X\n", label, target, result.size,
                result.immutable, result.flags, result.mapped, result.access);
    return result;
}

bool current(EGLDisplay display, EGLSurface surface, EGLContext context) {
    const EGLBoolean result = eglMakeCurrent(display, surface, surface, context);
    const EGLint egl_error = eglGetError();
    std::printf("CURRENT context=%p result=%u error=%04X\n", context, result, egl_error);
    return check(result == EGL_TRUE && egl_error == EGL_SUCCESS, "eglMakeCurrent");
}

GLenum bindingName(GLenum target) {
    return target == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : GL_ELEMENT_ARRAY_BUFFER_BINDING;
}

void fresh(GLenum target, bool negative) {
    std::printf("CASE fresh target=%04X flags=%04X\n", target, negative ? 0 : kAccess);
    GLuint name = 0;
    glGenBuffers(1, &name); error("fresh GenBuffers");
    glBindBuffer(target, name); error("fresh BindBuffer");
    storage(target, kBytes, nullptr, negative ? 0 : kAccess);
    error("fresh BufferStorageEXT");
    const auto allocated = metadata(target, "fresh-after-storage");
    check(allocated.size == kBytes && allocated.immutable == GL_TRUE &&
          allocated.flags == static_cast<GLint>(negative ? 0 : kAccess) &&
          allocated.mapped == GL_FALSE, "fresh actual storage contract");
    if (negative) {
        storage(target, kBytes, nullptr, kAccess);
        error("immutable flags0 BufferStorageEXT(C2)", GL_INVALID_OPERATION);
        const auto unchanged = metadata(target, "after-rejected-C2-storage");
        check(unchanged.size == kBytes && unchanged.immutable == GL_TRUE &&
              unchanged.flags == 0 && unchanged.mapped == GL_FALSE,
              "rejected storage preserves original flags0 object");
    }
    void* pointer = glMapBufferRange(target, 0, kBytes, kAccess);
    error("fresh MapBufferRange(C2)", negative ? GL_INVALID_OPERATION : GL_NO_ERROR);
    std::printf("MAP target=%04X name=%u pointer=%p\n", target, name, pointer);
    check(negative ? pointer == nullptr : pointer != nullptr, "fresh actual map result");
    const auto mapped = metadata(target, "fresh-after-map");
    check(mapped.immutable == GL_TRUE &&
          mapped.flags == static_cast<GLint>(negative ? 0 : kAccess) &&
          mapped.mapped == (negative ? GL_FALSE : GL_TRUE) &&
          mapped.access == static_cast<GLint>(negative ? 0 : kAccess),
          "map retains actual storage/access flags");
    if (pointer) {
        std::memset(pointer, 0x35, static_cast<size_t>(kBytes));
        const GLboolean unmapped = glUnmapBuffer(target);
        error("fresh UnmapBuffer");
        check(unmapped == GL_TRUE, "fresh actual unmap");
    }
    glDeleteBuffers(1, &name); error("fresh DeleteBuffers");
}

void converted(EGLDisplay display, EGLSurface surface, EGLContext owner,
               EGLContext sibling, GLenum initial_target, GLenum target, bool cross_context,
               bool fresh_persistent = false) {
    std::printf("CASE %s initial_target=%04X target=%04X cross_context=%u\n",
                fresh_persistent ? "fresh-persistent-from-live-pool" : "mutable-conversion",
                initial_target, target, static_cast<unsigned>(cross_context));
    if (!current(display, surface, owner)) return;
    GLuint name = 0;
    glGenBuffers(1, &name); error("conversion GenBuffers");
    glBindBuffer(initial_target, name); error("conversion BindBuffer(initial)");
    glBufferData(initial_target, kBytes, nullptr, GL_DYNAMIC_DRAW);
    error("conversion BufferData(DYNAMIC_DRAW)");
    const auto mutable_store = metadata(initial_target, "conversion-mutable-store");
    constexpr GLint mutable_flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_DYNAMIC_STORAGE_BIT_EXT;
    check(mutable_store.size == kBytes && mutable_store.immutable == GL_FALSE &&
          (fresh_persistent || mutable_store.flags == mutable_flags) &&
          mutable_store.mapped == GL_FALSE,
          fresh_persistent ? "live pooled name has actual mutable storage" :
                             "BufferData creates actual mutable READ/WRITE/DYNAMIC storage");
    // The original common name pool can return a previously mapped/unmapped
    // pixel-unpack object for a persistent vertex/index allocation.
    void* old_pointer = glMapBufferRange(initial_target, 0, kBytes,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    error("conversion mutable MapBufferRange(2A)");
    check(old_pointer != nullptr, "actual old mutable map");
    if (old_pointer) {
        std::memset(old_pointer, 0x63, static_cast<size_t>(kBytes));
        const GLboolean unmapped = glUnmapBuffer(initial_target);
        error("conversion mutable UnmapBuffer");
        check(unmapped == GL_TRUE, "actual old mutable unmap");
    }
    if (cross_context && !current(display, surface, sibling)) return;
    const GLuint pooled_name = name;
    if (fresh_persistent) {
        glGenBuffers(1, &name); error("persistent native GenBuffers(fresh)");
        check(name != 0 && name != pooled_name, "persistent name differs from still-live pooled name");
    }
    glBindBuffer(target, name);
    error(fresh_persistent ? "persistent BindBuffer(fresh-GLuint)" : "conversion BindBuffer(same-GLuint)");
    storage(target, kBytes, nullptr, kAccess);
    error("conversion BufferStorageEXT(C2)");
    const auto immutable_store = metadata(target, "conversion-after-C2-storage");
    check(immutable_store.size == kBytes && immutable_store.immutable == GL_TRUE &&
          immutable_store.flags == static_cast<GLint>(kAccess) &&
          immutable_store.mapped == GL_FALSE,
          fresh_persistent ? "fresh persistent name creates actual C2 immutable storage" :
                             "same GLuint converts to actual C2 immutable storage");
    void* pointer = glMapBufferRange(target, 0, kBytes, kAccess);
    error("conversion MapBufferRange(C2)");
    check(pointer != nullptr, "actual converted C2 map");
    const auto mapped = metadata(target, "conversion-after-C2-map");
    check(mapped.flags == static_cast<GLint>(kAccess) && mapped.mapped == GL_TRUE &&
          mapped.access == static_cast<GLint>(kAccess), "converted map retains native storage/access flags");
    if (pointer) {
        std::memset(pointer, 0x91, static_cast<size_t>(kBytes));
        const GLboolean unmapped = glUnmapBuffer(target);
        error("conversion C2 UnmapBuffer");
        check(unmapped == GL_TRUE, "actual converted C2 unmap");
    }
    glDeleteBuffers(1, &name); error("conversion DeleteBuffers");
    if (fresh_persistent) {
        // The repair leaves the original ordinary pool and its live objects
        // intact. Compare native metadata and read back the original bytes;
        // do not turn Mali's unrelated mutable flags query defect into a pass.
        glBindBuffer(initial_target, pooled_name); error("pool BindBuffer(original-live-name)");
        const auto preserved = metadata(initial_target, "pool-after-fresh-persistent-allocation");
        check(preserved.size == mutable_store.size &&
              preserved.immutable == mutable_store.immutable &&
              preserved.flags == mutable_store.flags && preserved.mapped == GL_FALSE,
              "fresh persistent allocation preserves live pooled mutable store");
        const GLboolean exists = glIsBuffer(pooled_name); error("pool IsBuffer(original-live-name)");
        check(exists == GL_TRUE, "ordinary pooled name remains in native namespace");
        const void* data = glMapBufferRange(initial_target, 0, kBytes, GL_MAP_READ_BIT);
        error("pool actual Read MapBufferRange");
        check(data != nullptr, "ordinary pooled object still maps through native GL");
        if (data) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            check(std::all_of(bytes, bytes + kBytes, [](unsigned char byte) { return byte == 0x63; }),
                  "fresh allocation preserves every original pooled byte");
            const GLboolean unmapped = glUnmapBuffer(initial_target);
            error("pool actual Read UnmapBuffer");
            check(unmapped == GL_TRUE, "ordinary pooled read map actually unmaps");
        }
        glDeleteBuffers(1, &pooled_name); error("pool test cleanup DeleteBuffers");
    }
    if (cross_context) {
        if (!current(display, surface, owner)) return;
        // Release a deleted object's remaining original-context reference.
        glBindBuffer(initial_target, 0); error("conversion owner cleanup BindBuffer(0)");
    }
}

void sharedReuse(EGLDisplay display, EGLSurface surface, EGLContext owner,
                 EGLContext sibling, GLenum target) {
    std::printf("CASE shared-delete-reuse target=%04X\n", target);
    if (!current(display, surface, owner)) return;
    GLuint owner_vao = 0, sibling_vao = 0, old_name = 0;
    glGenVertexArrays(1, &owner_vao); error("owner GenVertexArrays");
    glBindVertexArray(owner_vao); error("owner BindVertexArray");
    glGenBuffers(1, &old_name); error("owner GenBuffers");
    glBindBuffer(target, old_name); error("owner BindBuffer(old)");
    storage(target, kBytes, nullptr, 0); error("owner BufferStorageEXT(flags0)");
    const auto old = metadata(target, "owner-old-object");
    check(old.immutable == GL_TRUE && old.flags == 0, "owner flags0 native object");
    if (!current(display, surface, sibling)) return;
    glGenVertexArrays(1, &sibling_vao); error("sibling GenVertexArrays");
    glBindVertexArray(sibling_vao); error("sibling BindVertexArray");
    glBindBuffer(target, old_name); error("sibling BindBuffer(old)");
    if (!current(display, surface, owner)) return;
    glDeleteBuffers(1, &old_name); error("owner DeleteBuffers(old)");
    const GLboolean exists = glIsBuffer(old_name); error("owner IsBuffer(deleted)");
    check(exists == GL_FALSE, "deleted name removed from shared namespace");

    // Name reuse is legal but its allocation policy is implementation-defined.
    // Report incomplete coverage if a bounded reservation does not obtain it.
    std::array<GLuint, 4096> names{};
    glGenBuffers(static_cast<GLsizei>(names.size()), names.data());
    error("owner GenBuffers(reuse-reservation)");
    const bool reused = std::find(names.begin(), names.end(), old_name) != names.end();
    std::printf("NAME_REUSE old=%u observed=%u first=%u last=%u\n",
                old_name, static_cast<unsigned>(reused), names.front(), names.back());
    reuse_complete = reused && reuse_complete;
    const GLuint new_name = reused ? old_name : names.front();
    glBindBuffer(target, new_name); error("owner BindBuffer(new)");
    storage(target, kBytes, nullptr, kAccess); error("owner BufferStorageEXT(new-C2)");
    const auto replacement = metadata(target, "owner-new-object");
    check(replacement.immutable == GL_TRUE && replacement.flags == static_cast<GLint>(kAccess),
          "new namespace object has actual C2 storage");
    if (!current(display, surface, sibling)) return;
    GLint retained_name = -1;
    glGetIntegerv(bindingName(target), &retained_name); error("sibling GetBinding(old)");
    const auto retained = metadata(target, "sibling-retained-old-object");
    check(retained_name == static_cast<GLint>(old_name) && retained.immutable == GL_TRUE &&
          retained.flags == 0, "other context/VAO retains actual deleted flags0 object");
    if (reused) {
        // Reproduce the original integer-cache omission without replacing data.
        void* omitted = glMapBufferRange(target, 0, kBytes, kAccess);
        error("sibling cache-omitted MapBufferRange(C2)", GL_INVALID_OPERATION);
        check(omitted == nullptr, "same name cache omission maps old object and fails");
        if (omitted) { glUnmapBuffer(target); error("unexpected omitted map cleanup"); }
    } else {
        std::printf("CASE_INCOMPLETE numeric name reuse not observed within 4096 reservations\n");
    }
    glBindBuffer(target, new_name); error("sibling real BindBuffer(new-same-name)");
    const auto rebound = metadata(target, "sibling-after-real-bind");
    check(rebound.immutable == GL_TRUE && rebound.flags == static_cast<GLint>(kAccess),
          "real bind resolves the current namespace object");
    void* pointer = glMapBufferRange(target, 0, kBytes, kAccess);
    error("sibling real-bind MapBufferRange(C2)");
    check(pointer != nullptr, "real bind exposes an actual valid native C2 map");
    if (pointer) {
        std::memset(pointer, 0x5A, static_cast<size_t>(kBytes));
        const GLboolean unmapped = glUnmapBuffer(target); error("sibling UnmapBuffer(new)");
        check(unmapped == GL_TRUE, "shared new map actually unmapped");
    }
    glBindBuffer(target, 0); error("sibling cleanup BindBuffer(0)");
    glDeleteVertexArrays(1, &sibling_vao); error("sibling cleanup DeleteVertexArrays");
    if (!current(display, surface, owner)) return;
    glDeleteBuffers(static_cast<GLsizei>(names.size()), names.data());
    error("owner cleanup DeleteBuffers(reservations)");
    glDeleteVertexArrays(1, &owner_vao); error("owner cleanup DeleteVertexArrays");
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool fresh_control = argc == 2 && !std::strcmp(argv[1], "--fresh-persistent-control");
    if (argc != 1 && !fresh_control) {
        std::fprintf(stderr, "Usage: gl_storage_control [--fresh-persistent-control]\n");
        return 1;
    }
    std::printf("GL_STORAGE_CONTROL begin mode=%s\n", fresh_control ? "fresh-persistent" : "full-contract");
    alarm(60);
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!check(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor),
               "eglInitialize")) return 1;
    if (!check(eglBindAPI(EGL_OPENGL_ES_API) == EGL_TRUE, "eglBindAPI")) return 1;
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!check(eglChooseConfig(display, config_attributes, &config, 1, &count) && count == 1,
               "eglChooseConfig")) return 1;
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    const EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
    const EGLContext owner = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    const EGLContext sibling = eglCreateContext(display, config, owner, context_attributes);
    if (!check(surface != EGL_NO_SURFACE && owner != EGL_NO_CONTEXT && sibling != EGL_NO_CONTEXT,
               "create GLES3.2 pbuffer/shared contexts") || !current(display, surface, owner)) return 1;
    std::printf("DRIVER vendor=%s renderer=%s version=%s\n",
                reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    error("GetString(driver)");
    storage = reinterpret_cast<PFNGLBUFFERSTORAGEEXTPROC>(eglGetProcAddress("glBufferStorageEXT"));
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    error("GetString(extensions)");
    if (!check(storage && extensions && std::strstr(extensions, "GL_EXT_buffer_storage"),
               "actual GL_EXT_buffer_storage support")) return 1;
    for (GLenum target : {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER, GL_PIXEL_UNPACK_BUFFER}) {
        if (!fresh_control && target == GL_PIXEL_UNPACK_BUFFER) continue;
        fresh(target, false);
        fresh(target, true);
        for (bool cross_context : {false, true}) {
            converted(display, surface, owner, sibling, target, target, cross_context, fresh_control);
            converted(display, surface, owner, sibling, GL_PIXEL_UNPACK_BUFFER, target, cross_context, fresh_control);
        }
        if (!fresh_control) sharedReuse(display, surface, owner, sibling, target);
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, sibling);
    eglDestroyContext(display, owner);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    alarm(0);
    const int result = failures ? 1 : reuse_complete ? 0 : 2;
    std::printf("GL_STORAGE_CONTROL status=%s failures=%u reuse_complete=%u\n",
                result == 0 ? "PASS" : result == 1 ? "FAIL" : "INCOMPLETE",
                failures, static_cast<unsigned>(reuse_complete));
    return result;
}
