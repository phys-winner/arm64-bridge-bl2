#pragma once
#if BL2_ENABLE_QA_CONSOLE && !defined(BL2_NATIVE_ARM32) && !defined(BL2_STATIC_AOT)
#include <android/log.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unistd.h>
class DynarmicHost;
namespace bl2::iris::load_trace_qa {
struct State {
    const DynarmicHost* owner=nullptr;
    std::uint64_t events=0;
    unsigned messages=0;
    unsigned diagnosticEvents=0;
};
inline thread_local State state;
struct ErrnoGuard {
    int saved=errno;
    ~ErrnoGuard(){errno=saved;}
};
inline bool Read(const std::uint8_t* base,std::size_t arena,std::uint32_t address,void* output,std::size_t size) {
    const ErrnoGuard preserve;
    const auto pointer=reinterpret_cast<std::uintptr_t>(base);
    constexpr auto limit=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if(!base||address<0x10000||address>arena||size>arena-address||pointer>limit||address>limit-pointer||size>limit-pointer-address)return false;
    const int fd=::open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
    if(fd<0)return false;
    const bool read=::pread64(fd,output,size,static_cast<off64_t>(pointer+address))==static_cast<ssize_t>(size);
    ::close(fd);
    return read;
}
template<std::size_t N> inline bool Text(const std::uint8_t* base,std::size_t arena,std::uint32_t address,char(&output)[N]) {
    if(address>=arena)return false;
    const auto available=arena-address;
    const auto count=available<N-1?available:N-1;
    const bool valid=count&&Read(base,arena,address,output,count);
    output[valid?count:0]=0;
    return valid;
}
inline void ForwardLog(const DynarmicHost& host,const std::uint8_t* base,std::size_t arena,const std::uint32_t* regs,bool write) {
    if(state.owner!=&host||state.messages>=16)return;
    const ErrnoGuard preserve;
    const auto message=++state.messages;
    char format[257]={},text[1025]={};
    const bool format_valid=!write&&Text(base,arena,regs[2],format);
    const bool direct=write||(format_valid&&(!std::strcmp(format,"%s")||!std::strcmp(format,"%s\n")||!std::strcmp(format,"%s\r\n")));
    const bool text_valid=direct&&Text(base,arena,regs[write?2:3],text);
    __android_log_print(ANDROID_LOG_INFO,"BL2IrisLoadSVC",
        "load-guest-log message=%u kind=%s format_readable=%u text_readable=%u format=%.256s text=%.1024s",
        message,write?"write":"print",unsigned(format_valid),unsigned(text_valid),format,text);
}
class Scope {
    bool owns_=false;
public:
    Scope(const DynarmicHost& host,bool isLoadPackage) {
        if(isLoadPackage&&!state.owner) {
            const ErrnoGuard preserve;
            state.owner=&host;state.events=0;state.messages=0;state.diagnosticEvents=0;owns_=true;
            __android_log_print(ANDROID_LOG_INFO,"BL2IrisLoadSVC","load-svc begin owner=%p",static_cast<const void*>(&host));
        }
    }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    ~Scope() {
        if(owns_) {
            const ErrnoGuard preserve;
            __android_log_print(ANDROID_LOG_INFO,"BL2IrisLoadSVC","load-svc end owner=%p events=%llu",static_cast<const void*>(state.owner),static_cast<unsigned long long>(state.events));
            state.owner=nullptr;
        }
    }
};
}
#endif
