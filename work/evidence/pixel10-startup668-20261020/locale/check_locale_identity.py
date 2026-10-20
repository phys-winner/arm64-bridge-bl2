from pathlib import Path
import hashlib
import json
import re
import subprocess

ROOT=Path(__file__).resolve().parents[4]
OUT=Path(__file__).resolve().parent
source=ROOT/"arm64_bridge/src/bl2_locale.cpp"
text=source.read_text(encoding="utf-8")
def function(code,anchor):
    start=code.index(anchor);opening=code.index("{",start);depth=1
    for i in range(opening+1,len(code)):
        depth+=(code[i]=="{")-(code[i]=="}")
        if not depth:return code[start:i+1]
    raise AssertionError(anchor)
verify=function(text,"bool VerifyBl2LocaleGuest(")
ownership=function(text,"bool InstallBl2OrchidOwnership(")
owns=function(text,"bool Bl2OrchidOwnsApp(")
assert "s_payload_valid" not in verify
assert "if (!s_payload_valid) return true;" in function(text,"bool InstallBl2LocaleOverlay(")
assert "if (!BL2_ENABLE_DLC_CONTENT) return;" in text
legacy=verify.replace("VerifyBl2LocaleGuest", "LegacyVerifyBl2LocaleGuest",1).replace("    ObbSha256::Digest digest{};", "    if (!s_payload_valid) return true;\n    ObbSha256::Digest digest{};",1)
guest_sha=re.search(r'constexpr char kGuestSha256\[\]\s*=\s*"([A-F0-9]+)";',text).group(1)
assert guest_sha=="A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814"
ids={}
for role in ("sage","iris"):
    code=(ROOT/f"arm64_bridge/src/bl2_{role}_admission_data.h").read_text(encoding="utf-8")
    ids[role]=int(re.search(r'kAppId\s*=\s*(\d+)',code).group(1))
prefix=r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>
'''
prefix+=f'#include "{(ROOT/"arm64_bridge/src/obb_sha256.h").as_posix()}"\n'
prefix+=f'constexpr char kGuestSha256[]="{guest_sha}";\n'
prefix+=f'namespace bl2_sage_admission{{constexpr uint32_t kAppId={ids["sage"]};}}\nnamespace bl2_iris_admission{{constexpr uint32_t kAppId={ids["iris"]};}}\n'
prefix+=r'''
#if defined(BL2_NATIVE_ARM32)
bool Bl2NativeSha256Transform(uint32_t*,const uint8_t*,const uint32_t*){return false;}
#endif
bool s_guest_valid=false,s_payload_valid=false,s_russian=false,s_orchid_ownership_installed=false;
std::atomic<bool> s_orchid_admitted{},s_orchid_published{},s_sage_admitted{},s_sage_published{},s_iris_admitted{},s_iris_published{};
unsigned logErrors=0;
#define LOGI(...) ((void)0)
#define LOGE(...) (++logErrors)
namespace Dynarmic::A32 {struct Jit{std::array<uint32_t,16> regs{};auto& Regs(){return regs;}};}
class DynarmicHost;
struct Loader {
    uint32_t base=0x10000000,image=0x03089000;
    bool goodSymbol=true,readOnly=true;
    uint32_t GetBaseAddress()const{return base;}
    uint32_t GetImageSize()const{return image;}
    uint32_t GetSymbolAddress(const char*)const{return goodSymbol?base+0x01F58DF4:0;}
    bool IsReadOnlyRange(uint32_t address,size_t bytes)const{return readOnly&&address==base+0x01F58DF4&&bytes==8;}
};
struct Manager {
    std::function<void(Dynarmic::A32::Jit&,DynarmicHost&)> callback;
    std::vector<std::string> names;
    uint32_t RegisterThunk(const char* name,std::function<void(Dynarmic::A32::Jit&,DynarmicHost&)> next){
        names.emplace_back(name);callback=std::move(next);return 0x20000000;
    }
};
class DynarmicHost {
public:
    Loader loader;Manager manager;unsigned writes=0,invalidations=0;
    std::unordered_map<uint32_t,uint32_t> words;
    DynarmicHost(){words[loader.base+0x01F58DF4]=0xE3A00000;words[loader.base+0x01F58DF8]=0xE12FFF1E;words[0x20000000]=0xEF000055;}
    Loader& GetElfLoader(){return loader;}
    Manager& GetThunkManager(){return manager;}
    uint64_t GetGuestMemorySize(){return 0x100000000ULL;}
    uint32_t MemoryRead32(uint32_t a){return words[a];}
    void MemoryWrite32(uint32_t a,uint32_t word){++writes;words[a]=word;}
    void InvalidateGuestCode(uint32_t,size_t){++invalidations;}
};
'''
suffix=r'''
void Reset(){s_orchid_ownership_installed=false;s_guest_valid=false;}
int main(int argc,char** argv){
    assert(argc==2);
    std::ifstream file(argv[1],std::ios::binary);assert(file);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    assert(bytes.size()==49885152);
    {
        Reset();s_russian=false;s_payload_valid=false;
        const ObbSha256::MappingScope mapping(bytes.data(),bytes.size());
        assert(LegacyVerifyBl2LocaleGuest(bytes.data(),bytes.size())&&!s_guest_valid);
        DynarmicHost host;assert(!InstallBl2OrchidOwnership(host));
        assert(host.manager.names.empty()&&host.writes==0);
    }
    for(bool russian:{false,true}){
        Reset();s_russian=russian;s_payload_valid=russian;
        const ObbSha256::MappingScope mapping(bytes.data(),bytes.size());
        assert(VerifyBl2LocaleGuest(bytes.data(),bytes.size())&&s_guest_valid);
        DynarmicHost host;assert(InstallBl2OrchidOwnership(host));
        assert(host.manager.names==std::vector<std::string>{"bl2_orchid_local_ownership"});
        assert(host.writes==2&&host.invalidations==1);
        assert(InstallBl2OrchidOwnership(host)&&host.manager.names.size()==1);
        for(uint32_t app:{0u,1u,207870u,bl2_sage_admission::kAppId,bl2_iris_admission::kAppId,0xffffffffu}){
            Dynarmic::A32::Jit jit;jit.Regs()[1]=app;host.manager.callback(jit,host);assert(jit.Regs()[0]==0);
        }
        for(unsigned bad=0;bad<5;++bad){
            s_orchid_ownership_installed=false;DynarmicHost rejected;
            if(bad==0)rejected.loader.image=0x03088000;
            if(bad==1)rejected.loader.goodSymbol=false;
            if(bad==2)rejected.loader.readOnly=false;
            if(bad==3)rejected.words[rejected.loader.base+0x01F58DF4]^=1;
            if(bad==4)rejected.words[rejected.loader.base+0x01F58DF8]^=1;
            assert(!InstallBl2OrchidOwnership(rejected)&&rejected.manager.names.empty()&&rejected.writes==0);
        }
        assert(!VerifyBl2LocaleGuest(nullptr,bytes.size())&&!s_guest_valid);
        assert(!VerifyBl2LocaleGuest(bytes.data(),bytes.size()-1)&&!s_guest_valid);
        assert(!VerifyBl2LocaleGuest(bytes.data(),bytes.size()+1)&&!s_guest_valid);
    }
    bytes[1024]^=1;
    for(bool payload:{false,true}){
        Reset();s_payload_valid=payload;
        const ObbSha256::MappingScope mapping(bytes.data(),bytes.size());
        assert(!VerifyBl2LocaleGuest(bytes.data(),bytes.size())&&!s_guest_valid);
        DynarmicHost host;assert(!InstallBl2OrchidOwnership(host)&&host.manager.names.empty()&&host.writes==0);
    }
    assert(!s_orchid_admitted&&!s_orchid_published&&!s_sage_admitted&&!s_sage_published&&!s_iris_admitted&&!s_iris_published);
    std::puts("PASS actual locale verifier+ownership: legacy English failure reproduced; fixed English/no-DLC and Russian identity; exact single registration/order; no entitlement; wrong size/null/SHA/image/symbol/preimages rejected");
}
'''
cpp=OUT/"actual_locale_identity_check.cpp";cpp.write_text(prefix+owns+"\n"+verify+"\n"+legacy+"\n"+ownership+"\n"+suffix,encoding="utf-8")
engine=ROOT/"work/build/native-arm32-app/lib/armeabi-v7a/libUE3Runtime.so"
assert hashlib.sha256(engine.read_bytes()).hexdigest().upper()==guest_sha
vcvars=r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
checks=[]
for backend in ("bridge","native"):
    exe=OUT/f"actual_locale_identity_{backend}.exe"
    defines="/DBL2_NATIVE_ARM32=1" if backend=="native" else ""
    command=f'call "{vcvars}" >nul && cl /nologo /std:c++20 /EHsc /O2 /UNDEBUG /DBL2_ENABLE_QA_CONSOLE=0 /DBL2_ENABLE_DLC_CONTENT=0 {defines} "{cpp}" /Fe:"{exe}" /Fo:"{OUT / (backend+".obj")}"'
    batch=OUT/f"compile_{backend}.cmd";batch.write_text("@echo off\n"+command+"\n",encoding="utf-8")
    compiled=subprocess.run(["cmd.exe","/d","/s","/c",str(batch)],cwd=ROOT,capture_output=True,text=True)
    assert compiled.returncode==0,compiled.stdout+compiled.stderr
    run=subprocess.run([str(exe),str(engine)],capture_output=True,text=True,timeout=45)
    checks.append(dict(backend=backend,returncode=run.returncode,stdout=run.stdout,compiler_log=compiled.stdout+compiled.stderr))
    assert run.returncode==0 and "PASS" in run.stdout,checks[-1]
ndk=Path.home()/"AppData/Local/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
tus=[]
for backend,target in (("native","armv7-none-linux-androideabi24"),("bridge","aarch64-none-linux-android24")):
    flags=[str(ndk),"--target="+target,"-std=c++20","-O2","-DNDEBUG","-fPIC","-DBL2_ENABLE_QA_CONSOLE=0","-DBL2_ENABLE_DLC_CONTENT=0","-DHORN_ENABLE_LOGGING=0","-DHORN_ENABLE_PERF_PROBE=0"]
    if backend=="native":flags += ["-DBL2_NATIVE_ARM32=1","-DBL2_NATIVE_PERF_PROBE=0","-D_FILE_OFFSET_BITS=64","-fno-delete-null-pointer-checks","-mfpu=neon"]
    for path in (["arm32_compat/include","arm32_compat/src"] if backend=="native" else ["arm64_bridge/third_party/_deps/dynarmic-src/src","arm64_bridge/third_party/boost"]):flags.append("-I"+str(ROOT/path))
    for path in ("arm64_bridge/src","touch_native_design/arm64_bridge"):flags.append("-I"+str(ROOT/path))
    command=flags+["-c",str(source),"-o",str(OUT/f"locale-{backend}.o")]
    compiled=subprocess.run(command,capture_output=True,text=True)
    tus.append(dict(backend=backend,command=command,returncode=compiled.returncode,log=compiled.stdout+compiled.stderr))
    assert compiled.returncode==0,tus[-1]
result=dict(status="LANGUAGE_INDEPENDENT_A0_TRUST_GATE_EN_RU_INVALID_GUARDS_AND_BOTH_NDK_TUS_PASS",source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),header_sha256=hashlib.sha256((ROOT/"arm64_bridge/src/bl2_locale.h").read_bytes()).hexdigest(),checks=checks,ndk_tus=tus,unchanged_ownership_installer_sha256=hashlib.sha256(ownership.encode()).hexdigest(),unchanged_ownsapp_predicate_sha256=hashlib.sha256(owns.encode()).hexdigest(),overlay_without_payload_still_noop=True,dlc_content_build_gate_unchanged=True,ownership_registration_unchanged=True,limitations="Actual verifier and unchanged ownership installer bodies run on real pinned A0 bytes with modeled loader/thunk leaves. No device/runtime acceptance claim.")
(OUT/"RESULT.json").write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print(result["status"])
for check in checks:print(check["backend"],check["stdout"].strip())
