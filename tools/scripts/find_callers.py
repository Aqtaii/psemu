# Verilen RVA'yi "call rel32" ile cagiran yerleri bulur.
# Kullanim: python find_callers.py <eboot.bin> 0x2B2200
import struct
import sys

path, target = sys.argv[1], int(sys.argv[2], 0)
data = open(path, "rb").read()

elf_off, self_map = 0, {}
if data[:4] != b"\x7fELF":
    for i in range(0, 65536, 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    for s in range(struct.unpack_from("<H", data, 0x18)[0]):
        fl, fo, es, ds = struct.unpack_from("<QQQQ", data, 0x20 + s * 32)
        if fl & 0x800 and es == ds:
            self_map[fl >> 20] = fo

ph = struct.unpack_from("<Q", data, elf_off + 32)[0]
pn = struct.unpack_from("<H", data, elf_off + 56)[0]

hits = []
for i in range(pn):
    o = elf_off + ph + i * 56
    pt, fl, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    if pt != 1 or not (fl & 1):  # PT_LOAD + PF_X
        continue
    real = self_map.get(i, elf_off + po)
    seg = data[real:real + pf]
    print(f"segment vaddr=0x{pv:x} taraniyor ({pf} bayt)...")
    # "E8 rel32" ara: hedef = (rva + 5) + rel32
    pos = 0
    while True:
        pos = seg.find(b"\xE8", pos)
        if pos < 0 or pos + 5 > len(seg):
            break
        rel = struct.unpack_from("<i", seg, pos + 1)[0]
        if pv + pos + 5 + rel == target:
            hits.append(pv + pos)
        pos += 1

print(f"\n0x{target:x} adresini cagiran {len(hits)} yer bulundu:")
for h in hits[:40]:
    print(f"  RVA 0x{h:x}")
