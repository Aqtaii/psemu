# JMPREL girdilerini indeksleyip verilen PLT indekslerinin NID'ini gosterir.
# Kullanim: python plt_entry.py <eboot.bin> 26 32
import struct
import sys

data = open(sys.argv[1], "rb").read()
lo = int(sys.argv[2], 0)
hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else lo

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


symtab = v2f(g(6, 0x61000039))
strtab = v2f(g(5, 0x61000035))
jmprel = v2f(g(23, 0x61000031, 0x6100002F))
pltsz = g(2, 0x61000033)
if jmprel is None or pltsz is None:
    print("JMPREL bulunamadi")
    sys.exit(1)

n = pltsz // 24
print(f"JMPREL: {n} girdi")
for k in range(max(0, lo), min(n, hi + 1)):
    ro, ri, ad = struct.unpack_from("<QQq", data, jmprel + k * 24)
    si = ri >> 32
    st_name = struct.unpack_from("<I", data, symtab + si * 24)[0]
    end = data.index(b"\0", strtab + st_name)
    name = data[strtab + st_name:end].decode("ascii", "replace")
    print(f"  PLT#{k:<5} GOT RVA 0x{ro:x}  sym={si}  NID={name}")
