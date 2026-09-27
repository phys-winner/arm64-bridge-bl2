"""Check the two gameplay axis veneers and semantic-call preimages offline."""
from pathlib import Path
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
}

def main():
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
        result=dict(status='PASS',guest_sha256=EXPECTED,guarded_spans=len(SITES),veneers=veneers)
        (ROOT/'touch_native_design/analysis/gameplay-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps(result,indent=2))

if __name__=='__main__':main()
