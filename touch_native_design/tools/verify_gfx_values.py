"""Verify new Scaleform adapter preimages against the immutable current ELF."""
from pathlib import Path
import argparse
import hashlib
import re
import struct
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,help='Fixture output; defaults to the original build_checks path')
    args=parser.parse_args()
    guest = ROOT / 'app/assets/guest_libs/libUnrealEngine3.so'
    raw = guest.read_bytes()
    digest = hashlib.sha256(raw).hexdigest().upper()
    assert digest == 'A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814'
    source = (ROOT / 'arm64_bridge/src/bl2_touch_gfx_values.cpp').read_text(encoding='utf8')
    spans = re.findall(r'\{(0x[0-9A-F]+),\{((?:0x[0-9A-F]+,?)+)\}\}', source)
    assert len(spans) == 19, len(spans)
    fixture = []
    with guest.open('rb') as stream:
        elf = ELFFile(stream)
        for start, words in spans:
            rva = int(start, 16)
            for offset, word in enumerate(words.split(',')):
                at = rva + offset * 4
                segment = next(s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD' and
                               s['p_vaddr'] <= at and at + 4 <= s['p_vaddr'] + s['p_filesz'])
                at_file = segment['p_offset'] + at - segment['p_vaddr']
                actual = struct.unpack_from('<I', raw, at_file)[0]
                assert actual == int(word, 16), (hex(at), hex(actual), word)
                fixture.append(f'{at:08X} {actual:08X}\n')
    out = args.output or ROOT / 'touch_native_design/arm64_bridge/build_checks/gfx_abi_fixture.txt'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(''.join(fixture), encoding='ascii')
    print(f'PASS: {len(spans)} Scaleform ABI spans, {len(fixture)} ARM words, SHA {digest}')

if __name__ == '__main__':
    main()
