# Verilen r_offset'teki relocation'in SEMBOLUNU cozer (RELATIVE olmayanlar icin).
# Kullanim: python reloc_symbol.py <eboot.bin> 0x88dc088
import base64
import hashlib
import re
import struct
import sys

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")

known = {}
for line in open("include/nids.h", encoding="utf-8", errors="replace"):
    m = re.search(r'\{"([A-Za-z0-9+\-]{11})#[^"]*",\s*"([^"]+)"\}', line)
    if m:
        known.setdefault(m.group(1), m.group(2))

data = open(sys.argv[1], "rb").read()
want = int(sys.argv[2], 0)

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
for k in range(512):
    t, v = struct.unpack_from("<QQ", data, d + k * 16)
    if t == 0:
        break
    tags.append((t, v))


def g(*n):
    for t, v in tags:
        if t in n:
            return v


a, sz = v2f(g(7, 0x6100002F)), g(8, 0x61000031)
sf, tf = v2f(g(6, 0x61000039)), v2f(g(5, 0x61000035))

for k in range(sz // 24):
    ro, ri, ad = struct.unpack_from("<QQq", data, a + k * 24)
    if ro == want:
        si = ri >> 32
        st = struct.unpack_from("<I", data, sf + si * 24)[0]
        end = data.index(b"\0", tf + st)
        raw = data[tf + st:end].decode("utf-8", "replace")
        pref = raw.split("#")[0]
        print(f"  r_offset=0x{ro:x}  tip={ri & 0xFFFFFFFF}  sembol_NID={raw}")
        print(f"  -> isim: {known.get(pref, 'COZULEMEDI')}")
        break
else:
    print("  bu r_offset RELA tablosunda bulunamadi")
