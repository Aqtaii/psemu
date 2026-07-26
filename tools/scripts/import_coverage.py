# Oyunun TUM import'larini listeler ve nids.h'de isim karsiligi olup olmadigini
# gosterir. Cokme-cokme kovalamak yerine eksikleri toplu gormek icin.
# Kullanim: python import_coverage.py <eboot.bin>
import base64
import hashlib
import re
import struct
import sys

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid(n: str) -> str:
    h = hashlib.sha1(n.encode() + SALT).digest()[:8][::-1]
    return base64.b64encode(h).decode().replace("/", "-")[:11]


# nids.h: 11 karakterlik onek -> isim
known = {}
for line in open("include/nids.h", encoding="utf-8", errors="replace"):
    m = re.search(r'\{"([A-Za-z0-9+\-]{11})#[^"]*",\s*"([^"]+)"\}', line)
    if m:
        known.setdefault(m.group(1), m.group(2))

path = sys.argv[1]
data = open(path, "rb").read()

elf_off, self_map = 0, {}
if data[:4] != b"\x7fELF":
    for i in range(0, min(len(data), 65536), 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    num = struct.unpack_from("<H", data, 0x18)[0]
    for s in range(num):
        flags, foff, esz, dsz = struct.unpack_from("<QQQQ", data, 0x20 + s * 32)
        if flags & 0x800 and esz == dsz:
            self_map[flags >> 20] = foff

e_phoff = struct.unpack_from("<Q", data, elf_off + 32)[0]
e_phnum = struct.unpack_from("<H", data, elf_off + 56)[0]
loads, dynv = [], None
for i in range(e_phnum):
    o = elf_off + e_phoff + i * 56
    p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", data, o)
    real = self_map.get(i, elf_off + p_offset)
    if p_type == 1:
        loads.append((p_vaddr, p_filesz, real))
    elif p_type == 2:
        dynv = p_vaddr


def v2f(v):
    for vaddr, fsz, real in loads:
        if vaddr <= v < vaddr + fsz:
            return real + (v - vaddr)
    return None


doff = v2f(dynv)
tags = []
for k in range(0x2000 // 16):
    t, v = struct.unpack_from("<QQ", data, doff + k * 16)
    if t == 0:
        break
    tags.append((t, v))


def get(*names):
    for t, v in tags:
        if t in names:
            return v
    return None


jf = v2f(get(0x17, 0x61000029))
pltsz = get(0x2, 0x6100002D)
sf = v2f(get(6, 0x61000039))
tf = v2f(get(5, 0x61000035))


def sym_nid(idx):
    st_name = struct.unpack_from("<I", data, sf + idx * 24)[0]
    end = data.index(b"\0", tf + st_name)
    return data[tf + st_name:end].decode("utf-8", "replace")


resolved, unresolved = [], []
for k in range(pltsz // 24):
    r_offset, r_info, _ = struct.unpack_from("<QQq", data, jf + k * 24)
    raw = sym_nid(r_info >> 32)
    pref = raw.split("#")[0]
    name = known.get(pref)
    (resolved if name else unresolved).append((k, raw, name))

print(f"toplam import: {len(resolved) + len(unresolved)}")
print(f"  isim cozulen  : {len(resolved)}")
print(f"  COZULEMEYEN   : {len(unresolved)}")
if unresolved:
    print("\n--- cozulemeyen NID'ler (modul soneki ile) ---")
    for k, raw, _ in unresolved:
        print(f"  PLT#{k:<5} {raw}")
