// Shared shader compilation and SHA helpers come from the frozen material test.
// Its main is renamed; no production or frozen test source is modified.
#include <cmath>
#define main bl2_material_comparison_main
#include "native_vulkan_materials_check.cpp"
#undef main

namespace {
constexpr int kGfxWidth=1200,kGfxHeight=540;
using ActualImage=std::vector<unsigned char>;
struct GfxCapture {
    Material pair;
    std::array<std::int16_t,16> vertexSamplers{},fragmentSamplers{};
    std::vector<unsigned char> vertices,atlas;
    std::array<std::uint16_t,6> indices{};
};
Inputs ReadActualInputs(Cursor& cursor,std::uint32_t target,const bl2::vk::ShaderPack& pack,
                       std::array<std::int16_t,16>& samplers) {
    Inputs input;
    const auto rawBytes=cursor.Word(),normalizedBytes=cursor.Word(),rows=cursor.Word();
    Require(rawBytes && rawBytes<=131072 && normalizedBytes && normalizedBytes<=131072 && rows && rows<=4096,"Actual shader/register bounds");
    for(auto& bank:input.groups) {
        const auto* p=cursor.Take(4);
        bank.first_register=static_cast<std::int16_t>(std::uint16_t(p[0])|(std::uint16_t(p[1])<<8));
        bank.byte_count=std::uint16_t(p[2])|(std::uint16_t(p[3])<<8);
        Require(bank.first_register>=-1 && (bank.first_register<0?bank.byte_count==0xffff:
            bank.byte_count%16==0 && std::uint32_t(bank.first_register)+bank.byte_count/16<=rows),"Actual original private bank bounds");
    }
    for(auto& reg:samplers) {
        const auto* p=cursor.Take(2);reg=static_cast<std::int16_t>(std::uint16_t(p[0])|(std::uint16_t(p[1])<<8));
        Require(reg>=-1 && (reg<0 || std::uint32_t(reg)<rows),"Actual sampler register bounds");
    }
    input.words.resize(rows);
    for(auto& row:input.words)for(auto& word:row)word=cursor.Word();
    input.raw=cursor.Text(rawBytes);input.normalized=cursor.Text(normalizedBytes);
    std::string error;
    Require(pack.LookupRaw(target,input.raw,input.stage,&error),"Actual raw stage identity: "+error);
    Require(input.stage.normalizedDigest==Hash(input.normalized.data(),input.normalized.size()),"Actual normalized stage identity");
    return input;
}
GfxCapture ReadActualCapture(const char* path,const bl2::vk::ShaderPack& pack) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);Require(file.good(),"Open actual GFx fixture");
    const auto size=file.tellg();Require(size>64 && size<2*1024*1024,"Actual GFx fixture file bounds");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);Require(file.good(),"Read actual GFx fixture");
    Require(!std::memcmp(bytes.data(),"BL2GFX1\0",8) && U32(bytes.data()+8)==1 &&
        U32(bytes.data()+12)==kGfxWidth && U32(bytes.data()+16)==kGfxHeight && !U32(bytes.data()+20) &&
        U64(bytes.data()+24)==bytes.size()-64,"Actual GFx fixture header");
    const auto hash=Hash(bytes.data()+64,bytes.size()-64);Require(std::equal(hash.begin(),hash.end(),bytes.begin()+32),"Actual GFx payload SHA");
    Cursor cursor{bytes,64};Require(cursor.Word()==6,"Actual GFx capture number");
    const auto vertexBytes=cursor.Word(),atlasBytes=cursor.Word(),indexCount=cursor.Word();
    Require(vertexBytes==82 && atlasBytes==1024*1024 && indexCount==6,"Actual first-quad and atlas bounds");
    GfxCapture result;result.pair.name="actual_capture6_font";
    result.pair.vertex=ReadActualInputs(cursor,kVS,pack,result.vertexSamplers);
    result.pair.fragment=ReadActualInputs(cursor,kFS,pack,result.fragmentSamplers);
    Require(result.pair.vertex.stage.attribute_locations==std::vector<std::uint32_t>{0,2,8} &&
        result.pair.vertex.stage.samplers.empty() && result.pair.fragment.stage.samplers.size()==1 &&
        result.pair.fragment.stage.samplers[0].unit==0 && result.fragmentSamplers[0]==1,"Actual font shader interface");
    const auto* vertices=cursor.Take(vertexBytes);result.vertices.assign(vertices,vertices+vertexBytes);
    for(auto& index:result.indices) {const auto* p=cursor.Take(2);index=std::uint16_t(p[0])|(std::uint16_t(p[1])<<8);}
    Require(result.indices==std::array<std::uint16_t,6>{0,1,2,2,3,0},"Actual captured first-quad indices");
    const auto* atlas=cursor.Take(atlasBytes);result.atlas.assign(atlas,atlas+atlasBytes);
    Require(cursor.at==bytes.size(),"Actual GFx fixture trailing bytes");
    return result;
}
void ActualUniforms(GLuint program,const Inputs& input) {
    const std::array<std::uint32_t,4> zero{};
    for(const auto& field:input.stage.fields) {
        const auto& bank=input.groups[field.groupIndex];
        for(std::uint32_t row=0;row<field.count;++row) {
            const auto name=std::string(kBanks[field.groupIndex])+"["+std::to_string(row)+"]";
            const auto location=glGetUniformLocation(program,name.c_str());if(location<0)continue;
            const auto& words=bank.first_register>=0 && row<bank.byte_count/16?
                input.words.at(static_cast<std::size_t>(bank.first_register)+row):zero;
            if(field.type==bl2::vk::FieldType::Bool)glUniform1i(location,words[0]!=0);
            else if(field.type==bl2::vk::FieldType::IVec4)glUniform4iv(location,1,reinterpret_cast<const GLint*>(words.data()));
            else glUniform4fv(location,1,reinterpret_cast<const GLfloat*>(words.data()));
        }
    }
}
enum class ActualPhase { Baseline,ZeroAtlas,Unblended,Black,BlackUnblended,ZeroRed,ZeroRedUnblended,Restore };
const char* ActualName(ActualPhase phase) {
    switch(phase){case ActualPhase::Baseline:return "baseline";case ActualPhase::ZeroAtlas:return "zero_atlas_control";
        case ActualPhase::Unblended:return "alpha_unblended";case ActualPhase::Black:return "black_blended";
        case ActualPhase::BlackUnblended:return "black_unblended";case ActualPhase::ZeroRed:return "zero_red_blended";
        case ActualPhase::ZeroRedUnblended:return "zero_red_unblended";case ActualPhase::Restore:return "restore";}
    return "invalid";
}
bool ActualUnblended(ActualPhase phase) {
    return phase==ActualPhase::Unblended || phase==ActualPhase::BlackUnblended || phase==ActualPhase::ZeroRedUnblended;
}
std::array<unsigned char,3> ActualColor(ActualPhase phase) {
    if(phase==ActualPhase::Black || phase==ActualPhase::BlackUnblended)return {0,0,0};
    if(phase==ActualPhase::ZeroRed || phase==ActualPhase::ZeroRedUnblended)return {0,128,224};
    return {255,255,255};
}
std::vector<unsigned char> ActualVertices(const GfxCapture& capture,ActualPhase phase) {
    auto vertices=capture.vertices;const auto color=ActualColor(phase);
    // Only derived control phases change the three color bytes. Keep original
    // alpha, UVs, positions, alignment, shader sources and all private locals.
    if(color!=std::array<unsigned char,3>{255,255,255})for(std::size_t vertex=0;vertex<4;++vertex)
        std::copy(color.begin(),color.end(),vertices.begin()+2+vertex*20);
    return vertices;
}
#define ACTUAL_API(Name, ...) (Vulkan ? bl2vk::Name(__VA_ARGS__) : gl##Name(__VA_ARGS__))
template<bool Vulkan>
ActualImage RenderActual(const GfxCapture& capture,GLuint glesProgram,ActualPhase phase) {
    GLuint target=0,fbo=0,atlas=0,sampler=0,array=0,buffer=0,elements=0,programs[2]{};
    ACTUAL_API(GenTextures,1,&target);ACTUAL_API(ActiveTexture,GL_TEXTURE0);ACTUAL_API(BindTexture,GL_TEXTURE_2D,target);
    ACTUAL_API(TexImage2D,GL_TEXTURE_2D,0,GL_RGBA8,kGfxWidth,kGfxHeight,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    ACTUAL_API(GenFramebuffers,1,&fbo);ACTUAL_API(BindFramebuffer,GL_FRAMEBUFFER,fbo);
    ACTUAL_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    Require(ACTUAL_API(CheckFramebufferStatus,GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Actual replay FBO complete");
    ACTUAL_API(GenTextures,1,&atlas);ACTUAL_API(BindTexture,GL_TEXTURE_2D,atlas);
    // The capture owns eleven levels, exposes base=max=0 and uses trilinear minification.
    // GLES receives the original adapter's exact R8/RED + RRR1 translation of luminance.
    ACTUAL_API(TexStorage2D,GL_TEXTURE_2D,11,Vulkan?kLuminance:GL_R8,1024,1024);
    ACTUAL_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);ACTUAL_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    ACTUAL_API(PixelStorei,GL_UNPACK_ALIGNMENT,1);
    const auto data=phase==ActualPhase::ZeroAtlas?std::vector<unsigned char>(capture.atlas.size(),0):capture.atlas;
    ACTUAL_API(TexSubImage2D,GL_TEXTURE_2D,0,0,0,1024,1024,Vulkan?kLuminance:GL_RED,GL_UNSIGNED_BYTE,data.data());
    if constexpr(!Vulkan) {
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R,GL_RED);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_G,GL_RED);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_B,GL_RED);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_A,GL_ONE);
    }
    ACTUAL_API(GenSamplers,1,&sampler);
    ACTUAL_API(SamplerParameteri,sampler,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    ACTUAL_API(SamplerParameteri,sampler,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    for(const auto param:{GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T,GL_TEXTURE_WRAP_R})ACTUAL_API(SamplerParameteri,sampler,param,GL_CLAMP_TO_EDGE);
    ACTUAL_API(SamplerParameterf,sampler,GL_TEXTURE_MIN_LOD,-1000.f);ACTUAL_API(SamplerParameterf,sampler,GL_TEXTURE_MAX_LOD,1000.f);
    ACTUAL_API(BindSampler,0,sampler);
    if constexpr(Vulkan) {
        auto fragment=capture.pair.fragment.words;fragment.at(1)={atlas,sampler,0,0};
        bl2vk::GenPrograms(2,programs);
        Require(bl2vk::NamedProgramString(programs[0],kVS,capture.pair.vertex.raw,capture.pair.vertex.groups.data(),capture.vertexSamplers.data()),std::string("Actual VS: ")+bl2vk::LastFailure());
        Require(bl2vk::NamedProgramString(programs[1],kFS,capture.pair.fragment.raw,capture.pair.fragment.groups.data(),capture.fragmentSamplers.data()),std::string("Actual FS: ")+bl2vk::LastFailure());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[0],kVS,0,static_cast<GLsizei>(capture.pair.vertex.words.size()),capture.pair.vertex.words.front().data());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[1],kFS,0,static_cast<GLsizei>(fragment.size()),fragment.front().data());
        bl2vk::BindProgram(kVS,programs[0]);bl2vk::BindProgram(kFS,programs[1]);
    } else {
        glUseProgram(glesProgram);ActualUniforms(glesProgram,capture.pair.vertex);ActualUniforms(glesProgram,capture.pair.fragment);
        const auto location=glGetUniformLocation(glesProgram,"PSampler0");Require(location>=0,"Actual font sampler is linked");glUniform1i(location,0);
    }
    ACTUAL_API(GenVertexArrays,1,&array);ACTUAL_API(BindVertexArray,array);
    ACTUAL_API(GenBuffers,1,&buffer);ACTUAL_API(BindBuffer,GL_ARRAY_BUFFER,buffer);
    const auto vertices=ActualVertices(capture,phase);
    ACTUAL_API(BufferData,GL_ARRAY_BUFFER,Vulkan?vertices.size():vertices.size()-2,vertices.data()+(Vulkan?0:2),GL_STATIC_DRAW);
    const std::uintptr_t origin=Vulkan?2:0;
    for(const auto location:{0u,2u,8u}) {
        ACTUAL_API(EnableVertexAttribArray,location);
        ACTUAL_API(VertexAttribPointer,location,location==2?4:2,location==2?GL_UNSIGNED_BYTE:GL_FLOAT,
            location==2?GL_TRUE:GL_FALSE,20,reinterpret_cast<const void*>(origin+(location==2?0:location==8?4:12)));
    }
    ACTUAL_API(GenBuffers,1,&elements);ACTUAL_API(BindBuffer,GL_ELEMENT_ARRAY_BUFFER,elements);
    ACTUAL_API(BufferData,GL_ELEMENT_ARRAY_BUFFER,sizeof(capture.indices),capture.indices.data(),GL_STATIC_DRAW);
    ACTUAL_API(Viewport,0,0,kGfxWidth,kGfxHeight);ACTUAL_API(Disable,GL_SCISSOR_TEST);ACTUAL_API(Disable,GL_DEPTH_TEST);
    ACTUAL_API(Disable,GL_STENCIL_TEST);ACTUAL_API(Disable,GL_CULL_FACE);ACTUAL_API(DepthMask,GL_FALSE);ACTUAL_API(FrontFace,GL_CCW);
    ACTUAL_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);ACTUAL_API(BlendEquationSeparate,GL_FUNC_ADD,GL_FUNC_ADD);
    ACTUAL_API(BlendFuncSeparate,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    if(ActualUnblended(phase))ACTUAL_API(Disable,GL_BLEND);else ACTUAL_API(Enable,GL_BLEND);
    ACTUAL_API(ClearColor,kClear[0]/255.f,kClear[1]/255.f,kClear[2]/255.f,kClear[3]/255.f);ACTUAL_API(Clear,GL_COLOR_BUFFER_BIT);
    ACTUAL_API(DrawElements,GL_TRIANGLES,6,GL_UNSIGNED_SHORT,nullptr);ACTUAL_API(Finish);
    ActualImage pixels(kGfxWidth*kGfxHeight*4,0xcd);ACTUAL_API(PixelStorei,GL_PACK_ALIGNMENT,1);
    ACTUAL_API(ReadPixels,0,0,kGfxWidth,kGfxHeight,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    const auto error=ACTUAL_API(GetError);Require(error==GL_NO_ERROR,std::string(Vulkan?"Vulkan":"GLES")+" actual replay "+ActualName(phase)+" error="+std::to_string(error)+(Vulkan?std::string(" ")+bl2vk::LastFailure():""));
    ACTUAL_API(BindFramebuffer,GL_FRAMEBUFFER,0);ACTUAL_API(BindSampler,0,0);
    ACTUAL_API(DeleteFramebuffers,1,&fbo);ACTUAL_API(DeleteTextures,1,&target);ACTUAL_API(DeleteTextures,1,&atlas);
    ACTUAL_API(DeleteSamplers,1,&sampler);ACTUAL_API(DeleteBuffers,1,&buffer);ACTUAL_API(DeleteBuffers,1,&elements);ACTUAL_API(DeleteVertexArrays,1,&array);
    if constexpr(Vulkan)bl2vk::DeletePrograms(2,programs);
    return pixels;
}
#undef ACTUAL_API
std::array<std::size_t,4> ActualCounts(const ActualImage& image) {
    std::array<std::size_t,4> counts{};
    for(int y=255;y<268;++y)for(int x=302;x<312;++x) {
        const auto at=(y*kGfxWidth+x)*4;
        const bool changed=image[at]!=kClear[0] || image[at+1]!=kClear[1] || image[at+2]!=kClear[2];
        counts[0]+=changed;counts[1]+=changed && unsigned(image[at])+image[at+1]+image[at+2]>=32;
        counts[2]+=image[at+3]<=3;counts[3]+=image[at+3]>=10 && image[at+3]<=44;
    }
    return counts;
}
bool CompareActual(const ActualImage& gles,const ActualImage& vk,ActualPhase phase,const char* output) {
    unsigned maximum=0;std::size_t sum=0,beyond=0;
    for(std::size_t i=0;i<gles.size();++i) {const unsigned error=std::abs(int(gles[i])-vk[i]);maximum=std::max(maximum,error);sum+=error;beyond+=error>3;}
    const auto a=ActualCounts(gles),b=ActualCounts(vk);
    const bool parity=maximum<=8 && double(sum)/gles.size()<=.6 && beyond<=gles.size()/50;
    const bool shape=phase==ActualPhase::ZeroAtlas?(a[0]==0 && b[0]==0):
        ActualUnblended(phase)?(a[2]>=8 && b[2]>=8 && a[3]>=16 && b[3]>=16):
        (a[0]>=32 && b[0]>=32 && a[1]>=32 && b[1]>=32);
    std::printf("{\"case\":\"actual_capture6_font\",\"phase\":\"%s\",\"max_channel_error\":%u,\"mean_channel_error\":%.8f,\"channels_error_gt3\":%zu,\"gles_roi_changed\":%zu,\"vulkan_roi_changed\":%zu,\"gles_roi_colored\":%zu,\"vulkan_roi_colored\":%zu,\"gles_alpha_clear\":%zu,\"vulkan_alpha_clear\":%zu,\"gles_alpha_partial\":%zu,\"vulkan_alpha_partial\":%zu,\"status\":\"%s\"}\n",
        ActualName(phase),maximum,double(sum)/gles.size(),beyond,a[0],b[0],a[1],b[1],a[2],b[2],a[3],b[3],parity&&shape?"PASS":"FAIL");
    for(const auto& [image,api]:{std::pair{&gles,"gles"},std::pair{&vk,"vulkan"}}) {
        const auto path=std::string(output)+"/actual_capture6_font-"+ActualName(phase)+"-"+api+".rgba";
        std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(image->data()),image->size());Require(file.good(),"Write actual replay GPU pixels");
    }
    return parity&&shape;
}
bool CheckActualZeroRgb(const ActualImage& image,const ActualImage& alphaReference,ActualPhase phase,const char* api) {
    const auto color=ActualColor(phase);unsigned maximum=0;std::size_t errors=0,alphaErrors=0;
    std::array<unsigned char,3> minima{255,255,255},maxima{};
    for(int y=255;y<268;++y)for(int x=302;x<312;++x) {
        const auto at=(y*kGfxWidth+x)*4;const double alpha=alphaReference[at+3]/255.;
        for(unsigned channel=0;channel<3;++channel) {
            // The captured gamma=1 and color scale RGB=(1,1,1), so expected
            // unblended RGB is the input U8 color. Alpha comes independently
            // from the white unblended atlas phase and is unchanged by RGB.
            const double expected=ActualUnblended(phase)?color[channel]:
                color[channel]*alpha+kClear[channel]*(1-alpha);
            const unsigned difference=std::abs(int(image[at+channel])-int(std::lround(expected)));
            maximum=std::max(maximum,difference);errors+=difference>1;
            minima[channel]=std::min(minima[channel],image[at+channel]);maxima[channel]=std::max(maxima[channel],image[at+channel]);
        }
        if(ActualUnblended(phase))alphaErrors+=image[at+3]!=alphaReference[at+3];
    }
    const bool pass=!errors && !alphaErrors;
    std::printf("{\"case\":\"actual_capture6_font\",\"phase\":\"%s\",\"api\":\"%s\",\"check\":\"original_gamma_zero_rgb\",\"input_rgb\":[%u,%u,%u],\"roi_rgb_min\":[%u,%u,%u],\"roi_rgb_max\":[%u,%u,%u],\"max_expected_channel_error\":%u,\"channels_expected_error_gt1\":%zu,\"alpha_changed_pixels\":%zu,\"status\":\"%s\"}\n",
        ActualName(phase),api,unsigned(color[0]),unsigned(color[1]),unsigned(color[2]),
        unsigned(minima[0]),unsigned(minima[1]),unsigned(minima[2]),unsigned(maxima[0]),unsigned(maxima[1]),unsigned(maxima[2]),maximum,errors,alphaErrors,pass?"PASS":"FAIL");
    return pass;
}
} // namespace

#ifndef BL2_VK_GFX_CAPTURE_HELPERS_ONLY
int main(int argc,char** argv) {
    if(argc!=4){std::fputs("usage: native_vulkan_gfx_capture_check PACK ACTUAL6_FIXTURE OUTPUT_DIRECTORY\n",stderr);return 2;}
    try {
        const auto fd=::open(argv[1],O_RDONLY|O_CLOEXEC);Require(fd>=0,"Open actual V2 Vulkan pack");struct stat64 info{};Require(::fstat64(fd,&info)==0 && info.st_size>0,"Actual V2 descriptor size");
        auto pack=std::make_shared<bl2::vk::ShaderPack>();std::string error;const bool opened=pack->OpenFileDescriptor(fd,0,info.st_size,&error);::close(fd);Require(opened,"Actual V2 pack: "+error);
        const auto capture=ReadActualCapture(argv[2],*pack);
        const auto display=eglGetDisplay(EGL_DEFAULT_DISPLAY);Require(eglInitialize(display,nullptr,nullptr) && eglBindAPI(EGL_OPENGL_ES_API),"Actual GLES reference EGL initialize");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        EGLConfig config{};EGLint count=0;Require(eglChooseConfig(display,attributes,&config,1,&count) && count==1,"Actual GLES reference EGL config");
        const EGLint size[]{EGL_WIDTH,kGfxWidth,EGL_HEIGHT,kGfxHeight,EGL_NONE},version[]{EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
        const auto surface=eglCreatePbufferSurface(display,config,size),context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);
        Require(surface!=EGL_NO_SURFACE && context!=EGL_NO_CONTEXT && eglMakeCurrent(display,surface,surface,context),"Actual GLES3.2 context");
        std::printf("Actual57 GFx replay native ARM32 GLES: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
        Require(bl2vk::Initialize(pack,argv[3]),std::string("Actual direct Vulkan initialize: ")+bl2vk::LastFailure());
        const auto vkContext=bl2vk::CreateContext(0);Require(vkContext && bl2vk::MakeCurrent(vkContext,nullptr),"Actual offscreen Vulkan context");
        // V1 capture6 has unit RGB scale and gamma=1. Verify these premises
        // before using exact U8 colors as independent gamma boundary oracles.
        const auto& scale=capture.pair.vertex.words.at(capture.pair.vertex.groups[1].first_register+3);
        const auto& gamma=capture.pair.fragment.words.at(capture.pair.fragment.groups[6].first_register);
        Require(scale[0]==std::bit_cast<std::uint32_t>(1.f) && scale[1]==scale[0] && scale[2]==scale[0] &&
            gamma[0]==scale[0],"Captured gamma/color oracle premises");
        const auto program=Program(capture.pair);bool passed=true;ActualImage baselineGles,baselineVk,alphaGles,alphaVk;
        for(const auto phase:{ActualPhase::Baseline,ActualPhase::ZeroAtlas,ActualPhase::Unblended,
                ActualPhase::Black,ActualPhase::BlackUnblended,ActualPhase::ZeroRed,ActualPhase::ZeroRedUnblended,ActualPhase::Restore}) {
            const auto vertices=ActualVertices(capture,phase);
            const auto path=std::string(argv[3])+"/actual_capture6_font-"+ActualName(phase)+"-input.vertices20";
            std::ofstream inputFile(path,std::ios::binary);inputFile.write(reinterpret_cast<const char*>(vertices.data()),vertices.size());Require(inputFile.good(),"Write exact derived RGB boundary inputs");
            const auto reference=RenderActual<false>(capture,program,phase),candidate=RenderActual<true>(capture,0,phase);
            passed=CompareActual(reference,candidate,phase,argv[3]) && passed;
            if(phase==ActualPhase::Baseline){baselineGles=reference;baselineVk=candidate;}
            if(phase==ActualPhase::Unblended){alphaGles=reference;alphaVk=candidate;}
            if(ActualColor(phase)!=std::array<unsigned char,3>{255,255,255}) {
                passed=CheckActualZeroRgb(reference,alphaGles,phase,"gles") && passed;
                passed=CheckActualZeroRgb(candidate,alphaVk,phase,"vulkan") && passed;
            }
            if(phase==ActualPhase::Restore) {
                const bool restored=reference==baselineGles && candidate==baselineVk;
                std::printf("{\"case\":\"actual_capture6_font\",\"check\":\"exact_restore\",\"status\":\"%s\"}\n",restored?"PASS":"FAIL");passed=restored&&passed;
            }
        }
        glDeleteProgram(program);bl2vk::Finish();bl2vk::DestroyContext(vkContext);bl2vk::Shutdown();
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
        std::puts(passed?"PASS actual57 captured first font quad: original inputs, nonzero glyph, zero-atlas control, zero RGB gamma boundaries, partial alpha and exact restore":"FAIL actual57 captured font quad GPU parity");return passed?0:3;
    } catch(const std::exception& error){std::fprintf(stderr,"FAIL actual57 captured GFx replay: %s\n",error.what());bl2vk::Shutdown();return 1;}
}
#endif
