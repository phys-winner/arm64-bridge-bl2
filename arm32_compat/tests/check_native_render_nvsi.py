"""Execute the production A32 guard and the pinned original Render offline.

Requires the already installed Unicorn and NDK27; no device or downloads.
All compiler outputs and the receipt go to --output outside the project.
"""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import *

ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE, STACK, RETURN = 0x40000000, 0x08000000, 0x10000000
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12]


class Elf:
    def __init__(self, path):
        self.data = pathlib.Path(path).read_bytes()
        e = struct.unpack_from('<16sHHIIIIIHHHHHH', self.data)
        assert e[0][:7] == b'\x7fELF\x01\x01\x01' and e[2] == 40
        self.entry = e[4]
        self.ph = [struct.unpack_from('<8I', self.data, e[5]+i*e[9])
                   for i in range(e[10])]
        sh = [struct.unpack_from('<10I', self.data, e[6]+i*e[11])
              for i in range(e[12])]
        self.symbols, self.rel = {}, []
        for index, s in enumerate(sh):
            if s[1] not in (2, 11):
                continue
            strings = sh[s[6]]
            names = self.data[strings[4]:strings[4]+strings[5]]
            table = []
            for i in range(s[5]//s[9]):
                n, a, z, _, _, _ = struct.unpack_from('<IIIBBH', self.data, s[4]+i*s[9])
                name = names[n:names.index(0, n)].decode()
                table.append((name, a, z))
                self.symbols[name] = (a, z)
            for r in sh:
                if r[1] == 9 and r[6] == index:
                    for i in range(r[5]//r[9]):
                        a, info = struct.unpack_from('<II', self.data, r[4]+i*r[9])
                        self.rel.append((a, info & 255, table[info >> 8]))

    def bytes(self, address, size):
        for t, o, v, _, f, _, _, _ in self.ph:
            if t == 1 and v <= address and address+size <= v+f:
                return self.data[o+address-v:o+address-v+size]
        raise ValueError(hex(address))

    def word(self, address):
        return struct.unpack('<I', self.bytes(address, 4))[0]


def write(uc, address, value):
    uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def read(uc, address):
    return struct.unpack('<I', uc.mem_read(address, 4))[0]


def machine(elf, start, size, guards, bindings):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(BASE, 0x03089000)
    uc.mem_map(STACK, 0x100000)
    uc.mem_map(RETURN, 0x1000)
    uc.mem_write(BASE+start, elf.bytes(start, size))
    if not hasattr(elf, 'mapped_relocations'):
        first = min(a for a, _, _ in elf.rel)
        last = max(a for a, _, _ in elf.rel)+4
        data = bytearray(last-first)
        for a, kind, (_, value, _) in elf.rel:
            if kind == 23:
                struct.pack_into('<I', data, a-first, BASE+elf.word(a))
            elif kind in (21, 22):
                struct.pack_into('<I', data, a-first, BASE+value if value else RETURN+0x100)
        elf.mapped_relocations = first, bytes(data)
    first, data = elf.mapped_relocations
    uc.mem_write(BASE+first, data)
    for a, target, _ in bindings:
        assert read(uc, BASE+a) == BASE+target
    for a, value in guards:
        assert read(uc, BASE+a) == value
    uc.reg_write(UC_ARM_REG_CPSR, 0x10)
    uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf00000)
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(UC_ARM_REG_FPSCR, 0)
    return uc


def production_guard(elf, header, output, clang, start, size, guards, bindings, patches):
    # Compile and execute the exact template used by native_bootstrap.cpp.
    wrapper = output/'guard.cpp'
    wrapper.write_text('#include "native_render_nvsi.h"\n'
        'extern "C" unsigned Check(unsigned base,unsigned drop) {\n'
        ' unsigned writes=0,flushes=0,bad=0;\n'
        ' bool ok=bl2::native_render::Apply(base,\n'
        ' [](unsigned a){return *reinterpret_cast<const unsigned*>(a);},\n'
        ' [&](unsigned a,unsigned v){unsigned i=writes++;if(!(drop&(1u<<i)))'
        '*reinterpret_cast<unsigned*>(a)=v;},\n'
        ' [&](unsigned a,std::size_t n){++flushes;bad=(a!=base+'
        'bl2::native_render::kFunctionRva || n!=bl2::native_render::kFunctionBytes);});\n'
        ' return unsigned(ok)|(writes<<8)|(flushes<<16)|(bad<<31);\n}\n', encoding='utf-8')
    binary = output/'guard-arm.elf'
    command = [str(clang), '--target=armv7-none-linux-androideabi24', '-std=c++20',
        '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions', '-fno-rtti',
        '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
        '-nostdlib', '-no-pie', '-I'+str(header.parent), str(wrapper), '-o', str(binary),
        '-Wl,-Ttext=0x01000000', '-Wl,-e,Check', '-Wl,--build-id=none']
    process = subprocess.run(command, capture_output=True, text=True)
    assert process.returncode == 0, process.stdout+process.stderr
    code = Elf(binary)
    uc = machine(elf, start, size, guards, bindings)
    mapped = set()
    for t, off, address, _, f, n, _, _ in code.ph:
        if t != 1:
            continue
        for page in range(address & ~4095, (address+n+4095) & ~4095, 4096):
            if page not in mapped:
                uc.mem_map(page, 4096)
                mapped.add(page)
        uc.mem_write(address, code.data[off:off+f])

    def run(base=BASE, drop=0):
        uc.reg_write(UC_ARM_REG_SP, STACK+0xf0000)
        uc.reg_write(UC_ARM_REG_LR, RETURN)
        uc.reg_write(UC_ARM_REG_R0, base)
        uc.reg_write(UC_ARM_REG_R1, drop)
        uc.emu_start(code.entry, RETURN, count=20000)
        return uc.reg_read(UC_ARM_REG_R0)

    original = elf.bytes(start, size)

    def reset():
        uc.mem_write(BASE+start, original)
        for a, target, _ in bindings:
            write(uc, BASE+a, BASE+target)
        uc.ctl_remove_cache(BASE+start, BASE+start+size)

    assert run() == 0x00010301
    expected = bytearray(original)
    for a, value in patches:
        struct.pack_into('<I', expected, a-start, value)
    assert bytes(uc.mem_read(BASE+start, size)) == expected
    negative = 0
    for a, value in guards:
        reset()
        write(uc, BASE+a, value ^ 1)
        before = bytes(uc.mem_read(BASE+start, size))
        assert run() == 0 and bytes(uc.mem_read(BASE+start, size)) == before
        negative += 1
    for a, target, _ in bindings:
        reset()
        write(uc, BASE+a, (BASE+target) ^ 1)
        before = bytes(uc.mem_read(BASE+start, size))
        assert run() == 0 and bytes(uc.mem_read(BASE+start, size)) == before
        negative += 1
    reset()
    assert run(0xffffffff) == 0
    for drop in (1, 2, 4):
        reset()
        assert run(drop=drop) == 0x00010300  # Partial write is never accepted.
    return {'compile_command': command, 'word_GOT_mismatch_no_writes': negative,
            'base_overflow_no_writes': 1, 'dropped_write_rejected': 3,
            'success_writes': 3, 'success_flushes': 1}


def render(elf, start, size, guards, bindings, patches, scenario, status, poisoned):
    uc = machine(elf, start, size, guards, bindings)
    for a, value in patches:
        write(uc, BASE+a, value)
    env, loop, engine, world = STACK+0x1000, STACK+0x2000, STACK+0x3000, STACK+0x4000
    client, player, viewport, vtable = STACK+0x5000, STACK+0x6000, STACK+0x7000, STACK+0x8000
    output, log_vtable = STACK+0x9000, STACK+0xa000
    state, java, loop_flags = BASE+0x03059588, BASE+0x03057590, BASE+0x02F63D20
    quit_global = BASE+0x02F911E4
    write(uc, BASE+0x02F3A200, output)
    write(uc, output, log_vtable)
    write(uc, log_vtable+0x2c, RETURN+0x104)
    write(uc, BASE+0x03008D20, engine if scenario.get('engine') else 0)
    write(uc, engine, vtable)
    write(uc, vtable+0x168, RETURN+0x108)
    write(uc, engine+0x48c, client)
    write(uc, client, player)
    write(uc, player+0x40, viewport)
    write(uc, viewport, vtable)
    write(uc, vtable+0x148, RETURN+0x10c)
    write(uc, vtable+0x16c, RETURN+0x110)
    write(uc, BASE+0x0300D940, 0 if scenario.get('no_world') else world)
    write(uc, world+0xd8, client)
    write(uc, client+0x50, viewport)
    uc.mem_write(engine+0x484, struct.pack('<f', 0 if scenario.get('zero_gc_limit') else .05))
    uc.mem_write(BASE+0x02F3A108, struct.pack('<d', .1))
    write(uc, loop_flags+8, 1)
    write(uc, state-0xc70, scenario.get('state', 0))
    write(uc, state-0xcc0, scenario.get('ready', 1))
    write(uc, java+0x28, env)
    write(uc, quit_global, int(scenario.get('early_global', False)))
    write(uc, java+0x20, int(scenario.get('early_java', False)))
    initial_regs = [0x11220000+i*0x111 for i in range(13)]
    for reg, value in zip(REGS, initial_regs):
        uc.reg_write(reg, value)
    uc.reg_write(UC_ARM_REG_SP, STACK+0xf0000)
    uc.reg_write(UC_ARM_REG_LR, RETURN)
    vfp = [0x2233445566770000+i for i in range(8)]
    for i, value in enumerate(vfp):
        uc.reg_write(UC_ARM_REG_D8+i, value)
    events, nvsi, ticks, sleeps, instructions = [], [], 0, 0, 0
    calls = {
        0x01E253AC: ('jni', 0), 0x01E253C0: ('timing', 0),
        0x01E253F0: ('clock', 2), 0x01E25434: ('allocate', 1),
        0x01E25458: ('preinit', 2), 0x01E25474: ('log_flush', 1),
        0x01E25484: ('init', 1), 0x01E25490: ('input_map', 2),
        0x01E2549C: ('clock', 2), 0x01E254A8: ('clock', 2),
        0x01E2555C: ('engine_ready', 1), 0x01E25584: ('wait_tick', 1),
        0x01E255C0: ('sleep', 1), 0x01E255D8: ('sleep', 1),
        0x01E25610: ('viewport_gc', 1), 0x01E25628: ('begun_play', 1),
        0x01E25660: ('purge_pending', 0), 0x01E256A4: ('gc_full', 1),
        0x01E256B4: ('context_restore', 0), 0x01E256C4: ('tick', 1),
        0x01E256CC: ('nvsi_lock', 0), 0x01E256D4: ('nvsi_status', 0),
        0x01E25718: ('nvsi_error_log', 1), 0x01E2571C: ('nvsi_fatal', 0),
        0x01E25724: ('nvsi_unlock', 0), 0x01E25788: ('exit_log', 3),
        0x01E257C8: ('gc_incremental', 2), 0x01E258C0: ('string', 2),
        0x01E258D8: ('viewport_string', 4), 0x01E258E8: ('free', 1),
        0x01E258F8: ('free', 1), 0x01E25910: ('nvsi_exit', 1),
    }

    def hook(current, pc, length, unused):
        nonlocal ticks, sleeps, instructions
        instructions += 1
        rva = pc-BASE
        opcode = read(current, pc)
        is_call = opcode & 0x0f000000 == 0x0b000000 or opcode & 0x0ffffff0 == 0x012fff30
        if not is_call:
            return
        assert rva in calls, ('unreviewed callsite', hex(rva))
        name, argc = calls[rva]
        args = [current.reg_read(r) for r in REGS[:argc]]
        (nvsi if name.startswith('nvsi_') else events).append((name, args))
        old_r0 = current.reg_read(UC_ARM_REG_R0)
        if poisoned:
            for i, r in enumerate(REGS[:4]+[UC_ARM_REG_R12]):
                current.reg_write(r, 0xbadc0000+i)
            current.reg_write(UC_ARM_REG_CPSR, 0x90000010)
        result = old_r0
        if name == 'jni':
            result = env
        elif name == 'allocate':
            result = loop
        elif name == 'clock':
            current.mem_write(args[1], struct.pack('<II', 1, 2000000))
            result = 0
        elif name == 'engine_ready':
            result = int(scenario.get('engine_ready', False))
        elif name in ('tick', 'wait_tick'):
            ticks += 1
            write(current, state-0xc70, 0)
            if ticks >= scenario.get('ticks', 2):
                write(current, java+0x20 if scenario.get('java_exit') else quit_global, 1)
            result = 0xfeed0042  # Prove omitted status inputs are dead.
        elif name == 'sleep':
            sleeps += 1
            if sleeps >= 3:
                write(current, state-0xcc0, 1)
            result = 0
        elif name == 'begun_play':
            result = int(not scenario.get('not_begun'))
        elif name == 'purge_pending':
            result = int(scenario.get('purge', False))
        elif name in ('gc_full', 'gc_incremental'):
            write(current, state-0xcc0, 1)
            result = 0
        elif name in ('string', 'viewport_string'):
            current.mem_write(args[0], struct.pack('<III', STACK+0xb000, 2, 2))
        elif name == 'nvsi_status':
            result = status
        elif name in ('nvsi_lock', 'nvsi_unlock'):
            result = 0
        elif name in ('nvsi_exit', 'nvsi_fatal'):
            write(current, quit_global, 1)
            result = 0
        current.reg_write(UC_ARM_REG_R0, result)
        current.reg_write(UC_ARM_REG_LR, pc+4)
        current.reg_write(UC_ARM_REG_PC, pc+4)

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(BASE+start, RETURN, count=10000)
    assert uc.reg_read(UC_ARM_REG_PC) == RETURN, ('did not return', scenario)
    assert uc.reg_read(UC_ARM_REG_R0) == 0
    assert uc.reg_read(UC_ARM_REG_SP) == STACK+0xf0000
    assert [uc.reg_read(r) for r in REGS[4:12]] == initial_regs[4:12]
    assert [uc.reg_read(UC_ARM_REG_D8+i) for i in range(8)] == vfp
    globals_out = [read(uc, a) for a in (quit_global, java+0x20, state-0xc74,
        state-0xc70, state-0xcc0, loop_flags+8, loop_flags+0xc)]
    return {'events': events, 'nvsi': nvsi, 'ticks': ticks,
            'globals': globals_out, 'instructions': instructions,
            'apsr': uc.reg_read(UC_ARM_REG_CPSR) & 0xf80f0000,
            'fpscr': uc.reg_read(UC_ARM_REG_FPSCR)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=pathlib.Path, default=ROOT/'../app/assets/guest_libs/libUnrealEngine3.so')
    parser.add_argument('--clang', type=pathlib.Path, default=pathlib.Path.home()/'AppData/Local/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe')
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    repo = ROOT.parent.resolve()
    output = (args.output or pathlib.Path(tempfile.mkdtemp(prefix='bl2-render-nvsi-control-'))).resolve()
    assert repo != output and repo not in output.parents
    output.mkdir(parents=True, exist_ok=True)
    header = ROOT/'src/native_render_nvsi.h'
    text = header.read_text(encoding='utf-8')
    elf = Elf(args.elf)
    sha = hashlib.sha256(elf.data).hexdigest().upper()
    assert sha == re.search(r'"([A-F0-9]{64})"', text)[1]
    start, size = elf.symbols['_Z21NativeCallback_RenderPv']
    assert (start, size) == (0x01e25378, 0x5b0)
    guards = [(int(a, 16), int(v, 16)) for a, v in re.findall(r'\{(0x\w+),(0x\w+)\}', text.split('kGuards[]{')[1].split('};')[0])]
    bindings = [(int(a, 16), int(v, 16), name) for a, v, name in re.findall(r'\{(0x\w+),(0x\w+),"(\w+)"\}', text.split('kBindings[]{')[1].split('};')[0])]
    patches = [(int(a, 16), int(v, 16)) for a, v in re.findall(r'\{(0x\w+),(0x\w+)\}', text.split('kPatches[]{')[1].split('};')[0])]
    assert len(guards) == 43 and len(bindings) == len(patches) == 3
    assert all(elf.word(a) == value for a, value in guards)
    assert all(elf.word(a) == target and elf.symbols[name][0] == target for a, target, name in bindings)
    guard_result = production_guard(elf, header, output, args.clang, start, size, guards, bindings, patches)
    scenarios = [dict(name='early_global', early_global=True), dict(name='early_java', early_java=True),
        dict(name='normal_global'), dict(name='normal_java', java_exit=True),
        dict(name='native_wait', state=1), dict(name='sleep_wait', state=1, ready=0),
        dict(name='gc_full', state=1, ready=2, engine=True),
        dict(name='gc_incremental', state=1, ready=2, engine=True, purge=True),
        dict(name='gc_zero_limit', state=1, ready=2, engine=True, zero_gc_limit=True),
        dict(name='gc_not_begun', state=1, ready=2, engine=True, not_begun=True),
        dict(name='gc_no_world', state=1, ready=2, engine=True, no_world=True),
        dict(name='viewport_setup', state=1, engine=True, engine_ready=True)]
    results = []
    observed_callbacks = set()
    for scenario in scenarios:
        for status in (0, 1, 4, 15):
            for poisoned in (False, True):
                original = render(elf, start, size, guards, bindings, [], scenario, status, poisoned)
                changed = render(elf, start, size, guards, bindings, patches, scenario, status, poisoned)
                assert changed['nvsi'] == []
                assert original['events'] == changed['events'], (scenario, status, poisoned, original, changed)
                assert original['globals'] == changed['globals'] and original['ticks'] == changed['ticks']
                assert original['apsr'] == changed['apsr'] and original['fpscr'] == changed['fpscr']
                if not scenario.get('early_global') and not scenario.get('early_java'):
                    assert [n for n, _ in original['nvsi']].count('nvsi_status') > 0
                trace = [name for name, _ in changed['events']]
                observed_callbacks.update(trace)
                results.append({'scenario': scenario['name'], 'status': status, 'poisoned': poisoned,
                    'ticks': changed['ticks'], 'original_nvsi_calls': len(original['nvsi']),
                    'patched_direct_nvsi': 0, 'non_NVSI_callback_trace': trace})
    assert {'jni', 'timing', 'clock', 'allocate', 'preinit', 'log_flush', 'init',
            'input_map', 'engine_ready', 'wait_tick', 'sleep', 'viewport_gc',
            'begun_play', 'purge_pending', 'gc_full', 'context_restore', 'tick',
            'exit_log', 'gc_incremental', 'string', 'viewport_string', 'free'} == observed_callbacks
    for status in (2, 8, 17):
        original = render(elf, start, size, guards, bindings, [], dict(ticks=1), status, True)
        changed = render(elf, start, size, guards, bindings, patches, dict(ticks=1), status, True)
        assert changed['nvsi'] == []
        assert any(n == ('nvsi_exit' if status == 17 else 'nvsi_fatal') for n, _ in original['nvsi'])
    proof = {'result': 'PASS', 'elf_sha256': sha, 'render_rva': start, 'render_bytes': size,
        'render_sha256': hashlib.sha256(elf.bytes(start, size)).hexdigest().upper(),
        'production_guard': guard_result, 'render_controls': results,
        'non_NVSI_callback_coverage': sorted(observed_callbacks), 'NVSI_error_bypass_controls': [2, 8, 17],
        'patches': patches, 'claims': ['Only three Render words changed', 'All patched direct Render NVSI calls zero',
        'Original non-NVSI callbacks, both real quit paths, normal return, callee-saved regs/VFP and loop globals retained',
        'Original Tick remains; transitively called Ammo and other NVSI callers unchanged'],
        'limits': ['External engine/GLES/JNI/mutex calls intercepted; no device performance measurement',
        'Flush callback shape checked; actual host cache invalidation uses the existing tested native implementation'],
        'optimization_trials': 0, 'consecutive_no_gain': 0}
    (output/'result.json').write_text(json.dumps(proof, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'result': 'PASS', 'render_controls': len(results), 'production_guard_negative': 50,
        'receipt': str(output/'result.json'), 'sha256': hashlib.sha256((output/'result.json').read_bytes()).hexdigest().upper()}))


if __name__ == '__main__':
    main()
