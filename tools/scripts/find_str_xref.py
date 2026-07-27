# Bir metni ikilide bulur ve ona "lea reg,[rip+disp]" ile atifta bulunan
# kod adreslerini listeler. Assert/log mesajlarindan ilgili koda ulasmak icin.
# Kullanim: python find_str_xref.py <eboot.bin> "depthTarget != nullptr"
import struct
import sys

data = open(sys.argv[1], "rb").read()
needle = sys.argv[2].encode()

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
    pt, fl, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    if pt == 1:
        loads.append((pv, pf, sm.get(i, elf_off + po), bool(fl & 1)))


def f2v(off):
    for pv, pf, r, _x in loads:
        if r <= off < r + pf:
            return pv + (off - r)


# 1) metni bul
targets = []
pos = 0
while True:
    pos = data.find(needle, pos)
    if pos < 0:
        break
    v = f2v(pos)
    if v is not None:
        targets.append(v)
    pos += 1
print(f'"{sys.argv[2]}" -> {len(targets)} yerde: ' + " ".join(f"RVA 0x{t:x}" for t in targets[:6]))
if not targets:
    sys.exit(0)

# 2) calistirilabilir segmentte "48 8D <modrm 05> disp32" (lea reg,[rip+d]) ara
tset = set(targets)
hits = []
for pv, pf, r, execu in loads:
    if not execu:
        continue
    seg = data[r:r + pf]
    i = 0
    n = len(seg)
    while i + 7 <= n:
        # REX.W 48/4C + 8D + modrm(mod=00, rm=101)
        if seg[i] in (0x48, 0x4C) and seg[i + 1] == 0x8D and (seg[i + 2] & 0xC7) == 0x05:
            d = struct.unpack_from("<i", seg, i + 3)[0]
            if (pv + i + 7 + d) in tset:
                hits.append(pv + i)
        i += 1

print(f"\nbu metne atif yapan {len(hits)} kod adresi:")
for h in hits[:30]:
    print(f"  RVA 0x{h:x}")
