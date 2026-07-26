# ELF program basliklarini listeler (PS5 SCE tipleri dahil).
# Kullanim: python dump_segments.py <eboot.bin>
import struct
import sys

TYPES = {
    1: "PT_LOAD", 2: "PT_DYNAMIC", 3: "PT_INTERP", 4: "PT_NOTE", 6: "PT_PHDR",
    7: "PT_TLS", 0x6474E550: "PT_GNU_EH_FRAME", 0x6474E551: "PT_GNU_STACK",
    0x6474E552: "PT_GNU_RELRO",
    0x61000000: "PT_SCE_DYNLIBDATA", 0x61000001: "PT_SCE_PROCPARAM",
    0x61000002: "PT_SCE_MODULE_PARAM", 0x61000010: "PT_SCE_RELRO",
    0x6FFFFF00: "PT_SCE_COMMENT", 0x6FFFFF01: "PT_SCE_VERSION",
}

path = sys.argv[1]
data = open(path, "rb").read()

elf_off = 0
if data[:4] != b"\x7fELF":
    for i in range(0, min(len(data), 65536), 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break

e_phoff = struct.unpack_from("<Q", data, elf_off + 32)[0]
e_phnum = struct.unpack_from("<H", data, elf_off + 56)[0]
print(f"ELF ofseti 0x{elf_off:x}, {e_phnum} segment")
print(f"{'tip':<22} {'offset':>12} {'vaddr':>12} {'filesz':>12}")
for i in range(e_phnum):
    o = elf_off + e_phoff + i * 56
    p_type, _fl, p_offset, p_vaddr, _pa, p_filesz = struct.unpack_from("<IIQQQQ", data, o)
    name = TYPES.get(p_type, f"0x{p_type:x}")
    print(f"{name:<22} 0x{p_offset:>10x} 0x{p_vaddr:>10x} 0x{p_filesz:>10x}")
