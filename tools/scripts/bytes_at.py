# Verilen RVA'lardaki 16 bayti dosyadan okur (SELF eslemesiyle).
# Calisma zamani dokumuyle (PSEMU_DUMP_RVA) karsilastirmak icin.
# Kullanim: python bytes_at.py <eboot.bin> 0xe3810 0x24ae30
import struct
import sys

data = open(sys.argv[1], "rb").read()

elf_off, sm = 0, {}
if data[:4] != b"\x7fELF":
    for i in range(0, 65536, 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    for s in range(struct.unpack_from("<H", data, 0x18)[0]):
        fl, fo, es, ds = struct.unpack_from("<QQQQ", data, 0x20 + s * 32)
        if fl & 0x800 and es == ds:
            sm[fl >> 20] = fo

ph = struct.unpack_from("<Q", data, elf_off + 32)[0]
pn = struct.unpack_from("<H", data, elf_off + 56)[0]
loads = []
for i in range(pn):
    o = elf_off + ph + i * 56
    pt, _, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    if pt == 1:
        loads.append((pv, pf, sm.get(i, elf_off + po)))

for a in sys.argv[2:]:
    rva = int(a, 0)
    off = None
    for pv, pf, r in loads:
        if pv <= rva < pv + pf:
            off = r + (rva - pv)
    if off is None:
        print(f"  RVA 0x{rva:x}: PT_LOAD disinda")
        continue
    b = " ".join(f"{x:02X}" for x in data[off:off + 16])
    print(f"  RVA 0x{rva:x}: {b}")
