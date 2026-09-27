"""Check the runtime observer's ELF guards and displaced instruction contract."""
from pathlib import Path
import hashlib
import json
import re
import struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

ROOT = Path(__file__).resolve().parents[2]
GUEST = ROOT / 'app/assets/guest_libs/libUnrealEngine3.so'
EXPECTED = 'A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814'
HOOKS = [
    (0x01E228FC, 0, [0xE92D4FF0,0xED2D8B04,0xE1A04000,0xE59F5368]),
    (0x01CD8734, 0, [0xE92D47F0,0xED2D8B04,0xE1A04001,0xE59F8370]),
    (0x01CD1678, 0, [0xE92D47F0,0xE1A08000,0xE59002D8,0xE24DD008]),
    (0x023898B0, 4, [0xE59F3338,0xE92D4FF0,0xE1A04000,0xE59F7330]),
]

def main():
    raw = GUEST.read_bytes()
    assert hashlib.sha256(raw).hexdigest().upper() == EXPECTED
    with GUEST.open('rb') as stream:
        elf = ELFFile(stream)
        segments=list(elf.iter_segments())
        symbols={s.name:s.entry for s in elf.get_section_by_name('.dynsym').iter_symbols()}
        def read(rva, n):
            for seg in segments:
                if seg['p_type']=='PT_LOAD' and seg['p_vaddr']<=rva and rva+n<=seg['p_vaddr']+seg['p_filesz']:
                    off=seg['p_offset']+rva-seg['p_vaddr']
                    return raw[off:off+n]
            raise AssertionError(hex(rva))
        evidence=json.loads((ROOT/'touch_native_design/analysis/runtime_seams.json').read_text())
        checked=0
        for item in evidence['symbols']:
            assert symbols[item['symbol']]['st_value']==item['rva'], item['symbol']
            if item['preimage16']:
                preimage=bytes.fromhex(item['preimage16'])
                assert read(item['rva'],len(preimage))==preimage, item['symbol']
            checked+=1
        decoder=Cs(CS_ARCH_ARM,CS_MODE_ARM)
        moved=[]
        for rva,offset,words in HOOKS:
            assert read(rva,16)==struct.pack('<4I',*words)
            instructions=list(decoder.disasm(read(rva+offset,8),rva+offset))
            assert len(instructions)==2
            assert all(i.mnemonic in ('push','vpush','mov') and not re.search(r'\bpc\b',i.op_str) for i in instructions)
            moved.append(dict(site=hex(rva+offset),instructions=[i.mnemonic+' '+i.op_str for i in instructions]))
        result=dict(guest_sha256=EXPECTED,symbols_verified=checked,veneers=moved,status='PASS')
        target=ROOT/'touch_native_design/analysis/runtime-verification.json'
        target.write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps(result,indent=2))

if __name__=='__main__': main()
