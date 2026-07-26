# PS5 ELF/SELF'in import kutuphanelerini ve modullerini doker.
# NID sonekleri (#X#Y) kutuphane/modul INDEKSIDIR; hangi kutuphaneden geldigini
# bilmek, cozulemeyen bir NID'in arama alanini cok daraltir.
# Kullanim: python dump_imports.py <eboot.bin>
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()

# --- SELF ise gercek segment konumlarini cikar (bkz. loader.cpp) -------------
elf_off = 0
self_map = {}
if data[:4] != b"\x7fELF":
    for i in range(0, min(len(data), 65536), 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    num = struct.unpack_from("<H", data, 0x18)[0]
    for s in range(num):
        rec = 0x20 + s * 32
        flags, foff, esz, dsz = struct.unpack_from("<QQQQ", data, rec)
        if flags & 0x800 and esz == dsz:
            self_map[flags >> 20] = foff

e_phoff = struct.unpack_from("<Q", data, elf_off + 32)[0]
e_phnum = struct.unpack_from("<H", data, elf_off + 56)[0]

# vaddr -> dosya ofseti cevirici (SELF eslemesini kullanir)
loads = []
dyn = None
for i in range(e_phnum):
    o = elf_off + e_phoff + i * 56
    p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", data, o)
    real = self_map.get(i, elf_off + p_offset)
    if p_type == 1:  # PT_LOAD
        loads.append((p_vaddr, p_filesz, real))
    elif p_type == 2:  # PT_DYNAMIC
        dyn = (p_vaddr, p_filesz, real)

def v2f(v):
    for vaddr, fsz, real in loads:
        if vaddr <= v < vaddr + fsz:
            return real + (v - vaddr)
    return None

if dyn is None:
    print("PT_DYNAMIC yok")
    sys.exit(1)

dv, dsz, _own_off = dyn
# ONEMLI: PT_DYNAMIC'in KENDI p_offset'i SELF'te yaniltici. Icerigi bir PT_LOAD
# icinde yer aliyor; dogru konum vaddr'i PT_LOAD eslemesinden gecirmekle bulunur
# (psemu de bellege kopyaladiktan sonra base+vaddr'dan okuyor).
doff = v2f(dv)
if doff is None:
    print("PT_DYNAMIC hicbir PT_LOAD icinde degil")
    sys.exit(1)
print(f"PT_DYNAMIC vaddr=0x{dv:x} filesz=0x{dsz:x} -> dosya 0x{doff:x}")

# PS5 SCE dinamik etiketleri
DT_SCE_STRTAB   = 0x61000035
DT_SCE_STRSZ    = 0x61000037
# Gercek dosyadan olculen etiketler (Astro Bot): 0x61000019 x62 (import lib),
# 0x61000045 x59 (needed module), 0x61000049 x62 (lib attr).
DT_SCE_IMPORT_LIB    = 0x61000019
DT_SCE_NEEDED_MODULE = 0x61000045
DT_SCE_EXPORT_LIB    = 0x61000013
DT_SCE_MODULE_INFO   = 0x6100000D

tags = []
for k in range(dsz // 16):
    tag, val = struct.unpack_from("<QQ", data, doff + k * 16)
    if tag == 0:
        break
    tags.append((tag, val))

strtab_v = next((v for t, v in tags if t in (DT_SCE_STRTAB, 5)), None)
print(f"strtab vaddr=0x{strtab_v:x}" if strtab_v else "strtab bulunamadi")
strtab_f = v2f(strtab_v) if strtab_v else None

def s(off):
    if strtab_f is None:
        return "?"
    end = data.index(b"\0", strtab_f + off)
    return data[strtab_f + off:end].decode("utf-8", "replace")

B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

print("\n--- import kutuphaneleri / moduller ---")
for tag, val in tags:
    if tag in (DT_SCE_IMPORT_LIB, DT_SCE_EXPORT_LIB, DT_SCE_NEEDED_MODULE, DT_SCE_MODULE_INFO):
        name_off = val & 0xFFFFFFFF
        idx = (val >> 48) & 0xFFFF
        kind = {DT_SCE_IMPORT_LIB: "IMPORT_LIB", DT_SCE_EXPORT_LIB: "EXPORT_LIB",
                DT_SCE_NEEDED_MODULE: "MODULE", DT_SCE_MODULE_INFO: "MODULE_INFO"}[tag]
        enc = B64[idx] if idx < 64 else f"?{idx}"
        print(f"  {kind:<12} id={idx:<4} (#{enc})  {s(name_off)}")

print("\n--- tum dinamik etiketler (bilinmeyenler dahil) ---")
seen = {}
for tag, val in tags:
    seen[tag] = seen.get(tag, 0) + 1
for tag in sorted(seen):
    print(f"  0x{tag:x} x{seen[tag]}")
