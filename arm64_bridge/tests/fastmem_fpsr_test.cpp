// Real A32 JIT regression: fastmem, fault callbacks and guest FPSCR accumulation.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

namespace {
constexpr size_t kSize = 0x100000000ull;
constexpr uint32_t kHole = 0x10000, kCode = 0x1000, kData = 0x8000;
constexpr uint32_t kFlags = 0x0800009f;
constexpr uint32_t kProgram[] = {
    0xed910a00, 0xedd10a01, 0xee801a20, 0xedd21a00,
    0xee302a21, 0xed822a01, 0xeef10a10, 0xef000000,
};
struct Machine : Dynarmic::A32::UserCallbacks {
    uint8_t* memory = static_cast<uint8_t*>(mmap(nullptr,kSize,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));
    std::array<uint8_t,16384> shadow{};
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    bool hole = false, check_flags = true;
    uint32_t expected_flags = 0;
    unsigned callbacks = 0;
    Machine(bool fast, bool fault, bool recompile) : hole(fault) {
        assert(memory != MAP_FAILED);
        std::memcpy(memory+kCode,kProgram,sizeof(kProgram));
        const uint32_t data[] = {0x3f800000,0};
        std::memcpy(memory+kData,data,sizeof(data));
        std::memcpy(memory+kHole,data,sizeof(data));
        std::memcpy(shadow.data(),data,sizeof(data));
        if(hole) assert(mprotect(memory+kHole,shadow.size(),PROT_NONE)==0);
        Dynarmic::A32::UserConfig config;
        config.callbacks=this;
        config.arch_version=Dynarmic::A32::ArchVersion::v7;
        config.enable_cycle_counting=false;
        config.always_little_endian=true;
        config.recompile_on_fastmem_failure=recompile;
        if(fast) config.fastmem_pointer=reinterpret_cast<uintptr_t>(memory);
        jit=std::make_unique<Dynarmic::A32::Jit>(config);
    }
    ~Machine() { jit.reset(); munmap(memory,kSize); }
    uint8_t* Address(uint32_t addr,size_t size) {
        assert(static_cast<uint64_t>(addr)+size<=kSize);
        return hole && addr>=kHole && addr+size<=kHole+shadow.size()
            ? shadow.data()+addr-kHole : memory+addr;
    }
    void Callback(uint32_t addr) {
        ++callbacks;
        if(check_flags && addr>=kHole && addr<kHole+8)
            assert((jit->Fpscr() & kFlags)==expected_flags);
        // A native callback may change hardware FPSR; these are not guest flags.
        uint64_t poison=0x9d;
        asm volatile("msr fpsr, %0" :: "r"(poison));
    }
    template<class T> T Read(uint32_t addr) {
        T value; std::memcpy(&value,Address(addr,sizeof(T)),sizeof(T)); Callback(addr); return value;
    }
    template<class T> void Write(uint32_t addr,T value) {
        std::memcpy(Address(addr,sizeof(T)),&value,sizeof(T)); Callback(addr);
    }
    std::optional<uint32_t> MemoryReadCode(uint32_t addr) override {
        uint32_t value; std::memcpy(&value,memory+addr,4); return value;
    }
    uint8_t MemoryRead8(uint32_t a) override { return Read<uint8_t>(a); }
    uint16_t MemoryRead16(uint32_t a) override { return Read<uint16_t>(a); }
    uint32_t MemoryRead32(uint32_t a) override { return Read<uint32_t>(a); }
    uint64_t MemoryRead64(uint32_t a) override { return Read<uint64_t>(a); }
    void MemoryWrite8(uint32_t a,uint8_t v) override { Write(a,v); }
    void MemoryWrite16(uint32_t a,uint16_t v) override { Write(a,v); }
    void MemoryWrite32(uint32_t a,uint32_t v) override { Write(a,v); }
    void MemoryWrite64(uint32_t a,uint64_t v) override { Write(a,v); }
    void InterpreterFallback(uint32_t,size_t) override { assert(false); }
    void ExceptionRaised(uint32_t,Dynarmic::A32::Exception) override { assert(false); }
    void CallSVC(uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return UINT64_MAX; }
    void Run(uint32_t fpscr) {
        jit->Regs().fill(0); jit->ExtRegs().fill(0);
        jit->Regs()[1]=kData; jit->Regs()[2]=kHole; jit->Regs()[15]=kCode;
        jit->SetCpsr(0x10); jit->SetFpscr(fpscr);
        expected_flags=(fpscr & kFlags)|2;
        jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        assert(Dynarmic::Has(jit->Run(),Dynarmic::HaltReason::UserDefined1));
        assert(jit->Regs()[0]==(fpscr|2));
        assert(jit->Fpscr()==(fpscr|2));
        uint32_t result; std::memcpy(&result,Address(kHole+4,4),4);
        assert(result==0x40000000);
    }
};


}
int main(int argc,char** argv) {
    unsigned checks=0;
    for(auto mode : {0u,1u<<22,2u<<22,3u<<22}) {
        for(auto initial : {0u,0x08000000u}) {
            for(unsigned path=0;path<4;++path) {
                Machine m(path!=0,path>=2,path==3);
                for(unsigned repeat=0;repeat<3;++repeat) { m.Run(mode|initial); ++checks; }
                if(path>=2) assert(m.callbacks>=6);
            }
        }
    }
    std::printf("fastmem FPSCR: PASS %u runs, direct/callback/fault/recompile, 4 rounding modes, sticky QC/DZC, callback pollution\n",checks);
    if(argc==2) {
        FILE* file=std::fopen(argv[1],"rb"); assert(file);
        std::array<uint8_t,404> code{};
        assert(std::fread(code.data(),1,code.size(),file)==code.size());
        assert(std::fgetc(file)==EOF); std::fclose(file);
        uint32_t first,last;
        std::memcpy(&first,code.data(),4); std::memcpy(&last,code.data()+400,4);
        assert(first==0xed2d8b10 && last==0xe12fff1e);
        Machine direct(true,false,false), reference(false,false,false);
        auto setup=[&](Machine& m) {
            m.check_flags=false;
            std::memcpy(m.memory+kCode,code.data(),code.size());
            const uint32_t svc=0xef000000; std::memcpy(m.memory+0x2000,&svc,4);
        };
        setup(direct); setup(reference);
        auto run=[&](Machine& m,uint32_t mode) {
            m.jit->Regs().fill(0); m.jit->ExtRegs().fill(0xaabbccdd);
            m.jit->Regs()[0]=kData+128; m.jit->Regs()[1]=kData;
            m.jit->Regs()[2]=kData+64; m.jit->Regs()[13]=0x30000;
            m.jit->Regs()[14]=0x2000; m.jit->Regs()[15]=kCode;
            m.jit->SetCpsr(0x10); m.jit->SetFpscr(mode);
            m.jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
            assert(Dynarmic::Has(m.jit->Run(),Dynarmic::HaltReason::UserDefined1));
        };
        uint32_t random=0x4d425746;
        for(unsigned i=0;i<2000;++i) {
            for(unsigned word=0;word<48;++word) {
                random^=random<<13; random^=random>>17; random^=random<<5;
                std::memcpy(direct.memory+kData+word*4,&random,4);
                std::memcpy(reference.memory+kData+word*4,&random,4);
            }
            uint32_t mode=(i%4)<<22;
            if(i&4) mode|=1u<<24; // FZ
            if(i&8) mode|=1u<<25; // DN
            run(direct,mode); run(reference,mode);
            assert(std::memcmp(direct.memory+kData+128,reference.memory+kData+128,64)==0);
            assert(direct.jit->Regs()==reference.jit->Regs());
            assert(direct.jit->ExtRegs()==reference.jit->ExtRegs());
            assert(direct.jit->Fpscr()==reference.jit->Fpscr());
        }
        std::printf("stock matrix: PASS 2000 bit-exact outputs/registers/FPSCR including NaN, subnormal, rounding/FZ/DN\n");
        for(unsigned word=0;word<48;++word) {
            const uint32_t finite=0x3f800000+word*0x100;
            std::memcpy(direct.memory+kData+word*4,&finite,4);
        }
        const auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<50000;++i) run(direct,0);
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        std::printf("stock matrix direct fastmem: 50000 calls, %lld us\n",static_cast<long long>(us));
    }
}
