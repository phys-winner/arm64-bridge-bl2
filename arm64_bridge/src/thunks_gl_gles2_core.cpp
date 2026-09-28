#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "thunk_types.h"

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define LOG_TAG "Arm32Bridge_GLES2Core"
#include "bridge_log.h"
#include "bl2_fog_audit.h"
#include "perf_probe.h"
#include "guest_mapped_dirty_tracker.h"
#include "gl_blit_compat.h"
#include "scene_upper_readback.h"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

bool HasGlES3();
GLenum TranslateLegacyGlExternalFormat(GLenum format);
bool CopyGlBufferShadow(GLuint buffer, size_t offset, size_t size, void* output);
void TrackGlBufferBinding(GLenum target, GLuint buffer);
void UpdateGlBufferShadow(GLuint buffer, size_t offset, size_t size,
                          const void* data);
size_t SyncChangedGlBufferMapping(GLuint buffer, size_t offset, size_t size,
                                  void* native_mapping, const void* guest_data,
                                  bool force_full);
bool RefreshLegacyProgramStateForDraw();
bool CurrentLegacyPipelineIsGfxText();
bool CurrentLegacyPipelineIsPostprocessBlend();
void SceneAuditLogFinalQuadDraw(const char* operation, GLint first,
                                GLsizei count, GLsizei instances);
void SceneAuditLogFinalIndexedDraw(const char* operation, GLsizei count,
                                   GLenum type, const void* indices,
                                   GLint base_vertex, GLsizei instances);
void SceneAuditLogFinalInvocation(const char* operation, GLint first,
                                  GLsizei count, GLenum type,
                                  uint32_t indices_address,
                                  GLint base_vertex, GLsizei instances);
GLuint GetBoundGlBuffer(GLenum target);
void PreserveSceneAuditGlError(GLenum error);
bool GlStateAuditEnabled();
GLenum CaptureGlError(const char* operation);
void SceneAuditCaptureLightingState(uint32_t draw, size_t point);
void SceneAuditCaptureTargetLightingState(GLuint framebuffer, uint32_t draw,
                                         GLuint expected_program, const char* operation);
void InvalidateGlAttribCache();
void InvalidateLegacyTextureCache();
void InvalidateLegacyTextureCacheAcrossContexts();

namespace {

std::mutex s_desktop_occlusion_queries_mutex;
std::unordered_set<GLuint> s_desktop_occlusion_queries;

// The desktop guest expects a sample count, while GLES exposes a boolean.
// Compare bounded positive counts in one running process: UINT_MAX might
// overflow or overdrive a consumer that uses the count for coverage.
int OcclusionProbeMode() {
    static thread_local uint32_t calls = 0;
    static thread_local int mode = 0;
    if ((++calls & 31u) == 1u) {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.horn.occlusion_probe", value);
        const int next = strcmp(value, "log") == 0 ? 1 :
                         strcmp(value, "visible") == 0 ? 2 :
                         strcmp(value, "count65535") == 0 ? 3 :
                         strcmp(value, "count262144") == 0 ? 4 :
                         strcmp(value, "count1048576") == 0 ? 5 : 0;
        if (next != mode) {
            mode = next;
            LOGI("Occlusion probe mode=%d (0=normal,1=log,2=all-visible,"
                 "3=count65535,4=count262144,5=count1048576)", mode);
        }
    }
    return mode;
}

bool DrawAuditEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.draw_audit", value) > 0 &&
               value[0] != '\0' && strcmp(value, "0") != 0 &&
               strcmp(value, "false") != 0;
    }();
    return enabled;
}

bool PresentAuditEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.present_audit", value) > 0 &&
               value[0] != '\0' && strcmp(value, "0") != 0 &&
               strcmp(value, "false") != 0;
    }();
    return enabled;
}

bool AtlasUploadAuditEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.atlas_upload_audit", value) > 0 &&
               value[0] == '1';
    }();
    return enabled;
}

void AuditLuminanceAtlasUpload(GLint texture, GLint pbo, GLint level,
                               GLint x_offset, GLint y_offset,
                               GLsizei width, GLsizei height,
                               uint32_t pixels_address, const void* pixels) {
    if (!AtlasUploadAuditEnabled() || texture <= 0 || level != 0 ||
        width <= 0 || height <= 0 || width > 1024 || height > 1024) return;
    static std::atomic<uint32_t> candidates{0};
    const uint32_t candidate = candidates.fetch_add(1, std::memory_order_relaxed) + 1;
    if (candidate > 128) return;
    GLint unpack_alignment = 0, unpack_row_length = 0;
    GLint unpack_skip_pixels = 0, unpack_skip_rows = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
    const size_t byte_count = static_cast<size_t>(width) * height;
    LOGI("Atlas upload audit #%u texture=%d pbo=%d rect=%d,%d %dx%d offset=%u "
         "unpack={align:%d,row:%d,skipPixels:%d,skipRows:%d}",
         candidate, texture, pbo, x_offset, y_offset, width, height,
         pixels_address, unpack_alignment, unpack_row_length,
         unpack_skip_pixels, unpack_skip_rows);
    if ((unpack_row_length != 0 && unpack_row_length != width) ||
        unpack_skip_pixels != 0 || unpack_skip_rows != 0 ||
        unpack_alignment <= 0 || width % unpack_alignment != 0) {
        LOGI("Atlas upload audit #%u noncontiguous source; comparison skipped",
             candidate);
        return;
    }
    std::vector<uint8_t> source(byte_count);
    if (pbo > 0) {
        if (!CopyGlBufferShadow(static_cast<GLuint>(pbo), pixels_address,
                                byte_count, source.data())) {
            LOGI("Atlas upload audit #%u PBO CPU shadow unavailable", candidate);
            return;
        }
    } else {
        if (!pixels) {
            LOGI("Atlas upload audit #%u direct source unavailable", candidate);
            return;
        }
        std::memcpy(source.data(), pixels, byte_count);
    }
    size_t source_nonzero = 0, source_opaque = 0;
    uint64_t source_hash = 1469598103934665603ull;
    for (uint8_t byte : source) {
        source_nonzero += byte != 0;
        source_opaque += byte == 255;
        source_hash = (source_hash ^ byte) * 1099511628211ull;
    }
    GLint previous_read_fbo = 0, previous_read_buffer = 0;
    GLint previous_pack_buffer = 0, previous_pack_alignment = 0;
    GLint previous_pack_row = 0, previous_pack_skip_pixels = 0;
    GLint previous_pack_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
    glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_pack_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &previous_pack_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &previous_pack_skip_rows);
    const GLenum prior_error = glGetError();
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, static_cast<GLuint>(texture), level);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLenum read_error = GL_NO_ERROR;
    size_t differences = 0, first_difference = byte_count;
    uint8_t source_first = 0, gpu_first = 0;
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        std::vector<uint8_t> rgba(byte_count * 4);
        glReadPixels(x_offset, y_offset, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba.data());
        read_error = glGetError();
        if (read_error == GL_NO_ERROR) {
            for (size_t i = 0; i < byte_count; ++i) {
                const uint8_t actual = rgba[i * 4];
                if (source[i] != actual) {
                    if (first_difference == byte_count) {
                        first_difference = i;
                        source_first = source[i];
                        gpu_first = actual;
                    }
                    ++differences;
                }
            }
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previous_pack_buffer));
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, previous_pack_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, previous_pack_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, previous_pack_skip_rows);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read_fbo));
    glReadBuffer(static_cast<GLenum>(previous_read_buffer));
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    const GLenum cleanup_error = glGetError();
    LOGI("Atlas upload audit #%u texture=%d priorError=0x%04X "
         "fboStatus=0x%04X readError=0x%04X cleanupError=0x%04X differences=%zu/%zu "
         "first=%zu source=%u gpu=%u sourceNonzero=%zu sourceOpaque=%zu "
         "sourceHash=%016llX",
         candidate, texture, prior_error, status, read_error, cleanup_error,
         differences, byte_count, first_difference, source_first, gpu_first,
         source_nonzero, source_opaque,
         static_cast<unsigned long long>(source_hash));
}

void LogByteStats(const char* label, uint32_t index,
                  const uint8_t* bytes, size_t byte_count) {
    if (!bytes || byte_count == 0) {
        LOGE("Present audit #%u %s has no readable bytes", index, label);
        return;
    }
    uint64_t hash = 1469598103934665603ull;
    uint64_t nonzero = 0;
    uint8_t minimum = 255;
    uint8_t maximum = 0;
    for (size_t offset = 0; offset < byte_count; ++offset) {
        const uint8_t value = bytes[offset];
        hash ^= value;
        hash *= 1099511628211ull;
        nonzero += value != 0;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    LOGI("Present audit #%u %s bytes=%zu hash=0x%016llX "
         "nonzero=%llu/%zu range=%u..%u",
         index, label, byte_count, static_cast<unsigned long long>(hash),
         static_cast<unsigned long long>(nonzero), byte_count,
         static_cast<unsigned>(minimum), static_cast<unsigned>(maximum));
}

static float AuditHalfToFloat(uint16_t bits) {
    const uint32_t exponent = (bits >> 10) & 31u;
    const uint32_t mantissa = bits & 1023u;
    const float magnitude = exponent == 0
        ? std::ldexp(static_cast<float>(mantissa), -24)
        : exponent == 31
            ? (mantissa ? std::numeric_limits<float>::quiet_NaN()
                        : std::numeric_limits<float>::infinity())
            : std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                         static_cast<int>(exponent) - 15);
    return (bits & 0x8000u) ? -magnitude : magnitude;
}

void LogFramebufferPixels(const char* label, uint32_t index,
                          GLint x0, GLint y0, GLint x1, GLint y1) {
    const GLsizei width = std::max(x1 - x0, 0);
    const GLsizei height = std::max(y1 - y0, 0);
    GLint read_format = GL_RGBA;
    GLint read_type = GL_UNSIGNED_BYTE;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &read_format);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &read_type);
    size_t components = 4;
    if (read_format == GL_RED || read_format == GL_RED_INTEGER) components = 1;
    else if (read_format == GL_RG || read_format == GL_RG_INTEGER) components = 2;
    else if (read_format == GL_RGB || read_format == GL_RGB_INTEGER) components = 3;
    size_t component_bytes = 1;
    if (read_type == GL_HALF_FLOAT || read_type == GL_HALF_FLOAT_OES ||
        read_type == GL_UNSIGNED_SHORT || read_type == GL_SHORT) {
        component_bytes = 2;
    } else if (read_type == GL_FLOAT || read_type == GL_UNSIGNED_INT ||
               read_type == GL_INT) {
        component_bytes = 4;
    }
    const uint64_t byte_count = static_cast<uint64_t>(width) *
                                static_cast<uint64_t>(height) * components *
                                component_bytes;
    if (width == 0 || height == 0 || byte_count > 16 * 1024 * 1024) {
        LOGE("Present audit #%u %s rejected readback %dx%d", index, label,
             width, height);
        return;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(byte_count));
    GLint pack_buffer = 0;
    GLint pack_alignment = 4;
    GLint pack_row_length = 0;
    GLint pack_skip_rows = 0;
    GLint pack_skip_pixels = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
    if (pack_buffer != 0) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(x0, y0, width, height, static_cast<GLenum>(read_format),
                 static_cast<GLenum>(read_type), pixels.data());
    const GLenum error = glGetError();
    PreserveSceneAuditGlError(error);
    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
    if (pack_buffer != 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));
    }
    LOGI("Present audit #%u %s readback=%dx%d format=0x%04X type=0x%04X "
         "error=0x%04X pack_buffer=%d",
         index, label, width, height, read_format, read_type, error,
         pack_buffer);
    LogByteStats(label, index, pixels.data(), pixels.size());
    if (read_format == GL_RGBA &&
        (read_type == GL_HALF_FLOAT || read_type == GL_HALF_FLOAT_OES) &&
        width >= 800 && height >= 400 && index <= 40 && error == GL_NO_ERROR) {
        double sums[3] = {};
        float maximums[3] = {};
        uint64_t bright = 0;
        uint64_t finite = 0;
        for (size_t p = 0; p + 7 < pixels.size(); p += 8) {
            float channels[3] = {};
            bool valid = true;
            for (size_t c = 0; c < 3; ++c) {
                uint16_t bits = 0;
                std::memcpy(&bits, pixels.data() + p + 2 * c, sizeof(bits));
                channels[c] = AuditHalfToFloat(bits);
                valid = valid && std::isfinite(channels[c]);
            }
            if (!valid) continue;
            ++finite;
            for (size_t c = 0; c < 3; ++c) {
                sums[c] += channels[c];
                maximums[c] = std::max(maximums[c], channels[c]);
            }
            bright += std::max({channels[0], channels[1], channels[2]}) > 0.1f;
        }
        LOGI("Present audit #%u %s half_rgb finite=%llu/%zu mean={%.6g,%.6g,%.6g} "
             "max={%.6g,%.6g,%.6g} bright_gt_0.1=%llu",
             index, label, static_cast<unsigned long long>(finite),
             pixels.size() / 8,
             finite ? sums[0] / finite : 0.0,
             finite ? sums[1] / finite : 0.0,
             finite ? sums[2] / finite : 0.0,
             maximums[0], maximums[1], maximums[2],
             static_cast<unsigned long long>(bright));
    }
    if (read_format == GL_RGBA && read_type == GL_UNSIGNED_BYTE) {
        size_t colored = 0;
        uint8_t rgb_max = 0;
        for (size_t p = 0; p + 3 < pixels.size(); p += 4) {
            const uint8_t maximum = std::max({pixels[p], pixels[p + 1], pixels[p + 2]});
            colored += maximum != 0;
            rgb_max = std::max(rgb_max, maximum);
        }
        LOGI("Present audit #%u %s colored_rgb=%zu/%zu rgb_max=%u",
             index, label, colored, pixels.size() / 4, rgb_max);
    }
}

void LogDrawArraysInstancedState(uint32_t index, GLint first, GLsizei count) {
    GLint program = 0;
    GLint vao = 0;
    GLint array_buffer = 0;
    GLint array_buffer_size = 0;
    GLint framebuffer = 0;
    GLint viewport[4] = {};
    GLint scissor[4] = {};
    GLboolean color_mask[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    if (array_buffer != 0) {
        glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE,
                               &array_buffer_size);
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    const GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    LOGI("Draw audit #%u program=%d vao=%d array_buffer=%d size=%d framebuffer=%d "
         "status=0x%04X viewport=%d,%d %dx%d blend=%d scissor=%d "
         "box=%d,%d %dx%d cull=%d rasterizer_discard=%d depth=%d "
         "color_mask=%d%d%d%d",
         index, program, vao, array_buffer, array_buffer_size, framebuffer,
         framebuffer_status,
         viewport[0], viewport[1], viewport[2], viewport[3],
         glIsEnabled(GL_BLEND), glIsEnabled(GL_SCISSOR_TEST),
         scissor[0], scissor[1], scissor[2], scissor[3],
         glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_RASTERIZER_DISCARD),
         glIsEnabled(GL_DEPTH_TEST), color_mask[0], color_mask[1],
         color_mask[2], color_mask[3]);

    for (GLuint attribute : {0u, 8u}) {
        GLint enabled = 0;
        GLint size = 0;
        GLint stride = 0;
        GLint type = 0;
        GLint normalized = 0;
        GLint buffer = 0;
        GLint divisor = 0;
        void* pointer = nullptr;
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                            &normalized);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                            &buffer);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
        glGetVertexAttribPointerv(attribute, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                  &pointer);
        LOGI("Draw audit #%u attr=%u enabled=%d size=%d type=0x%04X "
             "normalized=%d stride=%d buffer=%d divisor=%d pointer=%p",
             index, attribute, enabled, size, type, normalized, stride, buffer,
             divisor, pointer);
    }

    if (array_buffer > 0 && first >= 0 && count > 0) {
        constexpr size_t kVertexStride = 6 * sizeof(float);
        const size_t byte_offset = static_cast<size_t>(first) * kVertexStride;
        const size_t vertex_count = std::min<size_t>(static_cast<size_t>(count), 4);
        std::array<float, 4 * 6> vertices{};
        const size_t byte_count = vertex_count * kVertexStride;
        if (CopyGlBufferShadow(static_cast<GLuint>(array_buffer), byte_offset,
                               byte_count, vertices.data())) {
            for (size_t vertex = 0; vertex < vertex_count; ++vertex) {
                const float* values = vertices.data() + vertex * 6;
                LOGI("Draw audit #%u vertex=%zu position={%.7g,%.7g,%.7g,%.7g} "
                     "texcoord={%.7g,%.7g}",
                     index, vertex, values[0], values[1], values[2], values[3],
                     values[4], values[5]);
            }
        } else {
            LOGE("Draw audit #%u missing VBO shadow buffer=%d offset=%zu "
                 "bytes=%zu", index, array_buffer, byte_offset, byte_count);
        }
    }

    GLint previous_active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
    for (GLuint unit = 0; unit < 3; ++unit) {
        GLint texture = 0;
        GLint sampler = 0;
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
        GLint compare_mode = 0;
        GLint min_filter = 0;
        GLint mag_filter = 0;
        if (sampler != 0) {
            glGetSamplerParameteriv(static_cast<GLuint>(sampler),
                                    GL_TEXTURE_COMPARE_MODE, &compare_mode);
            glGetSamplerParameteriv(static_cast<GLuint>(sampler),
                                    GL_TEXTURE_MIN_FILTER, &min_filter);
            glGetSamplerParameteriv(static_cast<GLuint>(sampler),
                                    GL_TEXTURE_MAG_FILTER, &mag_filter);
        }
        LOGI("Draw audit #%u unit=%u texture2D=%d sampler=%d "
             "compare=0x%04X min=0x%04X mag=0x%04X",
             index, unit, texture, sampler, compare_mode, min_filter,
             mag_filter);
    }
    glActiveTexture(static_cast<GLenum>(previous_active));
}

bool CheckedArrayBytes(size_t count, size_t components, size_t element_size,
                       size_t* bytes) {
    const uint64_t total = static_cast<uint64_t>(count) *
                           static_cast<uint64_t>(components) *
                           static_cast<uint64_t>(element_size);
    if (total > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    *bytes = static_cast<size_t>(total);
    return true;
}

template <typename T>
const T* GuestInputArray(DynarmicHost& host, uint32_t address, size_t count,
                         size_t components, const char* function_name) {
    size_t bytes = 0;
    if (!CheckedArrayBytes(count, components, sizeof(T), &bytes) ||
        bytes == 0 ||
        !GuestRangeValid(host.GetGuestMemorySize(), address, bytes)) {
        LOGE("%s rejected guest input: address=0x%08X count=%zu components=%zu",
             function_name, address, count, components);
        return nullptr;
    }
    return reinterpret_cast<const T*>(
        GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, bytes));
}

template <typename T>
T* GuestOutputArray(DynarmicHost& host, uint32_t address, size_t count,
                    size_t components, const char* function_name) {
    size_t bytes = 0;
    if (!CheckedArrayBytes(count, components, sizeof(T), &bytes) ||
        bytes == 0 ||
        !GuestRangeValid(host.GetGuestMemorySize(), address, bytes)) {
        LOGE("%s rejected guest output: address=0x%08X count=%zu components=%zu",
             function_name, address, count, components);
        return nullptr;
    }
    return reinterpret_cast<T*>(
        GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, bytes));
}

template <typename T>
T* OptionalGuestOutput(DynarmicHost& host, uint32_t address, size_t count,
                       const char* function_name, bool* valid) {
    if (address == 0) {
        *valid = true;
        return nullptr;
    }
    T* output = GuestOutputArray<T>(host, address, count, 1, function_name);
    *valid = output != nullptr;
    return output;
}

const void* CountedGuestBytes(DynarmicHost& host, uint32_t address,
                              GLsizei byte_count, const char* function_name,
                              bool* valid) {
    if (byte_count <= 0) {
        *valid = true;
        return nullptr;
    }
    const void* input = GuestInputArray<uint8_t>(
        host, address, static_cast<size_t>(byte_count), 1, function_name);
    *valid = input != nullptr;
    return input;
}

template <typename T>
const T* CountedGuestValues(DynarmicHost& host, uint32_t address,
                            GLsizei count, size_t components,
                            const char* function_name, bool* valid) {
    if (count <= 0) {
        *valid = true;
        return nullptr;
    }
    const T* input = GuestInputArray<T>(
        host, address, static_cast<size_t>(count), components, function_name);
    *valid = input != nullptr;
    return input;
}

size_t GetFloatStateValueCount(GLenum pname) {
    switch (pname) {
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_DEPTH_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
            return 2;
        case GL_BLEND_COLOR:
        case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK:
        case GL_SCISSOR_BOX:
        case GL_VIEWPORT:
            return 4;
        case GL_COMPRESSED_TEXTURE_FORMATS: {
            GLint count = 0;
            glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
            return count > 0 ? static_cast<size_t>(count) : 1;
        }
        case GL_SHADER_BINARY_FORMATS: {
            GLint count = 0;
            glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &count);
            return count > 0 ? static_cast<size_t>(count) : 1;
        }
        default:
            return 1;
    }
}

size_t GetVertexAttribValueCount(GLenum pname) {
    return pname == GL_CURRENT_VERTEX_ATTRIB ? 4 : 1;
}

size_t GetTextureParameterValueCount(GLenum pname) {
    // Core GLES texture parameter names are scalar. The border-color extension
    // reuses glTexParameter*v and is the one vector-valued exception exposed by
    // the Android GLES headers.
    return pname == GL_TEXTURE_BORDER_COLOR_EXT ? 4 : 1;
}

uint32_t HostVertexPointerToGuest(DynarmicHost& host, const void* pointer,
                                  GLint array_buffer_binding) {
    if (!pointer) return 0;

    const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    if (array_buffer_binding != 0) {
        return value <= std::numeric_limits<uint32_t>::max()
            ? static_cast<uint32_t>(value)
            : 0;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(host.GetGuestMemoryBase());
    const uintptr_t end = base + host.GetGuestMemorySize();
    if (value < base || value >= end) return 0;
    const uintptr_t offset = value - base;
    return offset <= std::numeric_limits<uint32_t>::max()
        ? static_cast<uint32_t>(offset)
        : 0;
}

const char* GuestGlName(DynarmicHost& host, uint32_t address) {
    if (!GuestRangeValid(host.GetGuestMemorySize(), address, 1)) return nullptr;
    const auto* text = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + address);
    const size_t limit = host.GetGuestMemorySize() - address;
    return strnlen(text, limit) < limit ? text : nullptr;
}

bool GuestGlNameArray(DynarmicHost& host, uint32_t address, GLsizei count,
                      const char* function_name,
                      std::vector<const GLchar*>* names) {
    names->clear();
    if (count <= 0) return true;
    const auto* addresses = GuestInputArray<uint32_t>(
        host, address, static_cast<size_t>(count), 1, function_name);
    if (!addresses) return false;
    names->reserve(static_cast<size_t>(count));
    for (GLsizei i = 0; i < count; ++i) {
        const char* name = GuestGlName(host, addresses[i]);
        if (!name) {
            LOGE("%s rejected guest string[%d]: address=0x%08X",
                 function_name, static_cast<int>(i), addresses[i]);
            names->clear();
            return false;
        }
        names->push_back(name);
    }
    return true;
}

struct GuestMappedBuffer {
    void* host_pointer = nullptr;
    uint32_t guest_address = 0;
    size_t length = 0;
    GLbitfield access = 0;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint buffer = 0;
    GLenum target = 0;
    GLintptr offset = 0;
    bool synchronized = false;
    bool tracked = false;
};

// OpenGL tracks mappings per buffer object, not per binding target. Unity commonly
// maps one object, rebinds GL_COPY_WRITE_BUFFER to another object, and maps that too.
// Native mapped pointers distinguish equal GLuint names in different contexts
// and retain identity when a context migrates to another host thread.
std::mutex s_mapping_mutex;
std::unordered_map<void*, GuestMappedBuffer> s_guest_mapped_buffers;

GuestMappedDirtyTracker& MappedDirtyTracker() {
    // A process-lifetime instance leaves no dangling signal-handler storage.
    static auto* tracker = new GuestMappedDirtyTracker;
    return *tracker;
}

bool DirtySyncRequested() {
    static const bool requested = [] {
        char value[PROP_VALUE_MAX] = {};
        // The full-range compare reads several GiB per gameplay frame. The
        // write-watch path has survived focused Pixel gameplay with movement
        // and is now the ordinary path; value 2 retains an explicit diagnostic
        // full-scan fallback without changing old 0/1 probe cleanup behavior.
        return !(__system_property_get("debug.horn.dirty_sync", value) > 0 &&
                 value[0] == '2');
    }();
    return requested;
}

constexpr std::optional<uint64_t> PixelUnpackBytes(uint32_t w, uint32_t h, uint32_t d,
                                                  uint32_t bpp, std::array<int, 6> u,
                                                  uint64_t limit) {
    if (!w || !h || !d) return 0;
    if (!bpp || u[0] < 1 || u[0] > 8 || (u[0] & (u[0] - 1))) return {};
    for (int value : u) if (value < 0) return {};
    const uint64_t row = ((uint64_t(u[1] ? u[1] : w) * bpp + u[0] - 1) / u[0]) * u[0];
    if (row > limit) return {};
    const uint64_t image = row * uint64_t(u[2] ? u[2] : h);
    if (image > limit) return {};
    uint64_t bytes = 0;
    for (uint64_t term : {uint64_t(u[5]) * image, uint64_t(u[4]) * row,
                          uint64_t(u[3]) * bpp, uint64_t(d - 1) * image,
                          uint64_t(h - 1) * row, uint64_t(w) * bpp}) {
        if (term > limit - bytes) return {};
        bytes += term;
    }
    return bytes;
}
static_assert(PixelUnpackBytes(1, 1, 1, 4, {4, 0, 0, 0, 0, 0}, UINT32_MAX) == 4);
static_assert(PixelUnpackBytes(3, 2, 2, 1, {4, 0, 0, 0, 0, 0}, UINT32_MAX) == 15);
static_assert(PixelUnpackBytes(2, 2, 2, 4, {8, 4, 3, 1, 1, 1}, 140) == 140);
static_assert(!PixelUnpackBytes(2, 2, 2, 4, {8, 4, 3, 1, 1, 1}, 139));
static_assert(!PixelUnpackBytes(INT32_MAX, INT32_MAX, INT32_MAX, 16, {8, 0, 0, 0, 0, 0}, UINT32_MAX));

struct GlStringCache {
    std::mutex mutex;
    std::unordered_map<std::string, uint32_t> values;
};

constexpr size_t kMaxGuestGlSyncs = 1024;
constexpr uint64_t kFirstGuestGlSyncHandle = 0x32000000ULL;

struct GuestGlSyncSlot {
    uint32_t guest_handle = 0;
    GLsync host_sync = nullptr;
};

std::array<GuestGlSyncSlot, kMaxGuestGlSyncs> g_guest_gl_syncs{};
std::mutex g_guest_gl_sync_mutex;
size_t g_guest_gl_sync_count = 0;
uint64_t g_next_guest_gl_sync_handle = kFirstGuestGlSyncHandle;

// ponytail: fixed 1024-slot linear scan; raise the cap only if measured sync volume needs it.
GuestGlSyncSlot* FindGuestGlSyncLocked(uint32_t handle) {
    if (!handle) return nullptr;
    for (GuestGlSyncSlot& slot : g_guest_gl_syncs) {
        if (slot.guest_handle == handle) return &slot;
    }
    return nullptr;
}

GuestGlSyncSlot* FindEmptyGuestGlSyncSlotLocked() {
    for (GuestGlSyncSlot& slot : g_guest_gl_syncs) {
        if (!slot.guest_handle) return &slot;
    }
    return nullptr;
}

uint32_t AllocateGuestGlSyncHandleLocked() {
    if (g_next_guest_gl_sync_handle > std::numeric_limits<uint32_t>::max()) return 0;
    return static_cast<uint32_t>(g_next_guest_gl_sync_handle++);
}

}  // namespace

void SceneAuditLogFramebufferPixels(const char* label, uint32_t index,
                                    GLint x0, GLint y0, GLint x1, GLint y1) {
    LogFramebufferPixels(label, index, x0, y0, x1, y1);
}

static_assert(sizeof(GLuint64) == 8);
static_assert(sizeof(GLenum) == 4);
static_assert(std::is_pointer_v<GLsync>);
static_assert(sizeof(GLsync) == sizeof(void*));

void SyncGuestMappedBuffersForGpuRead(DynarmicHost& host) {
    constexpr GLbitfield kMapPersistentBit = 0x0040;
    constexpr GLbitfield kMapCoherentBit = 0x0080;
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return;

    GuestMappedDirtyTracker* tracker = nullptr;
    if (DirtySyncRequested() &&
        MappedDirtyTracker().Initialize(host.GetGuestMemoryBase(),
                                        host.GetGuestMemorySize())) {
        tracker = &MappedDirtyTracker();
    }
    static thread_local std::vector<size_t> dirty_pages;
    dirty_pages.clear();
    size_t mapping_count = 0;
    size_t mapped_bytes = 0;
    size_t byte_count = 0;
    size_t copied_bytes = 0;
    std::lock_guard mapping_lock(s_mapping_mutex);
    const uint32_t dirty_epoch = tracker ? tracker->DirtyEpoch() : 0;
    const bool scan_dirty_pages = tracker && tracker->NeedsDirtyScan(dirty_epoch);
    if (scan_dirty_pages) {
        // Snapshot the dirty-page set, then re-protect it before reading any
        // page. A simultaneous writer either finishes before our comparison
        // or faults again and remains dirty for the next draw. Do this for all
        // mappings together: two guest mappings may share one 16 KiB page.
        for (const auto& [native_pointer, mapping] : s_guest_mapped_buffers) {
            (void)native_pointer;
            if (!mapping.tracked || !(mapping.access & GL_MAP_WRITE_BIT) ||
                !(mapping.access & kMapPersistentBit)) continue;
            const size_t first_page = mapping.guest_address / tracker->PageSize();
            const size_t past_last_page =
                (static_cast<uint64_t>(mapping.guest_address) +
                 mapping.length - 1) / tracker->PageSize() + 1;
            for (size_t page = first_page; page < past_last_page; ++page) {
                if (tracker->IsDirty(page)) dirty_pages.push_back(page);
            }
        }
        tracker->Rearm(dirty_pages);
        // Keep the epoch captured before the scan. A fault or failed re-arm
        // during it bumps the counter and forces another scan next draw.
        tracker->MarkDirtyScanComplete(dirty_epoch);
    }
    const size_t dirty_page_count = dirty_pages.size();
    for (auto& [native_pointer, mapping] : s_guest_mapped_buffers) {
        (void)native_pointer;
        // Buffer storage and mappings are shared between Horn's upload and
        // render EGL contexts. The guest pointer was created on the upload
        // context, but writes through it must be visible to draws issued by a
        // sibling context in the same share group.
        if (!(mapping.access & GL_MAP_WRITE_BIT) ||
            !(mapping.access & kMapPersistentBit) ||
            !GuestRangeValid(host.GetGuestMemorySize(), mapping.guest_address,
                             mapping.length)) {
            continue;
        }

        const void* guest_pointer =
            host.GetGuestMemoryBase() + mapping.guest_address;
        // The CPU shadow also backs client-array diagnostics. Compare it with
        // guest memory, not with the potentially uncached native mapping.
        // Force the first copy: storage allocated with NULL is undefined even
        // if both CPU shadows happened to start as zero-filled memory.
        // Arm before the initial copy. A write between that copy and Track()
        // would otherwise change guest bytes without marking the page dirty.
        const bool was_tracked = mapping.tracked;
        if (tracker && !mapping.tracked) {
            mapping.tracked = tracker->Track(mapping.guest_address, mapping.length);
        }
        // The dirty snapshot was taken before this loop. A newly successful
        // Track must scan its full range, including a retry after Track failed.
        if (tracker && was_tracked && mapping.tracked && mapping.synchronized) {
            const size_t page_size = tracker->PageSize();
            const uint64_t guest_begin = mapping.guest_address;
            const uint64_t guest_end = guest_begin + mapping.length;
            const size_t first_page = guest_begin / page_size;
            const size_t past_last_page = (guest_end - 1) / page_size + 1;
            auto page_it = std::lower_bound(dirty_pages.begin(),
                                            dirty_pages.end(), first_page);
            for (; page_it != dirty_pages.end() &&
                   *page_it < past_last_page; ++page_it) {
                const size_t page = *page_it;
                const uint64_t segment_begin =
                    std::max<uint64_t>(guest_begin, page * page_size);
                const uint64_t segment_end =
                    std::min<uint64_t>(guest_end, (page + 1) * page_size);
                const size_t relative = static_cast<size_t>(segment_begin - guest_begin);
                const size_t segment_size =
                    static_cast<size_t>(segment_end - segment_begin);
                copied_bytes += SyncChangedGlBufferMapping(
                    mapping.buffer, static_cast<size_t>(mapping.offset) + relative,
                    segment_size,
                    static_cast<uint8_t*>(mapping.host_pointer) + relative,
                    static_cast<const uint8_t*>(guest_pointer) + relative, false);
                byte_count += segment_size;
            }
        } else {
            copied_bytes += SyncChangedGlBufferMapping(
                mapping.buffer, static_cast<size_t>(mapping.offset),
                mapping.length, mapping.host_pointer, guest_pointer,
                !mapping.synchronized);
            byte_count += mapping.length;
            mapping.synchronized = true;
        }

        // Coherent native mappings become visible without an explicit flush.
        // For an explicit-flush mapping, flush here only when its original
        // binding is still current; the guest's own flush thunk handles all
        // other binding arrangements.
        if ((mapping.access & GL_MAP_FLUSH_EXPLICIT_BIT) &&
            !(mapping.access & kMapCoherentBit) &&
            GetBoundGlBuffer(mapping.target) == mapping.buffer) {
            glFlushMappedBufferRange(mapping.target, mapping.offset,
                                     static_cast<GLsizeiptr>(mapping.length));
        }
        ++mapping_count;
        mapped_bytes += mapping.length;
    }
    if (mapping_count != 0 && HornPerfEnabled()) {
        static thread_local uint64_t calls = 0;
        static thread_local uint64_t mapped_total = 0;
        static thread_local uint64_t scanned_total = 0;
        static thread_local uint64_t copied_total = 0;
        static thread_local uint64_t dirty_total = 0;
        static thread_local uint64_t skipped_total = 0;
        mapped_total += mapped_bytes;
        scanned_total += byte_count;
        copied_total += copied_bytes;
        dirty_total += dirty_page_count;
        skipped_total += tracker && !scan_dirty_pages;
        if (++calls == 1024) {
            LOGI("Persistent-map profile draws=1024 mapped=%.1fMiB scanned=%.1fMiB "
                 "copied=%.1fMiB dirty_pages=%llu watch=%d scan_skips=%llu",
                 mapped_total / 1048576.0, scanned_total / 1048576.0,
                 copied_total / 1048576.0,
                 static_cast<unsigned long long>(dirty_total),
                 tracker != nullptr,
                 static_cast<unsigned long long>(skipped_total));
            calls = mapped_total = scanned_total = copied_total = dirty_total =
                skipped_total = 0;
        }
    }

    if (mapping_count != 0) {
        static std::atomic<uint32_t> logs{0};
        const uint32_t index = logs.fetch_add(1, std::memory_order_relaxed);
        if (index < 16) {
            LOGI("Persistent-map staging sync #%u mappings=%zu scanned=%zu copied=%zu dirty_watch=%d",
                 index + 1, mapping_count, byte_count, copied_bytes,
                 tracker != nullptr);
            if (index == 0) {
                for (const auto& [native_pointer, mapping] : s_guest_mapped_buffers) {
                    (void)native_pointer;
                    if (!(mapping.access & GL_MAP_WRITE_BIT) ||
                        !(mapping.access & kMapPersistentBit)) continue;
                    LOGI("Persistent-map source buffer=%u target=0x%04X length=%zu "
                         "access=0x%04X guest=0x%08X",
                         mapping.buffer, mapping.target, mapping.length,
                         mapping.access, mapping.guest_address);
                }
            }
        }
    }
}

void ResetGuestMappedDirtyTracker() {
    std::lock_guard mapping_lock(s_mapping_mutex);
    MappedDirtyTracker().Shutdown();
    for (auto& [native_pointer, mapping] : s_guest_mapped_buffers) {
        (void)native_pointer;
        mapping.tracked = false;
    }
}

static int SceneAuditMode() {
    // Permit a bounded audit to be armed after the intro: the first 48 samples
    // otherwise belong to the menumap and never describe class/gameplay frames.
    static thread_local int mode = 0;
    static thread_local uint32_t calls = 0;
    if ((++calls & 127u) == 1u) {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.horn.scene_audit", value);
        mode = SceneAuditModeFromValue(value);
    }
    return mode;
}

bool SceneAuditEnabled() { return SceneAuditMode() != 0; }

struct SceneAuditPass {
    uint32_t draws = 0;
    uint32_t upper_dump_draw = 0;
    uint32_t samples = 0;
    bool color_writes = false;
    GLuint last_program = 0;
};

static thread_local std::unordered_map<GLuint, SceneAuditPass> s_scene_audit_passes;
uint32_t SceneAuditCurrentDrawCount(GLuint framebuffer) {
    const auto found = s_scene_audit_passes.find(framebuffer);
    return found == s_scene_audit_passes.end() ? 0 : found->second.draws;
}
static thread_local uint32_t s_scene_audit_transitions = 0;
static thread_local uint32_t s_scene_audit_samples = 0;
static thread_local uint32_t s_scene_audit_post_draw_samples = 0;
static thread_local GLuint s_scene_audit_post_draw_program = 0;
static std::atomic<uint32_t> s_scene_audit_error_logs{0};
static std::atomic<uint32_t> s_scene_audit_late_error_logs{0};

void SceneAuditLogError(const char* phase) {
    if (!SceneAuditEnabled()) return;
    GLint framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer <= 2) return;
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) return;
    PreserveSceneAuditGlError(error);
    const uint32_t index = s_scene_audit_error_logs.fetch_add(
        1, std::memory_order_relaxed);
    const bool late = framebuffer == 6 || framebuffer == 23 || framebuffer == 24;
    const uint32_t late_index = late ? s_scene_audit_late_error_logs.fetch_add(
        1, std::memory_order_relaxed) : 0;
    if (index >= 96 && (!late || late_index >= 24)) return;
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    LOGE("Scene GL error #%u phase=%s framebuffer=%d program=%d error=0x%04X tid=%d",
         index + 1, phase, framebuffer, program, error, gettid());
}

void SceneAuditNoteDraw(GLuint program) {
    if (!SceneAuditEnabled()) return;
    GLint framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer <= 2) return;
    SceneAuditPass& pass = s_scene_audit_passes[static_cast<GLuint>(framebuffer)];
    ++pass.draws;
    pass.last_program = program;
    GLboolean mask[4] = {};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    pass.color_writes = pass.color_writes || mask[0] || mask[1] || mask[2] || mask[3];
}

static void SceneAuditDumpUpperFramebuffer(GLuint framebuffer, uint32_t draw,
                                          const char* operation) {
    SceneUpperReadback capture;
    if (!CaptureUpperSceneFramebuffer(framebuffer, capture)) {
        LOGI("Scene upper skipped framebuffer=%u draw=%u operation=%s "
             "attachment=0x%04X object=%d level=%d target=%dx%d unit=%d "
             "scanned=%d/%d error=0x%04X restore=0x%04X",
             framebuffer, draw, operation, capture.object_type, capture.object,
             capture.level, capture.target_width, capture.target_height,
             capture.texture_unit, capture.scanned_units, capture.available_units,
             capture.error, capture.restore_error);
        return;
    }
    const std::string path =
        "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/scene-upper-pid" +
        std::to_string(getpid()) + "-tid" + std::to_string(gettid()) + "-fbo" +
        std::to_string(framebuffer) + "-ctx" +
        std::to_string(reinterpret_cast<uintptr_t>(capture.context)) +
        "-draw" + std::to_string(draw) + "-x" +
        std::to_string(capture.x) + "-y" + std::to_string(capture.y) +
        (capture.type == GL_FLOAT ? ".rgba32f" : ".rgba8");
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) { LOGE("Scene upper dump open failed path=%s", path.c_str()); return; }
    const bool complete = std::fwrite(capture.pixels.data(), 1, capture.pixels.size(), file) ==
        capture.pixels.size();
    const int close = std::fclose(file);
    LOGI("Scene upper dump framebuffer=%u draw=%u bytes=%zu complete=%d close=%d path=%s",
         framebuffer, draw, capture.pixels.size(), complete, close, path.c_str());
    if (!complete || close != 0) return;
    char metadata[3072];
    const int bytes = std::snprintf(metadata, sizeof(metadata),
        "{\"schema\":\"scene-upper-v1\",\"mode\":2,\"pid\":%d,\"tid\":%d,"
        "\"context\":%llu,\"read_surface\":%llu,\"read_framebuffer\":%u,"
        "\"draw_framebuffer\":%d,\"draw\":%u,\"operation\":\"%s\",\"program\":%d,"
        "\"attachment_type\":%d,\"attachment_object\":%d,\"level\":%d,\"samples\":%d,"
        "\"texture_unit\":%d,\"scanned_units\":%d,\"available_units\":%d,"
        "\"viewport\":[%d,%d,%d,%d],\"target_extent\":[%d,%d],\"roi\":[%d,%d,896,512],"
        "\"component_type\":%d,\"implementation_format\":%d,\"implementation_type\":%d,"
        "\"format\":%d,\"type\":%u,\"bytes\":%zu,\"read_error\":%u,\"restore_error\":%u,"
        "\"raw_complete\":true,\"raw_close\":0,\"raw_file\":\"%s\"}\n",
        getpid(), gettid(), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(capture.context)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(capture.read_surface)),
        framebuffer, capture.draw_framebuffer, draw, operation, capture.program,
        capture.object_type, capture.object, capture.level, capture.samples,
        capture.texture_unit, capture.scanned_units, capture.available_units,
        capture.viewport[0], capture.viewport[1], capture.viewport[2], capture.viewport[3],
        capture.target_width, capture.target_height, capture.x, capture.y,
        capture.component_type, capture.implementation_format, capture.implementation_type,
        GL_RGBA, capture.type, capture.pixels.size(), capture.error, capture.restore_error,
        path.substr(path.find_last_of('/') + 1).c_str());
    if (bytes <= 0 || static_cast<size_t>(bytes) >= sizeof(metadata)) return;
    const std::string receipt = path + ".json";
    file = std::fopen(receipt.c_str(), "wb");
    if (!file) { LOGE("Scene upper receipt open failed path=%s", receipt.c_str()); return; }
    const bool receipt_complete = std::fwrite(metadata, 1, bytes, file) == static_cast<size_t>(bytes);
    const int receipt_close = std::fclose(file);
    LOGI("Scene upper receipt bytes=%d complete=%d close=%d path=%s",
         bytes, receipt_complete, receipt_close, receipt.c_str());
}

static void SceneAuditDumpGfxFramebuffer(GLuint framebuffer, uint32_t draw,
                                        const char* operation = "transition") {
    const int mode = SceneAuditMode();
    if (!mode) return;
    if (mode == 2) {
        const auto pass = s_scene_audit_passes.find(framebuffer);
        if (pass == s_scene_audit_passes.end() ||
            !SceneUpperTakeDraw(draw, pass->second.upper_dump_draw)) return;
        SceneAuditDumpUpperFramebuffer(framebuffer, draw, operation);
        return;
    }
    constexpr GLsizei width = 896, height = 512;
    GLint old_read_framebuffer = 0, old_read_buffer = 0;
    GLint old_pack_buffer = 0, old_pack_alignment = 0;
    GLint old_pack_row = 0, old_pack_skip_pixels = 0, old_pack_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_pack_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pack_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_pack_skip_rows);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 8);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_HALF_FLOAT, pixels.data());
    const GLenum error = glGetError();
    PreserveSceneAuditGlError(error);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_pack_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pack_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_pack_skip_rows);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(old_pack_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_framebuffer));
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
    if (error != GL_NO_ERROR) {
        LOGE("Scene FBO dump framebuffer=%u draw=%u read error=0x%04X",
             framebuffer, draw, error);
        return;
    }
    const std::string path =
        "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/scene-fbo" +
        std::to_string(framebuffer) + "-draw" + std::to_string(draw) +
        ".rgba16f";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        LOGE("Scene FBO dump framebuffer=%u draw=%u open failed",
             framebuffer, draw);
        return;
    }
    const bool complete = std::fwrite(pixels.data(), 1, pixels.size(), file) ==
        pixels.size();
    const int close_result = std::fclose(file);
    LOGI("Scene FBO dump framebuffer=%u draw=%u bytes=%zu complete=%d "
         "close=%d path=%s", framebuffer, draw, pixels.size(), complete,
         close_result, path.c_str());
}

static void SceneAuditDumpDefaultFramebuffer(uint32_t index,
                                             const char* operation = "blit") {
    const int mode = SceneAuditMode();
    if (!mode) return;
    if (mode == 2) {
        SceneAuditDumpUpperFramebuffer(0, index, operation);
        return;
    }
    constexpr GLsizei width = 896, height = 512;
    GLint old_read_framebuffer = 0, old_read_buffer = 0;
    GLint old_pack_buffer = 0, old_pack_alignment = 0;
    GLint old_pack_row = 0, old_pack_skip_pixels = 0, old_pack_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_pack_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pack_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_pack_skip_rows);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const GLenum error = glGetError();
    PreserveSceneAuditGlError(error);
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_pack_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pack_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_pack_skip_rows);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(old_pack_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_framebuffer));
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
    if (error != GL_NO_ERROR) {
        LOGE("Scene default dump index=%u read error=0x%04X", index, error);
        return;
    }
    const std::string path =
        "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/scene-default-" +
        std::to_string(index) + ".rgba8";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    const bool complete = std::fwrite(pixels.data(), 1, pixels.size(), file) ==
        pixels.size();
    const int close_result = std::fclose(file);
    LOGI("Scene default dump index=%u bytes=%zu complete=%d close=%d path=%s",
         index, pixels.size(), complete, close_result, path.c_str());
}

static void SceneAuditTrackGroundPixels(uint32_t draw, GLuint program,
                                       const char* operation) {
    if (SceneAuditMode() != 1) return;
    if (draw > 1300) return;
    constexpr std::array<std::array<GLint, 2>, 12> points{{
        {{280, 172}}, {{300, 212}}, {{340, 152}},
        // Measured magenta locations in the native-size Pixel plank crop.
        {{247, 241}}, {{359, 225}}, {{463, 221}},
        {{544, 249}}, {{738, 246}}, {{786, 249}},
        // Native Pixel center-rock artifact, outside the old 896-wide crop.
        {{1105, 429}}, {{1136, 421}}, {{1171, 427}}
    }};
    static thread_local std::array<std::array<uint16_t, 4>, points.size()> last{};
    static thread_local std::array<bool, points.size()> seen{};
    GLint old_read_framebuffer = 0, old_read_buffer = 0;
    GLint old_pack_buffer = 0, old_pack_alignment = 0;
    GLint old_pack_row = 0, old_pack_skip_pixels = 0, old_pack_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_pack_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pack_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_pack_skip_rows);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 15);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    bool rock_changed = false;
    // Capture the rock first: per-draw deduplication must not let a changed
    // plank point consume the draw before its lighting-input readback.
    for (size_t order = 0; order < points.size(); ++order) {
        const size_t i = (order + 9) % points.size();
        if (points[i][0] >= viewport[2] || points[i][1] >= viewport[3]) continue;
        std::array<uint16_t, 4> pixel{};
        glReadPixels(points[i][0], points[i][1], 1, 1,
                     GL_RGBA, GL_HALF_FLOAT, pixel.data());
        const GLenum error = glGetError();
        PreserveSceneAuditGlError(error);
        if (error != GL_NO_ERROR) {
            LOGE("Scene ground history draw=%u point=%zu read_error=0x%04X",
                 draw, i, error);
            continue;
        }
        if (!seen[i] || pixel != last[i]) {
            LOGI("Scene ground history draw=%u point=%zu xy=%d,%d program=%u "
                 "operation=%s rgba16={%04X,%04X,%04X,%04X}",
                 draw, i, points[i][0], points[i][1], program, operation,
                 pixel[0], pixel[1], pixel[2], pixel[3]);
            last[i] = pixel;
            seen[i] = true;
            rock_changed = rock_changed || i >= 9;
            // Keep the ordinary lighting input too: the defect is intermittent,
            // and a threshold-only capture cannot provide a matched baseline.
            if (i >= 3) {
                SceneAuditCaptureLightingState(draw, i);
            }
        }
    }
    static thread_local uint32_t rock_captures = 0;
    if (rock_changed && rock_captures < 12 && viewport[2] >= 1256 && viewport[3] >= 480) {
        ++rock_captures;
        constexpr GLint x = 1064, y = 368;
        constexpr GLsizei width = 192, height = 112;
        std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * 4);
        const size_t byte_count = pixels.size() * sizeof(uint16_t);
        glReadPixels(x, y, width, height, GL_RGBA, GL_HALF_FLOAT, pixels.data());
        const GLenum error = glGetError();
        PreserveSceneAuditGlError(error);
        if (error == GL_NO_ERROR) {
            const std::string path =
                "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/scene-region-rock-draw" +
                std::to_string(draw) + "-192x112.rgba16f";
            if (FILE* file = std::fopen(path.c_str(), "wb")) {
                const bool complete = std::fwrite(pixels.data(), 1, byte_count, file) == byte_count;
                const int closed = std::fclose(file);
                LOGI("Scene region dump framebuffer=15 draw=%u xy=%d,%d size=%dx%d "
                     "bytes=%zu complete=%d close=%d path=%s",
                     draw, x, y, width, height, byte_count, complete, closed, path.c_str());
            }
        } else {
            LOGE("Scene region dump framebuffer=15 draw=%u read_error=0x%04X", draw, error);
        }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_pack_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pack_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_pack_skip_rows);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(old_pack_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_framebuffer));
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
}

static void SceneAuditTrackUiLine(uint32_t draw, GLuint program,
                                const char* operation) {
    if (SceneAuditMode() != 1) return;
    if (draw > 64) return;
    constexpr std::array<GLint, 3> rows{48, 58, 62};
    static thread_local std::array<std::array<uint16_t, 4>, rows.size()> last{};
    static thread_local std::array<bool, rows.size()> seen{};
    GLint old_read_framebuffer = 0, old_read_buffer = 0;
    GLint old_pack_buffer = 0, old_pack_alignment = 0;
    GLint old_pack_row = 0, old_pack_skip_pixels = 0, old_pack_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_pack_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pack_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_pack_skip_rows);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 28);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    for (size_t index = 0; index < rows.size(); ++index) {
        std::array<uint16_t, 4> pixel{};
        glReadPixels(500, rows[index], 1, 1,
                     GL_RGBA, GL_HALF_FLOAT, pixel.data());
        const GLenum error = glGetError();
        PreserveSceneAuditGlError(error);
        if (error == GL_NO_ERROR && (!seen[index] || pixel != last[index])) {
            LOGI("Scene UI line history draw=%u program=%u operation=%s "
                 "y=%d rgba16={%04X,%04X,%04X,%04X}",
                 draw, program, operation, rows[index],
                 pixel[0], pixel[1], pixel[2], pixel[3]);
            last[index] = pixel;
            seen[index] = true;
        } else if (error != GL_NO_ERROR) {
            LOGE("Scene UI line history draw=%u y=%d read_error=0x%04X",
                 draw, rows[index], error);
        }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_pack_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pack_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_pack_skip_rows);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(old_pack_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_framebuffer));
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
}

void SceneAuditDrawCompleted(const char* operation) {
    SceneAuditLogError(operation);
    if (SceneAuditEnabled()) {
        GLint framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        if (framebuffer > 2) {
            const auto pass = s_scene_audit_passes.find(static_cast<GLuint>(framebuffer));
            if (pass != s_scene_audit_passes.end() && pass->second.draws && pass->second.last_program) {
                SceneAuditCaptureTargetLightingState(static_cast<GLuint>(framebuffer),
                    pass->second.draws, pass->second.last_program, operation);
            }
        }
        if (framebuffer == 0) {
            static thread_local uint32_t default_draws = 0;
            if (default_draws < 16) {
                GLint program = 0, viewport[4] = {};
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                glGetIntegerv(GL_VIEWPORT, viewport);
                LOGI("Scene default draw #%u operation=%s program=%d "
                     "viewport=%d,%d %dx%d", ++default_draws, operation,
                     program, viewport[0], viewport[1], viewport[2], viewport[3]);
                if (default_draws <= 4) {
                    SceneAuditDumpDefaultFramebuffer(100 + default_draws, operation);
                }
            }
        } else if (framebuffer == 28) {
            const auto found = s_scene_audit_passes.find(28);
            if (found != s_scene_audit_passes.end()) {
                const uint32_t draws = found->second.draws;
                SceneAuditTrackUiLine(draws, found->second.last_program,
                                      operation);
                if (draws == 2 || draws == 30 || draws == 31 ||
                    draws == 51 || draws == 52) {
                    SceneAuditDumpGfxFramebuffer(28, draws, operation);
                }
            }
        } else if (framebuffer == 15) {
            const auto found = s_scene_audit_passes.find(15);
            if (found != s_scene_audit_passes.end()) {
                const uint32_t draws = found->second.draws;
                SceneAuditTrackGroundPixels(draws, found->second.last_program,
                                            operation);
                if (draws == 1 || draws == 10 || draws == 30 ||
                    draws == 63 || draws == 120 || draws == 200 ||
                    draws == 255 || draws == 294 || draws == 350 ||
                    draws == 529 || draws == 590 || draws == 950 ||
                    draws == 1200) {
                    SceneAuditDumpGfxFramebuffer(15, draws, operation);
                }
            }
        }
    }
    if (SceneAuditMode() != 1 || s_scene_audit_post_draw_samples >= 8) return;
    GLint framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 8) return;
    const auto found = s_scene_audit_passes.find(8);
    if (found == s_scene_audit_passes.end() || found->second.draws < 531 ||
        found->second.draws > 556 ||
        found->second.last_program == s_scene_audit_post_draw_program) return;
    s_scene_audit_post_draw_program = found->second.last_program;
    GLint old_read_framebuffer = 0;
    GLint old_read_buffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 8);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const uint32_t sample = ++s_scene_audit_post_draw_samples;
    LOGI("Scene after-draw #%u framebuffer=8 draws=%u program=%u operation=%s",
         sample, found->second.draws, found->second.last_program, operation);
    LogFramebufferPixels("after-draw", 40, 0, 0, 896, 512);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(old_read_framebuffer));
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
    SceneAuditLogError("scene after-draw readback");
}

void SceneAuditFramebufferTransition(GLenum target, GLuint next_framebuffer) {
    if (!SceneAuditEnabled() || target == GL_READ_FRAMEBUFFER) return;
    GLint previous = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
    if (previous <= 2 || static_cast<GLuint>(previous) == next_framebuffer) return;
    const auto found = s_scene_audit_passes.find(static_cast<GLuint>(previous));
    if (found == s_scene_audit_passes.end() || found->second.draws == 0) return;
    SceneAuditPass& pass = found->second;
    const uint32_t transition = ++s_scene_audit_transitions;
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (transition <= 64) {
        LOGI("Scene FBO transition #%u framebuffer=%d next=%u draws=%u "
             "color_writes=%d last_program=%u status=0x%04X",
             transition, previous, next_framebuffer, pass.draws,
             pass.color_writes, pass.last_program, status);
    }
    if (previous == 6 && pass.draws >= 300 && pass.draws <= 600 &&
        status == GL_FRAMEBUFFER_COMPLETE) {
        static thread_local unsigned captured = 0;
        if (captured < 2) {
            ++captured;
            SceneAuditDumpGfxFramebuffer(6, pass.draws);
        }
    }
    if (!pass.color_writes || pass.samples >= 4 ||
        s_scene_audit_samples >= 48 || status != GL_FRAMEBUFFER_COMPLETE) return;

    GLenum attachment = GL_NONE;
    for (GLenum candidate = GL_COLOR_ATTACHMENT0;
         candidate < GL_COLOR_ATTACHMENT0 + 4; ++candidate) {
        GLint object_type = GL_NONE;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, candidate,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                              &object_type);
        if (object_type != GL_NONE) {
            attachment = candidate;
            break;
        }
    }
    if (attachment == GL_NONE) return;
    GLint attachment_name = 0;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                          &attachment_name);

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (transition <= 64) {
        GLint object_type = GL_NONE, level = -1;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object_type);
        if (object_type == GL_TEXTURE) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
        }
        // Metadata remains useful above the bounded readback size. Join the
        // actual object to sampler level-size queries; do not assume its
        // binding target or allocate a full native-resolution pixel copy.
        LOGI("Scene FBO metadata #%u framebuffer=%d status=0x%04X "
             "attachment=0x%04X type=0x%04X object=%d level=%d "
             "draws=%u viewport=%d,%d %dx%d",
             transition, previous, status, attachment, object_type,
             attachment_name, level, pass.draws,
             viewport[0], viewport[1], viewport[2], viewport[3]);
    }
    if (SceneAuditMode() != 1 || viewport[0] < 0 || viewport[1] < 0 || viewport[2] <= 0 ||
        viewport[3] <= 0 || viewport[2] > 2048 || viewport[3] > 2048) return;

    GLint old_read_framebuffer = 0;
    GLint old_read_buffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
    glReadBuffer(attachment);
    const uint32_t sample = ++s_scene_audit_samples;
    ++pass.samples;
    LOGI("Scene FBO sample #%u framebuffer=%d attachment=0x%04X object=%d "
         "draws=%u color_writes=%d last_program=%u viewport=%d,%d %dx%d",
         sample, previous, attachment, attachment_name, pass.draws, pass.color_writes,
         pass.last_program, viewport[0], viewport[1], viewport[2], viewport[3]);
    LogFramebufferPixels("scene-source", sample, viewport[0], viewport[1],
                         viewport[0] + viewport[2], viewport[1] + viewport[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(old_read_framebuffer));
    glReadBuffer(static_cast<GLenum>(old_read_buffer));
    SceneAuditLogError("scene FBO readback");
}

bool GlSyncRegistrySelfCheck() {
    std::lock_guard<std::mutex> lock(g_guest_gl_sync_mutex);
    size_t occupied = 0;
    bool unique = true;
    bool slots_valid = true;
    for (size_t i = 0; i < g_guest_gl_syncs.size(); ++i) {
        const GuestGlSyncSlot& slot = g_guest_gl_syncs[i];
        if (slot.guest_handle) {
            ++occupied;
            slots_valid = slots_valid && slot.host_sync != nullptr;
            for (size_t j = i + 1; j < g_guest_gl_syncs.size(); ++j) {
                unique = unique && slot.guest_handle != g_guest_gl_syncs[j].guest_handle;
            }
        } else {
            slots_valid = slots_valid && slot.host_sync == nullptr;
        }
    }
    const bool ok = occupied == g_guest_gl_sync_count && slots_valid && unique &&
                    FindGuestGlSyncLocked(0) == nullptr &&
                    FindGuestGlSyncLocked(static_cast<uint32_t>(kFirstGuestGlSyncHandle - 1)) == nullptr &&
                    g_next_guest_gl_sync_handle != 0 &&
                    g_next_guest_gl_sync_handle <=
                        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    LOGE("GLsync registry self-check %s", ok ? "OK" : "FAILED");
    return ok;
}

void ResetGlSyncRegistry() {
    std::lock_guard<std::mutex> lock(g_guest_gl_sync_mutex);
    const bool can_delete = eglGetCurrentContext() != EGL_NO_CONTEXT;
    size_t deleted = 0;
    size_t dropped = 0;
    for (GuestGlSyncSlot& slot : g_guest_gl_syncs) {
        if (slot.host_sync) {
            if (can_delete) {
                glDeleteSync(slot.host_sync);
                ++deleted;
            } else {
                ++dropped;
            }
        }
        slot = {};
    }
    g_guest_gl_sync_count = 0;
    LOGE("GLsync registry reset deleted=%zu dropped_without_context=%zu", deleted, dropped);
}

// Shared by 2D and 3D uploads. Only a PIXEL_UNPACK_BUFFER binding makes data an offset.
bool ResolveGlPixels(DynarmicHost& host, int width, int height, int depth,
                     uint32_t format, uint32_t type, uint32_t address, const void** pixels, bool volume) {
    GLint pbo = 0;
    if (HasGlES3()) glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (pbo) {
        *pixels = reinterpret_cast<const void*>(static_cast<uintptr_t>(address));
        return true;
    }
    if (width < 0 || height < 0 || depth < 0) return false;
    if (!width || !height || !depth) { *pixels = nullptr; return true; }
    uint32_t bpp = 0, component_bytes = 0;
    switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1: bpp = 2; break;
        case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV:
        case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8: bpp = 4; break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: bpp = 8; break;
        case GL_BYTE: case GL_UNSIGNED_BYTE: component_bytes = 1; break;
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES:
            component_bytes = 2;
            break;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: component_bytes = 4; break;
        default: return false;
    }
    if (!bpp) {
        uint32_t components = 0;
        switch (format) {
            case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE:
            case GL_DEPTH_COMPONENT: components = 1; break;
            case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: components = 2; break;
            case GL_RGB: case GL_RGB_INTEGER: case GL_SRGB_EXT: components = 3; break;
            case GL_RGBA: case GL_RGBA_INTEGER: case GL_BGRA_EXT: case GL_SRGB_ALPHA_EXT:
                components = 4;
                break;
            case GL_DEPTH_STENCIL:
                bpp = 4;
                break;
            default: return false;
        }
        if (!bpp) bpp = components * component_bytes;
    }
    std::array<int, 6> unpack{4, 0, 0, 0, 0, 0};
    constexpr GLenum names[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT,
                               GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_IMAGES};
    const size_t unpack_count = HasGlES3() ? unpack.size() : size_t(1);
    for (size_t i = 0; i < unpack_count; ++i) glGetIntegerv(names[i], &unpack[i]);
    if (!volume) { unpack[2] = 0; unpack[5] = 0; }
    const auto bytes = PixelUnpackBytes(width, height, depth, bpp, unpack, host.GetGuestMemorySize());
    if (!bytes || !GuestRangeValid(host.GetGuestMemorySize(), address, *bytes)) {
        LOGE("Rejected pixel span address=0x%08X dimensions=%dx%dx%d type=0x%X format=0x%X",
             address, width, height, depth, type, format);
        return false;
    }
    *pixels = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(), address, *bytes);
    return true;
}

// Desktop GL permits BGRA bytes in RGBA/sRGB storage. GLES BGRA_EXT only
// permits BGRA internal storage, so convert actual uploads before submission.
bool UploadBgraPixels2D(DynarmicHost& host, GLenum target, GLint level,
                        GLint internalformat, GLsizei width, GLsizei height,
                        GLint border, GLint xoffset, GLint yoffset,
                        uint32_t address, bool subimage) {
    if (width < 0 || height < 0) return false;
    GLint unpack[4] = {};
    constexpr GLenum names[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
                                GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS};
    for (size_t i = 0; i < 4; ++i) glGetIntegerv(names[i], &unpack[i]);
    GLint pbo = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (width == 0 || height == 0) {
        if (!subimage) {
            if (pbo != 0) {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                TrackGlBufferBinding(GL_PIXEL_UNPACK_BUFFER, 0);
            }
            glTexImage2D(target, level, internalformat, width, height, border,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            if (pbo != 0) {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pbo));
                TrackGlBufferBinding(GL_PIXEL_UNPACK_BUFFER,
                                     static_cast<GLuint>(pbo));
            }
        }
        return true;
    }
    const std::array<int, 6> layout{unpack[0], unpack[1], 0,
                                    unpack[2], unpack[3], 0};
    constexpr uint64_t kMaxUploadBytes = 512ull * 1024ull * 1024ull;
    const auto required = PixelUnpackBytes(
        static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1, 4,
        layout, kMaxUploadBytes);
    const uint64_t output_bytes = static_cast<uint64_t>(width) *
                                  static_cast<uint64_t>(height) * 4u;
    if (!required || *required > kMaxUploadBytes ||
        output_bytes > kMaxUploadBytes) return false;

    std::vector<uint8_t> source_copy;
    const uint8_t* source = nullptr;
    if (pbo != 0) {
        try { source_copy.resize(static_cast<size_t>(*required)); }
        catch (...) { return false; }
        if (!CopyGlBufferShadow(static_cast<GLuint>(pbo), address,
                                source_copy.size(), source_copy.data())) {
            LOGE("BGRA upload missing PBO shadow pbo=%d offset=%u bytes=%zu",
                 pbo, address, source_copy.size());
            return false;
        }
        source = source_copy.data();
    } else {
        if (address == 0 || !GuestRangeValid(host.GetGuestMemorySize(),
                                             address, *required)) return false;
        source = static_cast<const uint8_t*>(GuestSpan(
            host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
            address, *required));
        if (!source) return false;
    }
    std::vector<uint8_t> rgba;
    try { rgba.resize(static_cast<size_t>(output_bytes)); }
    catch (...) { return false; }
    const uint64_t row_pixels = unpack[1] > 0
                                    ? static_cast<uint64_t>(unpack[1])
                                    : static_cast<uint64_t>(width);
    const uint64_t row_bytes = row_pixels * 4u;
    const uint64_t row_stride = (row_bytes + unpack[0] - 1u) &
                                ~static_cast<uint64_t>(unpack[0] - 1u);
    const uint64_t start = static_cast<uint64_t>(unpack[3]) * row_stride +
                           static_cast<uint64_t>(unpack[2]) * 4u;
    for (GLsizei y = 0; y < height; ++y) {
        const uint8_t* in = source + start + static_cast<uint64_t>(y) * row_stride;
        uint8_t* out = rgba.data() + static_cast<size_t>(y) *
                                     static_cast<size_t>(width) * 4u;
        for (GLsizei x = 0; x < width; ++x) {
            out[4u * x + 0] = in[4u * x + 2];
            out[4u * x + 1] = in[4u * x + 1];
            out[4u * x + 2] = in[4u * x + 0];
            out[4u * x + 3] = in[4u * x + 3];
        }
    }
    if (pbo != 0) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        TrackGlBufferBinding(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (subimage) {
        glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else {
        glTexImage2D(target, level, internalformat, width, height, border,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    for (size_t i = 0; i < 4; ++i) glPixelStorei(names[i], unpack[i]);
    if (pbo != 0) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(pbo));
        TrackGlBufferBinding(GL_PIXEL_UNPACK_BUFFER,
                             static_cast<GLuint>(pbo));
    }
    static std::atomic<uint32_t> logs{0};
    const uint32_t index = logs.fetch_add(1, std::memory_order_relaxed);
    if (index < 32) {
        LOGI("BGRA->RGBA upload #%u subimage=%d %dx%d pbo=%d "
             "unpack={%d,%d,%d,%d} input=%llu output=%llu",
             index + 1, subimage, width, height, pbo, unpack[0], unpack[1],
             unpack[2], unpack[3],
             static_cast<unsigned long long>(*required),
             static_cast<unsigned long long>(output_bytes));
    }
    if (PresentAuditEnabled()) {
        const GLenum error = glGetError();
        PreserveSceneAuditGlError(error);
        if (index < 32) {
            LOGI("BGRA->RGBA upload #%u call error=0x%04X", index + 1, error);
        }
    }
    return true;
}

void ForgetGuestGlMappings(DynarmicHost& host, GLsizei count, const GLuint* buffers) {
    if(count<=0 || !buffers)return;
    const EGLContext context=eglGetCurrentContext();
    std::lock_guard lock(s_mapping_mutex);
    for(auto it=s_guest_mapped_buffers.begin();it!=s_guest_mapped_buffers.end();) {
        if(it->second.context==context && std::find(buffers,buffers+count,it->second.buffer)!=buffers+count) {
            if (it->second.tracked) {
                MappedDirtyTracker().Untrack(it->second.guest_address,
                                             it->second.length);
            }
            host.GuestFree(it->second.guest_address);it=s_guest_mapped_buffers.erase(it);
        } else ++it;
    }
}

void RefreshGlBindings();
void TrackGlBufferBinding(GLenum target, GLuint buffer);
GLuint GetBoundGlBuffer(GLenum target);

static bool ResolveDrawElementIndices(DynarmicHost& host, GLsizei count,
                                      GLenum type, uint32_t address,
                                      const void** indices,
                                      const char* function_name) {
    if (GetBoundGlBuffer(GL_ELEMENT_ARRAY_BUFFER) != 0) {
        *indices = reinterpret_cast<const void*>(static_cast<uintptr_t>(address));
        return true;
    }
    if (count <= 0) {
        *indices = nullptr;
        return true;
    }

    size_t element_size = 0;
    switch (type) {
        case GL_UNSIGNED_BYTE: element_size = sizeof(GLubyte); break;
        case GL_UNSIGNED_SHORT: element_size = sizeof(GLushort); break;
        case GL_UNSIGNED_INT: element_size = sizeof(GLuint); break;
        default:
            // Let the driver report GL_INVALID_ENUM, but never expose an unchecked
            // guest address to the host for an element type it cannot consume.
            *indices = nullptr;
            return true;
    }

    size_t bytes = 0;
    if (!CheckedArrayBytes(static_cast<size_t>(count), 1, element_size, &bytes) ||
        !GuestRangeValid(host.GetGuestMemorySize(), address, bytes)) {
        LOGE("%s rejected guest indices: address=0x%08X count=%d type=0x%X",
             function_name, address, static_cast<int>(count), type);
        return false;
    }
    *indices = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                         address, bytes);
    return true;
}

template <typename Proc>
static Proc ResolveBaseVertexProc(std::atomic<Proc>& cached,
                                  const char* first, const char* second,
                                  const char* core) {
    Proc function = cached.load(std::memory_order_acquire);
    if (function) return function;

    function = reinterpret_cast<Proc>(eglGetProcAddress(first));
    if (!function) function = reinterpret_cast<Proc>(eglGetProcAddress(second));
    if (!function) function = reinterpret_cast<Proc>(eglGetProcAddress(core));
    // A lookup before an EGL context exists may legitimately fail. Cache only a
    // real entry point so that a later draw can retry after eglMakeCurrent.
    if (function) cached.store(function, std::memory_order_release);
    return function;
}

static PFNGLDRAWELEMENTSBASEVERTEXOESPROC
ResolveDrawElementsBaseVertexProc(bool prefer_ext) {
    static std::atomic<PFNGLDRAWELEMENTSBASEVERTEXOESPROC> oes_cached{nullptr};
    static std::atomic<PFNGLDRAWELEMENTSBASEVERTEXOESPROC> ext_cached{nullptr};
    return prefer_ext
        ? ResolveBaseVertexProc(ext_cached, "glDrawElementsBaseVertexEXT",
                                "glDrawElementsBaseVertexOES",
                                "glDrawElementsBaseVertex")
        : ResolveBaseVertexProc(oes_cached, "glDrawElementsBaseVertexOES",
                                "glDrawElementsBaseVertexEXT",
                                "glDrawElementsBaseVertex");
}

// Some GFx batches interleave 16-bit indices and 16/20-byte vertex records in
// one ring buffer. This leaves their float attributes at byte offset 2 mod 4.
// Mali's vertex fetch visibly drops triangles at those offsets. Copy only the
// referenced records to an aligned transient VBO; keep the original EBO.
// Seed221 identifies stride16 FLOAT2 UV/position records for the damaged LS/RS
// tutorial icons; stride20 adds a color field and is used by glyph batches.
static bool DrawGfxTextWithAlignedVertices(
    GLenum mode, GLsizei count, GLenum type, const void* indices,
    GLint base_vertex, PFNGLDRAWELEMENTSBASEVERTEXOESPROC function) {
    // ponytail: bound to 2048 glyph quads; widen only if a real UI exceeds it.
    constexpr uint32_t kMaxVertices = 8192;
    if (!CurrentLegacyPipelineIsGfxText() || !function ||
        mode != GL_TRIANGLES || type != GL_UNSIGNED_SHORT ||
        count <= 0 || count > (kMaxVertices / 4) * 6 || base_vertex != 0) return false;
    GLint source = 0, position_type = 0, position_stride = 0;
    void* position_pointer = nullptr;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &source);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &position_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &position_stride);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                              &position_pointer);
    if (source <= 0 || position_type != GL_FLOAT ||
        (position_stride != 16 && position_stride != 20) ||
        (reinterpret_cast<uintptr_t>(position_pointer) & 3u) == 0) return false;
    const size_t record_stride = static_cast<size_t>(position_stride);
    struct Attribute {
        GLuint index;
        GLint size, type, normalized, stride;
        void* pointer;
    };
    std::vector<Attribute> attributes;
    uintptr_t origin = std::numeric_limits<uintptr_t>::max();
    for (GLuint index = 0; index < 16; ++index) {
        GLint enabled = 0;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;
        Attribute attr{index, 0, 0, 0, 0, nullptr};
        GLint buffer = 0;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attr.size);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attr.type);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                           &attr.normalized);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attr.stride);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                           &buffer);
        glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                  &attr.pointer);
        if (buffer != source || attr.stride != position_stride ||
            (attr.type != GL_FLOAT && attr.type != GL_UNSIGNED_BYTE) ||
            attr.size < 1 || attr.size > 4) return false;
        origin = std::min(origin, reinterpret_cast<uintptr_t>(attr.pointer));
        attributes.push_back(attr);
    }
    if (attributes.empty() || (origin & 3u) != 2u) return false;
    InvalidateGlAttribCache();
    for (const Attribute& attr : attributes) {
        const uintptr_t relative = reinterpret_cast<uintptr_t>(attr.pointer) -
                                   origin;
        const size_t component = attr.type == GL_FLOAT ? 4 : 1;
        if (relative + static_cast<size_t>(attr.size) * component > record_stride ||
            relative % component != 0) return false;
    }
    GLint element_buffer = 0, old_array_buffer = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array_buffer);
    if (element_buffer <= 0) return false;
    std::vector<uint16_t> index_data(static_cast<size_t>(count));
    if (!CopyGlBufferShadow(static_cast<GLuint>(element_buffer),
                            reinterpret_cast<uintptr_t>(indices),
                            index_data.size() * sizeof(uint16_t),
                            index_data.data())) return false;
    const auto [first, last] = std::minmax_element(index_data.begin(),
                                                   index_data.end());
    if (*first != 0 || *last >= kMaxVertices) return false;
    const size_t vertex_bytes = (static_cast<size_t>(*last) + 1) * record_stride;
    std::vector<uint8_t> records(vertex_bytes);
    if (!CopyGlBufferShadow(static_cast<GLuint>(source), origin,
                            records.size(), records.data())) return false;
    GLuint scratch = 0;
    glGenBuffers(1, &scratch);
    glBindBuffer(GL_ARRAY_BUFFER, scratch);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertex_bytes),
                 records.data(), GL_STREAM_DRAW);
    GLint actual_size = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &actual_size);
    if (actual_size != static_cast<GLint>(vertex_bytes)) {
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_array_buffer));
        glDeleteBuffers(1, &scratch);
        return false;
    }
    for (const Attribute& attr : attributes) {
        const uintptr_t relative = reinterpret_cast<uintptr_t>(attr.pointer) -
                                   origin;
        glVertexAttribPointer(attr.index, attr.size,
                              static_cast<GLenum>(attr.type),
                              static_cast<GLboolean>(attr.normalized),
                              attr.stride, reinterpret_cast<const void*>(relative));
    }
    function(mode, count, type, indices, base_vertex);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(source));
    for (const Attribute& attr : attributes) {
        glVertexAttribPointer(attr.index, attr.size,
                              static_cast<GLenum>(attr.type),
                              static_cast<GLboolean>(attr.normalized),
                              attr.stride, attr.pointer);
    }
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_array_buffer));
    glDeleteBuffers(1, &scratch);
    static std::atomic<uint32_t> fixed_draws[2]{};
    const uint32_t occurrence = fixed_draws[record_stride == 16 ? 0 : 1].fetch_add(1,
                                                      std::memory_order_relaxed);
    if (occurrence < 12) {
        LOGI("GFx aligned text draw #%u source=%d origin=%zu stride=%zu records=%zu "
             "indices=%d", occurrence + 1, source, origin, record_stride,
             records.size(), count);
    }
    return true;
}

// Diagnostic A/B for the intermittent giant triangles in the menu world.
// The final blend's four records live deep inside a reused guest ring VBO.
// Snapshot only this exact fullscreen quad into fresh native storage so its
// vertex and index bytes cannot be overwritten while the GPU consumes them.
static bool DrawStablePostprocessQuad(GLenum mode, GLsizei count, GLenum type,
                                      const void* indices, GLint base_vertex) {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.bl2.stable_post_quad", value) > 0 &&
               value[0] == '1';
    }();
    if (!enabled || !CurrentLegacyPipelineIsPostprocessBlend() ||
        mode != GL_TRIANGLES || count != 6 || type != GL_UNSIGNED_SHORT ||
        base_vertex < 0 || base_vertex > 1000000) return false;

    GLint source = 0, uv_source = 0, ebo = 0, old_array = 0;
    GLint pos_enabled = 0, uv_enabled = 0;
    GLint pos_size = 0, uv_size = 0, pos_type = 0, uv_type = 0;
    GLint pos_stride = 0, uv_stride = 0;
    GLint pos_normalized = 0, uv_normalized = 0;
    GLint pos_divisor = 0, uv_divisor = 0;
    void* pos_pointer = nullptr;
    void* uv_pointer = nullptr;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &pos_enabled);
    glGetVertexAttribiv(8, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &uv_enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &source);
    glGetVertexAttribiv(8, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &uv_source);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &pos_size);
    glGetVertexAttribiv(8, GL_VERTEX_ATTRIB_ARRAY_SIZE, &uv_size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &pos_type);
    glGetVertexAttribiv(8, GL_VERTEX_ATTRIB_ARRAY_TYPE, &uv_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &pos_stride);
    glGetVertexAttribiv(8, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &uv_stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &pos_normalized);
    glGetVertexAttribiv(8, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &uv_normalized);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &pos_divisor);
    glGetVertexAttribiv(8, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &uv_divisor);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pos_pointer);
    glGetVertexAttribPointerv(8, GL_VERTEX_ATTRIB_ARRAY_POINTER, &uv_pointer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array);
    const uintptr_t position_offset = reinterpret_cast<uintptr_t>(pos_pointer);
    const uintptr_t uv_offset = reinterpret_cast<uintptr_t>(uv_pointer);
    if (!pos_enabled || !uv_enabled || source <= 0 || uv_source != source ||
        ebo <= 0 || pos_size != 4 || uv_size != 2 ||
        pos_type != GL_FLOAT || uv_type != GL_FLOAT ||
        pos_stride != 32 || uv_stride != 32 ||
        pos_normalized || uv_normalized || pos_divisor || uv_divisor ||
        position_offset % 32 != 0 || uv_offset != position_offset + 16) {
        return false;
    }

    std::array<uint16_t, 6> local_indices{};
    if (!CopyGlBufferShadow(static_cast<GLuint>(ebo),
                            reinterpret_cast<uintptr_t>(indices),
                            sizeof(local_indices), local_indices.data())) return false;
    for (uint16_t index : local_indices) {
        if (index > 3) return false;
    }
    const uint64_t origin64 = static_cast<uint64_t>(position_offset) +
                              static_cast<uint64_t>(base_vertex) * 32u;
    if (origin64 > std::numeric_limits<size_t>::max()) return false;
    std::array<uint8_t, 4 * 32> records{};
    if (!CopyGlBufferShadow(static_cast<GLuint>(source),
                            static_cast<size_t>(origin64), records.size(),
                            records.data())) return false;

    GLuint scratch[2] = {};
    glGenBuffers(2, scratch);
    if (!scratch[0] || !scratch[1]) {
        glDeleteBuffers(2, scratch);
        return false;
    }
    glBindBuffer(GL_ARRAY_BUFFER, scratch[0]);
    glBufferData(GL_ARRAY_BUFFER, records.size(), records.data(), GL_STREAM_DRAW);
    GLint actual_vertices = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &actual_vertices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scratch[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(local_indices),
                 local_indices.data(), GL_STREAM_DRAW);
    GLint actual_indices = 0;
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE,
                           &actual_indices);
    if (actual_vertices != static_cast<GLint>(records.size()) ||
        actual_indices != static_cast<GLint>(sizeof(local_indices))) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(ebo));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_array));
        glDeleteBuffers(2, scratch);
        return false;
    }
    InvalidateGlAttribCache();
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 32, nullptr);
    glVertexAttribPointer(8, 2, GL_FLOAT, GL_FALSE, 32,
                          reinterpret_cast<const void*>(16));
    glDrawElements(mode, count, type, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(source));
    glVertexAttribPointer(0, pos_size, static_cast<GLenum>(pos_type),
                          static_cast<GLboolean>(pos_normalized),
                          pos_stride, pos_pointer);
    glVertexAttribPointer(8, uv_size, static_cast<GLenum>(uv_type),
                          static_cast<GLboolean>(uv_normalized),
                          uv_stride, uv_pointer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(ebo));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_array));
    glDeleteBuffers(2, scratch);
    static std::atomic<uint32_t> staged{0};
    const uint32_t occurrence = staged.fetch_add(1, std::memory_order_relaxed);
    if (occurrence < 16 || occurrence % 2048 == 0) {
        LOGI("Stable postprocess quad #%u source=%d origin=%llu ebo=%d "
             "indices={%u,%u,%u,%u,%u,%u}", occurrence + 1, source,
             static_cast<unsigned long long>(origin64), ebo,
             local_indices[0], local_indices[1], local_indices[2],
             local_indices[3], local_indices[4], local_indices[5]);
    }
    return true;
}

static void SceneAuditVertexAlignment() {
    if (!SceneAuditEnabled()) return;
    static thread_local uint32_t draws = 0, floats = 0, unaligned = 0;
    static thread_local uint32_t bad_draws = 0;
    if (draws >= 1024) return;
    ++draws;
    GLint framebuffer = 0, program = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    bool bad = false;
    for (GLuint index = 0; index < 16; ++index) {
        GLint enabled = 0, type = 0;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        if (type != GL_FLOAT) continue;
        ++floats;
        void* pointer = nullptr;
        glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                  &pointer);
        const uintptr_t offset = reinterpret_cast<uintptr_t>(pointer);
        if ((offset & 3u) == 0) continue;
        ++unaligned;
        bad = true;
        if (bad_draws < 20) {
            LOGI("Scene vertex alignment draw=%u fbo=%d program=%d "
                 "attribute=%u floatOffset=0x%llX",
                 draws, framebuffer, program, index,
                 static_cast<unsigned long long>(offset));
        }
    }
    bad_draws += bad;
    if ((draws & 255u) == 0) {
        LOGI("Scene vertex alignment summary draws=%u floatAttrs=%u "
             "misalignedAttrs=%u misalignedDraws=%u",
             draws, floats, unaligned, bad_draws);
    }
}

static PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXOESPROC
ResolveDrawElementsInstancedBaseVertexProc(bool prefer_ext) {
    static std::atomic<PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXOESPROC> oes_cached{nullptr};
    static std::atomic<PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXOESPROC> ext_cached{nullptr};
    return prefer_ext
        ? ResolveBaseVertexProc(ext_cached,
                                "glDrawElementsInstancedBaseVertexEXT",
                                "glDrawElementsInstancedBaseVertexOES",
                                "glDrawElementsInstancedBaseVertex")
        : ResolveBaseVertexProc(oes_cached,
                                "glDrawElementsInstancedBaseVertexOES",
                                "glDrawElementsInstancedBaseVertexEXT",
                                "glDrawElementsInstancedBaseVertex");
}

void RegisterGles2CoreThunks(ThunkManager& manager) {
    manager.RegisterThunkIfAbsent("glBlendColor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLfloat red = args.F32();
        const GLfloat green = args.F32();
        const GLfloat blue = args.F32();
        const GLfloat alpha = args.F32();
        glBlendColor(red, green, blue, alpha);
    });
    manager.RegisterThunkIfAbsent("glBlendEquation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glBlendEquation(static_cast<GLenum>(args.U32()));
    });
    manager.RegisterThunkIfAbsent("glBlendEquationSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum rgb = static_cast<GLenum>(args.U32());
        const GLenum alpha = static_cast<GLenum>(args.U32());
        glBlendEquationSeparate(rgb, alpha);
    });
    manager.RegisterThunkIfAbsent("glClearDepthf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glClearDepthf(args.F32());
    });
    manager.RegisterThunkIfAbsent("glClearBufferfv", [](Dynarmic::A32::Jit& jit,
                                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum buffer = static_cast<GLenum>(args.U32());
        const GLint draw_buffer = static_cast<GLint>(args.I32());
        const uint32_t value_address = args.Ptr();
        size_t component_count = 0;
        if (buffer == GL_COLOR) component_count = 4;
        else if (buffer == GL_DEPTH) component_count = 1;
        else return;
        const GLfloat* value = GuestInputArray<GLfloat>(
            host, value_address, 1, component_count, "glClearBufferfv");
        if (!value) return;
        glClearBufferfv(buffer, draw_buffer, value);
    });
    manager.RegisterThunkIfAbsent("glClearStencil", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glClearStencil(static_cast<GLint>(args.I32()));
    });
    manager.RegisterThunkIfAbsent("glCompressedTexSubImage2D", [](Dynarmic::A32::Jit& jit,
                                                     DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLint xoffset = static_cast<GLint>(args.I32());
        const GLint yoffset = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        const GLenum format = static_cast<GLenum>(args.U32());
        const GLsizei image_size = static_cast<GLsizei>(args.I32());
        const uint32_t data_address = args.Ptr();
        GLint pbo = 0;
        if (HasGlES3()) glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
        const void* data = nullptr;
        if (pbo != 0) {
            data = reinterpret_cast<const void*>(static_cast<uintptr_t>(data_address));
        } else {
            bool valid = false;
            data = CountedGuestBytes(
                host, data_address, image_size, "glCompressedTexSubImage2D", &valid);
            if (!valid) return;
        }
        glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height,
                                  format, image_size, data);
    });
    manager.RegisterThunkIfAbsent("glTexSubImage2D", [](Dynarmic::A32::Jit& jit,
                                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLint x_offset = static_cast<GLint>(args.I32());
        const GLint y_offset = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        const GLenum format = static_cast<GLenum>(args.U32());
        const GLenum type = static_cast<GLenum>(args.U32());
        const uint32_t pixels_address = args.Ptr();
        if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
            if (!UploadBgraPixels2D(host, target, level, GL_RGBA8, width, height,
                                    0, x_offset, y_offset, pixels_address, true)) {
                LOGE("Rejected BGRA subimage level=%d rect=%d,%d %dx%d source=0x%08X",
                     level, x_offset, y_offset, width, height, pixels_address);
            }
            return;
        }
        const void* pixels = nullptr;
        if (!ResolveGlPixels(host, width, height, 1, format, type,
                             pixels_address, &pixels, false)) {
            return;
        }
        static std::atomic<uint32_t> calls{0};
        const uint32_t index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        GLint texture = 0;
        GLint pbo = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
        if (index <= 24) {
            LOGI("GL telemetry: glTexSubImage2D #%u texture=%d pbo=%d "
                 "target=0x%04X level=%d rect=%d,%d %dx%d "
                 "format=0x%04X type=0x%04X pixels=0x%08X",
                 index, texture, pbo, target, level, x_offset, y_offset,
                 width, height, format, type, pixels_address);
        } else if (index == 25) {
            LOGI("GL telemetry: suppressing further glTexSubImage2D calls "
                 "after 24");
        }
        const bool audit_upload = index <= 12 ||
                                  (index >= 31 && index <= 33) ||
                                  (index >= 91 && index <= 93) ||
                                  (index >= 181 && index <= 183);
        if (PresentAuditEnabled() && audit_upload && width > 0 && height > 0 &&
            type == GL_UNSIGNED_BYTE &&
            (format == GL_RED || format == GL_ALPHA || format == GL_LUMINANCE)) {
            const uint64_t byte_count64 = static_cast<uint64_t>(width) *
                                          static_cast<uint64_t>(height);
            if (byte_count64 <= 16 * 1024 * 1024) {
                const size_t byte_count = static_cast<size_t>(byte_count64);
                if (pbo == 0) {
                    LogByteStats("texture-upload", index,
                                 static_cast<const uint8_t*>(pixels), byte_count);
                } else {
                    GLint buffer_size = 0;
                    glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER,
                                           GL_BUFFER_SIZE, &buffer_size);
                    const uint64_t end = static_cast<uint64_t>(pixels_address) +
                                         byte_count64;
                    if (end <= static_cast<uint64_t>(std::max(buffer_size, 0))) {
                        void* mapped = glMapBufferRange(
                            GL_PIXEL_UNPACK_BUFFER,
                            static_cast<GLintptr>(pixels_address),
                            static_cast<GLsizeiptr>(byte_count), GL_MAP_READ_BIT);
                        const GLenum map_error = glGetError();
                        LOGI("Present audit #%u texture-upload pbo=%d size=%d "
                             "map_error=0x%04X", index, pbo, buffer_size,
                             map_error);
                        if (mapped) {
                            LogByteStats("texture-upload", index,
                                         static_cast<const uint8_t*>(mapped),
                                         byte_count);
                            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                        }
                    } else {
                        LOGE("Present audit #%u texture-upload range exceeds "
                             "pbo=%d size=%d offset=%u bytes=%zu",
                             index, pbo, buffer_size, pixels_address, byte_count);
                    }
                }
            }
        }
        if (PresentAuditEnabled() && audit_upload) {
            GLenum pending = GL_NO_ERROR;
            while ((pending = glGetError()) != GL_NO_ERROR) {
                LOGE("Present audit #%u texture-upload pre-call error=0x%04X",
                     index, pending);
            }
        }
        GLint atlas_width = 0, atlas_height = 0;
        if (AtlasUploadAuditEnabled() && target == GL_TEXTURE_2D &&
            format == GL_LUMINANCE && type == GL_UNSIGNED_BYTE && level == 0) {
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                                     &atlas_width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                                     &atlas_height);
        }
        const bool atlas_candidate = atlas_width == 1024 &&
            atlas_height == 1024 && x_offset < 875 &&
            static_cast<int64_t>(x_offset) + width > 747 && y_offset < 44 &&
            static_cast<int64_t>(y_offset) + height > 0;
        // This opt-in probe consumes pre-existing GL errors while isolating
        // this upload. Deferring them changes the guest's startup check.
        const GLenum before_upload_error = atlas_candidate ? glGetError() : GL_NO_ERROR;
        glTexSubImage2D(target, level, x_offset, y_offset, width, height,
                        TranslateLegacyGlExternalFormat(format), type, pixels);
        if (atlas_candidate) {
            const GLenum upload_error = glGetError();
            LOGI("Atlas upload pre-readback texture=%d beforeError=0x%04X "
                 "uploadError=0x%04X", texture, before_upload_error,
                 upload_error);
            AuditLuminanceAtlasUpload(texture, pbo, level, x_offset, y_offset,
                                       width, height, pixels_address, pixels);
        }
        if (PresentAuditEnabled() && audit_upload) {
            LOGI("Present audit #%u texture-upload call error=0x%04X", index,
                 glGetError());
        }
    });
    manager.RegisterThunkIfAbsent("glCopyTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLenum internal_format = static_cast<GLenum>(args.U32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        const GLint border = static_cast<GLint>(args.I32());
        glCopyTexImage2D(target, level, internal_format, x, y, width, height, border);
    });
    manager.RegisterThunkIfAbsent("glCopyTexSubImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLint xoffset = static_cast<GLint>(args.I32());
        const GLint yoffset = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    });
    manager.RegisterThunkIfAbsent("glDetachShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLuint shader = static_cast<GLuint>(args.U32());
        glDetachShader(program, shader);
    });

    manager.RegisterThunkIfAbsent("glGetAttachedShaders", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLsizei max_count = static_cast<GLsizei>(args.I32());
        const uint32_t count_address = args.Ptr();
        const uint32_t shaders_address = args.Ptr();
        bool count_valid = false;
        GLsizei* count = OptionalGuestOutput<GLsizei>(
            host, count_address, 1, "glGetAttachedShaders", &count_valid);
        if (!count_valid) return;
        GLuint* shaders = nullptr;
        if (max_count > 0) {
            shaders = GuestOutputArray<GLuint>(
                host, shaders_address, static_cast<size_t>(max_count), 1,
                "glGetAttachedShaders");
            if (!shaders) return;
        }
        glGetAttachedShaders(program, max_count, count, shaders);
    });
    manager.RegisterThunkIfAbsent("glGetBufferParameteriv", [](Dynarmic::A32::Jit& jit,
                                                  DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), 1, 1, "glGetBufferParameteriv");
        if (!params) return;
        glGetBufferParameteriv(target, pname, params);
    });
    manager.RegisterThunkIfAbsent("glGetQueryObjectuiv", [](Dynarmic::A32::Jit& jit,
                                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint query = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLuint* params = GuestOutputArray<GLuint>(
            host, args.Ptr(), 1, 1, "glGetQueryObjectuiv");
        if (!params) return;
        glGetQueryObjectuiv(query, pname, params);
        const GLuint native_value = *params;
        const int probe_mode = OcclusionProbeMode();
        bool desktop_query = false;
        if (pname == GL_QUERY_RESULT) {
            std::lock_guard lock(s_desktop_occlusion_queries_mutex);
            desktop_query = s_desktop_occlusion_queries.contains(query);
            if (desktop_query && (native_value != 0 || probe_mode == 2)) {
                // GLES exposes only a boolean result. A raw 1 would fail the
                // UE3 caller's sample-count threshold and falsely cull even
                // large visible objects. Keep the ordinary UINT_MAX mapping
                // until a bounded count is validated on the menu and gameplay.
                *params = probe_mode == 3 ? 65535u :
                          probe_mode == 4 ? 262144u :
                          probe_mode == 5 ? 1048576u :
                          std::numeric_limits<GLuint>::max();
            }
        }
        static std::atomic<uint32_t> query_reads{0};
        const uint32_t read = query_reads.fetch_add(1, std::memory_order_relaxed);
        static thread_local int logged_mode = -1;
        static thread_local uint32_t probe_logs = 0;
        if (logged_mode != probe_mode) {
            logged_mode = probe_mode;
            probe_logs = 0;
        }
        if (read < 64 || (probe_mode != 0 && probe_logs++ < 128)) {
            LOGI("Occlusion query read #%u id=%u pname=0x%04X native=%u guest=%u desktop=%d probe=%d",
                 read + 1, query, pname, native_value, *params,
                 desktop_query, probe_mode);
        }
    });
    manager.RegisterThunkIfAbsent("glGetFloatv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum pname = static_cast<GLenum>(args.U32());
        const size_t count = GetFloatStateValueCount(pname);
        GLfloat* data = GuestOutputArray<GLfloat>(
            host, args.Ptr(), count, 1, "glGetFloatv");
        if (!data) return;
        glGetFloatv(pname, data);
    });
    manager.RegisterThunkIfAbsent("glGetShaderPrecisionFormat", [](Dynarmic::A32::Jit& jit,
                                                      DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum shader_type = static_cast<GLenum>(args.U32());
        const GLenum precision_type = static_cast<GLenum>(args.U32());
        GLint* range = GuestOutputArray<GLint>(
            host, args.Ptr(), 2, 1, "glGetShaderPrecisionFormat");
        GLint* precision = GuestOutputArray<GLint>(
            host, args.Ptr(), 1, 1, "glGetShaderPrecisionFormat");
        if (!range || !precision) return;
        glGetShaderPrecisionFormat(shader_type, precision_type, range, precision);
    });
    manager.RegisterThunkIfAbsent("glGetShaderSource", [](Dynarmic::A32::Jit& jit,
                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint shader = static_cast<GLuint>(args.U32());
        const GLsizei buffer_size = static_cast<GLsizei>(args.I32());
        const uint32_t length_address = args.Ptr();
        const uint32_t source_address = args.Ptr();
        bool length_valid = false;
        GLsizei* length = OptionalGuestOutput<GLsizei>(
            host, length_address, 1, "glGetShaderSource", &length_valid);
        if (!length_valid) return;
        GLchar* source = nullptr;
        if (buffer_size > 0) {
            source = GuestOutputArray<GLchar>(
                host, source_address, static_cast<size_t>(buffer_size), 1,
                "glGetShaderSource");
            if (!source) return;
        }
        glGetShaderSource(shader, buffer_size, length, source);
    });
    manager.RegisterThunkIfAbsent("glGetTexParameterfv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLfloat* params = GuestOutputArray<GLfloat>(
            host, args.Ptr(), GetTextureParameterValueCount(pname), 1,
            "glGetTexParameterfv");
        if (!params) return;
        glGetTexParameterfv(target, pname, params);
    });
    manager.RegisterThunkIfAbsent("glGetTexParameteriv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), GetTextureParameterValueCount(pname), 1,
            "glGetTexParameteriv");
        if (!params) return;
        glGetTexParameteriv(target, pname, params);
    });
    manager.RegisterThunkIfAbsent("glGetUniformfv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLint location = static_cast<GLint>(args.I32());
        // A single GLES2 uniform location contains at most one mat4 (16 scalars).
        GLfloat* params = GuestOutputArray<GLfloat>(
            host, args.Ptr(), 16, 1, "glGetUniformfv");
        if (!params) return;
        glGetUniformfv(program, location, params);
    });
    manager.RegisterThunkIfAbsent("glGetUniformiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLint location = static_cast<GLint>(args.I32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), 16, 1, "glGetUniformiv");
        if (!params) return;
        glGetUniformiv(program, location, params);
    });
    manager.RegisterThunkIfAbsent("glGetVertexAttribfv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLfloat* params = GuestOutputArray<GLfloat>(
            host, args.Ptr(), GetVertexAttribValueCount(pname), 1,
            "glGetVertexAttribfv");
        if (!params) return;
        glGetVertexAttribfv(index, pname, params);
    });
    manager.RegisterThunkIfAbsent("glGetVertexAttribiv", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        GLint* params = GuestOutputArray<GLint>(
            host, args.Ptr(), GetVertexAttribValueCount(pname), 1,
            "glGetVertexAttribiv");
        if (!params) return;
        glGetVertexAttribiv(index, pname, params);
    });
    manager.RegisterThunkIfAbsent("glGetVertexAttribPointerv", [](Dynarmic::A32::Jit& jit,
                                                     DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        uint32_t* guest_pointer = GuestOutputArray<uint32_t>(
            host, args.Ptr(), 1, 1, "glGetVertexAttribPointerv");
        if (!guest_pointer) return;
        void* host_pointer = nullptr;
        GLint array_buffer_binding = 0;
        glGetVertexAttribPointerv(index, pname, &host_pointer);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                            &array_buffer_binding);
        *guest_pointer = HostVertexPointerToGuest(
            host, host_pointer, array_buffer_binding);
    });

    manager.RegisterThunkIfAbsent("glIsBuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        jit.Regs()[0] = static_cast<uint32_t>(glIsBuffer(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunkIfAbsent("glIsEnabled", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        jit.Regs()[0] = static_cast<uint32_t>(glIsEnabled(static_cast<GLenum>(args.U32())));
    });
    manager.RegisterThunkIfAbsent("glIsFramebuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        jit.Regs()[0] = static_cast<uint32_t>(
            glIsFramebuffer(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunkIfAbsent("glIsProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        jit.Regs()[0] = static_cast<uint32_t>(glIsProgram(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunkIfAbsent("glIsRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        jit.Regs()[0] = static_cast<uint32_t>(
            glIsRenderbuffer(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunkIfAbsent("glIsShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        jit.Regs()[0] = static_cast<uint32_t>(glIsShader(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunkIfAbsent("glIsTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        jit.Regs()[0] = static_cast<uint32_t>(glIsTexture(static_cast<GLuint>(args.U32())));
    });
    manager.RegisterThunkIfAbsent("glReleaseShaderCompiler", [](Dynarmic::A32::Jit&, DynarmicHost&) {
        glReleaseShaderCompiler();
    });
    manager.RegisterThunkIfAbsent("glSampleCoverage", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLfloat value = args.F32();
        const GLboolean invert = static_cast<GLboolean>(args.U32());
        glSampleCoverage(value, invert);
    });

    manager.RegisterThunkIfAbsent("glStencilFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum function = static_cast<GLenum>(args.U32());
        const GLint reference = static_cast<GLint>(args.I32());
        const GLuint mask = static_cast<GLuint>(args.U32());
        glStencilFunc(function, reference, mask);
    });
    manager.RegisterThunkIfAbsent("glStencilFuncSeparate", [](Dynarmic::A32::Jit& jit,
                                                 DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum face = static_cast<GLenum>(args.U32());
        const GLenum function = static_cast<GLenum>(args.U32());
        const GLint reference = static_cast<GLint>(args.I32());
        const GLuint mask = static_cast<GLuint>(args.U32());
        glStencilFuncSeparate(face, function, reference, mask);
    });
    manager.RegisterThunkIfAbsent("glStencilMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glStencilMask(static_cast<GLuint>(args.U32()));
    });
    manager.RegisterThunkIfAbsent("glStencilMaskSeparate", [](Dynarmic::A32::Jit& jit,
                                                 DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum face = static_cast<GLenum>(args.U32());
        const GLuint mask = static_cast<GLuint>(args.U32());
        glStencilMaskSeparate(face, mask);
    });
    manager.RegisterThunkIfAbsent("glStencilOp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum fail = static_cast<GLenum>(args.U32());
        const GLenum depth_fail = static_cast<GLenum>(args.U32());
        const GLenum depth_pass = static_cast<GLenum>(args.U32());
        glStencilOp(fail, depth_fail, depth_pass);
    });
    manager.RegisterThunkIfAbsent("glStencilOpSeparate", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum face = static_cast<GLenum>(args.U32());
        const GLenum stencil_fail = static_cast<GLenum>(args.U32());
        const GLenum depth_fail = static_cast<GLenum>(args.U32());
        const GLenum depth_pass = static_cast<GLenum>(args.U32());
        glStencilOpSeparate(face, stencil_fail, depth_fail, depth_pass);
    });
    manager.RegisterThunkIfAbsent("glTexParameterfv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        const GLfloat* params = GuestInputArray<GLfloat>(
            host, args.Ptr(), GetTextureParameterValueCount(pname), 1,
            "glTexParameterfv");
        if (!params) return;
        glTexParameterfv(target, pname, params);
    });

    manager.RegisterThunkIfAbsent("glUniform1fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLfloat* value = CountedGuestValues<GLfloat>(
            host, value_address, count, 1, "glUniform1fv", &valid);
        if (!valid) return;
        glUniform1fv(location, count, value);
    });
    manager.RegisterThunkIfAbsent("glUniform1iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 1, "glUniform1iv", &valid);
        if (!valid) return;
        glUniform1iv(location, count, value);
    });
    manager.RegisterThunkIfAbsent("glUniform2fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLfloat* value = CountedGuestValues<GLfloat>(
            host, value_address, count, 2, "glUniform2fv", &valid);
        if (!valid) return;
        glUniform2fv(location, count, value);
    });
    manager.RegisterThunkIfAbsent("glUniform2i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        glUniform2i(location, x, y);
    });
    manager.RegisterThunkIfAbsent("glUniform2iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 2, "glUniform2iv", &valid);
        if (!valid) return;
        glUniform2iv(location, count, value);
    });
    manager.RegisterThunkIfAbsent("glUniform3i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLint z = static_cast<GLint>(args.I32());
        glUniform3i(location, x, y, z);
    });
    manager.RegisterThunkIfAbsent("glUniform3iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 3, "glUniform3iv", &valid);
        if (!valid) return;
        glUniform3iv(location, count, value);
    });
    manager.RegisterThunkIfAbsent("glUniform4i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLint x = static_cast<GLint>(args.I32());
        const GLint y = static_cast<GLint>(args.I32());
        const GLint z = static_cast<GLint>(args.I32());
        const GLint w = static_cast<GLint>(args.I32());
        glUniform4i(location, x, y, z, w);
    });
    manager.RegisterThunkIfAbsent("glUniform4iv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLint* value = CountedGuestValues<GLint>(
            host, value_address, count, 4, "glUniform4iv", &valid);
        if (!valid) return;
        glUniform4iv(location, count, value);
    });
    manager.RegisterThunkIfAbsent("glUniformMatrix2fv", [](Dynarmic::A32::Jit& jit,
                                              DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const GLboolean transpose = static_cast<GLboolean>(args.U32());
        const uint32_t value_address = args.Ptr();
        bool valid = false;
        const GLfloat* value = CountedGuestValues<GLfloat>(
            host, value_address, count, 4, "glUniformMatrix2fv", &valid);
        if (!valid) return;
        glUniformMatrix2fv(location, count, transpose, value);
    });
    manager.RegisterThunkIfAbsent("glValidateProgram", [](Dynarmic::A32::Jit& jit,
                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glValidateProgram(static_cast<GLuint>(args.U32()));
    });

    manager.RegisterThunkIfAbsent("glVertexAttrib1f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        glVertexAttrib1f(index, x);
    });
    manager.RegisterThunkIfAbsent("glVertexAttrib1fv", [](Dynarmic::A32::Jit& jit,
                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 1, "glVertexAttrib1fv");
        if (!values) return;
        glVertexAttrib1fv(index, values);
    });
    manager.RegisterThunkIfAbsent("glVertexAttrib2f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        const GLfloat y = args.F32();
        glVertexAttrib2f(index, x, y);
    });
    manager.RegisterThunkIfAbsent("glVertexAttrib2fv", [](Dynarmic::A32::Jit& jit,
                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 2, "glVertexAttrib2fv");
        if (!values) return;
        glVertexAttrib2fv(index, values);
    });
    manager.RegisterThunkIfAbsent("glVertexAttrib3f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        const GLfloat y = args.F32();
        const GLfloat z = args.F32();
        glVertexAttrib3f(index, x, y, z);
    });
    manager.RegisterThunkIfAbsent("glVertexAttrib3fv", [](Dynarmic::A32::Jit& jit,
                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 3, "glVertexAttrib3fv");
        if (!values) return;
        glVertexAttrib3fv(index, values);
    });
    manager.RegisterThunkIfAbsent("glVertexAttrib4f", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat x = args.F32();
        const GLfloat y = args.F32();
        const GLfloat z = args.F32();
        const GLfloat w = args.F32();
        glVertexAttrib4f(index, x, y, z, w);
    });
    manager.RegisterThunkIfAbsent("glVertexAttrib4fv", [](Dynarmic::A32::Jit& jit,
                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLfloat* values = GuestInputArray<GLfloat>(
            host, args.Ptr(), 1, 4, "glVertexAttrib4fv");
        if (!values) return;
        glVertexAttrib4fv(index, values);
    });

    // Keep the advertised GLES 3.0 surface complete. Unity resolves these as a
    // capability table; returning null for isolated core functions disables otherwise
    // valid shader, UBO, program-binary, instancing, and 3D-texture paths.
    manager.RegisterThunkIfAbsent("glFinish", [](Dynarmic::A32::Jit&, DynarmicHost&) {
        glFinish();
    });
    manager.RegisterThunkIfAbsent("glGetFramebufferAttachmentParameteriv",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            Arm32CallArgs args(jit, host);
            const GLenum target = static_cast<GLenum>(args.U32());
            const GLenum attachment = static_cast<GLenum>(args.U32());
            const GLenum pname = static_cast<GLenum>(args.U32());
            GLint* params = GuestOutputArray<GLint>(
                host, args.Ptr(), 1, 1, "glGetFramebufferAttachmentParameteriv");
            if (params) {
                glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
            }
        });
    manager.RegisterThunkIfAbsent("glTexParameteriv", [](Dynarmic::A32::Jit& jit,
                                                   DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        const GLint* params = GuestInputArray<GLint>(
            host, args.Ptr(), GetTextureParameterValueCount(pname), 1,
            "glTexParameteriv");
        if (params) glTexParameteriv(target, pname, params);
    });

    manager.RegisterThunkIfAbsent("glGenQueries", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t ids_address = args.Ptr();
        GLuint* ids = count > 0 ? GuestOutputArray<GLuint>(
            host, ids_address, static_cast<size_t>(count), 1, "glGenQueries") : nullptr;
        if (count <= 0 || ids) glGenQueries(count, ids);
    });
    manager.RegisterThunkIfAbsent("glDeleteQueries", [](Dynarmic::A32::Jit& jit,
                                                  DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLsizei count = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const GLuint* ids = CountedGuestValues<GLuint>(
            host, args.Ptr(), count, 1, "glDeleteQueries", &valid);
        if (valid) {
            if (count > 0) {
                std::lock_guard lock(s_desktop_occlusion_queries_mutex);
                for (GLsizei i = 0; i < count; ++i) {
                    s_desktop_occlusion_queries.erase(ids[i]);
                }
            }
            glDeleteQueries(count, ids);
        }
    });
    manager.RegisterThunkIfAbsent("glBeginQuery", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLuint id = static_cast<GLuint>(args.U32());
        // Desktop GL counts samples; GLES 3.2 exposes only boolean occlusion.
        // Preserve the guest's nonzero-visibility test without passing its
        // unsupported enum to the driver.
        constexpr GLenum kDesktopSamplesPassed = 0x8914;
        const GLenum native_target = target == kDesktopSamplesPassed
            ? GL_ANY_SAMPLES_PASSED : target;
        {
            std::lock_guard lock(s_desktop_occlusion_queries_mutex);
            if (target == kDesktopSamplesPassed) {
                s_desktop_occlusion_queries.insert(id);
            } else {
                s_desktop_occlusion_queries.erase(id);
            }
        }
        static std::atomic<uint32_t> query_starts{0};
        const uint32_t start = query_starts.fetch_add(1, std::memory_order_relaxed);
        if (start < 64) {
            LOGI("Occlusion query begin #%u guest_target=0x%04X native_target=0x%04X id=%u",
                 start + 1, target, native_target, id);
        }
        glBeginQuery(native_target, id);
    });
    manager.RegisterThunkIfAbsent("glEndQuery", [](Dynarmic::A32::Jit& jit,
                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        constexpr GLenum kDesktopSamplesPassed = 0x8914;
        const GLenum native_target = target == kDesktopSamplesPassed
            ? GL_ANY_SAMPLES_PASSED : target;
        static std::atomic<uint32_t> query_ends{0};
        const uint32_t end = query_ends.fetch_add(1, std::memory_order_relaxed);
        if (end < 64) {
            LOGI("Occlusion query end #%u guest_target=0x%04X native_target=0x%04X",
                 end + 1, target, native_target);
        }
        glEndQuery(native_target);
    });

    manager.RegisterThunkIfAbsent("glBeginTransformFeedback", [](Dynarmic::A32::Jit& jit,
                                                            DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glBeginTransformFeedback(static_cast<GLenum>(args.U32()));
    });
    manager.RegisterThunkIfAbsent("glEndTransformFeedback", [](Dynarmic::A32::Jit&,
                                                          DynarmicHost&) {
        glEndTransformFeedback();
    });
    manager.RegisterThunkIfAbsent("glBindTransformFeedback", [](Dynarmic::A32::Jit& jit,
                                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glBindTransformFeedback(static_cast<GLenum>(args.U32()),
                                static_cast<GLuint>(args.U32()));
    });
    manager.RegisterThunkIfAbsent("glGenTransformFeedbacks", [](Dynarmic::A32::Jit& jit,
                                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t ids_address = args.Ptr();
        GLuint* ids = count > 0 ? GuestOutputArray<GLuint>(
            host, ids_address, static_cast<size_t>(count), 1,
            "glGenTransformFeedbacks") : nullptr;
        if (count <= 0 || ids) glGenTransformFeedbacks(count, ids);
    });
    manager.RegisterThunkIfAbsent("glDeleteTransformFeedbacks", [](Dynarmic::A32::Jit& jit,
                                                              DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLsizei count = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const GLuint* ids = CountedGuestValues<GLuint>(
            host, args.Ptr(), count, 1, "glDeleteTransformFeedbacks", &valid);
        if (valid) glDeleteTransformFeedbacks(count, ids);
    });
    manager.RegisterThunkIfAbsent("glTransformFeedbackVaryings", [](Dynarmic::A32::Jit& jit,
                                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t varyings_address = args.Ptr();
        const GLenum buffer_mode = static_cast<GLenum>(args.U32());
        std::vector<const GLchar*> varyings;
        if (!GuestGlNameArray(host, varyings_address, count,
                              "glTransformFeedbackVaryings", &varyings)) {
            return;
        }
        glTransformFeedbackVaryings(
            program, count, count > 0 ? varyings.data() : nullptr, buffer_mode);
    });

    manager.RegisterThunkIfAbsent("glBlitFramebuffer", [](Dynarmic::A32::Jit& jit,
                                                    DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint src_x0 = static_cast<GLint>(args.I32());
        const GLint src_y0 = static_cast<GLint>(args.I32());
        const GLint src_x1 = static_cast<GLint>(args.I32());
        const GLint src_y1 = static_cast<GLint>(args.I32());
        const GLint dst_x0 = static_cast<GLint>(args.I32());
        const GLint dst_y0 = static_cast<GLint>(args.I32());
        const GLint dst_x1 = static_cast<GLint>(args.I32());
        const GLint dst_y1 = static_cast<GLint>(args.I32());
        const GLbitfield mask = static_cast<GLbitfield>(args.U32());
        const GLenum filter = static_cast<GLenum>(args.U32());
        GLint read_framebuffer = 0;
        GLint draw_framebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
        static std::atomic<uint32_t> calls{0};
        const uint32_t index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (index <= 24) {
            LOGI("GL telemetry: glBlitFramebuffer #%u read=%d draw=%d "
                 "src=%d,%d-%d,%d dst=%d,%d-%d,%d mask=0x%08X filter=0x%04X",
                 index, read_framebuffer, draw_framebuffer,
                 src_x0, src_y0, src_x1, src_y1,
                 dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
        } else if (index == 25) {
            LOGI("GL telemetry: suppressing further glBlitFramebuffer calls "
                 "after 24");
        }
        // BL2 restarts its render worker after the Bink movies. Sample the
        // first two default-framebuffer presents on each worker, not only
        // process-global movie frames8/16.
        static thread_local uint32_t audited_presents = 0;
        const uint32_t local_present = draw_framebuffer == 0 ? ++audited_presents : 0;
        const bool audit = PresentAuditEnabled() && draw_framebuffer == 0 &&
                           (local_present == 1 || local_present == 2 ||
                            local_present == 100 || local_present == 110 ||
                            local_present == 128);
        if (audit) {
            GLenum pending = GL_NO_ERROR;
            while ((pending = glGetError()) != GL_NO_ERROR) {
                LOGE("Present audit #%u pending GL error=0x%04X", index,
                     pending);
            }
            const GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            const GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            LOGI("Present audit #%u status read=0x%04X draw=0x%04X",
                 index, read_status, draw_status);
            LogFramebufferPixels("source", index,
                                 src_x0, src_y0, src_x1, src_y1);
        }
        const bool error_audit = GlStateAuditEnabled();
        GLint read_buffer = 0, draw_buffer0 = 0, max_draw_buffers = 0;
        uint32_t active_draw_mask = 0;
        bool all_draw_buffers_read = true;
        if (error_audit) {
            glGetIntegerv(GL_READ_BUFFER, &read_buffer);
            glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
            // ponytail: snapshot at most16 draw buffers; log the host maximum
            // so a larger host cannot be mistaken for complete coverage.
            for (GLint i = 0; i < std::min(max_draw_buffers, 16); ++i) {
                GLint draw_buffer = 0;
                glGetIntegerv(GL_DRAW_BUFFER0 + i, &draw_buffer);
                if (i == 0) draw_buffer0 = draw_buffer;
                if (draw_buffer == GL_NONE) continue;
                active_draw_mask |= 1u << i;
                all_draw_buffers_read &= draw_buffer == read_buffer;
            }
        }
        const bool identity_noop = IsIdentityColorBlit(
            read_framebuffer, draw_framebuffer,
            src_x0, src_y0, src_x1, src_y1,
            dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
        const GLenum before_error = error_audit
            ? CaptureGlError("before glBlitFramebuffer") : GL_NO_ERROR;
        if (!identity_noop) {
            glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1,
                              dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
        } else {
            static std::atomic<uint32_t> identity_copies{0};
            if (identity_copies.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOGI("GL compatibility: identity color self-blit #%u "
                     "framebuffer=%d retained unchanged", index, read_framebuffer);
            }
        }
        const GLenum after_error = error_audit
            ? CaptureGlError("after glBlitFramebuffer") : GL_NO_ERROR;
        static std::atomic<uint32_t> audit_error_snapshots{0};
        if (error_audit && (index <= 24 ||
            ((before_error != GL_NO_ERROR || after_error != GL_NO_ERROR) &&
             audit_error_snapshots.fetch_add(1, std::memory_order_relaxed) < 16))) {
            LOGI("GL blit audit #%u before=0x%04X after=0x%04X "
                 "read=%d draw=%d readBuffer=0x%04X drawBuffer0=0x%04X "
                 "activeDrawMask=0x%04X allDrawRead=%d maxDrawBuffers=%d "
                 "identityNoop=%d",
                 index, before_error, after_error, read_framebuffer,
                 draw_framebuffer, read_buffer, draw_buffer0,
                 active_draw_mask, all_draw_buffers_read, max_draw_buffers,
                 identity_noop);
        }
        // A viewport belongs to a draw, not to this blit's source rectangle.
        // Mode2 deliberately observes only post-draw/transition upper regions.
        if (SceneAuditMode() == 1 && read_framebuffer == 23 &&
            draw_framebuffer == 0) {
            static thread_local uint32_t scene_presents = 0;
            if (scene_presents < 3 || scene_presents == 96 ||
                scene_presents == 160) {
                const uint32_t sample = ++scene_presents;
                SceneAuditDumpGfxFramebuffer(23, 10000 + sample);
                SceneAuditDumpDefaultFramebuffer(sample);
            }
        }
        if (audit) {
            const GLenum blit_error = glGetError();
            LOGI("Present audit #%u blit error=0x%04X", index, blit_error);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            LogFramebufferPixels("destination", index,
                                 dst_x0, dst_y0, dst_x1, dst_y1);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              static_cast<GLuint>(read_framebuffer));
        }
    });
    manager.RegisterThunkIfAbsent("glRenderbufferStorageMultisample",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            Arm32CallArgs args(jit, host);
            const GLenum target = static_cast<GLenum>(args.U32());
            const GLsizei samples = static_cast<GLsizei>(args.I32());
            const GLenum internal_format = static_cast<GLenum>(args.U32());
            const GLsizei width = static_cast<GLsizei>(args.I32());
            const GLsizei height = static_cast<GLsizei>(args.I32());
            glRenderbufferStorageMultisample(
                target, samples, internal_format, width, height);
        });
    manager.RegisterThunkIfAbsent("glFramebufferTextureLayer", [](Dynarmic::A32::Jit& jit,
                                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum attachment = static_cast<GLenum>(args.U32());
        const GLuint texture = static_cast<GLuint>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLint layer = static_cast<GLint>(args.I32());
        glFramebufferTextureLayer(target, attachment, texture, level, layer);
    });
    manager.RegisterThunkIfAbsent("glReadBuffer", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        glReadBuffer(static_cast<GLenum>(args.U32()));
    });

    manager.RegisterThunkIfAbsent("glDrawArraysInstanced", [](Dynarmic::A32::Jit& jit,
                                                         DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum mode = static_cast<GLenum>(args.U32());
        const GLint first = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const GLsizei instance_count = static_cast<GLsizei>(args.I32());
        SyncGuestMappedBuffersForGpuRead(host);
        if (!RefreshLegacyProgramStateForDraw()) return;
        SceneAuditLogFinalInvocation("glDrawArraysInstanced", first, count,
                                      GL_NONE, 0, 0, instance_count);
        SceneAuditLogFinalQuadDraw("glDrawArraysInstanced", first, count,
                                   instance_count);
        static std::atomic<uint32_t> calls{0};
        const uint32_t index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (index <= 32) {
            LOGI("GL telemetry: glDrawArraysInstanced #%u mode=0x%04X "
                 "first=%d count=%d instances=%d",
                 index, mode, first, count, instance_count);
        } else if (index == 33) {
            LOGI("GL telemetry: suppressing further glDrawArraysInstanced calls "
                 "after 32");
        }
        const bool present_audit = PresentAuditEnabled() && index == 8;
        const bool audit = (DrawAuditEnabled() && index <= 3) || present_audit;
        if (audit) {
            GLenum pending_error = GL_NO_ERROR;
            while ((pending_error = glGetError()) != GL_NO_ERROR) {
                LOGE("Draw audit #%u pending GL error=0x%04X", index,
                     pending_error);
            }
            LogDrawArraysInstancedState(index, first, count);
            GLenum audit_error = GL_NO_ERROR;
            while ((audit_error = glGetError()) != GL_NO_ERROR) {
                LOGE("Draw audit #%u state-query GL error=0x%04X", index,
                     audit_error);
            }
        }
        GLuint samples_query = 0;
        if (present_audit) {
            GLint current_query = 0;
            glGetQueryiv(GL_ANY_SAMPLES_PASSED, GL_CURRENT_QUERY,
                         &current_query);
            if (current_query == 0) {
                glGenQueries(1, &samples_query);
                glBeginQuery(GL_ANY_SAMPLES_PASSED, samples_query);
            }
        }
        glDrawArraysInstanced(mode, first, count, instance_count);
        // Bl2FogAuditCompleted();  // Pixel readback diagnosis paused.
        if (SceneAuditEnabled()) SceneAuditDrawCompleted("glDrawArraysInstanced");
        if (samples_query != 0) {
            glEndQuery(GL_ANY_SAMPLES_PASSED);
            GLuint any_samples = 0;
            glGetQueryObjectuiv(samples_query, GL_QUERY_RESULT, &any_samples);
            glDeleteQueries(1, &samples_query);
            LOGI("Present audit #%u GL_ANY_SAMPLES_PASSED=%u", index,
                 any_samples);
        }
        if (audit) {
            const GLenum draw_error = glGetError();
            LOGI("Draw audit #%u glDrawArraysInstanced error=0x%04X", index,
                 draw_error);
        }
    });
    manager.RegisterThunkIfAbsent("glDrawElementsInstanced", [](Dynarmic::A32::Jit& jit,
                                                           DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum mode = static_cast<GLenum>(args.U32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const GLenum type = static_cast<GLenum>(args.U32());
        const uint32_t indices_address = args.Ptr();
        const GLsizei instance_count = static_cast<GLsizei>(args.I32());
        SyncGuestMappedBuffersForGpuRead(host);
        if (!RefreshLegacyProgramStateForDraw()) return;
        SceneAuditLogFinalInvocation("glDrawElementsInstanced", 0, count,
                                      type, indices_address, 0,
                                      instance_count);

        const void* indices = nullptr;
        if (GetBoundGlBuffer(GL_ELEMENT_ARRAY_BUFFER) != 0) {
            indices = reinterpret_cast<const void*>(
                static_cast<uintptr_t>(indices_address));
        } else if (count > 0 && instance_count > 0) {
            size_t element_size = 0;
            switch (type) {
                case GL_UNSIGNED_BYTE: element_size = sizeof(GLubyte); break;
                case GL_UNSIGNED_SHORT: element_size = sizeof(GLushort); break;
                case GL_UNSIGNED_INT: element_size = sizeof(GLuint); break;
                default:
                    // Preserve the driver's GL_INVALID_ENUM path without exposing an
                    // unchecked guest pointer to the host.
                    glDrawElementsInstanced(
                        mode, count, type, nullptr, instance_count);
                    return;
            }
            size_t bytes = 0;
            if (!CheckedArrayBytes(static_cast<size_t>(count), 1, element_size, &bytes) ||
                !GuestRangeValid(host.GetGuestMemorySize(), indices_address, bytes)) {
                LOGE("glDrawElementsInstanced rejected guest indices: address=0x%08X count=%d type=0x%X",
                     indices_address, static_cast<int>(count), type);
                return;
            }
            indices = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                indices_address, bytes);
        }
        SceneAuditLogFinalIndexedDraw("glDrawElementsInstanced", count,
                                      type, indices, 0, instance_count);
        glDrawElementsInstanced(mode, count, type, indices, instance_count);
        // Bl2FogAuditCompleted();  // Pixel readback diagnosis paused.
        if (SceneAuditEnabled()) SceneAuditDrawCompleted("glDrawElementsInstanced");
    });
    auto make_draw_elements_base_vertex = [](bool prefer_ext) {
      return [prefer_ext](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::Draw);
        Arm32CallArgs args(jit, host);
        const GLenum mode = static_cast<GLenum>(args.U32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const GLenum type = static_cast<GLenum>(args.U32());
        const uint32_t indices_address = args.Ptr();
        const GLint base_vertex = static_cast<GLint>(args.I32());
        const bool profile = HornPerfEnabled();
        const uint64_t profile_start = profile ? HornPerfNowNs() : 0;
        SyncGuestMappedBuffersForGpuRead(host);
        const uint64_t profile_sync = profile ? HornPerfNowNs() : 0;
        if (!RefreshLegacyProgramStateForDraw()) return;
        const uint64_t profile_legacy = profile ? HornPerfNowNs() : 0;
        SceneAuditVertexAlignment();
        SceneAuditLogFinalInvocation("glDrawElementsBaseVertex", 0, count,
                                      type, indices_address, base_vertex, 1);

        const void* indices = nullptr;
        if (!ResolveDrawElementIndices(host, count, type, indices_address,
                                       &indices, "glDrawElementsBaseVertex")) {
            return;
        }
        SceneAuditLogFinalIndexedDraw("glDrawElementsBaseVertex", count,
                                      type, indices, base_vertex, 1);
        const auto function = ResolveDrawElementsBaseVertexProc(prefer_ext);
        static std::atomic<uint32_t> calls{0};
        const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed);
        if (call < 8) {
            LOGI("BaseVertex draw #%u mode=0x%X count=%d type=0x%X indices=0x%08X base=%d native=%p",
                 call + 1, mode, count, type, indices_address, base_vertex,
                 reinterpret_cast<void*>(function));
        }
        if (DrawAuditEnabled() && call < 3) {
            LogDrawArraysInstancedState(call + 1, 0, count);
            LOGI("BaseVertex audit #%u before_error=0x%X", call + 1, glGetError());
        }
        // Rejected seed239 experiment; keep the normal draw path while paused.
        // const bool stable_post_draw = DrawStablePostprocessQuad(
        //     mode, count, type, indices, base_vertex);
        const bool stable_post_draw = false;
        const bool scratch_draw = !stable_post_draw &&
            DrawGfxTextWithAlignedVertices(
                mode, count, type, indices, base_vertex, function);
        if (stable_post_draw) {
            // The bounded fullscreen quad was submitted from owned GL storage.
        } else if (scratch_draw) {
            // The diagnostic helper submitted the draw and restored GL state.
        } else if (function) {
            function(mode, count, type, indices, base_vertex);
        } else if (base_vertex == 0) {
            glDrawElements(mode, count, type, indices);
        } else {
            LOGE("Missing native glDrawElementsBaseVertex entry point");
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
        // Bl2FogAuditCompleted();  // Pixel readback diagnosis paused.
        if (SceneAuditEnabled()) SceneAuditDrawCompleted("glDrawElementsBaseVertex");
        if (DrawAuditEnabled() && call < 3) {
            LOGI("BaseVertex audit #%u after_error=0x%X", call + 1, glGetError());
        }
        if (profile) {
            static thread_local std::array<uint64_t, 4> totals{};
            totals[0] += profile_sync - profile_start;
            totals[1] += profile_legacy - profile_sync;
            totals[2] += HornPerfNowNs() - profile_legacy;
            if (++totals[3] == 512) {
                LOGI("BaseVertex profile draws=512 mapped_sync=%.2fms legacy_state=%.2fms native_draw=%.2fms",
                     totals[0] / 1e6, totals[1] / 1e6, totals[2] / 1e6);
                totals = {};
            }
        }
      };
    };
    manager.RegisterThunkIfAbsent("glDrawElementsBaseVertexOES",
                          make_draw_elements_base_vertex(false));
    manager.RegisterThunkIfAbsent("glDrawElementsBaseVertexEXT",
                          make_draw_elements_base_vertex(true));
    // The desktop UE3 renderer resolves the unsuffixed core names through
    // dlsym. GLES 3.2/OES/EXT have the same argument/offset semantics here.
    manager.RegisterThunkIfAbsent("glDrawElementsBaseVertex",
                          make_draw_elements_base_vertex(false));

    auto make_draw_elements_instanced_base_vertex = [](bool prefer_ext) {
      return [prefer_ext](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::Draw);
        Arm32CallArgs args(jit, host);
        const GLenum mode = static_cast<GLenum>(args.U32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const GLenum type = static_cast<GLenum>(args.U32());
        const uint32_t indices_address = args.Ptr();
        const GLsizei instance_count = static_cast<GLsizei>(args.I32());
        const GLint base_vertex = static_cast<GLint>(args.I32());
        SyncGuestMappedBuffersForGpuRead(host);
        if (!RefreshLegacyProgramStateForDraw()) return;
        SceneAuditLogFinalInvocation("glDrawElementsInstancedBaseVertex", 0,
                                      count, type, indices_address,
                                      base_vertex, instance_count);

        const void* indices = nullptr;
        if (instance_count > 0 &&
            !ResolveDrawElementIndices(host, count, type, indices_address,
                                       &indices,
                                       "glDrawElementsInstancedBaseVertex")) {
            return;
        }
        SceneAuditLogFinalIndexedDraw("glDrawElementsInstancedBaseVertex",
                                      count, type, indices, base_vertex,
                                      instance_count);
        const auto function = ResolveDrawElementsInstancedBaseVertexProc(prefer_ext);
        if (function) {
            function(mode, count, type, indices, instance_count, base_vertex);
        } else if (base_vertex == 0) {
            glDrawElementsInstanced(mode, count, type, indices, instance_count);
        } else {
            LOGE("Missing native glDrawElementsInstancedBaseVertex entry point");
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
        // Bl2FogAuditCompleted();  // Pixel readback diagnosis paused.
        if (SceneAuditEnabled()) SceneAuditDrawCompleted("glDrawElementsInstancedBaseVertex");
      };
    };
    manager.RegisterThunkIfAbsent("glDrawElementsInstancedBaseVertexOES",
                          make_draw_elements_instanced_base_vertex(false));
    manager.RegisterThunkIfAbsent("glDrawElementsInstancedBaseVertexEXT",
                          make_draw_elements_instanced_base_vertex(true));
    manager.RegisterThunkIfAbsent("glDrawElementsInstancedBaseVertex",
                          make_draw_elements_instanced_base_vertex(false));
    const auto copy_image_sub_data = [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint source = args.U32();
        const GLenum source_target = args.U32();
        const GLint source_level = args.I32();
        const GLint source_x = args.I32();
        const GLint source_y = args.I32();
        const GLint source_z = args.I32();
        const GLuint destination = args.U32();
        const GLenum destination_target = args.U32();
        const GLint destination_level = args.I32();
        const GLint destination_x = args.I32();
        const GLint destination_y = args.I32();
        const GLint destination_z = args.I32();
        const GLsizei width = args.I32();
        const GLsizei height = args.I32();
        const GLsizei depth = args.I32();
        static const auto function = [] {
            for (const char* name : {"glCopyImageSubData", "glCopyImageSubDataEXT",
                                     "glCopyImageSubDataOES"}) {
                const auto address = eglGetProcAddress(name);
                if (address) return reinterpret_cast<PFNGLCOPYIMAGESUBDATAPROC>(address);
            }
            return static_cast<PFNGLCOPYIMAGESUBDATAPROC>(nullptr);
        }();
        if (!function) {
            LOGE("Missing native GLES3.2 glCopyImageSubData entry point");
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        static std::atomic<uint32_t> calls{0};
        const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed);
        if (call < 16) {
            LOGI("CopyImage #%u src=%u/0x%X mip=%d xyz=%d,%d,%d "
                 "dst=%u/0x%X mip=%d xyz=%d,%d,%d size=%dx%dx%d",
                 call + 1, source, source_target, source_level,
                 source_x, source_y, source_z, destination, destination_target,
                 destination_level, destination_x, destination_y, destination_z,
                 width, height, depth);
        }
        function(source, source_target, source_level, source_x, source_y, source_z,
                 destination, destination_target, destination_level,
                 destination_x, destination_y, destination_z, width, height, depth);
    };
    manager.RegisterThunkIfAbsent("glCopyImageSubData", copy_image_sub_data);
    manager.RegisterThunkIfAbsent("glCopyImageSubDataEXT", copy_image_sub_data);
    manager.RegisterThunkIfAbsent("glCopyImageSubDataOES", copy_image_sub_data);
    manager.RegisterThunkIfAbsent("glCopyImageSubDataNV", copy_image_sub_data);
    manager.RegisterThunkIfAbsent("glCopyBufferSubData", [](Dynarmic::A32::Jit& jit,
                                                      DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum read_target = static_cast<GLenum>(args.U32());
        const GLenum write_target = static_cast<GLenum>(args.U32());
        const GLintptr read_offset = static_cast<GLintptr>(args.I32());
        const GLintptr write_offset = static_cast<GLintptr>(args.I32());
        const GLsizeiptr size = static_cast<GLsizeiptr>(args.I32());
        glCopyBufferSubData(read_target, write_target, read_offset, write_offset, size);
    });

    manager.RegisterThunkIfAbsent("glTexImage3D", [](Dynarmic::A32::Jit& jit,
                                              DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = static_cast<GLint>(args.I32());
        const GLint internal_format = static_cast<GLint>(args.I32());
        const GLsizei width = static_cast<GLsizei>(args.I32());
        const GLsizei height = static_cast<GLsizei>(args.I32());
        const GLsizei depth = static_cast<GLsizei>(args.I32());
        const GLint border = static_cast<GLint>(args.I32());
        const GLenum format = static_cast<GLenum>(args.U32());
        const GLenum type = static_cast<GLenum>(args.U32());
        const uint32_t pixels_address = args.Ptr();

        GLint pbo = 0;
        if (HasGlES3()) glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
        const void* pixels = nullptr;
        if ((pbo != 0 || pixels_address != 0) &&
            !ResolveGlPixels(host, width, height, depth, format, type,
                             pixels_address, &pixels, true)) {
            return;
        }
        glTexImage3D(target, level, internal_format, width, height, depth,
                     border, format, type, pixels);
    });
    manager.RegisterThunkIfAbsent("glCompressedTexSubImage3D",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            Arm32CallArgs args(jit, host);
            const GLenum target = static_cast<GLenum>(args.U32());
            const GLint level = static_cast<GLint>(args.I32());
            const GLint x_offset = static_cast<GLint>(args.I32());
            const GLint y_offset = static_cast<GLint>(args.I32());
            const GLint z_offset = static_cast<GLint>(args.I32());
            const GLsizei width = static_cast<GLsizei>(args.I32());
            const GLsizei height = static_cast<GLsizei>(args.I32());
            const GLsizei depth = static_cast<GLsizei>(args.I32());
            const GLenum format = static_cast<GLenum>(args.U32());
            const GLsizei image_size = static_cast<GLsizei>(args.I32());
            const uint32_t data_address = args.Ptr();

            GLint pbo = 0;
            if (HasGlES3()) glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
            const void* data = nullptr;
            if (pbo != 0) {
                data = reinterpret_cast<const void*>(static_cast<uintptr_t>(data_address));
            } else {
                bool valid = false;
                data = CountedGuestBytes(
                    host, data_address, image_size, "glCompressedTexSubImage3D", &valid);
                if (!valid) return;
            }
            glCompressedTexSubImage3D(target, level, x_offset, y_offset, z_offset,
                                      width, height, depth, format, image_size, data);
        });

    manager.RegisterThunkIfAbsent("glGetIntegeri_v", [](Dynarmic::A32::Jit& jit,
                                                  DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLuint index = static_cast<GLuint>(args.U32());
        GLint* data = GuestOutputArray<GLint>(
            host, args.Ptr(), 1, 1, "glGetIntegeri_v");
        if (data) glGetIntegeri_v(target, index, data);
    });
    manager.RegisterThunkIfAbsent("glGetInternalformativ", [](Dynarmic::A32::Jit& jit,
                                                         DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLenum internal_format = static_cast<GLenum>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        const GLsizei buffer_size = static_cast<GLsizei>(args.I32());
        const uint32_t params_address = args.Ptr();
        GLint* params = buffer_size > 0 ? GuestOutputArray<GLint>(
            host, params_address, static_cast<size_t>(buffer_size), 1,
            "glGetInternalformativ") : nullptr;
        if (buffer_size <= 0 || params) {
            glGetInternalformativ(target, internal_format, pname, buffer_size, params);
        }
    });

    manager.RegisterThunkIfAbsent("glGetProgramBinary", [](Dynarmic::A32::Jit& jit,
                                                      DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLsizei buffer_size = static_cast<GLsizei>(args.I32());
        const uint32_t length_address = args.Ptr();
        const uint32_t binary_format_address = args.Ptr();
        const uint32_t binary_address = args.Ptr();

        bool valid = false;
        GLsizei* length = OptionalGuestOutput<GLsizei>(
            host, length_address, 1, "glGetProgramBinary", &valid);
        if (!valid) return;
        GLenum* binary_format = GuestOutputArray<GLenum>(
            host, binary_format_address, 1, 1, "glGetProgramBinary");
        if (!binary_format) return;
        void* binary = buffer_size > 0 ? GuestOutputArray<uint8_t>(
            host, binary_address, static_cast<size_t>(buffer_size), 1,
            "glGetProgramBinary") : nullptr;
        if (buffer_size > 0 && !binary) return;
        glGetProgramBinary(program, buffer_size, length, binary_format, binary);
    });
    manager.RegisterThunkIfAbsent("glProgramBinary", [](Dynarmic::A32::Jit& jit,
                                                   DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLenum binary_format = static_cast<GLenum>(args.U32());
        const uint32_t binary_address = args.Ptr();
        const GLsizei length = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const void* binary = CountedGuestBytes(
            host, binary_address, length, "glProgramBinary", &valid);
        if (valid) glProgramBinary(program, binary_format, binary, length);
    });

    manager.RegisterThunkIfAbsent("glGenSamplers", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t samplers_address = args.Ptr();
        GLuint* samplers = count > 0 ? GuestOutputArray<GLuint>(
            host, samplers_address, static_cast<size_t>(count), 1,
            "glGenSamplers") : nullptr;
        if (count <= 0 || samplers) glGenSamplers(count, samplers);
    });
    manager.RegisterThunkIfAbsent("glDeleteSamplers", [](Dynarmic::A32::Jit& jit,
                                                   DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLsizei count = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const GLuint* samplers = CountedGuestValues<GLuint>(
            host, args.Ptr(), count, 1, "glDeleteSamplers", &valid);
        if (valid) {
            glDeleteSamplers(count, samplers);
            InvalidateLegacyTextureCacheAcrossContexts();
        }
    });
    manager.RegisterThunkIfAbsent("glSamplerParameteri", [](Dynarmic::A32::Jit& jit,
                                                       DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint sampler = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        const GLint param = static_cast<GLint>(args.I32());
        glSamplerParameteri(sampler, pname, param);
    });

    manager.RegisterThunkIfAbsent("glGetUniformIndices", [](Dynarmic::A32::Jit& jit,
                                                       DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t names_address = args.Ptr();
        const uint32_t indices_address = args.Ptr();
        std::vector<const GLchar*> names;
        if (!GuestGlNameArray(host, names_address, count,
                              "glGetUniformIndices", &names)) {
            return;
        }
        GLuint* indices = count > 0 ? GuestOutputArray<GLuint>(
            host, indices_address, static_cast<size_t>(count), 1,
            "glGetUniformIndices") : nullptr;
        if (count <= 0 || indices) {
            glGetUniformIndices(program, count, count > 0 ? names.data() : nullptr, indices);
        }
    });
    manager.RegisterThunkIfAbsent("glGetActiveUniformsiv", [](Dynarmic::A32::Jit& jit,
                                                         DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        const uint32_t indices_address = args.Ptr();
        const GLenum pname = static_cast<GLenum>(args.U32());
        const uint32_t params_address = args.Ptr();
        bool valid = false;
        const GLuint* indices = CountedGuestValues<GLuint>(
            host, indices_address, count, 1, "glGetActiveUniformsiv", &valid);
        if (!valid) return;
        GLint* params = count > 0 ? GuestOutputArray<GLint>(
            host, params_address, static_cast<size_t>(count), 1,
            "glGetActiveUniformsiv") : nullptr;
        if (count <= 0 || params) glGetActiveUniformsiv(program, count, indices, pname, params);
    });
    manager.RegisterThunkIfAbsent("glGetUniformBlockIndex", [](Dynarmic::A32::Jit& jit,
                                                          DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const char* name = GuestGlName(host, args.Ptr());
        jit.Regs()[0] = name ? glGetUniformBlockIndex(program, name) : GL_INVALID_INDEX;
    });
    manager.RegisterThunkIfAbsent("glGetActiveUniformBlockiv", [](Dynarmic::A32::Jit& jit,
                                                             DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = static_cast<GLuint>(args.U32());
        const GLuint block_index = static_cast<GLuint>(args.U32());
        const GLenum pname = static_cast<GLenum>(args.U32());
        const uint32_t params_address = args.Ptr();
        size_t value_count = 1;
        if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) {
            GLint active_uniforms = 0;
            glGetActiveUniformBlockiv(
                program, block_index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS,
                &active_uniforms);
            if (active_uniforms > 0) value_count = static_cast<size_t>(active_uniforms);
        }
        GLint* params = GuestOutputArray<GLint>(
            host, params_address, value_count, 1, "glGetActiveUniformBlockiv");
        if (params) glGetActiveUniformBlockiv(program, block_index, pname, params);
    });
    manager.RegisterThunkIfAbsent("glGetActiveUniformBlockName",
        [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            Arm32CallArgs args(jit, host);
            const GLuint program = static_cast<GLuint>(args.U32());
            const GLuint block_index = static_cast<GLuint>(args.U32());
            const GLsizei buffer_size = static_cast<GLsizei>(args.I32());
            const uint32_t length_address = args.Ptr();
            const uint32_t name_address = args.Ptr();
            bool valid = false;
            GLsizei* length = OptionalGuestOutput<GLsizei>(
                host, length_address, 1, "glGetActiveUniformBlockName", &valid);
            if (!valid) return;
            GLchar* name = buffer_size > 0 ? GuestOutputArray<GLchar>(
                host, name_address, static_cast<size_t>(buffer_size), 1,
                "glGetActiveUniformBlockName") : nullptr;
            if (buffer_size > 0 && !name) return;
            glGetActiveUniformBlockName(
                program, block_index, buffer_size, length, name);
        });

    manager.RegisterThunkIfAbsent("glUniform1uiv", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const GLuint* values = CountedGuestValues<GLuint>(
            host, args.Ptr(), count, 1, "glUniform1uiv", &valid);
        if (valid) glUniform1uiv(location, count, values);
    });
    manager.RegisterThunkIfAbsent("glUniform2uiv", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const GLuint* values = CountedGuestValues<GLuint>(
            host, args.Ptr(), count, 2, "glUniform2uiv", &valid);
        if (valid) glUniform2uiv(location, count, values);
    });
    manager.RegisterThunkIfAbsent("glUniform3uiv", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const GLuint* values = CountedGuestValues<GLuint>(
            host, args.Ptr(), count, 3, "glUniform3uiv", &valid);
        if (valid) glUniform3uiv(location, count, values);
    });
    manager.RegisterThunkIfAbsent("glUniform4uiv", [](Dynarmic::A32::Jit& jit,
                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLint location = static_cast<GLint>(args.I32());
        const GLsizei count = static_cast<GLsizei>(args.I32());
        bool valid = false;
        const GLuint* values = CountedGuestValues<GLuint>(
            host, args.Ptr(), count, 4, "glUniform4uiv", &valid);
        if (valid) glUniform4uiv(location, count, values);
    });
    manager.RegisterThunkIfAbsent("glVertexAttribIPointer", [](Dynarmic::A32::Jit& jit,
                                                                  DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLint size = static_cast<GLint>(args.I32());
        const GLenum type = static_cast<GLenum>(args.U32());
        const GLsizei stride = static_cast<GLsizei>(args.I32());
        const uint32_t pointer_address = args.Ptr();
        const void* pointer = nullptr;
        if (GetBoundGlBuffer(GL_ARRAY_BUFFER) != 0) {
            pointer = reinterpret_cast<const void*>(
                static_cast<uintptr_t>(pointer_address));
        } else if (pointer_address != 0) {
            if (!GuestRangeValid(host.GetGuestMemorySize(), pointer_address, 1)) {
                LOGE("glVertexAttribIPointer rejected guest pointer: address=0x%08X",
                     pointer_address);
                return;
            }
            pointer = GuestSpan(host.GetGuestMemoryBase(), host.GetGuestMemorySize(),
                                pointer_address, 1);
        }
        glVertexAttribIPointer(index, size, type, stride, pointer);
        InvalidateGlAttribCache();
    });
    manager.RegisterThunkIfAbsent("glVertexAttribDivisor", [](Dynarmic::A32::Jit& jit,
                                                                DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint index = static_cast<GLuint>(args.U32());
        const GLuint divisor = static_cast<GLuint>(args.U32());
        glVertexAttribDivisor(index, divisor);
    });

    // Unity selects GLES3 on this GPU; these are real driver calls, not success stubs.
    auto strings = std::make_shared<GlStringCache>();
    manager.RegisterThunkIfAbsent("glGetStringi", [strings](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum name = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint index = static_cast<GLuint>(jit.Regs()[1]);
        const GLubyte* value = glGetStringi(name, index);
        if (!value) {
            LOGE("GL telemetry: glGetStringi name=0x%04X index=%u returned null",
                 name, index);
            jit.Regs()[0] = 0;
            return;
        }
        const std::string text(reinterpret_cast<const char*>(value));
        if (index < 8 || (index & 63u) == 63u) {
            LOGI("GL telemetry: glGetStringi name=0x%04X index=%u value='%s'",
                 name, index, text.c_str());
        }
        std::lock_guard<std::mutex> lock(strings->mutex);
        auto found = strings->values.find(text);
        if (found == strings->values.end()) {
            const uint32_t address = host.GuestAlloc(text.size() + 1, 1);
            if (!address) { jit.Regs()[0] = 0; return; }
            memcpy(host.GetGuestMemoryBase() + address, text.c_str(), text.size() + 1);
            found = strings->values.emplace(text, address).first;
        }
        jit.Regs()[0] = found->second;
    });
    manager.RegisterThunkIfAbsent("glDepthRangef", [](Dynarmic::A32::Jit& j,DynarmicHost&) { glDepthRangef(ReadSF(j,0),ReadSF(j,1)); });
    manager.RegisterThunkIfAbsent("glBlendFuncSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glBlendFuncSeparate(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
    });
    manager.RegisterThunkIfAbsent("glBindAttribLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const char* name = GuestGlName(host, jit.Regs()[2]);
        if (name) glBindAttribLocation(jit.Regs()[0], jit.Regs()[1], name);
    });
    manager.RegisterThunkIfAbsent("glBufferSubData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLintptr offset = static_cast<int32_t>(jit.Regs()[1]);
        const GLsizeiptr size = static_cast<int32_t>(jit.Regs()[2]);
        bool valid = false;
        const void* data = CountedGuestBytes(host, jit.Regs()[3], size, "glBufferSubData", &valid);
        if (valid) glBufferSubData(jit.Regs()[0], offset, size, data);
    });
    manager.RegisterThunkIfAbsent("glMapBufferRange", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        std::lock_guard mapping_lock(s_mapping_mutex);
        const GLintptr offset = static_cast<int32_t>(jit.Regs()[1]);
        const GLsizeiptr length = static_cast<int32_t>(jit.Regs()[2]);
        const GLbitfield access = static_cast<GLbitfield>(jit.Regs()[3]);
        const GLuint buffer = GetBoundGlBuffer(target);
        if (offset < 0 || length <= 0 ||
            static_cast<uint64_t>(length) > host.GetGuestMemorySize()) {
            jit.Regs()[0] = 0;
            return;
        }
        // Write watch protects whole host pages. Sharing either edge page with
        // an unrelated guest allocation can make kernel writes (read/pread)
        // fail with EFAULT or a short read without entering our SIGSEGV handler.
        // Reserve complete, aligned pages even in the full-scan fallback so
        // mapping ownership never extends into another guest allocation.
        static const size_t mapping_page_size = [] {
            const long page = ::sysconf(_SC_PAGESIZE);
            return page > 0 && (page & (page - 1)) == 0
                ? static_cast<size_t>(page) : size_t{0};
        }();
        if (!mapping_page_size) {
            jit.Regs()[0] = 0;
            return;
        }
        const uint64_t reserved_length =
            (static_cast<uint64_t>(length) + mapping_page_size - 1) &
            ~static_cast<uint64_t>(mapping_page_size - 1);
        if (reserved_length > host.GetGuestMemorySize()) {
            jit.Regs()[0] = 0;
            return;
        }
        void* host_pointer = glMapBufferRange(target, offset, length, access);
        if (!host_pointer) {
            jit.Regs()[0] = 0;
            return;
        }
        const uint32_t guest_address = host.GuestAlloc(
            static_cast<size_t>(reserved_length), mapping_page_size);
        if (!guest_address) {
            glUnmapBuffer(target);
            jit.Regs()[0] = 0;
            return;
        }
        if (access & GL_MAP_READ_BIT) {
            memcpy(host.GetGuestMemoryBase() + guest_address, host_pointer,
                   static_cast<size_t>(length));
        } else if (!CopyGlBufferShadow(
                       buffer, static_cast<size_t>(offset),
                       static_cast<size_t>(length),
                       host.GetGuestMemoryBase() + guest_address)) {
            std::memset(host.GetGuestMemoryBase() + guest_address, 0,
                        static_cast<size_t>(length));
        }
        // A shared context or native host code may have deleted an old mapping.
        // A successful new native map owns this address exclusively.
        if(auto old=s_guest_mapped_buffers.find(host_pointer);old!=s_guest_mapped_buffers.end()) {
            if (old->second.tracked) {
                MappedDirtyTracker().Untrack(old->second.guest_address,
                                             old->second.length);
            }
            host.GuestFree(old->second.guest_address);s_guest_mapped_buffers.erase(old);
        }
        s_guest_mapped_buffers.emplace(host_pointer,
            GuestMappedBuffer{host_pointer, guest_address, static_cast<size_t>(length), access,
                              eglGetCurrentContext(), buffer, target, offset});
        static std::atomic<uint32_t> map_logs{0};
        const uint32_t map_index =
            map_logs.fetch_add(1, std::memory_order_relaxed);
        if (map_index < 32) {
            LOGI("GL telemetry: glMapBufferRange #%u target=0x%04X buffer=%u "
                 "offset=%lld length=%lld access=0x%04X guest=0x%08X "
                 "reserved=%llu page=%zu",
                 map_index + 1, target, buffer,
                 static_cast<long long>(offset),
                 static_cast<long long>(length), access, guest_address,
                 static_cast<unsigned long long>(reserved_length), mapping_page_size);
        }
        jit.Regs()[0] = guest_address;
    });
    manager.RegisterThunkIfAbsent("glUnmapBuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        std::lock_guard mapping_lock(s_mapping_mutex);
        void* native = nullptr; glGetBufferPointerv(target, GL_BUFFER_MAP_POINTER, &native);
        auto it = s_guest_mapped_buffers.find(native);
        if (it == s_guest_mapped_buffers.end()) {
            jit.Regs()[0] = static_cast<uint32_t>(glUnmapBuffer(target));
            return;
        }
        const GuestMappedBuffer mapping = it->second;
        if (mapping.tracked) {
            MappedDirtyTracker().Untrack(mapping.guest_address, mapping.length);
        }
        if ((mapping.access & GL_MAP_WRITE_BIT) && !(mapping.access & GL_MAP_FLUSH_EXPLICIT_BIT) &&
            GuestRangeValid(host.GetGuestMemorySize(), mapping.guest_address, mapping.length)) {
            const void* guest_pointer =
                host.GetGuestMemoryBase() + mapping.guest_address;
            memcpy(mapping.host_pointer, guest_pointer, mapping.length);
            UpdateGlBufferShadow(mapping.buffer,
                                 static_cast<size_t>(mapping.offset),
                                 mapping.length, guest_pointer);
        }
        const GLboolean result = glUnmapBuffer(target);
        host.GuestFree(mapping.guest_address);
        s_guest_mapped_buffers.erase(it);
        jit.Regs()[0] = static_cast<uint32_t>(result);
    });
    manager.RegisterThunkIfAbsent("glGetBufferPointerv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if(!GuestRangeValid(host.GetGuestMemorySize(),jit.Regs()[2],4))return;
        std::lock_guard mapping_lock(s_mapping_mutex);
        void* native=nullptr;glGetBufferPointerv(jit.Regs()[0],jit.Regs()[1],&native);
        const auto found=s_guest_mapped_buffers.find(native);
        host.MemoryWrite32(jit.Regs()[2],found==s_guest_mapped_buffers.end()?0:found->second.guest_address);
    });
    manager.RegisterThunkIfAbsent("glFlushMappedBufferRange", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        std::lock_guard mapping_lock(s_mapping_mutex);
        const GLintptr offset = static_cast<int32_t>(jit.Regs()[1]);
        const GLsizeiptr length = static_cast<int32_t>(jit.Regs()[2]);
        void* native = nullptr; glGetBufferPointerv(target, GL_BUFFER_MAP_POINTER, &native);
        auto it = s_guest_mapped_buffers.find(native);
        if (it != s_guest_mapped_buffers.end() &&
            (it->second.access & GL_MAP_WRITE_BIT) && offset >= 0 && length >= 0) {
            const GuestMappedBuffer& mapping = it->second;
            const uint64_t mapping_start = static_cast<uint64_t>(mapping.offset);
            const uint64_t flush_start = static_cast<uint64_t>(offset);
            const uint64_t flush_end = flush_start + static_cast<uint64_t>(length);
            const uint64_t mapping_end = mapping_start + mapping.length;
            if (flush_start >= mapping_start && flush_end <= mapping_end) {
                const size_t relative = static_cast<size_t>(flush_start - mapping_start);
                const uint64_t guest_end =
                    static_cast<uint64_t>(mapping.guest_address) + relative +
                    static_cast<uint64_t>(length);
                if (guest_end <= host.GetGuestMemorySize() &&
                GuestRangeValid(host.GetGuestMemorySize(),
                                mapping.guest_address + static_cast<uint32_t>(relative),
                                static_cast<size_t>(length))) {
                    const void* guest_pointer =
                        host.GetGuestMemoryBase() + mapping.guest_address + relative;
                    memcpy(reinterpret_cast<uint8_t*>(mapping.host_pointer) + relative,
                           guest_pointer, static_cast<size_t>(length));
                    UpdateGlBufferShadow(mapping.buffer,
                                         static_cast<size_t>(offset),
                                         static_cast<size_t>(length), guest_pointer);
                }
            }
        }
        glFlushMappedBufferRange(target, offset, length);
    });
    manager.RegisterThunkIfAbsent("glBindBufferBase", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint buffer = jit.Regs()[2];
        glBindBufferBase(target, jit.Regs()[1], buffer);
        TrackGlBufferBinding(target, buffer);
    });
    manager.RegisterThunkIfAbsent("glBindSampler", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glBindSampler(jit.Regs()[0], jit.Regs()[1]);
        InvalidateLegacyTextureCache();
    });
    manager.RegisterThunkIfAbsent("glGenVertexArrays", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<int32_t>(jit.Regs()[0]);
        if (count <= 0) { glGenVertexArrays(count, nullptr); return; }
        auto* arrays = GuestOutputArray<GLuint>(host, jit.Regs()[1], count, 1, "glGenVertexArrays");
        if (arrays) glGenVertexArrays(count, arrays);
    });
    manager.RegisterThunkIfAbsent("glDeleteVertexArrays", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<int32_t>(jit.Regs()[0]);
        bool valid = false;
        const auto* arrays = CountedGuestValues<GLuint>(host, jit.Regs()[1], count, 1, "glDeleteVertexArrays", &valid);
        if (valid) { glDeleteVertexArrays(count, arrays); RefreshGlBindings(); }
    });
    manager.RegisterThunkIfAbsent("glBindVertexArray", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glBindVertexArray(jit.Regs()[0]);
        RefreshGlBindings();
    });
    manager.RegisterThunkIfAbsent("glIsVertexArray", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = glIsVertexArray(jit.Regs()[0]);
    });
    manager.RegisterThunkIfAbsent("glDrawBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<int32_t>(jit.Regs()[0]);
        bool valid = false;
        const auto* buffers = CountedGuestValues<GLenum>(host, jit.Regs()[1], count, 1, "glDrawBuffers", &valid);
        if (valid) glDrawBuffers(count, buffers);
    });
    manager.RegisterThunkIfAbsent("glTexStorage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glTexStorage2D(jit.Regs()[0], static_cast<int32_t>(jit.Regs()[1]), jit.Regs()[2],
                       static_cast<int32_t>(jit.Regs()[3]), static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13])));
    });
    manager.RegisterThunkIfAbsent("glTexStorage3D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glTexStorage3D(jit.Regs()[0], static_cast<int32_t>(jit.Regs()[1]), jit.Regs()[2],
                       static_cast<int32_t>(jit.Regs()[3]), static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13])),
                       static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13] + 4)));
    });
    manager.RegisterThunkIfAbsent("glTexSubImage3D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = args.U32();
        const GLint level = args.I32(), x = args.I32(), y = args.I32(), z = args.I32();
        const GLsizei width = args.I32(), height = args.I32(), depth = args.I32();
        const GLenum format = args.U32(), type = args.U32();
        const uint32_t address = args.Ptr();
        const void* pixels = nullptr;
        if (ResolveGlPixels(host, width, height, depth, format, type, address, &pixels, true))
            glTexSubImage3D(target, level, x, y, z, width, height, depth, format, type, pixels);
    });
    manager.RegisterThunkIfAbsent("glFenceSync", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const GLenum condition = static_cast<GLenum>(jit.Regs()[0]);
        const GLbitfield flags = static_cast<GLbitfield>(jit.Regs()[1]);
        jit.Regs()[0] = 0;
        if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
            LOGE("glFenceSync rejected without current EGL context");
            return;
        }
        std::lock_guard<std::mutex> lock(g_guest_gl_sync_mutex);
        GuestGlSyncSlot* slot = FindEmptyGuestGlSyncSlotLocked();
        const uint32_t handle = slot ? AllocateGuestGlSyncHandleLocked() : 0;
        if (!slot || !handle) {
            LOGE("glFenceSync registry exhausted count=%zu", g_guest_gl_sync_count);
            return;
        }
        const GLsync host_sync = glFenceSync(condition, flags);
        if (!host_sync) {
            LOGE("glFenceSync host call returned null condition=0x%X flags=0x%X", condition, flags);
            return;
        }
        slot->guest_handle = handle;
        slot->host_sync = host_sync;
        ++g_guest_gl_sync_count;
        jit.Regs()[0] = handle;
    });
    manager.RegisterThunkIfAbsent("glClientWaitSync", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const uint32_t handle = args.U32();
        const GLbitfield flags = static_cast<GLbitfield>(args.U32());
        const GLuint64 timeout = static_cast<GLuint64>(args.U64());
        jit.Regs()[0] = GL_WAIT_FAILED;
        if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
            LOGE("glClientWaitSync rejected without current EGL context handle=0x%08X", handle);
            return;
        }
        std::lock_guard<std::mutex> lock(g_guest_gl_sync_mutex);
        GuestGlSyncSlot* slot = FindGuestGlSyncLocked(handle);
        if (!slot) {
            LOGE("glClientWaitSync rejected stale handle=0x%08X", handle);
            return;
        }
        const GLenum result = glClientWaitSync(slot->host_sync, flags, timeout);
        jit.Regs()[0] = result;
    });
    manager.RegisterThunkIfAbsent("glDeleteSync", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const uint32_t handle = jit.Regs()[0];
        std::lock_guard<std::mutex> lock(g_guest_gl_sync_mutex);
        GuestGlSyncSlot* slot = FindGuestGlSyncLocked(handle);
        if (!slot) {
            LOGE("glDeleteSync rejected stale handle=0x%08X", handle);
            return;
        }
        if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
            LOGE("glDeleteSync retained without current EGL context handle=0x%08X", handle);
            return;
        }
        glDeleteSync(slot->host_sync);
        *slot = {};
        --g_guest_gl_sync_count;
    });
    manager.RegisterThunkIfAbsent("glInvalidateFramebuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<int32_t>(jit.Regs()[1]);
        bool valid = false;
        const auto* attachments = CountedGuestValues<GLenum>(
            host, jit.Regs()[2], count, 1, "glInvalidateFramebuffer", &valid);
        if (valid) glInvalidateFramebuffer(jit.Regs()[0], count, attachments);
    });
    manager.RegisterThunkIfAbsent("glDiscardFramebufferEXT", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static auto function = reinterpret_cast<PFNGLDISCARDFRAMEBUFFEREXTPROC>(
            eglGetProcAddress("glDiscardFramebufferEXT"));
        const GLsizei count = static_cast<int32_t>(jit.Regs()[1]);
        bool valid = false;
        const auto* attachments = CountedGuestValues<GLenum>(
            host, jit.Regs()[2], count, 1, "glDiscardFramebufferEXT", &valid);
        if (!function) {
            LOGE("Missing native glDiscardFramebufferEXT");
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        if (valid) function(jit.Regs()[0], count, attachments);
    });
    manager.RegisterThunkIfAbsent("glRenderbufferStorageMultisampleEXT", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static auto function = reinterpret_cast<PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC>(eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
        if (!function) {
            LOGE("Missing native glRenderbufferStorageMultisampleEXT");
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        function(jit.Regs()[0], static_cast<int32_t>(jit.Regs()[1]), jit.Regs()[2],
                 static_cast<int32_t>(jit.Regs()[3]), static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13])));
    });
    manager.RegisterThunkIfAbsent("glFramebufferTexture2DMultisampleEXT", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static auto function = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC>(eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
        if (!function) {
            LOGE("Missing native glFramebufferTexture2DMultisampleEXT");
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        function(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3],
                 static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13])),
                 static_cast<int32_t>(host.MemoryRead32(jit.Regs()[13] + 4)));
    });
    manager.RegisterThunkIfAbsent("glProgramParameteri", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glProgramParameteri(jit.Regs()[0], jit.Regs()[1], static_cast<int32_t>(jit.Regs()[2]));
    });
    manager.RegisterThunkIfAbsent("glGetProgramInterfaceiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto* value = GuestOutputArray<GLint>(host, jit.Regs()[3], 1, 1, "glGetProgramInterfaceiv");
        if (value) glGetProgramInterfaceiv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], value);
    });
    manager.RegisterThunkIfAbsent("glGetProgramResourceiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLuint program = args.U32();
        const GLenum interface = args.U32();
        const GLuint index = args.U32();
        const GLsizei prop_count = args.I32();
        const uint32_t props_address = args.Ptr();
        const GLsizei capacity = args.I32();
        const uint32_t length_address = args.Ptr(), params_address = args.Ptr();
        bool valid = false;
        const auto* props = CountedGuestValues<GLenum>(host, props_address, prop_count, 1, "glGetProgramResourceiv", &valid);
        if (!valid) return;
        auto* length = OptionalGuestOutput<GLsizei>(host, length_address, 1, "glGetProgramResourceiv", &valid);
        if (!valid) return;
        auto* params = capacity > 0 ? GuestOutputArray<GLint>(host, params_address, capacity, 1, "glGetProgramResourceiv") : nullptr;
        if (capacity > 0 && !params) return;
        glGetProgramResourceiv(program, interface, index, prop_count, props, capacity, length, params);
    });
    manager.RegisterThunkIfAbsent("glGetProgramResourceName", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei capacity = static_cast<int32_t>(jit.Regs()[3]);
        bool valid = false;
        auto* length = OptionalGuestOutput<GLsizei>(host, host.MemoryRead32(jit.Regs()[13]), 1, "glGetProgramResourceName", &valid);
        if (!valid) return;
        auto* name = capacity > 0 ? GuestOutputArray<GLchar>(host, host.MemoryRead32(jit.Regs()[13] + 4), capacity, 1, "glGetProgramResourceName") : nullptr;
        if (capacity > 0 && !name) return;
        glGetProgramResourceName(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], capacity, length, name);
    });
    manager.RegisterThunkIfAbsent("glUniformBlockBinding", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        glUniformBlockBinding(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2]);
    });
    manager.RegisterThunkIfAbsent("glLabelObjectEXT", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static auto function = reinterpret_cast<PFNGLLABELOBJECTEXTPROC>(eglGetProcAddress("glLabelObjectEXT"));
        const GLsizei length = static_cast<int32_t>(jit.Regs()[2]);
        const char* label = length > 0 ? GuestInputArray<char>(host, jit.Regs()[3], length, 1, "glLabelObjectEXT") : GuestGlName(host, jit.Regs()[3]);
        if (function && (label || !jit.Regs()[3])) function(jit.Regs()[0], jit.Regs()[1], length, label);
    });
    manager.RegisterThunkIfAbsent("glGetObjectLabelEXT", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static auto function = reinterpret_cast<PFNGLGETOBJECTLABELEXTPROC>(eglGetProcAddress("glGetObjectLabelEXT"));
        const GLsizei capacity = static_cast<int32_t>(jit.Regs()[2]);
        bool valid = false;
        auto* length = OptionalGuestOutput<GLsizei>(host, jit.Regs()[3], 1, "glGetObjectLabelEXT", &valid);
        if (!valid) return;
        auto* label = capacity > 0 ? GuestOutputArray<GLchar>(host, host.MemoryRead32(jit.Regs()[13]), capacity, 1, "glGetObjectLabelEXT") : nullptr;
        if (capacity > 0 && !label) return;
        if (function) function(jit.Regs()[0], jit.Regs()[1], capacity, length, label);
        else { if (length) *length = 0; if (label) *label = 0; }
    });
}
