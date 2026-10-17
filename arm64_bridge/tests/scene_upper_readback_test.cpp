#include "scene_upper_readback.h"

#include <array>
#include <cassert>
#include <climits>
#include <cstdio>

static GLenum deferred_error = GL_NO_ERROR;
void PreserveSceneAuditGlError(GLenum error) {
    if (error != GL_NO_ERROR && deferred_error == GL_NO_ERROR) deferred_error = error;
}

static GLint Get(GLenum name) { GLint value = 0; glGetIntegerv(name, &value); return value; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(SceneAuditModeFromValue(nullptr) == 0 && SceneAuditModeFromValue("") == 0);
    assert(SceneAuditModeFromValue("0") == 0 && SceneAuditModeFromValue("false") == 0);
    assert(SceneAuditModeFromValue("2") == 2 && SceneAuditModeFromValue("1") == 1);
    assert(SceneAuditModeFromValue("true") == 1 && SceneAuditModeFromValue("02") == 1);
    uint32_t last_draw = 0;
    assert(!SceneUpperTakeDraw(0, last_draw));
    assert(SceneUpperTakeDraw(2, last_draw) && !SceneUpperTakeDraw(2, last_draw));
    assert(SceneUpperTakeDraw(30, last_draw) && !SceneUpperTakeDraw(2, last_draw));
    GLint x = -1, y = -1;
    GLint vp[] = {7, 11, 896, 640};
    assert(SceneUpperRoi(vp, 903, 651, x, y) && x == 7 && y == 139);
    assert(!SceneUpperRoi(vp, 902, 651, x, y));
    assert(!SceneUpperRoi(vp, 903, 650, x, y));
    vp[0] = -1; assert(!SceneUpperRoi(vp, INT_MAX, INT_MAX, x, y));
    vp[0] = INT_MAX; assert(!SceneUpperRoi(vp, INT_MAX, INT_MAX, x, y));
    vp[0] = 0; vp[1] = INT_MAX; assert(!SceneUpperRoi(vp, INT_MAX, INT_MAX, x, y));
    vp[1] = 0; vp[2] = 895; assert(!SceneUpperRoi(vp, 896, 640, x, y));
    vp[2] = 896; vp[3] = 511; assert(!SceneUpperRoi(vp, 896, 640, x, y));

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr));
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config{}; EGLint count = 0;
    assert(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1);
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint surf_attrs[] = {EGL_WIDTH, 896, EGL_HEIGHT, 640, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attrs);
    EGLSurface surface = eglCreatePbufferSurface(display, config, surf_attrs);
    assert(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE);
    assert(eglMakeCurrent(display, surface, surface, context));
    std::printf("GPU=%s GLES=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint texture = 0, fbo = 0, renderbuffer = 0, rb_fbo = 0, pbo = 0;
    glGenTextures(1, &texture); glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1792, 1280, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 896, 640, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 1);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, 896, 640);
    glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST); glScissor(0, 128, 896, 512);
    glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
    glReadBuffer(GL_NONE);
    glGenRenderbuffers(1, &renderbuffer); glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 896, 640);
    glGenFramebuffers(1, &rb_fbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, rb_fbo);
    glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
    glGenBuffers(1, &pbo); glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, 16, nullptr, GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 8); glPixelStorei(GL_PACK_ROW_LENGTH, 1024);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 3); glPixelStorei(GL_PACK_SKIP_ROWS, 2);
    glActiveTexture(GL_TEXTURE7);
    assert(glGetError() == GL_NO_ERROR);
    const auto state = [] { return std::array<GLint, 10>{Get(GL_READ_FRAMEBUFFER_BINDING),
        Get(GL_READ_BUFFER), Get(GL_DRAW_FRAMEBUFFER_BINDING), Get(GL_PIXEL_PACK_BUFFER_BINDING),
        Get(GL_PACK_ALIGNMENT), Get(GL_PACK_ROW_LENGTH), Get(GL_PACK_SKIP_PIXELS),
        Get(GL_PACK_SKIP_ROWS), Get(GL_ACTIVE_TEXTURE), Get(GL_RENDERBUFFER_BINDING)}; };
    const auto before = state();
    SceneUpperReadback capture;
    assert(CaptureUpperSceneFramebuffer(fbo, capture));
    assert(state() == before && capture.x == 0 && capture.y == 128 && capture.texture_unit == 3);
    assert(capture.target_width == 896 && capture.target_height == 640);
    assert(capture.level == 1);
    assert(capture.scanned_units == 4 && capture.available_units >= 4);
    assert(capture.type == GL_UNSIGNED_BYTE && capture.pixels.size() == 1835008);
    for (size_t i = 0; i < capture.pixels.size(); i += 4) {
        assert(capture.pixels[i] == 0 && capture.pixels[i+1] == 0 &&
               capture.pixels[i+2] == 255 && capture.pixels[i+3] == 255);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo); assert(Get(GL_READ_BUFFER) == GL_NONE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rb_fbo);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0); glActiveTexture(GL_TEXTURE7);
    SceneUpperReadback unbound;
    assert(!CaptureUpperSceneFramebuffer(fbo, unbound) && state() == before);
    assert(unbound.error == GL_NO_ERROR && unbound.texture_unit == -1);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, texture); glActiveTexture(GL_TEXTURE7);
    glViewport(0, 0, 895, 640); SceneUpperReadback narrow;
    assert(!CaptureUpperSceneFramebuffer(fbo, narrow) && state() == before);
    glViewport(0, 0, 896, 640);
    glEnable(0xFFFFFFFFu); SceneUpperReadback pending;
    assert(!CaptureUpperSceneFramebuffer(fbo, pending) && deferred_error == GL_INVALID_ENUM);
    assert(state() == before && glGetError() == GL_NO_ERROR); deferred_error = GL_NO_ERROR;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rb_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    const auto rb_before = state(); SceneUpperReadback rb;
    assert(CaptureUpperSceneFramebuffer(rb_fbo, rb) && state() == rb_before);
    assert(rb.object_type == GL_RENDERBUFFER && rb.target_width == 896 && rb.samples == 0);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 2, GL_RGBA8, 896, 640);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    const auto ms_before = state(); SceneUpperReadback ms;
    assert(!CaptureUpperSceneFramebuffer(rb_fbo, ms) && ms.samples > 0 && state() == ms_before);

    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 896, 640, 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    const GLfloat color[] = {0.25f, 0.5f, 0.75f, 1.0f}; glClearBufferfv(GL_COLOR, 0, color);
    glActiveTexture(GL_TEXTURE7);
    const auto fp_before = state(); SceneUpperReadback fp;
    assert(CaptureUpperSceneFramebuffer(fbo, fp) && state() == fp_before);
    assert(fp.type == GL_FLOAT && fp.pixels.size() == 7340032);
    for (size_t i = 0; i < fp.pixels.size(); i += 16) {
        GLfloat value[4]; std::memcpy(value, fp.pixels.data()+i, 16);
        for (unsigned c = 0; c < 4; ++c) assert(value[c] == color[c]);
    }
    glActiveTexture(GL_TEXTURE3);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 896, 640, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glActiveTexture(GL_TEXTURE7);
    assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    const auto integer_before = state(); SceneUpperReadback integer;
    assert(!CaptureUpperSceneFramebuffer(fbo, integer) && state() == integer_before);
    assert(integer.component_type == GL_UNSIGNED_INT && integer.error == GL_NO_ERROR);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    const auto default_before = state(); SceneUpperReadback def;
    assert(CaptureUpperSceneFramebuffer(0, def) && state() == default_before);
    assert(def.read_surface == surface && def.target_width == 896 && def.target_height == 640);
    assert(def.type == GL_UNSIGNED_BYTE && def.y == 128);
    assert(def.samples == Get(GL_SAMPLES) && def.available_units == -1 && def.scanned_units == 0);
    for (size_t i = 0; i < def.pixels.size(); i += 4) {
        assert(def.pixels[i] == 0 && def.pixels[i+1] == 255 &&
               def.pixels[i+2] == 0 && def.pixels[i+3] == 255);
    }
    EGLSurface other = eglCreatePbufferSurface(display, config, surf_attrs);
    assert(other != EGL_NO_SURFACE && eglMakeCurrent(display, surface, other, context));
    SceneUpperReadback split; assert(!CaptureUpperSceneFramebuffer(0, split));
    assert(glGetError() == GL_NO_ERROR && deferred_error == GL_NO_ERROR);
    assert(eglGetError() == EGL_SUCCESS);
    assert(eglMakeCurrent(display, surface, surface, context));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glDeleteBuffers(1, &pbo);
    glDeleteFramebuffers(1, &fbo); glDeleteFramebuffers(1, &rb_fbo);
    glDeleteRenderbuffers(1, &renderbuffer); glDeleteTextures(1, &texture);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, other); eglDestroySurface(display, surface);
    eglDestroyContext(display, context); eglTerminate(display);
    std::puts("PASS scene upper ROI, texture/float/RBO/default pixels, skips, GL state and pending error");
}
