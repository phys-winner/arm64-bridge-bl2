// Reuse the exact fixture parser and original shader/reference helpers.
#define BL2_VK_GFX_CAPTURE_HELPERS_ONLY 1
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "native_vulkan_gfx_capture_check.cpp"
#pragma clang diagnostic pop

namespace {
constexpr unsigned kColumns=96,kRows=40,kGlyphs=kColumns*kRows,kLogicalFrames=4;
using StressImages=std::array<ActualImage,kLogicalFrames>;
enum class BufferScenario { Ordinary, CoherentSpan, ExplicitBoundary };
struct GlyphInput {
    std::vector<unsigned char> vertices;
    std::array<std::uint16_t,6> indices{};
    std::array<std::array<GLuint,4>,4> rows{};
    std::array<GLuint,4> fragment{};
    unsigned count{};
};
std::uint32_t GlyphKey(unsigned frame,unsigned glyph) {
    return (glyph+1)*1664525u+(frame+1)*1013904223u;
}
GlyphInput Glyph(const GfxCapture& capture,unsigned frame,unsigned glyph) {
    GlyphInput input;input.vertices=capture.vertices;
    const auto key=GlyphKey(frame,glyph);
    const std::array<unsigned char,4> color{
        static_cast<unsigned char>(48+(key>>3)%176),static_cast<unsigned char>(60+(key>>11)%164),
        static_cast<unsigned char>(80+(key>>19)%144),static_cast<unsigned char>(192+(key>>26)%64)};
    for(unsigned record=0;record<4;++record) {
        auto* vertex=input.vertices.data()+2+record*20;std::copy(color.begin(),color.end(),vertex);
        // UV reversal makes stale VBO contents observable without changing the
        // original glyph atlas or substituting the original shader strings.
        if(key&1u) {
            float u{};std::memcpy(&u,vertex+4,4);u=26.f/1024.f-u;std::memcpy(vertex+4,&u,4);
        }
    }
    switch((key>>1)&3u) {
        case 0:input.indices={0,1,2,2,3,0};input.count=6;break;
        case 1:input.indices={0,1,2,0,1,2};input.count=3;break;
        case 2:input.indices={0,2,3,0,2,3};input.count=3;break;
        default:input.indices={0,3,2,2,1,0};input.count=6;break;
    }
    const auto first=static_cast<std::size_t>(capture.pair.vertex.groups[1].first_register);
    std::copy_n(capture.pair.vertex.words.begin()+first,4,input.rows.begin());
    // Preserve the captured quad dimensions, place each draw in a distinct
    // cell, and vary private VS color/alpha and FS gamma independently.
    const float left=(glyph%kColumns)*12.5f+1.5f,bottom=(glyph/kColumns)*13.5f+.25f;
    const float yScale=std::bit_cast<float>(input.rows[1][1]);
    float bottomVertex{};std::memcpy(&bottomVertex,capture.vertices.data()+2+2*20+16,4);
    input.rows[0][3]=std::bit_cast<GLuint>(2.f*left/kGfxWidth-1.f);
    input.rows[1][3]=std::bit_cast<GLuint>(2.f*bottom/kGfxHeight-1.f-yScale*bottomVertex);
    input.rows[3]={std::bit_cast<GLuint>(1.f),std::bit_cast<GLuint>(.85f),std::bit_cast<GLuint>(.7f),
        std::bit_cast<GLuint>(.5f+.125f*frame)};
    input.fragment=capture.pair.fragment.words.at(capture.pair.fragment.groups[6].first_register);
    input.fragment[0]=std::bit_cast<GLuint>(.75f+.25f*((key>>4)&3u));
    return input;
}
std::vector<unsigned char> StressAtlas(const GfxCapture& capture,unsigned frame) {
    auto pixels=capture.atlas;
    // Alternate this glyph's mask using an ordinary subimage update of the
    // same texture while previously submitted frames retain their old results.
    if(frame&1u)for(unsigned y=1;y<14;++y)for(unsigned x=1;x<25;++x)
        pixels[y*1024+x]=255-pixels[y*1024+x];
    return pixels;
}
#define STRESS_API(Name, ...) (Vulkan ? bl2vk::Name(__VA_ARGS__) : gl##Name(__VA_ARGS__))
template<bool Vulkan>
void UpdateStressBuffer(GLenum target,const void* bytes,std::size_t size,bool mapped) {
    if(mapped) {
        auto* destination=STRESS_API(MapBufferRange,target,0,static_cast<GLsizeiptr>(size),GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT);
        Require(destination!=nullptr,"Stress mapped buffer update");std::memcpy(destination,bytes,size);
        Require(STRESS_API(UnmapBuffer,target)==GL_TRUE,"Stress unmap result");
    } else STRESS_API(BufferSubData,target,0,static_cast<GLsizeiptr>(size),bytes);
}
template<bool Vulkan>
StressImages RenderStress(const GfxCapture& capture,GLuint glesProgram,BufferScenario scenario=BufferScenario::Ordinary) {
    const bool small=scenario!=BufferScenario::Ordinary;
    constexpr GLint baseVertex=744;constexpr std::size_t vertexOffset=65536,indexOffset=0x3d14;
    constexpr GLbitfield coherent=GL_MAP_WRITE_BIT|0x40u|0x80u;
    const unsigned glyphs=small?1:kGlyphs;void* vertexMap=nullptr;void* indexMap=nullptr;
    const auto initial=small?Glyph(capture,0,0):GlyphInput{};
    std::array<GLuint,kLogicalFrames> targets{},fbos{};
    GLuint atlas=0,sampler=0,array=0,vbo=0,ebo=0,programs[2]{};
    STRESS_API(GenTextures,kLogicalFrames,targets.data());STRESS_API(GenFramebuffers,kLogicalFrames,fbos.data());
    STRESS_API(ActiveTexture,GL_TEXTURE0);
    for(unsigned frame=0;frame<kLogicalFrames;++frame) {
        STRESS_API(BindTexture,GL_TEXTURE_2D,targets[frame]);
        STRESS_API(TexImage2D,GL_TEXTURE_2D,0,GL_RGBA8,kGfxWidth,kGfxHeight,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        STRESS_API(BindFramebuffer,GL_FRAMEBUFFER,fbos[frame]);
        STRESS_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,targets[frame],0);
        Require(STRESS_API(CheckFramebufferStatus,GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Stress output FBO complete");
    }
    STRESS_API(GenTextures,1,&atlas);STRESS_API(BindTexture,GL_TEXTURE_2D,atlas);
    STRESS_API(TexStorage2D,GL_TEXTURE_2D,11,Vulkan?kLuminance:GL_R8,1024,1024);
    STRESS_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);STRESS_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    if constexpr(!Vulkan) {
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_R,GL_RED);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_G,GL_RED);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_B,GL_RED);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_A,GL_ONE);
    }
    STRESS_API(GenSamplers,1,&sampler);STRESS_API(SamplerParameteri,sampler,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    STRESS_API(SamplerParameteri,sampler,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    for(const auto parameter:{GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T,GL_TEXTURE_WRAP_R})STRESS_API(SamplerParameteri,sampler,parameter,GL_CLAMP_TO_EDGE);
    STRESS_API(BindSampler,0,sampler);
    std::array<GLint,4> vertexLocations{};GLint fragmentLocation=-1;
    if constexpr(Vulkan) {
        auto fragment=capture.pair.fragment.words;fragment.at(1)={atlas,sampler,0,0};
        bl2vk::GenPrograms(2,programs);
        Require(bl2vk::NamedProgramString(programs[0],kVS,capture.pair.vertex.raw,capture.pair.vertex.groups.data(),capture.vertexSamplers.data()),"Stress exact original VS");
        Require(bl2vk::NamedProgramString(programs[1],kFS,capture.pair.fragment.raw,capture.pair.fragment.groups.data(),capture.fragmentSamplers.data()),"Stress exact original FS");
        bl2vk::NamedProgramLocalParametersI4uiv(programs[0],kVS,0,capture.pair.vertex.words.size(),capture.pair.vertex.words.front().data());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[1],kFS,0,fragment.size(),fragment.front().data());
        bl2vk::BindProgram(kVS,programs[0]);bl2vk::BindProgram(kFS,programs[1]);
    } else {
        glUseProgram(glesProgram);ActualUniforms(glesProgram,capture.pair.vertex);ActualUniforms(glesProgram,capture.pair.fragment);
        for(unsigned row=0;row<4;++row) {const auto name="VConstFloat["+std::to_string(row)+"]";vertexLocations[row]=glGetUniformLocation(glesProgram,name.c_str());}
        fragmentLocation=glGetUniformLocation(glesProgram,"PConstFloat[0]");const auto samplerLocation=glGetUniformLocation(glesProgram,"PSampler0");
        Require(std::all_of(vertexLocations.begin(),vertexLocations.end(),[](GLint location){return location>=0;}) && fragmentLocation>=0 && samplerLocation>=0,"Stress GLES original uniform locations");
        glUniform1i(samplerLocation,0);
    }
    STRESS_API(GenVertexArrays,1,&array);STRESS_API(BindVertexArray,array);
    STRESS_API(GenBuffers,1,&vbo);STRESS_API(BindBuffer,GL_ARRAY_BUFFER,vbo);
    if(small) {
        std::vector<unsigned char> bytes(196608,0xa5);std::memcpy(bytes.data()+vertexOffset,initial.vertices.data()+2,80);
        if constexpr(Vulkan)bl2vk::BufferStorage(GL_ARRAY_BUFFER,bytes.size(),bytes.data(),coherent);
        else glBufferData(GL_ARRAY_BUFFER,bytes.size(),bytes.data(),GL_DYNAMIC_DRAW);
    } else STRESS_API(BufferData,GL_ARRAY_BUFFER,Vulkan?82:80,nullptr,GL_DYNAMIC_DRAW);
    const std::uintptr_t origin=small?vertexOffset-baseVertex*20:Vulkan?2:0;
    for(const auto location:{0u,2u,8u}) {
        STRESS_API(EnableVertexAttribArray,location);STRESS_API(VertexAttribPointer,location,location==2?4:2,
            location==2?GL_UNSIGNED_BYTE:GL_FLOAT,location==2?GL_TRUE:GL_FALSE,20,
            reinterpret_cast<const void*>(origin+(location==2?0:location==8?4:12)));
    }
    STRESS_API(GenBuffers,1,&ebo);STRESS_API(BindBuffer,GL_ELEMENT_ARRAY_BUFFER,ebo);
    if(small) {
        std::vector<unsigned char> bytes(16384,0xa5);std::memcpy(bytes.data()+indexOffset,initial.indices.data(),12);
        if constexpr(Vulkan)bl2vk::BufferStorage(GL_ELEMENT_ARRAY_BUFFER,bytes.size(),bytes.data(),coherent);
        else glBufferData(GL_ELEMENT_ARRAY_BUFFER,bytes.size(),bytes.data(),GL_DYNAMIC_DRAW);
    } else STRESS_API(BufferData,GL_ELEMENT_ARRAY_BUFFER,12,nullptr,GL_DYNAMIC_DRAW);
    STRESS_API(Viewport,0,0,kGfxWidth,kGfxHeight);STRESS_API(Disable,GL_SCISSOR_TEST);STRESS_API(Disable,GL_DEPTH_TEST);
    STRESS_API(Disable,GL_STENCIL_TEST);STRESS_API(Disable,GL_CULL_FACE);STRESS_API(DepthMask,GL_FALSE);STRESS_API(FrontFace,GL_CCW);
    STRESS_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);STRESS_API(Enable,GL_BLEND);
    STRESS_API(BlendEquationSeparate,GL_FUNC_ADD,GL_FUNC_ADD);STRESS_API(BlendFuncSeparate,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    STRESS_API(PixelStorei,GL_UNPACK_ALIGNMENT,1);
    if(small)std::printf("{\"scenario\":\"%s\",\"api\":\"%s\",\"queued_frames\":4,\"draws_per_frame\":1,\"vertex_offset\":65536,\"index_offset\":15636}\n",scenario==BufferScenario::CoherentSpan?"coherent-span":"coherent-explicit-boundary",Vulkan?"vulkan":"gles");
    for(unsigned frame=0;frame<kLogicalFrames;++frame) {
        STRESS_API(BindFramebuffer,GL_FRAMEBUFFER,fbos[frame]);
        STRESS_API(ClearColor,kClear[0]/255.f,kClear[1]/255.f,kClear[2]/255.f,kClear[3]/255.f);STRESS_API(Clear,GL_COLOR_BUFFER_BIT);
        const auto pixels=small?capture.atlas:StressAtlas(capture,frame);
        ObbSha256 inputs;Require(inputs.Update(pixels.data(),pixels.size()),"Stress input atlas SHA");
        STRESS_API(TexSubImage2D,GL_TEXTURE_2D,0,0,0,1024,1024,Vulkan?kLuminance:GL_RED,GL_UNSIGNED_BYTE,pixels.data());
        if constexpr(Vulkan) {
            if((scenario==BufferScenario::CoherentSpan&&frame==0)||(scenario==BufferScenario::ExplicitBoundary&&frame==1)) {
                const auto flags=coherent|(scenario==BufferScenario::ExplicitBoundary?GL_MAP_FLUSH_EXPLICIT_BIT:0);
                vertexMap=bl2vk::MapBufferRange(GL_ARRAY_BUFFER,0,196608,flags);indexMap=bl2vk::MapBufferRange(GL_ELEMENT_ARRAY_BUFFER,0,16384,flags);
                Require(vertexMap&&indexMap,"High-offset persistent coherent mappings");
            }
            if(scenario==BufferScenario::ExplicitBoundary&&frame==2) {
                Require(bl2vk::UnmapBuffer(GL_ARRAY_BUFFER)==GL_TRUE&&bl2vk::UnmapBuffer(GL_ELEMENT_ARRAY_BUFFER)==GL_TRUE,"D2 unmap before unchanged CPU-byte draw");vertexMap=indexMap=nullptr;
            }
        }
        const GLint drawBase=scenario==BufferScenario::CoherentSpan&&frame>=2?-baseVertex:baseVertex;
        for(unsigned glyph=0;glyph<glyphs;++glyph) {
            // D2 frames 2/3 consume exactly B; neither API invalidates buffers after unmap.
            auto input=Glyph(capture,scenario==BufferScenario::ExplicitBoundary&&frame?1:frame,glyph);
            if(small&&drawBase<0)for(auto& index:input.indices)index+=2*baseVertex;
            for(const auto& [bytes,size]:{std::pair{static_cast<const unsigned char*>(input.vertices.data()),input.vertices.size()},
                    std::pair{reinterpret_cast<const unsigned char*>(input.indices.data()),sizeof(input.indices)},
                    std::pair{reinterpret_cast<const unsigned char*>(input.rows.data()),sizeof(input.rows)},
                    std::pair{reinterpret_cast<const unsigned char*>(input.fragment.data()),sizeof(input.fragment)},
                    std::pair{reinterpret_cast<const unsigned char*>(&input.count),sizeof(input.count)}})
                Require(inputs.Update(bytes,size),"Stress logical input SHA");
            if(small) {
                if(scenario==BufferScenario::CoherentSpan||frame==1) {
                    if constexpr(Vulkan){std::memcpy(static_cast<unsigned char*>(vertexMap)+vertexOffset,input.vertices.data()+2,80);std::memcpy(static_cast<unsigned char*>(indexMap)+indexOffset,input.indices.data(),12);}
                    else {glBufferSubData(GL_ARRAY_BUFFER,vertexOffset,80,input.vertices.data()+2);glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,indexOffset,12,input.indices.data());}
                }
            } else {
                UpdateStressBuffer<Vulkan>(GL_ARRAY_BUFFER,input.vertices.data()+(Vulkan?0:2),Vulkan?82:80,(glyph&1u)!=0);
                UpdateStressBuffer<Vulkan>(GL_ELEMENT_ARRAY_BUFFER,input.indices.data(),12,(glyph&2u)!=0);
            }
            if constexpr(Vulkan) {
                const auto first=static_cast<GLuint>(capture.pair.vertex.groups[1].first_register);
                bl2vk::NamedProgramLocalParametersI4uiv(programs[0],kVS,first,4,input.rows.front().data());
                bl2vk::NamedProgramLocalParametersI4uiv(programs[1],kFS,capture.pair.fragment.groups[6].first_register,1,input.fragment.data());
            } else {
                for(unsigned row=0;row<4;++row)glUniform4fv(vertexLocations[row],1,reinterpret_cast<const GLfloat*>(input.rows[row].data()));
                glUniform4fv(fragmentLocation,1,reinterpret_cast<const GLfloat*>(input.fragment.data()));
            }
            if(small) {
                Require(inputs.Update(reinterpret_cast<const unsigned char*>(&drawBase),sizeof(drawBase)),"High-offset logical draw base SHA");
                std::printf("{\"scenario\":\"%s\",\"api\":\"%s\",\"frame\":%u,\"draw_base_vertex\":%d}\n",scenario==BufferScenario::CoherentSpan?"coherent-span":"coherent-explicit-boundary",Vulkan?"vulkan":"gles",frame,drawBase);
                STRESS_API(DrawElementsBaseVertex,GL_TRIANGLES,input.count,GL_UNSIGNED_SHORT,reinterpret_cast<const void*>(indexOffset),drawBase);
            }
            else STRESS_API(DrawElements,GL_TRIANGLES,input.count,GL_UNSIGNED_SHORT,nullptr);
        }
        STRESS_API(Flush);
        const auto error=STRESS_API(GetError);Require(error==GL_NO_ERROR,std::string(Vulkan?"Vulkan":"GLES")+" stress submitted frame="+std::to_string(frame)+" error="+std::to_string(error)+(Vulkan?std::string(" ")+bl2vk::LastFailure():""));
        const auto hash=inputs.Finalize();
        std::printf("{\"api\":\"%s\",\"submitted_frame\":%u,\"glyph_draws\":%u,\"readbacks_so_far\":0,\"finish_calls\":0,\"logical_inputs_sha256\":\"",Vulkan?"vulkan":"gles",frame,glyphs);
        for(const auto byte:hash)std::printf("%02x",unsigned(byte));std::puts("\"}");std::fflush(stdout);
    }
    // All four logical frames have been submitted. ReadPixels supplies the
    // first explicit completion needed by the observable image oracle.
    StressImages images;
    for(unsigned frame=0;frame<kLogicalFrames;++frame) {
        STRESS_API(BindFramebuffer,GL_FRAMEBUFFER,fbos[frame]);images[frame].resize(kGfxWidth*kGfxHeight*4,0xcd);
        STRESS_API(PixelStorei,GL_PACK_ALIGNMENT,1);STRESS_API(ReadPixels,0,0,kGfxWidth,kGfxHeight,GL_RGBA,GL_UNSIGNED_BYTE,images[frame].data());
        Require(STRESS_API(GetError)==GL_NO_ERROR,std::string(Vulkan?"Vulkan":"GLES")+" stress readback "+(Vulkan?bl2vk::LastFailure():""));
    }
    if constexpr(Vulkan)if(vertexMap)Require(bl2vk::UnmapBuffer(GL_ARRAY_BUFFER)==GL_TRUE&&bl2vk::UnmapBuffer(GL_ELEMENT_ARRAY_BUFFER)==GL_TRUE,"Coherent span mapping cleanup");
    STRESS_API(BindFramebuffer,GL_FRAMEBUFFER,0);STRESS_API(BindSampler,0,0);
    STRESS_API(DeleteFramebuffers,kLogicalFrames,fbos.data());STRESS_API(DeleteTextures,kLogicalFrames,targets.data());
    STRESS_API(DeleteTextures,1,&atlas);STRESS_API(DeleteSamplers,1,&sampler);STRESS_API(DeleteBuffers,1,&vbo);STRESS_API(DeleteBuffers,1,&ebo);
    STRESS_API(DeleteVertexArrays,1,&array);if constexpr(Vulkan)bl2vk::DeletePrograms(2,programs);
    return images;
}
#undef STRESS_API
std::array<std::size_t,3> StressCoverage(const ActualImage& pixels,unsigned glyphs=kGlyphs) {
    std::size_t minimum=~std::size_t(0),colored=0,cells=0;
    for(unsigned row=0;row<kRows;++row)for(unsigned column=0;column<kColumns;++column) {
        if(row*kColumns+column>=glyphs)continue;
        std::size_t count=0;
        for(int y=static_cast<int>(row*13.5);y<static_cast<int>((row+1)*13.5);++y)
            for(int x=static_cast<int>(column*12.5);x<static_cast<int>((column+1)*12.5);++x) {
                const auto at=(y*kGfxWidth+x)*4;
                if((pixels[at]!=kClear[0] || pixels[at+1]!=kClear[1] || pixels[at+2]!=kClear[2]) &&
                    unsigned(pixels[at])+pixels[at+1]+pixels[at+2]>=32)++count;
            }
        minimum=std::min(minimum,count);colored+=count;cells+=count>=8;
    }
    return {minimum,colored,cells};
}
bool CompareStress(const ActualImage& gles,const ActualImage& vk,unsigned frame,const char* output,const char* scenario=nullptr) {
    const unsigned glyphs=scenario?1:kGlyphs;
    unsigned maximum=0;std::size_t sum=0,beyond=0;
    for(std::size_t i=0;i<gles.size();++i){const unsigned error=std::abs(int(gles[i])-vk[i]);maximum=std::max(maximum,error);sum+=error;beyond+=error>3;}
    const auto a=StressCoverage(gles,glyphs),b=StressCoverage(vk,glyphs);
    const bool pass=maximum<=8 && double(sum)/gles.size()<=.6 && beyond<=gles.size()/50 &&
        a[0]>=8 && b[0]>=8 && a[2]==glyphs && b[2]==glyphs;
    if(scenario)std::printf("{\"scenario\":\"%s\",\"frame\":%u,\"status\":\"%s\"}\n",scenario,frame,pass?"PASS":"FAIL");
    std::printf("{\"frame\":%u,\"glyph_draws\":%u,\"gles_min_colored_per_cell\":%zu,\"vulkan_min_colored_per_cell\":%zu,\"gles_colored_pixels\":%zu,\"vulkan_colored_pixels\":%zu,\"gles_visible_cells\":%zu,\"vulkan_visible_cells\":%zu,\"max_channel_error\":%u,\"mean_channel_error\":%.8f,\"channels_error_gt3\":%zu,\"status\":\"%s\"}\n",
        frame,glyphs,a[0],b[0],a[1],b[1],a[2],b[2],maximum,double(sum)/gles.size(),beyond,pass?"PASS":"FAIL");
    for(const auto& [pixels,api]:{std::pair{&gles,"gles"},std::pair{&vk,"vulkan"}}) {
        const auto path=std::string(output)+"/font-lifetime"+(scenario?std::string("-")+scenario:"")+"-frame"+std::to_string(frame)+"-"+api+".rgba";
        std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(pixels->data()),pixels->size());Require(file.good(),"Write lifetime GPU image evidence");
    }
    return pass;
}
std::size_t StressDifference(const ActualImage& a,const ActualImage& b) {
    std::size_t count=0;for(std::size_t at=0;at<a.size();at+=4)
        count+=std::abs(int(a[at])-b[at])+std::abs(int(a[at+1])-b[at+1])+std::abs(int(a[at+2])-b[at+2])>5;
    return count;
}
} // namespace
int main(int argc,char** argv) {
    if(argc!=4){std::fputs("usage: native_vulkan_gfx_lifetime_check PACK ACTUAL6_FIXTURE OUTPUT_DIRECTORY\n",stderr);return 2;}
    try {
        const auto fd=::open(argv[1],O_RDONLY|O_CLOEXEC);Require(fd>=0,"Open stress V2 shader pack");struct stat64 info{};Require(::fstat64(fd,&info)==0 && info.st_size>0,"Stress pack descriptor size");
        auto pack=std::make_shared<bl2::vk::ShaderPack>();std::string error;const bool opened=pack->OpenFileDescriptor(fd,0,info.st_size,&error);::close(fd);Require(opened,"Stress V2 pack: "+error);
        const auto capture=ReadActualCapture(argv[2],*pack);
        const auto display=eglGetDisplay(EGL_DEFAULT_DISPLAY);Require(eglInitialize(display,nullptr,nullptr) && eglBindAPI(EGL_OPENGL_ES_API),"Stress GLES EGL initialization");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        EGLConfig config{};EGLint count=0;Require(eglChooseConfig(display,attributes,&config,1,&count) && count==1,"Stress GLES EGL config");
        const EGLint size[]{EGL_WIDTH,kGfxWidth,EGL_HEIGHT,kGfxHeight,EGL_NONE},version[]{EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
        const auto surface=eglCreatePbufferSurface(display,config,size),context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);
        Require(surface!=EGL_NO_SURFACE && context!=EGL_NO_CONTEXT && eglMakeCurrent(display,surface,surface,context),"Stress GLES3.2 context");
        std::printf("Native ARM32 original-font lifetime GLES: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
        const auto program=Program(capture.pair);const auto gles=RenderStress<false>(capture,program);
        const auto coherentGles=RenderStress<false>(capture,program,BufferScenario::CoherentSpan),explicitGles=RenderStress<false>(capture,program,BufferScenario::ExplicitBoundary);glDeleteProgram(program);
        Require(bl2vk::Initialize(pack,argv[3]),std::string("Stress Vulkan initialize: ")+bl2vk::LastFailure());
        const auto vkContext=bl2vk::CreateContext(0);Require(vkContext && bl2vk::MakeCurrent(vkContext,nullptr),"Stress Vulkan offscreen context");
        const auto vk=RenderStress<true>(capture,0);bool passed=true;
        for(unsigned frame=0;frame<kLogicalFrames;++frame) {
            passed=CompareStress(gles[frame],vk[frame],frame,argv[3]) && passed;
            if(frame) {
                const auto a=StressDifference(gles[frame-1],gles[frame]),b=StressDifference(vk[frame-1],vk[frame]);
                const bool changed=a>=kGlyphs*16 && b>=kGlyphs*16;
                std::printf("{\"frame\":%u,\"check\":\"observable_frame_resource_updates\",\"gles_changed_pixels\":%zu,\"vulkan_changed_pixels\":%zu,\"status\":\"%s\"}\n",frame,a,b,changed?"PASS":"FAIL");passed=changed&&passed;
            }
        }
        for(const auto scenario:{BufferScenario::CoherentSpan,BufferScenario::ExplicitBoundary}) {
            const auto actual=RenderStress<true>(capture,0,scenario);const auto& reference=scenario==BufferScenario::CoherentSpan?coherentGles:explicitGles;
            const auto* label=scenario==BufferScenario::CoherentSpan?"coherent-span":"coherent-explicit-boundary";
            for(unsigned frame=0;frame<kLogicalFrames;++frame)passed=CompareStress(reference[frame],actual[frame],frame,argv[3],label)&&passed;
            bool observable=StressDifference(reference[0],reference[1])>=16&&StressDifference(actual[0],actual[1])>=16;
            if(scenario==BufferScenario::CoherentSpan)for(unsigned frame=2;frame<kLogicalFrames;++frame)observable=StressDifference(reference[frame-1],reference[frame])>=16&&StressDifference(actual[frame-1],actual[frame])>=16&&observable;
            const bool preserved=scenario!=BufferScenario::ExplicitBoundary||(reference[1]==reference[2]&&reference[1]==reference[3]&&actual[1]==actual[2]&&actual[1]==actual[3]);
            std::printf("{\"scenario\":\"%s\",\"check\":\"observable_writes_and_unmapped_preservation\",\"observable\":%u,\"preserved\":%u,\"status\":\"%s\"}\n",label,observable,preserved,observable&&preserved?"PASS":"FAIL");passed=observable&&preserved&&passed;
        }
        bl2vk::DestroyContext(vkContext);bl2vk::Shutdown();eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
        std::puts(passed?"PASS 4 queued original-font frames: 15360 draws/API, updated shared VBO/EBO/atlas/private parameters, no Finish between draws, observable GPU parity":"FAIL original-font frame resource lifetime");return passed?0:3;
    } catch(const std::exception& error){std::fprintf(stderr,"FAIL native original-font lifetime check: %s\n",error.what());bl2vk::Shutdown();return 1;}
}
