"""Verify the added diagnostic against the unchanged ARM ELF, without a device."""
from hashlib import sha256
from io import BytesIO
from pathlib import Path
from struct import unpack

from elftools.elf.elffile import ELFFile


def main():
    root = Path(__file__).resolve().parents[2]
    data = (root / "app/assets/guest_libs/libUnrealEngine3.so").read_bytes()
    assert len(data) == 49885152
    assert sha256(data).hexdigest().upper() == "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814"
    elf = ELFFile(BytesIO(data))

    def word(address):
        for segment in elf.iter_segments():
            h = segment.header
            if h.p_type == "PT_LOAD" and h.p_vaddr <= address and address + 4 <= h.p_vaddr + h.p_filesz:
                offset = h.p_offset + address - h.p_vaddr
                return unpack("<I", data[offset:offset + 4])[0]
        raise AssertionError(hex(address))

    symbols = {s.name: s.entry for s in elf.get_section_by_name(".dynsym").iter_symbols()}
    function = symbols["_ZN7PxScene15generateIslandsEb"]
    assert function.st_value == 0x02573280 and function.st_size == 0x5E8
    assert word(0x025732D8) == 0xE50B7028  # Save r7 at fp - 0x28.
    assert word(0x02573684) == 0xE51B2028  # Reload the saved cookie into r2.
    assert word(0x02573688) == 0xE790300C  # GOT pointer from r0 + r12.
    assert word(0x0257368C) == 0xE5933000  # Current guard through that pointer.
    assert word(0x02573838) == 0xE1A0E00F  # LR = architectural PC = 0x02573840.
    assert function.st_value + 0x5C0 == 0x02573840
    got_base = (0x0257329C + 8 + word(0x02573844)) & 0xFFFFFFFF
    guard_slot = (got_base + word(0x02573840)) & 0xFFFFFFFF
    assert got_base == 0x02F39D00 and guard_slot == 0x02F366E8
    guard_relocations = []
    for section in elf.iter_sections():
        if section.header.sh_type not in ("SHT_REL", "SHT_RELA"):
            continue
        linked = elf.get_section(section.header.sh_link)
        for relocation in section.iter_relocations():
            if relocation.entry.r_offset == guard_slot:
                guard_relocations.append(linked.get_symbol(relocation.entry.r_info_sym).name)
    assert guard_relocations == ["__stack_chk_guard"]
    assert (0x42F39D00 + 0xFFFFC9E8) & 0xFFFFFFFF == 0x42F366E8
    assert 0x4EA9DBE4 - 0x28 == 0x4EA9DBBC
    print("PASS original generateIslands symbol, cookie, failure LR and imported guard chain")


if __name__ == "__main__":
    main()
