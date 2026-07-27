# Verilen degeri ADDEND olarak tasiyan RELATIVE relocation'lari bulur.
# Dogrudan cagirani olmayan (vtable/tablo uzerinden cagrilan) fonksiyonlarin
# nerede saklandigini bulmak icin.
# Kullanim: python find_addend.py <eboot.bin> 0x7044490
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
for k in range(2048):
    t, v = struct.unpack_from("<QQ", data, d + k * 16)
    if t == 0:
        break
    tags.append((t, v))


def g(*n):
    for t, v in tags:
        if t in n:
            return v


a, sz = v2f(g(7, 0x6100002F)), g(8, 0x61000031)
hits = []
for k in range(sz // 24):
    ro, ri, ad = struct.unpack_from("<QQq", data, a + k * 24)
    if (ri & 0xFFFFFFFF) == 8 and ad == target:  # R_X86_64_RELATIVE
        hits.append(ro)

print(f"addend 0x{target:x} tasiyan {len(hits)} relocation:")
for h in hits[:20]:
    print(f"  slot RVA 0x{h:x}")
    # Ayni tablodaki komsu girdileri de goster (vtable ise sinifi tanitir)
    for d2 in (-16, -8, 8, 16):
        f2 = v2f(h + d2)
        if f2 is None:
            continue
        for k in range(sz // 24):
            ro, ri, ad = struct.unpack_from("<QQq", data, a + k * 24)
            if ro == h + d2:
                print(f"      komsu {d2:+4d}: -> RVA 0x{ad:x}")
                break
