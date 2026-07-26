# Verilen RVA araligina yazan RELA girdilerini listeler.
# Kullanim: python relocs_in_range.py <eboot.bin> 0x8E3C000 0x8E3C380
import struct
import sys

path, lo, hi = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
data = open(path, "rb").read()

elf_off, self_map = 0, {}
if data[:4] != b"\x7fELF":
    for i in range(0, min(len(data), 65536), 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    for s in range(struct.unpack_from("<H", data, 0x18)[0]):
        fl, fo, es, ds = struct.unpack_from("<QQQQ", data, 0x20 + s * 32)
        if fl & 0x800 and es == ds:
            self_map[fl >> 20] = fo

e_phoff = struct.unpack_from("<Q", data, elf_off + 32)[0]
e_phnum = struct.unpack_from("<H", data, elf_off + 56)[0]
loads, dynv = [], None
for i in range(e_phnum):
    o = elf_off + e_phoff + i * 56
    pt, _, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    real = self_map.get(i, elf_off + po)
    if pt == 1:
        loads.append((pv, pf, real))
    elif pt == 2:
        dynv = pv


def v2f(v):
    for pv, pf, real in loads:
        if pv <= v < pv + pf:
            return real + (v - pv)


tags = []
doff = v2f(dynv)
for k in range(0x2000 // 16):
    t, v = struct.unpack_from("<QQ", data, doff + k * 16)
    if t == 0:
        break
    tags.append((t, v))


def g(*n):
    for t, v in tags:
        if t in n:
            return v


TYPES = {1: "R_X86_64_64", 6: "GLOB_DAT", 7: "JUMP_SLOT", 8: "RELATIVE", 5: "COPY",
         18: "TPOFF64", 16: "DTPMOD64", 17: "DTPOFF64"}

total = 0
for name, tag_a, tag_sz in (("RELA", 7, 8), ("JMPREL", 0x17, 0x2)):
    a, sz = g(tag_a, 0x6100002F if name == "RELA" else 0x61000029), g(tag_sz)
    if not a or not sz:
        continue
    f = v2f(a)
    print(f"--- {name}: {sz // 24} girdi ---")
    for k in range(sz // 24):
        ro, ri, ad = struct.unpack_from("<QQq", data, f + k * 24)
        if lo <= ro < hi:
            t = ri & 0xFFFFFFFF
            print(f"  r_offset=0x{ro:x}  tip={TYPES.get(t, t)}  addend=0x{ad & 0xFFFFFFFFFFFFFFFF:x}")
            total += 1
print(f"\naralikta toplam {total} relocation")
