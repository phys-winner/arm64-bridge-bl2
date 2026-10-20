#include "native_elf_loader.h"
#include "thunk_manager.h"
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

bool ElfLoader::Attach(void* handle) {
    if (!handle) return false;
    handle_=handle;
    void* entry=dlsym(handle,"JNI_OnLoad");
    Dl_info info{};
    if (!entry || !dladdr(entry,&info) || !info.dli_fbase) return false;
    base_=reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    const auto* header=reinterpret_cast<const Elf32_Ehdr*>(base_);
    if (memcmp(header->e_ident,ELFMAG,SELFMAG) || header->e_ident[EI_CLASS]!=ELFCLASS32 || header->e_machine!=EM_ARM) return false;
    const auto* phdr=reinterpret_cast<const Elf32_Phdr*>(base_+header->e_phoff);
    segments_.assign(phdr,phdr+header->e_phnum);
    for (auto& p:segments_) if(p.p_type==PT_LOAD) size_=std::max(size_,p.p_vaddr+p.p_memsz);
    size_=(size_+4095u)&~4095u;
    return size_==0x03089000;
}
std::uint32_t ElfLoader::GetSymbolAddress(const std::string& name) const { return reinterpret_cast<std::uintptr_t>(handle_ ? dlsym(handle_,name.c_str()) : nullptr); }
bool ElfLoader::IsReadOnlyRange(std::uint32_t a,std::size_t n) const {
    if (a<base_) return false;
    for(auto& p:segments_) if(p.p_type==PT_LOAD && !(p.p_flags&PF_W) && a-base_>=p.p_vaddr && std::uint64_t{a-base_}+n<=std::uint64_t{p.p_vaddr}+p.p_memsz) return true;
    return false;
}
bool ElfLoader::IsCodeRange(std::uint32_t a,std::size_t n) const {
    if (a<base_) return false;
    for(auto& p:segments_) if(p.p_type==PT_LOAD && (p.p_flags&PF_X) && a-base_>=p.p_vaddr && std::uint64_t{a-base_}+n<=std::uint64_t{p.p_vaddr}+p.p_memsz) return true;
    return false;
}
bool ElfLoader::RedirectImports(const ThunkManager& manager) {
    const Elf32_Dyn* dynamic=nullptr;
    for(auto& p:segments_) if(p.p_type==PT_DYNAMIC) dynamic=reinterpret_cast<const Elf32_Dyn*>(base_+p.p_vaddr);
    if(!dynamic) return false;
    std::uint32_t symtab=0,strtab=0,rel=0,relBytes=0,jmprel=0,jmpBytes=0;
    for(auto* d=dynamic;d->d_tag!=DT_NULL;++d) switch(d->d_tag) {
        case DT_SYMTAB:symtab=base_+d->d_un.d_ptr;break;
        case DT_STRTAB:strtab=base_+d->d_un.d_ptr;break;
        case DT_REL:rel=base_+d->d_un.d_ptr;break;
        case DT_RELSZ:relBytes=d->d_un.d_val;break;
        case DT_JMPREL:jmprel=base_+d->d_un.d_ptr;break;
        case DT_PLTRELSZ:jmpBytes=d->d_un.d_val;break;
    }
    if(!symtab || !strtab) return false;
    const auto* symbols=reinterpret_cast<const Elf32_Sym*>(symtab);
    const auto* strings=reinterpret_cast<const char*>(strtab);
    const std::size_t pageSize=sysconf(_SC_PAGESIZE);
    std::size_t redirected=0;
    auto patch=[&](std::uint32_t table,std::uint32_t bytes) {
        const auto* rows=reinterpret_cast<const Elf32_Rel*>(table);
        for(std::size_t i=0;i<bytes/sizeof(Elf32_Rel);++i) {
            auto& r=rows[i];
            auto& s=symbols[ELF32_R_SYM(r.r_info)];
            const auto type=ELF32_R_TYPE(r.r_info);
            if(s.st_shndx!=SHN_UNDEF || (type!=R_ARM_JUMP_SLOT && type!=R_ARM_GLOB_DAT)) continue;
            const auto replacement=manager.FindRegisteredSymbol(strings+s.st_name);
            if(!replacement) continue;
            auto* slot=reinterpret_cast<std::uint32_t*>(base_+r.r_offset);
            const auto page=reinterpret_cast<std::uintptr_t>(slot)&~(pageSize-1);
            if(mprotect(reinterpret_cast<void*>(page),pageSize,PROT_READ|PROT_WRITE)) return false;
            *slot=replacement; ++redirected;
            // Relocation slots are in writable ELF segments or GNU RELRO.
            bool relro=false;
            for(auto& p:segments_) if(p.p_type==PT_GNU_RELRO && r.r_offset>=p.p_vaddr && r.r_offset<std::uint64_t{p.p_vaddr}+p.p_memsz) relro=true;
            if(relro && mprotect(reinterpret_cast<void*>(page),pageSize,PROT_READ)) return false;
        }
        return true;
    };
    if(!patch(rel,relBytes)||!patch(jmprel,jmpBytes)) return false;
    __android_log_print(ANDROID_LOG_INFO,"BL2Arm32","Native ARM32 base=%08x imports=%zu",base_,redirected);
    return true;
}
