# Verilen RVA'daki C string'i okur (SELF segment eslemesiyle).
# Kullanim: python read_string.py <eboot.bin> 0x812956A [adet]
import struct
import sys

data = open(sys.argv[1], "rb").read()
rva = int(sys.argv[2], 0)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 1

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


def v2f(v):
    for pv, pf, r in loads:
        if pv <= v < pv + pf:
            return r + (v - pv)


off = v2f(rva)
if off is None:
    print("RVA hicbir PT_LOAD icinde degil")
    sys.exit(1)

for k in range(count):
    end = data.index(b"\0", off)
    s = data[off:end].decode("utf-8", "replace")
    print(f"  RVA 0x{rva:x}: \"{s}\"")
    rva += (end - off) + 1
    off = end + 1
