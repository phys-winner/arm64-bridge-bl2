"""Check every model-reader ABI preimage against the unchanged canonical guest."""
from pathlib import Path
import hashlib
import json
import re
import struct
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]

def main():
    guest=ROOT/'app/assets/guest_libs/libUnrealEngine3.so'
    raw=guest.read_bytes();sha=hashlib.sha256(raw).hexdigest().upper()
    assert sha=='A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814'
    source=(ROOT/'arm64_bridge/src/bl2_touch_ui_model.cpp').read_text(encoding='utf-8')
    pairs=[tuple(int(x,16) for x in p) for p in re.findall(r'\{(0x[0-9A-F]+),(0x[0-9A-F]+)\}',source)]
    assert len(pairs)==16
    with guest.open('rb') as stream:
        elf=ELFFile(stream)
        for rva,word in pairs:
            seg=next(s for s in elf.iter_segments() if s['p_type']=='PT_LOAD' and
                s['p_vaddr']<=rva and rva+4<=s['p_vaddr']+s['p_filesz'])
            off=seg['p_offset']+rva-seg['p_vaddr']
            assert struct.unpack_from('<I',raw,off)[0]==word,hex(rva)
    evidence=ROOT/'touch_native_design/analysis'
    counts={}
    for name,count in (('ui-widget-bytecode',18),('ui-bytecode-analysis',11),('camera-bytecode-analysis',5)):
        rows=json.loads((evidence/(name+'.json')).read_text())
        assert len(rows)==count and all(r['status']=='PASS' for r in rows),name
        counts[name]=count
    result=dict(status='PASS',guest_sha256=sha,reflection_abi_spans=len(pairs),
                complete_function_decodes=counts,hardware_ui_model_verified=False,
                note='Structural observer only; no complete widget identity, bounds or content generation claimed.')
    (evidence/'ui-model-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
