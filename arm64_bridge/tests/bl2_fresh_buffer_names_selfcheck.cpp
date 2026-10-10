#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdio>
#include <unistd.h>

extern "C" bool Bl2FreshPersistentBufferNamesSelfCheck();

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(60);
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_ES_API)) return 1;
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                             EGL_OPENGL_ES3_BIT_KHR, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) return 1;
    const EGLint surface_attributes[]{EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    const EGLint context_attributes[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) return 1;
    const bool passed = Bl2FreshPersistentBufferNamesSelfCheck();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    alarm(0);
    return passed ? 0 : 1;
}
