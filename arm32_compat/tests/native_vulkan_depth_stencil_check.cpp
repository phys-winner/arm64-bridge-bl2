// Reuse the unchanged original-font fixture, source identity, GLES compilation
// and image helpers. The included comparison executable keeps a renamed main.
#define main bl2_depth_stencil_material_helpers_main
#include "native_vulkan_materials_check.cpp"
#undef main

namespace {
enum class DsPhase {
    Baseline, DepthOnlyStencilEqual, BothStencilReject, StencilOnlyDepthNever,
    BothDepthReject, DepthOnlyStencilWrite, StencilOnlyDepthWrite,
    DepthOnlyStencilClear, DepthOnlyStencilMaskedClear, StencilOnlyDepthClear,
    DepthOnlyStencilClearBuffer, StencilOnlyDepthClearBuffer,
    DepthOnlyCombinedClearBuffer, StencilOnlyCombinedClearBuffer,
    BothStencilMaskedClear, BothDepthClear, BothStencilClear,
    SourceWithoutStencilBlit, DestinationWithoutStencilBlit,
    SourceWithoutDepthBlit, DestinationWithoutDepthBlit, BothBlit,
    ColorOnlyTests
};
constexpr std::array<DsPhase,23> kDsPhases{
    DsPhase::Baseline, DsPhase::DepthOnlyStencilEqual, DsPhase::BothStencilReject,
    DsPhase::StencilOnlyDepthNever, DsPhase::BothDepthReject,
    DsPhase::DepthOnlyStencilWrite, DsPhase::StencilOnlyDepthWrite,
    DsPhase::DepthOnlyStencilClear, DsPhase::DepthOnlyStencilMaskedClear,
    DsPhase::StencilOnlyDepthClear, DsPhase::DepthOnlyStencilClearBuffer,
    DsPhase::StencilOnlyDepthClearBuffer, DsPhase::DepthOnlyCombinedClearBuffer,
    DsPhase::StencilOnlyCombinedClearBuffer, DsPhase::BothStencilMaskedClear,
    DsPhase::BothDepthClear, DsPhase::BothStencilClear,
    DsPhase::SourceWithoutStencilBlit, DsPhase::DestinationWithoutStencilBlit,
    DsPhase::SourceWithoutDepthBlit, DsPhase::DestinationWithoutDepthBlit,
    DsPhase::BothBlit, DsPhase::ColorOnlyTests
};
const char* DsName(DsPhase phase) {
    switch(phase) {
        case DsPhase::Baseline:return "both_baseline";
        case DsPhase::DepthOnlyStencilEqual:return "depth_only_absent_stencil_passes";
        case DsPhase::BothStencilReject:return "both_stencil_reject_control";
        case DsPhase::StencilOnlyDepthNever:return "stencil_only_absent_depth_passes";
        case DsPhase::BothDepthReject:return "both_depth_reject_control";
        case DsPhase::DepthOnlyStencilWrite:return "depth_only_stencil_write_preserved";
        case DsPhase::StencilOnlyDepthWrite:return "stencil_only_depth_write_preserved";
        case DsPhase::DepthOnlyStencilClear:return "depth_only_stencil_clear_preserved";
        case DsPhase::DepthOnlyStencilMaskedClear:return "depth_only_masked_stencil_clear_preserved";
        case DsPhase::StencilOnlyDepthClear:return "stencil_only_depth_clear_preserved";
        case DsPhase::DepthOnlyStencilClearBuffer:return "depth_only_stencil_clearbuffer_preserved";
        case DsPhase::StencilOnlyDepthClearBuffer:return "stencil_only_depth_clearbuffer_preserved";
        case DsPhase::DepthOnlyCombinedClearBuffer:return "depth_only_combined_clearbuffer_preserves_stencil";
        case DsPhase::StencilOnlyCombinedClearBuffer:return "stencil_only_combined_clearbuffer_preserves_depth";
        case DsPhase::BothStencilMaskedClear:return "both_masked_stencil_clear_control";
        case DsPhase::BothDepthClear:return "both_depth_clear_control";
        case DsPhase::BothStencilClear:return "both_stencil_clear_control";
        case DsPhase::SourceWithoutStencilBlit:return "blit_source_absent_stencil_preserved";
        case DsPhase::DestinationWithoutStencilBlit:return "blit_destination_absent_stencil_preserved";
        case DsPhase::SourceWithoutDepthBlit:return "blit_source_absent_depth_preserved";
        case DsPhase::DestinationWithoutDepthBlit:return "blit_destination_absent_depth_preserved";
        case DsPhase::BothBlit:return "both_depth_stencil_blit_control";
        case DsPhase::ColorOnlyTests:return "color_only_absent_tests_pass";
    }
    return "invalid";
}
bool DsHidden(DsPhase phase) { return phase==DsPhase::BothStencilReject || phase==DsPhase::BothDepthReject; }

#define DS_API(Name, ...) (Vulkan ? bl2vk::Name(__VA_ARGS__) : gl##Name(__VA_ARGS__))
template<bool Vulkan>
Image RenderDs(const Material& font,GLuint glesProgram,DsPhase phase) {
    GLuint programs[2]{},textures[5]{},framebuffers[2]{},buffers[2]{},array=0;
    if constexpr(Vulkan) {
        std::array<std::int16_t,16> samplers;samplers.fill(-1);
        bl2vk::GenPrograms(2,programs);
        Require(bl2vk::NamedProgramString(programs[0],kVS,font.vertex.raw,font.vertex.groups.data(),samplers.data()),std::string("DS original VS: ")+bl2vk::LastFailure());
        Require(bl2vk::NamedProgramString(programs[1],kFS,font.fragment.raw,font.fragment.groups.data(),samplers.data()),std::string("DS original FS: ")+bl2vk::LastFailure());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[0],kVS,0,static_cast<GLsizei>(font.vertex.words.size()),font.vertex.words.front().data());
        bl2vk::NamedProgramLocalParametersI4uiv(programs[1],kFS,0,static_cast<GLsizei>(font.fragment.words.size()),font.fragment.words.front().data());
        bl2vk::BindProgram(kVS,programs[0]);bl2vk::BindProgram(kFS,programs[1]);
    } else {
        glUseProgram(glesProgram);Uniforms(glesProgram,font.vertex,font.vertex.words);Uniforms(glesProgram,font.fragment,font.fragment.words);
        const auto sampler=glGetUniformLocation(glesProgram,"PSampler0");Require(sampler>=0,"Original font sampler");glUniform1i(sampler,0);
    }
    DS_API(GenTextures,5,textures);DS_API(ActiveTexture,GL_TEXTURE0);
    for(const auto index:{0,4}) {
        DS_API(BindTexture,GL_TEXTURE_2D,textures[index]);DS_API(TexStorage2D,GL_TEXTURE_2D,1,GL_RGBA8,kSide,kSide);
    }
    for(const auto index:{1,2}) {
        DS_API(BindTexture,GL_TEXTURE_2D,textures[index]);DS_API(TexStorage2D,GL_TEXTURE_2D,1,GL_DEPTH24_STENCIL8,kSide,kSide);
    }
    DS_API(BindTexture,GL_TEXTURE_2D,textures[3]);DS_API(PixelStorei,GL_UNPACK_ALIGNMENT,1);
    const auto atlas=SingleChannel(Phase::Baseline,true);
    DS_API(TexImage2D,GL_TEXTURE_2D,0,GL_R8,8,8,0,GL_RED,GL_UNSIGNED_BYTE,atlas.data());
    DS_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);DS_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    DS_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);DS_API(TexParameteri,GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    DS_API(BindSampler,0,0);
    DS_API(GenVertexArrays,1,&array);DS_API(BindVertexArray,array);DS_API(GenBuffers,2,buffers);
    DS_API(BindBuffer,GL_ARRAY_BUFFER,buffers[0]);const auto vertices=FontVertices(font,Phase::Baseline);const std::uintptr_t origin=Vulkan?2:0;
    DS_API(BufferData,GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(Vulkan?vertices.size():vertices.size()-2),vertices.data()+(Vulkan?0:2),GL_STATIC_DRAW);
    for(const auto location:{0u,2u,8u}) {
        DS_API(EnableVertexAttribArray,location);
        DS_API(VertexAttribPointer,location,location==2?4:2,location==2?GL_UNSIGNED_BYTE:GL_FLOAT,location==2?GL_TRUE:GL_FALSE,20,
               reinterpret_cast<const void*>(origin+(location==2?16:location==8?8:0)));
    }
    constexpr std::array<std::uint16_t,6> indices{0,1,2,3,4,5};DS_API(BindBuffer,GL_ELEMENT_ARRAY_BUFFER,buffers[1]);
    DS_API(BufferData,GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices.data(),GL_STATIC_DRAW);
    DS_API(Viewport,0,0,kSide,kSide);DS_API(Disable,GL_SCISSOR_TEST);DS_API(Disable,GL_CULL_FACE);DS_API(Disable,GL_BLEND);
    DS_API(Disable,GL_DEPTH_TEST);DS_API(Disable,GL_STENCIL_TEST);DS_API(DepthMask,GL_TRUE);DS_API(FrontFace,GL_CCW);
    DS_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);DS_API(StencilMaskSeparate,GL_FRONT_AND_BACK,255);
    DS_API(StencilOpSeparate,GL_FRONT_AND_BACK,GL_KEEP,GL_KEEP,GL_KEEP);
    DS_API(ClearColor,kClear[0]/255.f,kClear[1]/255.f,kClear[2]/255.f,kClear[3]/255.f);
    DS_API(GenFramebuffers,2,framebuffers);
    const auto attach=[&](unsigned framebuffer,unsigned texture,bool depth,bool stencil) {
        DS_API(BindFramebuffer,GL_FRAMEBUFFER,framebuffers[framebuffer]);
        DS_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[framebuffer?4:0],0);
        DS_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depth?textures[texture]:0,0);
        DS_API(FramebufferTexture2D,GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_TEXTURE_2D,stencil?textures[texture]:0,0);
        Require(DS_API(CheckFramebufferStatus,GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,std::string("DS FBO: ")+DsName(phase));
    };
    const auto initialize=[&](float depth,GLint stencil) {
        DS_API(DepthMask,GL_TRUE);DS_API(StencilMaskSeparate,GL_FRONT_AND_BACK,255);DS_API(ClearDepthf,depth);DS_API(ClearStencil,stencil);
        DS_API(Clear,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    };
    const auto draw=[&] {DS_API(DrawElements,GL_TRIANGLES,6,GL_UNSIGNED_SHORT,nullptr);};
    const auto readColor=[&] {
        DS_API(Finish);Image image;image.fill(0xcd);DS_API(PixelStorei,GL_PACK_ALIGNMENT,1);
        DS_API(ReadPixels,0,0,kSide,kSide,GL_RGBA,GL_UNSIGNED_BYTE,image.data());return image;
    };
    const auto stencilProbe=[&](GLint reference) {
        DS_API(Disable,GL_DEPTH_TEST);DS_API(Enable,GL_STENCIL_TEST);
        DS_API(StencilFuncSeparate,GL_FRONT_AND_BACK,GL_EQUAL,reference,255);
        DS_API(StencilOpSeparate,GL_FRONT_AND_BACK,GL_KEEP,GL_KEEP,GL_KEEP);
    };
    const auto depthProbe=[&](GLenum function) {DS_API(Disable,GL_STENCIL_TEST);DS_API(Enable,GL_DEPTH_TEST);DS_API(DepthFunc,function);DS_API(DepthMask,GL_FALSE);};
    attach(0,1,true,true);initialize(.25f,0x35);
    if(phase==DsPhase::DepthOnlyStencilEqual || phase==DsPhase::BothStencilReject) {
        if(phase==DsPhase::DepthOnlyStencilEqual)attach(0,1,true,false);stencilProbe(1);
    } else if(phase==DsPhase::StencilOnlyDepthNever || phase==DsPhase::BothDepthReject) {
        if(phase==DsPhase::StencilOnlyDepthNever)attach(0,1,false,true);depthProbe(GL_NEVER);
    } else if(phase==DsPhase::DepthOnlyStencilWrite) {
        attach(0,1,true,false);DS_API(Enable,GL_STENCIL_TEST);DS_API(StencilFuncSeparate,GL_FRONT_AND_BACK,GL_ALWAYS,0x7a,255);
        DS_API(StencilOpSeparate,GL_FRONT_AND_BACK,GL_REPLACE,GL_REPLACE,GL_REPLACE);DS_API(ColorMask,GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);draw();
        DS_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);attach(0,1,true,true);stencilProbe(0x35);
    } else if(phase==DsPhase::StencilOnlyDepthWrite) {
        attach(0,1,false,true);DS_API(Enable,GL_DEPTH_TEST);DS_API(DepthFunc,GL_ALWAYS);DS_API(DepthMask,GL_TRUE);
        DS_API(ColorMask,GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);draw();DS_API(ColorMask,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        attach(0,1,true,true);depthProbe(GL_GREATER);
    } else if(phase==DsPhase::DepthOnlyStencilClear || phase==DsPhase::DepthOnlyStencilMaskedClear || phase==DsPhase::DepthOnlyStencilClearBuffer) {
        attach(0,1,true,false);DS_API(StencilMaskSeparate,GL_FRONT_AND_BACK,phase==DsPhase::DepthOnlyStencilMaskedClear?15:255);
        if(phase==DsPhase::DepthOnlyStencilClearBuffer) {const GLint value=0x7a;DS_API(ClearBufferiv,GL_STENCIL,0,&value);}
        else {DS_API(ClearStencil,0x7a);DS_API(Clear,GL_STENCIL_BUFFER_BIT);}
        attach(0,1,true,true);stencilProbe(0x35);
    } else if(phase==DsPhase::StencilOnlyDepthClear || phase==DsPhase::StencilOnlyDepthClearBuffer) {
        attach(0,1,false,true);
        if(phase==DsPhase::StencilOnlyDepthClearBuffer) {const GLfloat value=.75f;DS_API(ClearBufferfv,GL_DEPTH,0,&value);}
        else {DS_API(ClearDepthf,.75f);DS_API(Clear,GL_DEPTH_BUFFER_BIT);}
        attach(0,1,true,true);depthProbe(GL_GREATER);
    } else if(phase==DsPhase::DepthOnlyCombinedClearBuffer || phase==DsPhase::StencilOnlyCombinedClearBuffer) {
        const bool depthOnly=phase==DsPhase::DepthOnlyCombinedClearBuffer;attach(0,1,depthOnly,!depthOnly);
        DS_API(ClearBufferfi,GL_DEPTH_STENCIL,0,.75f,0x7a);attach(0,1,true,true);
        depthProbe(depthOnly?GL_LESS:GL_GREATER);stencilProbe(depthOnly?0x35:0x7a);DS_API(Enable,GL_DEPTH_TEST);
    } else if(phase==DsPhase::BothStencilMaskedClear || phase==DsPhase::BothStencilClear) {
        DS_API(StencilMaskSeparate,GL_FRONT_AND_BACK,phase==DsPhase::BothStencilMaskedClear?15:255);
        DS_API(ClearStencil,0x7a);DS_API(Clear,GL_STENCIL_BUFFER_BIT);stencilProbe(phase==DsPhase::BothStencilMaskedClear?0x3a:0x7a);
    } else if(phase==DsPhase::BothDepthClear) {
        DS_API(ClearDepthf,.75f);DS_API(Clear,GL_DEPTH_BUFFER_BIT);depthProbe(GL_LESS);
    } else if(phase==DsPhase::ColorOnlyTests) {
        attach(0,1,false,false);stencilProbe(1);DS_API(Enable,GL_DEPTH_TEST);DS_API(DepthFunc,GL_NEVER);
    } else if(phase!=DsPhase::Baseline) {
        const bool stencil=phase==DsPhase::SourceWithoutStencilBlit || phase==DsPhase::DestinationWithoutStencilBlit;
        const bool sourceMissing=phase==DsPhase::SourceWithoutStencilBlit || phase==DsPhase::SourceWithoutDepthBlit;
        const bool destinationMissing=phase==DsPhase::DestinationWithoutStencilBlit || phase==DsPhase::DestinationWithoutDepthBlit;
        attach(1,2,true,true);initialize(.75f,0x7a);
        if(phase==DsPhase::BothBlit) {
            // Test the source sentinels before copying. Use independent color
            // probes so GLES does not need noncore depth/stencil ReadPixels.
            depthProbe(GL_LESS);draw();const auto depthVisible=Visible(readColor());DS_API(Clear,GL_COLOR_BUFFER_BIT);
            stencilProbe(0x7a);draw();const auto stencilVisible=Visible(readColor());DS_API(Clear,GL_COLOR_BUFFER_BIT);
            std::printf("{\"phase\":\"%s\",\"api\":\"%s\",\"source_depth_probe\":%zu,\"source_stencil_probe\":%zu}\n",DsName(phase),Vulkan?"vulkan":"gles",depthVisible,stencilVisible);
            Require(depthVisible>=64 && stencilVisible>=64,"BOTH blit source sentinels");
        }
        if(sourceMissing)attach(1,2,stencil,!stencil);
        attach(0,1,destinationMissing?stencil:true,destinationMissing?!stencil:true);
        DS_API(Disable,GL_DEPTH_TEST);DS_API(Disable,GL_STENCIL_TEST);
        DS_API(DepthMask,GL_TRUE);DS_API(StencilMaskSeparate,GL_FRONT_AND_BACK,255);
        Require(DS_API(GetError)==GL_NO_ERROR,"DS pre-blit state");
        DS_API(BindFramebuffer,GL_READ_FRAMEBUFFER,framebuffers[1]);DS_API(BindFramebuffer,GL_DRAW_FRAMEBUFFER,framebuffers[0]);
        const auto mask=phase==DsPhase::BothBlit?GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT:stencil?GL_STENCIL_BUFFER_BIT:GL_DEPTH_BUFFER_BIT;
        DS_API(BlitFramebuffer,0,0,kSide,kSide,0,0,kSide,kSide,mask,GL_NEAREST);
        const auto blitError=DS_API(GetError);
        Require(blitError==GL_NO_ERROR,std::string("DS blit ")+DsName(phase)+" error="+std::to_string(blitError));
        if(phase==DsPhase::BothBlit)DS_API(BindFramebuffer,GL_FRAMEBUFFER,framebuffers[0]);
        else attach(0,1,true,true);
        if(phase==DsPhase::BothBlit) {
            depthProbe(GL_LESS);draw();const auto depthVisible=Visible(readColor());DS_API(Clear,GL_COLOR_BUFFER_BIT);
            depthProbe(GL_GREATER);draw();const auto staleDepthVisible=Visible(readColor());DS_API(Clear,GL_COLOR_BUFFER_BIT);
            stencilProbe(0x7a);draw();const auto stencilVisible=Visible(readColor());DS_API(Clear,GL_COLOR_BUFFER_BIT);
            const auto probeError=DS_API(GetError);
            std::printf("{\"phase\":\"%s\",\"api\":\"%s\",\"copied_depth_probe\":%zu,\"old_depth_probe\":%zu,\"copied_stencil_probe\":%zu,\"probe_error\":%u}\n",DsName(phase),Vulkan?"vulkan":"gles",depthVisible,staleDepthVisible,stencilVisible,probeError);
            Require(probeError==GL_NO_ERROR && depthVisible>=64 && staleDepthVisible==0 && stencilVisible>=64,"BOTH blit copied sentinels");
        }
        else if(stencil)stencilProbe(0x35);else depthProbe(GL_GREATER);
    }
    draw();const auto image=readColor();
    const auto error=DS_API(GetError);Require(error==GL_NO_ERROR,std::string(Vulkan?"Vulkan":"GLES")+" DS "+DsName(phase)+" error="+std::to_string(error)+(Vulkan?std::string(" ")+bl2vk::LastFailure():""));
    DS_API(BindFramebuffer,GL_FRAMEBUFFER,0);DS_API(DeleteFramebuffers,2,framebuffers);DS_API(DeleteTextures,5,textures);
    DS_API(DeleteBuffers,2,buffers);DS_API(DeleteVertexArrays,1,&array);if constexpr(Vulkan)bl2vk::DeletePrograms(2,programs);
    return image;
}
#undef DS_API
bool CompareDs(const Image& gles,const Image& vk,DsPhase phase,const char* directory) {
    unsigned maximum=0;std::size_t different=0;
    for(std::size_t i=0;i<gles.size();++i) {const auto difference=static_cast<unsigned>(std::abs(int(gles[i])-int(vk[i])));maximum=std::max(maximum,difference);different+=difference!=0;}
    const auto reference=Visible(gles),candidate=Visible(vk);
    const bool oracle=DsHidden(phase)?reference==0 && candidate==0:reference>=64 && candidate>=64;
    const bool passed=maximum<=3 && oracle;
    std::printf("{\"case\":\"logical_depth_stencil\",\"phase\":\"%s\",\"expected_visible\":%u,\"gles_visible\":%zu,\"vulkan_visible\":%zu,\"max_channel_error\":%u,\"changed_channels\":%zu,\"status\":\"%s\"}\n",DsName(phase),!DsHidden(phase),reference,candidate,maximum,different,passed?"PASS":"FAIL");
    for(const auto& item:{std::pair{"gles",&gles},std::pair{"vulkan",&vk}}) {
        std::ofstream output(std::string(directory)+"/depth-stencil-"+DsName(phase)+"-"+item.first+".rgba",std::ios::binary);
        output.write(reinterpret_cast<const char*>(item.second->data()),item.second->size());Require(output.good(),"Write DS GPU evidence");
    }
    return passed;
}
}

int main(int argc,char** argv) {
    if(argc!=4) {std::fputs("usage: native_vulkan_depth_stencil_check PACK MATERIALS_V5 OUTPUT_DIRECTORY\n",stderr);return 2;}
    try {
        const auto fd=::open(argv[1],O_RDONLY|O_CLOEXEC);Require(fd>=0,"Open DS shader pack");struct stat64 info{};
        Require(::fstat64(fd,&info)==0 && info.st_size>0,"DS pack descriptor");auto pack=std::make_shared<bl2::vk::ShaderPack>();std::string error;
        const bool opened=pack->OpenFileDescriptor(fd,0,static_cast<std::uint64_t>(info.st_size),&error);::close(fd);Require(opened,"DS pack: "+error);
        const auto materials=ReadMaterials(argv[2],*pack);const auto font=std::find_if(materials.begin(),materials.end(),[](const Material& m){return m.flags==4;});Require(font!=materials.end(),"Unchanged V5 original-font fixture");
        const auto display=eglGetDisplay(EGL_DEFAULT_DISPLAY);Require(eglInitialize(display,nullptr,nullptr) && eglBindAPI(EGL_OPENGL_ES_API),"DS GLES initialization");
        const EGLint attributes[]{EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        EGLConfig config{};EGLint count=0;Require(eglChooseConfig(display,attributes,&config,1,&count) && count==1,"DS GLES configuration");
        const EGLint dimensions[]{EGL_WIDTH,kSide,EGL_HEIGHT,kSide,EGL_NONE},version[]{EGL_CONTEXT_MAJOR_VERSION_KHR,3,EGL_CONTEXT_MINOR_VERSION_KHR,2,EGL_NONE};
        const auto surface=eglCreatePbufferSurface(display,config,dimensions),context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);
        Require(surface!=EGL_NO_SURFACE && context!=EGL_NO_CONTEXT && eglMakeCurrent(display,surface,surface,context),"DS GLES3.2 context");
        std::printf("ARM32 DS oracle GLES GPU: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
        const auto program=Program(*font);Require(bl2vk::Initialize(pack,argv[3]),std::string("DS Vulkan initialize: ")+bl2vk::LastFailure());
        const auto vkContext=bl2vk::CreateContext(0);Require(vkContext && bl2vk::MakeCurrent(vkContext,nullptr),"DS Vulkan context");bool passed=true;
        for(const auto phase:kDsPhases) {
            try {const auto reference=RenderDs<false>(*font,program,phase),candidate=RenderDs<true>(*font,0,phase);passed=CompareDs(reference,candidate,phase,argv[3]) && passed;}
            catch(const std::exception& failure) {passed=false;std::fprintf(stderr,"FAIL logical depth/stencil %s: %s\n",DsName(phase),failure.what());}
        }
        bl2vk::Finish();bl2vk::DestroyContext(vkContext);bl2vk::Shutdown();glDeleteProgram(program);
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);eglDestroySurface(display,surface);eglTerminate(display);
        std::printf("%s %zu logical depth/stencil attachment phases with unchanged original font shaders\n",passed?"PASS":"FAIL",kDsPhases.size());return passed?0:3;
    } catch(const std::exception& failure) {std::fprintf(stderr,"FAIL DS oracle: %s\n",failure.what());bl2vk::Shutdown();return 1;}
}
