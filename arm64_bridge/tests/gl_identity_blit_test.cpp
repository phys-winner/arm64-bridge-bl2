#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <array>
#include <cassert>
#include <cstdio>

// Fault injection covers the unqueryable host-capability guard boundaries.
static GLint injected_max = -1;
static void TestGetIntegerv(GLenum name, GLint* value) {
    if (name == GL_MAX_DRAW_BUFFERS && injected_max >= 0) *value = injected_max;
    else glGetIntegerv(name, value);
}
#define glGetIntegerv TestGetIntegerv
#include "gl_blit_compat.h"
#undef glGetIntegerv

static unsigned native_calls = 0, skipped_calls = 0;
static bool Blit(GLint sx0 = 0, GLint sy0 = 0, GLint sx1 = 4, GLint sy1 = 4,
                 GLint dx0 = 0, GLint dy0 = 0, GLint dx1 = 4, GLint dy1 = 4,
                 GLbitfield mask = GL_COLOR_BUFFER_BIT, GLenum filter = GL_NEAREST) {
    GLint read = 0, draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    if (IsIdentityColorBlit(read, draw, sx0, sy0, sx1, sy1,
                           dx0, dy0, dx1, dy1, mask, filter)) {
        ++skipped_calls;
        return true;
    }
    ++native_calls;
    std::printf("native#%u read=%d draw=%d src=%d,%d-%d,%d dst=%d,%d-%d,%d mask=%x filter=%x\n",
                native_calls, read, draw, sx0, sy0, sx1, sy1,
                dx0, dy0, dx1, dy1, mask, filter);
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
    return false;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr));
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    assert(eglChooseConfig(display, config_attrs, &config, 1, &count) && count == 1);
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint surface_attrs[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    assert(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE);
    assert(eglMakeCurrent(display, surface, surface, context));
    std::printf("GPU=%s GLES=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    std::array<GLubyte, 64> expected{}, actual{}, zeros{};
    for (unsigned i = 0; i < expected.size(); ++i) expected[i] = (i * 37 + 13) & 255;
    GLuint textures[2]{}, fbos[3]{};
    glGenTextures(2, textures);
    for (GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     texture == textures[0] ? expected.data() : zeros.data());
    }
    glGenFramebuffers(3, fbos);
    for (unsigned i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, textures[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
    const GLenum color0 = GL_COLOR_ATTACHMENT0;
    glReadBuffer(color0);
    glDrawBuffers(1, &color0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(1, 1, 2, 2);
    GLint active_before = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_before);
    assert(glGetError() == GL_NO_ERROR);
    assert(Blit() && glGetError() == GL_NO_ERROR);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    assert(actual == expected && glGetError() == GL_NO_ERROR);
    GLint read = 0, draw = 0, selector = 0, active_after = 0, scissor[4]{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_BUFFER, &selector);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_after);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    assert(read == GLint(fbos[0]) && draw == read && selector == GLint(color0));
    assert(active_after == active_before && glIsEnabled(GL_SCISSOR_TEST));
    assert(scissor[0] == 1 && scissor[1] == 1 && scissor[2] == 2 && scissor[3] == 2);
    glDisable(GL_SCISSOR_TEST);
    glEnable(0xffffffffu); // A preexisting error must survive the compatibility query.
    assert(Blit() && glGetError() == GL_INVALID_ENUM);

    // Rejected cases issue native except READ_NONE: this Adreno driver crashes
    // on that standalone command, so verify its guard decision without issuing it.
    assert(!Blit(0, 0, 4, 4, 1, 0, 4, 4)); assert(glGetError() == GL_INVALID_OPERATION);
    assert(!Blit(0, 0, 0, 4, 0, 0, 0, 4)); assert(glGetError() == GL_INVALID_OPERATION);
    assert(!Blit(4, 0, 0, 4, 4, 0, 0, 4)); assert(glGetError() == GL_INVALID_OPERATION);
    assert(!Blit(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_LINEAR));
    assert(glGetError() == GL_INVALID_OPERATION);
    assert(!Blit(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    assert(glGetError() == GL_INVALID_OPERATION);
    glReadBuffer(GL_NONE);
    assert(!IsIdentityColorBlit(fbos[0], fbos[0], 0, 0, 4, 4,
                               0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST));
    assert(glGetError() == GL_NO_ERROR);
    glReadBuffer(color0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    assert(!Blit()); (void)glGetError();
    glDrawBuffers(1, &color0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                           GL_TEXTURE_2D, textures[1], 0);
    const GLenum both[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, both);
    assert(!Blit() && glGetError() == GL_INVALID_OPERATION);
    const GLenum second_only[] = {GL_NONE, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, second_only);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    assert(actual == zeros && actual != expected);
    glReadBuffer(color0);
    assert(glGetError() == GL_NO_ERROR);
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    const GLenum different_attachment_error = glGetError();
    assert(!Blit() && glGetError() == different_attachment_error);
    std::printf("different-attachment native-control-error=%x preserved\n",
                different_attachment_error);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    assert(actual == (different_attachment_error == GL_NO_ERROR ? expected : zeros));
    assert(glGetError() == GL_NO_ERROR);
    glReadBuffer(color0); glDrawBuffers(1, &color0);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA,
                    GL_UNSIGNED_BYTE, zeros.data());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[1]);
    assert(!Blit() && glGetError() == GL_NO_ERROR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
    assert(actual == expected && glGetError() == GL_NO_ERROR);
    glBindFramebuffer(GL_FRAMEBUFFER, fbos[2]);
    assert(!Blit() && glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    assert(!Blit()); (void)glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
    for (GLint limit : {0, 17}) {
        injected_max = limit;
        assert(!Blit() && glGetError() == GL_INVALID_OPERATION);
    }
    injected_max = -1;
    assert(Blit() && glGetError() == GL_NO_ERROR);
    assert(native_calls == 13 && skipped_calls == 3);
    std::printf("PASS native=%u skipped=%u guard-only-READ_NONE=1 pixels/state/pending-error/routing/capability-guards\n",
                native_calls, skipped_calls);
    glDeleteFramebuffers(3, fbos); glDeleteTextures(2, textures);
    assert(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    assert(eglDestroySurface(display, surface) && eglDestroyContext(display, context));
    assert(eglTerminate(display));
}
