#include "bl2_fog_audit.h"
#include "bridge_log.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/system_properties.h>

#define LOG_TAG "BL2_FogAudit"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)

extern bool CopyGlBufferShadow(GLuint, size_t, size_t, void*);

namespace {
struct Audit {
    bool enabled = false;
    bool armed = false;
    bool pending = false;
    bool pixel_mode = false;
    bool final_mode = false;
    bool final_pending = false;
    GLuint final_fbo = 0;
    unsigned session = 0, frame = 0, draws = 0, fog = 0, completed = 0;
    std::chrono::steady_clock::time_point start;
};
thread_local Audit state;
constexpr unsigned kMaxFrames = 360;
constexpr unsigned kMaxSessions = 3;

// Diagnostic only: two points on opposite sides of the observed menu flashes
// and one central point. All three fit both the 2400x1080 native surface and
// its full-resolution scene targets.
constexpr std::array<std::array<GLint, 2>, 3> kPixelPoints{{
    {{240, 180}}, {{1200, 540}}, {{1920, 180}}
}};

void PixelProbe(const char* phase, GLuint framebuffer) {
    GLint previous_read = 0, previous_buffer = 0, previous_pack_buffer = 0;
    GLint previous_alignment = 0, previous_row = 0;
    GLint previous_skip_pixels = 0, previous_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_READ_BUFFER, &previous_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &previous_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &previous_skip_rows);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadBuffer(framebuffer ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);

    std::array<uint16_t, 12> hdr{};
    std::array<uint8_t, 12> display{};
    for (size_t i = 0; i < kPixelPoints.size(); ++i) {
        const auto& xy = kPixelPoints[i];
        if (framebuffer) {
            glReadPixels(xy[0], xy[1], 1, 1, GL_RGBA, GL_HALF_FLOAT,
                         hdr.data() + 4 * i);
        } else {
            glReadPixels(xy[0], xy[1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                         display.data() + 4 * i);
        }
    }
    const GLenum error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, previous_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, previous_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, previous_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, previous_skip_rows);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previous_pack_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
    glReadBuffer(static_cast<GLenum>(previous_buffer));
    if (framebuffer) {
        LOGI("FOG_PIXELS session=%u frame=%u phase=%s fbo=%u error=0x%X "
             "p0=%04X,%04X,%04X,%04X p1=%04X,%04X,%04X,%04X "
             "p2=%04X,%04X,%04X,%04X",
             state.session, state.frame, phase, framebuffer, error,
             hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7],
             hdr[8], hdr[9], hdr[10], hdr[11]);
    } else {
        LOGI("FOG_PIXELS session=%u frame=%u phase=%s fbo=0 error=0x%X "
             "p0=%u,%u,%u,%u p1=%u,%u,%u,%u p2=%u,%u,%u,%u",
             state.session, state.frame, phase, error,
             display[0], display[1], display[2], display[3],
             display[4], display[5], display[6], display[7],
             display[8], display[9], display[10], display[11]);
    }
}

void FinalSourceProbe(GLuint texture) {
    GLint previous_read = 0, previous_buffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_READ_BUFFER, &previous_buffer);
    GLuint temporary = 0;
    glGenFramebuffers(1, &temporary);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, temporary);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        PixelProbe("final-source", temporary);
    } else {
        LOGI("FINAL_SOURCE_UNAVAILABLE session=%u frame=%u texture=%u status=0x%X",
             state.session, state.frame, texture, status);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
    glReadBuffer(static_cast<GLenum>(previous_buffer));
    glDeleteFramebuffers(1, &temporary);
}

uint64_t Hash(const void* pointer, size_t bytes) {
    uint64_t value = 1469598103934665603ULL;
    const auto* data = static_cast<const uint8_t*>(pointer);
    for (size_t i = 0; i < bytes; ++i) value = (value ^ data[i]) * 1099511628211ULL;
    return value;
}

void Uniforms(GLuint program, const char* bank, unsigned count) {
    // Driver values are queried independently of the NV upload cache.
    std::array<float, 32> values{};
    uint32_t active = 0;
    for (unsigned i = 0; i < count && i < 8; ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "%s[%u]", bank, i);
        const GLint location = glGetUniformLocation(program, name);
        if (location < 0) continue;
        active |= 1u << i;
        glGetUniformfv(program, location, values.data() + 4 * i);
    }
    LOGI("FOG_UNIFORMS session=%u frame=%u draw=%u bank=%s active=0x%X hash=%016llX "
         "v0=%g,%g,%g,%g v1=%g,%g,%g,%g v2=%g,%g,%g,%g v3=%g,%g,%g,%g "
         "v4=%g,%g,%g,%g v5=%g,%g,%g,%g v6=%g,%g,%g,%g v7=%g,%g,%g,%g",
         state.session, state.frame, state.draws, bank, active,
         static_cast<unsigned long long>(Hash(values.data(), values.size() * sizeof(float))),
         values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7],
         values[8],values[9],values[10],values[11],values[12],values[13],values[14],values[15],
         values[16],values[17],values[18],values[19],values[20],values[21],values[22],values[23],
         values[24],values[25],values[26],values[27],values[28],values[29],values[30],values[31]);
}
}  // namespace

bool Bl2FogAuditActive() { return state.enabled && !state.final_mode; }

bool Bl2FinalAuditActive() { return state.enabled && state.final_mode; }

void Bl2FinalAuditDraw() {
    if (!Bl2FinalAuditActive()) return;
    GLint viewport[4] = {}, framebuffer = 0, program = 0;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer <= 0 || viewport[2] < 2000 || viewport[3] < 1000) return;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    const GLint sampler_location = glGetUniformLocation(program, "PSampler0");
    GLint unit = -1;
    if (sampler_location >= 0) glGetUniformiv(program, sampler_location, &unit);
    if (unit < 0 || unit >= 16) return;
    GLint previous_active = 0, texture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glActiveTexture(static_cast<GLenum>(previous_active));
    if (texture <= 0) return;
    state.final_pending = true;
    state.final_fbo = static_cast<GLuint>(framebuffer);
    FinalSourceProbe(static_cast<GLuint>(texture));
    if (state.frame < 8) {
        LOGI("FINAL_PASS session=%u frame=%u program=%d fbo=%d "
             "source=%d sampler_unit=%d viewport=%dx%d",
             state.session, state.frame, program, framebuffer, texture, unit,
             viewport[2], viewport[3]);
    }
}

void Bl2FogAuditDraw(bool fog, const char* operation, GLint first, GLsizei count,
                    GLenum type, uint32_t indices, GLint base, GLsizei instances) {
    if (!state.enabled) return;
    state.pending = false;
    ++state.draws;
    if (!fog) return;
    ++state.fog;
    state.pending = true;
    if (state.fog > 4) return;
    if (state.pixel_mode) {
        GLint framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        if (framebuffer > 0) PixelProbe("before", static_cast<GLuint>(framebuffer));
        return;
    }
    GLint program=0, fbo=0, color_type=0, color=0, viewport[4]={};
    GLint blend_src=0, blend_dst=0, depth_func=0, cull_mode=0;
    GLboolean mask[4]={}, depth_mask=0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    if (fbo) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &color_type);
        if (color_type != GL_NONE) glGetFramebufferAttachmentParameteriv(
            GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &color);
    }
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    GLint active=0, unit=-1, texture=0, sampler=0;
    const GLint sampler_location=glGetUniformLocation(program, "PSampler0");
    if (sampler_location >= 0) glGetUniformiv(program, sampler_location, &unit);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    if (unit >= 0 && unit < 16) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
        glActiveTexture(static_cast<GLenum>(active));
    }
    LOGI("FOG_DRAW session=%u frame=%u draw=%u fog=%u operation=%s first=%d count=%d "
         "type=0x%X indices=0x%X base=%d instances=%d program=%d fbo=%d color=%d "
         "viewport=%d,%d,%d,%d blend=%d/%X/%X depth=%d/%X/%d cull=%d/%X "
         "mask=%d,%d,%d,%d sampler_unit=%d texture=%d sampler=%d",
         state.session,state.frame,state.draws,state.fog,operation,first,count,type,indices,base,
         instances,program,fbo,color,viewport[0],viewport[1],viewport[2],viewport[3],
         glIsEnabled(GL_BLEND),blend_src,blend_dst,glIsEnabled(GL_DEPTH_TEST),depth_func,depth_mask,
         glIsEnabled(GL_CULL_FACE),cull_mode,mask[0],mask[1],mask[2],mask[3],unit,texture,sampler);
    Uniforms(program,"VConstFloat",7);
    Uniforms(program,"VConstGlobal",5);
    Uniforms(program,"PConstFloat",8);

    GLint vbo=0, ebo=0, size=0, element_type=0, stride=0;
    void* pointer=nullptr;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &vbo);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &element_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo);
    std::array<uint32_t, 6> ids{};
    bool index_ok=first>=0 && count>0;
    const unsigned n=count>0?static_cast<unsigned>(count < 6 ? count : 6):0;
    if (type==GL_NONE) {
        for(unsigned i=0;i<n;++i) ids[i]=static_cast<uint32_t>(first)+i;
    } else if (type==GL_UNSIGNED_SHORT && ebo>0) {
        std::array<uint16_t,6> raw{};
        index_ok=CopyGlBufferShadow(ebo,indices,n*sizeof(uint16_t),raw.data());
        for(unsigned i=0;i<n;++i) ids[i]=raw[i];
    } else if (type==GL_UNSIGNED_INT && ebo>0) {
        index_ok=CopyGlBufferShadow(ebo,indices,n*sizeof(uint32_t),ids.data());
    } else index_ok=false;
    std::array<float,24> positions{};
    bool vertex_ok=index_ok && vbo>0 && element_type==GL_FLOAT && size>=2 && size<=4 && stride>=0;
    const size_t record_stride=stride>0?static_cast<size_t>(stride):static_cast<size_t>(size)*4;
    for(unsigned i=0;vertex_ok && i<n;++i) {
        const int64_t id=static_cast<int64_t>(ids[i])+base;
        if(id<0 || id>1000000) {vertex_ok=false;break;}
        const uint64_t offset=reinterpret_cast<uintptr_t>(pointer)+static_cast<uint64_t>(id)*record_stride;
        if(offset>std::numeric_limits<size_t>::max()) {vertex_ok=false;break;}
        vertex_ok=CopyGlBufferShadow(vbo,static_cast<size_t>(offset),size*4,positions.data()+4*i);
    }
    LOGI("FOG_VERTICES session=%u frame=%u draw=%u vbo=%d ebo=%d offset=%llu "
         "size=%d type=0x%X stride=%d index_ok=%d shadow_ok=%d "
         "ids=%u,%u,%u,%u,%u,%u xy=%g,%g;%g,%g;%g,%g;%g,%g;%g,%g;%g,%g hash=%016llX",
         state.session,state.frame,state.draws,vbo,ebo,
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pointer)),
         size,element_type,stride,index_ok,vertex_ok,ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],
         positions[0],positions[1],positions[4],positions[5],positions[8],positions[9],
         positions[12],positions[13],positions[16],positions[17],positions[20],positions[21],
         static_cast<unsigned long long>(Hash(positions.data(),positions.size()*sizeof(float))));
}

void Bl2FogAuditCompleted() {
    if (state.enabled && state.final_pending) {
        PixelProbe("final-after", state.final_fbo);
        state.final_pending = false;
    }
    if (state.enabled && state.pending) {
        if (state.pixel_mode && state.fog <= 4) {
            GLint framebuffer = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
            if (framebuffer > 0) PixelProbe("after", static_cast<GLuint>(framebuffer));
        }
        ++state.completed;
        state.pending = false;
    }
}

void Bl2FogAuditPresent() {
    char value[PROP_VALUE_MAX]={};
    __system_property_get("debug.bl2.fog_audit",value);
    const unsigned requested_mode = value[0]=='1' ? 1u :
                                    value[0]=='2' ? 2u :
                                    value[0]=='3' ? 3u : 0u;
    if (state.enabled) {
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-state.start).count();
        if (state.pixel_mode || state.final_mode) {
            PixelProbe("present", 0);
        } else {
            LOGI("FOG_FRAME session=%u frame=%u elapsed_ms=%lld draws=%u fog=%u completed=%u pending=%d",
                 state.session,state.frame,static_cast<long long>(elapsed),state.draws,state.fog,
                 state.completed,state.pending);
        }
        ++state.frame;
        state.draws=state.fog=state.completed=0;
        state.pending=false;
        state.final_pending=false;
        const bool readback_mode = state.pixel_mode || state.final_mode;
        const unsigned max_frames = readback_mode ? 9000u : kMaxFrames;
        const long long max_ms = readback_mode ? 300000LL : 20000LL;
        const unsigned current_mode = state.final_mode ? 3u :
                                      state.pixel_mode ? 2u : 1u;
        if (requested_mode != current_mode ||
            state.frame>=max_frames || elapsed>=max_ms) {
            state.enabled=false;
            LOGI("FOG_AUDIT_END session=%u mode=%u frames=%u",state.session,
                 current_mode,state.frame);
        }
    }
    if (!requested_mode) state.armed=false;
    if (requested_mode && !state.armed && state.session<kMaxSessions) {
        state.armed=true;state.enabled=true;++state.session;
        state.pixel_mode=requested_mode==2u;
        state.final_mode=requested_mode==3u;
        state.frame=state.draws=state.fog=state.completed=0;
        state.pending=state.final_pending=false;
        state.start=std::chrono::steady_clock::now();
        LOGI("FOG_AUDIT_BEGIN session=%u mode=%u max_frames=%u max_ms=%lld readbacks=%d",
             state.session,requested_mode,
             state.pixel_mode || state.final_mode ? 9000u : kMaxFrames,
             state.pixel_mode || state.final_mode ? 300000LL : 20000LL,
             state.pixel_mode || state.final_mode ? 9 : 0);
    }
}
