"""Check the two gameplay axis veneers and semantic-call preimages offline."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

ROOT=Path(__file__).resolve().parents[2]
GUEST=ROOT/'app/assets/guest_libs/libUnrealEngine3.so'
EXPECTED='A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814'
SITES={
    0x0183AA48:[0xEE071A10,0xEDDF7A18,0xEDD06A40,0xED906A45],
    0x0183ABCC:[0xE59530E0,0xE3530000,0x0A00001E,0xE593236C],
    0x01821D6C:[0xEE072A10,0xEDD17A00,0xEE777A87,0xEDC17A00],
    0x0236E210:[0xE92D4070,0xE3A03000,0xE59F4298,0xE24DD018],
    0x0235679C:[0xE92D0070,0xE5903290,0xE3530000,0x1A000002],
    0x0236D8FC:[0xE92D47F0,0xE1A04001,0xE5911004,0xE24DD018],
    0x0235734C:[0xE92D4070,0xE59056E0,0xE59F4090,0xE3550000],
    0x0229ECA0:[0xE92D4FF8,0xE5905188,0xE59F91B0,0xE3550000],
    0x010E826C:[0xE92D4FF0,0xED2D8B06,0xE1A04001,0xE3003402],
    0x010E86C8:[0xE59FC088,0xE92D41F0,0xE1A05000,0xE1A08001],
    0x01ABCA00:[0xE590006C,0xE12FFF1E,0xE92D4010,0xEDD07A26],
    0x00F26444:[0xE92D4010,0xE1A01000,0xE59F4044,0xE24DD008],
    0x0207B904:[0xE59030A4,0xE3530000,0xBA000009,0xE590205C],
    0x0233735C:[0xE5900CB4,0xE2900000,0x13A00001,0xE12FFF1E],
    0x01CC52A8:[0xE590303C,0xE3530000,0x1583105C,0xE12FFF1E],
    0x01CD8760:[0xE581A048,0xE59DC068,0xE08F8008,0xE581C050],
    0x01CDEF68:[0xE5932048,0xE3520000,0x1A00000D,0xE59F607C],
    0x01CD16B4:[0xE5841048,0xE5982010,0xE5985014,0xE0825105],
}

def native_query_checks(read):
    # Execute the actual ARM bytes. Only GMalloc's external Free is a stub;
    # this proves the original allocator receives (this, Data), not Host.Free.
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R4,
        UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
        UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_SP, UC_ARM_REG_LR)
    preserved=(UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,
        UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11)
    total=0
    for base in (0x10000000,0x40000000):
        vm=Uc(UC_ARCH_ARM,UC_MODE_ARM); pages=set(); freed=[]
        def memory(address,size):
            for page in range(address&~4095,(address+size+4095)&~4095,4096):
                if page not in pages: vm.mem_map(page,4096);pages.add(page)
        def word(address,value):
            memory(address,4);vm.mem_write(address,struct.pack('<I',value&0xFFFFFFFF))
        for rva,size in ((0x00F26444,92),(0x0207B904,64),(0x0233735C,16)):
            memory(base+rva,size);vm.mem_write(base+rva,read(rva,size))
        tree,nodes,pawn,allocator,table=0x60000000,0x60002000,0x60003000,0x60005000,0x60006000
        stop,free=0x70000000,0x70001000
        memory(tree,65536);word(stop,0xE12FFF1E);word(free,0xE12FFF1E)
        # appFree's original PC-relative GMalloc load, with initialized GMalloc.
        word(base+0x00F2645C+0x0206AD84+0x14C,allocator)
        word(allocator,table);word(table+12,free)
        def observe_free(uc,address,size,unused):
            if address==free: freed.append((uc.reg_read(UC_ARM_REG_R0),uc.reg_read(UC_ARM_REG_R1)))
        vm.hook_add(UC_HOOK_CODE,observe_free)
        def run(rva,arg):
            nonlocal total
            expected=[0xA1100000+i for i in range(len(preserved))]
            for reg,value in zip(preserved,expected): vm.reg_write(reg,value)
            vm.reg_write(UC_ARM_REG_SP,tree+65520);vm.reg_write(UC_ARM_REG_LR,stop)
            vm.reg_write(UC_ARM_REG_R0,arg);vm.emu_start(base+rva,stop,count=256)
            assert vm.reg_read(UC_ARM_REG_SP)==tree+65520
            assert [vm.reg_read(reg) for reg in preserved]==expected
            total+=1;return vm.reg_read(UC_ARM_REG_R0)
        word(tree+0x58,nodes);word(tree+0x5C,2);word(nodes,0x60009000);word(nodes+36,0x60009100)
        for index,expected in ((-1,0),(2,0),(0,0x60009000),(1,0x60009100)):
            word(tree+0xA4,index);assert run(0x0207B904,tree)==expected
        word(tree+0x5C,0);word(tree+0xA4,0);assert run(0x0207B904,tree)==0
        word(tree+0x5C,2);word(nodes,0);assert run(0x0207B904,tree)==0
        for active in (0,0x60009200):
            word(pawn+0xCB4,active);assert run(0x0233735C,pawn)==bool(active)
        for data in (0,0x60009300):
            before=len(freed);run(0x00F26444,data)
            assert len(freed)==before+1 and freed[-1]==(allocator,data)
    return dict(status='PASS',actual_arm_calls=total,rebases=2,
        checks=['skill definition root/index/null','active skill witness','original GMalloc Free argument and callee-saved ABI'])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,help='Receipt output; defaults to the original analysis path')
    args=parser.parse_args()
    raw=GUEST.read_bytes()
    assert hashlib.sha256(raw).hexdigest().upper()==EXPECTED
    with GUEST.open('rb') as stream:
        elf=ELFFile(stream); segments=list(elf.iter_segments())
        def read(rva,n):
            for s in segments:
                if s['p_type']=='PT_LOAD' and s['p_vaddr']<=rva and rva+n<=s['p_vaddr']+s['p_filesz']:
                    off=s['p_offset']+rva-s['p_vaddr']; return raw[off:off+n]
            raise AssertionError(hex(rva))
        for address,words in SITES.items(): assert read(address,16)==struct.pack('<4I',*words),hex(address)
        decoder=Cs(CS_ARCH_ARM,CS_MODE_ARM); veneers=[]
        for address in (0x0183AA50,0x0183ABCC):
            ops=list(decoder.disasm(read(address,8),address))
            assert len(ops)==2
            assert all(op.mnemonic in ('vldr','ldr','cmp') and not re.search(r'\bpc\b',op.op_str) for op in ops)
            veneers.append(dict(site=hex(address),moved=[op.mnemonic+' '+op.op_str for op in ops]))
        result=dict(status='PASS',guest_sha256=EXPECTED,guarded_spans=len(SITES),veneers=veneers,
                    native_queries=native_query_checks(read))
        out=args.output or ROOT/'touch_native_design/analysis/gameplay-verification.json'
        out.parent.mkdir(parents=True,exist_ok=True)
        out.write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps(result,indent=2))

if __name__=='__main__':main()
