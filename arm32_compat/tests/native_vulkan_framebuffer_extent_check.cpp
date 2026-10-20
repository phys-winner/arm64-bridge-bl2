// Reuse the unchanged genuine font sources, parameters and image helpers.
#define main bl2_framebuffer_extent_material_helpers_main
#include "native_vulkan_materials_check.cpp"
#undef main

namespace {
struct ExtentCase {
    const char* name;
    int colorWidth,colorHeight,dsWidth,dsHeight;
    bool inactiveColor=false,hdr=false,mip=false;
};
constexpr std::array<ExtentCase,7> kExtentCases{{
    {"equal_extent_control",32,32,32,32},
    {"color_shorter_than_ds",32,32,32,36},
    {"ds_shorter_than_color",32,36,32,32},
    {"crossed_attachment_extents",36,32,32,36},
    {"inactive_attached_color_limits_extent",32,32,32,32,true},
    {"rgba16f_with_padded_ds",32,32,32,36,false,true},
    {"selected_mip_with_padded_ds",64,64,32,36,false,false,true}
}};
enum class ExtentPhase { Glyph, StencilClip, StencilReject, MaskedClear, Blit, Copy };
constexpr std::array<ExtentPhase,6> kExtentPhases{
    ExtentPhase::Glyph,ExtentPhase::StencilClip,ExtentPhase::StencilReject,
    ExtentPhase::MaskedClear,ExtentPhase::Blit,ExtentPhase::Copy
};
const char* ExtentPhaseName(ExtentPhase phase){
    switch(phase){case ExtentPhase::Glyph:return "glyph";case ExtentPhase::StencilClip:return "stencil_scissor_clip";case ExtentPhase::StencilReject:return "stencil_rejection_control";case ExtentPhase::MaskedClear:return "masked_clear";case ExtentPhase::Blit:return "read_draw_color_blit";case ExtentPhase::Copy:return "copy_from_read_buffer";}
    return "invalid";
}
struct ExtentPixels { int width{},height{};std::vector<unsigned char> rgba; };
std::pair<int,int> CommonExtent(const ExtentCase& test){
    const int colorWidth=test.colorWidth>>(test.mip?1:0),colorHeight=test.colorHeight>>(test.mip?1:0);
    return {std::min({colorWidth,test.dsWidth,test.inactiveColor?16:colorWidth}),std::min({colorHeight,test.dsHeight,test.inactiveColor?16:colorHeight})};
}

#define EXTENT_API(Name, ...) (Vulkan ? bl2vk::Name(__VA_ARGS__) : gl##Name(__VA_ARGS__))
template<bool Vulkan>
ExtentPixels RenderExtent(const Material& font,GLuint glesProgram,const ExtentCase& test,ExtentPhase phase){
    const auto [width,height]=CommonExtent(test);
    GLuint programs[2]{},textures[6]{},framebuffers[2]{},buffers[2]{},array=0;
    if constexpr(Vulkan){
        std::array<std::int16_t,16> samplers;samplers.fill(-1);bl2vk::GenPrograms(2,programs);
        Require(bl2vk::NamedProgramString(programs[0],kVS,font.vertex.raw,font.vertex.groups.data(),samplers.data()),std::string("Extent original VS: ")+bl2vk::LastFailure());
        Require(bl2vk::NamedProgramString(programs[1],kFS,font.fragment.raw,font.fragment.groups.data(),samplers.data()),std::string("Extent original FS: ")+bl2vk::LastFailure());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[0],kVS,0,static_cast<GLsizei>(font.vertex.words.size()),font.vertex.words.front().data());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[1],kFS,0,static_cast<GLsizei>(font.fragment.words.size()),font.fragment.words.front().data());
        bl2vk::BindProgram(kVS,programs[0]);bl2vk::BindProgram(kFS,programs[1]);
    }else{
        glUseProgram(glesProgram);Uniforms(glesProgram,font.vertex,font.vertex.words);Uniforms(glesProgram,font.fragment,font.fragment.words);
        const auto sampler=glGetUniformLocation(glesProgram,"PSampler0");Require(sampler>=0,"Extent original font sampler");glUniform1i(sampler,0);
    }
    EXTENT_API(GenTextures,6,textures);EXTENT_API(ActiveTexture,GL_TEXTURE0);
    const auto storage=[&](unsigned index,GLenum format,int w,int h,int levels=1){EXTENT_API(BindTexture,GL_TEXTURE_2D,textures[index]);EXTENT_API(TexStorage2D,GL_TEXTURE_2D,levels,format,w,h);};
    storage(0,test.hdr?GL_RGBA16F:GL_RGBA8,test.colorWidth,test.colorHeight,test.mip?2:1);
    storage(1,GL_DEPTH24_STENCIL8,test.dsWidth,test.dsHeight);
    storage(3,GL_RGBA8,16,16);storage(4,GL_RGBA8,width,height);storage(5,test.hdr?GL_RGBA16F:GL_RGBA8,width,height);
    EXTENT_API(BindTexture,GL_TEXTURE_2D,textures[2]);EXTENT_API(PixelStorei,GL_UNPACK_ALIGNMENT,1);
    const auto atlas=SingleChannel(Phase::Baseline,true);
    EXTENT_API(TexImage2D,GL_TEXTURE_2D,0,GL_R8,8,8,0,GL_RED,GL_UNSIGNED_BYTE,atlas.data());
    EXTENT_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);EXTENT_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    EXTENT_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);EXTENT_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);EXTENT_API(BindSampler,0,0);
    EXTENT_API(GenVertexArrays,1,&array);EXTENT_API(BindVertexArray,array);EXTENT_API(GenBuffers,2,buffers);
    const auto vertices=FontVertices(font,Phase::Baseline);const std::uintptr_t origin=Vulkan?2:0;
    EXTENT_API(BindBuffer,GL_ARRAY_BUFFER,buffers[0]);EXTENT_API(BufferData,GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(Vulkan?vertices.size():vertices.size()-2),vertices.data()+(Vulkan?0:2),GL_STATIC_DRAW);
    for(const auto location:{0u,2u,8u}){
        EXTENT_API(EnableVertexAttribArray,location);EXTENT_API(VertexAttribPointer,location,location==2?4:2,location==2?GL_UNSIGNED_BYTE:GL_FLOAT,location==2?GL_TRUE:GL_FALSE,20,reinterpret_cast<const void*>(origin+(location==2?16:location==8?8:0)));
    }
    constexpr std::array<std::uint16_t,6> indices{0,1,2,3,4,5};EXTENT_API(BindBuffer,GL_ELEMENT_ARRAY_BUFFER,buffers[1]);EXTENT_API(BufferData,GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices.data(),GL_STATIC_DRAW);
    EXTENT_API(Viewport,0,0,kSide,kSide);EXTENT_API(Disable,GL_SCISSOR_TEST);EXTENT_API(Disable,GL_CULL_FACE);EXTENT_API(Disable,GL_BLEND);EXTENT_API(Disable,GL_DEPTH_TEST);EXTENT_API(Disable,GL_STENCIL_TEST);
    EXTENT_API(DepthMask,GL_TRUE);EXTENT_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);EXTENT_API(FrontFace,GL_CCW);
    EXTENT_API(StencilMaskSeparate,GL_FRONT_AND_BACK,255);EXTENT_API(StencilOpSeparate,GL_FRONT_AND_BACK,GL_KEEP,GL_KEEP,GL_KEEP);
    EXTENT_API(ClearColor,kClear[0]/255.f,kClear[1]/255.f,kClear[2]/255.f,kClear[3]/255.f);EXTENT_API(ClearDepthf,1);EXTENT_API(ClearStencil,0);
    EXTENT_API(GenFramebuffers,2,framebuffers);EXTENT_API(BindFramebuffer,GL_FRAMEBUFFER,framebuffers[0]);
    EXTENT_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[0],test.mip?1:0);
    EXTENT_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,textures[1],0);
    if(test.inactiveColor)EXTENT_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_TEXTURE_2D,textures[3],0);
    constexpr GLenum draw=GL_COLOR_ATTACHMENT0;EXTENT_API(DrawBuffers,1,&draw);EXTENT_API(ReadBuffer,GL_COLOR_ATTACHMENT0);
    const auto status=EXTENT_API(CheckFramebufferStatus,GL_FRAMEBUFFER);
    std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"api\":\"%s\",\"framebuffer_status\":%u,\"defined_common_extent\":[%d,%d]}\n",test.name,ExtentPhaseName(phase),Vulkan?"vulkan":"gles",status,width,height);
    Require(status==GL_FRAMEBUFFER_COMPLETE,"Mixed extent framebuffer must be complete");
    EXTENT_API(Clear,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    const auto drawGlyph=[&]{EXTENT_API(DrawElements,GL_TRIANGLES,6,GL_UNSIGNED_SHORT,nullptr);};
    if(phase==ExtentPhase::MaskedClear){
        EXTENT_API(ColorMask,GL_TRUE,GL_FALSE,GL_FALSE,GL_FALSE);EXTENT_API(ClearColor,.4f,.8f,.2f,.1f);EXTENT_API(Clear,GL_COLOR_BUFFER_BIT);EXTENT_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    }else if(phase==ExtentPhase::StencilClip){
        EXTENT_API(Enable,GL_STENCIL_TEST);EXTENT_API(StencilFuncSeparate,GL_FRONT_AND_BACK,GL_ALWAYS,1,255);EXTENT_API(StencilOpSeparate,GL_FRONT_AND_BACK,GL_KEEP,GL_KEEP,GL_REPLACE);
        EXTENT_API(ColorMask,GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);EXTENT_API(Enable,GL_SCISSOR_TEST);EXTENT_API(Scissor,0,0,width/2,height);drawGlyph();
        EXTENT_API(Disable,GL_SCISSOR_TEST);EXTENT_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);EXTENT_API(StencilFuncSeparate,GL_FRONT_AND_BACK,GL_EQUAL,1,255);EXTENT_API(StencilOpSeparate,GL_FRONT_AND_BACK,GL_KEEP,GL_KEEP,GL_KEEP);drawGlyph();
    }else if(phase==ExtentPhase::StencilReject){
        EXTENT_API(Enable,GL_STENCIL_TEST);EXTENT_API(StencilFuncSeparate,GL_FRONT_AND_BACK,GL_EQUAL,1,255);drawGlyph();
    }else drawGlyph();
    bool outputHdr=test.hdr;
    if(phase==ExtentPhase::Blit){
        EXTENT_API(BindFramebuffer,GL_DRAW_FRAMEBUFFER,framebuffers[1]);EXTENT_API(FramebufferTexture2D,GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[4],0);
        Require(EXTENT_API(CheckFramebufferStatus,GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Blit destination complete");
        EXTENT_API(Disable,GL_SCISSOR_TEST);EXTENT_API(Clear,GL_COLOR_BUFFER_BIT);EXTENT_API(Enable,GL_SCISSOR_TEST);EXTENT_API(Scissor,0,0,width/2,height);
        EXTENT_API(BlitFramebuffer,0,0,width,height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_NEAREST);EXTENT_API(Disable,GL_SCISSOR_TEST);EXTENT_API(BindFramebuffer,GL_READ_FRAMEBUFFER,framebuffers[1]);outputHdr=false;
    }else if(phase==ExtentPhase::Copy){
        if constexpr(!Vulkan){if(test.hdr){
            Require(glGetError()==GL_NO_ERROR,"HDR copy precondition error");
            // This conversion was the v1 oracle error, not an extent requirement.
            // ES 3.2 does not guarantee CopyTex* between different effective formats.
            glBindTexture(GL_TEXTURE_2D,textures[4]);glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,width,height);const auto oldError=glGetError();
            std::printf("{\"diagnostic\":\"v1_hdr_copy_format\",\"source_internal\":%u,\"destination_internal\":%u,\"immediate_gles_error\":%u,\"conversion_guaranteed\":false}\n",GL_RGBA16F,GL_RGBA8,oldError);
            Require(oldError==GL_NO_ERROR||oldError==GL_INVALID_OPERATION,"Unexpected error in v1 HDR format diagnostic");
        }}
        EXTENT_API(BindTexture,GL_TEXTURE_2D,textures[5]);EXTENT_API(CopyTexSubImage2D,GL_TEXTURE_2D,0,0,0,0,0,width,height);
        const auto copyError=EXTENT_API(GetError);std::printf("{\"diagnostic\":\"matching_format_copy\",\"case\":\"%s\",\"api\":\"%s\",\"internal\":%u,\"immediate_error\":%u}\n",test.name,Vulkan?"vulkan":"gles",test.hdr?GL_RGBA16F:GL_RGBA8,copyError);Require(copyError==GL_NO_ERROR,"Matching-format copy failed");
        EXTENT_API(BindFramebuffer,GL_FRAMEBUFFER,framebuffers[1]);EXTENT_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[5],0);
        Require(EXTENT_API(CheckFramebufferStatus,GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"Copy destination complete");outputHdr=test.hdr;
    }
    EXTENT_API(Finish);EXTENT_API(PixelStorei,GL_PACK_ALIGNMENT,1);EXTENT_API(PixelStorei,GL_PACK_ROW_LENGTH,0);
    ExtentPixels pixels{width,height,std::vector<unsigned char>(static_cast<std::size_t>(width)*height*4,0xcd)};
    if(outputHdr){
        std::vector<float> floating(pixels.rgba.size());EXTENT_API(ReadPixels,0,0,width,height,GL_RGBA,GL_FLOAT,floating.data());
        for(std::size_t i=0;i<floating.size();++i)pixels.rgba[i]=static_cast<unsigned char>(std::lround(std::clamp(floating[i],0.f,1.f)*255));
    }else EXTENT_API(ReadPixels,0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.rgba.data());
    Require(EXTENT_API(GetError)==GL_NO_ERROR,std::string("Mixed extent operation error: ")+test.name+"/"+ExtentPhaseName(phase));
    EXTENT_API(Disable,GL_STENCIL_TEST);EXTENT_API(DeleteFramebuffers,2,framebuffers);EXTENT_API(DeleteTextures,6,textures);EXTENT_API(DeleteBuffers,2,buffers);EXTENT_API(DeleteVertexArrays,1,&array);
    if constexpr(Vulkan)bl2vk::DeletePrograms(2,programs);
    return pixels;
}
#undef EXTENT_API

bool CompareExtent(const ExtentPixels& reference,const ExtentPixels& candidate,const ExtentCase& test,ExtentPhase phase,const char* output){
    Require(reference.width==candidate.width&&reference.height==candidate.height&&reference.rgba.size()==candidate.rgba.size(),"Extent comparison dimensions");
    unsigned maximum=0;std::size_t changed[2]{};bool masks=true;
    for(std::size_t i=0;i<reference.rgba.size();++i)maximum=std::max(maximum,static_cast<unsigned>(std::abs(int(reference.rgba[i])-candidate.rgba[i])));
    for(const auto& [pixels,api]:{std::pair{&reference,0u},std::pair{&candidate,1u}}){
        for(int y=0;y<pixels->height;++y)for(int x=0;x<pixels->width;++x){
            const auto at=static_cast<std::size_t>(y*pixels->width+x)*4;changed[api]+=!std::equal(kClear.begin(),kClear.end(),pixels->rgba.begin()+at);
            if(phase==ExtentPhase::MaskedClear)masks&=std::abs(int(pixels->rgba[at])-102)<=1&&pixels->rgba[at+1]==kClear[1]&&pixels->rgba[at+2]==kClear[2]&&pixels->rgba[at+3]==kClear[3];
        }
    }
    const int endX=std::min(reference.width,26),endY=std::min(reference.height,26);
    const bool half=phase==ExtentPhase::StencilClip||phase==ExtentPhase::Blit;
    const auto glyphWidth=std::max(0,(half?std::min(endX,reference.width/2):endX)-6),glyphHeight=std::max(0,endY-6);
    const auto expected=phase==ExtentPhase::StencilReject?0u:phase==ExtentPhase::MaskedClear?static_cast<unsigned>(reference.width*reference.height):static_cast<unsigned>(glyphWidth*glyphHeight);
    const bool passed=maximum<=2&&masks&&changed[0]==expected&&changed[1]==expected;
    std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"gles_changed_pixels\":%zu,\"vulkan_changed_pixels\":%zu,\"expected_changed_pixels\":%u,\"max_channel_error\":%u,\"write_mask_preserved\":%s,\"status\":\"%s\"}\n",test.name,ExtentPhaseName(phase),changed[0],changed[1],expected,maximum,masks?"true":"false",passed?"PASS":"FAIL");
    for(const auto& [pixels,api]:{std::pair{&reference,"gles"},std::pair{&candidate,"vulkan"}}){const auto path=std::string(output)+"/extent-"+test.name+"-"+ExtentPhaseName(phase)+"-"+api+".rgba";std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(pixels->rgba.data()),pixels->rgba.size());Require(file.good(),"Write extent pixel evidence");}
    return passed;
}
} // namespace

int main(int argc,char** argv){
    if(argc!=4){std::fputs("usage: native_vulkan_framebuffer_extent_check PACK FIXTURES OUTPUT_DIRECTORY\n",stderr);return 2;}
    try{
        const auto fd=::open(argv[1],O_RDONLY|O_CLOEXEC);Require(fd>=0,"Open extent shader pack");struct stat64 info{};Require(::fstat64(fd,&info)==0&&info.st_size>0,"Extent pack size");
        auto pack=std::make_shared<bl2::vk::ShaderPack>();std::string error;const auto opened=pack->OpenFileDescriptor(fd,0,static_cast<std::uint64_t>(info.st_size),&error);::close(fd);Require(opened,"Extent shader pack: "+error);
        const auto materials=ReadMaterials(argv[2],*pack);const Material* font=nullptr;for(const auto& material:materials)if(material.flags&4)font=&material;Require(font,"Genuine font fixture required");
        const auto display=eglGetDisplay(EGL_DEFAULT_DISPLAY);Require(eglInitialize(display,nullptr,nullptr)&&eglBindAPI(EGL_OPENGL_ES_API),"Extent GLES initialization");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};EGLConfig config{};EGLint count=0;
        Require(eglChooseConfig(display,attributes,&config,1,&count)&&count==1,"Extent GLES config");const EGLint size[]{EGL_WIDTH,kSide,EGL_HEIGHT,kSide,EGL_NONE},version[]{EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
        const auto surface=eglCreatePbufferSurface(display,config,size),context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);Require(surface!=EGL_NO_SURFACE&&context!=EGL_NO_CONTEXT&&eglMakeCurrent(display,surface,surface,context),"Extent GLES context");
        std::printf("ARM32 GLES extent reference: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));const auto program=Program(*font);
        Require(bl2vk::Initialize(pack,argv[3]),std::string("Extent Vulkan initialization: ")+bl2vk::LastFailure());const auto vkContext=bl2vk::CreateContext(0);Require(vkContext&&bl2vk::MakeCurrent(vkContext,nullptr),"Extent Vulkan offscreen context");
        bool passed=true;for(const auto& test:kExtentCases)for(const auto phase:kExtentPhases){try{const auto reference=RenderExtent<false>(*font,program,test,phase),candidate=RenderExtent<true>(*font,0,test,phase);passed=CompareExtent(reference,candidate,test,phase,argv[3])&&passed;}catch(const std::exception& failure){passed=false;std::fprintf(stderr,"FAIL extent %s/%s: %s\n",test.name,ExtentPhaseName(phase),failure.what());}}
        bl2vk::Finish();bl2vk::DestroyContext(vkContext);bl2vk::Shutdown();glDeleteProgram(program);eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
        std::printf("%s %zu original-font mixed framebuffer extent phases; compared only defined common areas\n",passed?"PASS":"FAIL",kExtentCases.size()*kExtentPhases.size());return passed?0:3;
    }catch(const std::exception& failure){std::fprintf(stderr,"FAIL extent oracle: %s\n",failure.what());bl2vk::Shutdown();return 1;}
}
