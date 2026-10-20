#include "native_vulkan_thunks.h"
#include "native_vulkan_renderer.h"
#include "native_vulkan_shader_pack.h"
#include "native_host.h"
#include "thunk_manager.h"
#include "obb_pair_selection.h"
#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
std::uint32_t Address(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}
std::uint32_t Call(ThunkManager& manager, const char* name,
                   std::initializer_list<std::uint32_t> words = {}) {
    Dynarmic::A32::Jit frame;
    std::array<std::uint32_t, 12> stack{};
    assert(words.size() <= 4 + stack.size());
    std::size_t index = 0;
    for (const auto word : words) {
        if (index < 4) frame.Regs()[index] = word;
        else stack[index - 4] = word;
        ++index;
    }
    frame.Regs()[13] = Address(stack.data());
    assert(manager.DispatchNamedThunk(name, frame));
    return frame.Regs()[0];
}
void NoErrors(ThunkManager& manager) {
    assert(Call(manager, "eglGetError") == EGL_SUCCESS);
    assert(Call(manager, "glGetError") == GL_NO_ERROR);
}
void ImageCopyCheck(ThunkManager& manager) {
    std::array<GLuint, 3> textures{};
    Call(manager, "glGenTextures", {textures.size(), Address(textures.data())});
    Call(manager, "glBindTexture", {GL_TEXTURE_2D, textures[0]});
    Call(manager, "glTexStorage2D", {GL_TEXTURE_2D, 3, GL_RGBA8, 8, 8});
    std::array<std::uint8_t, 64> source{};
    for (std::size_t i = 0; i < 16; ++i) {
        source[i * 4] = 10 + i; source[i * 4 + 1] = 40 + i;
        source[i * 4 + 2] = 80 + i; source[i * 4 + 3] = 255;
    }
    Call(manager, "glTexSubImage2D", {GL_TEXTURE_2D, 1, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, Address(source.data())});
    Call(manager, "glBindTexture", {GL_TEXTURE_2D, textures[1]});
    Call(manager, "glTexStorage2D", {GL_TEXTURE_2D, 3, GL_RGBA8, 8, 8});
    // Fifteen scalar arguments exercise four core registers and eleven stack words.
    Call(manager, "glCopyImageSubData", {textures[0], GL_TEXTURE_2D, 1, 1, 1, 0,
        textures[1], GL_TEXTURE_2D, 2, 0, 0, 0, 2, 2, 1});
    GLuint framebuffer = 0;
    Call(manager, "glGenFramebuffers", {1, Address(&framebuffer)});
    Call(manager, "glBindFramebuffer", {GL_FRAMEBUFFER, framebuffer});
    Call(manager, "glFramebufferTexture2D", {GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 2});
    assert(Call(manager, "glCheckFramebufferStatus", {GL_FRAMEBUFFER}) == GL_FRAMEBUFFER_COMPLETE);
    std::array<std::uint8_t, 16> copied{}, expected{};
    for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 2; ++x)
        std::copy_n(source.data() + ((y + 1) * 4 + x + 1) * 4, 4, expected.data() + (y * 2 + x) * 4);
    Call(manager, "glReadPixels", {0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, Address(copied.data())});
    assert(copied == expected);
    NoErrors(manager);
    Call(manager, "glBindFramebuffer", {GL_FRAMEBUFFER, 0});
    Call(manager, "glDeleteFramebuffers", {1, Address(&framebuffer)});
    // Source and allocation must use the same converted format before later copies.
    Call(manager, "glBindTexture", {GL_TEXTURE_2D, textures[2]});
    Call(manager, "glTexStorage2D", {GL_TEXTURE_2D, 1, 0x8C4D, 4, 4});
    GLint internal = 0;
    Call(manager, "glGetTexLevelParameteriv", {GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, Address(&internal)});
    assert(internal == (kBl2Astc4Textures ? 0x93D0 : GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2));
    Call(manager, "glDeleteTextures", {textures.size(), Address(textures.data())});
    Call(manager, "glInvalidateBufferData", {0});
    assert(Call(manager, "glGetError") == GL_INVALID_VALUE);
    NoErrors(manager);
}
int GpuCheck(ThunkManager& manager, const char* packPath, const char* cachePath) {
    const auto fd = ::open(packPath, O_RDONLY | O_CLOEXEC);
    struct stat info{};
    if (fd < 0 || ::fstat(fd, &info) || info.st_size <= 0) {
        if (fd >= 0) ::close(fd);
        std::fprintf(stderr, "Cannot open Vulkan shader pack: %s\n", packPath);
        return 20;
    }
    auto pack = std::make_shared<bl2::vk::ShaderPack>();
    std::string error;
    const bool opened = pack->OpenFileDescriptor(fd, 0, static_cast<std::uint64_t>(info.st_size), &error);
    ::close(fd);
    if (!opened || !bl2vk::Initialize(pack, cachePath)) {
        std::fprintf(stderr, "Vulkan initialization failed: %s %s\n", error.c_str(), bl2vk::LastFailure());
        bl2vk::Shutdown();
        return 21;
    }
    bl2vk::EnableFatalDispatch();
    const auto display = Call(manager, "eglGetDisplay", {0});
    EGLint major = -1, minor = -1;
    assert(Call(manager, "eglInitialize", {display, Address(&major), Address(&minor)}) == EGL_TRUE);
    assert(major == 1 && minor == 5);
    std::uint32_t config = 0;
    EGLint count = 0;
    assert(Call(manager, "eglGetConfigs", {display, Address(&config), 1, Address(&count)}) == EGL_TRUE);
    assert(config && count == 1);
    EGLint colorType = 0x1234;
    assert(Call(manager, "eglGetConfigAttrib", {display, config, EGL_COLOR_BUFFER_TYPE, Address(&colorType)}) == EGL_TRUE);
    assert(colorType == EGL_RGB_BUFFER);
    EGLint colorAttributes[]{EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE};
    std::uint32_t colorConfig = 0;
    EGLint colorCount = -1;
    assert(Call(manager, "eglChooseConfig", {display, Address(colorAttributes), Address(&colorConfig), 1, Address(&colorCount)}) == EGL_TRUE);
    assert(colorCount == 1 && colorConfig == config);
    colorAttributes[1] = EGL_LUMINANCE_BUFFER; colorConfig = 0x1234; colorCount = -1;
    assert(Call(manager, "eglChooseConfig", {display, Address(colorAttributes), Address(&colorConfig), 1, Address(&colorCount)}) == EGL_TRUE);
    assert(colorCount == 0 && colorConfig == 0x1234);
    NoErrors(manager);

    EGLint maximum = 0;
    assert(Call(manager, "eglGetConfigAttrib", {display, config, EGL_MAX_PBUFFER_WIDTH, Address(&maximum)}) == EGL_TRUE);
    assert(maximum >= 4);
    const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLint undefinedFlags[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_CONTEXT_FLAGS_KHR, 8, EGL_NONE};
    assert(Call(manager, "eglCreateContext", {display, config, 0, Address(undefinedFlags)}) == 0);
    assert(Call(manager, "eglGetError") == EGL_BAD_ATTRIBUTE);
    const EGLint undefinedVersion[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    assert(Call(manager, "eglCreateContext", {display, config, 0, Address(undefinedVersion)}) == 0);
    assert(Call(manager, "eglGetError") == EGL_BAD_MATCH);
    const auto context = Call(manager, "eglCreateContext", {display, config, 0, Address(contextAttributes)});
    assert(context);
    EGLint value = 0;
    assert(Call(manager, "eglQueryContext", {display, context, EGL_CONTEXT_CLIENT_VERSION, Address(&value)}) == EGL_TRUE && value == 2);
    assert(Call(manager, "eglQueryContext", {display, context, EGL_RENDER_BUFFER, Address(&value)}) == EGL_TRUE && value == EGL_NONE);
    const auto emptySurface = Call(manager, "eglCreatePbufferSurface", {display, config, 0});
    assert(emptySurface);
    value = -1;
    assert(Call(manager, "eglQuerySurface", {display, emptySurface, EGL_WIDTH, Address(&value)}) == EGL_TRUE && value == 0);
    assert(Call(manager, "eglDestroySurface", {display, emptySurface}) == EGL_TRUE);
    const EGLint oversized[]{EGL_WIDTH, maximum + 1, EGL_HEIGHT, 1, EGL_NONE};
    assert(Call(manager, "eglCreatePbufferSurface", {display, config, Address(oversized)}) == 0);
    assert(Call(manager, "eglGetError") == EGL_BAD_ALLOC);
    const EGLint dimensions[]{EGL_WIDTH, 4, EGL_HEIGHT, 3, EGL_NONE};
    const auto surface = Call(manager, "eglCreatePbufferSurface", {display, config, Address(dimensions)});
    assert(surface);
    assert(Call(manager, "eglMakeCurrent", {display, surface, surface, context}) == EGL_TRUE);
    assert(Call(manager, "eglGetCurrentContext") == context);
    assert(Call(manager, "eglQuerySurface", {display, surface, EGL_RENDER_BUFFER, Address(&value)}) == EGL_TRUE && value == EGL_BACK_BUFFER);
    int width = 0, height = 0;
    assert(NativeVulkanCurrentSurfaceSize(&width, &height) && width == 4 && height == 3);
    GLint viewport[4]{};
    Call(manager, "glGetIntegerv", {GL_VIEWPORT, Address(viewport)});
    assert(viewport[2] == 4 && viewport[3] == 3);
    value = 0x1234;
    Call(manager, "glGetProgramivARB", {0x8620, GL_STENCIL_BACK_FUNC, Address(&value)});
    assert(value == 0x1234 && Call(manager, "glGetError") == GL_INVALID_ENUM);
    Call(manager, "glGetProgramivARB", {0x8620, 0x88B4, Address(&value)});
    assert(value == 4096);
    Call(manager, "glClearColor", {0x3F800000, 0, 0, 0x3F800000});
    Call(manager, "glClear", {GL_COLOR_BUFFER_BIT});
    std::array<std::uint8_t, 4> pixel{};
    // An edge pixel catches an allocated 1x1 target despite a correct EGL size query.
    Call(manager, "glReadPixels", {3, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, Address(pixel.data())});
    assert((pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
    NoErrors(manager);

    const EGLint alternateDimensions[]{EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE};
    const auto alternate = Call(manager, "eglCreatePbufferSurface", {display, config, Address(alternateDimensions)});
    assert(alternate);
    assert(Call(manager, "eglMakeCurrent", {display, alternate, alternate, context}) == EGL_TRUE);
    assert(NativeVulkanCurrentSurfaceSize(&width, &height) && width == 2 && height == 2);
    Call(manager, "glClearColor", {0, 0x3F800000, 0, 0x3F800000});
    Call(manager, "glClear", {GL_COLOR_BUFFER_BIT});
    Call(manager, "glReadPixels", {1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, Address(pixel.data())});
    assert((pixel == std::array<std::uint8_t, 4>{0, 255, 0, 255}));
    assert(Call(manager, "eglMakeCurrent", {display, surface, surface, context}) == EGL_TRUE);
    Call(manager, "glReadPixels", {3, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, Address(pixel.data())});
    assert((pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
    assert(Call(manager, "eglDestroySurface", {display, alternate}) == EGL_TRUE);
    const auto handoff = Call(manager, "eglCreateContext", {display, config, context, Address(contextAttributes)});
    assert(handoff);
    Call(manager, "glGenBuffers", {0xFFFFFFFF, 0});
    assert(Call(manager, "eglMakeCurrent", {display, surface, surface, handoff}) == EGL_TRUE);
    assert(Call(manager, "glGetError") == GL_NO_ERROR);
    Call(manager, "glReadPixels", {3, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, Address(pixel.data())});
    assert((pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
    assert(Call(manager, "eglDestroyContext", {display, handoff}) == EGL_TRUE);
    assert(Call(manager, "eglMakeCurrent", {display, surface, surface, context}) == EGL_TRUE);
    assert(Call(manager, "glGetError") == GL_INVALID_VALUE);
    NoErrors(manager);
    ImageCopyCheck(manager);

    std::uint32_t workerContext = 0, workerSurface = 0;
    GLuint sharedBuffer = 0;
    std::thread worker([&] {
        assert(Call(manager, "eglMakeCurrent", {display, surface, surface, context}) == EGL_FALSE);
        assert(Call(manager, "eglGetError") == EGL_BAD_ACCESS);
        workerContext = Call(manager, "eglCreateContext", {display, config, context, Address(contextAttributes)});
        workerSurface = Call(manager, "eglCreatePbufferSurface", {display, config, Address(dimensions)});
        assert(workerContext && workerSurface);
        assert(Call(manager, "eglMakeCurrent", {display, surface, surface, workerContext}) == EGL_FALSE);
        assert(Call(manager, "eglGetError") == EGL_BAD_ACCESS);
        assert(Call(manager, "eglMakeCurrent", {display, workerSurface, workerSurface, workerContext}) == EGL_TRUE);
        Call(manager, "glGenBuffers", {1, Address(&sharedBuffer)});
        Call(manager, "glBindBuffer", {GL_ARRAY_BUFFER, sharedBuffer});
        assert(Call(manager, "glIsBuffer", {sharedBuffer}) == GL_TRUE);
        assert(Call(manager, "eglDestroyContext", {display, workerContext}) == EGL_TRUE);
        assert(Call(manager, "eglDestroySurface", {display, workerSurface}) == EGL_TRUE);
        assert(Call(manager, "eglGetCurrentContext") == workerContext);
        NoErrors(manager);
        // Implicit thread teardown must release and retire both deferred objects.
    });
    worker.join();
    assert(Call(manager, "glIsBuffer", {sharedBuffer}) == GL_TRUE);
    assert(Call(manager, "eglQueryContext", {display, workerContext, EGL_CONFIG_ID, Address(&value)}) == EGL_FALSE);
    assert(Call(manager, "eglGetError") == EGL_BAD_CONTEXT);
    assert(Call(manager, "eglQuerySurface", {display, workerSurface, EGL_WIDTH, Address(&value)}) == EGL_FALSE);
    assert(Call(manager, "eglGetError") == EGL_BAD_SURFACE);
    Call(manager, "glDeleteBuffers", {1, Address(&sharedBuffer)});
    assert(Call(manager, "eglDestroyContext", {display, context}) == EGL_TRUE);
    assert(Call(manager, "eglDestroySurface", {display, surface}) == EGL_TRUE);
    assert(Call(manager, "eglGetCurrentContext") == context);
    assert(Call(manager, "eglMakeCurrent", {display, 0, 0, 0}) == EGL_TRUE);
    assert(Call(manager, "eglGetCurrentContext") == 0);
    assert(!NativeVulkanCurrentSurfaceSize(&width, &height));
    NoErrors(manager);
    assert(Call(manager, "eglTerminate", {display}) == EGL_TRUE);
    bl2vk::Shutdown();
    std::puts("native Vulkan thunk EGL/GPU pbuffer, mip image copy, shared resources, ownership and deferred teardown PASS");
    return 0;
}
}

int main(int argc, char** argv) {
    static_assert(sizeof(void*) == 4, "Exercise the real ARM32 ABI");
    if (!NativeVulkanThunkSelfCheck()) return 1;
    DynarmicHost host;
    ThunkManager manager(host);
    RegisterNativeVulkanThunks(manager, host);
    constexpr const char* required[]{
        // The 97 static graphics imports of the pinned original guest image.
        "eglBindAPI", "eglCreateContext", "eglCreatePbufferSurface", "eglCreateWindowSurface",
        "eglDestroySurface", "eglGetConfigAttrib", "eglGetConfigs", "eglGetCurrentContext",
        "eglGetCurrentSurface", "eglGetDisplay", "eglGetProcAddress", "eglInitialize",
        "eglMakeCurrent", "eglQueryString", "eglQuerySurface", "eglSurfaceAttrib", "eglSwapBuffers",
        "glActiveTexture", "glBeginQuery", "glBindBuffer", "glBindFramebuffer",
        "glBindRenderbuffer", "glBindSampler", "glBindTexture", "glBindVertexArray",
        "glBlendEquation", "glBlendEquationSeparate", "glBlendFunc", "glBlendFuncSeparate",
        "glBlitFramebuffer", "glBufferData", "glClear", "glClearBufferfv", "glClearColor",
        "glClearDepthf", "glClearStencil", "glClientWaitSync", "glColorMask", "glCompressedTexImage2D",
        "glCompressedTexImage3D", "glCompressedTexSubImage2D", "glCompressedTexSubImage3D",
        "glCullFace", "glDeleteBuffers", "glDeleteFramebuffers", "glDeleteQueries",
        "glDeleteRenderbuffers", "glDeleteSync", "glDeleteTextures", "glDepthFunc", "glDepthMask",
        "glDepthRangef", "glDisable", "glDisableVertexAttribArray", "glDrawArrays",
        "glDrawArraysInstanced", "glDrawBuffers", "glEnable", "glEnableVertexAttribArray",
        "glEndQuery", "glFenceSync", "glFlush", "glFramebufferRenderbuffer", "glFramebufferTexture2D",
        "glGenBuffers", "glGenFramebuffers", "glGenQueries", "glGenRenderbuffers", "glGenSamplers",
        "glGenTextures", "glGetError", "glGetIntegerv", "glGetQueryObjectuiv", "glGetString",
        "glGetStringi", "glMapBufferRange", "glPixelStorei", "glPolygonOffset", "glReadBuffer",
        "glReadPixels", "glRenderbufferStorage", "glSamplerParameteri", "glScissor", "glStencilFunc",
        "glStencilFuncSeparate", "glStencilMask", "glStencilOp", "glStencilOpSeparate", "glTexImage2D",
        "glTexImage3D", "glTexParameteri", "glTexSubImage2D", "glTexSubImage3D", "glUnmapBuffer",
        "glVertexAttribDivisor", "glVertexAttribPointer", "glViewport",
        // Required legacy and supported optional dynamic entrypoints.
        "eglChooseConfig", "dlsym", "glBufferStorage",
        "glCopyImageSubData", "glCopyImageSubDataEXT", "glCopyImageSubDataOES", "glCopyImageSubDataNV",
        "glGenProgramsARB", "glNamedProgramStringEXT",
        "glNamedProgramLocalParametersI4uivEXT", "glDrawElementsBaseVertex",
        "glDrawElementsInstancedBaseVertex", "glGetTextureSamplerHandleNV", "glMakeTextureHandleResidentNV"};
    for (const auto* name : required) if (!manager.FindRegisteredSymbol(name)) {
        std::fprintf(stderr, "Missing Vulkan facade import: %s\n", name);
        return 2;
    }
    // Missing optional SSO/indirect entrypoints must not expose the GLES driver.
    for (const auto* name : {"glCreateShaderProgramv", "glBindProgramPipeline",
                            "glMultiDrawElementsIndirectBindlessNV"})
        if (manager.FindRegisteredSymbol(name)) return 3;
    Dynarmic::A32::Jit frame;
    for (const auto* name : {"glCreateShaderProgramv", "glMultiDrawElementsIndirectBindlessNV"}) {
        frame.Regs()[0] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(name));
        if (!manager.DispatchNamedThunk("eglGetProcAddress", frame) || frame.Regs()[0] != 0) return 10;
        frame.Regs()[0] = 0;
        frame.Regs()[1] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(name));
        if (!manager.DispatchNamedThunk("dlsym", frame) || frame.Regs()[0] != 0) return 11;
    }
    constexpr const char* boundName = "glBindBuffer";
    frame.Regs()[0] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(boundName));
    if (!manager.DispatchNamedThunk("eglGetProcAddress", frame) ||
        frame.Regs()[0] != manager.FindRegisteredSymbol(boundName)) return 12;
    int width = -1, height = -1;
    if (NativeVulkanCurrentSurfaceSize(&width, &height) || width != -1 || height != -1) return 13;
    frame.Regs()[0] = GL_ARRAY_BUFFER;
    frame.Regs()[1] = 1;
    if (!manager.DispatchNamedThunk("glBindBuffer", frame)) return 4;
    if (!manager.DispatchNamedThunk("glGetError", frame) || frame.Regs()[0] != GL_INVALID_OPERATION) return 5;
    if (!manager.DispatchNamedThunk("glGetError", frame) || frame.Regs()[0] != GL_NO_ERROR) return 6;
    frame.Regs()[0] = 0;
    if (!manager.DispatchNamedThunk("eglGetDisplay", frame) || !frame.Regs()[0]) return 7;
    frame.Regs()[1] = 0;
    frame.Regs()[2] = 0;
    if (!manager.DispatchNamedThunk("eglInitialize", frame) || frame.Regs()[0] != EGL_FALSE) return 8;
    if (!manager.DispatchNamedThunk("eglGetError", frame) || frame.Regs()[0] != EGL_NOT_INITIALIZED) return 9;
    std::puts("native Vulkan thunk source/range/import/error checks PASS");
    if (argc == 3) return GpuCheck(manager, argv[1], argv[2]);
    if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [shader-pack pipeline-cache-directory]\n", argv[0]);
        return 22;
    }
    return 0;
}
