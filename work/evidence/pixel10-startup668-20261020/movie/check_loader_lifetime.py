"""Extract the actual loader control flow; inject a failure at every called stage."""
from pathlib import Path
import difflib,hashlib,json,subprocess
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
actual=(ROOT/'arm64_bridge/src/jni_bridge.cpp').read_text()
block='''    if (!InstallBl2MovieSkip(*g_host)) {
        LOGE("Movie skip unavailable: unexpected FFullScreenMovieBink code");
    }
'''
fixed='    PinHostForProcessLifetime();\n    // UI movie callbacks must never retain a host that a later load check resets.\n'+block
assert actual.count(fixed)==1
legacy_anchor='    if (!InstallBl2StartupScreens(*g_host)) {'
assert actual.count(legacy_anchor)==1
source=actual.replace(fixed,'    PinHostForProcessLifetime();\n').replace(legacy_anchor,block+legacy_anchor)
assert source.count(block)==1
proposed=source.replace(block,'').replace('    PinHostForProcessLifetime();\n','    PinHostForProcessLifetime();\n    // UI movie callbacks must never retain a host that a later load check resets.\n'+block)
assert actual==proposed,'Current production source differs from tested proposal'
(HERE/'PROPOSED_MOVIE_LIFETIME.patch').write_text(''.join(difflib.unified_diff(source.splitlines(True),proposed.splitlines(True),fromfile='a/arm64_bridge/src/jni_bridge.cpp',tofile='b/arm64_bridge/src/jni_bridge.cpp')))
prefix=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
using jint=int;
using jobject=void*;
struct JNIEnv { bool ExceptionCheck(); void ExceptionDescribe(){} void ExceptionClear(){} };
static std::string failStage;
static std::vector<std::string> trace;
static std::set<unsigned> alive;
static unsigned nextId=0,publishedId=0;
static bool Step(const char* name) {trace.push_back(name);return failStage!=name;}
struct DynarmicHost {
 unsigned id=++nextId;
 DynarmicHost(){alive.insert(id);}
 ~DynarmicHost(){alive.erase(id);}
 bool Initialize(){return Step("Initialize");}
 bool LoadGameFromMemory(const uint8_t*,size_t){return Step("LoadGameFromMemory");}
 bool EnableLargeBlockRegion(){return Step("EnableLargeBlockRegion");}
 bool InstallJitWarmupTable(std::vector<uint8_t>){return Step("InstallJitWarmupTable");}
 void LoadLearnedJitProfile(const std::string&){}
 uint32_t RunFunction(uint32_t,std::vector<uint32_t>){return Step("guest_JNI_OnLoad")?0x10006:0;}
};
static std::unique_ptr<DynarmicHost> g_host;
static std::recursive_mutex g_load_mutex;
enum class GuestLoadState {Cold,Loading,Ready,Fatal};
static GuestLoadState g_guest_load_state=GuestLoadState::Cold;
static std::string g_saved_apk_path="test.apk",g_saved_files_dir="files";
static std::vector<std::string> g_saved_split_apk_paths={"main.apk","patch.apk"};
static bool g_split_source_dirs_captured=true;
static void* g_jvm=nullptr;
static constexpr int kBl2PerformanceDefaults=0;
namespace cfg {static const char* kGuestLib="guest.so";static const char* kGuestJniOnLoad="JNI_OnLoad";}
struct GuestMmapView {const uint8_t* data=nullptr;size_t size=0;};
namespace ObbSha256 {struct MappingScope {MappingScope(const uint8_t*,size_t){}};}
#define LOGE(...) ((void)0)
#define LOGI(...) ((void)0)
template<class... T> void CaptureHostPaths(T&&...){}
int IndexApk(const char*){return Step("IndexApk")?0:-1;}
int IndexObbSplitApks(const char* const*,size_t){return Step("IndexObbSplitApks")?0:-2;}
bool MmapGuestFromApk(const std::string&,std::vector<std::string>,GuestMmapView&){return Step("MmapGuestFromApk");}
std::vector<std::string> ApkCandidates(const char*){return {};}
void CloseGuestMmap(GuestMmapView&){}
void SeedBl2LearnedJitProfile(const std::string&){}
std::vector<uint8_t> LoadBl2JitWarmupTable(){return {};}
uint32_t Sym(const char*){return Step("guest_symbol")?0x1234:0;}
void PinHostForProcessLifetime(){assert(g_host);Step("PinHostForProcessLifetime");}
bool IsSupportedGuestJniVersion(jint v){return v==0x10006;}
bool JNIEnv::ExceptionCheck(){return !Step("JNI_ExceptionCheck");}
struct FakeJniEnv {
 static FakeJniEnv& Instance(){static FakeJniEnv f;return f;}
 void SetJavaVM(void*){} void Init(DynarmicHost&){} void AttachHostEnv(JNIEnv*){}
 uint32_t VmTableGuestAddr(){return 0x5678;}
};
bool InstallBl2MovieSkip(DynarmicHost& host){
 if(!Step("InstallBl2MovieSkip"))return false;
 publishedId=host.id;return true;
}
namespace bl2::iris::wheeled {bool InstallCallbacks(DynarmicHost&){return Step("InstallCallbacks");}}
'''
names=['IndexBl2CacheBase','IndexBl2LocaleSplits','VerifyBl2TouchInput','VerifyBl2PauseDebugMenuInput','VerifyBl2IoFailureProbeInput','VerifyBl2LocaleGuest','VerifyBl2NativeResolutionInput','InstallBl2AstcBlockBytes','InstallBl2NativeResolutionPatch','InstallBl2FreshPersistentBufferNames','InstallBl2LocalSessionPatch','InstallBl2LanDiscoveryCompat','InstallBl2PauseDebugMenu','InstallOfflineDlcEnumerationPatch','InstallBl2LocaleOverlay','InstallBl2TouchRuntime','InstallHornReferenceAspectPatch','InstallDiagnosticGFxEntryTrace','InstallTegra4GraphicsSelectionPatch','InstallBl2GraphicsSettings','InstallBorderlandsNvsiLifecycle','InstallBl2RenderWithoutNvsi','InstallHornFastPaths','InstallBl2VertexElementFastpath','InstallBl2ConvexFastpath','InstallBl2LimiterPeakFastpath','InstallBl2ProgramLocalsFastpath','InstallBl2SetupVertexArraysFastpath','InstallBl2AudioDecodeFastpaths','InstallBl2StartupScreens','InstallBl2WeaponAmmo','InstallBl2LanPacketAdapter','InstallBl2SubtitleTiming','InstallBl2OrchidOwnership','StartBl2GuestIntegrityWatch']
prefix+='\n'.join('template<class... T> bool '+name+'(T&&...){return Step("'+name+'");}' for name in names)+'\n'
suffix=r'''
int main(int argc,char**argv){
 const bool expectDangling=argc>1;
 auto reset=[] {g_host.reset();g_guest_load_state=GuestLoadState::Cold;trace.clear();publishedId=0;};
 JNIEnv env;
 reset();assert(EnsureGameLoaded(&env,nullptr));
 const auto stages=trace;
 unsigned dangling=0,fatal=0,cases=0;
 std::set<std::string> unique(stages.begin(),stages.end());
 for(const auto& name:unique){
  if(name=="PinHostForProcessLifetime")continue;
  reset();failStage=name;
  const bool ready=EnsureGameLoaded(&env,nullptr);
  ++cases;if(!ready)++fatal;
  if(publishedId&&!alive.count(publishedId)){
   ++dangling;std::printf("DANGLING after injected %s\n",name.c_str());
  }
 }
 if(expectDangling)assert(dangling==6);
 else assert(dangling==0);
 reset();failStage.clear();assert(EnsureGameLoaded(&env,nullptr));
 assert(publishedId&&alive.count(publishedId));
 auto before=trace;assert(EnsureGameLoaded(&env,nullptr));assert(trace==before);
 std::printf("PASS actual EnsureGameLoaded %u injected stages, %u fatal exits, dangling=%u, Ready reentry stable\n",cases,fatal,dangling);
}
'''
def body(text):return text[text.index('static bool EnsureGameLoaded('):text.index('// Guest JNIEnv* argument for a Java_* entry point.')]
for variant,text in [('before',source),('proposed',proposed)]:
 (HERE/f'loader_{variant}.cpp').write_text(prefix+body(text)+suffix)
cmd='@echo off\ncall "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
for variant in ['before','proposed']:
 cmd+=f'cl /nologo /std:c++20 /EHsc /O2 "{HERE}/loader_{variant}.cpp" /Fo:"{HERE}/loader_{variant}.obj" /Fe:"{HERE}/loader_{variant}.exe"\nif errorlevel 1 exit /b %errorlevel%\n'
cmd+=f'"{HERE}/loader_before.exe" --expect-dangling\nif errorlevel 1 exit /b %errorlevel%\n"{HERE}/loader_proposed.exe"\nexit /b %errorlevel%\n'
(HERE/'build_loader.cmd').write_text(cmd)
r=subprocess.run(['cmd','/c',str(HERE/'build_loader.cmd')],capture_output=True,text=True)
(HERE/'LOADER_HOST.log').write_text(r.stdout+r.stderr)
print(r.stdout+r.stderr)
assert r.returncode==0,r.returncode
json.dump({'status':'ACTUAL_LOADER_ALL_STAGE_FAILURES_REPRODUCE_SIX_DANGLING_BEFORE_AND_ZERO_AFTER_FIX','production_source_literal_exact_to_tested_proposal':True,'source_after_sha256':hashlib.sha256((ROOT/'arm64_bridge/src/jni_bridge.cpp').read_bytes()).hexdigest(),'legacy_control_flow_sha256':hashlib.sha256(source.encode()).hexdigest(),'all_other_loader_control_flow_exact':True,'log':r.stdout+r.stderr},(HERE/'LOADER_HOST_CHECKS.json').open('w'),indent=2)
