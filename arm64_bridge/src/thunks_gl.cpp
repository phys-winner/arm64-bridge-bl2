#include "thunk_manager.h"
#include "bridge_log.h"
#include "bl2_fog_audit.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "perf_probe.h"
#include "legacy_glsl_compat.h"
#include "buffer_shadow_sync.h"
#include "obb_sha256.h"
#include "thunk_types.h"
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#define LOG_TAG "Arm32Bridge_GL"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

void SyncGuestMappedBuffersForGpuRead(DynarmicHost& host);
bool SceneAuditEnabled();
void SceneAuditLogError(const char* phase);
void SceneAuditNoteDraw(GLuint program);
uint32_t SceneAuditCurrentDrawCount(GLuint framebuffer);
void SceneAuditDrawCompleted(const char* operation);
void SceneAuditFramebufferTransition(GLenum target, GLuint next_framebuffer);
void SceneAuditLogFramebufferPixels(const char* label, uint32_t index,
                                    GLint x0, GLint y0, GLint x1, GLint y1);
bool UploadBgraPixels2D(DynarmicHost& host, GLenum target, GLint level,
                        GLint internalformat, GLsizei width, GLsizei height,
                        GLint border, GLint xoffset, GLint yoffset,
                        uint32_t address, bool subimage);

// GL bindings belong to the context current on a thread. Horn renders and
// prewarms resources on different threads, so a process-global mirror can make
// an offset look like a guest pointer (or vice versa). Querying the driver in
// every draw is too expensive; keep the mirror thread-local instead.
static thread_local GLuint s_bound_array_buffer = 0;
static thread_local GLuint s_bound_element_buffer = 0;
static thread_local GLuint s_bound_framebuffer = 0;
static thread_local EGLContext s_depth_bias_context = EGL_NO_CONTEXT;
static thread_local GLuint s_depth_bias_framebuffer = 0;
static thread_local GLint s_depth_bias_depth_bits = 0;
static thread_local float s_depth_bias_units_scale = 1.0f;
static thread_local bool s_depth_bias_cache_valid = false;
static thread_local std::unordered_map<GLenum, GLuint> s_bound_buffers;
void InvalidateGlStateCaches();

static bool ShouldLogTelemetry(std::atomic<uint32_t>& counter, uint32_t limit,
                               const char* name, uint32_t* index) {
    const uint32_t value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (index) *index = value;
    if (value <= limit) return true;
    if (value == limit + 1) {
        LOGI("GL telemetry: suppressing further %s calls after %u", name, limit);
    }
    return false;
}

bool HasGlES3() {
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return false;
    EGLint version = 0;
    return eglQueryContext(eglGetCurrentDisplay(), context,
                           EGL_CONTEXT_CLIENT_VERSION, &version) && version >= 3;
}

void TrackGlBufferBinding(GLenum target, GLuint buffer) {
    s_bound_buffers[target] = buffer;
    if (target == GL_ARRAY_BUFFER) s_bound_array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) s_bound_element_buffer = buffer;
}

GLuint GetBoundGlBuffer(GLenum target) {
    const auto it = s_bound_buffers.find(target);
    if (it != s_bound_buffers.end()) return it->second;
    if (target == GL_ARRAY_BUFFER) return s_bound_array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return s_bound_element_buffer;
    return 0;
}

void ForgetGlBufferBindings(GLsizei count, const GLuint* buffers) {
    if (count <= 0 || !buffers) return;
    for (GLsizei i = 0; i < count; ++i) {
        const GLuint deleted = buffers[i];
        if (!deleted) continue;
        for (auto& binding : s_bound_buffers) {
            if (binding.second == deleted) binding.second = 0;
        }
        if (s_bound_array_buffer == deleted) s_bound_array_buffer = 0;
        if (s_bound_element_buffer == deleted) s_bound_element_buffer = 0;
    }
}

void RefreshGlBindings() {
    InvalidateGlStateCaches();
    s_bound_array_buffer = 0;
    s_bound_element_buffer = 0;
    s_bound_framebuffer = 0;
    s_bound_buffers.clear();
    s_depth_bias_cache_valid = false;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return;
    GLint array = 0, element = 0, framebuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    TrackGlBufferBinding(GL_ARRAY_BUFFER, static_cast<GLuint>(array));
    TrackGlBufferBinding(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(element));
    s_bound_framebuffer = static_cast<GLuint>(framebuffer);
}

static uint64_t EstimatedPixelBytes(GLsizei width, GLsizei height,
                                    GLenum format, GLenum type) {
    if (width <= 0 || height <= 0) return 0;
    uint64_t bytes_per_pixel = 4;
    if (type == GL_UNSIGNED_SHORT_5_6_5 ||
        type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1) {
        bytes_per_pixel = 2;
    } else {
        uint64_t components = 4;
        if (format == GL_ALPHA || format == GL_LUMINANCE) components = 1;
        else if (format == GL_LUMINANCE_ALPHA) components = 2;
        else if (format == GL_RGB) components = 3;
        uint64_t component_bytes = 1;
        if (type == GL_FLOAT) component_bytes = 4;
#ifdef GL_HALF_FLOAT_OES
        else if (type == GL_HALF_FLOAT_OES) component_bytes = 2;
#endif
        bytes_per_pixel = components * component_bytes;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
           bytes_per_pixel;
}

GLenum TranslateLegacyGlExternalFormat(GLenum format) {
    if (format == GL_LUMINANCE || format == GL_ALPHA) return GL_RED;
    if (format == GL_LUMINANCE_ALPHA) return GL_RG;
    return format;
}

enum class LegacyTextureChannels {
    None,
    Luminance,
    Alpha,
    LuminanceAlpha,
};

static LegacyTextureChannels TranslateLegacyGlInternalFormat(
    GLint guest_internal, GLint* host_internal) {
    if (!host_internal) return LegacyTextureChannels::None;
    switch (static_cast<GLenum>(guest_internal)) {
        case GL_LUMINANCE:
            *host_internal = GL_R8;
            return LegacyTextureChannels::Luminance;
        case GL_ALPHA:
            *host_internal = GL_R8;
            return LegacyTextureChannels::Alpha;
        case GL_LUMINANCE_ALPHA:
            *host_internal = GL_RG8;
            return LegacyTextureChannels::LuminanceAlpha;
        default:
            *host_internal = guest_internal;
            return LegacyTextureChannels::None;
    }
}

static GLenum TextureParameterTarget(GLenum image_target) {
    if (image_target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
        image_target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
        return GL_TEXTURE_CUBE_MAP;
    }
    return image_target;
}

static void ApplyLegacyTextureSwizzle(GLenum image_target,
                                      LegacyTextureChannels channels) {
    if (channels == LegacyTextureChannels::None) return;
    constexpr GLenum kSwizzleParameters[] = {
        GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
        GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A,
    };
    GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    if (channels == LegacyTextureChannels::Luminance) {
        swizzle[0] = swizzle[1] = swizzle[2] = GL_RED;
        swizzle[3] = GL_ONE;
    } else if (channels == LegacyTextureChannels::Alpha) {
        swizzle[0] = swizzle[1] = swizzle[2] = GL_ZERO;
        swizzle[3] = GL_RED;
    } else {
        swizzle[0] = swizzle[1] = swizzle[2] = GL_RED;
        swizzle[3] = GL_GREEN;
    }
    // GLES exposes the four individual swizzles. The combined desktop-GL
    // GL_TEXTURE_SWIZZLE_RGBA token is invalid here and leaves R8 luminance
    // textures at (R,0,0,1), breaking shaders that read luminance through G/B.
    const GLenum target = TextureParameterTarget(image_target);
    for (size_t component = 0; component < std::size(kSwizzleParameters);
         ++component) {
        glTexParameteri(target, kSwizzleParameters[component], swizzle[component]);
    }
}

// Horn's UE3 ES2 renderer converts its normalized depth bias to polygon-offset
// units with a hard-coded 65535 multiplier (the range of the 16-bit depth
// buffer used by the original Tegra build). Android EGL is allowed to satisfy
// a DEPTH_SIZE=16 request with a 24-bit buffer. On such a surface the same
// units value is about 256 times too small, allowing coplanar decal/shadow
// passes to z-fight with the base surface. Scale only the units term back to
// the normalized bias UE3 intended; the slope-dependent factor is unchanged.
//
// debug.horn.depth_bias_scale is a diagnostic escape hatch. "auto" (and an
// unset property) uses the current framebuffer's depth precision; a finite
// positive number forces that multiplier, with 1 restoring the old behavior.
static bool GuestUses24BitPolygonOffsetUnits(DynarmicHost& host) {
    static const bool verified = [&host] {
        const auto& loader = host.GetElfLoader();
        if (loader.GetImageSize() != 0x03089000) return false;
        const uint32_t setter = loader.GetSymbolAddress(
            "_ZN17FOpenGLDynamicRHI18SetRasterizerStateEP19TDynamicRHIResourceIL17ERHIResourceTypes2EE");
        constexpr uint32_t expected[] = {
            0xEDD17A03, 0xEDDF6A42, 0xED907A04, 0xE79F3003,
            0xED936A00, 0xEE767A27, 0xEE677AA6,
        };
        if (!setter || !loader.IsReadOnlyRange(setter, 0x150) ||
            host.MemoryRead32(setter + 0x14C) != 0x4B7FFFFF) return false;
        for (size_t i = 0; i < std::size(expected); ++i) {
            if (host.MemoryRead32(setter + 0x38 + static_cast<uint32_t>(i * 4)) !=
                expected[i]) return false;
        }
        LOGI("BL2 polygon offset signature verified: guest already uses "
             "24-bit units (16777215); preserving GL units without Horn scaling");
        return true;
    }();
    return verified;
}

static float PolygonOffsetUnitsScale(DynarmicHost& host, GLint depth_bits) {
    static const float override_scale = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.horn.depth_bias_scale", value) <= 0 ||
            value[0] == '\0' || strcmp(value, "auto") == 0) {
            return 0.0f;
        }

        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end != value && *end == '\0' && std::isfinite(parsed) &&
            parsed > 0.0f && parsed <= 65537.0f) {
            return parsed;
        }
        LOGE("Ignoring invalid debug.horn.depth_bias_scale='%s'; using auto", value);
        return 0.0f;
    }();

    if (override_scale > 0.0f) return override_scale;
    // BL2's desktop renderer has already multiplied normalized bias by
    // 2^24-1. Its glPolygonOffset units are ready for the GL API; applying
    // Horn's 16-bit normalization again overbiases a 24-bit FBO by 256x.
    if (GuestUses24BitPolygonOffsetUnits(host)) return 1.0f;
    if (depth_bits <= 16 || depth_bits > 30) return 1.0f;

    const double actual_levels = std::ldexp(1.0, depth_bits) - 1.0;
    constexpr double kGuestDepthLevels = 65535.0;
    return static_cast<float>(actual_levels / kGuestDepthLevels);
}

static float CurrentPolygonOffsetUnitsScale(DynarmicHost& host, GLint& depth_bits) {
    const EGLContext context = eglGetCurrentContext();
    if (!s_depth_bias_cache_valid || context != s_depth_bias_context ||
        s_bound_framebuffer != s_depth_bias_framebuffer) {
        glGetIntegerv(GL_DEPTH_BITS, &s_depth_bias_depth_bits);
        s_depth_bias_units_scale =
            PolygonOffsetUnitsScale(host, s_depth_bias_depth_bits);
        s_depth_bias_context = context;
        s_depth_bias_framebuffer = s_bound_framebuffer;
        s_depth_bias_cache_valid = true;
    }
    depth_bits = s_depth_bias_depth_bits;
    return s_depth_bias_units_scale;
}

static bool GlStateAuditEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.gl_audit", value) > 0 &&
               strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
    }();
    return enabled;
}

static bool PresentAuditEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.present_audit", value) > 0 &&
               value[0] != '\0' && strcmp(value, "0") != 0 &&
               strcmp(value, "false") != 0;
    }();
    return enabled;
}

static thread_local GLenum s_deferred_gl_error = GL_NO_ERROR;
static std::atomic<uint32_t> s_gl_error_logs{0};

// Diagnostic glGetError calls must not conceal an error from the guest.
void PreserveSceneAuditGlError(GLenum error) {
    if (error != GL_NO_ERROR && s_deferred_gl_error == GL_NO_ERROR) {
        s_deferred_gl_error = error;
    }
}

static void CaptureGlError(const char* operation) {
    if (!GlStateAuditEnabled()) return;
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) return;
    if (s_deferred_gl_error == GL_NO_ERROR) s_deferred_gl_error = error;
    const uint32_t index = s_gl_error_logs.fetch_add(1, std::memory_order_relaxed);
    if (index < 128) {
        LOGE("GL audit: %s produced 0x%04X (tid=%d)", operation, error, gettid());
    }
}

static void AuditBufferBinding(
    const char* operation, GLenum binding_name, GLuint mirrored_binding
) {
    if (!GlStateAuditEnabled()) return;
    static thread_local uint32_t calls = 0;
    const uint32_t call = ++calls;
    if ((call <= 4096 && (call & 63u) != 0) ||
        (call > 4096 && (call & 1023u) != 0)) {
        return;
    }

    GLint actual_binding = 0;
    glGetIntegerv(binding_name, &actual_binding);
    if (static_cast<GLuint>(actual_binding) == mirrored_binding) return;
    static std::atomic<uint32_t> mismatch_logs{0};
    const uint32_t index = mismatch_logs.fetch_add(1, std::memory_order_relaxed);
    if (index < 128) {
        LOGE("GL audit: %s binding mismatch mirror=%u actual=%d context=%p tid=%d",
             operation, mirrored_binding, actual_binding,
             reinterpret_cast<void*>(eglGetCurrentContext()), gettid());
    }
}

struct GuestBufferMapping {
    GLenum target;
    GLenum access;
    GLsizeiptr size;
    void* host_pointer;
    uint32_t guest_address;
};

static std::mutex s_buffer_mapping_mutex;
static std::unordered_map<GLuint, GuestBufferMapping> s_buffer_mappings;
static std::unordered_map<GLuint, std::vector<uint8_t>> s_buffer_shadows;

bool CopyGlBufferShadow(GLuint buffer, size_t offset, size_t size, void* output) {
    if (!buffer || !output) return false;
    std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
    const auto found = s_buffer_shadows.find(buffer);
    if (found == s_buffer_shadows.end() || offset > found->second.size() ||
        size > found->second.size() - offset) {
        return false;
    }
    std::memcpy(output, found->second.data() + offset, size);
    return true;
}

size_t SyncChangedGlBufferMapping(GLuint buffer, size_t offset, size_t size,
                                  void* native_mapping, const void* guest_data,
                                  bool force_full) {
    std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
    const auto found = s_buffer_shadows.find(buffer);
    if (found == s_buffer_shadows.end() || offset > found->second.size() ||
        size > found->second.size() - offset) {
        // Preserve the previous complete-copy behavior without a trustworthy
        // CPU mirror. Never infer that missing shadow data is unchanged.
        std::memcpy(native_mapping, guest_data, size);
        return size;
    }
    return SyncChangedBufferChunks(native_mapping, found->second.data() + offset,
                                    guest_data, size, force_full);
}

void UpdateGlBufferShadow(GLuint buffer, size_t offset, size_t size,
                          const void* data) {
    if (!buffer || (!data && size != 0)) return;
    std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
    const auto found = s_buffer_shadows.find(buffer);
    if (found == s_buffer_shadows.end() || offset > found->second.size() ||
        size > found->second.size() - offset) {
        return;
    }
    if (size != 0) {
        std::memcpy(found->second.data() + offset, data, size);
    }
}

static std::mutex s_egl_display_mutex;
static std::unordered_map<uint32_t, EGLDisplay> s_egl_displays;
static uint32_t s_next_egl_display_handle = 1;

// UI scaling, the virtual-resolution way: tell the guest the screen is
// s_virtual{Width,Height} and stretch the result here. The guest sizes its viewport
// in those units while the real default framebuffer is still the surface size, so
// the window-system framebuffer -- and only that one -- gets mapped back. Offscreen
// targets are allocated in the guest's own units and must not be touched, which is
// what the s_bound_framebuffer check is for.
// Leave s_virtualWidth/Height at 0 to disable the whole path.
static void ScaleViewportToSurface(GLint& x, GLint& y, GLsizei& w, GLsizei& h) {
    if (s_bound_framebuffer != 0) return;
    const uint32_t vw = ThunkManager::s_virtualWidth, vh = ThunkManager::s_virtualHeight;
    const uint32_t rw = ThunkManager::s_screenWidth, rh = ThunkManager::s_screenHeight;
    if (!vw || !vh || !rw || !rh || (vw == rw && vh == rh)) return;
    x = static_cast<GLint>((float)x * rw / vw);
    w = static_cast<GLsizei>((float)w * rw / vw);
    y = static_cast<GLint>((float)y * rh / vh);
    h = static_cast<GLsizei>((float)h * rh / vh);
}
// One raw UE3 mobile post-process shader contains desktop-style #include lines
// inside an inactive #if UNUSED block. GLSL ES 1.00 has no #include directive,
// and current drivers reject the quote tokens before eliminating the block.
// Match the complete, unique block and its neighboring symbols; changing only
// "#i" to "//" keeps byte counts and source line numbers stable.
static bool NeutralizeKnownInactiveUberIncludes(std::string& source) {
    constexpr char kFeature[] = "#define SUPPORTS_DEPTH_TEXTURES 0";
    constexpr char kGuard[] = "#if UNUSED";
    constexpr char kInclude0[] = "#include \"PostProcessCommon.usf\"";
    constexpr char kInclude1[] = "#include \"DepthOfFieldCommon.usf\"";
    constexpr char kInclude2[] = "#include \"MotionBlurCommon.usf\"";
    constexpr char kEndif[] = "#endif";
    constexpr char kFollowingSymbol[] = "BloomTintAndScreenBlendThreshold";

    const size_t feature = source.find(kFeature);
    const size_t guard = source.find(kGuard);
    const size_t include0 = source.find(kInclude0);
    const size_t include1 = source.find(kInclude1);
    const size_t include2 = source.find(kInclude2);
    const size_t endif = source.find(kEndif, include2);
    const size_t following = source.find(kFollowingSymbol, endif);
    if (feature == std::string::npos || guard == std::string::npos ||
        include0 == std::string::npos || include1 == std::string::npos ||
        include2 == std::string::npos || endif == std::string::npos ||
        following == std::string::npos) {
        return false;
    }
    if (!(feature < guard && guard < include0 && include0 < include1 &&
          include1 < include2 && include2 < endif && endif < following) ||
        guard - feature > 128 || include0 - guard > 128 ||
        include1 - include0 > 128 || include2 - include1 > 128 ||
        endif - include2 > 128 || following - endif > 512) {
        return false;
    }
    if (source.find(kInclude0, include0 + 1) != std::string::npos ||
        source.find(kInclude1, include1 + 1) != std::string::npos ||
        source.find(kInclude2, include2 + 1) != std::string::npos) {
        return false;
    }

    source.replace(include0, 2, "//");
    source.replace(include1, 2, "//");
    source.replace(include2, 2, "//");
    return true;
}

static GLuint BoundBufferForTarget(GLenum target) {
    return GetBoundGlBuffer(target);
}

// Advertise a guest-side S3TC profile so UE3 selects its cooked texture path.
// The paired patch OBB contains ETC2 blocks in the original DXT-sized mip slots.
// Map the guest S3TC enums to GLES3 ETC2; the driver receives compressed blocks.
static bool IsUnavailableHornTextureExtension(const std::string& extension) {
    if (extension == "GL_EXT_texture_compression_s3tc") return false;
    if (extension == "GL_OES_compressed_ETC1_RGB8_texture") return true;
    if (extension == "GL_NV_texture_compression") return true;
    if (extension.find("texture_compression") != std::string::npos) return true;
    if (extension.find("compressed_ATC") != std::string::npos) return true;
    return extension.find("ETC2") != std::string::npos;
}

constexpr GLenum kDxt1Rgb = 0x83F0;
constexpr GLenum kDxt1Rgba = 0x83F1;
constexpr GLenum kDxt3Rgba = 0x83F2;
constexpr GLenum kDxt5Rgba = 0x83F3;
constexpr GLenum kDxt1Srgb = 0x8C4C;
constexpr GLenum kDxt1Srgba = 0x8C4D;
constexpr GLenum kDxt3Srgba = 0x8C4E;
constexpr GLenum kDxt5Srgba = 0x8C4F;

static bool IsDxt1(GLenum format) {
    return format == kDxt1Rgb || format == kDxt1Rgba ||
           format == kDxt1Srgb || format == kDxt1Srgba;
}

static bool IsDxt3(GLenum format) {
    return format == kDxt3Rgba || format == kDxt3Srgba;
}

static bool IsDxt5(GLenum format) {
    return format == kDxt5Rgba || format == kDxt5Srgba;
}

static GLenum Etc2InternalFormatForDxt(GLenum format) {
    switch (format) {
        case kDxt1Rgb:
        case kDxt1Rgba:
            return GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2;
        case kDxt1Srgb:
        case kDxt1Srgba:
            return GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2;
        case kDxt5Rgba:
            return GL_COMPRESSED_RGBA8_ETC2_EAC;
        case kDxt5Srgba:
            return GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC;
        default:
            return 0;  // DXT3 was absent from the offline texture inventory.
    }
}

static size_t DxtBlockBytes(GLenum format) {
    if (IsDxt1(format)) return 8;
    if (IsDxt3(format) || IsDxt5(format)) return 16;
    return 0;
}

static bool DxtPayloadSize(GLenum format, GLsizei width, GLsizei height,
                           size_t& size) {
    const size_t block_bytes = DxtBlockBytes(format);
    if (block_bytes == 0 || width <= 0 || height <= 0) return false;
    const uint64_t blocks_x = (static_cast<uint64_t>(width) + 3u) / 4u;
    const uint64_t blocks_y = (static_cast<uint64_t>(height) + 3u) / 4u;
    const uint64_t bytes = blocks_x * blocks_y * block_bytes;
    if (bytes > SIZE_MAX) return false;
    size = static_cast<size_t>(bytes);
    return true;
}

static const uint8_t* GuestTextureBytes(DynarmicHost& host, uint32_t guest_ptr,
                                       size_t required,
                                       std::vector<uint8_t>& scratch,
                                       bool& valid) {
    GLint unpack_buffer = 0;
    if (HasGlES3()) glGetIntegerv(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */,
                                  &unpack_buffer);
    if (unpack_buffer != 0) {
        std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
        const auto it = s_buffer_shadows.find(static_cast<GLuint>(unpack_buffer));
        const uint64_t offset = guest_ptr;
        if (it == s_buffer_shadows.end() || offset > it->second.size() ||
            required > it->second.size() - static_cast<size_t>(offset)) {
            valid = false;
            LOGE("Compressed texture upload references untracked/out-of-range unpack buffer %d offset=%u bytes=%zu",
                 unpack_buffer, guest_ptr, required);
            return nullptr;
        }
        try {
            const auto begin = it->second.begin() + static_cast<size_t>(offset);
            scratch.assign(begin, begin + required);
        } catch (...) {
            valid = false;
            return nullptr;
        }
        valid = true;
        return scratch.data();
    }
    const void* span = GuestSpan(host.GetGuestMemoryBase(),
                                 host.GetGuestMemorySize(), guest_ptr, required);
    valid = span != nullptr;
    if (!valid) {
        LOGE("Compressed texture upload guest range invalid: address=0x%08X bytes=%zu",
             guest_ptr, required);
    }
    return static_cast<const uint8_t*>(span);
}

class ScopedUnpackBufferUnbind {
public:
    ScopedUnpackBufferUnbind() {
        if (!HasGlES3()) return;
        glGetIntegerv(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &previous_);
        if (previous_ != 0) {
            glBindBuffer(0x88EC /* GL_PIXEL_UNPACK_BUFFER */, 0);
            TrackGlBufferBinding(0x88EC, 0);
            active_ = true;
        }
    }
    ~ScopedUnpackBufferUnbind() {
        if (active_) {
            glBindBuffer(0x88EC /* GL_PIXEL_UNPACK_BUFFER */,
                         static_cast<GLuint>(previous_));
            TrackGlBufferBinding(0x88EC, static_cast<GLuint>(previous_));
        }
    }
    ScopedUnpackBufferUnbind(const ScopedUnpackBufferUnbind&) = delete;
    ScopedUnpackBufferUnbind& operator=(const ScopedUnpackBufferUnbind&) = delete;

private:
    GLint previous_ = 0;
    bool active_ = false;
};

static bool HasExtensionToken(const char* extensions, const char* token) {
    if (!extensions || !token || !*token) return false;
    const size_t token_length = strlen(token);
    const char* cursor = extensions;
    while ((cursor = strstr(cursor, token)) != nullptr) {
        const bool starts_token =
            cursor == extensions || *(cursor - 1) == ' ';
        const char following = cursor[token_length];
        if (starts_token && (following == '\0' || following == ' ')) return true;
        cursor += token_length;
    }
    return false;
}

// The desktop GL guest resolves this entry point even when the GLES driver
// does not expose depth-bounds testing. Returning NULL leaves a call through
// glDepthBoundsEXT at CommitDirtyState+0x478 and kills its render thread.
// Keep the guest entry point valid; forward it only when the actual context
// supports the extension. GLES has no equivalent for the unsupported case.
using DepthBoundsProc = void (*)(double, double);
static DepthBoundsProc NativeDepthBoundsProc() {
    static thread_local EGLContext checked_context = EGL_NO_CONTEXT;
    static thread_local bool checked = false;
    static thread_local DepthBoundsProc function = nullptr;
    const EGLContext context = eglGetCurrentContext();
    if (checked && context == checked_context) return function;
    checked_context = context;
    checked = true;
    function = nullptr;
    if (context != EGL_NO_CONTEXT) {
        const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (HasExtensionToken(extensions, "GL_EXT_depth_bounds_test")) {
            function = reinterpret_cast<DepthBoundsProc>(
                eglGetProcAddress("glDepthBoundsEXT"));
        }
    }
    LOGI("Depth bounds: native extension %s; guest entry available",
         function ? "active" : "absent");
    return function;
}

constexpr GLenum kDepthBoundsTestEXT = 0x8890;

// The 2013 guest never sets anisotropic filtering. Request the host maximum
// only when the real driver advertises the extension; querying its enum on an
// unsupported context would itself generate GL_INVALID_ENUM.
static std::atomic<int> s_aniso_support{-1};  // -1 unknown, 0 absent, 1 present
static std::atomic<GLint> s_max_aniso{-1};
static std::atomic<bool> s_logged_aniso{false};
static void ForceAnisotropy(GLenum target) {
    if (target != GL_TEXTURE_2D) return;

    int support = s_aniso_support.load(std::memory_order_acquire);
    if (support < 0) {
        const auto* extensions = reinterpret_cast<const char*>(
            glGetString(GL_EXTENSIONS));
        if (!extensions) return;
        support = HasExtensionToken(
            extensions, "GL_EXT_texture_filter_anisotropic") ? 1 : 0;
        s_aniso_support.store(support, std::memory_order_release);
    }
    if (support == 0) return;

    GLint max_aniso = s_max_aniso.load(std::memory_order_acquire);
    if (max_aniso < 0) {
        glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
        if (max_aniso < 1) max_aniso = 0;
        s_max_aniso.store(max_aniso, std::memory_order_release);
    }
    if (max_aniso > 1) {
        glTexParameteri(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, max_aniso);
        bool expected = false;
        if (s_logged_aniso.compare_exchange_strong(expected, true)) {
            LOGI("Anisotropic filtering: host maximum %dx", max_aniso);
        }
    }
}

// Keep the game's mip filtering, but blend between adjacent mip levels rather
// than snapping to the nearest one. Both values are exact integers even in the
// glTexParameterf entry point.
static GLint UpgradeMinFilter(GLenum pname, GLint value) {
    return pname == GL_TEXTURE_MIN_FILTER && value == GL_LINEAR_MIPMAP_NEAREST
        ? GL_LINEAR_MIPMAP_LINEAR
        : value;
}
static thread_local bool s_logged_trilinear_upgrade = false;

enum class UniformTuning : uint8_t {
    None,
    BloomTintAndScreenBlendThreshold,
    BloomScaleAndThreshold,
    DofFactor,
    DofPackedW,
    LightShaftY,
};

struct UniformBinding {
    std::string name;
    UniformTuning tuning;
};

// OpenGL contexts are current to one thread, but UE3 may compile/query a shared
// program on a worker and later use it on the render thread. Current program is
// therefore thread-local while the immutable location/name cache is shared.
static thread_local GLuint s_current_program = 0;
static thread_local bool s_program_cache_valid = false;
struct LegacyUnitBinding {
    GLenum target = 0;
    GLuint texture = 0;
    GLuint sampler = 0;
    bool texture_valid = false;
    bool sampler_valid = false;
};
static thread_local std::array<LegacyUnitBinding, 32> s_legacy_unit_bindings{};
static std::atomic<uint64_t> s_legacy_bindings_epoch{1};
static thread_local uint64_t s_seen_legacy_bindings_epoch = 0;
struct AttribPointerBinding {
    GLint size = 0;
    GLenum type = 0;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    uintptr_t pointer = 0;
    GLuint buffer = 0;
    bool valid = false;
};
static thread_local std::array<AttribPointerBinding, 32> s_attrib_pointers{};
static std::atomic<uint64_t> s_gl_program_attempts{0}, s_gl_program_redundant{0},
    s_gl_program_skipped{0}, s_gl_texture_attempts{0},
    s_gl_texture_redundant{0}, s_gl_texture_skipped{0},
    s_gl_sampler_attempts{0}, s_gl_sampler_redundant{0},
    s_gl_sampler_skipped{0}, s_gl_attrib_attempts{0},
    s_gl_attrib_redundant{0}, s_gl_attrib_skipped{0};

static int GlStateCacheMode() {
    static const int mode = [] {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.bl2.gl_state_mode", value) <= 0) return 0;
        return std::clamp(atoi(value), 0, 7);
    }();
    return mode;
}

static bool GlStateCacheAuditEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.bl2.gl_state_audit", value) > 0 &&
               value[0] == '1';
    }();
    return enabled;
}

void InvalidateGlAttribCache() {
    for (auto& entry : s_attrib_pointers) entry.valid = false;
}

void InvalidateLegacyTextureCache() {
    for (auto& entry : s_legacy_unit_bindings) {
        entry.texture_valid = false;
        entry.sampler_valid = false;
    }
    s_seen_legacy_bindings_epoch = s_legacy_bindings_epoch.load(std::memory_order_acquire);
}

void InvalidateGlStateCaches() {
    s_program_cache_valid = false;
    InvalidateLegacyTextureCache();
    InvalidateGlAttribCache();
}

void InvalidateLegacyTextureCacheAcrossContexts() {
    s_legacy_bindings_epoch.fetch_add(1, std::memory_order_release);
    InvalidateLegacyTextureCache();
}

void GlStateProbePresent() {
    if (!GlStateCacheAuditEnabled()) return;
    static std::atomic<uint64_t> frames{0};
    const uint64_t frame = frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame % 240 != 0) return;
    LOGI("GL_STATE mode=%d frames=%llu program=%llu/%llu/%llu "
         "texture=%llu/%llu/%llu sampler=%llu/%llu/%llu attrib=%llu/%llu/%llu",
         GlStateCacheMode(), static_cast<unsigned long long>(frame),
         static_cast<unsigned long long>(s_gl_program_attempts.exchange(0)),
         static_cast<unsigned long long>(s_gl_program_redundant.exchange(0)),
         static_cast<unsigned long long>(s_gl_program_skipped.exchange(0)),
         static_cast<unsigned long long>(s_gl_texture_attempts.exchange(0)),
         static_cast<unsigned long long>(s_gl_texture_redundant.exchange(0)),
         static_cast<unsigned long long>(s_gl_texture_skipped.exchange(0)),
         static_cast<unsigned long long>(s_gl_sampler_attempts.exchange(0)),
         static_cast<unsigned long long>(s_gl_sampler_redundant.exchange(0)),
         static_cast<unsigned long long>(s_gl_sampler_skipped.exchange(0)),
         static_cast<unsigned long long>(s_gl_attrib_attempts.exchange(0)),
         static_cast<unsigned long long>(s_gl_attrib_redundant.exchange(0)),
         static_cast<unsigned long long>(s_gl_attrib_skipped.exchange(0)));
}
static thread_local bool s_current_gfx_text = false;
static thread_local bool s_current_postprocess_blend = false;
static thread_local GLuint s_scene_final_program = 0;
static thread_local std::unordered_set<GLuint> s_scene_gfx_programs;
static thread_local std::unordered_set<GLuint> s_scene_gfx_perspective_programs;
static std::shared_mutex s_uniform_binding_mutex;
static std::unordered_map<uint64_t, UniformBinding> s_uniform_bindings;
static std::atomic<uint64_t> s_uniform_binding_epoch{1};
static std::atomic<uint32_t> s_logged_uniform_tunings{0};

// The Shield build carries two representations of every shader in one blob:
// a GLES-oriented GLSL source string followed by a NUL and the !!NVvp5.0 or
// !!NVfp5.0 program submitted to NVIDIA's desktop-GL driver.  The latter is
// impossible to execute on a normal Android GLES driver, but the former is
// exactly the source we need.  Recover that adjacent source and emulate the
// ARB assembly-program object model with linked GLES 3.2 programs.
constexpr GLenum kVertexProgramArb = 0x8620;
constexpr GLenum kFragmentProgramArb = 0x8804;
constexpr GLenum kProgramFormatAsciiArb = 0x8875;
constexpr uint32_t kFirstLegacyProgramName = 0x60000000u;
constexpr size_t kMaxLegacyShaderSource = 1024 * 1024;

struct LegacyUniformArray {
    GLint location = -1;
    GLsizei size = 0;
};

struct LegacyConstantGroup {
    int16_t first_register = -1;
    uint16_t byte_count = 0;
};

struct LegacyAssemblyStage {
    GLenum target = 0;
    GLuint shader = 0;
    std::string name;
    std::string diagnostic_source;
    std::vector<uint32_t> local_parameters;
    std::array<LegacyConstantGroup, 9> constant_groups{};
    std::array<int16_t, 16> sampler_registers = [] {
        std::array<int16_t, 16> values{};
        values.fill(-1);
        return values;
    }();
    std::array<GLenum, 16> sampler_targets{};
};

struct LegacyAssemblyPipeline {
    GLuint program = 0;
    bool samplers_initialized = false;
    // Uniform values belong to the linked GL program, rather than the current
    // context. The program mutex serializes stage changes and draw uploads.
    std::array<std::vector<uint32_t>, 9> uploaded_constants{};
    std::array<GLsizei, 9> uploaded_counts{};
    EGLContext uniforms_context = EGL_NO_CONTEXT;
    LegacyUniformArray vertex_float;
    LegacyUniformArray fragment_float;
    LegacyUniformArray vertex_global;
    LegacyUniformArray fragment_global;
    LegacyUniformArray vertex_bones;
    LegacyUniformArray vertex_int;
    LegacyUniformArray fragment_int;
    LegacyUniformArray vertex_bool;
    LegacyUniformArray fragment_bool;
};

static std::mutex s_legacy_program_mutex;
static std::unordered_map<GLuint, LegacyAssemblyStage> s_legacy_stages;
static std::unordered_map<uint64_t, LegacyAssemblyPipeline> s_legacy_pipelines;
static std::atomic<uint32_t> s_next_legacy_program_name{kFirstLegacyProgramName};
static std::atomic<uint32_t> s_legacy_compile_logs{0};
static std::atomic<uint32_t> s_legacy_link_logs{0};
static thread_local GLuint s_bound_legacy_vertex = 0;
static thread_local GLuint s_bound_legacy_fragment = 0;

static bool ModelAuditEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.model_audit", value) > 0 &&
               value[0] == '1';
    }();
    return enabled;
}

struct LegacyTextureHandle {
    GLuint texture = 0;
    GLuint sampler = 0;
    bool resident = false;
};

constexpr uint64_t kLegacyTextureHandleMagic = 0x484F524E00000000ull;
static std::atomic<uint32_t> s_next_legacy_texture_handle{1};
static std::unordered_map<uint64_t, LegacyTextureHandle>
    s_legacy_texture_handles;
static std::unordered_map<uint64_t, uint64_t> s_legacy_texture_handle_by_pair;

struct SceneTextureFingerprint {
    std::array<char, 65> sha256{};
    GLsizei width = 0;
    GLsizei height = 0;
    GLenum format = 0;
};

static std::mutex s_scene_texture_fingerprints_mutex;
static std::unordered_map<GLuint, SceneTextureFingerprint>
    s_scene_texture_fingerprints;

static bool SceneTextureHashEnabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.scene_texture_hash", value) > 0 &&
               value[0] == '1';
    }();
    return enabled;
}

static void RecordSceneTextureUpload(GLenum target, GLint level,
                                     GLsizei width, GLsizei height,
                                     GLenum format, const void* data,
                                     size_t bytes) {
    if (!SceneTextureHashEnabled() || target != GL_TEXTURE_2D || level != 0 ||
        width < 128 || height < 128 || !data || bytes == 0) return;
    GLint texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    if (texture <= 0) return;
    ObbSha256 hash;
    if (!hash.Update(static_cast<const uint8_t*>(data), bytes)) return;
    SceneTextureFingerprint fingerprint;
    fingerprint.sha256 = ObbSha256::Hex(hash.Finalize());
    fingerprint.width = width;
    fingerprint.height = height;
    fingerprint.format = format;
    std::lock_guard<std::mutex> lock(s_scene_texture_fingerprints_mutex);
    s_scene_texture_fingerprints[static_cast<GLuint>(texture)] = fingerprint;
}

static uint64_t LegacyPipelineKey(GLuint vertex, GLuint fragment) {
    return (static_cast<uint64_t>(vertex) << 32) | fragment;
}

static bool IsLegacyProgramTarget(GLenum target) {
    return target == kVertexProgramArb || target == kFragmentProgramArb;
}

static GLenum LegacyShaderType(GLenum target) {
    return target == kVertexProgramArb ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
}

static GLenum LegacySamplerTarget(std::string_view declaration) {
    if (declaration.find("samplerCubeArray") != std::string_view::npos) {
        return 0x9009; // GL_TEXTURE_CUBE_MAP_ARRAY
    }
    if (declaration.find("sampler2DArray") != std::string_view::npos) {
        return 0x8C1A; // GL_TEXTURE_2D_ARRAY
    }
    if (declaration.find("sampler2DMS") != std::string_view::npos) {
        return 0x9100; // GL_TEXTURE_2D_MULTISAMPLE
    }
    if (declaration.find("samplerBuffer") != std::string_view::npos) {
        return 0x8C2A; // GL_TEXTURE_BUFFER
    }
    if (declaration.find("samplerCube") != std::string_view::npos) {
        return GL_TEXTURE_CUBE_MAP;
    }
    if (declaration.find("sampler3D") != std::string_view::npos) {
        return 0x806F; // GL_TEXTURE_3D
    }
    if (declaration.find("sampler2D") != std::string_view::npos ||
        declaration.find("SHAD__SMP") != std::string_view::npos) {
        return GL_TEXTURE_2D;
    }
    return 0;
}

static std::array<GLenum, 16> DiscoverLegacySamplerTargets(
    std::string_view source, GLenum target) {
    std::array<GLenum, 16> targets{};
    const char prefix = target == kVertexProgramArb ? 'V' : 'P';
    for (size_t unit = 0; unit < targets.size(); ++unit) {
        const std::string name = std::string(1, prefix) +
                                 "Sampler" + std::to_string(unit);
        const size_t position = source.find(name);
        if (position == std::string_view::npos) continue;
        size_t line_begin = source.rfind('\n', position);
        line_begin = line_begin == std::string_view::npos ? 0 : line_begin + 1;
        size_t line_end = source.find('\n', position);
        if (line_end == std::string_view::npos) line_end = source.size();
        targets[unit] = LegacySamplerTarget(
            source.substr(line_begin, line_end - line_begin));
    }
    return targets;
}

static uint64_t GetOrCreateLegacyTextureHandleLocked(GLuint texture,
                                                      GLuint sampler) {
    const uint64_t pair = (static_cast<uint64_t>(texture) << 32) | sampler;
    const auto existing = s_legacy_texture_handle_by_pair.find(pair);
    if (existing != s_legacy_texture_handle_by_pair.end()) {
        return existing->second;
    }
    uint32_t serial = s_next_legacy_texture_handle.fetch_add(
        1, std::memory_order_relaxed);
    if (!serial) {
        serial = s_next_legacy_texture_handle.fetch_add(
            1, std::memory_order_relaxed);
    }
    const uint64_t handle = kLegacyTextureHandleMagic | serial;
    s_legacy_texture_handles.emplace(
        handle, LegacyTextureHandle{texture, sampler, false});
    s_legacy_texture_handle_by_pair.emplace(pair, handle);
    return handle;
}

static uint64_t GuestU64(const Dynarmic::A32::Jit& jit, size_t first_reg = 0) {
    return static_cast<uint64_t>(jit.Regs()[first_reg]) |
           (static_cast<uint64_t>(jit.Regs()[first_reg + 1]) << 32);
}

static void ReturnGuestU64(Dynarmic::A32::Jit& jit, uint64_t value) {
    jit.Regs()[0] = static_cast<uint32_t>(value);
    jit.Regs()[1] = static_cast<uint32_t>(value >> 32);
}

static std::string LegacyShaderName(std::string_view source) {
    constexpr std::string_view marker = "//ShaderFileName:";
    const size_t marker_pos = source.find(marker);
    if (marker_pos == std::string_view::npos) return "unknown";
    size_t begin = marker_pos + marker.size();
    while (begin < source.size() &&
           (source[begin] == ' ' || source[begin] == '\t')) {
        ++begin;
    }
    size_t end = source.find_first_of("\r\n", begin);
    if (end == std::string_view::npos) end = source.size();
    while (end > begin &&
           (source[end - 1] == ' ' || source[end - 1] == '\t')) {
        --end;
    }
    return std::string(source.substr(begin, end - begin));
}

static bool RecoverAdjacentLegacyGlsl(const DynarmicHost& host,
                                      uint32_t nv_address,
                                      size_t nv_length,
                                      std::string* source) {
    if (!source || nv_address < 2 || nv_length == 0 ||
        nv_length > kMaxLegacyShaderSource ||
        !GuestRangeValid(host.GetGuestMemorySize(), nv_address, nv_length)) {
        return false;
    }

    const uint8_t* memory = host.GetGuestMemoryBase();
    if (memory[nv_address - 1] != 0) return false;

    const uint32_t lower = nv_address > kMaxLegacyShaderSource
        ? nv_address - static_cast<uint32_t>(kMaxLegacyShaderSource)
        : 0;
    uint32_t begin = nv_address - 1;
    while (begin > lower && memory[begin - 1] != 0) --begin;
    if (begin == nv_address - 1) return false;

    const size_t length = static_cast<size_t>(nv_address - 1 - begin);
    if (length == 0 || length > kMaxLegacyShaderSource) return false;
    std::string recovered(reinterpret_cast<const char*>(memory + begin), length);
    const size_t marker = recovered.rfind("//ShaderFileName:");
    if (marker == std::string::npos) return false;
    if (marker != 0) recovered.erase(0, marker);
    if (recovered.find("void main") == std::string::npos) {
        return false;
    }
    *source = std::move(recovered);
    return true;
}

static std::string NormalizeLegacyGlsl(std::string_view source, GLenum target) {
    std::string normalized;
    normalized.reserve(source.size() + 32);
    normalized += "#version 320 es\n";
    // Desktop GLSL does not quantize texture() results. GLES defaults plain
    // sampler2D/samplerCube to lowp even when floats default to highp; a
    // lowp lookup can lose the RGBA16F scene alpha used as fog depth.
    normalized += "precision highp sampler2D;\n"
                  "precision highp samplerCube;\n"
                  "precision highp sampler2DShadow;\n"
                  "precision highp samplerCubeShadow;\n"
                  "precision highp sampler2DArrayShadow;\n";
    // Opt-in isolation of depth disagreement between separately linked
    // terrain programs sharing an original NV vertex program. GLSL ES 3.20
    // section 4.8 permits different position rounding without invariance.
    static const bool invariant_position_probe = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.invariant_position", value) > 0 &&
               value[0] == '1';
    }();
    if (invariant_position_probe && target == kVertexProgramArb) {
        normalized += "invariant gl_Position;\n";
        static std::atomic<uint32_t> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) < 4) {
            LOGI("Legacy vertex position invariance probe shader=%s",
                 LegacyShaderName(source).c_str());
        }
    }

    size_t cursor = 0;
    while (cursor < source.size()) {
        size_t end = source.find('\n', cursor);
        if (end == std::string_view::npos) end = source.size();
        std::string_view line = source.substr(cursor, end - cursor);
        std::string_view trimmed = line;
        while (!trimmed.empty() &&
               (trimmed.front() == ' ' || trimmed.front() == '\t' ||
                trimmed.front() == '\r')) {
            trimmed.remove_prefix(1);
        }
        // The cache keeps candidate version lines as comments today.  Drop an
        // active one too, so malformed/alternate caches cannot create a second
        // #version after the bridge's GLES 3.20 directive.
        if (!trimmed.starts_with("#version") &&
            !trimmed.starts_with("#extension GL_OES_standard_derivatives")) {
            // NVIDIA's compiler accepted input aliases initialized from vertex
            // attributes at global scope.  GLSL ES correctly rejects those as
            // non-constant global initializers.  A textual alias has identical
            // read-only semantics for the generated shaders and requires no
            // per-shader rewriting inside main().
            constexpr std::string_view attribute_decl = "vec4 Un_Attr";
            const size_t equals = trimmed.find(" = _Un_Attr");
            if (trimmed.starts_with(attribute_decl) &&
                equals != std::string_view::npos && trimmed.ends_with(';')) {
                const std::string_view alias = trimmed.substr(
                    sizeof("vec4 ") - 1, equals - (sizeof("vec4 ") - 1));
                std::string_view input = trimmed.substr(equals + 3);
                input.remove_suffix(1);
                if (input.size() == alias.size() + 1 && input.front() == '_' &&
                    input.substr(1) == alias) {
                    normalized += "#define ";
                    normalized.append(alias.data(), alias.size());
                    normalized.push_back(' ');
                    normalized.append(input.data(), input.size());
                    normalized.push_back('\n');
                } else {
                    normalized.append(line.data(), line.size());
                    normalized.push_back('\n');
                }
            } else {
                normalized.append(line.data(), line.size());
                normalized.push_back('\n');
            }
        }
        cursor = end == source.size() ? end : end + 1;
    }
    normalized = legacy_glsl::NormalizeProjectedSwizzles(normalized);
    return normalized;
}

static std::string ShaderInfoLog(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) return {};
    std::string log(static_cast<size_t>(length), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, length, &written, log.data());
    if (written >= 0 && written < length) log.resize(static_cast<size_t>(written));
    return log;
}

static std::string ProgramInfoLog(GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) return {};
    std::string log(static_cast<size_t>(length), '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(program, length, &written, log.data());
    if (written >= 0 && written < length) log.resize(static_cast<size_t>(written));
    return log;
}

static LegacyUniformArray FindLegacyUniformArray(GLuint program,
                                                  const char* wanted) {
    LegacyUniformArray result;
    result.location = glGetUniformLocation(program, wanted);
    if (result.location < 0) return result;

    GLint count = 0;
    GLint max_name = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_name);
    if (count <= 0 || max_name <= 1) return result;

    std::vector<GLchar> name(static_cast<size_t>(max_name));
    const std::string wanted_array = std::string(wanted) + "[0]";
    for (GLint index = 0; index < count; ++index) {
        GLsizei written = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, static_cast<GLuint>(index), max_name,
                           &written, &size, &type, name.data());
        if (written <= 0) continue;
        const std::string_view active(name.data(), static_cast<size_t>(written));
        if (active == wanted || active == wanted_array) {
            result.size = static_cast<GLsizei>(std::max(size, 0));
            break;
        }
    }
    return result;
}

static void ConfigureLegacySamplers(GLuint program) {
    GLint max_units = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_units);
    const GLint limit = std::clamp(max_units, 0, 32);
    for (GLint unit = 0; unit < limit; ++unit) {
        for (const char prefix : {'V', 'P'}) {
            const std::string name = std::string(1, prefix) +
                                     "Sampler" + std::to_string(unit);
            const GLint location = glGetUniformLocation(program, name.c_str());
            if (location >= 0) glUniform1i(location, unit);
        }
    }
}

enum class LegacyConstantType { Float4, Int4, Bool };

static void UploadLegacyConstantGroup(LegacyAssemblyPipeline& pipeline,
                                     const LegacyAssemblyStage& stage,
                                     const LegacyUniformArray& uniforms,
                                     size_t group_index,
                                     LegacyConstantType type) {
    const auto& group = stage.constant_groups[group_index];
    if (uniforms.location < 0 || uniforms.size <= 0 || group.first_register < 0 ||
        group.byte_count == 0) return;
    const size_t first_word = static_cast<size_t>(group.first_register) * 4;
    if (first_word >= stage.local_parameters.size()) return;
    const size_t words = std::min<size_t>(group.byte_count / 4,
                                         stage.local_parameters.size() - first_word);
    // All metadata groups are ranges of four-word NV local registers. Even
    // scalar booleans occupy one register each: original NVFP reads c[n].x.
    // glUniform1iv instead consumes consecutive scalar words, so bools need
    // gathering before upload (c[0].y is not the next logical boolean).
    constexpr size_t width = 4;
    const GLsizei count = static_cast<GLsizei>(
        std::min<size_t>(words / width, static_cast<size_t>(uniforms.size)));
    if (count == 0) return;
    const uint32_t* data = stage.local_parameters.data() + first_word;
    auto& previous = pipeline.uploaded_constants[group_index];
    if (type == LegacyConstantType::Bool) {
        bool unchanged = pipeline.uploaded_counts[group_index] == count &&
                         previous.size() == static_cast<size_t>(count);
        for (GLsizei index = 0; unchanged && index < count; ++index) {
            unchanged = previous[index] == static_cast<uint32_t>(data[index * width] != 0);
        }
        if (unchanged) return;
        previous.resize(static_cast<size_t>(count));
        for (GLsizei index = 0; index < count; ++index) {
            previous[index] = static_cast<uint32_t>(data[index * width] != 0);
        }
        glUniform1iv(uniforms.location, count,
                     reinterpret_cast<const GLint*>(previous.data()));
        pipeline.uploaded_counts[group_index] = count;
        return;
    }
    const size_t uploaded_words = static_cast<size_t>(count) * width;
    if (pipeline.uploaded_counts[group_index] == count &&
        previous.size() == uploaded_words &&
        std::memcmp(previous.data(), data,
                    uploaded_words * sizeof(uint32_t)) == 0) {
        return;
    }
    switch (type) {
        case LegacyConstantType::Float4:
            glUniform4fv(uniforms.location, count, reinterpret_cast<const GLfloat*>(data));
            break;
        case LegacyConstantType::Int4:
            glUniform4iv(uniforms.location, count, reinterpret_cast<const GLint*>(data));
            break;
        case LegacyConstantType::Bool:
            // Scalar booleans were gathered and uploaded above.
            break;
    }
    previous.assign(data, data + uploaded_words);
    pipeline.uploaded_counts[group_index] = count;
}

static void BindLegacyStageTexturesLocked(const LegacyAssemblyStage& stage) {
    const bool cache_enabled = (GlStateCacheMode() & 2) != 0;
    const bool audit_enabled = GlStateCacheAuditEnabled();
    const bool track_bindings = cache_enabled || audit_enabled;
    if (track_bindings && s_seen_legacy_bindings_epoch !=
        s_legacy_bindings_epoch.load(std::memory_order_acquire)) {
        InvalidateLegacyTextureCache();
    }
    GLint previous_active = GL_TEXTURE0;
    bool changed_active = false;
    for (size_t unit = 0; unit < stage.sampler_registers.size(); ++unit) {
        const int register_index = stage.sampler_registers[unit];
        const GLenum texture_target = stage.sampler_targets[unit];
        if (register_index < 0 || !texture_target) continue;
        const size_t word = static_cast<size_t>(register_index) * 4;
        if (word + 1 >= stage.local_parameters.size()) continue;
        const uint64_t handle =
            static_cast<uint64_t>(stage.local_parameters[word]) |
            (static_cast<uint64_t>(stage.local_parameters[word + 1]) << 32);
        const auto resolved = s_legacy_texture_handles.find(handle);
        if (resolved == s_legacy_texture_handles.end()) continue;
        if (!track_bindings) {
            if (!changed_active) {
                glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
                changed_active = true;
            }
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glBindTexture(texture_target, resolved->second.texture);
            glBindSampler(static_cast<GLuint>(unit), resolved->second.sampler);
            continue;
        }
        auto& bound = s_legacy_unit_bindings[unit];
        const bool same_texture = bound.texture_valid &&
            bound.target == texture_target &&
            bound.texture == resolved->second.texture;
        const bool same_sampler = bound.sampler_valid &&
            bound.sampler == resolved->second.sampler;
        const bool skip_texture = cache_enabled && same_texture;
        const bool skip_sampler = cache_enabled && same_sampler;
        if (audit_enabled) {
            s_gl_texture_attempts.fetch_add(1, std::memory_order_relaxed);
            s_gl_sampler_attempts.fetch_add(1, std::memory_order_relaxed);
            if (same_texture) s_gl_texture_redundant.fetch_add(1, std::memory_order_relaxed);
            if (same_sampler) s_gl_sampler_redundant.fetch_add(1, std::memory_order_relaxed);
            if (skip_texture) s_gl_texture_skipped.fetch_add(1, std::memory_order_relaxed);
            if (skip_sampler) s_gl_sampler_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        if (!skip_texture || !skip_sampler) {
            if (!changed_active) {
                glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
                changed_active = true;
            }
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            if (!skip_texture) glBindTexture(texture_target, resolved->second.texture);
            if (!skip_sampler) glBindSampler(static_cast<GLuint>(unit), resolved->second.sampler);
        }
        bound.target = texture_target;
        bound.texture = resolved->second.texture;
        bound.sampler = resolved->second.sampler;
        bound.texture_valid = true;
        bound.sampler_valid = true;
    }
    if (changed_active) glActiveTexture(static_cast<GLenum>(previous_active));
}

static void UploadLegacyPipelineStateLocked(
    LegacyAssemblyPipeline& pipeline,
    const LegacyAssemblyStage& vertex,
    const LegacyAssemblyStage& fragment) {
    // The 9 metadata pairs map logical UE3 constant banks to *packed* NV
    // local registers. SetProgramLocals/sub_1E331A0 subtracts each logical
    // bank's offset then adds this per-shader register base. Raw D3D c249/c217
    // are NOT NV register indices. Vertex bank2 is the separately declared
    // VConstBones array, not an offset into VConstFloat.
    UploadLegacyConstantGroup(pipeline, vertex, pipeline.vertex_global, 0, LegacyConstantType::Float4);
    UploadLegacyConstantGroup(pipeline, vertex, pipeline.vertex_float, 1, LegacyConstantType::Float4);
    UploadLegacyConstantGroup(pipeline, vertex, pipeline.vertex_bones, 2, LegacyConstantType::Float4);
    UploadLegacyConstantGroup(pipeline, vertex, pipeline.vertex_int, 3, LegacyConstantType::Int4);
    UploadLegacyConstantGroup(pipeline, vertex, pipeline.vertex_bool, 4, LegacyConstantType::Bool);
    UploadLegacyConstantGroup(pipeline, fragment, pipeline.fragment_global, 5, LegacyConstantType::Float4);
    UploadLegacyConstantGroup(pipeline, fragment, pipeline.fragment_float, 6, LegacyConstantType::Float4);
    UploadLegacyConstantGroup(pipeline, fragment, pipeline.fragment_int, 7, LegacyConstantType::Int4);
    UploadLegacyConstantGroup(pipeline, fragment, pipeline.fragment_bool, 8, LegacyConstantType::Bool);
    // Texture-unit assignments belong to the private linked program and do
    // not change when textures/constant registers are rebound between draws.
    if (!pipeline.samplers_initialized) {
        ConfigureLegacySamplers(pipeline.program);
        pipeline.samplers_initialized = true;
    }
    BindLegacyStageTexturesLocked(vertex);
    BindLegacyStageTexturesLocked(fragment);
}

static void EraseLegacyPipelinesForStageLocked(GLuint stage_name) {
    for (auto it = s_legacy_pipelines.begin(); it != s_legacy_pipelines.end();) {
        const GLuint vertex = static_cast<GLuint>(it->first >> 32);
        const GLuint fragment = static_cast<GLuint>(it->first);
        if (vertex == stage_name || fragment == stage_name) {
            if (it->second.program) glDeleteProgram(it->second.program);
            it = s_legacy_pipelines.erase(it);
        } else {
            ++it;
        }
    }
}

static LegacyAssemblyPipeline* GetLegacyPipelineLocked(GLuint vertex_name,
                                                        GLuint fragment_name) {
    auto vertex = s_legacy_stages.find(vertex_name);
    auto fragment = s_legacy_stages.find(fragment_name);
    if (vertex == s_legacy_stages.end() || fragment == s_legacy_stages.end() ||
        vertex->second.target != kVertexProgramArb ||
        fragment->second.target != kFragmentProgramArb ||
        !vertex->second.shader || !fragment->second.shader) {
        return nullptr;
    }

    const uint64_t key = LegacyPipelineKey(vertex_name, fragment_name);
    auto existing = s_legacy_pipelines.find(key);
    if (existing != s_legacy_pipelines.end()) return &existing->second;

    LegacyAssemblyPipeline pipeline;
    pipeline.program = glCreateProgram();
    glAttachShader(pipeline.program, vertex->second.shader);
    glAttachShader(pipeline.program, fragment->second.shader);
    glLinkProgram(pipeline.program);
    GLint linked = GL_FALSE;
    glGetProgramiv(pipeline.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        const std::string log = ProgramInfoLog(pipeline.program);
        LOGE("Legacy GLASM link failed vertex=%u(%s) fragment=%u(%s): %s",
             vertex_name, vertex->second.name.c_str(), fragment_name,
             fragment->second.name.c_str(), log.c_str());
        glDeleteProgram(pipeline.program);
        if (s_deferred_gl_error == GL_NO_ERROR) {
            s_deferred_gl_error = GL_INVALID_OPERATION;
        }
        return nullptr;
    }

    pipeline.vertex_float = FindLegacyUniformArray(pipeline.program,
                                                    "VConstFloat");
    pipeline.fragment_float = FindLegacyUniformArray(pipeline.program,
                                                      "PConstFloat");
    pipeline.vertex_global = FindLegacyUniformArray(pipeline.program,
                                                     "VConstGlobal");
    pipeline.fragment_global = FindLegacyUniformArray(pipeline.program,
                                                       "PConstGlobal");
    pipeline.vertex_bones = FindLegacyUniformArray(pipeline.program,
                                                    "VConstBones");
    pipeline.vertex_int = FindLegacyUniformArray(pipeline.program, "VConstInt");
    pipeline.fragment_int = FindLegacyUniformArray(pipeline.program, "PConstInt");
    pipeline.vertex_bool = FindLegacyUniformArray(pipeline.program, "VConstBool");
    pipeline.fragment_bool = FindLegacyUniformArray(pipeline.program, "PConstBool");
    const uint32_t log_index =
        s_legacy_link_logs.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 64) {
        LOGI("Legacy GLASM linked #%u host=%u vertex=%u(%s) fragment=%u(%s) "
             "float_uniforms=%d/%d global_uniforms=%d/%d",
             log_index + 1, pipeline.program, vertex_name,
             vertex->second.name.c_str(), fragment_name,
             fragment->second.name.c_str(), pipeline.vertex_float.size,
             pipeline.fragment_float.size, pipeline.vertex_global.size,
             pipeline.fragment_global.size);
    }
    auto [inserted, unused] =
        s_legacy_pipelines.emplace(key, std::move(pipeline));
    return &inserted->second;
}

static void BindLegacyProgram(GLenum target, GLuint name) {
    if (target == kVertexProgramArb) s_bound_legacy_vertex = name;
    else if (target == kFragmentProgramArb) s_bound_legacy_fragment = name;
    else {
        if (s_deferred_gl_error == GL_NO_ERROR) s_deferred_gl_error = GL_INVALID_ENUM;
        return;
    }

    if (!s_bound_legacy_vertex || !s_bound_legacy_fragment) {
        glUseProgram(0);
        s_current_program = 0;
        s_program_cache_valid = false;
        return;
    }

    // Vertex and fragment stages are bound separately. Linking here combines
    // a new stage with the previous draw's other stage and can manufacture
    // invalid varying interfaces. Resolve the final pair immediately at draw.
}

static void RefreshBoundLegacyConstants(GLuint) {
    // Setters have already updated the stage's shadow constants/handles under
    // the program mutex. Draw-time upload sees them without linking transient
    // stage combinations or requiring an active program during updates.
}

bool RefreshLegacyProgramStateForDraw() {
    s_current_gfx_text = false;
    s_current_postprocess_blend = false;
    if (!s_bound_legacy_vertex && !s_bound_legacy_fragment) return true;

    std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
    if (SceneAuditEnabled()) SceneAuditLogError("before legacy state");
    LegacyAssemblyPipeline* pipeline = GetLegacyPipelineLocked(
        s_bound_legacy_vertex, s_bound_legacy_fragment);
    if (!pipeline) {
        // Never draw with a previous pipeline after this pair failed. Apart
        // from rendering the wrong effect, the old program's sampler bindings
        // can be incompatible with the new framebuffer/textures on the driver.
        glUseProgram(0);
        s_current_program = 0;
        s_program_cache_valid = false;
        if (s_deferred_gl_error == GL_NO_ERROR) {
            s_deferred_gl_error = GL_INVALID_OPERATION;
        }
        LOGE("Legacy draw rejected: no valid pipeline vertex=%u fragment=%u",
             s_bound_legacy_vertex, s_bound_legacy_fragment);
        return false;
    }
    const auto vertex = s_legacy_stages.find(s_bound_legacy_vertex);
    const auto fragment = s_legacy_stages.find(s_bound_legacy_fragment);
    if (vertex == s_legacy_stages.end() ||
        fragment == s_legacy_stages.end()) {
        return false;
    }
    bool skip_program = false;
    const bool cache_program = (GlStateCacheMode() & 1) != 0;
    const bool audit_program = GlStateCacheAuditEnabled();
    if (cache_program || audit_program) {
        const bool same_program = s_program_cache_valid &&
            s_current_program == pipeline->program;
        skip_program = cache_program && same_program;
        if (audit_program) {
            s_gl_program_attempts.fetch_add(1, std::memory_order_relaxed);
            if (same_program) s_gl_program_redundant.fetch_add(1, std::memory_order_relaxed);
            if (skip_program) s_gl_program_skipped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!skip_program) glUseProgram(pipeline->program);
    s_current_program = pipeline->program;
    s_program_cache_valid = true;
    const bool gfx_text = vertex->second.name.starts_with("GFxVertexShader") &&
        vertex->second.name.find("_text") != std::string::npos &&
        fragment->second.name.starts_with("GFxPixelShaderbase_text");
    static const bool align_all_gfx = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.horn.gfx_align_all", value) > 0 &&
               value[0] == '1';
    }();
    s_current_gfx_text = gfx_text ||
        (align_all_gfx && vertex->second.name.starts_with("GFxVertexShader") &&
         fragment->second.name.starts_with("GFxPixelShader"));
    s_current_postprocess_blend =
        vertex->second.name == "UberPostProcessVertexShader" &&
        fragment->second.name == "UberPostProcessBlendPixelShader";
    if (SceneAuditEnabled() &&
        ((s_bound_legacy_fragment >= 1610612847u &&
          s_bound_legacy_fragment <= 1610612852u) ||
         s_bound_legacy_fragment == 1610612748u)) {
        static std::unordered_set<GLuint> dumped;
        if (dumped.insert(s_bound_legacy_fragment).second) {
            for (const auto& [stage, label] :
                 {std::pair{&vertex->second, "vertex"},
                  std::pair{&fragment->second, "fragment"}}) {
                const std::string path =
                    "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/" +
                    std::string("scene-ground-") +
                    std::to_string(s_bound_legacy_fragment) + "-" + label +
                    ".glsl";
                FILE* file = std::fopen(path.c_str(), "wb");
                if (!file) continue;
                const size_t written = std::fwrite(
                    stage->diagnostic_source.data(), 1,
                    stage->diagnostic_source.size(), file);
                const int close_result = std::fclose(file);
                LOGI("Scene ground shader fragment=%u stage=%s bytes=%zu "
                     "written=%zu close=%d path=%s",
                     s_bound_legacy_fragment, label,
                     stage->diagnostic_source.size(), written, close_result,
                     path.c_str());
            }
        }
    }
    const EGLContext context = eglGetCurrentContext();
    if (pipeline->uniforms_context != context) {
        // An EGL context change may be a share-group transition or recreation.
        // In either case re-upload once instead of trusting the old GL state.
        pipeline->uploaded_counts.fill(0);
        pipeline->uniforms_context = context;
    }
    if (SceneAuditEnabled() && vertex->second.name.starts_with("GFx") &&
        fragment->second.name.starts_with("GFx")) {
        s_scene_gfx_programs.insert(pipeline->program);
        if (vertex->second.name.ends_with("Alt0")) {
            s_scene_gfx_perspective_programs.insert(pipeline->program);
        }
    }
    if (SceneAuditEnabled() &&
        fragment->second.name == "UberPostProcessBlendPixelShader") {
        s_scene_final_program = pipeline->program;
    }
    UploadLegacyPipelineStateLocked(*pipeline, vertex->second,
                                    fragment->second);
    if (ModelAuditEnabled()) {
        const auto& bones = vertex->second.constant_groups[2];
        if (bones.first_register >= 0 && bones.byte_count >= 16) {
            static std::atomic<uint32_t> model_draws{0};
            const uint32_t index = model_draws.fetch_add(1, std::memory_order_relaxed);
            if (index < 48) {
                GLint framebuffer = 0;
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
                LOGI("Model audit draw #%u host=%u vertex=%u(%s) fragment=%u(%s) "
                     "fbo=%d bone_first=%d bone_bytes=%u local_words=%zu "
                     "uniform_location=%d uniform_size=%d",
                     index + 1, pipeline->program, s_bound_legacy_vertex,
                     vertex->second.name.c_str(), s_bound_legacy_fragment,
                     fragment->second.name.c_str(), framebuffer,
                     bones.first_register, bones.byte_count,
                     vertex->second.local_parameters.size(),
                     pipeline->vertex_bones.location, pipeline->vertex_bones.size);
                if (index < 12 && pipeline->vertex_bones.location >= 0) {
                    const size_t word = static_cast<size_t>(bones.first_register) * 4;
                    if (word + 4 <= vertex->second.local_parameters.size()) {
                        float guest[4] = {};
                        GLfloat driver[4] = {};
                        std::memcpy(guest, vertex->second.local_parameters.data() + word,
                                    sizeof(guest));
                        glGetUniformfv(pipeline->program, pipeline->vertex_bones.location,
                                       driver);
                        LOGI("Model audit bone0 host=%u guest={%.7g,%.7g,%.7g,%.7g} "
                             "driver={%.7g,%.7g,%.7g,%.7g}", pipeline->program,
                             guest[0], guest[1], guest[2], guest[3],
                             driver[0], driver[1], driver[2], driver[3]);
                    }
                }
            }
        }
    }
    if (fragment->second.name == "UberPostProcessBlendPixelShader") {
        static const bool input_readback_probe = [] {
            char value[PROP_VALUE_MAX] = {};
            return __system_property_get("debug.horn.scene_input_readback", value) > 0 &&
                   value[0] == '1';
        }();
        static thread_local std::unordered_map<GLuint, uint32_t> draw_counts;
        const uint32_t occurrence = input_readback_probe
            ? ++draw_counts[pipeline->program] : 0;
        GLint current_draw_framebuffer = 0;
        if (input_readback_probe) {
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,
                          &current_draw_framebuffer);
        }
        if (input_readback_probe && current_draw_framebuffer == 6 &&
            (occurrence == 1 || occurrence == 16 || occurrence == 32 ||
             occurrence == 48 || occurrence == 64)) {
            GLint old_active = GL_TEXTURE0;
            GLint old_read_framebuffer = 0;
            GLint old_read_buffer = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_framebuffer);
            glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
            glActiveTexture(GL_TEXTURE0);
            GLint source_texture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &source_texture);
            glActiveTexture(static_cast<GLenum>(old_active));
            GLuint temporary_framebuffer = 0;
            glGenFramebuffers(1, &temporary_framebuffer);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, temporary_framebuffer);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D,
                                   static_cast<GLuint>(source_texture), 0);
            const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            LOGI("Scene final-input readback host=%u occurrence=%u texture=%d "
                 "temporary_fbo=%u status=0x%04X", pipeline->program,
                 occurrence, source_texture, temporary_framebuffer, status);
            if (status == GL_FRAMEBUFFER_COMPLETE) {
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                SceneAuditLogFramebufferPixels("final-input", 40,
                                               0, 0, 896, 512);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              static_cast<GLuint>(old_read_framebuffer));
            glReadBuffer(static_cast<GLenum>(old_read_buffer));
            glDeleteFramebuffers(1, &temporary_framebuffer);
            SceneAuditLogError("final-input readback");
        }
    }
    if (SceneAuditEnabled()) {
        SceneAuditLogError("legacy state");
        SceneAuditNoteDraw(pipeline->program);
        static thread_local std::unordered_set<GLuint> logged_programs;
        if (logged_programs.size() < 256 &&
            logged_programs.insert(pipeline->program).second) {
            LOGI("Scene pipeline host=%u vertex=%u(%s) fragment=%u(%s) "
                 "Vglobal=%d Vfloat=%d Pglobal=%d Pfloat=%d",
                 pipeline->program, s_bound_legacy_vertex,
                 vertex->second.name.c_str(), s_bound_legacy_fragment,
                 fragment->second.name.c_str(), pipeline->vertex_global.size,
                 pipeline->vertex_float.size, pipeline->fragment_global.size,
                 pipeline->fragment_float.size);
            if (fragment->second.name == "UberPostProcessBlendPixelShader") {
                const GLint sampler_location = glGetUniformLocation(
                    pipeline->program, "PSampler0");
                GLint sampler_uniform = -1;
                if (sampler_location >= 0) {
                    glGetUniformiv(pipeline->program, sampler_location,
                                   &sampler_uniform);
                }
                GLboolean blend = GL_FALSE, depth = GL_FALSE;
                GLboolean stencil = GL_FALSE, scissor = GL_FALSE;
                glGetBooleanv(GL_BLEND, &blend);
                glGetBooleanv(GL_DEPTH_TEST, &depth);
                glGetBooleanv(GL_STENCIL_TEST, &stencil);
                glGetBooleanv(GL_SCISSOR_TEST, &scissor);
                LOGI("Scene final state host=%u PSampler0 location=%d unit=%d "
                     "blend=%d depth=%d stencil=%d scissor=%d",
                     pipeline->program, sampler_location, sampler_uniform,
                     blend, depth, stencil, scissor);
                GLint vao = 0;
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
                for (GLuint attribute : {0u, 8u}) {
                    GLint enabled = 0, size = 0, type = 0, normalized = 0;
                    GLint stride = 0, buffer = 0;
                    void* pointer = nullptr;
                    GLfloat current[4] = {};
                    glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                                        &enabled);
                    glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_SIZE,
                                        &size);
                    glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                                        &type);
                    glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                                        &normalized);
                    glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                                        &stride);
                    glGetVertexAttribiv(attribute,
                                        GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                                        &buffer);
                    glGetVertexAttribPointerv(attribute,
                                              GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                              &pointer);
                    glGetVertexAttribfv(attribute, GL_CURRENT_VERTEX_ATTRIB,
                                        current);
                    LOGI("Scene quad host=%u vao=%d attribute=%u enabled=%d "
                         "size=%d type=0x%04X normalized=%d stride=%d "
                         "buffer=%d pointer=0x%llX current={%.6g,%.6g,%.6g,%.6g}",
                         pipeline->program, vao, attribute, enabled, size,
                         type, normalized, stride, buffer,
                         static_cast<unsigned long long>(
                             reinterpret_cast<uintptr_t>(pointer)),
                         current[0], current[1], current[2], current[3]);
                    if (!enabled || buffer <= 0 || type != GL_FLOAT ||
                        size < 2 || size > 4 || stride < 0) continue;
                    const size_t element_stride = stride == 0
                        ? static_cast<size_t>(size) * sizeof(float)
                        : static_cast<size_t>(stride);
                    for (size_t item : {size_t(0), size_t(1), size_t(2),
                                        size_t(3), size_t(88), size_t(89),
                                        size_t(90), size_t(91)}) {
                        const size_t offset =
                            reinterpret_cast<uintptr_t>(pointer) +
                            item * element_stride;
                        GLfloat values[4] = {};
                        const bool shadow = CopyGlBufferShadow(
                            static_cast<GLuint>(buffer), offset,
                            static_cast<size_t>(size) * sizeof(float), values);
                        LOGI("Scene quad host=%u attribute=%u vertex=%zu "
                             "shadow=%d offset=%zu values={%.6g,%.6g,%.6g,%.6g}",
                             pipeline->program, attribute, item, shadow,
                             offset, values[0], values[1], values[2], values[3]);
                    }
                }
            }
            const auto log_samplers = [&](const LegacyAssemblyStage& stage,
                                          char prefix) {
                for (size_t unit = 0; unit < stage.sampler_registers.size(); ++unit) {
                    const int register_index = stage.sampler_registers[unit];
                    if (register_index < 0 || !stage.sampler_targets[unit]) continue;
                    const size_t word = static_cast<size_t>(register_index) * 4;
                    if (word + 1 >= stage.local_parameters.size()) {
                        LOGI("Scene sampler host=%u stage=%c unit=%zu reg=%d "
                             "target=0x%04X local_range_missing=%zu",
                             pipeline->program, prefix, unit, register_index,
                             stage.sampler_targets[unit],
                             stage.local_parameters.size());
                        continue;
                    }
                    const uint64_t handle =
                        static_cast<uint64_t>(stage.local_parameters[word]) |
                        (static_cast<uint64_t>(stage.local_parameters[word + 1]) << 32);
                    const auto found = s_legacy_texture_handles.find(handle);
                    GLint actual_texture = 0, width = 0, height = 0, format = 0;
                    if (stage.sampler_targets[unit] == GL_TEXTURE_2D &&
                        found != s_legacy_texture_handles.end()) {
                        GLint previous_active = 0;
                        glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
                        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
                        glGetIntegerv(GL_TEXTURE_BINDING_2D, &actual_texture);
                        if (actual_texture > 0 &&
                            static_cast<GLuint>(actual_texture) == found->second.texture) {
                            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                GL_TEXTURE_WIDTH, &width);
                            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                GL_TEXTURE_HEIGHT, &height);
                            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                GL_TEXTURE_INTERNAL_FORMAT, &format);
                        }
                        glActiveTexture(static_cast<GLenum>(previous_active));
                    }
                    LOGI("Scene sampler host=%u stage=%c unit=%zu reg=%d "
                         "target=0x%04X handle=0x%016llX resolved=%d "
                         "texture=%u sampler=%u resident=%d bound=%d "
                         "size0=%dx%d format=0x%04X",
                         pipeline->program, prefix, unit, register_index,
                         stage.sampler_targets[unit],
                         static_cast<unsigned long long>(handle),
                         found != s_legacy_texture_handles.end(),
                         found == s_legacy_texture_handles.end() ? 0 : found->second.texture,
                         found == s_legacy_texture_handles.end() ? 0 : found->second.sampler,
                         found != s_legacy_texture_handles.end() && found->second.resident,
                         actual_texture, width, height, format);
                }
            };
            log_samplers(vertex->second, 'V');
            log_samplers(fragment->second, 'P');
        }
        GLint draw_framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
        if (draw_framebuffer == 6 || draw_framebuffer == 23 ||
            draw_framebuffer == 24 ||
            (draw_framebuffer == 8 && pipeline->program == 258)) {
            if (draw_framebuffer == 8 && pipeline->program == 258) {
                GLint depth_func = 0;
                GLint depth_mask = 0;
                GLint blend_src = 0;
                GLint blend_dst = 0;
                GLint viewport[4] = {};
                glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
                glGetIntegerv(GL_DEPTH_WRITEMASK, &depth_mask);
                glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src);
                glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst);
                glGetIntegerv(GL_VIEWPORT, viewport);
                LOGI("Scene basepass258 state depth=%d func=0x%04X mask=%d "
                     "blend=%d src=0x%04X dst=0x%04X stencil=%d scissor=%d "
                     "viewport=%d,%d %dx%d",
                     glIsEnabled(GL_DEPTH_TEST), depth_func, depth_mask,
                     glIsEnabled(GL_BLEND), blend_src, blend_dst,
                     glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_SCISSOR_TEST),
                     viewport[0], viewport[1], viewport[2], viewport[3]);
            }
            static thread_local std::unordered_set<GLuint> dumped_programs;
            if (dumped_programs.size() < 12 &&
                dumped_programs.insert(pipeline->program).second) {
                const auto dump_uniforms = [&](const LegacyAssemblyStage& stage,
                                               const char* uniform_name,
                                               size_t group_index,
                                               GLsizei count) {
                    const auto& group = stage.constant_groups[group_index];
                    LOGI("Scene constants host=%u uniform=%s group=%zu first=%d "
                         "bytes=%u local_words=%zu active=%d",
                         pipeline->program, uniform_name, group_index,
                         group.first_register, group.byte_count,
                         stage.local_parameters.size(), count);
                    const GLsizei shown = std::min<GLsizei>(count, 8);
                    for (GLsizei item = 0; item < shown; ++item) {
                        const std::string name = std::string(uniform_name) +
                            "[" + std::to_string(item) + "]";
                        const GLint location = glGetUniformLocation(
                            pipeline->program, name.c_str());
                        if (location < 0) continue;
                        GLfloat actual[4] = {};
                        glGetUniformfv(pipeline->program, location, actual);
                        GLfloat guest[4] = {};
                        if (group.first_register >= 0 &&
                            static_cast<size_t>(item) * 16 < group.byte_count) {
                            const size_t word =
                                static_cast<size_t>(group.first_register + item) * 4;
                            if (word + 4 <= stage.local_parameters.size()) {
                                std::memcpy(guest,
                                            stage.local_parameters.data() + word,
                                            sizeof(guest));
                            }
                        }
                        LOGI("Scene uniform host=%u %s guest={%.6g,%.6g,%.6g,%.6g} "
                             "driver={%.6g,%.6g,%.6g,%.6g}",
                             pipeline->program, name.c_str(), guest[0], guest[1],
                             guest[2], guest[3], actual[0], actual[1],
                             actual[2], actual[3]);
                    }
                };
                dump_uniforms(vertex->second, "VConstFloat", 1,
                              pipeline->vertex_float.size);
                dump_uniforms(fragment->second, "PConstFloat", 6,
                              pipeline->fragment_float.size);

                GLint previous_active = GL_TEXTURE0;
                glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
                for (size_t unit = 0; unit < 5; ++unit) {
                    if (fragment->second.sampler_targets[unit] != GL_TEXTURE_2D) continue;
                    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
                    GLint binding = 0;
                    GLint sampler = 0;
                    GLint width0 = 0;
                    GLint height0 = 0;
                    GLint width1 = 0;
                    GLint format = 0;
                    GLint base = 0;
                    GLint maximum = 0;
                    GLint immutable_levels = 0;
                    GLint texture_min = 0;
                    GLint sampler_min = 0;
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
                    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
                    if (binding != 0) {
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width0);
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height0);
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
                        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &width1);
                        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
                        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maximum);
                        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS,
                                            &immutable_levels);
                        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                            &texture_min);
                    }
                    if (sampler != 0) {
                        glGetSamplerParameteriv(static_cast<GLuint>(sampler),
                                                GL_TEXTURE_MIN_FILTER, &sampler_min);
                    }
                    LOGI("Scene texture host=%u unit=%zu binding=%d sampler=%d "
                         "size0=%dx%d size1width=%d format=0x%04X base=%d max=%d "
                         "immutable_levels=%d texture_min=0x%04X sampler_min=0x%04X",
                         pipeline->program, unit, binding, sampler, width0,
                         height0, width1, format, base, maximum,
                         immutable_levels, texture_min, sampler_min);
                }
                glActiveTexture(static_cast<GLenum>(previous_active));
                const auto dump_stage = [&](const LegacyAssemblyStage& stage,
                                            const char* label) {
                    if (stage.name != "UberPostProcessBlendPixelShader" &&
                        pipeline->program != 258) return;
                    size_t begin = 0;
                    LOGI("Scene GLSL source host=%u stage=%s shader=%s bytes=%zu",
                         pipeline->program, label, stage.name.c_str(),
                         stage.diagnostic_source.size());
                    for (size_t line = 1;
                         begin < stage.diagnostic_source.size() &&
                         begin < 131072 && line <= 1200; ++line) {
                        const size_t next = stage.diagnostic_source.find('\n', begin);
                        const size_t end = next == std::string::npos
                            ? stage.diagnostic_source.size() : next;
                        if (line >= 90 || pipeline->program == 258) {
                            LOGI("Scene GLSL host=%u stage=%s shader=%s line=%zu: %.*s",
                                 pipeline->program, label, stage.name.c_str(),
                                 line,
                                 static_cast<int>(std::min<size_t>(end - begin, 1024)),
                                 stage.diagnostic_source.data() + begin);
                        }
                        begin = end == stage.diagnostic_source.size() ? end : end + 1;
                    }
                };
                dump_stage(vertex->second, "vertex");
                dump_stage(fragment->second, "fragment");
            }
        }
    }
    return true;
}

bool CurrentLegacyPipelineIsGfxText() {
    return s_current_gfx_text;
}

bool CurrentLegacyPipelineIsPostprocessBlend() {
    return s_current_postprocess_blend;
}

// Read the actual render-target input of a captured lighting draw. Keep the
// guest's framebuffer and pack state intact; this runs only in scene audit.
static void SceneAuditCaptureLightingTexture(GLuint texture, GLint width,
                                             GLint height,
                                             const std::string& prefix) {
    if (!texture || width <= 0 || height <= 0 || width > 4096 || height > 4096) return;
    GLint old_read = 0, old_buffer = 0, old_pack = 0, old_alignment = 0;
    GLint old_row = 0, old_skip_pixels = 0, old_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_READ_BUFFER, &old_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_skip_rows);
    PreserveSceneAuditGlError(glGetError());
    GLuint temporary = 0;
    glGenFramebuffers(1, &temporary);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, temporary);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const GLenum error = glGetError();
        PreserveSceneAuditGlError(error);
        if (error == GL_NO_ERROR) {
            const std::string path = prefix + "-input-" + std::to_string(width) +
                "x" + std::to_string(height) + ".rgba8";
            if (FILE* file = std::fopen(path.c_str(), "wb")) {
                const size_t written = std::fwrite(pixels.data(), 1, pixels.size(), file);
                const int closed = std::fclose(file);
                LOGI("Scene lighting input texture=%u size=%dx%d bytes=%zu "
                     "written=%zu close=%d path=%s", texture, width, height,
                     pixels.size(), written, closed, path.c_str());
            }
        } else {
            LOGE("Scene lighting input texture=%u read_error=0x%04X", texture, error);
        }
    } else {
        LOGE("Scene lighting input texture=%u framebuffer_status=0x%04X", texture, status);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_skip_rows);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(old_pack));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    glReadBuffer(static_cast<GLenum>(old_buffer));
    glDeleteFramebuffers(1, &temporary);
    PreserveSceneAuditGlError(glGetError());
}

void SceneAuditCaptureLightingState(uint32_t draw, size_t point) {
    if (!SceneAuditEnabled()) return;
    std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
    const auto vertex = s_legacy_stages.find(s_bound_legacy_vertex);
    const auto fragment = s_legacy_stages.find(s_bound_legacy_fragment);
    if (vertex == s_legacy_stages.end() || fragment == s_legacy_stages.end() ||
        (fragment->second.name != "DirectionalLightPixelShader" &&
         fragment->second.name != "BasePassPixelShader" &&
         fragment->second.name != "HeightFogPixelShader")) return;
    const bool fog = fragment->second.name == "HeightFogPixelShader";
    static thread_local std::unordered_set<GLuint> captured_programs;
    static thread_local std::unordered_set<uint32_t> captured_draws;
    static thread_local std::unordered_set<uint32_t> captured_fog_draws;
    // A program is reused with different mesh inputs. Rock samples need the
    // state at their actual draw even if this program was seen on the plank.
    // Give fog its own bounded budget so opaque geometry cannot exhaust it.
    if (fog) {
        if (captured_fog_draws.size() >= 4 ||
            !captured_fog_draws.insert(draw).second) return;
    } else if (captured_draws.size() >= 24 ||
               (point < 9 && captured_programs.contains(s_current_program)) ||
               !captured_draws.insert(draw).second) return;
    captured_programs.insert(s_current_program);
    const std::string prefix =
        "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/scene-lighting-draw" +
        std::to_string(draw) + "-program" + std::to_string(s_current_program);
    const auto write_file = [](const std::string& path, const void* data, size_t size) {
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return;
        const size_t written = std::fwrite(data, 1, size, file);
        const int closed = std::fclose(file);
        LOGI("Scene lighting file bytes=%zu written=%zu close=%d path=%s",
             size, written, closed, path.c_str());
    };
    LOGI("Scene lighting capture draw=%u point=%zu host=%u vertex=%u fragment=%u",
         draw, point, s_current_program, s_bound_legacy_vertex, s_bound_legacy_fragment);
    // Query at this draw: once-per-program bindings can describe an earlier
    // pass. This also detects sampling an image attached to the draw FBO.
    GLint framebuffer = 0, color_type = 0, color_object = 0, color_level = -1;
    GLint src_rgb = 0, dst_rgb = 0, src_alpha = 0, dst_alpha = 0;
    GLint blend_rgb = 0, blend_alpha = 0, depth_func = 0, viewport[4] = {};
    GLboolean color_mask[4] = {}, depth_mask = GL_FALSE;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &color_type);
        if (color_type != GL_NONE) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &color_object);
        }
        if (color_type == GL_TEXTURE) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &color_level);
        }
    }
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_alpha);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    LOGI("Scene lighting state draw=%u host=%u name=%s fbo=%d "
         "colorType=0x%X colorObject=%d colorLevel=%d viewport=%d,%d,%d,%d "
         "blend=%d blendRGB=0x%X/0x%X/0x%X blendA=0x%X/0x%X/0x%X "
         "colorMask=%d,%d,%d,%d depth=%d depthFunc=0x%X depthMask=%d",
         draw, s_current_program, fragment->second.name.c_str(), framebuffer,
         color_type, color_object, color_level,
         viewport[0], viewport[1], viewport[2], viewport[3],
         glIsEnabled(GL_BLEND), src_rgb, dst_rgb, blend_rgb,
         src_alpha, dst_alpha, blend_alpha,
         color_mask[0], color_mask[1], color_mask[2], color_mask[3],
         glIsEnabled(GL_DEPTH_TEST), depth_func, depth_mask);
    for (const auto& [stage, label] :
         {std::pair{&vertex->second, "vertex"}, std::pair{&fragment->second, "fragment"}}) {
        const std::string base = prefix + "-" + label;
        std::string source = stage->diagnostic_source;
        if (source.empty() && stage->shader) {
            GLint length = 0;
            glGetShaderiv(stage->shader, GL_SHADER_SOURCE_LENGTH, &length);
            if (length > 1 && length <= static_cast<GLint>(kMaxLegacyShaderSource)) {
                source.resize(static_cast<size_t>(length));
                GLsizei written = 0;
                glGetShaderSource(stage->shader, length, &written, source.data());
                source.resize(static_cast<size_t>(std::max(written, 0)));
            }
        }
        write_file(base + ".glsl", source.data(), source.size());
        write_file(base + ".locals.bin", stage->local_parameters.data(),
                   stage->local_parameters.size() * sizeof(uint32_t));
        std::string metadata = "name=" + stage->name + "\n";
        for (size_t group = 0; group < stage->constant_groups.size(); ++group) {
            metadata += "group=" + std::to_string(group) + " first=" +
                std::to_string(stage->constant_groups[group].first_register) +
                " bytes=" + std::to_string(stage->constant_groups[group].byte_count) + "\n";
        }
        for (size_t unit = 0; unit < stage->sampler_registers.size(); ++unit) {
            if (stage->sampler_registers[unit] < 0) continue;
            metadata += "sampler_unit=" + std::to_string(unit) + " register=" +
                std::to_string(stage->sampler_registers[unit]) + " target=" +
                std::to_string(stage->sampler_targets[unit]) + "\n";
        }
        write_file(base + ".metadata.txt", metadata.data(), metadata.size());
        // Query active vector uniforms, independently of the upload cache.
        // Raw words make any float/int register or cache mismatch reviewable.
        constexpr const char* banks[] = {"VConstGlobal", "VConstFloat", "VConstBones",
            "VConstInt", "VConstBool", "PConstGlobal", "PConstFloat", "PConstInt", "PConstBool"};
        std::string uniforms = "bank\tindex\tregister\tlocation\texpected_words\tgpu_words\n";
        const size_t first_group = stage->target == kVertexProgramArb ? 0 : 5;
        const size_t end_group = stage->target == kVertexProgramArb ? 4 : 8;
        for (size_t g = first_group; g < end_group; ++g) {
            const auto& constant = stage->constant_groups[g];
            if (constant.first_register < 0 || constant.byte_count == 0xFFFF) continue;
            for (size_t index = 0; index < std::min<size_t>(constant.byte_count / 16, 256); ++index) {
                const size_t word = (static_cast<size_t>(constant.first_register) + index) * 4;
                if (word + 4 > stage->local_parameters.size()) break;
                const std::string name = std::string(banks[g]) + "[" + std::to_string(index) + "]";
                const GLint location = glGetUniformLocation(s_current_program, name.c_str());
                if (location < 0) continue;
                uint32_t actual[4] = {};
                if (g == 3 || g == 7) {
                    GLint values[4] = {};
                    glGetUniformiv(s_current_program, location, values);
                    std::memcpy(actual, values, sizeof(actual));
                } else {
                    GLfloat values[4] = {};
                    glGetUniformfv(s_current_program, location, values);
                    std::memcpy(actual, values, sizeof(actual));
                }
                const uint32_t* expected = stage->local_parameters.data() + word;
                char row[256];
                std::snprintf(row, sizeof(row),
                    "%s\t%zu\t%zu\t%d\t%08X,%08X,%08X,%08X\t%08X,%08X,%08X,%08X\n",
                    banks[g], index, word / 4, location,
                    expected[0], expected[1], expected[2], expected[3],
                    actual[0], actual[1], actual[2], actual[3]);
                uniforms += row;
            }
        }
        write_file(base + ".uniforms.tsv", uniforms.data(), uniforms.size());
        // Compare the driver's booleans with the original NV scalar lanes.
        // Both representations are needed before changing register unpacking.
        const size_t bool_group = stage->target == kVertexProgramArb ? 4 : 8;
        const auto& group = stage->constant_groups[bool_group];
        if (group.first_register >= 0 && group.byte_count != 0xFFFF) {
            const char* bank = stage->target == kVertexProgramArb ? "VConstBool" : "PConstBool";
            for (size_t index = 0; index < std::min<size_t>(group.byte_count / 16, 16); ++index) {
                const size_t word = (static_cast<size_t>(group.first_register) + index) * 4;
                if (word + 4 > stage->local_parameters.size()) break;
                const std::string name = std::string(bank) + "[" + std::to_string(index) + "]";
                const GLint location = glGetUniformLocation(s_current_program, name.c_str());
                if (location < 0) continue;
                GLint actual = 0;
                glGetUniformiv(s_current_program, location, &actual);
                const uint32_t* values = stage->local_parameters.data() + word;
                LOGI("Scene lighting bool draw=%u host=%u stage=%s index=%zu "
                     "register=%zu raw={%08X,%08X,%08X,%08X} gpu=%d",
                     draw, s_current_program, label, index, word / 4,
                     values[0], values[1], values[2], values[3], actual);
            }
        }
    }
    GLint previous_active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
    for (size_t unit = 0; unit < fragment->second.sampler_registers.size(); ++unit) {
        if (fragment->second.sampler_registers[unit] < 0 ||
            fragment->second.sampler_targets[unit] != GL_TEXTURE_2D) continue;
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        GLint texture = 0, width = 0, height = 0, format = 0, swizzle[4] = {};
        GLint sampler = 0, min_filter = 0, mag_filter = 0;
        GLint base_level = 0, max_level = 0, actual_unit = -1;
        const std::string sampler_name = "PSampler" + std::to_string(unit);
        const GLint sampler_location = glGetUniformLocation(s_current_program, sampler_name.c_str());
        if (sampler_location >= 0) glGetUniformiv(s_current_program, sampler_location, &actual_unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
        if (texture > 0) {
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
            constexpr GLenum names[] = {GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                                       GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A};
            for (size_t c = 0; c < 4; ++c) glGetTexParameteriv(GL_TEXTURE_2D, names[c], &swizzle[c]);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base_level);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &max_level);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
        }
        if (sampler) {
            glGetSamplerParameteriv(static_cast<GLuint>(sampler), GL_TEXTURE_MIN_FILTER, &min_filter);
            glGetSamplerParameteriv(static_cast<GLuint>(sampler), GL_TEXTURE_MAG_FILTER, &mag_filter);
        }
        std::string hash = "unrecorded";
        if (SceneTextureHashEnabled() && texture > 0) {
            std::lock_guard<std::mutex> hash_lock(s_scene_texture_fingerprints_mutex);
            const auto fingerprint = s_scene_texture_fingerprints.find(static_cast<GLuint>(texture));
            if (fingerprint != s_scene_texture_fingerprints.end()) hash = fingerprint->second.sha256.data();
        }
        LOGI("Scene lighting sampler draw=%u host=%u unit=%zu texture=%d size=%dx%d "
             "internal=0x%04X swizzle={0x%04X,0x%04X,0x%04X,0x%04X} uploadSHA256=%s "
             "sampler=%d location=%d actualUnit=%d min=0x%X mag=0x%X base=%d max=%d "
             "sameDrawImage=%d",
             draw, s_current_program, unit, texture, width, height, format,
             swizzle[0], swizzle[1], swizzle[2], swizzle[3], hash.c_str(),
             sampler, sampler_location, actual_unit, min_filter, mag_filter, base_level, max_level,
             color_type == GL_TEXTURE && texture > 0 && texture == color_object &&
                 color_level >= base_level && color_level <= max_level);
        if (point >= 9 && unit == 0 && format == GL_RGBA8 &&
            fragment->second.name == "DirectionalLightPixelShader") {
            SceneAuditCaptureLightingTexture(static_cast<GLuint>(texture), width, height, prefix);
        }
    }
    glActiveTexture(static_cast<GLenum>(previous_active));
}

static void SceneAuditLogFinalQuadVertices(const char* operation,
                                           const GLint* vertices,
                                           GLsizei vertex_count,
                                           GLsizei instances) {
    if (!SceneAuditEnabled() ||
        (s_current_program != 258 &&
         (!s_scene_final_program ||
          s_current_program != s_scene_final_program))) return;
    static thread_local std::unordered_set<GLuint> logged_programs;
    if (logged_programs.contains(s_current_program) ||
        logged_programs.size() >= 4) return;
    logged_programs.insert(s_current_program);
    LOGI("Scene final draw operation=%s program=%u vertices={%d,%d,%d,%d} "
         "count=%d instances=%d", operation, s_current_program,
         vertices[0], vertices[1], vertices[2], vertices[3],
         vertex_count, instances);
    for (GLuint attribute : {0u, 1u, 3u, 5u, 8u}) {
        GLint enabled = 0, size = 0, type = 0, stride = 0, buffer = 0;
        void* pointer = nullptr;
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                            &buffer);
        glGetVertexAttribPointerv(attribute, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                  &pointer);
        if (!enabled || buffer <= 0 || type != GL_FLOAT || size < 2 ||
            size > 4 || stride < 0 || vertex_count <= 0) {
            LOGI("Scene final attribute=%u unavailable enabled=%d size=%d "
                 "type=0x%04X stride=%d buffer=%d", attribute, enabled,
                 size, type, stride, buffer);
            continue;
        }
        const size_t element_stride = stride == 0
            ? static_cast<size_t>(size) * sizeof(float)
            : static_cast<size_t>(stride);
        for (GLsizei vertex = 0; vertex < std::min(vertex_count, 4); ++vertex) {
            if (vertices[vertex] < 0) continue;
            const size_t element = static_cast<size_t>(vertices[vertex]);
            const size_t offset = reinterpret_cast<uintptr_t>(pointer) +
                                  element * element_stride;
            GLfloat values[4] = {};
            const bool shadow = CopyGlBufferShadow(
                static_cast<GLuint>(buffer), offset,
                static_cast<size_t>(size) * sizeof(float), values);
            LOGI("Scene final attribute=%u vertex=%zu shadow=%d offset=%zu "
                 "values={%.6g,%.6g,%.6g,%.6g}", attribute, element,
                 shadow, offset, values[0], values[1], values[2], values[3]);
        }
    }
}

void SceneAuditLogFinalQuadDraw(const char* operation, GLint first,
                                GLsizei count, GLsizei instances) {
    if (!SceneAuditEnabled() ||
        (s_current_program != 258 &&
         (!s_scene_final_program ||
          s_current_program != s_scene_final_program)) || first < 0 ||
        count <= 0) return;
    GLint vertices[4] = {first, first, first, first};
    for (GLsizei i = 1; i < std::min(count, 4); ++i) {
        vertices[i] = first + i;
    }
    SceneAuditLogFinalQuadVertices(operation, vertices,
                                   std::min(count, 4), instances);
}

static void SceneAuditLogGfxIndexedBounds(GLsizei count, GLenum type,
                                          uint32_t indices_address,
                                          GLint base_vertex) {
    // Read every vertex of a bounded UI batch. The first three indices alone
    // cannot exclude a later corrupt quad that stretches across the screen.
    if (count <= 0 || count > 8192 || !s_bound_element_buffer) {
        LOGI("GFx bounds unavailable program=%u count=%d ebo=%u",
             s_current_program, count, s_bound_element_buffer);
        return;
    }
    const size_t index_size = type == GL_UNSIGNED_BYTE ? 1 :
        type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
    GLint enabled = 0, size = 0, element_type = 0, normalized = 0;
    GLint stride = 0, buffer = 0;
    void* pointer = nullptr;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &element_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    if (!index_size || !enabled || size != 2 || buffer <= 0 || stride < 0 ||
        (element_type != GL_FLOAT && element_type != GL_SHORT)) {
        LOGI("GFx bounds unsupported program=%u indexType=0x%04X "
             "positionEnabled=%d size=%d type=0x%04X buffer=%d stride=%d",
             s_current_program, type, enabled, size, element_type, buffer,
             stride);
        return;
    }
    GLfloat rows[4][4] = {};
    for (unsigned row = 0; row < 4; ++row) {
        const std::string name = "VConstFloat[" + std::to_string(row) + "]";
        const GLint location = glGetUniformLocation(s_current_program, name.c_str());
        if (location < 0) {
            if (row < 2) {
                LOGI("GFx bounds unavailable program=%u uniform=%s",
                     s_current_program, name.c_str());
                return;
            }
            continue;
        }
        glGetUniformfv(s_current_program, location, rows[row]);
    }
    const bool perspective =
        s_scene_gfx_perspective_programs.contains(s_current_program);
    const size_t component_size = element_type == GL_FLOAT ? 4 : 2;
    const size_t element_stride = stride ? static_cast<size_t>(stride) :
        2 * component_size;
    const size_t pointer_offset = reinterpret_cast<uintptr_t>(pointer);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::array<double, 2>> points(static_cast<size_t>(count), {nan, nan});
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = min_x;
    double max_x = -min_x, max_y = -min_x;
    size_t valid = 0, invalid = 0, outside = 0;
    {
        std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
        const auto indices = s_buffer_shadows.find(s_bound_element_buffer);
        const auto positions = s_buffer_shadows.find(static_cast<GLuint>(buffer));
        const size_t index_offset = static_cast<size_t>(indices_address);
        const size_t index_bytes = static_cast<size_t>(count) * index_size;
        if (indices == s_buffer_shadows.end() ||
            positions == s_buffer_shadows.end() ||
            index_offset > indices->second.size() ||
            index_bytes > indices->second.size() - index_offset) {
            LOGI("GFx bounds missing shadow program=%u ebo=%u vbo=%d "
                 "indexOffset=%zu indexBytes=%zu",
                 s_current_program, s_bound_element_buffer, buffer,
                 index_offset, index_bytes);
            return;
        }
        const auto& ebo = indices->second;
        const auto& vbo = positions->second;
        for (GLsizei i = 0; i < count; ++i) {
            uint32_t index = 0;
            std::memcpy(&index, ebo.data() + index_offset +
                        static_cast<size_t>(i) * index_size, index_size);
            const int64_t vertex = static_cast<int64_t>(index) + base_vertex;
            if (vertex < 0 || static_cast<uint64_t>(vertex) >
                (std::numeric_limits<size_t>::max() - pointer_offset) /
                    element_stride) {
                ++invalid;
                continue;
            }
            const size_t offset = pointer_offset +
                static_cast<size_t>(vertex) * element_stride;
            if (offset > vbo.size() || 2 * component_size > vbo.size() - offset) {
                ++invalid;
                continue;
            }
            double x = 0, y = 0;
            if (element_type == GL_FLOAT) {
                float raw[2];
                std::memcpy(raw, vbo.data() + offset, sizeof(raw));
                x = raw[0]; y = raw[1];
            } else {
                int16_t raw[2];
                std::memcpy(raw, vbo.data() + offset, sizeof(raw));
                x = normalized ? std::max(-1.0, raw[0] / 32767.0) : raw[0];
                y = normalized ? std::max(-1.0, raw[1] / 32767.0) : raw[1];
            }
            const double w = perspective
                ? x * rows[3][0] + y * rows[3][1] + rows[3][3] : 1.0;
            if (!std::isfinite(w) || w <= 0) {
                ++invalid;
                continue;
            }
            const double clip_x =
                (x * rows[0][0] + y * rows[0][1] + rows[0][3]) / w;
            const double clip_y =
                (x * rows[1][0] + y * rows[1][1] + rows[1][3]) / w;
            if (!std::isfinite(clip_x) || !std::isfinite(clip_y)) {
                ++invalid;
                continue;
            }
            points[static_cast<size_t>(i)] = {clip_x, clip_y};
            min_x = std::min(min_x, clip_x); max_x = std::max(max_x, clip_x);
            min_y = std::min(min_y, clip_y); max_y = std::max(max_y, clip_y);
            if (std::abs(clip_x) > 1.5 || std::abs(clip_y) > 1.5) ++outside;
            ++valid;
        }
    }
    double max_triplet_box = 0;
    size_t max_triplet = 0;
    for (size_t i = 0; i + 2 < points.size(); i += 3) {
        if (!std::isfinite(points[i][0]) || !std::isfinite(points[i + 1][0]) ||
            !std::isfinite(points[i + 2][0])) continue;
        const double dx = std::max({points[i][0], points[i + 1][0], points[i + 2][0]}) -
                          std::min({points[i][0], points[i + 1][0], points[i + 2][0]});
        const double dy = std::max({points[i][1], points[i + 1][1], points[i + 2][1]}) -
                          std::min({points[i][1], points[i + 1][1], points[i + 2][1]});
        if (dx * dy > max_triplet_box) {
            max_triplet_box = dx * dy;
            max_triplet = i / 3;
        }
    }
    LOGI("GFx bounds program=%u perspective=%d count=%d valid=%zu invalid=%zu "
         "outside1p5=%zu clipX=[%.6g,%.6g] clipY=[%.6g,%.6g] "
         "maxTripletBox=%.6g triplet=%zu",
         s_current_program, perspective, count, valid, invalid, outside,
         min_x, max_x, min_y, max_y, max_triplet_box, max_triplet);
}

static void SceneAuditCompareGpuShadowRange(const char* label, GLuint buffer,
                                            size_t offset, size_t byte_count) {
    if (!buffer || !byte_count || byte_count > 65536 ||
        offset > std::numeric_limits<GLintptr>::max() - byte_count) {
        LOGI("GFx GPU compare skipped label=%s buffer=%u offset=%zu bytes=%zu",
             label, buffer, offset, byte_count);
        return;
    }
    std::vector<uint8_t> cpu_before(byte_count), cpu_after(byte_count);
    std::vector<uint8_t> gpu(byte_count);
    if (!CopyGlBufferShadow(buffer, offset, byte_count, cpu_before.data())) {
        LOGI("GFx GPU compare missing CPU shadow label=%s buffer=%u "
             "offset=%zu bytes=%zu", label, buffer, offset, byte_count);
        return;
    }
    // Copy through a separate staging buffer because the guest's stream VBO
    // itself is already persistently mapped for writing.
    GLint previous_read = 0, previous_write = 0;
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &previous_write);
    const GLenum prior_error = glGetError();
    PreserveSceneAuditGlError(prior_error);
    GLuint staging = 0;
    glGenBuffers(1, &staging);
    glBindBuffer(GL_COPY_READ_BUFFER, buffer);
    glBindBuffer(GL_COPY_WRITE_BUFFER, staging);
    glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(byte_count),
                 nullptr, GL_STREAM_READ);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(offset), 0,
                        static_cast<GLsizeiptr>(byte_count));
    const GLenum copy_error = glGetError();
    bool mapped = false;
    GLenum map_error = GL_NO_ERROR;
    if (copy_error == GL_NO_ERROR) {
        void* mapped_bytes = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0,
                                              static_cast<GLsizeiptr>(byte_count),
                                              GL_MAP_READ_BIT);
        map_error = glGetError();
        if (mapped_bytes && map_error == GL_NO_ERROR) {
            std::memcpy(gpu.data(), mapped_bytes, byte_count);
            mapped = true;
        }
        if (mapped_bytes) glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    }
    glBindBuffer(GL_COPY_READ_BUFFER, static_cast<GLuint>(previous_read));
    glBindBuffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(previous_write));
    if (staging) glDeleteBuffers(1, &staging);
    const GLenum cleanup_error = glGetError();
    const bool cpu_after_valid = CopyGlBufferShadow(
        buffer, offset, byte_count, cpu_after.data());
    size_t changed_cpu = 0, differs_before = 0, differs_after = 0;
    size_t first_difference = byte_count;
    if (cpu_after_valid) {
        for (size_t i = 0; i < byte_count; ++i) {
            changed_cpu += cpu_before[i] != cpu_after[i];
            if (mapped) {
                differs_before += gpu[i] != cpu_before[i];
                differs_after += gpu[i] != cpu_after[i];
                if (gpu[i] != cpu_before[i] && first_difference == byte_count) {
                    first_difference = i;
                }
            }
        }
    }
    LOGI("GFx GPU compare label=%s program=%u buffer=%u offset=%zu "
         "bytes=%zu mapped=%d copyError=0x%04X mapError=0x%04X "
         "cleanupError=0x%04X cpuAfter=%d cpuChanged=%zu "
         "gpuVsBefore=%zu gpuVsAfter=%zu firstDiff=%zu",
         label, s_current_program, buffer, offset, byte_count, mapped,
         copy_error, map_error, cleanup_error, cpu_after_valid, changed_cpu,
         differs_before, differs_after, first_difference);
}

// Opt-in numbered sessions rearm bounded GFx stream captures without restarting.
// The caller still requires scene_audit; leave this unset outside a focused probe.
static unsigned GfxStreamAuditSession() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.horn.gfx_stream_audit", value) <= 0) return 0;
    return value[0] >= '1' && value[0] <= '9' && value[1] == 0
        ? static_cast<unsigned>(value[0] - '0') : 0;
}

static void SceneAuditCompareGfxGpuBuffers(GLsizei count, GLenum type,
                                           uint32_t indices_address,
                                           GLint base_vertex) {
    if (type != GL_UNSIGNED_SHORT || count <= 0 || count > 4096 ||
        !s_bound_element_buffer) return;
    GLint enabled = 0, element_type = 0, stride = 0, buffer = 0;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &element_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
    if (!enabled || buffer <= 0 || stride <= 0) return;
    static thread_local unsigned text_samples = 0, screen_text_samples = 0;
    static thread_local unsigned shape_samples = 0;
    const unsigned session = GfxStreamAuditSession();
    static thread_local unsigned last_session = 0, focused_samples = 0;
    if (session != last_session) {
        last_session = session;
        focused_samples = 0;
    }
    const char* label = nullptr;
    if (session != 0) {
        // Include short-coordinate icons and shapes, not just glyph quads.
        if (focused_samples >= 64) return;
        ++focused_samples;
        label = "focused";
    } else if (element_type == GL_FLOAT && text_samples < 2) {
        ++text_samples;
        label = "text";
    } else if (element_type == GL_FLOAT && screen_text_samples < 2) {
        const GLint location = glGetUniformLocation(s_current_program,
                                                    "VConstFloat[0]");
        GLfloat row[4] = {};
        if (location >= 0) glGetUniformfv(s_current_program, location, row);
        if (row[0] > 0 && row[0] < 0.001f) {
            ++screen_text_samples;
            label = "screen-text";
        }
    } else if (element_type == GL_SHORT && count >= 1000 &&
               shape_samples < 2) {
        ++shape_samples;
        label = "shape";
    }
    if (!label) return;
    const size_t index_bytes = static_cast<size_t>(count) * sizeof(uint16_t);
    std::vector<uint16_t> indices(static_cast<size_t>(count));
    if (!CopyGlBufferShadow(s_bound_element_buffer, indices_address,
                            index_bytes, indices.data())) {
        LOGI("GFx GPU compare indices unavailable label=%s program=%u",
             label, s_current_program);
        return;
    }
    const auto [first, last] = std::minmax_element(indices.begin(), indices.end());
    const int64_t min_vertex = static_cast<int64_t>(*first) + base_vertex;
    const int64_t max_vertex = static_cast<int64_t>(*last) + base_vertex;
    void* pointer = nullptr;
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    if (min_vertex < 0 || max_vertex < min_vertex) return;
    const size_t position_begin = reinterpret_cast<uintptr_t>(pointer) +
        static_cast<size_t>(min_vertex) * static_cast<size_t>(stride);
    LOGI("GFx GPU compare selection label=%s program=%u count=%d "
         "vertices=%lld..%lld vbo=%d stride=%d",
         label, s_current_program, count,
         static_cast<long long>(min_vertex),
         static_cast<long long>(max_vertex), buffer, stride);
    SceneAuditCompareGpuShadowRange("indices", s_bound_element_buffer,
                                    indices_address, index_bytes);
    // Compare the complete interleaved record, including the first vertex's
    // color and UV fields which can precede its position pointer by 12 bytes.
    uintptr_t record_begin = reinterpret_cast<uintptr_t>(pointer);
    uintptr_t record_end = record_begin;
    bool interleaved = true;
    for (GLuint attribute = 0; attribute < 16; ++attribute) {
        GLint active = 0, source = 0, components = 0, attribute_type = 0;
        GLint attribute_stride = 0;
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &active);
        if (!active) continue;
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &source);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_SIZE, &components);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute_type);
        glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribute_stride);
        if (source != buffer || attribute_stride != stride || components < 1 || components > 4) {
            interleaved = false; break;
        }
        size_t component_bytes = 0;
        switch (attribute_type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: component_bytes = 1; break;
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: component_bytes = 2; break;
        case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: component_bytes = 4; break;
        default: interleaved = false; break;
        }
        if (!interleaved) break;
        void* attribute_pointer = nullptr;
        glGetVertexAttribPointerv(attribute, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attribute_pointer);
        const uintptr_t start = reinterpret_cast<uintptr_t>(attribute_pointer);
        const size_t bytes = static_cast<size_t>(components) * component_bytes;
        if (start > std::numeric_limits<uintptr_t>::max() - bytes) {
            interleaved = false; break;
        }
        record_begin = std::min(record_begin, start);
        record_end = std::max(record_end, start + bytes);
    }
    if (interleaved && record_end >= record_begin &&
        record_end - record_begin <= static_cast<size_t>(stride)) {
        const uint64_t begin = static_cast<uint64_t>(record_begin) +
            static_cast<uint64_t>(min_vertex) * static_cast<size_t>(stride);
        const uint64_t end = static_cast<uint64_t>(record_end) +
            static_cast<uint64_t>(max_vertex) * static_cast<size_t>(stride);
        if (end >= begin && end - begin <= 65536 &&
            end <= std::numeric_limits<size_t>::max()) {
            LOGI("GFx GPU record span session=%u sample=%u program=%u buffer=%d "
                 "origin=%llu offset=%llu bytes=%llu positionPrefix=%llu",
                 session, focused_samples, s_current_program, buffer,
                 static_cast<unsigned long long>(record_begin),
                 static_cast<unsigned long long>(begin),
                 static_cast<unsigned long long>(end - begin),
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pointer) - record_begin));
            SceneAuditCompareGpuShadowRange("records", static_cast<GLuint>(buffer),
                                            static_cast<size_t>(begin), static_cast<size_t>(end - begin));
        }
    } else {
        LOGI("GFx GPU record span skipped: noninterleaved attributes program=%u", s_current_program);
    }
    if (std::strcmp(label, "text") == 0 && text_samples <= 2 &&
        stride == 20 && min_vertex == 0 && position_begin >= 12) {
        const std::string base =
            "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/gfx-text-pass" +
            std::to_string(text_samples);
        const size_t vertex_bytes = static_cast<size_t>(max_vertex + 1) * 20;
        std::vector<uint8_t> records(vertex_bytes);
        const bool have_records = CopyGlBufferShadow(
            static_cast<GLuint>(buffer), position_begin - 12,
            vertex_bytes, records.data());
        const auto save = [&](const char* suffix, const void* bytes,
                              size_t count) {
            const std::string path = base + suffix;
            FILE* file = std::fopen(path.c_str(), "wb");
            if (!file) return false;
            const bool complete = std::fwrite(bytes, 1, count, file) == count;
            return std::fclose(file) == 0 && complete;
        };
        const bool indices_saved = save("-indices.bin", indices.data(),
                                        index_bytes);
        const bool vertices_saved = have_records &&
            save("-vertices.bin", records.data(), records.size());
        const std::string meta_path = base + "-meta.txt";
        FILE* meta = std::fopen(meta_path.c_str(), "wb");
        if (meta) {
            std::fprintf(meta, "program=%u count=%d stride=%d vertices=%lld..%lld\n",
                         s_current_program, count, stride,
                         static_cast<long long>(min_vertex),
                         static_cast<long long>(max_vertex));
            for (unsigned row = 0; row < 6; ++row) {
                const std::string name =
                    "VConstFloat[" + std::to_string(row) + "]";
                const GLint location = glGetUniformLocation(
                    s_current_program, name.c_str());
                GLfloat values[4] = {};
                if (location >= 0) {
                    glGetUniformfv(s_current_program, location, values);
                }
                std::fprintf(meta, "%s=%.9g,%.9g,%.9g,%.9g\n",
                             name.c_str(), values[0], values[1], values[2],
                             values[3]);
            }
            std::fclose(meta);
        }
        LOGI("GFx text dump pass=%u indices=%d vertices=%d meta=%d",
             text_samples, indices_saved, vertices_saved, meta != nullptr);
    }
}

static void SceneAuditLogGfxAtlasContent(GLuint texture, GLint width) {
    if (!texture || width <= 0 || width > 1024) return;
    static thread_local std::unordered_set<GLuint> sampled;
    if (sampled.size() >= 8 || !sampled.insert(texture).second) return;
    GLint height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    if (height <= 0 || height > 1024) return;
    GLint prior_read_fbo = 0, prior_read_buffer = 0;
    GLint prior_pack_buffer = 0, prior_pack_alignment = 0;
    GLint prior_pack_row = 0, prior_pack_skip_pixels = 0;
    GLint prior_pack_skip_rows = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prior_read_fbo);
    glGetIntegerv(GL_READ_BUFFER, &prior_read_buffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prior_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prior_pack_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &prior_pack_row);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &prior_pack_skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &prior_pack_skip_rows);
    PreserveSceneAuditGlError(glGetError());
    GLuint read_fbo = 0;
    glGenFramebuffers(1, &read_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLenum error = GL_NO_ERROR;
    size_t nonzero = 0;
    uint64_t hash = 1469598103934665603ull;
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba.data());
        error = glGetError();
        if (error == GL_NO_ERROR) {
            for (size_t i = 0; i < rgba.size(); i += 4) {
                const uint8_t red = rgba[i];
                nonzero += red != 0;
                hash = (hash ^ red) * 1099511628211ull;
            }
            if (width == 1024 && height == 1024) {
                const std::string path =
                    "/sdcard/Android/data/com.tt2kgames.Borderlands2/files/" +
                    std::string("gfx-atlas-") + std::to_string(texture) + ".pgm";
                if (FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fprintf(file, "P5\n%d %d\n255\n", width, height);
                    for (size_t i = 0; i < rgba.size(); i += 4) {
                        std::fputc(rgba[i], file);
                    }
                    const int close_result = std::fclose(file);
                    LOGI("GFx atlas dump path=%s close=%d", path.c_str(),
                         close_result);
                } else {
                    LOGE("GFx atlas dump failed path=%s", path.c_str());
                }
            }
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prior_pack_buffer));
    glPixelStorei(GL_PACK_ALIGNMENT, prior_pack_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, prior_pack_row);
    glPixelStorei(GL_PACK_SKIP_PIXELS, prior_pack_skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, prior_pack_skip_rows);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prior_read_fbo));
    glReadBuffer(static_cast<GLenum>(prior_read_buffer));
    glDeleteFramebuffers(1, &read_fbo);
    const GLenum cleanup_error = glGetError();
    LOGI("GFx atlas content texture=%u size=%dx%d status=0x%04X "
         "readError=0x%04X cleanupError=0x%04X nonzero=%zu/%zu hash=%016llX",
         texture, width, height, status, error, cleanup_error, nonzero,
         static_cast<size_t>(width) * height,
         static_cast<unsigned long long>(hash));
}

void SceneAuditLogFinalInvocation(const char* operation, GLint first,
                                  GLsizei count, GLenum type,
                                  uint32_t indices_address,
                                  GLint base_vertex, GLsizei instances) {
    // Pixel readback diagnosis paused; preserve the probe for a later run.
    // if (Bl2FogAuditActive()) {
    //     bool fog = false;
    //     {
    //         std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
    //         const auto fragment = s_legacy_stages.find(s_bound_legacy_fragment);
    //         fog = fragment != s_legacy_stages.end() &&
    //               fragment->second.name == "HeightFogPixelShader";
    //     }
    //     Bl2FogAuditDraw(fog, operation, first, count, type, indices_address,
    //                      base_vertex, instances);
    // }
    if (!SceneAuditEnabled()) return;
    if ((s_bound_legacy_fragment >= 1610612847u &&
         s_bound_legacy_fragment <= 1610612852u) ||
        s_bound_legacy_fragment == 1610612748u) {
        GLint framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        const uint32_t draw = framebuffer == 15 ?
            SceneAuditCurrentDrawCount(15) + 1 : 0;
        // Capture both the black initializer and textured terrain batches.
        // Frame-relative draw numbers change with camera/visibility and must
        // not decide whether a particular material is observed at all.
        static thread_local std::unordered_map<GLuint, uint32_t> material_draws;
        if (framebuffer == 15 && material_draws[s_bound_legacy_fragment] < 6) {
            const uint32_t sample = ++material_draws[s_bound_legacy_fragment];
            LOGI("Scene ground inputs draw=%u sample=%u host=%u fragment=%u "
                 "operation=%s count=%d type=0x%X indices=0x%08X base=%d "
                 "instances=%d ebo=%u", draw, sample, s_current_program,
                 s_bound_legacy_fragment, operation, count, type,
                 indices_address, base_vertex, instances, s_bound_element_buffer);
            static std::unordered_set<GLuint> state_logged;
            if (state_logged.insert(s_current_program).second) {
                GLboolean color_mask[4] = {};
                GLboolean depth_mask = GL_FALSE;
                GLint depth_func = 0, src_rgb = 0, dst_rgb = 0;
                GLint src_alpha = 0, dst_alpha = 0;
                GLint blend_rgb = 0, blend_alpha = 0;
                GLint depth_bits = 0, cull_mode = 0, front_face = 0;
                GLfloat polygon_factor = 0, polygon_units = 0, depth_range[2] = {};
                glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
                glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
                glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
                glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
                glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
                glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
                glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
                glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_rgb);
                glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_alpha);
                glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
                glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &polygon_factor);
                glGetFloatv(GL_POLYGON_OFFSET_UNITS, &polygon_units);
                glGetFloatv(GL_DEPTH_RANGE, depth_range);
                glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
                glGetIntegerv(GL_FRONT_FACE, &front_face);
                LOGI("Scene ground state draw=%u host=%u blend=%d "
                     "blendRGB=0x%X/0x%X/0x%X blendA=0x%X/0x%X/0x%X "
                     "depth=%d depthFunc=0x%X depthMask=%d "
                     "colorMask=%d,%d,%d,%d depthBits=%d range=%g,%g "
                     "polygon=%d factor=%g units=%g stencil=%d "
                     "cull=%d cullMode=0x%X frontFace=0x%X",
                     draw, s_current_program, glIsEnabled(GL_BLEND),
                     src_rgb, dst_rgb, blend_rgb,
                     src_alpha, dst_alpha, blend_alpha,
                     glIsEnabled(GL_DEPTH_TEST), depth_func, depth_mask,
                     color_mask[0], color_mask[1], color_mask[2], color_mask[3],
                     depth_bits, depth_range[0], depth_range[1],
                     glIsEnabled(GL_POLYGON_OFFSET_FILL), polygon_factor,
                     polygon_units, glIsEnabled(GL_STENCIL_TEST),
                     glIsEnabled(GL_CULL_FACE), cull_mode, front_face);
            }
            if (s_bound_legacy_fragment == 1610612847u) {
                std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
                const auto stage = s_legacy_stages.find(s_bound_legacy_fragment);
                if (stage != s_legacy_stages.end()) {
                    const auto& group = stage->second.constant_groups[5];
                    const size_t word = group.first_register >= 0 ?
                        static_cast<size_t>(group.first_register + 4) * 4 : 0;
                    GLfloat values[4] = {};
                    if (group.byte_count >= 5 * 16 &&
                        word + 4 <= stage->second.local_parameters.size()) {
                        std::memcpy(values, stage->second.local_parameters.data() + word,
                                    sizeof(values));
                    }
                    LOGI("Scene ground shadow global4 stage=%u first=%d "
                         "bytes=%u local_words=%zu values={%.7g,%.7g,%.7g,%.7g}",
                         s_bound_legacy_fragment, group.first_register,
                         group.byte_count, stage->second.local_parameters.size(),
                         values[0], values[1], values[2], values[3]);
                }
            }
            for (const char* name : {"PConstGlobal[0]", "PConstGlobal[4]",
                                     "PConstFloat[0]", "PConstFloat[1]",
                                     "PConstFloat[2]", "PConstFloat[3]",
                                     "PConstFloat[4]", "PConstFloat[5]",
                                     "PConstFloat[6]", "PConstFloat[7]"}) {
                const GLint location = glGetUniformLocation(s_current_program, name);
                GLfloat values[4] = {};
                if (location >= 0) glGetUniformfv(s_current_program, location, values);
                LOGI("Scene ground uniform draw=%u name=%s location=%d "
                     "values={%.7g,%.7g,%.7g,%.7g}", draw, name, location,
                     values[0], values[1], values[2], values[3]);
            }
            GLint previous_active = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
            std::array<bool, 16> used_units{};
            {
                std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
                const auto stage = s_legacy_stages.find(s_bound_legacy_fragment);
                if (stage != s_legacy_stages.end()) {
                    for (size_t unit = 0; unit < used_units.size(); ++unit) {
                        used_units[unit] = stage->second.sampler_registers[unit] >= 0;
                    }
                }
            }
            for (GLint unit = 0; unit < static_cast<GLint>(used_units.size()); ++unit) {
                if (!used_units[unit]) continue;
                glActiveTexture(GL_TEXTURE0 + unit);
                GLint texture = 0, sampler = 0, width = 0, height = 0;
                GLint internal_format = 0;
                GLint swizzle[4] = {};
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
                glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
                if (texture) {
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                             GL_TEXTURE_INTERNAL_FORMAT,
                                             &internal_format);
                    constexpr GLenum kSwizzleParameters[] = {
                        GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                        GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A,
                    };
                    for (size_t component = 0; component < std::size(swizzle);
                         ++component) {
                        glGetTexParameteriv(GL_TEXTURE_2D,
                            kSwizzleParameters[component], &swizzle[component]);
                    }
                }
                std::string payload_sha256 = "unrecorded";
                if (SceneTextureHashEnabled() && texture > 0) {
                    std::lock_guard<std::mutex> lock(
                        s_scene_texture_fingerprints_mutex);
                    const auto found = s_scene_texture_fingerprints.find(
                        static_cast<GLuint>(texture));
                    if (found != s_scene_texture_fingerprints.end()) {
                        payload_sha256 = found->second.sha256.data();
                    }
                }
                LOGI("Scene ground sampler draw=%u unit=%d texture=%d "
                     "sampler=%d size=%dx%d internal=0x%04X "
                     "swizzle={0x%04X,0x%04X,0x%04X,0x%04X} uploadSHA256=%s",
                     draw, unit, texture, sampler, width, height,
                     internal_format, swizzle[0], swizzle[1], swizzle[2],
                     swizzle[3], payload_sha256.c_str());
            }
            glActiveTexture(static_cast<GLenum>(previous_active));
        }
    }
    if (s_scene_gfx_programs.contains(s_current_program)) {
        static thread_local std::unordered_map<GLuint, uint32_t> gfx_draws;
        static thread_local uint32_t total_gfx_draws = 0;
        static thread_local unsigned gfx_stream_session = 0;
        const unsigned requested_session = GfxStreamAuditSession();
        if (requested_session != 0 && requested_session != gfx_stream_session) {
            gfx_stream_session = requested_session;
            gfx_draws.clear();
            total_gfx_draws = 0;
        }
        if (gfx_draws[s_current_program] < 16 && total_gfx_draws < 160) {
            ++gfx_draws[s_current_program];
            ++total_gfx_draws;
            GLint framebuffer = 0, vao = 0;
            GLint viewport[4] = {}, scissor_box[4] = {};
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
            LOGI("GFx geometry draw=%u operation=%s program=%u fbo=%d vao=%d "
                 "first=%d count=%d type=0x%04X indices=0x%08X base=%d "
                 "instances=%d ebo=%u viewport=%d,%d %dx%d scissor=%d "
                 "box=%d,%d %dx%d",
                 total_gfx_draws, operation, s_current_program, framebuffer,
                 vao, first, count, type, indices_address, base_vertex,
                 instances, s_bound_element_buffer, viewport[0], viewport[1],
                 viewport[2], viewport[3], glIsEnabled(GL_SCISSOR_TEST),
                 scissor_box[0], scissor_box[1], scissor_box[2],
                 scissor_box[3]);
            std::array<GLint, 3> vertices{-1, -1, -1};
            const GLsizei sample_count = std::clamp(count, 0, 3);
            if (type == GL_NONE) {
                for (GLsizei i = 0; i < sample_count; ++i) {
                    if (first >= 0 && first <= std::numeric_limits<GLint>::max() - i)
                        vertices[i] = first + i;
                }
            } else {
                const size_t index_size = type == GL_UNSIGNED_BYTE ? 1 :
                    type == GL_UNSIGNED_SHORT ? 2 :
                    type == GL_UNSIGNED_INT ? 4 : 0;
                std::array<uint8_t, 12> raw{};
                const bool valid = index_size != 0 && s_bound_element_buffer != 0 &&
                    CopyGlBufferShadow(s_bound_element_buffer, indices_address,
                                       static_cast<size_t>(sample_count) * index_size,
                                       raw.data());
                LOGI("GFx geometry indices program=%u shadow=%d element_size=%zu",
                     s_current_program, valid, index_size);
                if (valid) {
                    for (GLsizei i = 0; i < sample_count; ++i) {
                        uint32_t index = 0;
                        std::memcpy(&index, raw.data() + i * index_size, index_size);
                        const int64_t vertex = static_cast<int64_t>(index) + base_vertex;
                        if (vertex >= 0 && vertex <= std::numeric_limits<GLint>::max())
                            vertices[i] = static_cast<GLint>(vertex);
                    }
                }
            }
            LOGI("GFx geometry vertices program=%u sample={%d,%d,%d}",
                 s_current_program, vertices[0], vertices[1], vertices[2]);
            for (GLuint attribute : {0u, 1u, 2u, 3u, 8u}) {
                GLint enabled = 0, size = 0, element_type = 0;
                GLint normalized = 0, stride = 0, buffer = 0;
                void* pointer = nullptr;
                glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
                glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
                glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_TYPE, &element_type);
                glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
                glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
                glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                                    &buffer);
                glGetVertexAttribPointerv(attribute, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                          &pointer);
                LOGI("GFx geometry attribute=%u enabled=%d size=%d type=0x%04X "
                     "normalized=%d stride=%d buffer=%d pointer=0x%llX",
                     attribute, enabled, size, element_type, normalized,
                     stride, buffer, static_cast<unsigned long long>(
                         reinterpret_cast<uintptr_t>(pointer)));
                if (attribute == 3 && !enabled) {
                    GLfloat current[4] = {};
                    glGetVertexAttribfv(attribute, GL_CURRENT_VERTEX_ATTRIB,
                                        current);
                    LOGI("GFx color1 constant program=%u values={%.6g,%.6g,%.6g,%.6g}",
                         s_current_program, current[0], current[1],
                         current[2], current[3]);
                }
                const size_t component_size = element_type == GL_FLOAT ||
                    element_type == GL_INT || element_type == GL_UNSIGNED_INT ? 4 :
                    element_type == GL_SHORT || element_type == GL_UNSIGNED_SHORT ||
                    element_type == GL_HALF_FLOAT ? 2 :
                    element_type == GL_BYTE || element_type == GL_UNSIGNED_BYTE ? 1 : 0;
                if (!enabled || buffer <= 0 || size < 1 || size > 4 ||
                    stride < 0 || component_size == 0) continue;
                const size_t bytes = static_cast<size_t>(size) * component_size;
                const size_t element_stride = stride ? static_cast<size_t>(stride) : bytes;
                for (GLint vertex : vertices) {
                    if (vertex < 0) continue;
                    const size_t offset = reinterpret_cast<uintptr_t>(pointer) +
                        static_cast<size_t>(vertex) * element_stride;
                    std::array<uint64_t, 2> raw{};
                    const bool shadow = CopyGlBufferShadow(
                        static_cast<GLuint>(buffer), offset, bytes, raw.data());
                    LOGI("GFx geometry vertex program=%u attribute=%u vertex=%d "
                         "offset=%zu bytes=%zu shadow=%d raw=%016llX%016llX",
                         s_current_program, attribute, vertex, offset,
                         bytes, shadow,
                         static_cast<unsigned long long>(raw[1]),
                         static_cast<unsigned long long>(raw[0]));
                }
            }
            for (unsigned row = 0; row < 4; ++row) {
                const std::string name = "VConstFloat[" + std::to_string(row) + "]";
                const GLint location = glGetUniformLocation(
                    s_current_program, name.c_str());
                GLfloat values[4] = {};
                if (location >= 0) glGetUniformfv(s_current_program, location, values);
                LOGI("GFx geometry uniform=%s location=%d values={%.7g,%.7g,%.7g,%.7g}",
                     name.c_str(), location, values[0], values[1], values[2],
                     values[3]);
            }
            const GLint fragment_location = glGetUniformLocation(
                s_current_program, "PConstFloat[0]");
            GLfloat fragment_constant[4] = {};
            if (fragment_location >= 0) {
                glGetUniformfv(s_current_program, fragment_location,
                               fragment_constant);
            }
            GLint blend_src_rgb = 0, blend_dst_rgb = 0;
            GLint blend_src_alpha = 0, blend_dst_alpha = 0;
            GLint blend_equation_rgb = 0, blend_equation_alpha = 0;
            glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
            glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation_rgb);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_equation_alpha);
            LOGI("GFx fragment state program=%u PConst0={%.6g,%.6g,%.6g,%.6g} "
                 "blend=%d srcRGB=0x%04X dstRGB=0x%04X srcA=0x%04X "
                 "dstA=0x%04X eqRGB=0x%04X eqA=0x%04X depth=%d stencil=%d "
                 "cull=%d",
                 s_current_program, fragment_constant[0], fragment_constant[1],
                 fragment_constant[2], fragment_constant[3],
                 glIsEnabled(GL_BLEND), blend_src_rgb, blend_dst_rgb,
                 blend_src_alpha, blend_dst_alpha, blend_equation_rgb,
                 blend_equation_alpha, glIsEnabled(GL_DEPTH_TEST),
                 glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_CULL_FACE));
            GLint previous_active_texture = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);
            glActiveTexture(GL_TEXTURE0);
            GLint texture = 0, sampler = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
            GLint swizzle[4] = {};
            GLint texture_min_filter = 0, sampler_min_filter = 0;
            GLint mip0_width = 0, mip1_width = 0;
            GLint base_level = 0, max_level = 0, immutable_levels = 0;
            if (texture > 0) {
                constexpr GLenum names[] = {GL_TEXTURE_SWIZZLE_R,
                    GL_TEXTURE_SWIZZLE_G, GL_TEXTURE_SWIZZLE_B,
                    GL_TEXTURE_SWIZZLE_A};
                for (size_t component = 0; component < 4; ++component) {
                    glGetTexParameteriv(GL_TEXTURE_2D, names[component],
                                        &swizzle[component]);
                }
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                    &texture_min_filter);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                                         &mip0_width);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH,
                                         &mip1_width);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL,
                                    &base_level);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                                    &max_level);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS,
                                    &immutable_levels);
            }
            if (sampler > 0) {
                glGetSamplerParameteriv(static_cast<GLuint>(sampler),
                                        GL_TEXTURE_MIN_FILTER,
                                        &sampler_min_filter);
            }
            glActiveTexture(static_cast<GLenum>(previous_active_texture));
            LOGI("GFx texture state program=%u tex0=%d sampler0=%d "
                 "swizzle={0x%04X,0x%04X,0x%04X,0x%04X} "
                 "texMin=0x%04X samplerMin=0x%04X mip0=%d mip1=%d "
                 "base=%d max=%d immutable=%d",
                 s_current_program, texture, sampler, swizzle[0], swizzle[1],
                 swizzle[2], swizzle[3], texture_min_filter,
                 sampler_min_filter, mip0_width, mip1_width, base_level,
                 max_level, immutable_levels);
            if (texture > 0) {
                glActiveTexture(GL_TEXTURE0);
                SceneAuditLogGfxAtlasContent(static_cast<GLuint>(texture),
                                             mip0_width);
                glActiveTexture(static_cast<GLenum>(previous_active_texture));
            }
            if (type != GL_NONE) {
                SceneAuditLogGfxIndexedBounds(count, type, indices_address,
                                               base_vertex);
                SceneAuditCompareGfxGpuBuffers(count, type, indices_address,
                                               base_vertex);
            }
        }
    }
    if (s_current_program != 258 &&
        (!s_scene_final_program ||
         s_current_program != s_scene_final_program)) return;
    static thread_local std::unordered_map<GLuint, uint32_t> logged;
    if (logged[s_current_program]++ >= 4) return;
    LOGI("Scene final invocation operation=%s program=%u first=%d "
         "count=%d type=0x%04X indices=0x%08X base=%d instances=%d",
         operation, s_current_program, first, count, type,
         indices_address, base_vertex, instances);
}

void SceneAuditLogFinalIndexedDraw(const char* operation, GLsizei count,
                                   GLenum type, const void* indices,
                                   GLint base_vertex, GLsizei instances) {
    if (!SceneAuditEnabled() ||
        (s_current_program != 258 &&
         (!s_scene_final_program ||
          s_current_program != s_scene_final_program)) || count <= 0 ||
        instances <= 0) return;
    static thread_local std::unordered_set<GLuint> attempted_programs;
    if (attempted_programs.contains(s_current_program) ||
        attempted_programs.size() >= 4) return;
    attempted_programs.insert(s_current_program);
    const size_t element_size = type == GL_UNSIGNED_BYTE ? 1 :
                                type == GL_UNSIGNED_SHORT ? 2 :
                                type == GL_UNSIGNED_INT ? 4 : 0;
    if (!element_size) {
        LOGI("Scene final indexed draw operation=%s unsupported type=0x%04X",
             operation, type);
        return;
    }
    const GLsizei sample_count = std::min(count, 4);
    std::array<uint8_t, 16> raw{};
    bool valid = false;
    if (s_bound_element_buffer != 0) {
        valid = CopyGlBufferShadow(s_bound_element_buffer,
                                   reinterpret_cast<uintptr_t>(indices),
                                   sample_count * element_size, raw.data());
    } else if (indices) {
        std::memcpy(raw.data(), indices, sample_count * element_size);
        valid = true;
    }
    LOGI("Scene final indexed draw operation=%s count=%d type=0x%04X "
         "ebo=%u offset=%p base=%d instances=%d shadow=%d",
         operation, count, type, s_bound_element_buffer, indices,
         base_vertex, instances, valid);
    if (!valid) return;
    GLint vertices[4] = {};
    for (GLsizei i = 0; i < sample_count; ++i) {
        uint32_t raw_index = 0;
        if (element_size == 1) raw_index = raw[i];
        else if (element_size == 2) {
            uint16_t value = 0;
            std::memcpy(&value, raw.data() + i * element_size, sizeof(value));
            raw_index = value;
        } else {
            std::memcpy(&raw_index, raw.data() + i * element_size,
                        sizeof(raw_index));
        }
        const int64_t vertex = static_cast<int64_t>(raw_index) + base_vertex;
        vertices[i] = vertex >= 0 && vertex <= std::numeric_limits<GLint>::max()
            ? static_cast<GLint>(vertex) : -1;
    }
    SceneAuditLogFinalQuadVertices(operation, vertices, sample_count,
                                   instances);
}

// Uniform updates are one of the hottest import paths in particle-heavy
// scenes. The binding table changes only while programs are linked/queried,
// yet the old path took a shared_mutex and searched an unordered_map for every
// glUniform* call. Keep a tiny per-render-thread direct cache and invalidate it
// with an epoch whenever the shared table changes. Cache misses retain the
// fully synchronized path, including program-id reuse after glDeleteProgram.
struct UniformTuningCacheEntry {
    uint64_t key = 0;
    uint64_t epoch = 0;
    UniformTuning tuning = UniformTuning::None;
};
constexpr size_t kUniformTuningCacheSize = 128;
static_assert((kUniformTuningCacheSize & (kUniformTuningCacheSize - 1)) == 0);
static thread_local std::array<UniformTuningCacheEntry,
                               kUniformTuningCacheSize>
    s_uniform_tuning_cache{};

constexpr GLfloat kBloomUniformScale = 1.15f;
constexpr GLfloat kDofUniformScale = 1.10f;
constexpr GLfloat kLightShaftUniformScale = 1.15f;

static uint64_t UniformKey(GLuint program, GLint location) {
    return (static_cast<uint64_t>(program) << 32) |
           static_cast<uint32_t>(location);
}

static UniformTuning ClassifyUniform(const char* raw_name) {
    if (!raw_name) return UniformTuning::None;
    std::string name(raw_name);
    if (name.ends_with("[0]")) name.resize(name.size() - 3);
    if (name == "BloomTintAndScreenBlendThreshold") {
        return UniformTuning::BloomTintAndScreenBlendThreshold;
    }
    if (name == "BloomScaleAndThreshold") {
        return UniformTuning::BloomScaleAndThreshold;
    }
    if (name == "DOFFactor") return UniformTuning::DofFactor;
    if (name == "DOFPackedParameters") return UniformTuning::DofPackedW;
    if (name == "LightShaftParameters") return UniformTuning::LightShaftY;
    return UniformTuning::None;
}

static UniformTuning CurrentUniformTuning(GLint location) {
    if (s_current_program == 0 || location < 0) return UniformTuning::None;

    const uint64_t key = UniformKey(s_current_program, location);
    const uint64_t epoch =
        s_uniform_binding_epoch.load(std::memory_order_acquire);
    const size_t cache_index = static_cast<size_t>(
        (key ^ (key >> 33) ^ (key >> 17)) &
        (kUniformTuningCacheSize - 1));
    UniformTuningCacheEntry& cached = s_uniform_tuning_cache[cache_index];
    if (cached.epoch == epoch && cached.key == key) return cached.tuning;

    std::shared_lock lock(s_uniform_binding_mutex);
    const auto it = s_uniform_bindings.find(key);
    const UniformTuning tuning =
        it == s_uniform_bindings.end() ? UniformTuning::None : it->second.tuning;
    cached = {key, epoch, tuning};
    return tuning;
}

static const char* UniformTuningName(UniformTuning tuning) {
    switch (tuning) {
        case UniformTuning::BloomTintAndScreenBlendThreshold:
            return "BloomTintAndScreenBlendThreshold";
        case UniformTuning::BloomScaleAndThreshold:
            return "BloomScaleAndThreshold";
        case UniformTuning::DofFactor: return "DOFFactor";
        case UniformTuning::DofPackedW: return "DOFPackedParameters.w";
        case UniformTuning::LightShaftY: return "LightShaftParameters.y";
        case UniformTuning::None: return "none";
    }
    return "unknown";
}

static void ApplyUniformTuning(
    UniformTuning tuning, GLfloat* values, GLsizei count, GLint width
) {
    if (tuning == UniformTuning::None || !values || count <= 0 || width <= 0) return;
    const uint32_t log_bit = 1u << static_cast<uint32_t>(tuning);
    if ((s_logged_uniform_tunings.fetch_or(log_bit, std::memory_order_relaxed) & log_bit) == 0) {
        LOGI("Uniform tuning applied: program=%u name=%s",
             s_current_program, UniformTuningName(tuning));
    }
    GLfloat scale = 1.0f;
    switch (tuning) {
        case UniformTuning::BloomTintAndScreenBlendThreshold:
        case UniformTuning::BloomScaleAndThreshold:
            scale = kBloomUniformScale;
            break;
        case UniformTuning::DofFactor: scale = kDofUniformScale; break;
        case UniformTuning::DofPackedW:
            if (width > 3) {
                for (GLsizei row = 0; row < count; ++row) values[row * width + 3] *= kDofUniformScale;
            }
            return;
        case UniformTuning::LightShaftY:
            if (width > 1) {
                for (GLsizei row = 0; row < count; ++row) values[row * width + 1] *= kLightShaftUniformScale;
            }
            return;
        case UniformTuning::None: return;
    }
    for (size_t i = 0; i < static_cast<size_t>(count) * width; ++i) values[i] *= scale;
}

static const GLfloat* TunedUniformValues(
    GLint location, GLsizei count, GLint width, const GLfloat* source,
    std::vector<GLfloat>& scratch
) {
    const UniformTuning tuning = CurrentUniformTuning(location);
    if (tuning == UniformTuning::None || !source || count <= 0) return source;
    scratch.assign(source, source + static_cast<size_t>(count) * width);
    ApplyUniformTuning(tuning, scratch.data(), count, width);
    return scratch.data();
}

static std::string FilterHornGuestExtensions(const GLubyte* raw_extensions) {
    if (!raw_extensions) return {};

    const std::string raw(reinterpret_cast<const char*>(raw_extensions));
    const bool has_depth_texture =
        HasExtensionToken(raw.c_str(), "GL_OES_depth_texture") ||
        HasExtensionToken(raw.c_str(), "GL_ARB_depth_texture");
    const bool has_mapbuffer =
        HasExtensionToken(raw.c_str(), "GL_OES_mapbuffer");
    const bool has_discard =
        HasExtensionToken(raw.c_str(), "GL_EXT_discard_framebuffer");
    const bool has_nonlinear_depth =
        HasExtensionToken(raw.c_str(), "GL_NV_depth_nonlinear");
    const bool has_aniso =
        HasExtensionToken(raw.c_str(), "GL_EXT_texture_filter_anisotropic");
    s_aniso_support.store(has_aniso ? 1 : 0, std::memory_order_release);

    static std::atomic<bool> logged_capabilities{false};
    bool expected = false;
    if (logged_capabilities.compare_exchange_strong(expected, true)) {
        const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        GLint depth_bits = 0;
        GLint stencil_bits = 0;
        GLint samples = 0;
        glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
        glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
        glGetIntegerv(GL_SAMPLES, &samples);
        LOGI("Host GL: vendor=%s renderer=%s version=%s depthBits=%d "
             "stencilBits=%d samples=%d depthTexture=%d mapBuffer=%d "
             "discardFramebuffer=%d anisotropy=%d nvNonlinearDepth=%d; "
             "guest S3TC facade=1",
             vendor ? vendor : "(null)", renderer ? renderer : "(null)",
             version ? version : "(null)", depth_bits, stencil_bits, samples,
             has_depth_texture, has_mapbuffer, has_discard, has_aniso,
             has_nonlinear_depth);
    }

    std::string filtered;
    bool has_s3tc = false;
    size_t cursor = 0;
    while (cursor < raw.size()) {
        while (cursor < raw.size() && raw[cursor] == ' ') ++cursor;
        const size_t begin = cursor;
        while (cursor < raw.size() && raw[cursor] != ' ') ++cursor;
        if (begin == cursor) continue;
        const std::string extension = raw.substr(begin, cursor - begin);
        if (IsUnavailableHornTextureExtension(extension)) continue;
        if (!filtered.empty()) filtered.push_back(' ');
        filtered += extension;
        has_s3tc |= extension == "GL_EXT_texture_compression_s3tc";
    }
    // Mali does not normally advertise S3TC. The guest only uses this token to
    // select its DXT package/layout; resulting uploads are decoded to RGBA8
    // (or SRGB8_ALPHA8) before they reach the real driver. ASTC is deferred.
    if (!has_s3tc) {
        if (!filtered.empty()) filtered.push_back(' ');
        filtered += "GL_EXT_texture_compression_s3tc";
    }
    return filtered;
}

// EGLDisplay is an opaque host pointer on Android. Give the ARM32 guest a
// stable 32-bit token instead of ever truncating the native pointer.
static uint32_t RegisterEglDisplay(EGLDisplay display) {
    if (display == EGL_NO_DISPLAY) return 0;

    std::lock_guard<std::mutex> lock(s_egl_display_mutex);
    for (const auto& entry : s_egl_displays) {
        if (entry.second == display) return entry.first;
    }

    const uint32_t handle = s_next_egl_display_handle++;
    if (handle == 0) {
        LOGE("EGL display handle space exhausted");
        return 0;
    }
    s_egl_displays.emplace(handle, display);
    return handle;
}

static EGLDisplay ResolveEglDisplay(uint32_t handle) {
    std::lock_guard<std::mutex> lock(s_egl_display_mutex);
    const auto it = s_egl_displays.find(handle);
    return it == s_egl_displays.end() ? EGL_NO_DISPLAY : it->second;
}

void ThunkManager::RegisterGLThunks() {
    uint8_t* mem = m_host.GetGuestMemoryBase();

    RegisterThunk("glActiveTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glActiveTexture(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glGenProgramsARB", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const uint32_t output_address = jit.Regs()[1];
        if (count < 0 ||
            (count > 0 && !GuestRangeValid(host.GetGuestMemorySize(),
                                           output_address,
                                           static_cast<size_t>(count) *
                                               sizeof(GLuint)))) {
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_VALUE;
            }
            return;
        }
        auto* output = reinterpret_cast<GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), output_address));
        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        for (GLsizei index = 0; index < count; ++index) {
            uint32_t name = s_next_legacy_program_name.fetch_add(
                1, std::memory_order_relaxed);
            if (name < kFirstLegacyProgramName) {
                name = kFirstLegacyProgramName;
                s_next_legacy_program_name.store(name + 1,
                                                  std::memory_order_relaxed);
            }
            output[index] = name;
            s_legacy_stages.try_emplace(name);
        }
    });

    RegisterThunk("glGetTextureHandleNV", [](Dynarmic::A32::Jit& jit,
                                               DynarmicHost& host) {
        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        ReturnGuestU64(jit, GetOrCreateLegacyTextureHandleLocked(
            jit.Regs()[0], 0));
    });

    RegisterThunk("glGetTextureSamplerHandleNV",
                  [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint texture = jit.Regs()[0];
        const GLuint sampler = jit.Regs()[1];
        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        const uint64_t handle = GetOrCreateLegacyTextureHandleLocked(
            texture, sampler);
        static std::atomic<uint32_t> calls{0};
        const uint32_t index = calls.fetch_add(1, std::memory_order_relaxed);
        if (index < 32) {
            LOGI("Bindless texture compatibility #%u texture=%u sampler=%u "
                 "handle=0x%016llX",
                 index + 1, texture, sampler,
                 static_cast<unsigned long long>(handle));
        }
        ReturnGuestU64(jit, handle);
    });

    RegisterThunk("glMakeTextureHandleResidentNV",
                  [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint64_t handle = GuestU64(jit);
        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        const auto found = s_legacy_texture_handles.find(handle);
        if (found == s_legacy_texture_handles.end()) {
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_VALUE;
            }
            return;
        }
        found->second.resident = true;
    });

    RegisterThunk("glMakeTextureHandleNonResidentNV",
                  [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint64_t handle = GuestU64(jit);
        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        const auto found = s_legacy_texture_handles.find(handle);
        if (found != s_legacy_texture_handles.end()) {
            found->second.resident = false;
        }
    });

    RegisterThunk("glIsTextureHandleResidentNV",
                  [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const uint64_t handle = GuestU64(jit);
        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        const auto found = s_legacy_texture_handles.find(handle);
        jit.Regs()[0] = found != s_legacy_texture_handles.end() &&
                                found->second.resident
            ? GL_TRUE
            : GL_FALSE;
    });

    RegisterThunk("glDeleteProgramsARB", [](Dynarmic::A32::Jit& jit,
                                              DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const uint32_t names_address = jit.Regs()[1];
        if (count < 0 ||
            (count > 0 && !GuestRangeValid(host.GetGuestMemorySize(),
                                           names_address,
                                           static_cast<size_t>(count) *
                                               sizeof(GLuint)))) {
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_VALUE;
            }
            return;
        }
        const auto* names = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), names_address));
        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        for (GLsizei index = 0; index < count; ++index) {
            const GLuint name = names[index];
            auto stage = s_legacy_stages.find(name);
            if (stage == s_legacy_stages.end()) continue;
            EraseLegacyPipelinesForStageLocked(name);
            if (stage->second.shader) glDeleteShader(stage->second.shader);
            s_legacy_stages.erase(stage);
            if (s_bound_legacy_vertex == name) s_bound_legacy_vertex = 0;
            if (s_bound_legacy_fragment == name) s_bound_legacy_fragment = 0;
        }
    });

    RegisterThunk("glNamedProgramStringEXT", [](Dynarmic::A32::Jit& jit,
                                                  DynarmicHost& host) {
        const GLuint name = jit.Regs()[0];
        const GLenum target = static_cast<GLenum>(jit.Regs()[1]);
        const GLenum format = static_cast<GLenum>(jit.Regs()[2]);
        const GLsizei length = static_cast<GLsizei>(jit.Regs()[3]);
        const uint32_t source_address = host.MemoryRead32(jit.Regs()[13]);
        if (!IsLegacyProgramTarget(target) || format != kProgramFormatAsciiArb) {
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_ENUM;
            }
            return;
        }
        if (length <= 0 ||
            !GuestRangeValid(host.GetGuestMemorySize(), source_address,
                             static_cast<size_t>(length))) {
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_VALUE;
            }
            return;
        }

        const char* nv_source = reinterpret_cast<const char*>(
            host.GetGuestMemoryBase() + source_address);
        if (name == 1610612847u) {
            const char* path = "/sdcard/Android/data/com.tt2kgames.Borderlands2/"
                               "files/scene-ground-original.nvfp";
            FILE* file = std::fopen(path, "wb");
            if (file) {
                const size_t written = std::fwrite(nv_source, 1,
                                                    static_cast<size_t>(length), file);
                const int close_result = std::fclose(file);
                LOGI("Scene ground NV source bytes=%d written=%zu close=%d",
                     length, written, close_result);
            }
        }
        const bool expected_header =
            length >= 9 &&
            ((target == kVertexProgramArb &&
              std::memcmp(nv_source, "!!NVvp5.0", 9) == 0) ||
             (target == kFragmentProgramArb &&
              std::memcmp(nv_source, "!!NVfp5.0", 9) == 0));
        std::string cached_glsl;
        if (!expected_header ||
            !RecoverAdjacentLegacyGlsl(host, source_address,
                                       static_cast<size_t>(length),
                                       &cached_glsl)) {
            LOGE("Legacy GLASM source recovery failed program=%u target=0x%04X "
                 "guest=0x%08X length=%d header=%d",
                 name, target, source_address, length, expected_header ? 1 : 0);
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_OPERATION;
            }
            return;
        }

        const std::string shader_name = LegacyShaderName(cached_glsl);
        const std::array<GLenum, 16> sampler_targets =
            DiscoverLegacySamplerTargets(cached_glsl, target);
        std::array<int16_t, 16> sampler_registers{};
        sampler_registers.fill(-1);
        std::array<LegacyConstantGroup, 9> constant_groups{};
        const uint64_t metadata_address64 =
            static_cast<uint64_t>(source_address) +
            static_cast<uint64_t>(length) + 1;
        if (metadata_address64 <= UINT32_MAX &&
            GuestRangeValid(host.GetGuestMemorySize(),
                            static_cast<uint32_t>(metadata_address64), 0x48)) {
            const uint8_t* metadata = host.GetGuestMemoryBase() +
                                      static_cast<uint32_t>(metadata_address64);
            for (size_t group = 0; group < constant_groups.size(); ++group) {
                std::memcpy(&constant_groups[group].first_register, metadata + group * 4, 2);
                std::memcpy(&constant_groups[group].byte_count, metadata + group * 4 + 2, 2);
            }
            for (size_t unit = 0; unit < sampler_registers.size(); ++unit) {
                uint16_t raw = 0xFFFF;
                std::memcpy(&raw, metadata + 0x24 + unit * sizeof(raw),
                            sizeof(raw));
                if (raw != 0xFFFF) {
                    sampler_registers[unit] = static_cast<int16_t>(raw);
                }
            }
        }
        const std::string source = NormalizeLegacyGlsl(cached_glsl, target);
        if (ModelAuditEnabled() && target == kVertexProgramArb &&
            constant_groups[2].first_register >= 0 &&
            constant_groups[2].byte_count >= 16) {
            static std::atomic<uint32_t> model_shaders{0};
            const uint32_t index = model_shaders.fetch_add(1, std::memory_order_relaxed);
            if (index < 32) {
                const auto& bones = constant_groups[2];
                LOGI("Model audit shader #%u stage=%u(%s) bone_first=%d "
                     "bone_bytes=%u float_first=%d float_bytes=%u GLSL=%zu",
                     index + 1, name, shader_name.c_str(), bones.first_register,
                     bones.byte_count, constant_groups[1].first_register,
                     constant_groups[1].byte_count, source.size());
                if (index < 3) {
                    size_t cursor = 0;
                    uint32_t lines = 0;
                    while (cursor < source.size() && lines < 20) {
                        const size_t end = source.find('\n', cursor);
                        const size_t line_end = end == std::string::npos ? source.size() : end;
                        const std::string_view line(source.data() + cursor, line_end - cursor);
                        if (line.find("Attr") != std::string_view::npos ||
                            line.find("VConstFloat") != std::string_view::npos) {
                            LOGI("Model audit GLSL stage=%u line=%.*s", name,
                                 static_cast<int>(std::min<size_t>(line.size(), 500)),
                                 line.data());
                            ++lines;
                        }
                        cursor = end == std::string::npos ? source.size() : end + 1;
                    }
                }
            }
        }
        // Bounded, opt-in shader-source audit for the distorted Scaleform UI.
        // It does not alter the compiled program or its vertex/fragment state.
        static const bool gfx_shader_dump = [] {
            char value[PROP_VALUE_MAX] = {};
            return __system_property_get("debug.horn.gfx_shader_dump", value) > 0 &&
                   value[0] == '1';
        }();
        if (gfx_shader_dump) {
            constexpr std::string_view names[] = {
                "GFxVertexShaderbase", "GFxPixelShaderbase",
                "GFxVertexShaderbase_text", "GFxPixelShaderbase_text",
                "GFxVertexShaderbaseAlt0", "GFxVertexShaderbase_textAlt0"};
            static std::atomic<uint32_t> dumped_mask{0};
            for (size_t slot = 0; slot < std::size(names); ++slot) {
                if (shader_name != names[slot]) continue;
                const uint32_t bit = 1u << slot;
                if ((dumped_mask.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
                    constexpr size_t kMaxDump = 20000;
                    // Android log records truncate long messages; keep each
                    // segment below the per-record limit so no GLSL is lost.
                    constexpr size_t kChunk = 650;
                    const size_t length = std::min(source.size(), kMaxDump);
                    for (size_t offset = 0; offset < length; offset += kChunk) {
                        std::string chunk = source.substr(offset, std::min(kChunk, length - offset));
                        for (char& c : chunk) if (c == '\n' || c == '\r') c = '|';
                        LOGI("GFx GLSL audit slot=%zu shader=%s offset=%zu total=%zu chunk=%s",
                             slot, shader_name.c_str(), offset, source.size(), chunk.c_str());
                    }
                }
                break;
            }
        }
        const GLuint shader = glCreateShader(LegacyShaderType(target));
        const GLchar* source_pointer = source.c_str();
        const GLint source_length = static_cast<GLint>(source.size());
        glShaderSource(shader, 1, &source_pointer, &source_length);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            const std::string log = ShaderInfoLog(shader);
            LOGE("Legacy GLASM compile failed program=%u target=0x%04X "
                 "shader=%s GLSL=%zu NV=%d: %s",
                 name, target, shader_name.c_str(), source.size(), length,
                 log.c_str());
            // Bounded, opt-in evidence for portability failures in the runtime
            // cache. Keep line numbers identical to the submitted GLSL source.
            static std::atomic<uint32_t> source_dumps{0};
            if (HornPerfEnabled() &&
                source_dumps.fetch_add(1, std::memory_order_relaxed) < 4) {
                size_t begin = 0;
                size_t line_number = 1;
                while (begin < source.size() && begin < 16384 &&
                       line_number <= 256) {
                    const size_t next = source.find('\n', begin);
                    const size_t end = next == std::string::npos
                        ? source.size() : next;
                    LOGE("Legacy GLSL source program=%u line=%zu: %.*s",
                         name, line_number,
                         static_cast<int>(std::min<size_t>(end - begin, 1024)),
                         source.data() + begin);
                    begin = end == source.size() ? end : end + 1;
                    ++line_number;
                }
            }
            glDeleteShader(shader);
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_OPERATION;
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
            LegacyAssemblyStage& stage = s_legacy_stages[name];
            EraseLegacyPipelinesForStageLocked(name);
            if (stage.shader) glDeleteShader(stage.shader);
            stage.target = target;
            stage.shader = shader;
            stage.name = shader_name;
            stage.diagnostic_source =
                SceneAuditEnabled() ||
                    (SceneTextureHashEnabled() &&
                     (shader_name == "DirectionalLightPixelShader" ||
                      shader_name == "DirectionalLightVertexShader" ||
                      shader_name == "BasePassPixelShader" ||
                      shader_name == "BasePassVertexShader")) ||
                    (name >= 1610612847u && name <= 1610612852u) ||
                    name == 1610612748u || name == 1610612749u
                    ? source : std::string{};
            stage.local_parameters.clear();
            stage.constant_groups = constant_groups;
            stage.sampler_registers = sampler_registers;
            stage.sampler_targets = sampler_targets;
        }
        const uint32_t log_index =
            s_legacy_compile_logs.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 96) {
            LOGI("Legacy GLASM compiled #%u program=%u target=0x%04X "
                 "shader=%s GLSL=%zu NV=%d",
                 log_index + 1, name, target, shader_name.c_str(),
                 source.size(), length);
            if (log_index < 6) {
                for (size_t group = 0; group < constant_groups.size(); ++group) {
                    if (constant_groups[group].first_register >= 0 && constant_groups[group].byte_count) {
                        LOGI("Legacy constant map program=%u group=%zu first=%d bytes=%u",
                             name, group, constant_groups[group].first_register,
                             constant_groups[group].byte_count);
                    }
                }
            }
        }
    });

    RegisterThunk("glNamedProgramLocalParametersI4uivEXT",
                  [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint name = jit.Regs()[0];
        const GLenum target = static_cast<GLenum>(jit.Regs()[1]);
        const GLuint first = jit.Regs()[2];
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[3]);
        const uint32_t values_address = host.MemoryRead32(jit.Regs()[13]);
        const uint64_t end = static_cast<uint64_t>(first) +
                             static_cast<uint64_t>(std::max(count, 0));
        const size_t bytes = count > 0
            ? static_cast<size_t>(count) * 4 * sizeof(uint32_t)
            : 0;
        if (!IsLegacyProgramTarget(target) || count < 0 || end > 4096 ||
            (bytes && !GuestRangeValid(host.GetGuestMemorySize(),
                                       values_address, bytes))) {
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_VALUE;
            }
            return;
        }

        std::lock_guard<std::mutex> lock(s_legacy_program_mutex);
        auto stage = s_legacy_stages.find(name);
        if (stage == s_legacy_stages.end() ||
            (stage->second.target && stage->second.target != target)) {
            if (s_deferred_gl_error == GL_NO_ERROR) {
                s_deferred_gl_error = GL_INVALID_OPERATION;
            }
            return;
        }
        if (count > 0) {
            if (name == 1610612847u) {
                const auto& group = stage->second.constant_groups[5];
                const uint32_t target_register =
                    group.first_register >= 0 ?
                    static_cast<uint32_t>(group.first_register + 4) : UINT32_MAX;
                if (target_register >= first && target_register < end) {
                    const uint32_t* input = reinterpret_cast<const uint32_t*>(
                        host.GetGuestMemoryBase() + values_address);
                    GLfloat values[4] = {};
                    std::memcpy(values, input + (target_register - first) * 4,
                                sizeof(values));
                    static std::atomic<uint32_t> logs{0};
                    if (logs.fetch_add(1, std::memory_order_relaxed) < 12) {
                        LOGI("Scene ground global4 write stage=%u first=%u count=%d "
                             "mapped_register=%u values={%.7g,%.7g,%.7g,%.7g}",
                             name, first, count, target_register,
                             values[0], values[1], values[2], values[3]);
                    }
                }
            }
            if (PresentAuditEnabled() && stage->second.name == "BinkShaders") {
                static std::atomic<uint32_t> logs{0};
                const uint32_t log_index =
                    logs.fetch_add(1, std::memory_order_relaxed);
                if (log_index < 8) {
                    const uint32_t* values = reinterpret_cast<const uint32_t*>(
                        host.GetGuestMemoryBase() + values_address);
                    const GLsizei shown = std::min<GLsizei>(count, 8);
                    LOGI("Present audit: Bink local parameters #%u program=%u "
                         "target=0x%04X first=%u count=%d",
                         log_index + 1, name, target, first, count);
                    for (GLsizei parameter = 0; parameter < shown; ++parameter) {
                        const uint32_t* words = values + parameter * 4;
                        float floats[4] = {};
                        std::memcpy(floats, words, sizeof(floats));
                        LOGI("Present audit: c[%u]={%08X,%08X,%08X,%08X} "
                             "float={%.7g,%.7g,%.7g,%.7g}",
                             first + static_cast<GLuint>(parameter),
                             words[0], words[1], words[2], words[3],
                             floats[0], floats[1], floats[2], floats[3]);
                    }
                }
            }
            // A subrange write must not truncate higher registers: globals
            // and bindless handles frequently survive a lower float update.
            if (stage->second.local_parameters.size() < static_cast<size_t>(end) * 4) {
                stage->second.local_parameters.resize(static_cast<size_t>(end) * 4, 0);
            }
            std::memcpy(stage->second.local_parameters.data() +
                            static_cast<size_t>(first) * 4,
                        host.GetGuestMemoryBase() + values_address, bytes);
        }
        RefreshBoundLegacyConstants(name);
    });

    RegisterThunk("glBindProgramARB", [](Dynarmic::A32::Jit& jit,
                                           DynarmicHost& host) {
        BindLegacyProgram(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glAttachShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glAttachShader(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glBindBuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint buffer = jit.Regs()[1];
        TrackGlBufferBinding(target, buffer);
        glBindBuffer(target, buffer);
    });

    RegisterThunk("glBindFramebuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint framebuffer = jit.Regs()[1];
        if (SceneAuditEnabled()) SceneAuditFramebufferTransition(target, framebuffer);
        if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
            s_bound_framebuffer = framebuffer;
        }
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 48, "glBindFramebuffer", &index)) {
            LOGI("GL telemetry: glBindFramebuffer #%u target=0x%04X "
                 "framebuffer=%u", index, target, framebuffer);
        }
        glBindFramebuffer(target, framebuffer);
    });

    RegisterThunk("glBindRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBindRenderbuffer(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glBindTexture", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBindTexture(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
        InvalidateLegacyTextureCache();
    });

    RegisterThunk("glBlendColor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendColor(ReadSF(jit, 0), ReadSF(jit, 1), ReadSF(jit, 2), ReadSF(jit, 3));
    });

    RegisterThunk("glBlendEquationSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendEquationSeparate(static_cast<GLenum>(jit.Regs()[0]),
                                static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glBlendFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendFunc(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glBlendFuncSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glBlendFuncSeparate(static_cast<GLenum>(jit.Regs()[0]),
                            static_cast<GLenum>(jit.Regs()[1]),
                            static_cast<GLenum>(jit.Regs()[2]),
                            static_cast<GLenum>(jit.Regs()[3]));
    });

    RegisterThunk("glBufferData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLsizeiptr size =
            static_cast<GLsizeiptr>(static_cast<int32_t>(jit.Regs()[1]));
        const void* data = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]);
        const GLenum usage = static_cast<GLenum>(jit.Regs()[3]);
        const GLuint buffer = BoundBufferForTarget(target);
        HornPerfScope perf(HornPerfCategory::BufferUpload,
                           size > 0 ? static_cast<uint64_t>(size) : 0);
        glBufferData(target, size, data, usage);
        if (buffer != 0 && size >= 0) {
            std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
            auto& shadow = s_buffer_shadows[buffer];
            shadow.resize(static_cast<size_t>(size));
            if (data && size > 0) {
                std::memcpy(shadow.data(), data, static_cast<size_t>(size));
            } else if (size > 0) {
                std::memset(shadow.data(), 0, static_cast<size_t>(size));
            }
        }
        CaptureGlError("glBufferData");
    });

    RegisterThunk("glBufferStorage", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLsizeiptr size =
            static_cast<GLsizeiptr>(static_cast<int32_t>(jit.Regs()[1]));
        const void* data = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]);
        const GLbitfield flags = static_cast<GLbitfield>(jit.Regs()[3]);
        const GLuint buffer = BoundBufferForTarget(target);
        static const auto buffer_storage_ext =
            reinterpret_cast<PFNGLBUFFERSTORAGEEXTPROC>(
                eglGetProcAddress("glBufferStorageEXT"));
        static std::atomic<bool> logged_path{false};
        HornPerfScope perf(HornPerfCategory::BufferUpload,
                           size > 0 ? static_cast<uint64_t>(size) : 0);
        if (buffer_storage_ext) {
            if (!logged_path.exchange(true, std::memory_order_relaxed)) {
                LOGI("glBufferStorage compatibility: using native glBufferStorageEXT");
            }
            buffer_storage_ext(target, size, data, flags);
        } else {
            if (!logged_path.exchange(true, std::memory_order_relaxed)) {
                LOGE("glBufferStorage compatibility: GL_EXT_buffer_storage unavailable; "
                     "falling back to glBufferData(GL_DYNAMIC_DRAW)");
            }
            glBufferData(target, size, data, GL_DYNAMIC_DRAW);
        }
        if (buffer != 0 && size >= 0) {
            std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
            auto& shadow = s_buffer_shadows[buffer];
            shadow.resize(static_cast<size_t>(size));
            if (data && size > 0) {
                std::memcpy(shadow.data(), data, static_cast<size_t>(size));
            } else if (size > 0) {
                std::memset(shadow.data(), 0, static_cast<size_t>(size));
            }
        }
        CaptureGlError("glBufferStorage");
    });

    RegisterThunk("glInvalidateBufferData", [](Dynarmic::A32::Jit& jit,
                                                 DynarmicHost& host) {
        const GLuint buffer = jit.Regs()[0];
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            LOGI("glInvalidateBufferData compatibility: ignoring optional "
                 "invalidation hint for buffer %u", buffer);
        }
        // Invalidation is only a performance hint. Retaining the existing
        // store is valid and avoids disturbing bindings or shadow contents.
        (void)host;
    });

    RegisterThunk("glBufferSubData", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLintptr offset = static_cast<GLintptr>(static_cast<int32_t>(jit.Regs()[1]));
        const GLsizeiptr size = static_cast<GLsizeiptr>(static_cast<int32_t>(jit.Regs()[2]));
        const void* data = GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]);
        const GLuint buffer = BoundBufferForTarget(target);
        HornPerfScope perf(HornPerfCategory::BufferUpload,
                           size > 0 ? static_cast<uint64_t>(size) : 0);
        glBufferSubData(target, offset, size, data);
        if (buffer != 0 && data && offset >= 0 && size >= 0) {
            std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
            const auto it = s_buffer_shadows.find(buffer);
            const uint64_t end = static_cast<uint64_t>(offset) +
                                 static_cast<uint64_t>(size);
            if (it != s_buffer_shadows.end() && end <= it->second.size()) {
                std::memcpy(it->second.data() + static_cast<size_t>(offset),
                            data, static_cast<size_t>(size));
            } else if (GlStateAuditEnabled()) {
                LOGE("GL audit: glBufferSubData has no complete shadow for buffer %u "
                     "offset=%lld size=%lld",
                     buffer, static_cast<long long>(offset),
                     static_cast<long long>(size));
            }
        }
        CaptureGlError("glBufferSubData");
    });

    RegisterThunk("glCheckFramebufferStatus", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glCheckFramebufferStatus(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glClear", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLbitfield mask = static_cast<GLbitfield>(jit.Regs()[0]);
        if (SceneAuditEnabled()) {
            GLint framebuffer = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
            if (framebuffer == 8) {
                static std::atomic<uint32_t> scene_clear_logs{0};
                if (scene_clear_logs.fetch_add(1, std::memory_order_relaxed) < 12) {
                    GLfloat color[4] = {};
                    glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
                    LOGI("Scene clear framebuffer=8 mask=0x%08X color={%g,%g,%g,%g}",
                         mask, color[0], color[1], color[2], color[3]);
                }
            }
        }
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 8, "glClear", &index)) {
            LOGI("GL telemetry: glClear #%u mask=0x%08X", index, mask);
        }
        glClear(mask);
    });

    RegisterThunk("glClearColor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        float r = *reinterpret_cast<float*>(&jit.Regs()[0]);
        float g = *reinterpret_cast<float*>(&jit.Regs()[1]);
        float b = *reinterpret_cast<float*>(&jit.Regs()[2]);
        float a = *reinterpret_cast<float*>(&jit.Regs()[3]);
        glClearColor(r, g, b, a);
    });

    RegisterThunk("glClearDepthf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glClearDepthf(ReadSF(jit, 0));
    });

    RegisterThunk("glClearStencil", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glClearStencil(static_cast<GLint>(jit.Regs()[0]));
    });

    RegisterThunk("glColorMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glColorMask(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
    });

    RegisterThunk("glCompileShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint shader = jit.Regs()[0];
        HornPerfScope perf(HornPerfCategory::ShaderCompile);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 64, "glCompileShader", &index)) {
            LOGI("GL telemetry: glCompileShader #%u shader=%u compiled=%d",
                 index, shader, compiled);
        }
        if (!compiled) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOGE("Shader %u compile failed: %s", shader, log);
        }
    });

    RegisterThunk("glTexStorage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLsizei levels = args.I32();
        const GLenum guest_format = static_cast<GLenum>(args.U32());
        const GLsizei width = args.I32();
        const GLsizei height = args.I32();
        const bool remap = DxtBlockBytes(guest_format) != 0;
        const GLenum host_format = remap ? Etc2InternalFormatForDxt(guest_format)
                                         : guest_format;
        if (!host_format) {
            LOGE("Unsupported DXT format for immutable texture storage: 0x%04X",
                 guest_format);
            return;
        }
        glTexStorage2D(target, levels, host_format, width, height);
        CaptureGlError(remap ? "glTexStorage2D(ETC2)" : "glTexStorage2D(native)");
    });

    RegisterThunk("glTexStorage3D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLsizei levels = args.I32();
        const GLenum format = static_cast<GLenum>(args.U32());
        const GLsizei width = args.I32();
        const GLsizei height = args.I32();
        const GLsizei depth = args.I32();
        if (DxtBlockBytes(format)) {
            LOGE("Unexpected DXT volume storage after offline ETC2 conversion: 0x%04X",
                 format);
            return;
        }
        glTexStorage3D(target, levels, format, width, height, depth);
        CaptureGlError("glTexStorage3D(native)");
    });

    RegisterThunk("glCompressedTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = args.I32();
        const GLenum guest_format = static_cast<GLenum>(args.U32());
        const GLsizei width = args.I32();
        const GLsizei height = args.I32();
        const GLint border = args.I32();
        const GLsizei image_size = args.I32();
        const uint32_t data_address = args.Ptr();
        static std::atomic<uint32_t> calls{0};
        uint32_t call_index = 0;
        if (ShouldLogTelemetry(calls, 32, "glCompressedTexImage2D", &call_index)) {
            LOGI("GL telemetry: glCompressedTexImage2D #%u format=0x%04X %dx%d size=%d",
                 call_index, guest_format, width, height, image_size);
        }
        HornPerfScope perf(HornPerfCategory::TextureUpload,
                           image_size > 0 ? static_cast<uint64_t>(image_size) : 0);
        if (image_size < 0) return;

        const bool remap = DxtBlockBytes(guest_format) != 0;
        size_t required = static_cast<size_t>(image_size);
        GLenum host_format = guest_format;
        if (remap) {
            host_format = Etc2InternalFormatForDxt(guest_format);
            if (!host_format || !DxtPayloadSize(guest_format, width, height, required) ||
                required > static_cast<size_t>(image_size)) {
                LOGE("Unsupported or malformed preconverted ETC2 image: guest=0x%04X %dx%d size=%d",
                     guest_format, width, height, image_size);
                return;
            }
            static std::atomic<uint32_t> remapped{0};
            uint32_t index = 0;
            if (ShouldLogTelemetry(remapped, 64, "ETC2 image upload", &index)) {
                LOGI("ETC2 image upload #%u guest=0x%04X host=0x%04X %dx%d bytes=%zu",
                     index, guest_format, host_format, width, height, required);
            }
        }
        GLint unpack_buffer = 0;
        if (HasGlES3()) {
            glGetIntegerv(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &unpack_buffer);
        }
        if (remap && data_address == 0 && unpack_buffer == 0) {
            glCompressedTexImage2D(target, level, host_format, width, height,
                                   border, static_cast<GLsizei>(required), nullptr);
            CaptureGlError("glCompressedTexImage2D(ETC2 allocation)");
            return;
        }
        bool valid = false;
        std::vector<uint8_t> source_scratch;
        const void* data = GuestTextureBytes(host, data_address,
                                             required, source_scratch, valid);
        if (!valid || !data) return;
        ScopedUnpackBufferUnbind unpack_buffer_guard;
        glCompressedTexImage2D(target, level, host_format, width, height,
                               border, static_cast<GLsizei>(required), data);
        CaptureGlError(remap ? "glCompressedTexImage2D(ETC2)"
                             : "glCompressedTexImage2D(native)");
        if (remap) {
            RecordSceneTextureUpload(target, level, width, height,
                                     host_format, data, required);
        }
    });

    RegisterThunk("glCompressedTexSubImage2D", [](Dynarmic::A32::Jit& jit,
                                                    DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = args.I32();
        const GLint xoffset = args.I32();
        const GLint yoffset = args.I32();
        const GLsizei width = args.I32();
        const GLsizei height = args.I32();
        const GLenum guest_format = static_cast<GLenum>(args.U32());
        const GLsizei image_size = args.I32();
        const uint32_t data_address = args.Ptr();
        if (image_size < 0) return;

        const bool remap = DxtBlockBytes(guest_format) != 0;
        size_t required = static_cast<size_t>(image_size);
        GLenum host_format = guest_format;
        if (remap) {
            host_format = Etc2InternalFormatForDxt(guest_format);
            if (!host_format || !DxtPayloadSize(guest_format, width, height, required) ||
                required > static_cast<size_t>(image_size)) {
                LOGE("Unsupported or malformed preconverted ETC2 subimage: guest=0x%04X %dx%d size=%d",
                     guest_format, width, height, image_size);
                return;
            }
            static std::atomic<uint32_t> remapped{0};
            uint32_t index = 0;
            if (ShouldLogTelemetry(remapped, 64, "ETC2 subimage upload", &index)) {
                LOGI("ETC2 subimage upload #%u guest=0x%04X host=0x%04X rect=%d,%d %dx%d bytes=%zu",
                     index, guest_format, host_format, xoffset, yoffset,
                     width, height, required);
            }
        }
        GLint unpack_buffer = 0;
        if (HasGlES3()) {
            glGetIntegerv(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &unpack_buffer);
        }
        if (remap && data_address == 0 && unpack_buffer == 0) {
            static std::atomic<uint32_t> null_updates{0};
            uint32_t index = 0;
            if (ShouldLogTelemetry(null_updates, 16, "null ETC2 subimage", &index)) {
                LOGI("Ignoring null ETC2 subimage #%u host=0x%04X rect=%d,%d %dx%d",
                     index, host_format, xoffset, yoffset, width, height);
            }
            return;
        }
        bool valid = false;
        std::vector<uint8_t> source_scratch;
        const void* data = GuestTextureBytes(host, data_address,
                                             required, source_scratch, valid);
        if (!valid || !data) return;
        ScopedUnpackBufferUnbind unpack_buffer_guard;
        glCompressedTexSubImage2D(target, level, xoffset, yoffset, width,
                                  height, host_format,
                                  static_cast<GLsizei>(required), data);
        CaptureGlError(remap ? "glCompressedTexSubImage2D(ETC2)"
                             : "glCompressedTexSubImage2D(native)");
        if (remap && xoffset == 0 && yoffset == 0) {
            RecordSceneTextureUpload(target, level, width, height,
                                     host_format, data, required);
        }
        if (remap) {
            static std::atomic<uint32_t> format_probes{0};
            const uint32_t probe = format_probes.fetch_add(1, std::memory_order_relaxed);
            if (probe < 16) {
                GLint actual_format = 0;
                GLint compressed = 0;
                glGetTexLevelParameteriv(target, level, GL_TEXTURE_INTERNAL_FORMAT,
                                         &actual_format);
                glGetTexLevelParameteriv(target, level, GL_TEXTURE_COMPRESSED,
                                         &compressed);
                LOGI("ETC2 GPU storage #%u level=%d internal=0x%04X compressed=%d expected=0x%04X",
                     probe + 1, level, actual_format, compressed, host_format);
                CaptureGlError("glGetTexLevelParameteriv(ETC2 storage)");
            }
        }
    });

    RegisterThunk("glCompressedTexImage3D", [](Dynarmic::A32::Jit& jit,
                                                  DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = args.I32();
        const GLenum guest_format = static_cast<GLenum>(args.U32());
        const GLsizei width = args.I32();
        const GLsizei height = args.I32();
        const GLsizei depth = args.I32();
        const GLint border = args.I32();
        const GLsizei image_size = args.I32();
        const uint32_t data_address = args.Ptr();
        if (image_size < 0) return;
        if (DxtBlockBytes(guest_format)) {
            LOGE("Unexpected DXT volume upload after offline ETC2 conversion: format=0x%04X %dx%dx%d",
                 guest_format, width, height, depth);
            return;
        }
        bool valid = false;
        std::vector<uint8_t> source_scratch;
        const void* data = GuestTextureBytes(host, data_address,
            static_cast<size_t>(image_size), source_scratch, valid);
        if (!valid) return;
        ScopedUnpackBufferUnbind unpack_buffer_guard;
        glCompressedTexImage3D(target, level, guest_format, width, height,
                               depth, border, image_size, data);
        CaptureGlError("glCompressedTexImage3D(native)");
    });

    RegisterThunk("glCompressedTexSubImage3D", [](Dynarmic::A32::Jit& jit,
                                                     DynarmicHost& host) {
        Arm32CallArgs args(jit, host);
        const GLenum target = static_cast<GLenum>(args.U32());
        const GLint level = args.I32();
        const GLint xoffset = args.I32();
        const GLint yoffset = args.I32();
        const GLint zoffset = args.I32();
        const GLsizei width = args.I32();
        const GLsizei height = args.I32();
        const GLsizei depth = args.I32();
        const GLenum guest_format = static_cast<GLenum>(args.U32());
        const GLsizei image_size = args.I32();
        const uint32_t data_address = args.Ptr();
        if (image_size < 0) return;
        if (DxtBlockBytes(guest_format)) {
            LOGE("Unexpected DXT volume subupload after offline ETC2 conversion: format=0x%04X %dx%dx%d",
                 guest_format, width, height, depth);
            return;
        }
        bool valid = false;
        std::vector<uint8_t> source_scratch;
        const void* data = GuestTextureBytes(host, data_address,
            static_cast<size_t>(image_size), source_scratch, valid);
        if (!valid) return;
        ScopedUnpackBufferUnbind unpack_buffer_guard;
        glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset,
            width, height, depth, guest_format, image_size, data);
        CaptureGlError("glCompressedTexSubImage3D(native)");
    });

    RegisterThunk("glCreateProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint program = glCreateProgram();
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 32, "glCreateProgram", &index)) {
            LOGI("GL telemetry: glCreateProgram #%u -> %u", index, program);
        }
        jit.Regs()[0] = program;
    });

    RegisterThunk("glCreateShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum type = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint shader = glCreateShader(type);
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 64, "glCreateShader", &index)) {
            LOGI("GL telemetry: glCreateShader #%u type=0x%04X -> %u",
                 index, type, shader);
        }
        jit.Regs()[0] = shader;
    });

    RegisterThunk("glCullFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glCullFace(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDeleteBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const auto* buffers = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        glDeleteBuffers(count, buffers);
        InvalidateGlAttribCache();
        if (count > 0 && buffers) {
            std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
            ForgetGlBufferBindings(count, buffers);
            for (GLsizei i = 0; i < count; ++i) {
                s_buffer_shadows.erase(buffers[i]);
                if (s_bound_array_buffer == buffers[i]) s_bound_array_buffer = 0;
                if (s_bound_element_buffer == buffers[i]) s_bound_element_buffer = 0;
            }
        }
        CaptureGlError("glDeleteBuffers");
    });

    RegisterThunk("glDeleteFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDeleteFramebuffers(jit.Regs()[0], reinterpret_cast<const GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glDeleteProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint program = jit.Regs()[0];
        glDeleteProgram(program);
        s_program_cache_valid = false;
        {
            std::unique_lock lock(s_uniform_binding_mutex);
            for (auto it = s_uniform_bindings.begin(); it != s_uniform_bindings.end();) {
                if (static_cast<GLuint>(it->first >> 32) == program) it = s_uniform_bindings.erase(it);
                else ++it;
            }
            s_uniform_binding_epoch.fetch_add(1, std::memory_order_release);
        }
        if (s_current_program == program) s_current_program = 0;
    });

    RegisterThunk("glDeleteRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDeleteRenderbuffers(jit.Regs()[0], reinterpret_cast<const GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glDeleteShader", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDeleteShader(jit.Regs()[0]);
    });

    RegisterThunk("glDeleteTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[0]);
        const auto* textures = reinterpret_cast<const GLuint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        if (SceneTextureHashEnabled() && count > 0 && textures) {
            std::lock_guard<std::mutex> lock(s_scene_texture_fingerprints_mutex);
            for (GLsizei index = 0; index < count; ++index) {
                s_scene_texture_fingerprints.erase(textures[index]);
            }
        }
        glDeleteTextures(count, textures);
        InvalidateLegacyTextureCacheAcrossContexts();
    });

    RegisterThunk("glDepthFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthFunc(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glDepthMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthMask(static_cast<GLboolean>(jit.Regs()[0]));
    });

    RegisterThunk("glDepthRangef", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDepthRangef(ReadSF(jit, 0), ReadSF(jit, 1));
    });

    RegisterThunk("glDepthBoundsEXT", [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        const double near_bound = ReadDF(jit, 0);
        const double far_bound = ReadDF(jit, 1);
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 4, "glDepthBoundsEXT", &index)) {
            LOGI("glDepthBoundsEXT #%u near=%.9g far=%.9g", index,
                 near_bound, far_bound);
        }
        if (const auto function = NativeDepthBoundsProc()) {
            function(near_bound, far_bound);
        }
    });

    RegisterThunk("glDisable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum capability = static_cast<GLenum>(jit.Regs()[0]);
        if (IsLegacyProgramTarget(capability)) return;
        if (capability == kDepthBoundsTestEXT && !NativeDepthBoundsProc()) return;
        glDisable(capability);
    });

    RegisterThunk("glDisableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glDisableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glDrawArrays", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::Draw);
        SyncGuestMappedBuffersForGpuRead(host);
        if (!RefreshLegacyProgramStateForDraw()) return;
        SceneAuditLogFinalInvocation("glDrawArrays",
                                      static_cast<GLint>(jit.Regs()[1]),
                                      static_cast<GLsizei>(jit.Regs()[2]),
                                      GL_NONE, 0, 0, 1);
        SceneAuditLogFinalQuadDraw("glDrawArrays", static_cast<GLint>(jit.Regs()[1]),
                                   static_cast<GLsizei>(jit.Regs()[2]), 1);
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 16, "glDrawArrays", &index)) {
            LOGI("GL telemetry: glDrawArrays #%u mode=0x%04X first=%d count=%d",
                 index, jit.Regs()[0], static_cast<int32_t>(jit.Regs()[1]),
                 static_cast<int32_t>(jit.Regs()[2]));
        }
        glDrawArrays(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLint>(jit.Regs()[1]), static_cast<GLsizei>(jit.Regs()[2]));
        // Bl2FogAuditCompleted();  // Pixel readback diagnosis paused.
        if (SceneAuditEnabled()) SceneAuditDrawCompleted("glDrawArrays");
        CaptureGlError("glDrawArrays");
    });

    RegisterThunk("glDrawElements", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::Draw);
        SyncGuestMappedBuffersForGpuRead(host);
        if (!RefreshLegacyProgramStateForDraw()) return;
        SceneAuditLogFinalInvocation("glDrawElements", 0,
                                      static_cast<GLsizei>(jit.Regs()[1]),
                                      static_cast<GLenum>(jit.Regs()[2]),
                                      jit.Regs()[3], 0, 1);
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 16, "glDrawElements", &index)) {
            LOGI("GL telemetry: glDrawElements #%u mode=0x%04X count=%d type=0x%04X indices=0x%08X",
                 index, jit.Regs()[0], static_cast<int32_t>(jit.Regs()[1]),
                 jit.Regs()[2], jit.Regs()[3]);
        }
        uint32_t indices_gaddr = jit.Regs()[3];
        AuditBufferBinding(
            "glDrawElements", GL_ELEMENT_ARRAY_BUFFER_BINDING,
            s_bound_element_buffer);
        const void* indices = (s_bound_element_buffer != 0) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(indices_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), indices_gaddr);
        SceneAuditLogFinalIndexedDraw("glDrawElements",
                                      static_cast<GLsizei>(jit.Regs()[1]),
                                      static_cast<GLenum>(jit.Regs()[2]),
                                      indices, 0, 1);
        glDrawElements(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLsizei>(jit.Regs()[1]),
                       static_cast<GLenum>(jit.Regs()[2]), indices);
        // Bl2FogAuditCompleted();  // Pixel readback diagnosis paused.
        if (SceneAuditEnabled()) SceneAuditDrawCompleted("glDrawElements");
        CaptureGlError("glDrawElements");
    });

    RegisterThunk("glEnable", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum capability = static_cast<GLenum>(jit.Regs()[0]);
        if (IsLegacyProgramTarget(capability)) return;
        if (capability == kDepthBoundsTestEXT && !NativeDepthBoundsProc()) return;
        glEnable(capability);
    });

    RegisterThunk("glEnableVertexAttribArray", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glEnableVertexAttribArray(jit.Regs()[0]);
    });

    RegisterThunk("glFinish", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::GpuSync);
        glFinish();
    });

    RegisterThunk("glFlush", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::GpuSync);
        glFlush();
    });

    RegisterThunk("glFramebufferRenderbuffer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFramebufferRenderbuffer(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
        s_depth_bias_cache_valid = false;
    });

    RegisterThunk("glFramebufferTexture2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLenum attachment = static_cast<GLenum>(jit.Regs()[1]);
        const GLenum texture_target = static_cast<GLenum>(jit.Regs()[2]);
        const GLuint texture = jit.Regs()[3];
        const GLint level = static_cast<GLint>(host.MemoryRead32(jit.Regs()[13]));
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 24, "glFramebufferTexture2D", &index)) {
            LOGI("GL telemetry: glFramebufferTexture2D #%u target=0x%04X "
                 "attachment=0x%04X texture_target=0x%04X texture=%u level=%d",
                 index, target, attachment, texture_target, texture, level);
        }
        glFramebufferTexture2D(target, attachment, texture_target, texture, level);
        s_depth_bias_cache_valid = false;
    });

    RegisterThunk("glFrontFace", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glFrontFace(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glGenBuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenBuffers(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenFramebuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenFramebuffers(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenRenderbuffers", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenRenderbuffers(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenTextures", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGenTextures(jit.Regs()[0], reinterpret_cast<GLuint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGenerateMipmap", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::Mipmap);
        glGenerateMipmap(static_cast<GLenum>(jit.Regs()[0]));
    });

    RegisterThunk("glGetActiveAttrib", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLsizei* length = reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        GLint* size = reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp)));
        GLenum* type = reinterpret_cast<GLenum*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 4)));
        GLchar* name = reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8)));
        glGetActiveAttrib(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], length, size, type, name);
    });

    RegisterThunk("glGetActiveUniform", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLsizei* length = reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3]));
        GLint* size = reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp)));
        GLenum* type = reinterpret_cast<GLenum*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 4)));
        GLchar* name = reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8)));
        glGetActiveUniform(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], length, size, type, name);
    });

    RegisterThunk("glGetAttribLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        jit.Regs()[0] = glGetAttribLocation(jit.Regs()[0], reinterpret_cast<const GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1])));
    });

    RegisterThunk("glGetError", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        if (s_deferred_gl_error != GL_NO_ERROR) {
            jit.Regs()[0] = s_deferred_gl_error;
            s_deferred_gl_error = GL_NO_ERROR;
        } else {
            jit.Regs()[0] = glGetError();
        }
    });

    RegisterThunk("glGetIntegerv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLenum pname = static_cast<GLenum>(jit.Regs()[0]);
        auto* value = reinterpret_cast<GLint*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        glGetIntegerv(pname, value);
        if (pname == GL_NUM_EXTENSIONS || pname == GL_MAJOR_VERSION ||
            pname == GL_MINOR_VERSION) {
            LOGI("GL telemetry: glGetIntegerv pname=0x%04X value=%d guest_out=0x%08X",
                 pname, value ? *value : -1, jit.Regs()[1]);
        }
    });

    RegisterThunk("glGetProgramInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLsizei* length = jit.Regs()[2] ?
            reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])) : nullptr;
        glGetProgramInfoLog(jit.Regs()[0], jit.Regs()[1], length,
                           reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glGetProgramiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetProgramiv(jit.Regs()[0], static_cast<GLenum>(jit.Regs()[1]),
                       reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glGetRenderbufferParameteriv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetRenderbufferParameteriv(jit.Regs()[0], jit.Regs()[1],
                                     reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    RegisterThunk("glGetShaderInfoLog", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLsizei* length = jit.Regs()[2] ?
            reinterpret_cast<GLsizei*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])) : nullptr;
        glGetShaderInfoLog(jit.Regs()[0], jit.Regs()[1], length,
                          reinterpret_cast<GLchar*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glGetShaderiv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glGetShaderiv(jit.Regs()[0], static_cast<GLenum>(jit.Regs()[1]),
                      reinterpret_cast<GLint*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2])));
    });

    // GL promises a pointer valid for the life of the context, and callers keep
    // it. One block per enum, so a capability check inside a frame loop costs
    // nothing after the first frame - GL_EXTENSIONS alone is a couple of KiB.
    RegisterThunk("glGetString", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        static std::mutex m;
        static std::unordered_map<GLenum, uint32_t> cache;
        static std::unordered_map<GLenum, std::string> filtered_strings;
        GLenum name = static_cast<GLenum>(jit.Regs()[0]);
        std::lock_guard<std::mutex> lock(m);
        auto it = cache.find(name);
        if (it == cache.end()) {
            const GLubyte* str = glGetString(name);
            const std::string native_string = str
                ? reinterpret_cast<const char*>(str) : std::string();
            if (name == GL_EXTENSIONS) {
                filtered_strings[name] = FilterHornGuestExtensions(str);
                str = reinterpret_cast<const GLubyte*>(filtered_strings[name].c_str());
            }
            else if (name == GL_VENDOR) {
                // Renderer selection in this Tegra-only build happens before
                // capability enumeration. Keep a coherent virtual NVIDIA
                // identity for the guest while all host calls still target the
                // verified GLES 3.2 context.
                filtered_strings[name] = "NVIDIA Corporation";
                str = reinterpret_cast<const GLubyte*>(filtered_strings[name].c_str());
            }
            else if (name == GL_RENDERER) {
                filtered_strings[name] = "NVIDIA Tegra";
                str = reinterpret_cast<const GLubyte*>(filtered_strings[name].c_str());
            }
            else if (name == GL_VERSION) {
                // The Tegra build selects its renderer only after parsing a
                // desktop-style 4.5 version. The actual host remains the GLES
                // 3.2 context created by the EGL bridge; used desktop calls are
                // translated explicitly instead of being sent to the driver.
                filtered_strings[name] = "4.5.0 NVIDIA 361.00";
                str = reinterpret_cast<const GLubyte*>(filtered_strings[name].c_str());
            }
            const char* guest_string = str
                ? reinterpret_cast<const char*>(str) : "";
            LOGI("GL telemetry: glGetString name=0x%04X native_len=%zu guest_len=%zu "
                 "native_prefix='%.192s' guest_prefix='%.192s'",
                 name, native_string.size(), strlen(guest_string),
                 native_string.c_str(), guest_string);
            uint32_t guest_addr = 0;
            if (str) {
                size_t len = strlen(reinterpret_cast<const char*>(str)) + 1;
                guest_addr = host.GuestAlloc(len);
                if (guest_addr) memcpy(host.GetGuestMemoryBase() + guest_addr, str, len);
            }
            it = cache.emplace(name, guest_addr).first;
        }
        jit.Regs()[0] = it->second;
    });

    RegisterThunk("glGetUniformLocation", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLuint program = jit.Regs()[0];
        const auto* name = reinterpret_cast<const GLchar*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[1]));
        const GLint location = glGetUniformLocation(program, name);
        if (name && location >= 0) {
            UniformBinding binding{reinterpret_cast<const char*>(name), ClassifyUniform(name)};
            const bool tuned = binding.tuning != UniformTuning::None;
            {
                std::unique_lock lock(s_uniform_binding_mutex);
                s_uniform_bindings.insert_or_assign(
                    UniformKey(program, location), std::move(binding));
                s_uniform_binding_epoch.fetch_add(1, std::memory_order_release);
            }
            if (tuned) {
                LOGI("Uniform tuning: program=%u location=%d name=%s",
                     program, location,
                     reinterpret_cast<const char*>(name));
            }
        }
        jit.Regs()[0] = static_cast<uint32_t>(location);
    });

    RegisterThunk("glHint", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glHint(static_cast<GLenum>(jit.Regs()[0]), static_cast<GLenum>(jit.Regs()[1]));
    });

    RegisterThunk("glLineWidth", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glLineWidth(*reinterpret_cast<float*>(&jit.Regs()[0]));
    });

    RegisterThunk("glLinkProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint prog = jit.Regs()[0];
        HornPerfScope perf(HornPerfCategory::ShaderLink);
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 64, "glLinkProgram", &index)) {
            LOGI("GL telemetry: glLinkProgram #%u program=%u linked=%d",
                 index, prog, linked);
        }
        if (!linked) {
            char log[1024];
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            LOGE("Program %u link failed: %s", prog, log);
        }
    });

    RegisterThunk("glMapBufferOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::BufferUpload);
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLenum access = static_cast<GLenum>(jit.Regs()[1]);
        const GLuint buffer = BoundBufferForTarget(target);

        if (buffer == 0) {
            LOGE("glMapBufferOES: unsupported target 0x%04X or no buffer bound", target);
            jit.Regs()[0] = 0;
            return;
        }
        if (access != GL_WRITE_ONLY_OES) {
            LOGE("glMapBufferOES: unsupported access 0x%04X for guest staging", access);
            jit.Regs()[0] = 0;
            return;
        }

        std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
        if (s_buffer_mappings.find(buffer) != s_buffer_mappings.end()) {
            LOGE("glMapBufferOES: buffer %u already has guest staging", buffer);
            jit.Regs()[0] = 0;
            return;
        }

        GLint already_mapped = GL_FALSE;
        GLint buffer_size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_MAPPED_OES, &already_mapped);
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &buffer_size);
        if (already_mapped || buffer_size <= 0) {
            LOGE("glMapBufferOES: buffer %u mapped=%d size=%d", buffer,
                 already_mapped, buffer_size);
            jit.Regs()[0] = 0;
            return;
        }
        perf.SetBytes(static_cast<uint64_t>(buffer_size));

        void* host_pointer = glMapBufferOES(target, access);
        if (!host_pointer) {
            LOGE("glMapBufferOES: host mapping failed for buffer %u", buffer);
            jit.Regs()[0] = 0;
            return;
        }

        const uint32_t guest_address = host.GuestAlloc(static_cast<size_t>(buffer_size), 16);
        if (!guest_address) {
            LOGE("glMapBufferOES: guest staging allocation failed for buffer %u (%d bytes)",
                 buffer, buffer_size);
            glUnmapBufferOES(target);
            jit.Regs()[0] = 0;
            return;
        }

        // OES_mapbuffer section 2 explicitly requires every byte the caller does
        // not write to retain its original contents. A host WRITE_ONLY mapping
        // cannot legally be read back, so preserve the store in a CPU shadow and
        // seed the guest staging block from it. Zeroing this block used to erase
        // the untouched portion of Horn's dynamic vertex/index buffers every
        // frame, producing geometry-sized black flicker.
        void* guest_pointer = GuestToHost(host.GetGuestMemoryBase(), guest_address);
        const auto shadow = s_buffer_shadows.find(buffer);
        const bool complete_shadow =
            shadow != s_buffer_shadows.end() &&
            shadow->second.size() == static_cast<size_t>(buffer_size);
        if (complete_shadow) {
            std::memcpy(guest_pointer, shadow->second.data(),
                        static_cast<size_t>(buffer_size));
        } else {
            std::memset(guest_pointer, 0, static_cast<size_t>(buffer_size));
            LOGE("glMapBufferOES: missing complete shadow for buffer %u "
                 "(have=%zu need=%d)",
                 buffer,
                 shadow == s_buffer_shadows.end() ? 0u : shadow->second.size(),
                 buffer_size);
        }
        s_buffer_mappings.emplace(buffer, GuestBufferMapping{
            target, access, static_cast<GLsizeiptr>(buffer_size), host_pointer, guest_address});
        if (GlStateAuditEnabled()) {
            static std::atomic<uint32_t> map_logs{0};
            const uint32_t index = map_logs.fetch_add(1, std::memory_order_relaxed);
            if (index < 64) {
                LOGI("GL audit: glMapBufferOES buffer=%u target=0x%04X size=%d "
                     "shadow=%d context=%p tid=%d",
                     buffer, target, buffer_size, complete_shadow,
                     reinterpret_cast<void*>(eglGetCurrentContext()), gettid());
            }
        }
        CaptureGlError("glMapBufferOES");
        jit.Regs()[0] = guest_address;
    });

    RegisterThunk("glPixelStorei", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glPixelStorei(static_cast<GLenum>(jit.Regs()[0]), jit.Regs()[1]);
    });

    RegisterThunk("glPolygonOffset", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const float factor = ReadSF(jit, 0);
        const float units = ReadSF(jit, 1);
        GLint depth_bits = 0;
        const float units_scale = CurrentPolygonOffsetUnitsScale(host, depth_bits);
        const float scaled_units = units * units_scale;
        if (GlStateAuditEnabled()) {
            static std::atomic<uint32_t> logs{0};
            const uint32_t index = logs.fetch_add(1, std::memory_order_relaxed);
            if (index < 64) {
                LOGI("GL audit: glPolygonOffset factor=%g units=%g scaledUnits=%g "
                     "scale=%g depthBits=%d framebuffer=%u enabled=%d context=%p tid=%d",
                     factor, units, scaled_units, units_scale, depth_bits,
                     s_bound_framebuffer, glIsEnabled(GL_POLYGON_OFFSET_FILL),
                     reinterpret_cast<void*>(eglGetCurrentContext()), gettid());
            }
        }
        glPolygonOffset(factor, scaled_units);
        CaptureGlError("glPolygonOffset");
    });

    RegisterThunk("glReadPixels", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        GLenum format = host.MemoryRead32(sp);
        GLenum type = host.MemoryRead32(sp + 4);
        void* pixels = GuestToHost(host.GetGuestMemoryBase(), host.MemoryRead32(sp + 8));
        HornPerfScope perf(
            HornPerfCategory::GpuSync,
            EstimatedPixelBytes(static_cast<GLsizei>(jit.Regs()[2]),
                                static_cast<GLsizei>(jit.Regs()[3]), format,
                                type));
        glReadPixels(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3], format, type, pixels);
    });

    RegisterThunk("glRenderbufferStorage", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glRenderbufferStorage(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
        s_depth_bias_cache_valid = false;
    });

    RegisterThunk("glScissor", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei w = jit.Regs()[2], h = jit.Regs()[3];
        ScaleViewportToSurface(x, y, w, h);
        glScissor(x, y, w, h);
    });

    RegisterThunk("glShaderSource", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint shader = jit.Regs()[0];
        GLsizei count = jit.Regs()[1];
        uint32_t str_array_gaddr = jit.Regs()[2];
        uint32_t len_array_gaddr = jit.Regs()[3];

        std::vector<const char*> strings(count);
        for (GLsizei i = 0; i < count; ++i) {
            uint32_t str_gaddr = host.MemoryRead32(str_array_gaddr + i * 4);
            strings[i] = reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), str_gaddr));
        }

        const GLint* lengths = len_array_gaddr ?
            reinterpret_cast<const GLint*>(GuestToHost(host.GetGuestMemoryBase(), len_array_gaddr)) : nullptr;

        std::string joined_source;
        for (GLsizei i = 0; i < count; ++i) {
            const size_t length = lengths && lengths[i] >= 0
                ? static_cast<size_t>(lengths[i])
                : std::strlen(strings[i]);
            joined_source.append(strings[i], length);
        }
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 64, "glShaderSource", &index)) {
            std::string preview = joined_source.substr(0, 192);
            std::replace(preview.begin(), preview.end(), '\n', ' ');
            std::replace(preview.begin(), preview.end(), '\r', ' ');
            LOGI("GL telemetry: glShaderSource #%u shader=%u count=%d bytes=%zu prefix='%s'",
                 index, shader, count, joined_source.size(), preview.c_str());
        }
        if (joined_source.size() <= static_cast<size_t>(std::numeric_limits<GLint>::max()) &&
            NeutralizeKnownInactiveUberIncludes(joined_source)) {
            const char* rewritten = joined_source.data();
            const GLint rewritten_length = static_cast<GLint>(joined_source.size());
            glShaderSource(shader, 1, &rewritten, &rewritten_length);
            LOGI("Neutralized inactive UE3 UberPostProcess include block for shader %u", shader);
        } else {
            glShaderSource(shader, count, strings.data(), lengths);
        }
    });

    RegisterThunk("glStencilFunc", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glStencilFunc(static_cast<GLenum>(jit.Regs()[0]),
                      static_cast<GLint>(jit.Regs()[1]),
                      static_cast<GLuint>(jit.Regs()[2]));
    });

    RegisterThunk("glStencilFuncSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glStencilFuncSeparate(static_cast<GLenum>(jit.Regs()[0]),
                              static_cast<GLenum>(jit.Regs()[1]),
                              static_cast<GLint>(jit.Regs()[2]),
                              static_cast<GLuint>(jit.Regs()[3]));
    });

    RegisterThunk("glStencilMask", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glStencilMask(static_cast<GLuint>(jit.Regs()[0]));
    });

    RegisterThunk("glStencilOp", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glStencilOp(static_cast<GLenum>(jit.Regs()[0]),
                    static_cast<GLenum>(jit.Regs()[1]),
                    static_cast<GLenum>(jit.Regs()[2]));
    });

    RegisterThunk("glStencilOpSeparate", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glStencilOpSeparate(static_cast<GLenum>(jit.Regs()[0]),
                            static_cast<GLenum>(jit.Regs()[1]),
                            static_cast<GLenum>(jit.Regs()[2]),
                            static_cast<GLenum>(jit.Regs()[3]));
    });

    RegisterThunk("glTexImage2D", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = jit.Regs()[0];
        GLint level = jit.Regs()[1];
        GLint internalformat = jit.Regs()[2];
        GLsizei width = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei height = host.MemoryRead32(sp);
        GLint border = host.MemoryRead32(sp + 4);
        GLenum format = host.MemoryRead32(sp + 8);
        GLenum type = host.MemoryRead32(sp + 12);
        uint32_t pixels_gaddr = host.MemoryRead32(sp + 16);
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 32, "glTexImage2D", &index)) {
            LOGI("GL telemetry: glTexImage2D #%u internal=0x%04X %dx%d format=0x%04X type=0x%04X pixels=0x%08X",
                 index, internalformat, width, height, format, type, pixels_gaddr);
        }
        const void* pixels = (pixels_gaddr < 0x10000) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(pixels_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), pixels_gaddr);
        HornPerfScope perf(HornPerfCategory::TextureUpload,
                           EstimatedPixelBytes(width, height, format, type));
        GLint host_internalformat = internalformat;
        const LegacyTextureChannels legacy_channels =
            TranslateLegacyGlInternalFormat(internalformat,
                                            &host_internalformat);
        GLenum host_format = TranslateLegacyGlExternalFormat(format);
        // Desktop GL accepts BGRA bytes for RGBA/sRGB storage; GLES does not.
        // Convert data uploads, while empty render targets need only the legal
        // RGBA external format (there are no bytes to swizzle in that case).
        if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE &&
            (host_internalformat == GL_RGBA8 ||
             host_internalformat == GL_SRGB8_ALPHA8)) {
            GLint unpack_buffer = 0;
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
            if (pixels_gaddr != 0 || unpack_buffer != 0) {
                if (!UploadBgraPixels2D(host, target, level, host_internalformat,
                                        width, height, border, 0, 0,
                                        pixels_gaddr, false)) {
                    LOGE("Rejected BGRA image level=%d %dx%d source=0x%08X",
                         level, width, height, pixels_gaddr);
                }
                CaptureGlError("glTexImage2D(BGRA conversion)");
                return;
            } else {
                host_format = GL_RGBA;
                static std::atomic<uint32_t> bgra_null_allocations{0};
                uint32_t translation_index = 0;
                if (ShouldLogTelemetry(bgra_null_allocations, 16,
                                       "empty BGRA render-target allocation",
                                       &translation_index)) {
                    LOGI("Empty BGRA allocation #%u internal=0x%04X "
                         "external=0x%04X->0x%04X %dx%d",
                         translation_index, host_internalformat, format,
                         host_format, width, height);
                }
            }
        }
        glTexImage2D(target, level, host_internalformat, width, height, border,
                     host_format, type, pixels);
        ApplyLegacyTextureSwizzle(target, legacy_channels);
        if (legacy_channels != LegacyTextureChannels::None) {
            static std::atomic<uint32_t> translations{0};
            uint32_t translation_index = 0;
            if (ShouldLogTelemetry(translations, 16,
                                   "legacy texture format translation",
                                   &translation_index)) {
                LOGI("Legacy texture translation #%u internal=0x%04X->0x%04X "
                     "format=0x%04X->0x%04X %dx%d",
                     translation_index, internalformat, host_internalformat,
                     format, host_format, width, height);
            }
        }
        CaptureGlError("glTexImage2D");
    });

    RegisterThunk("glTexParameterf", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = jit.Regs()[0];
        GLenum pname = jit.Regs()[1];
        const GLfloat requested = *reinterpret_cast<float*>(&jit.Regs()[2]);
        const GLfloat upgraded =
            pname == GL_TEXTURE_MIN_FILTER &&
                    requested == static_cast<GLfloat>(GL_LINEAR_MIPMAP_NEAREST)
                ? static_cast<GLfloat>(GL_LINEAR_MIPMAP_LINEAR)
                : requested;
        if (upgraded != requested && !s_logged_trilinear_upgrade) {
            LOGI("Trilinear filtering: GL_LINEAR_MIPMAP_NEAREST -> GL_LINEAR_MIPMAP_LINEAR");
            s_logged_trilinear_upgrade = true;
        }
        glTexParameterf(target, pname, upgraded);
        if (pname == GL_TEXTURE_MIN_FILTER) ForceAnisotropy(target);
    });

    RegisterThunk("glTexParameteri", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLenum target = jit.Regs()[0];
        GLenum pname = jit.Regs()[1];
        const GLint requested = static_cast<GLint>(jit.Regs()[2]);
        const GLint upgraded = UpgradeMinFilter(pname, requested);
        if (upgraded != requested && !s_logged_trilinear_upgrade) {
            LOGI("Trilinear filtering: GL_LINEAR_MIPMAP_NEAREST -> GL_LINEAR_MIPMAP_LINEAR");
            s_logged_trilinear_upgrade = true;
        }
        glTexParameteri(target, pname, upgraded);
        if (pname == GL_TEXTURE_MIN_FILTER) ForceAnisotropy(target);
    });

    RegisterThunk("glUniform1f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        GLfloat values[] = {*reinterpret_cast<float*>(&jit.Regs()[1])};
        ApplyUniformTuning(CurrentUniformTuning(location), values, 1, 1);
        glUniform1f(location, values[0]);
    });

    RegisterThunk("glUniform1fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[1]);
        const auto* source = reinterpret_cast<const GLfloat*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::vector<GLfloat> scratch;
        glUniform1fv(location, count, TunedUniformValues(location, count, 1, source, scratch));
    });

    RegisterThunk("glUniform1i", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniform1i(jit.Regs()[0], jit.Regs()[1]);
    });

    RegisterThunk("glUniform2f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        GLfloat values[] = {*reinterpret_cast<float*>(&jit.Regs()[1]), *reinterpret_cast<float*>(&jit.Regs()[2])};
        ApplyUniformTuning(CurrentUniformTuning(location), values, 1, 2);
        glUniform2f(location, values[0], values[1]);
    });

    RegisterThunk("glUniform2fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[1]);
        const auto* source = reinterpret_cast<const GLfloat*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::vector<GLfloat> scratch;
        glUniform2fv(location, count, TunedUniformValues(location, count, 2, source, scratch));
    });

    RegisterThunk("glUniform3f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        GLfloat values[] = {*reinterpret_cast<float*>(&jit.Regs()[1]),
                            *reinterpret_cast<float*>(&jit.Regs()[2]),
                            *reinterpret_cast<float*>(&jit.Regs()[3])};
        ApplyUniformTuning(CurrentUniformTuning(location), values, 1, 3);
        glUniform3f(location, values[0], values[1], values[2]);
    });

    RegisterThunk("glUniform3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[1]);
        const auto* source = reinterpret_cast<const GLfloat*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::vector<GLfloat> scratch;
        glUniform3fv(location, count, TunedUniformValues(location, count, 3, source, scratch));
    });

    RegisterThunk("glUniform4f", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        uint32_t sp = jit.Regs()[13];
        uint32_t w_bits = host.MemoryRead32(sp);
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        GLfloat values[] = {*reinterpret_cast<float*>(&jit.Regs()[1]),
                            *reinterpret_cast<float*>(&jit.Regs()[2]),
                            *reinterpret_cast<float*>(&jit.Regs()[3]),
                            *reinterpret_cast<float*>(&w_bits)};
        ApplyUniformTuning(CurrentUniformTuning(location), values, 1, 4);
        glUniform4f(location, values[0], values[1], values[2], values[3]);
    });

    RegisterThunk("glUniform4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        const GLint location = static_cast<GLint>(jit.Regs()[0]);
        const GLsizei count = static_cast<GLsizei>(jit.Regs()[1]);
        const auto* source = reinterpret_cast<const GLfloat*>(
            GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[2]));
        std::vector<GLfloat> scratch;
        glUniform4fv(location, count, TunedUniformValues(location, count, 4, source, scratch));
    });

    RegisterThunk("glUniformMatrix3fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniformMatrix3fv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                           reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glUniformMatrix4fv", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        glUniformMatrix4fv(jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                           reinterpret_cast<const GLfloat*>(GuestToHost(host.GetGuestMemoryBase(), jit.Regs()[3])));
    });

    RegisterThunk("glUnmapBufferOES", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        HornPerfScope perf(HornPerfCategory::BufferUpload);
        const GLenum target = static_cast<GLenum>(jit.Regs()[0]);
        const GLuint buffer = BoundBufferForTarget(target);
        if (buffer == 0) {
            LOGE("glUnmapBufferOES: unsupported target 0x%04X or no buffer bound", target);
            jit.Regs()[0] = GL_FALSE;
            return;
        }

        std::lock_guard<std::mutex> lock(s_buffer_mapping_mutex);
        const auto it = s_buffer_mappings.find(buffer);
        if (it == s_buffer_mappings.end() || it->second.target != target) {
            LOGE("glUnmapBufferOES: no guest staging for buffer %u target 0x%04X",
                 buffer, target);
            jit.Regs()[0] = GL_FALSE;
            return;
        }

        const GuestBufferMapping mapping = it->second;
        perf.SetBytes(mapping.size > 0 ? static_cast<uint64_t>(mapping.size) : 0);
        void* guest_pointer = GuestToHost(host.GetGuestMemoryBase(), mapping.guest_address);
        if (!guest_pointer) {
            LOGE("glUnmapBufferOES: lost guest staging for buffer %u", buffer);
            glUnmapBufferOES(target);
            host.GuestFree(mapping.guest_address);
            s_buffer_mappings.erase(it);
            jit.Regs()[0] = GL_FALSE;
            return;
        }

        if (GlStateAuditEnabled()) {
            static std::atomic<uint32_t> unmap_logs{0};
            const uint32_t index = unmap_logs.fetch_add(1, std::memory_order_relaxed);
            if (index < 64) {
                size_t changed = 0;
                const auto shadow = s_buffer_shadows.find(buffer);
                if (shadow != s_buffer_shadows.end() &&
                    shadow->second.size() == static_cast<size_t>(mapping.size)) {
                    const auto* staged = static_cast<const uint8_t*>(guest_pointer);
                    for (size_t i = 0; i < shadow->second.size(); ++i) {
                        changed += staged[i] != shadow->second[i];
                    }
                }
                LOGI("GL audit: glUnmapBufferOES buffer=%u size=%lld changed=%zu "
                     "context=%p tid=%d",
                     buffer, static_cast<long long>(mapping.size), changed,
                     reinterpret_cast<void*>(eglGetCurrentContext()), gettid());
            }
        }
        std::memcpy(mapping.host_pointer, guest_pointer, static_cast<size_t>(mapping.size));
        const GLboolean result = glUnmapBufferOES(target);
        if (result == GL_TRUE) {
            auto& shadow = s_buffer_shadows[buffer];
            shadow.resize(static_cast<size_t>(mapping.size));
            std::memcpy(shadow.data(), guest_pointer, static_cast<size_t>(mapping.size));
        } else {
            s_buffer_shadows.erase(buffer);
        }
        host.GuestFree(mapping.guest_address);
        s_buffer_mappings.erase(it);
        jit.Regs()[0] = static_cast<uint32_t>(result);
        CaptureGlError("glUnmapBufferOES");
    });

    RegisterThunk("glUseProgram", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        s_current_program = jit.Regs()[0];
        s_program_cache_valid = false;
        static std::atomic<uint32_t> calls{0};
        uint32_t index = 0;
        if (ShouldLogTelemetry(calls, 32, "glUseProgram", &index)) {
            LOGI("GL telemetry: glUseProgram #%u program=%u", index, s_current_program);
        }
        glUseProgram(s_current_program);
    });

    RegisterThunk("glVertexAttribPointer", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLuint index = jit.Regs()[0];
        GLint size = jit.Regs()[1];
        GLenum type = jit.Regs()[2];
        GLboolean normalized = jit.Regs()[3];
        uint32_t sp = jit.Regs()[13];
        GLsizei stride = host.MemoryRead32(sp);
        uint32_t ptr_gaddr = host.MemoryRead32(sp + 4);
        
        AuditBufferBinding(
            "glVertexAttribPointer", GL_ARRAY_BUFFER_BINDING,
            s_bound_array_buffer);
        const void* ptr = (s_bound_array_buffer != 0) ?
            reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr_gaddr)) :
            GuestToHost(host.GetGuestMemoryBase(), ptr_gaddr);

        if (s_bound_array_buffer == 0 && GlStateAuditEnabled()) {
            static std::atomic<uint32_t> client_array_logs{0};
            const uint32_t log_index =
                client_array_logs.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 64 || (SceneAuditEnabled() && log_index < 256)) {
                LOGI("GL audit: client vertex array index=%u ptr=0x%08X "
                     "size=%d type=0x%04X stride=%d context=%p tid=%d",
                     index, ptr_gaddr, size, type, stride,
                     reinterpret_cast<void*>(eglGetCurrentContext()), gettid());
            }
        }
        const bool cache_attrib = (GlStateCacheMode() & 4) != 0;
        const bool audit_attrib = GlStateCacheAuditEnabled();
        if (!cache_attrib && !audit_attrib) {
            glVertexAttribPointer(index, size, type, normalized, stride, ptr);
            CaptureGlError("glVertexAttribPointer");
            return;
        }
        
        AttribPointerBinding* cached = index < s_attrib_pointers.size()
            ? &s_attrib_pointers[index] : nullptr;
        const bool same_attrib = cached && cached->valid &&
            cached->size == size && cached->type == type &&
            cached->normalized == normalized && cached->stride == stride &&
            cached->pointer == reinterpret_cast<uintptr_t>(ptr) &&
            cached->buffer == s_bound_array_buffer;
        const bool valid_shape = size >= 1 && size <= 4 && stride >= 0 &&
            (normalized == GL_FALSE || normalized == GL_TRUE) &&
            (type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_SHORT ||
             type == GL_UNSIGNED_SHORT || type == GL_HALF_FLOAT ||
             type == GL_FLOAT || type == GL_FIXED || type == GL_INT ||
             type == GL_UNSIGNED_INT);
        const bool skip_attrib = cache_attrib && same_attrib &&
            valid_shape && s_bound_array_buffer != 0;
        if (audit_attrib) {
            s_gl_attrib_attempts.fetch_add(1, std::memory_order_relaxed);
            if (same_attrib) s_gl_attrib_redundant.fetch_add(1, std::memory_order_relaxed);
            if (skip_attrib) s_gl_attrib_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        if (!skip_attrib) glVertexAttribPointer(index, size, type, normalized, stride, ptr);
        if (cached) {
            cached->size = size;
            cached->type = type;
            cached->normalized = normalized;
            cached->stride = stride;
            cached->pointer = reinterpret_cast<uintptr_t>(ptr);
            cached->buffer = s_bound_array_buffer;
            cached->valid = valid_shape;
        }
        CaptureGlError("glVertexAttribPointer");
    });

    RegisterThunk("glViewport", [](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        GLint x = jit.Regs()[0], y = jit.Regs()[1];
        GLsizei w = jit.Regs()[2], h = jit.Regs()[3];
        ScaleViewportToSurface(x, y, w, h);
        glViewport(x, y, w, h);
    });
}
