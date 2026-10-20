#pragma once
#include <GLES3/gl32.h>
#include <android/native_window.h>
#include <array>
#include "native_vulkan_seed.h"
#include <cstdint>
#include <memory>
#include <string_view>

namespace bl2::vk { class ShaderPack; }

// GL-shaped calls deliberately operate only on Vulkan-owned objects.
namespace bl2vk {
struct ConstantGroup { std::int16_t first_register = -1; std::uint16_t byte_count = 0; };
struct PipelineStats { std::uint64_t startupCreates{}, runtimeCreates{}, lateMandatoryCreates{}, successfulDraws{}, rejectedDraws{}; std::size_t resident{}, mandatory{}, candidates{}; };
PipelineStats GetPipelineStats();
bool Initialize(std::shared_ptr<bl2::vk::ShaderPack> pack, const char* cache_directory);
bool Initialize(std::shared_ptr<bl2::vk::ShaderPack> pack, const char* cache_directory, const seed::Request* mandatory);
bool IsInitialized();
void EnableFatalDispatch();
const char* LastFailure();
void Shutdown();
std::uintptr_t CreateContext(std::uintptr_t share);
bool DestroyContext(std::uintptr_t context);
bool MakeCurrent(std::uintptr_t context, ANativeWindow* window,
                 std::uint32_t pbuffer_width=1,std::uint32_t pbuffer_height=1,
                 std::uintptr_t pbuffer_id=0);
bool SwapBuffers(ANativeWindow* window);
void ReleaseWindow(ANativeWindow* window);
void ReleasePbuffer(std::uintptr_t id);
void SwapInterval(int interval);
void Flush();
void Finish();
// Persist learned driver/pipeline caches without requiring a current context.
void PersistCaches();
// JNI lifecycle notifications defer WSI work to the rendering thread.
void SetPresentationPaused(bool paused);
void NotifyPresentationSurfaceChanged(ANativeWindow* fresh);
GLenum GetError();
void RecordError(GLenum error);
void Unsupported(const char* operation);
const GLubyte* GetString(GLenum name);
const GLubyte* GetStringi(GLenum name, GLuint index);
void GetIntegerv(GLenum name, GLint* value);
void GetFloatv(GLenum name, GLfloat* value);
void GetBooleanv(GLenum name, GLboolean* value);
GLboolean IsEnabled(GLenum cap);

void GenPrograms(GLsizei count, GLuint* names);
void DeletePrograms(GLsizei count, const GLuint* names);
void BindProgram(GLenum target, GLuint name);
bool NamedProgramString(GLuint name, GLenum target, std::string_view raw_glsl,
                        const ConstantGroup* groups, const std::int16_t* sampler_registers,
                        GLsizei original_program_length=0);
void NamedProgramLocalParametersI4uiv(GLuint name, GLenum target, GLuint index,
                                    GLsizei count, const GLuint* words);
void GetProgramiv(GLenum target, GLenum pname, GLint* value);

void GenBuffers(GLsizei count, GLuint* names);
void DeleteBuffers(GLsizei count, const GLuint* names);
void BindBuffer(GLenum target, GLuint name);
GLboolean IsBuffer(GLuint name);
void BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void BufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);
void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
void CopyBufferSubData(GLenum read_target, GLenum write_target, GLintptr read_offset,
                       GLintptr write_offset, GLsizeiptr size);
void* MapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
void* MapBuffer(GLenum target, GLenum access);
GLboolean UnmapBuffer(GLenum target);
void FlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
void GetBufferParameteriv(GLenum target, GLenum pname, GLint* value);
void GetBufferPointerv(GLenum target, GLenum pname, void** value);
void GetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* output);

void GenTextures(GLsizei count, GLuint* names);
void DeleteTextures(GLsizei count, const GLuint* names);
void BindTexture(GLenum target, GLuint name);
void BindMultiTexture(GLenum unit, GLenum target, GLuint name);
void ActiveTexture(GLenum unit);
GLboolean IsTexture(GLuint name);
void TexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width,
                GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
void TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
                   GLsizei height, GLenum format, GLenum type, const void* pixels);
void CompressedTexImage2D(GLenum target, GLint level, GLenum internal_format,
                          GLsizei width, GLsizei height, GLint border, GLsizei size,
                          const void* pixels);
void CompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                             GLsizei width, GLsizei height, GLenum format,
                             GLsizei size, const void* pixels);
void TexStorage2D(GLenum target, GLsizei levels, GLenum internal_format,
                  GLsizei width, GLsizei height);
void TexImage3D(GLenum target, GLint level, GLint internal_format, GLsizei width,
                GLsizei height, GLsizei depth, GLint border, GLenum format,
                GLenum type, const void* pixels);
void TexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z,
                   GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                   GLenum type, const void* pixels);
void CompressedTexImage3D(GLenum target, GLint level, GLenum internal_format,
                          GLsizei width, GLsizei height, GLsizei depth, GLint border,
                          GLsizei size, const void* pixels);
void CompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z,
                             GLsizei width, GLsizei height, GLsizei depth,
                             GLenum format, GLsizei size, const void* pixels);
void TexStorage3D(GLenum target, GLsizei levels, GLenum internal_format,
                  GLsizei width, GLsizei height, GLsizei depth);
void TexParameteri(GLenum target, GLenum pname, GLint value);
void TexParameterf(GLenum target, GLenum pname, GLfloat value);
void TexParameterfv(GLenum target, GLenum pname, const GLfloat* values);
void GetTexParameteriv(GLenum target, GLenum pname, GLint* value);
void GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* value);
void GenerateMipmap(GLenum target);
void PixelStorei(GLenum pname, GLint value);
void CopyTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                       GLint source_x, GLint source_y, GLsizei width, GLsizei height);
void CopyImageSubData(GLuint source, GLenum source_target, GLint source_level,
                      GLint source_x, GLint source_y, GLint source_z,
                      GLuint destination, GLenum destination_target, GLint destination_level,
                      GLint destination_x, GLint destination_y, GLint destination_z,
                      GLsizei width, GLsizei height, GLsizei depth);
void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                 GLenum format, GLenum type, void* output);

void GenSamplers(GLsizei count, GLuint* names);
void DeleteSamplers(GLsizei count, const GLuint* names);
void BindSampler(GLuint unit, GLuint name);
void SamplerParameteri(GLuint name, GLenum pname, GLint value);
void SamplerParameterf(GLuint name, GLenum pname, GLfloat value);

void GenVertexArrays(GLsizei count, GLuint* names);
void DeleteVertexArrays(GLsizei count, const GLuint* names);
void BindVertexArray(GLuint name);
void EnableVertexAttribArray(GLuint index);
void DisableVertexAttribArray(GLuint index);
void VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                         GLsizei stride, const void* pointer);
void VertexAttribIPointer(GLuint index, GLint size, GLenum type,
                          GLsizei stride, const void* pointer);
void VertexAttribDivisor(GLuint index, GLuint divisor);
void VertexAttrib4fv(GLuint index, const GLfloat* value);
void GetVertexAttribiv(GLuint index, GLenum pname, GLint* value);
void GetVertexAttribPointerv(GLuint index, GLenum pname, void** value);

void GenFramebuffers(GLsizei count, GLuint* names);
void DeleteFramebuffers(GLsizei count, const GLuint* names);
void BindFramebuffer(GLenum target, GLuint name);
void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum texture_target,
                          GLuint texture, GLint level);
void FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture,
                             GLint level, GLint layer);
GLenum CheckFramebufferStatus(GLenum target);
void GetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                         GLenum pname, GLint* value);
void DrawBuffers(GLsizei count, const GLenum* buffers);
void ReadBuffer(GLenum buffer);
void BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                      GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                      GLbitfield mask, GLenum filter);
void InvalidateFramebuffer(GLenum target, GLsizei count, const GLenum* attachments);
void GenRenderbuffers(GLsizei count, GLuint* names);
void DeleteRenderbuffers(GLsizei count, const GLuint* names);
void BindRenderbuffer(GLenum target, GLuint name);
void RenderbufferStorage(GLenum target, GLenum format, GLsizei width, GLsizei height);
void RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum format,
                                    GLsizei width, GLsizei height);
void FramebufferRenderbuffer(GLenum target, GLenum attachment,
                             GLenum renderbuffer_target, GLuint renderbuffer);
void GetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* value);

void Enable(GLenum cap);
void Disable(GLenum cap);
void Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void Scissor(GLint x, GLint y, GLsizei width, GLsizei height);
void DepthRangef(GLfloat near_value, GLfloat far_value);
void DepthFunc(GLenum function);
void DepthMask(GLboolean enable);
void DepthBounds(double minimum, double maximum);
void StencilFuncSeparate(GLenum face, GLenum function, GLint reference, GLuint mask);
void StencilOpSeparate(GLenum face, GLenum fail, GLenum depth_fail, GLenum pass);
void StencilMaskSeparate(GLenum face, GLuint mask);
void BlendFuncSeparate(GLenum source_rgb, GLenum destination_rgb,
                        GLenum source_alpha, GLenum destination_alpha);
void BlendEquationSeparate(GLenum rgb, GLenum alpha);
void BlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void CullFace(GLenum face);
void FrontFace(GLenum orientation);
void PolygonOffset(GLfloat factor, GLfloat units);
void LineWidth(GLfloat width);
void ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void ClearDepthf(GLfloat depth);
void ClearStencil(GLint stencil);
void Clear(GLbitfield mask);
void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
void DrawArrays(GLenum mode, GLint first, GLsizei count);
void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances);
void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                            const void* indices, GLint base_vertex);
void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                     const void* indices, GLsizei instances, GLint base_vertex);

void GenQueries(GLsizei count, GLuint* names);
void DeleteQueries(GLsizei count, const GLuint* names);
void BeginQuery(GLenum target, GLuint name);
void EndQuery(GLenum target);
void GetQueryObjectuiv(GLuint name, GLenum pname, GLuint* value);
GLsync FenceSync(GLenum condition, GLbitfield flags);
GLenum ClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void WaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void DeleteSync(GLsync sync);
bool SelfCheck();
}
