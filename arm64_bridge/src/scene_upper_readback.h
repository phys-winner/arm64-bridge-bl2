#pragma once

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

void PreserveSceneAuditGlError(GLenum error);

inline int SceneAuditModeFromValue(const char* value) {
    if (!value || !*value || std::strcmp(value, "0") == 0 ||
        std::strcmp(value, "false") == 0) return 0;
    return std::strcmp(value, "2") == 0 ? 2 : 1;
}

inline bool SceneUpperTakeDraw(uint32_t draw, uint32_t& previous) {
    if (draw <= previous) return false;
    previous = draw;
    return true;
}

struct SceneUpperReadback {
    static constexpr GLsizei width = 896, height = 512;
    GLint viewport[4] = {};
    GLint draw_framebuffer = 0, program = 0;
    GLint object_type = GL_NONE, object = 0, level = -1, samples = 0;
    GLint target_width = 0, target_height = 0, x = 0, y = 0;
    GLint texture_unit = -1, available_units = -1, scanned_units = 0;
    GLint component_type = 0, implementation_format = 0, implementation_type = 0;
    GLenum type = GL_NONE, error = GL_NO_ERROR, restore_error = GL_NO_ERROR;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface read_surface = EGL_NO_SURFACE;
    std::vector<uint8_t> pixels;
};

inline bool SceneUpperRoi(const GLint* viewport, GLint target_width,
                          GLint target_height, GLint& x, GLint& y) {
    if (viewport[0] < 0 || viewport[1] < 0 ||
        viewport[2] < SceneUpperReadback::width ||
        viewport[3] < SceneUpperReadback::height) return false;
    const int64_t right = int64_t(viewport[0]) + SceneUpperReadback::width;
    const int64_t top = int64_t(viewport[1]) + viewport[3];
    if (right > target_width || top > target_height) return false;
    x = viewport[0];
    y = static_cast<GLint>(top - SceneUpperReadback::height);
    return true;
}

// Called with the actual read framebuffer bound. No texture is rebound to a
// guessed target; unsupported or unbound attachment objects remain unobserved.
inline bool SceneUpperExtent(GLuint framebuffer, SceneUpperReadback& out) {
    if (!framebuffer) {
        out.object_type = GL_FRAMEBUFFER_DEFAULT;
        out.read_surface = eglGetCurrentSurface(EGL_READ);
        const EGLDisplay display = eglGetCurrentDisplay();
        if (display == EGL_NO_DISPLAY || out.read_surface == EGL_NO_SURFACE ||
            out.read_surface != eglGetCurrentSurface(EGL_DRAW)) return false;
        if (!eglQuerySurface(display, out.read_surface, EGL_WIDTH, &out.target_width) ||
            !eglQuerySurface(display, out.read_surface, EGL_HEIGHT, &out.target_height)) return false;
        glGetIntegerv(GL_SAMPLES, &out.samples);
        return true;
    }
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &out.object_type);
    if (out.object_type != GL_TEXTURE && out.object_type != GL_RENDERBUFFER) return false;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &out.object);
    if (out.object <= 0) return false;
    if (out.object_type == GL_RENDERBUFFER) {
        GLint old_renderbuffer = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(out.object));
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &out.target_width);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &out.target_height);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &out.samples);
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(old_renderbuffer));
        return out.samples == 0;
    }
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &out.level);
    GLint old_active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &out.available_units);
    // ponytail: scan at most128 native bindings; skip outputs beyond this cap.
    // Increase it only after a recorded unsupported output needs that coverage.
    const GLint scan_limit = std::clamp(out.available_units, 0, 128);
    for (GLint unit = 0; unit < scan_limit; ++unit) {
        ++out.scanned_units;
        glActiveTexture(GL_TEXTURE0 + unit);
        GLint texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        if (texture != out.object) continue;
        out.texture_unit = unit;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, out.level, GL_TEXTURE_WIDTH, &out.target_width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, out.level, GL_TEXTURE_HEIGHT, &out.target_height);
        break;
    }
    glActiveTexture(static_cast<GLenum>(old_active));
    return out.texture_unit >= 0;
}

inline bool CaptureUpperSceneFramebuffer(GLuint framebuffer, SceneUpperReadback& out) {
    out.context = eglGetCurrentContext();
    if (out.context == EGL_NO_CONTEXT) return false;
    out.error = glGetError();
    PreserveSceneAuditGlError(out.error);
    if (out.error != GL_NO_ERROR) return false;
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &out.draw_framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &out.program);
    glGetIntegerv(GL_VIEWPORT, out.viewport);
    if (major < 3 || (major == 3 && minor < 1) ||
        out.draw_framebuffer != static_cast<GLint>(framebuffer)) return false;

    GLint old_read = 0, old_read_buffer = 0, target_read_buffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &target_read_buffer);
    const GLenum attachment = framebuffer ? GL_COLOR_ATTACHMENT0 : GL_BACK;
    bool ready = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        SceneUpperExtent(framebuffer, out) &&
        SceneUpperRoi(out.viewport, out.target_width, out.target_height, out.x, out.y);
    if (ready) {
        glReadBuffer(attachment);
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
            GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &out.component_type);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &out.implementation_format);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &out.implementation_type);
        if (out.component_type == GL_FLOAT) out.type = GL_FLOAT;
        else if (out.component_type == GL_UNSIGNED_NORMALIZED ||
                 out.component_type == GL_SIGNED_NORMALIZED) out.type = GL_UNSIGNED_BYTE;
        else ready = false;
    }
    GLint pack_buffer = 0, alignment = 0, row = 0, skip_pixels = 0, skip_rows = 0;
    if (ready) {
        out.pixels.resize(size_t(SceneUpperReadback::width) * SceneUpperReadback::height *
                          (out.type == GL_FLOAT ? 16 : 4));
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glReadPixels(out.x, out.y, SceneUpperReadback::width, SceneUpperReadback::height,
                     GL_RGBA, out.type, out.pixels.data());
        out.error = glGetError();
        PreserveSceneAuditGlError(out.error);
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, row);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));
    } else {
        out.error = glGetError();
        PreserveSceneAuditGlError(out.error);
    }
    glReadBuffer(static_cast<GLenum>(target_read_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
    out.restore_error = glGetError();
    PreserveSceneAuditGlError(out.restore_error);
    return ready && out.error == GL_NO_ERROR && out.restore_error == GL_NO_ERROR;
}
