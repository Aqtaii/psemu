# PS5 ELF'in PT_DYNAMIC / PT_SCE_DYNLIBDATA etiketlerini doker.
# Kullanim: python dump_dynamic.py <eboot.bin>
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()

# SELF konteyneri ise gomulu ELF'i bul
elf_off = 0
if data[:4] != b"\x7fELF":
    for i in range(0, min(len(data), 65536), 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    else:
        print("ELF bulunamadi")
        sys.exit(1)
print(f"ELF ofseti: 0x{elf_off:x}")

e_phoff = struct.unpack_from("<Q", data, elf_off + 32)[0]
e_phnum = struct.unpack_from("<H", data, elf_off + 56)[0]

PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000

SCE_TAGS = {
    0x61000005: "DT_SCE_STRTAB?", 0x61000007: "DT_SCE_STRSZ?",
    0x6100000D: "DT_SCE_HASH", 0x6100000F: "DT_SCE_HASHSZ",
    0x61000019: "DT_SCE_PLTGOT", 0x6100001B: "DT_SCE_JMPREL?",
    0x61000023: "DT_SCE_HASH2", 0x61000025: "DT_SCE_HASH",
    0x61000027: "DT_SCE_HASHSZ", 0x61000029: "DT_SCE_JMPREL",
    0x6100002B: "DT_SCE_PLTREL", 0x6100002D: "DT_SCE_PLTRELSZ",
    0x6100002F: "DT_SCE_RELA", 0x61000031: "DT_SCE_RELASZ",
    0x61000033: "DT_SCE_RELAENT", 0x61000035: "DT_SCE_STRTAB",
    0x61000037: "DT_SCE_STRSZ", 0x61000039: "DT_SCE_SYMTAB",
    0x6100003B: "DT_SCE_SYMENT", 0x6100003F: "DT_SCE_SYMTABSZ",
    0x61000041: "DT_SCE_HIOS",
}
STD_TAGS = {
    1: "DT_NEEDED", 2: "DT_PLTRELSZ", 3: "DT_PLTGOT", 4: "DT_HASH", 5: "DT_STRTAB",
    6: "DT_SYMTAB", 7: "DT_RELA", 8: "DT_RELASZ", 9: "DT_RELAENT", 10: "DT_STRSZ",
    11: "DT_SYMENT", 12: "DT_INIT", 13: "DT_FINI", 20: "DT_PLTREL", 23: "DT_JMPREL",
    25: "DT_INIT_ARRAY", 26: "DT_FINI_ARRAY", 27: "DT_INIT_ARRAYSZ",
    28: "DT_FINI_ARRAYSZ",
}

for i in range(e_phnum):
    o = elf_off + e_phoff + i * 56
    p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", data, o)
    if p_type == PT_SCE_DYNLIBDATA:
        print(f"PT_SCE_DYNLIBDATA: offset=0x{p_offset:x} filesz=0x{p_filesz:x}")
    if p_type != PT_DYNAMIC:
        continue
    print(f"PT_DYNAMIC: offset=0x{p_offset:x} filesz=0x{p_filesz:x}")
    n = p_filesz // 16
    for k in range(n):
        tag, val = struct.unpack_from("<QQ", data, elf_off + p_offset + k * 16)
        if tag == 0:
            break
        name = STD_TAGS.get(tag) or SCE_TAGS.get(tag) or f"0x{tag:x}"
        print(f"   {name:<20} = 0x{val:x}")
