# Verilen ARALIKTAKI vtable'larin belirli bir SLOTUNU listeler.
# "Bu slot hangi turlerde gercek bir fonksiyon, hangilerinde bos stub?"
# sorusunu cevaplamak icin.
# Kullanim: python vtable_slot.py <eboot.bin> 0x88ea000 0x88eb000 6
import struct
import sys

data = open(sys.argv[1], "rb").read()
lo, hi, slot = int(sys.argv[2], 0), int(sys.argv[3], 0), int(sys.argv[4], 0)

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
rel = {}
for k in range(sz // 24):
    ro, ri, ad = struct.unpack_from("<QQq", data, a + k * 24)
    if (ri & 0xFFFFFFFF) == 8 and lo <= ro < hi:
        rel[ro] = ad

# Ardisik relocation bloklarini vtable kabul et; her blogun basi bir vtable.
offs = sorted(rel)
blocks, cur = [], []
for o in offs:
    if cur and o != cur[-1] + 8:
        blocks.append(cur)
        cur = []
    cur.append(o)
if cur:
    blocks.append(cur)

print(f"{len(blocks)} vtable blogu, slot {slot}:")
for b in blocks:
    base = b[0]
    tgt = base + slot * 8
    if tgt in rel:
        print(f"  vtable RVA 0x{base:x}  ({len(b)} slot)  slot{slot} -> RVA 0x{rel[tgt]:x}")
