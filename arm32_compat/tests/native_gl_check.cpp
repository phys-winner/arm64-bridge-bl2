#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <cstdio>
#include "../../arm64_bridge/src/bl2_shader_warmup.h"

#if !defined(__arm__)
#error "This check must execute natively on ARM32."
#endif

class DynarmicHost;
bool NativeLegacyPipelineSelfCheck(EGLDisplay, EGLSurface, EGLContext, EGLContext, DynarmicHost*);

int main(int argc, char** argv) {
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API)) return 1;
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) return 2;
    const EGLint size[] = {EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE};
    const EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const EGLContext sibling = eglCreateContext(display, config, context, version);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || sibling == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) return 3;
    std::printf("ARM32 GPU: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    bool passed = NativeLegacyPipelineSelfCheck(display, surface, context, sibling, nullptr);
    if (passed && argc > 1) {
        FILE* file = std::fopen(argv[1], "rb");
        std::vector<uint8_t> bytes;
        if (file && std::fseek(file, 0, SEEK_END) == 0) {
            const long length = std::ftell(file);
            if (length > 0 && static_cast<size_t>(length) <= Bl2ShaderWarmup::kMaxBytes) {
                bytes.resize(static_cast<size_t>(length));
                std::rewind(file);
                if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
            }
        }
        if (file) std::fclose(file);
        Bl2ShaderWarmup warmup;
        passed = warmup.Warm(bytes) && glGetError() == GL_NO_ERROR;
        std::puts(passed ? "PASS actual warmup asset compile/link compatibility" : "FAIL actual warmup asset");
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, sibling);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    std::puts(passed ? "PASS native SSO transfer, independent uniforms, sampler and shared contexts"
                     : "FAIL native SSO pipeline check");
    return passed ? 0 : 4;
}
