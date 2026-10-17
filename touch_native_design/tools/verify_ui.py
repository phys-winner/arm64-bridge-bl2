"""Verify native UI ABI sites and complete UI script evidence against current guest."""
from pathlib import Path
import hashlib
import json
import struct
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
GUEST=ROOT/'app/assets/guest_libs/libUnrealEngine3.so'
EXPECTED='A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814'
SITES={
    0x01CC5E7C:(0xE24DD008,0xE92D4FF0),
    0x01E2651C:(0xE590003C,0xE12FFF1E),
    0x01E2652C:(0xE5900040,0xE12FFF1E),
    0x01C074F4:(0xE3A03001,0xE5803238),
    0x01CC52D4:(0xE59FC0C8,0xE92D47F0),
    0x01CC6348:(0xE5923000,0xE5933090,0xE12FFF33),
    0x01E0E984:(0xE12FFF1E,),
    0x00F475C0:(0xE590000C,0xE7E00ED0,0xE12FFF1E),
    0x01CC62B8:(0,2),
    0x02E27330:(0x00F475C0,),
    0x02E1D8F0:(0x00F475C0,),
}
KEYS={'KEY_Escape':0x03009FB8,'KEY_Enter':0x03009FA0,'KEY_Up':0x03009FF0,
      'KEY_Down':0x0300A000,'KEY_Left':0x03009FE8,'KEY_Right':0x03009FF8}
def main():
    data=GUEST.read_bytes();assert hashlib.sha256(data).hexdigest().upper()==EXPECTED
    with GUEST.open('rb') as stream:
        elf=ELFFile(stream); segments=list(elf.iter_segments())
        def read(rva,n):
            for s in segments:
                if s['p_type']=='PT_LOAD' and s['p_vaddr']<=rva and rva+n<=s['p_vaddr']+s['p_filesz']:
                    off=s['p_offset']+rva-s['p_vaddr'];return data[off:off+n]
            raise AssertionError(hex(rva))
        for rva,words in SITES.items():assert read(rva,4*len(words))==struct.pack('<'+'I'*len(words),*words),hex(rva)
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.dynsym').iter_symbols()}
        for name,address in KEYS.items():assert symbols[name]==address,(name,hex(address))
        text=elf.get_section_by_name('.text');assert text['sh_addr']==0x00E823B0 and text['sh_addr']+text['sh_size']==0x02BB91EC
    decoded=json.loads((ROOT/'touch_native_design/analysis/ui-bytecode-analysis.json').read_text())
    assert len(decoded)==11 and all(row['status']=='PASS' for row in decoded)
    result=dict(status='PASS',guest_sha256=EXPECTED,guarded_spans=len(SITES),key_symbols=KEYS,
                full_ui_function_decodes=len(decoded),hardware_ui_delivery_verified=False,
                remaining='direct widget identity, drag/cancel, same-movie content mutations, scroll/pinch')
    (ROOT/'touch_native_design/analysis/ui-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
