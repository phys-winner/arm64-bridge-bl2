from pathlib import Path
import hashlib
import json
import subprocess

ROOT = next(parent for parent in Path(__file__).resolve().parents
            if (parent / 'arm32_compat/src/native_vfs.cpp').is_file())
OUT = Path(__file__).resolve().parent
OUT.mkdir(parents=True, exist_ok=True)
VFS = ROOT / 'arm32_compat/src/native_vfs.cpp'
COMMON = ROOT / 'arm64_bridge/src'


def block(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = VFS.read_text(encoding='utf-8')
adapter = '\n'.join(block(source, signature) for signature in (
    'struct DirectoryState {', 'DIR* NativeOpendir(', 'struct dirent* NativeReaddir('))
# The extracted declaration requires its terminating semicolon.
adapter = adapter.replace('}\nDIR* NativeOpendir(', '};\nDIR* NativeOpendir(', 1)
closedir = source[source.index('    manager.RegisterThunk("closedir"'):source.index('    manager.RegisterThunk("chdir"')]
assert 's_directories.erase(dir)' in closedir and '::closedir(dir)' in closedir
assert 'Bl2GetDirectorySnapshot' in adapter
assert 'bl2_directory_cache.cpp' in (ROOT / 'arm32_compat/CMakeLists.txt').read_text()

stubs = OUT / 'stubs'
for name in ('sys', 'android'):
    (stubs / name).mkdir(parents=True, exist_ok=True)
for name in ('dirent.h', 'sys/stat.h', 'sys/system_properties.h', 'unistd.h'):
    (stubs / name).write_text('#pragma once\n#include "fake_posix.h"\n', encoding='utf-8')
(stubs / 'fake_posix.h').write_text(r'''
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#define DT_DIR 4
#define DT_REG 8
#define S_ISDIR(mode) (((mode) & 0170000) == 0040000)
#define PROP_VALUE_MAX 92
struct TestTime { int64_t tv_sec = 1, tv_nsec = 0; };
struct stat {
    uint64_t st_dev = 1, st_ino = 1;
    uint32_t st_mode = 0040755, st_nlink = 2, st_uid = 1000, st_gid = 1000;
    int64_t st_size = 4096;
    TestTime st_mtim{}, st_ctim{};
};
struct dirent { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; };
static_assert(offsetof(dirent, d_name) == 19);
struct FakeNode {
    struct stat stamp{};
    std::vector<std::string> names;
    int read_error = 0;
    bool mutate_on_read = false;
    unsigned opens = 0, reads = 0, closes = 0;
};
struct DIR { std::string path; size_t next = 0; dirent entry{}; int fd = 0; };
inline std::unordered_map<std::string, FakeNode> fs;
inline std::unordered_map<int, DIR*> fds;
inline std::string cwd = "/storage/emulated/0/Borderlands2";
inline int mode = 1, next_fd = 100;
inline bool fail_appfiles = false;
inline std::string Resolve(const char* path) { return path && std::strcmp(path, ".") == 0 ? cwd : path ? path : ""; }
inline int stat(const char* path, struct stat* out) {
    auto it = fs.find(Resolve(path));
    if (it == fs.end()) { errno = ENOENT; return -1; }
    *out = it->second.stamp; return 0;
}
inline DIR* opendir(const char* path) {
    const auto key = Resolve(path);
    auto it = fs.find(key);
    if (it == fs.end() || (fail_appfiles && key == "/app/files")) { errno = EACCES; return nullptr; }
    ++it->second.opens;
    auto* dir = new DIR{key}; dir->fd = next_fd++; fds[dir->fd] = dir; return dir;
}
inline dirent* readdir(DIR* dir) {
    auto& node = fs.at(dir->path); ++node.reads;
    if (node.read_error) { errno = node.read_error; return nullptr; }
    if (node.mutate_on_read) { ++node.stamp.st_mtim.tv_nsec; node.mutate_on_read = false; }
    if (dir->next == node.names.size()) return nullptr;
    const auto& name = node.names[dir->next++];
    dir->entry = {}; dir->entry.d_ino = 0x100000000ULL + dir->next;
    dir->entry.d_off = 0x200000000LL + dir->next; dir->entry.d_type = DT_REG;
    std::strncpy(dir->entry.d_name, name.c_str(), 255);
    dir->entry.d_reclen = static_cast<uint16_t>(19 + std::strlen(dir->entry.d_name) + 1);
    return &dir->entry;
}
inline int closedir(DIR* dir) { ++fs.at(dir->path).closes; fds.erase(dir->fd); delete dir; return 0; }
inline int dirfd(DIR* dir) { return dir->fd; }
inline int fstat(int fd, struct stat* out) { return stat(fds.at(fd)->path.c_str(), out); }
inline char* getcwd(char* out, size_t size) { if (cwd.size() >= size) { errno = ERANGE; return nullptr; } std::memcpy(out, cwd.c_str(), cwd.size() + 1); return out; }
inline int __system_property_get(const char*, char* value) { value[0] = mode == 2 ? '2' : '1'; value[1] = 0; return 1; }
''', encoding='utf-8')

prefix = r'''
#include "fake_posix.h"
#include "bl2_directory_cache.h"
namespace bl2_orchid { enum class NodeKind { None, File, Directory }; struct Child { std::string name; NodeKind kind; }; }
std::vector<bl2_orchid::Child> orchid{{"Content", bl2_orchid::NodeKind::Directory}};
bl2_orchid::NodeKind Bl2OrchidKind(const char* path) { return std::strcmp(path,"/orchid/file") == 0 ? bl2_orchid::NodeKind::File : bl2_orchid::NodeKind::None; }
const std::vector<bl2_orchid::Child>* Bl2OrchidChildren(const char* path) { return std::strcmp(path,"/orchid") == 0 ? &orchid : nullptr; }
const std::string root = "/storage/emulated/0/Borderlands2";
const std::string assetroot = root + "/WillowGame/CookedAndroid_DXT";
const std::string obbroot = "/storage/emulated/0/Android/obb/com.tt2kgames.Borderlands2";
const std::string_view kMainObbName = "main.31.com.tt2kgames.Borderlands2.obb";
const std::string_view kPatchObbName = "patch.31.com.tt2kgames.Borderlands2.obb";
bool IsObbDirectory(const char* path) { return path == obbroot; }
int ObbIndex(const char* path) { return path == kMainObbName ? 0 : path == kPatchObbName ? 1 : -1; }
bool IsManagedBl2ZipObbName(const char* path) { return std::string_view(path).ends_with(".managed.obb"); }
std::string s_files_root = "/app/files";
std::mutex s_directories_mutex;
'''
adapter = adapter.replace('DIR* NativeOpendir(', 'std::unordered_map<DIR*, DirectoryState> s_directories;\nDIR* NativeOpendir(', 1)
tests = r'''
int Close(DIR* dir) { { std::lock_guard lock(s_directories_mutex); s_directories.erase(dir); } return ::closedir(dir); }
void CheckRead(DIR* dir, const char* name) {
    auto* entry = NativeReaddir(dir); assert(entry && std::strcmp(entry->d_name, name) == 0);
    assert(entry->d_reclen == 19 + std::strlen(name) + 1);
}
int main(int argc, char**) {
    mode = argc > 1 ? 2 : 1;
    fs["/app/files"] = {}; fs[root] = {}; fs[assetroot] = {};
    if (mode == 2) {
        for (unsigned i=0;i<20;++i) { auto* dir=NativeOpendir(root.c_str()); assert(dir); assert(!NativeReaddir(dir)); Close(dir); }
        assert(fs[root].reads == 20 && fs[root].opens == 20 && s_directories.empty());
        std::puts("PASS uncached mode2 baseline: 20 FUSE-like EOF reads"); return 0;
    }
    errno = EBUSY;
    for (unsigned i=0;i<1000;++i) {
        auto* dir = NativeOpendir(root.c_str()); assert(dir && dir->path == "/app/files");
        assert(errno == (i ? 0 : EBUSY)); assert(!NativeReaddir(dir) && errno == 0); Close(dir);
    }
    assert(fs[root].opens == 1 && fs[root].reads == 1 && fs[root].closes == 1);
    assert(fs["/app/files"].opens == 1000 && fs["/app/files"].closes == 1000);
    fs[root].names = {"Config", std::string(255, 'x')}; ++fs[root].stamp.st_mtim.tv_nsec;
    auto* held = NativeOpendir(root.c_str()); assert(held);
    auto* first = NativeReaddir(held); assert(first && std::strcmp(first->d_name,"Config") == 0);
    assert(first->d_ino == 0x100000001ULL && first->d_off == 0x200000001LL && first->d_type == DT_REG);
    CheckRead(held, std::string(255,'x').c_str()); assert(!NativeReaddir(held) && errno == 0);
    fs[root].names = {"Changed"}; ++fs[root].stamp.st_ctim.tv_nsec;
    auto* updated = NativeOpendir(root.c_str()); CheckRead(updated,"Changed"); Close(updated);
    assert(!NativeReaddir(held)); Close(held);
    assert(fs[root].opens == 3);
    cwd = root; auto* dot = NativeOpendir("."); CheckRead(dot,"Changed"); Close(dot);
    fs[assetroot].names = {"Asset"}; cwd = assetroot;
    dot = NativeOpendir("."); CheckRead(dot,"Asset"); Close(dot);
    const auto errorpath = assetroot + "/error"; fs[errorpath].read_error = EACCES;
    for (int i=0;i<2;++i) { auto* dir=NativeOpendir(errorpath.c_str()); assert(dir && dir->path == errorpath); errno=0; assert(!NativeReaddir(dir) && errno == EACCES); Close(dir); }
    assert(fs[errorpath].opens == 4);
    const auto racing = assetroot + "/racing"; fs[racing].mutate_on_read = true;
    auto* changed = NativeOpendir(racing.c_str()); assert(changed && changed->path == racing); Close(changed);
    changed = NativeOpendir(racing.c_str()); assert(changed && changed->path == "/app/files"); Close(changed);
    const auto config = root + "/WillowGame/Config"; fs[config].names={"Save"};
    auto* excluded = NativeOpendir(config.c_str()); assert(excluded->path == config); CheckRead(excluded,"Save"); Close(excluded);
    assert(fs[config].opens == 1);
    fs[obbroot].names={std::string(kMainObbName),std::string(kPatchObbName),"extra.managed.obb","external.bin"};
    auto* obb = NativeOpendir(obbroot.c_str()); assert(obb->path == obbroot);
    CheckRead(obb,kMainObbName.data()); CheckRead(obb,kPatchObbName.data()); CheckRead(obb,"external.bin"); assert(!NativeReaddir(obb)); Close(obb);
    assert(fs[obbroot].opens == 1);
    fs.erase(obbroot); obb = NativeOpendir(obbroot.c_str()); assert(obb && obb->path == "/app/files");
    CheckRead(obb,kMainObbName.data()); CheckRead(obb,kPatchObbName.data()); assert(!NativeReaddir(obb)); Close(obb);
    auto* dlc = NativeOpendir("/orchid"); assert(dlc && dlc->path == "/app/files");
    auto* child=NativeReaddir(dlc); assert(child && std::strcmp(child->d_name,"Content") == 0 && child->d_type == DT_DIR);
    assert(!NativeReaddir(dlc) && errno == 0); Close(dlc);
    assert(!NativeOpendir("/orchid/file") && errno == ENOTDIR);
    assert(!NativeOpendir(nullptr) && errno == EFAULT);
    assert(!NativeOpendir("/missing") && errno == EACCES);
    fail_appfiles = true; assert(!NativeOpendir(root.c_str()) && errno == EACCES); fail_appfiles=false;
    assert(s_directories.empty() && fds.empty());
    std::puts("PASS actual shared helper + actual native adapter: 1000 empty scans -> one physical read; entry64/name255/reclen/EOF/errno/metadata/cwd/race/error/exclusion/OBB/orchid/ownership");
}
'''
harness = OUT / 'native_directory_cache_check.cpp'
harness.write_text(prefix + '\n' + adapter + '\n' + tests, encoding='utf-8')
cmd = OUT / 'compile.cmd'
exe = OUT / 'native_directory_cache_check.exe'
cmd.write_text(
    '@echo off\r\ncall "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\n'
    f'cl /nologo /std:c++20 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /DHORN_ENABLE_LOGGING=0 /DBL2_ENABLE_QA_CONSOLE=0 /FI"{stubs / "fake_posix.h"}" '
    f'/I"{stubs}" /I"{COMMON}" "{harness}" "{COMMON / "bl2_directory_cache.cpp"}" '
    f'/Fe:"{exe}" /Fo:"{OUT.as_posix()}/"\r\n', encoding='utf-8')
compile_result = subprocess.run(['cmd.exe', '/d', '/s', '/c', str(cmd)], capture_output=True, text=True)
assert compile_result.returncode == 0, compile_result.stdout + compile_result.stderr
results = []
for args in ([], ['mode2']):
    result = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
    results.append(result.stdout.strip())
receipt = {
    'status': 'PASS',
    'actual_source': True,
    'source_sha256': {str(p.relative_to(ROOT)).replace('\\','/'): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (VFS, ROOT/'arm32_compat/CMakeLists.txt', COMMON/'bl2_directory_cache.cpp', COMMON/'bl2_directory_cache.h')},
    'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    'harness_sha256': hashlib.sha256(harness.read_bytes()).hexdigest(),
    'results': results,
    'limitations': 'Host POSIX stubs deterministically model metadata, CWD, FUSE EOF/errors; no device/runtime acceptance and no full APK build.'
}
(OUT/'RESULT.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
print(json.dumps(receipt,indent=2))
