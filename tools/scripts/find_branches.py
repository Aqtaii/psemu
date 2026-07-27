# Verilen RVA'ya DALLANAN yerleri bulur: jcc rel32 (0F 8x), jmp rel32 (E9),
# jcc rel8 (7x) ve jmp rel8 (EB). find_callers.py yalnizca "call" ariyor;
# assert/hata yollarina genelde JUMP ile gidilir.
# Kullanim: python find_branches.py <eboot.bin> 0x7410999
import struct
import sys

data = open(sys.argv[1], "rb").read()
target = int(sys.argv[2], 0)

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
hits = []
for i in range(pn):
    o = elf_off + ph + i * 56
    pt, fl, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    if pt != 1 or not (fl & 1):
        continue
    r = sm.get(i, elf_off + po)
    seg = data[r:r + pf]
    n = len(seg)
    for k in range(n - 6):
        b = seg[k]
        if b == 0x0F and 0x80 <= seg[k + 1] <= 0x8F:
            d = struct.unpack_from("<i", seg, k + 2)[0]
            if pv + k + 6 + d == target:
                hits.append((pv + k, "jcc rel32"))
        elif b == 0xE9:
            d = struct.unpack_from("<i", seg, k + 1)[0]
            if pv + k + 5 + d == target:
                hits.append((pv + k, "jmp rel32"))
        elif 0x70 <= b <= 0x7F:
            d = struct.unpack_from("<b", seg, k + 1)[0]
            if pv + k + 2 + d == target:
                hits.append((pv + k, "jcc rel8"))
        elif b == 0xEB:
            d = struct.unpack_from("<b", seg, k + 1)[0]
            if pv + k + 2 + d == target:
                hits.append((pv + k, "jmp rel8"))

print(f"0x{target:x} adresine dallanan {len(hits)} yer:")
for a, kind in hits[:30]:
    print(f"  RVA 0x{a:x}   ({kind})")
