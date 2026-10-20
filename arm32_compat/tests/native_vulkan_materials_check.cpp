#include "native_vulkan_renderer.h"
#include "native_vulkan_shader_pack.h"
#include "obb_sha256.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static_assert(sizeof(void*) == 4, "This GPU comparison must run in a native ARM32 process");
namespace {
constexpr int kSide = 32;
constexpr std::uint32_t kVS = 0x8620, kFS = 0x8804;
constexpr GLenum kLuminance = 0x1909;
constexpr std::array<unsigned char, 4> kClear{13,17,23,255};
constexpr const char* kBanks[] = {"VConstGlobal", "VConstFloat", "VConstBones", "VConstInt", "VConstBool",
                                "PConstGlobal", "PConstFloat", "PConstInt", "PConstBool"};
using Image = std::array<unsigned char, kSide * kSide * 4>;
using Words = std::vector<std::array<std::uint32_t,4>>;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::uint32_t U32(const unsigned char* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
std::uint64_t U64(const unsigned char* p) { return U32(p) | (std::uint64_t(U32(p+4)) << 32); }
ObbSha256::Digest Hash(const void* data, std::size_t size) {
    ObbSha256 hash; Require(hash.Update(static_cast<const std::uint8_t*>(data),size),"SHA input"); return hash.Finalize();
}
struct Cursor {
    const std::vector<unsigned char>& bytes;
    std::size_t at = 0;
    const unsigned char* Take(std::size_t size) {
        Require(at <= bytes.size() && size <= bytes.size()-at,"Truncated material fixture");
        const auto* p=bytes.data()+at; at+=size; return p;
    }
    std::uint32_t Word() { return U32(Take(4)); }
    std::string Text(std::size_t size) { const auto* p=Take(size); return {reinterpret_cast<const char*>(p),size}; }
};
struct Inputs {
    std::string raw, normalized;
    bl2::vk::Stage stage;
    std::array<bl2vk::ConstantGroup,9> groups{};
    Words words;
};
struct Material {
    std::string name;
    std::uint32_t flags=0, variation=0;
    std::array<std::uint32_t,4> changed{};
    Inputs vertex, fragment;
};
Inputs ReadInputs(Cursor& cursor, std::uint32_t target, const bl2::vk::ShaderPack& pack) {
    Inputs result;
    const auto rawBytes=cursor.Word(), normalizedBytes=cursor.Word(), count=cursor.Word();
    Require(rawBytes && rawBytes<=131072 && normalizedBytes && normalizedBytes<=131072 && count && count<=4096,"Fixture source/register bounds");
    for(auto& group:result.groups) {
        const auto* p=cursor.Take(4);
        const auto raw=std::uint16_t(p[0]) | (std::uint16_t(p[1])<<8);
        group.first_register=static_cast<std::int16_t>(raw);
        group.byte_count=static_cast<std::uint16_t>(std::uint16_t(p[2]) | (std::uint16_t(p[3])<<8));
        Require(group.first_register>=-1 &&
                (group.first_register<0 ? (group.byte_count==0 || group.byte_count==0xffff) :
                 group.byte_count%16==0 && std::uint32_t(group.first_register)+group.byte_count/16<=count),"Fixture private register bank bounds");
    }
    result.words.resize(count);
    for(auto& row:result.words) for(auto& word:row) word=cursor.Word();
    result.raw=cursor.Text(rawBytes); result.normalized=cursor.Text(normalizedBytes);
    std::string error;
    Require(pack.LookupRaw(target,result.raw,result.stage,&error),"Exact fixture shader identity: "+error);
    Require(result.stage.normalizedDigest==Hash(result.normalized.data(),result.normalized.size()),"Fixture normalized source differs from exact pack stage");
    for(const auto& field:result.stage.fields) {
        const auto& group=result.groups.at(field.groupIndex);
        Require(group.first_register>=0 && group.byte_count==field.count*16,"Fixture omits a declared original uniform array");
    }
    return result;
}
std::vector<Material> ReadMaterials(const char* path, const bl2::vk::ShaderPack& pack) {
    std::ifstream file(path,std::ios::binary|std::ios::ate); Require(file.good(),"Open material fixture");
    const auto size=file.tellg(); Require(size>64 && size<=1024*1024,"Material fixture file size");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size)); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),size); Require(file.good(),"Read material fixture");
    const auto caseCount=U32(bytes.data()+12);
    Require(!std::memcmp(bytes.data(),"BL2MAT1\0",8) && U32(bytes.data()+8)==1 && (caseCount==5 || caseCount==6) && U64(bytes.data()+16)==bytes.size()-64 && !U64(bytes.data()+56),"Material fixture header");
    const auto digest=Hash(bytes.data()+64,bytes.size()-64); Require(std::equal(digest.begin(),digest.end(),bytes.begin()+24),"Material fixture payload SHA");
    Cursor cursor{bytes,64}; std::vector<Material> materials;
    for(unsigned n=0;n<caseCount;++n) {
        Material material; const auto nameBytes=cursor.Word(); material.flags=cursor.Word(); material.variation=cursor.Word();
        for(auto& word:material.changed) word=cursor.Word();
        Require(nameBytes && nameBytes<=64 && (material.flags==0 || material.flags==1 || material.flags==2 || material.flags==4) && material.variation<256,"Material fixture case bounds");
        material.name=cursor.Text(nameBytes);
        Require(std::all_of(material.name.begin(),material.name.end(),[](char c){return (c>='a'&&c<='z') || c=='_';}),"Material fixture case name");
        material.vertex=ReadInputs(cursor,kVS,pack); material.fragment=ReadInputs(cursor,kFS,pack);
        Require(material.fragment.groups[6].first_register>=0 && material.variation<material.fragment.groups[6].byte_count/16,"Fragment variation bounds");
        if(material.flags&4) Require(caseCount==6 && material.name=="font_gfx_r_atlas" &&
            material.vertex.stage.attribute_locations==std::vector<std::uint32_t>{0,2,8} && material.vertex.stage.samplers.empty() &&
            material.fragment.stage.samplers.size()==1 && material.fragment.stage.samplers[0].unit==0 &&
            material.fragment.stage.samplers[0].type==bl2::vk::SamplerType::Sampler2D,"Original packed font fixture interface");
        materials.push_back(std::move(material));
    }
    Require(cursor.at==bytes.size(),"Material fixture trailing data"); return materials;
}
GLuint Shader(GLenum kind,const std::string& source) {
    const auto shader=glCreateShader(kind); const auto* text=source.data(); const auto size=static_cast<GLint>(source.size());
    glShaderSource(shader,1,&text,&size); glCompileShader(shader); GLint ok=0; glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) {std::array<char,8192> log{}; glGetShaderInfoLog(shader,static_cast<GLsizei>(log.size()),nullptr,log.data()); throw std::runtime_error(std::string("Original GLES shader compile: ")+log.data());}
    return shader;
}
GLuint Program(const Material& material) {
    const auto vertex=Shader(GL_VERTEX_SHADER,material.vertex.normalized),fragment=Shader(GL_FRAGMENT_SHADER,material.fragment.normalized);
    const auto program=glCreateProgram(); glAttachShader(program,vertex); glAttachShader(program,fragment); glLinkProgram(program);
    glDeleteShader(vertex); glDeleteShader(fragment); GLint ok=0; glGetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok) {std::array<char,8192> log{}; glGetProgramInfoLog(program,static_cast<GLsizei>(log.size()),nullptr,log.data()); throw std::runtime_error(std::string("Original GLES pair link: ")+log.data());}
    return program;
}
void Uniforms(GLuint program,const Inputs& input,const Words& words) {
    for(const auto& field:input.stage.fields) {
        const auto& group=input.groups[field.groupIndex];
        for(std::uint32_t n=0;n<field.count;++n) {
            const auto name=std::string(kBanks[field.groupIndex])+"["+std::to_string(n)+"]";
            const auto location=glGetUniformLocation(program,name.c_str()); if(location<0) continue;
            const auto& row=words.at(static_cast<std::size_t>(group.first_register)+n);
            if(field.type==bl2::vk::FieldType::Bool) glUniform1i(location,row[0]!=0);
            else if(field.type==bl2::vk::FieldType::IVec4) glUniform4iv(location,1,reinterpret_cast<const GLint*>(row.data()));
            else glUniform4fv(location,1,reinterpret_cast<const GLfloat*>(row.data()));
        }
    }
}
std::array<float,6*16*4> Vertices(const Material& material) {
    std::array<float,6*16*4> data{};
    constexpr std::array<std::array<float,2>,6> positions{{{-.65f,-.65f},{.65f,-.65f},{.65f,.65f},{-.65f,-.65f},{.65f,.65f},{-.65f,.65f}}};
    for(std::size_t n=0;n<6;++n) {
        const auto x=positions[n][0],y=positions[n][1],u=(x+.65f)/1.3f,v=(y+.65f)/1.3f;
        const auto put=[&](std::size_t location,std::array<float,4> value){std::copy(value.begin(),value.end(),data.begin()+n*64+location*4);};
        // Original mesh VSs explicitly decode packed basis components with
        // value*0.007843-1. Particle VS uses normal as a positional endpoint.
        const bool particle=(material.flags&1)!=0;
        put(0,{x,y,.2f,1}); put(1,{255,127.5f,127.5f,255}); put(2,{.3f+.3f*u,.5f+.2f*v,.4f,.35f+.3f*u});
        put(3,{.3f+.3f*u,.5f+.2f*v,.4f,.35f+.3f*u}); put(5,particle?std::array<float,4>{.5f,.5f,1,1}:std::array<float,4>{127.5f,127.5f,255,255}); put(6,{1,0,0,0}); put(7,{0,0,0,0});
        put(8,{u,v,0,1}); put(9,{0,.5f,0,1}); put(10,{0,0,0,1}); put(11,{0,0,0,1});
    }
    return data;
}
std::array<unsigned char,8*8*4> Texture(std::uint32_t unit,unsigned face) {
    std::array<unsigned char,8*8*4> result{};
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
        const auto at=(y*8+x)*4;
        result[at]=unit==1?128:static_cast<unsigned char>(55+x*16+face*3);
        result[at+1]=unit==1?128:static_cast<unsigned char>(65+y*15+unit*2);
        result[at+2]=unit==1?255:static_cast<unsigned char>(90+(x+y)*5+face*3);
        result[at+3]=static_cast<unsigned char>(105+x*7+y*3);
    }
    return result;
}
enum class Phase { Baseline, Fragment, Vertex, Restore, Bool, Clockwise, CullVisible, CullHidden, SnormPositive, SnormSigned, FontMask, FontColor, FontNoBlend, LuminanceLow, LuminanceHigh };
constexpr const char* PhaseName(Phase phase) {
    switch(phase) {case Phase::Baseline:return "baseline";case Phase::Fragment:return "fragment";case Phase::Vertex:return "vertex";case Phase::Restore:return "restore";case Phase::Bool:return "bool";case Phase::Clockwise:return "clockwise";case Phase::CullVisible:return "cull_visible";case Phase::CullHidden:return "cull_hidden";case Phase::SnormPositive:return "snorm_positive";case Phase::SnormSigned:return "snorm_signed";case Phase::FontMask:return "font_mask";case Phase::FontColor:return "font_color";case Phase::FontNoBlend:return "font_alpha_unblended";case Phase::LuminanceLow:return "luminance_low";case Phase::LuminanceHigh:return "luminance_high";}
    return "invalid";
}
bool SnormPhase(Phase phase) { return phase==Phase::SnormPositive || phase==Phase::SnormSigned; }
bool LuminancePhase(Phase phase) { return phase==Phase::LuminanceLow || phase==Phase::LuminanceHigh; }
std::array<unsigned char,8*8> SingleChannel(Phase phase,bool font) {
    std::array<unsigned char,8*8> result{};
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
        const unsigned glyph=(x%4==0 || y==1 || y==6)?255:0;
        const unsigned low=24+(x*11+y*7)%96;
        result[y*8+x]=static_cast<unsigned char>(font?(phase==Phase::FontMask?255-glyph:glyph):(phase==Phase::LuminanceHigh?255-low:low));
    }
    return result;
}
std::vector<unsigned char> FontVertices(const Material& material,Phase phase) {
    const auto logical=Vertices(material);std::vector<unsigned char> result(2+6*20,0xa5);
    const auto color=phase==Phase::FontColor?std::array<unsigned char,4>{192,48,96,96}:std::array<unsigned char,4>{64,128,224,192};
    for(std::size_t n=0;n<6;++n) {auto* record=result.data()+2+n*20;std::memcpy(record,logical.data()+n*64,8);std::memcpy(record+8,logical.data()+n*64+8*4,8);std::memcpy(record+16,color.data(),4);}
    return result;
}
void WriteInput(const char* output,const Material& material,Phase phase,const char* api,const char* suffix,const void* data,std::size_t bytes) {
    const auto path=std::string(output)+"/"+material.name+"-"+PhaseName(phase)+"-"+api+suffix;
    std::ofstream file(path,std::ios::binary);file.write(static_cast<const char*>(data),static_cast<std::streamsize>(bytes));Require(file.good(),"Write exact comparison input evidence");
}
std::array<std::int8_t,8*8*2> SnormTexture(bool signedPattern) {
    constexpr std::array<std::int8_t,8> pattern{-128,-127,-64,0,64,127,-128,127};
    std::array<std::int8_t,8*8*2> result{};
    const auto value=[&](unsigned n){const int raw=pattern[n];return static_cast<std::int8_t>(signedPattern?raw:std::min(127,std::abs(raw)));};
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {result[(y*8+x)*2]=value(x);result[(y*8+x)*2+1]=value(y);}
    return result;
}
// Both paths receive the same unmodified game code, logical rows, arrays and images.
#define API(Name, ...) (Vulkan ? bl2vk::Name(__VA_ARGS__) : gl##Name(__VA_ARGS__))
template<bool Vulkan>
Image Render(const Material& material,GLuint glesProgram,Phase phase,const char* output) {
    auto vertexWords=material.vertex.words,fragmentWords=material.fragment.words;
    if(phase==Phase::Fragment) fragmentWords.at(static_cast<std::size_t>(material.fragment.groups[6].first_register)+material.variation)=material.changed;
    if(phase==Phase::Vertex) {
        auto& column=vertexWords.at(static_cast<std::size_t>(material.vertex.groups[1].first_register));
        column[0]=std::bit_cast<std::uint32_t>(.45f);
    }
    if(phase==Phase::Bool) fragmentWords.at(static_cast<std::size_t>(material.fragment.groups[8].first_register))[0]=1;
    GLuint programs[2]{};
    if constexpr(Vulkan) {
        std::array<std::int16_t,16> samplers{}; samplers.fill(-1); bl2vk::GenPrograms(2,programs);
        Require(bl2vk::NamedProgramString(programs[0],kVS,material.vertex.raw,material.vertex.groups.data(),samplers.data()),std::string("Vulkan exact VS: ")+bl2vk::LastFailure());
        Require(bl2vk::NamedProgramString(programs[1],kFS,material.fragment.raw,material.fragment.groups.data(),samplers.data()),std::string("Vulkan exact FS: ")+bl2vk::LastFailure());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[0],kVS,0,static_cast<GLsizei>(vertexWords.size()),vertexWords.front().data());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[1],kFS,0,static_cast<GLsizei>(fragmentWords.size()),fragmentWords.front().data());
        bl2vk::BindProgram(kVS,programs[0]); bl2vk::BindProgram(kFS,programs[1]);
    } else {glUseProgram(glesProgram);Uniforms(glesProgram,material.vertex,vertexWords);Uniforms(glesProgram,material.fragment,fragmentWords);}
    GLuint target=0,framebuffer=0,buffer=0,array=0,elements=0; API(GenTextures,1,&target); API(ActiveTexture,GL_TEXTURE0); API(BindTexture,GL_TEXTURE_2D,target);
    API(TexImage2D,GL_TEXTURE_2D,0,GL_RGBA8,kSide,kSide,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    API(GenFramebuffers,1,&framebuffer); API(BindFramebuffer,GL_FRAMEBUFFER,framebuffer); API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    Require(API(CheckFramebufferStatus,GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Comparison FBO complete");
    std::vector<GLuint> textures;
    std::array<bool,16> used2d{},usedCube{};
    for(const auto* input:{&material.vertex,&material.fragment}) for(const auto& sampler:input->stage.samplers) {
        const bool cube=sampler.type==bl2::vk::SamplerType::SamplerCube;
        Require(sampler.type!=bl2::vk::SamplerType::Sampler2DShadow,"These comparison cases require a dedicated shadow fixture for depth samplers");
        if constexpr(!Vulkan) {const auto name=std::string(input->stage.target==kVS?"VSampler":"PSampler")+std::to_string(sampler.unit);const auto location=glGetUniformLocation(glesProgram,name.c_str());if(location>=0)glUniform1i(location,static_cast<GLint>(sampler.unit));}
        auto& used=cube?usedCube[sampler.unit]:used2d[sampler.unit]; if(used)continue;used=true;
        GLuint texture=0; API(GenTextures,1,&texture); textures.push_back(texture); API(ActiveTexture,GL_TEXTURE0+sampler.unit);
        const GLenum kind=cube?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_2D; API(BindTexture,kind,texture);
        API(TexParameteri,kind,GL_TEXTURE_MIN_FILTER,GL_NEAREST); API(TexParameteri,kind,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        API(TexParameteri,kind,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); API(TexParameteri,kind,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        // Actual particle PS reads unit1.G into blend alpha; the static BasePass
        // PS reads unit0.RGB into diffuse color. Both retain their exact sources.
        const auto signedUnit=(material.flags&1)?1U:0U;
        if(!cube && (((material.flags&4) && sampler.unit==0) || (LuminancePhase(phase) && sampler.unit==1))) {
            const bool font=(material.flags&4)!=0;const auto pixels=SingleChannel(phase,font);
            API(PixelStorei,GL_UNPACK_ALIGNMENT,1);
            if constexpr(Vulkan) bl2vk::TexImage2D(GL_TEXTURE_2D,0,font?GL_R8:kLuminance,8,8,0,font?GL_RED:kLuminance,GL_UNSIGNED_BYTE,pixels.data());
            else {
                glTexImage2D(GL_TEXTURE_2D,0,GL_R8,8,8,0,GL_RED,GL_UNSIGNED_BYTE,pixels.data());
                // Exact original GLES adapter semantics for guest luminance.
                if(!font) {glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R,GL_RED);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_G,GL_RED);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_B,GL_RED);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_A,GL_ONE);}
            }
            WriteInput(output,material,phase,Vulkan?"vulkan":"gles","-input.r8",pixels.data(),pixels.size());
        } else if(SnormPhase(phase) && !cube && sampler.unit==signedUnit) {
            const auto pixels=SnormTexture(phase==Phase::SnormSigned);
            API(PixelStorei,GL_UNPACK_ALIGNMENT,1);
            API(TexImage2D,GL_TEXTURE_2D,0,GL_RG8_SNORM,8,8,0,GL_RG,GL_BYTE,pixels.data());
            const auto path=std::string(output)+"/"+material.name+"-"+PhaseName(phase)+"-"+(Vulkan?"vulkan":"gles")+"-input.rg8snorm";
            std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));Require(file.good(),"Write exact signed texture input evidence");
        } else for(unsigned face=0;face<(cube?6U:1U);++face) {const auto pixels=Texture(sampler.unit,face);API(TexImage2D,cube?GL_TEXTURE_CUBE_MAP_POSITIVE_X+face:GL_TEXTURE_2D,0,GL_RGBA8,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());}
    }
    API(GenVertexArrays,1,&array); API(BindVertexArray,array); API(GenBuffers,1,&buffer); API(BindBuffer,GL_ARRAY_BUFFER,buffer);
    if(material.flags&4) {
        const auto vertices=FontVertices(material,phase);const std::uintptr_t origin=Vulkan?2:0;
        // Original GLES text workaround copies unchanged records to aligned origin.
        API(BufferData,GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(Vulkan?vertices.size():vertices.size()-2),vertices.data()+(Vulkan?0:2),GL_STATIC_DRAW);
        for(const auto location:{0u,2u,8u}) {API(EnableVertexAttribArray,location);API(VertexAttribPointer,location,location==2?4:2,location==2?GL_UNSIGNED_BYTE:GL_FLOAT,location==2?GL_TRUE:GL_FALSE,20,reinterpret_cast<const void*>(origin+(location==2?16:location==8?8:0)));}
        constexpr std::array<std::uint16_t,6> indices{0,1,2,3,4,5};API(GenBuffers,1,&elements);API(BindBuffer,GL_ELEMENT_ARRAY_BUFFER,elements);API(BufferData,GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices.data(),GL_STATIC_DRAW);
        WriteInput(output,material,phase,Vulkan?"vulkan":"gles","-input.vertices20",vertices.data(),vertices.size());
    } else {
        const auto vertices=Vertices(material);API(BufferData,GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(sizeof(vertices)),vertices.data(),GL_STATIC_DRAW);
        for(const auto location:material.vertex.stage.attribute_locations) {API(EnableVertexAttribArray,location);API(VertexAttribPointer,location,4,GL_FLOAT,GL_FALSE,16*4*sizeof(float),reinterpret_cast<const void*>(std::uintptr_t(location)*4*sizeof(float)));}
    }
    API(Viewport,0,0,kSide,kSide); API(Disable,GL_SCISSOR_TEST); API(Disable,GL_DEPTH_TEST); API(Disable,GL_STENCIL_TEST); API(Disable,GL_CULL_FACE); API(Disable,GL_BLEND);
    API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); API(FrontFace,phase==Phase::Clockwise || phase==Phase::CullHidden?GL_CW:GL_CCW);
    if(phase==Phase::CullVisible || phase==Phase::CullHidden) {API(Enable,GL_CULL_FACE);API(CullFace,GL_BACK);}
    if((material.flags&1) || ((material.flags&4) && phase!=Phase::FontNoBlend)) {API(Enable,GL_BLEND);API(BlendFuncSeparate,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);API(BlendEquationSeparate,GL_FUNC_ADD,GL_FUNC_ADD);}
    API(ClearColor,kClear[0]/255.f,kClear[1]/255.f,kClear[2]/255.f,kClear[3]/255.f); API(Clear,GL_COLOR_BUFFER_BIT);
    if(material.flags&4) API(DrawElements,GL_TRIANGLES,6,GL_UNSIGNED_SHORT,nullptr);else API(DrawArrays,GL_TRIANGLES,0,6);API(Finish);
    Image image{}; image.fill(0xcd); API(PixelStorei,GL_PACK_ALIGNMENT,1); API(ReadPixels,0,0,kSide,kSide,GL_RGBA,GL_UNSIGNED_BYTE,image.data());
    const auto error=API(GetError); Require(error==GL_NO_ERROR,std::string(Vulkan?"Vulkan":"GLES")+" material "+material.name+"/"+PhaseName(phase)+" error="+std::to_string(error)+(Vulkan?std::string(" ")+bl2vk::LastFailure():""));
    API(BindFramebuffer,GL_FRAMEBUFFER,0); API(DeleteFramebuffers,1,&framebuffer); API(DeleteTextures,1,&target);if(!textures.empty())API(DeleteTextures,static_cast<GLsizei>(textures.size()),textures.data()); API(DeleteBuffers,1,&buffer);if(elements)API(DeleteBuffers,1,&elements);API(DeleteVertexArrays,1,&array);
    if constexpr(Vulkan) bl2vk::DeletePrograms(2,programs);
    return image;
}
#undef API
std::size_t Visible(const Image& image) {
    std::size_t changed=0;
    for(std::size_t i=0;i<image.size();i+=4) if(image[i]!=kClear[0] || image[i+1]!=kClear[1] || image[i+2]!=kClear[2]) ++changed;
    return changed;
}
std::size_t Difference(const Image& a,const Image& b) {
    std::size_t changed=0; for(std::size_t i=0;i<a.size();i+=4) if(std::abs(int(a[i])-b[i])+std::abs(int(a[i+1])-b[i+1])+std::abs(int(a[i+2])-b[i+2])>5) ++changed;return changed;
}
std::size_t Colored(const Image& image) {
    std::size_t colored=0;
    for(std::size_t i=0;i<image.size();i+=4) if((image[i]!=kClear[0] || image[i+1]!=kClear[1] || image[i+2]!=kClear[2]) && unsigned(image[i])+image[i+1]+image[i+2]>=32) ++colored;
    return colored;
}
std::array<std::size_t,2> FontCoverage(const Image& image,bool alpha) {
    std::array<std::size_t,2> counts{};
    // This rectangle is strictly inside the unchanged synthetic font quad.
    for(unsigned y=8;y<24;++y) for(unsigned x=8;x<24;++x) {
        const auto at=(y*kSide+x)*4;
        if(alpha) {counts[0]+=image[at+3]<=3;counts[1]+=image[at+3]>=64;}
        else {const bool clear=image[at]==kClear[0] && image[at+1]==kClear[1] && image[at+2]==kClear[2];counts[0]+=clear;counts[1]+=!clear;}
    }
    return counts;
}
bool Compare(const Material& material,Phase phase,const Image& reference,const Image& candidate,const char* output) {
    unsigned maximum=0;std::size_t sum=0,beyond=0;
    for(std::size_t i=0;i<reference.size();++i) {const auto difference=static_cast<unsigned>(std::abs(int(reference[i])-candidate[i]));maximum=std::max(maximum,difference);sum+=difference;beyond+=difference>3;}
    const double mean=double(sum)/reference.size();const auto glesVisible=Visible(reference),vkVisible=Visible(candidate);
    const bool visibleRequired=phase==Phase::Baseline || phase==Phase::Fragment || phase==Phase::Vertex || phase==Phase::Restore || phase==Phase::CullVisible || SnormPhase(phase) || LuminancePhase(phase) || phase==Phase::FontMask || phase==Phase::FontColor || phase==Phase::FontNoBlend;
    const auto glesColored=Colored(reference),vkColored=Colored(candidate);
    const bool pass=maximum<=8 && mean<=.6 && beyond<=reference.size()/50 && (!visibleRequired || (glesVisible>=64 && vkVisible>=64 && glesColored>=32 && vkColored>=32));
    std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"gles_changed_rgb_pixels\":%zu,\"vulkan_changed_rgb_pixels\":%zu,\"gles_nonzero_colored_pixels\":%zu,\"vulkan_nonzero_colored_pixels\":%zu,\"max_channel_error\":%u,\"mean_channel_error\":%.6f,\"channels_error_gt3\":%zu,\"status\":\"%s\"}\n",material.name.c_str(),PhaseName(phase),glesVisible,vkVisible,glesColored,vkColored,maximum,mean,beyond,pass?"PASS":"FAIL");
    for(const auto& [image,suffix]:{std::pair{&reference,"gles"},std::pair{&candidate,"vulkan"}}) {
        const auto path=std::string(output)+"/"+material.name+"-"+PhaseName(phase)+"-"+suffix+".rgba";
        std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(image->data()),static_cast<std::streamsize>(image->size()));Require(file.good(),"Write GPU pixel evidence");
    }
    return pass;
}
} // namespace

int main(int argc,char** argv) {
    if(argc!=4) {std::fputs("usage: native_vulkan_materials_check PACK FIXTURES OUTPUT_DIRECTORY\n",stderr);return 2;}
    try {
        const auto fd=::open(argv[1],O_RDONLY|O_CLOEXEC);Require(fd>=0,"Open actual Vulkan shader pack");struct stat64 info{};Require(::fstat64(fd,&info)==0 && info.st_size>0,"Vulkan shader pack descriptor size");
        auto pack=std::make_shared<bl2::vk::ShaderPack>();std::string error;const auto opened=pack->OpenFileDescriptor(fd,0,static_cast<std::uint64_t>(info.st_size),&error);::close(fd);Require(opened,"Shader pack: "+error);
        const auto materials=ReadMaterials(argv[2],*pack);
        const auto display=eglGetDisplay(EGL_DEFAULT_DISPLAY);Require(eglInitialize(display,nullptr,nullptr) && eglBindAPI(EGL_OPENGL_ES_API),"GLES reference EGL initialization");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        EGLConfig config{};EGLint count=0;Require(eglChooseConfig(display,attributes,&config,1,&count) && count==1,"GLES reference EGL config");
        const EGLint size[]{EGL_WIDTH,kSide,EGL_HEIGHT,kSide,EGL_NONE},version[]{EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
        const auto surface=eglCreatePbufferSurface(display,config,size),context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);
        Require(surface!=EGL_NO_SURFACE && context!=EGL_NO_CONTEXT && eglMakeCurrent(display,surface,surface,context),"GLES3.2 reference context");
        std::printf("ARM32 GLES reference GPU: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
        Require(bl2vk::Initialize(pack,argv[3]),std::string("Direct Vulkan initialize: ")+bl2vk::LastFailure());const auto vkContext=bl2vk::CreateContext(0);Require(vkContext && bl2vk::MakeCurrent(vkContext,nullptr),"Direct Vulkan offscreen context");
        bool passed=true;
        for(const auto& material:materials) {
            GLuint program=0;
            try {
                program=Program(material);
                Image baselineGles{},baselineVk{},positiveGles{},positiveVk{},lowGles{},lowVk{};
                std::vector<Phase> phases{Phase::Baseline,Phase::Fragment,Phase::Vertex,Phase::Restore};
                if(material.flags&2) phases.insert(phases.end(),{Phase::Bool,Phase::Clockwise,Phase::CullVisible,Phase::CullHidden});
                if((material.flags&1) || material.name=="landscape_static_representative") phases.insert(phases.end(),{Phase::SnormPositive,Phase::SnormSigned});
                if(material.flags&1) phases.insert(phases.end(),{Phase::LuminanceLow,Phase::LuminanceHigh});
                if(material.flags&4) phases.insert(phases.end(),{Phase::FontMask,Phase::FontColor,Phase::FontNoBlend});
                for(const auto phase:phases) {
                    const auto reference=Render<false>(material,program,phase,argv[3]),candidate=Render<true>(material,0,phase,argv[3]);
                    passed=Compare(material,phase,reference,candidate,argv[3]) && passed;
                    if(phase==Phase::Baseline) {baselineGles=reference;baselineVk=candidate;}
                    else if(phase==Phase::SnormPositive) {positiveGles=reference;positiveVk=candidate;}
                    else if(phase==Phase::LuminanceLow) {lowGles=reference;lowVk=candidate;}
                    else if(phase==Phase::LuminanceHigh) {
                        const auto glesChanged=Difference(lowGles,reference),vkChanged=Difference(lowVk,candidate);
                        const bool observable=glesChanged>=16 && vkChanged>=16;
                        std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"check\":\"observable_luminance_G_alpha\",\"gles_changed_pixels\":%zu,\"vulkan_changed_pixels\":%zu,\"status\":\"%s\"}\n",material.name.c_str(),PhaseName(phase),glesChanged,vkChanged,observable?"PASS":"FAIL");passed=observable && passed;
                    }
                    else if(phase==Phase::SnormSigned) {
                        const auto glesChanged=Difference(positiveGles,reference),vkChanged=Difference(positiveVk,candidate);
                        const bool observable=glesChanged>=16 && vkChanged>=16;
                        std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"check\":\"observable_signed_sampler\",\"gles_changed_pixels\":%zu,\"vulkan_changed_pixels\":%zu,\"status\":\"%s\"}\n",material.name.c_str(),PhaseName(phase),glesChanged,vkChanged,observable?"PASS":"FAIL");
                        passed=observable && passed;
                    }
                    else if(phase==Phase::Fragment || phase==Phase::Vertex || phase==Phase::Bool || phase==Phase::Clockwise || phase==Phase::FontMask || phase==Phase::FontColor) {
                        const auto glesChanged=Difference(baselineGles,reference),vkChanged=Difference(baselineVk,candidate);
                        const bool observable=glesChanged>=16 && vkChanged>=16;
                        std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"check\":\"observable_change\",\"gles_changed_pixels\":%zu,\"vulkan_changed_pixels\":%zu,\"status\":\"%s\"}\n",material.name.c_str(),PhaseName(phase),glesChanged,vkChanged,observable?"PASS":"FAIL");
                        passed=observable && passed;
                    } else if(phase==Phase::Restore || phase==Phase::CullVisible) {
                        const bool exact=baselineGles==reference && baselineVk==candidate;
                        std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"check\":\"exact_baseline\",\"status\":\"%s\"}\n",material.name.c_str(),PhaseName(phase),exact?"PASS":"FAIL");
                        passed=exact && passed;
                    } else if(phase==Phase::CullHidden) {
                        const bool hidden=Visible(reference)==0 && Visible(candidate)==0;
                        std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"check\":\"fully_culled\",\"status\":\"%s\"}\n",material.name.c_str(),PhaseName(phase),hidden?"PASS":"FAIL");
                        passed=hidden && passed;
                    }
                    if((material.flags&4) && phase!=Phase::Vertex) {
                        const bool alpha=phase==Phase::FontNoBlend;
                        const auto gles=FontCoverage(reference,alpha),vk=FontCoverage(candidate,alpha);
                        const bool coverage=gles[0]>=16 && gles[1]>=16 && vk[0]>=16 && vk[1]>=16;
                        std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"check\":\"%s\",\"gles_zero_or_clear\":%zu,\"gles_covered\":%zu,\"vulkan_zero_or_clear\":%zu,\"vulkan_covered\":%zu,\"status\":\"%s\"}\n",material.name.c_str(),PhaseName(phase),alpha?"direct_alpha_mask":"glyph_mask_shape",gles[0],gles[1],vk[0],vk[1],coverage?"PASS":"FAIL");passed=coverage && passed;
                    }
                }
            } catch(const std::exception& failure) {
                passed=false;
                std::fprintf(stderr,"FAIL material %s: %s\n",material.name.c_str(),failure.what());
            }
            if(program)glDeleteProgram(program);
        }
        bl2vk::Finish();bl2vk::DestroyContext(vkContext);bl2vk::Shutdown();eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
        if(passed)std::printf("PASS %zu original game shader pairs: nonzero pixels, independent parameters, exact restore, face/cull, signed RG8, one-channel alpha and packed GFx parity versus native GLES\n",materials.size());else std::puts("FAIL material pixel comparison");return passed?0:3;
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL native Vulkan material check: %s\n",error.what());bl2vk::Shutdown();return 1;}
}
