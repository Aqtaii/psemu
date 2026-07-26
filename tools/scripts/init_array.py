# DT_INIT_ARRAY / DT_INIT_ARRAYSZ okur ve dizinin GERCEK sinirlarini gosterir.
# Yuruyucunun nereye kadar gitmesi gerektigini bilmek icin.
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

NAMES = {12: "DT_INIT", 13: "DT_FINI", 25: "DT_INIT_ARRAY", 26: "DT_FINI_ARRAY",
         27: "DT_INIT_ARRAYSZ", 28: "DT_FINI_ARRAYSZ", 32: "DT_PREINIT_ARRAY",
         33: "DT_PREINIT_ARRAYSZ"}
found = {}
for t, v in tags:
    if t in NAMES:
        found[t] = v
        print(f"  {NAMES[t]:<20} = 0x{v:x}")

if 25 in found and 27 in found:
    start, size = found[25], found[27]
    n = size // 8
    print(f"\n.init_array: RVA 0x{start:x} .. 0x{start + size:x}  ({n} girdi)")
    print("Yuruyucu SONDAN geriye gidiyor; bitis siniri 0x%x olmali." % (start + size))
