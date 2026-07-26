import struct, sys, collections
data = open(sys.argv[1],"rb").read()
elf_off, self_map = 0, {}
if data[:4] != b"\x7fELF":
    for i in range(0, 65536, 4):
        if data[i:i+4] == b"\x7fELF": elf_off = i; break
    for s in range(struct.unpack_from("<H", data, 0x18)[0]):
        fl, fo, es, ds = struct.unpack_from("<QQQQ", data, 0x20+s*32)
        if fl & 0x800 and es == ds: self_map[fl>>20] = fo
ph = struct.unpack_from("<Q", data, elf_off+32)[0]; pn = struct.unpack_from("<H", data, elf_off+56)[0]
loads, dynv = [], None
for i in range(pn):
    o = elf_off+ph+i*56
    pt,_,po,pv,_,pf = struct.unpack_from("<IIQQQQ", data, o)
    r = self_map.get(i, elf_off+po)
    if pt==1: loads.append((pv,pf,r))
    elif pt==2: dynv=pv
def v2f(v):
    for pv,pf,r in loads:
        if pv<=v<pv+pf: return r+(v-pv)
tags=[]; d=v2f(dynv)
for k in range(512):
    t,v = struct.unpack_from("<QQ", data, d+k*16)
    if t==0: break
    tags.append((t,v))
def g(*n):
    for t,v in tags:
        if t in n: return v
a,sz = v2f(g(7,0x6100002F)), g(8,0x61000031)
c = collections.Counter()
for k in range(sz//24):
    ro,ri,ad = struct.unpack_from("<QQq", data, a+k*24)
    c[ri & 0xFFFFFFFF] += 1
NAMES={1:"R_X86_64_64",5:"COPY",6:"GLOB_DAT",7:"JUMP_SLOT",8:"RELATIVE",16:"DTPMOD64",17:"DTPOFF64",18:"TPOFF64",37:"IRELATIVE"}
print("RELA tablosundaki relocation tipleri:")
for t,n in sorted(c.items(), key=lambda x:-x[1]):
    print(f"  tip {t:<4} {NAMES.get(t,'BILINMEYEN'):<12} x{n}")
