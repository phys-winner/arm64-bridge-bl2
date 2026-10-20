#pragma once
// QA-only exact Cliffs sky selector and bounded output attachment query.
// Include after GLES declarations. This header never grants a generic shader gate.
#if defined(BL2_ENABLE_QA_CONSOLE) && BL2_ENABLE_QA_CONSOLE
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
void PreserveSceneAuditGlError(GLenum error);
namespace bl2_sky_qa {
inline constexpr std::string_view kName = "DLC/Orchid/OasisScene/TBasePassPixelShaderFNoLightMapPolicySkyLight_a0428c0a387ca840b3a36e69a8832c7e";
inline constexpr std::array<uint8_t, 32> kDigest = {0x98, 0xd5, 0xd0, 0x06, 0xbd, 0x23, 0xae, 0xd6, 0xa3, 0xbc, 0x0a, 0xba, 0x39, 0x10, 0x23, 0xdb, 0x65, 0x75, 0x57, 0x47, 0xcb, 0xd0, 0x8d, 0x16, 0x6f, 0x0c, 0x76, 0x35, 0xbe, 0x43, 0x50, 0x8b};
struct StageCounts {
    bool active=false, finished=false;
    uint64_t deadline=0;
    uint32_t presents=0, drawEntries=0, explicitDraws=0, legacyDraws=0;
    uint32_t postDraws=0, eligibleFbo=0, exactName=0, exactHash=0;
    uint32_t entries=0, optInRejected=0, selected=0, validated=0;
    uint32_t guardRejected[4]={};
    uint32_t attempts=0, complete=0;
    // One render-thread summary after six seconds, or earlier exact disarm.
    const char* Present(bool requested, uint64_t now) {
        if (finished) return nullptr;
        if (!active) {
            if (!requested) return nullptr;
            active=true;
            deadline=now+6'000'000'000ull;
        }
        ++presents;
        if (requested && now < deadline) return nullptr;
        active=false;
        finished=true;
        return requested ? "deadline" : "disarmed";
    }
};
inline StageCounts& Counts() { static thread_local StageCounts counts; return counts; }
inline bool Enabled() {
    // Set before the first scene-audit draw; existing bridge scene_audit can
    // still be armed after loading. Native32 already caches scene_audit at launch.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.scene_audit", value) > 0 &&
               std::strcmp(value, "sky83df") == 0;
    }();
    return enabled;
}
inline bool Matches(std::string_view name, const std::array<uint8_t, 32>& digest) {
    return name == kName && digest == kDigest;
}
struct Output {
    GLint width=0, height=0, internal=0, samples=0, unit=-1;
    GLenum error=GL_NO_ERROR;
    GLint componentType=0, redBits=0, greenBits=0, blueBits=0, alphaBits=0, colorEncoding=0;
    bool componentsObserved=false, observed=false;
};
inline Output QueryOutput(GLint type, GLint object, GLint level) {
    Output out;
    if (object <= 0) return out;
    // Attachment metadata needs no texture-target guess or temporary binding.
    if (type==GL_RENDERBUFFER || (type==GL_TEXTURE && level>=0)) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,&out.componentType);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,&out.redBits);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE,&out.greenBits);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE,&out.blueBits);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE,&out.alphaBits);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,&out.colorEncoding);
    }
    if (type == GL_RENDERBUFFER) {
        GLint previous=0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(object));
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &out.width);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &out.height);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &out.internal);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &out.samples);
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous));
        out.observed=out.width>0 && out.height>0;
    } else if (type == GL_TEXTURE && level >= 0) {
        // Only query an actual 2D binding of this exact output object. Never
        // bind an unknown texture name to a guessed target or infer its format.
        GLint previous=0, available=0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previous);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &available);
        for (GLint unit=0; unit<std::clamp(available,0,128); ++unit) {
            glActiveTexture(GL_TEXTURE0+static_cast<GLenum>(unit));
            GLint bound=0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            if (bound != object) continue;
            out.unit=unit;
            glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_WIDTH,&out.width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_HEIGHT,&out.height);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_INTERNAL_FORMAT,&out.internal);
            out.observed=out.width>0 && out.height>0;
            break;
        }
        glActiveTexture(static_cast<GLenum>(previous));
    }
    out.error=glGetError();
    out.componentsObserved=out.error==GL_NO_ERROR && out.componentType!=0 &&
        out.redBits>0 && out.greenBits>0 && out.blueBits>0;
    PreserveSceneAuditGlError(out.error);
    return out;
}
}  // namespace bl2_sky_qa
#endif
