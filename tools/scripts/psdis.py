# Verilen RVA'dan itibaren disassemble eder (SELF segment eslemesiyle).
# Kullanim: python dis.py <eboot.bin> 0x29FB20 [bayt]
#           python dis.py <eboot.bin> 0x29FB20 -back   (fonksiyon basini bul)
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

data = open(sys.argv[1], "rb").read()
rva = int(sys.argv[2], 0)
arg3 = sys.argv[3] if len(sys.argv) > 3 else "128"

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


md = Cs(CS_ARCH_X86, CS_MODE_64)

if arg3 == "-back":
    # Geriye dogru "push rbp; mov rbp,rsp" (55 48 89 E5) ara.
    lo = v2f(rva - 0x400)
    hi = v2f(rva)
    blob = data[lo:hi]
    p = blob.rfind(b"\x55\x48\x89\xe5")
    if p < 0:
        print("0x400 bayt geriye dogru standart prologue bulunamadi")
        sys.exit(1)
    start = rva - 0x400 + p
    print(f"fonksiyon basi (tahmin): RVA 0x{start:x}   [+0x{rva - start:x}]")
    rva, n = start, (rva - start) + 48
elif arg3 == "-ret":
    # rva bir DONUS adresi: hizalamayi deneme-yanilma ile bul, cagri komutunu goster.
    best = None
    for back in range(96, 3, -1):
        s = rva - back
        f0 = v2f(s)
        for ins in md.disasm(data[f0:f0 + back], s):
            if ins.address + ins.size == rva and ins.mnemonic.startswith("call"):
                best = s
                break
        if best:
            break
    if best is None:
        print("hizalama bulunamadi (bu adrese biten bir 'call' yok)")
        sys.exit(1)
    rva, n = best - 0x60, 0x60 + (rva - best) + 32
    while v2f(rva) is None:
        rva += 1
else:
    n = int(arg3, 0)

off = v2f(rva)
for ins in md.disasm(data[off:off + n], rva):
    mark = "  <<<" if ins.address == int(sys.argv[2], 0) else ""
    print(f"  0x{ins.address:08x}: {ins.mnemonic:<8} {ins.op_str}{mark}")
