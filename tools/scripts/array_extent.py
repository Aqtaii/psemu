# Verilen RVA'yi iceren KESINTISIZ 8-bayt relocation blogunu bulur ve
# blogun hemen altindaki dosya degerini gosterir (-1 sonlandiricisi var mi?).
# Kullanim: python array_extent.py <eboot.bin> 0x8E3C368
import struct
import sys

data = open(sys.argv[1], "rb").read()
anchor = int(sys.argv[2], 0)

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
loads, dynv = [], None
for i in range(pn):
    o = elf_off + ph + i * 56
    pt, _, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    r = sm.get(i, elf_off + po)
    if pt == 1:
        loads.append((pv, pf, r))
    elif pt == 2:
        dynv = pv


def v2f(v):
    for pv, pf, r in loads:
        if pv <= v < pv + pf:
            return r + (v - pv)


tags = []
d = v2f(dynv)
for k in range(1024):
    t, v = struct.unpack_from("<QQ", data, d + k * 16)
    if t == 0:
        break
    tags.append((t, v))


def g(*n):
    for t, v in tags:
        if t in n:
            return v


a, sz = v2f(g(7, 0x6100002F)), g(8, 0x61000031)
offs = set()
for k in range(sz // 24):
    ro, ri, ad = struct.unpack_from("<QQq", data, a + k * 24)
    offs.add(ro)

# anchor'dan asagi dogru kesintisiz 8'lik zinciri takip et
low = anchor
while (low - 8) in offs:
    low -= 8
high = anchor
while (high + 8) in offs:
    high += 8

n = (high - low) // 8 + 1
print(f"kesintisiz relocation blogu: RVA 0x{low:x} .. 0x{high:x}  ({n} girdi)")

below = low - 8
fo = v2f(below)
if fo is not None:
    v = struct.unpack_from("<Q", data, fo)[0]
    has_rel = below in offs
    print(f"blogun HEMEN ALTI  RVA 0x{below:x}: dosya degeri = 0x{v:x}"
          f"  (relocation {'VAR' if has_rel else 'YOK'})")
    print("  -> -1 (0xffffffffffffffff) ise yuruyucu burada DURUR; degilse TASAR.")
else:
    print(f"blogun alti (0x{below:x}) hicbir PT_LOAD icinde degil")
