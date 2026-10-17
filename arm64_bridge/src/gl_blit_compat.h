#pragma once

#include <GLES3/gl32.h>

// GLES rejects a self-blit even when every destination pixel is unchanged.
// Translate only the guest's complete, single-image identity color copy to a
// no-op. Query real selectors, including with GL diagnostics/cache disabled.
inline bool IsIdentityColorBlit(GLint read_framebuffer, GLint draw_framebuffer,
                                GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                GLbitfield mask, GLenum filter) {
    if (read_framebuffer == 0 || read_framebuffer != draw_framebuffer ||
        sx0 != dx0 || sy0 != dy0 || sx1 != dx1 || sy1 != dy1 ||
        sx1 <= sx0 || sy1 <= sy0 || mask != GL_COLOR_BUFFER_BIT ||
        filter != GL_NEAREST) return false;
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    GLint read_buffer = GL_NONE, max_draw_buffers = 0;
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
    // ponytail: at most16 selectors; larger hosts retain the native operation
    // until their full routing is checked rather than accepting a partial view.
    if (read_buffer == GL_NONE || max_draw_buffers < 1 || max_draw_buffers > 16)
        return false;
    bool has_draw_buffer = false;
    for (GLint i = 0; i < max_draw_buffers; ++i) {
        GLint draw_buffer = GL_NONE;
        glGetIntegerv(GL_DRAW_BUFFER0 + i, &draw_buffer);
        if (draw_buffer == GL_NONE) continue;
        has_draw_buffer = true;
        if (draw_buffer != read_buffer) return false;
    }
    return has_draw_buffer;
}
