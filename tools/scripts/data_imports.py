# VERI SEMBOLU import'larini listeler (RELA tip 1 / 6) ve isimlerini cozer.
#
# Neden onemli: nid_bulk.py yalnizca JMPREL'i (fonksiyon import'lari) isliyordu.
# Oysa oyun libc/libkernel'den GLOBAL DEGISKEN de import ediyor. psemu bunlari
# bos birakirsa oyun "ilklendim" saniyor ama veri yok - tam olarak Astro Bot'ta
# gordugumuz tablo.
#
# Kullanim: python data_imports.py <eboot.bin>
import base64
import hashlib
import re
import struct
import sys
from collections import Counter

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")

# nids.h + indirilen veritabani
known = {}
for line in open("include/nids.h", encoding="utf-8", errors="replace"):
    m = re.search(r'\{"([A-Za-z0-9+\-]{11})#[^"]*",\s*"([^"]+)"\}', line)
    if m:
        known.setdefault(m.group(1), m.group(2))
try:
    with open("tools/nid_db/aerolib.csv", encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) >= 2 and len(p[0]) == 11:
                known.setdefault(p[0], p[1])
except OSError:
    pass

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

syms = Counter()
for k in range(sz // 24):
    ro, ri, ad = struct.unpack_from("<QQq", data, a + k * 24)
    t = ri & 0xFFFFFFFF
    if t not in (1, 6):  # R_X86_64_64 / GLOB_DAT
        continue
    si = ri >> 32
    if si == 0:
        continue
    st = struct.unpack_from("<I", data, sf + si * 24)[0]
    end = data.index(b"\0", tf + st)
    syms[data[tf + st:end].decode("utf-8", "replace")] += 1

print(f"veri sembolu import'u: {sum(syms.values())} relocation, {len(syms)} farkli sembol\n")
resolved, unresolved = [], []
for raw, n in syms.most_common():
    name = known.get(raw.split("#")[0])
    (resolved if name else unresolved).append((raw, n, name))

print(f"--- ismi cozulen ({len(resolved)}) ---")
for raw, n, name in resolved:
    print(f"  {n:>5}x  {name}")
if unresolved:
    print(f"\n--- COZULEMEYEN ({len(unresolved)}) ---")
    for raw, n, _ in unresolved[:20]:
        print(f"  {n:>5}x  {raw}")

# nids.h'ye eklenmeye hazir satirlar. Veri sembolleri toplu cozume dahil
# DEGILDI (nid_bulk.py yalnizca JMPREL'i isliyor), bu yuzden ayrica gerekiyor.
nidsh = open("include/nids.h", encoding="utf-8", errors="replace").read()
missing = [(raw, name) for raw, n, name in resolved if raw.split("#")[0] not in nidsh]
if missing:
    print(f"\n--- nids.h'de OLMAYAN {len(missing)} sembol (eklenecek satirlar) ---")
    for raw, name in missing:
        print(f'    {{"{raw}", "{name}"}},')
