# Verilen RVA'daki GOT slotunun hangi import sembolune ait oldugunu bulur.
# Kullanim: python find_got_slot.py <eboot.bin> 0x8FD4028
import struct
import sys

path = sys.argv[1]
want = int(sys.argv[2], 0)
data = open(path, "rb").read()

# SELF eslemesi (bkz. loader.cpp)
elf_off = 0
self_map = {}
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

loads, dyn = [], None
for i in range(e_phnum):
    o = elf_off + e_phoff + i * 56
    p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", data, o)
    real = self_map.get(i, elf_off + p_offset)
    if p_type == 1:
        loads.append((p_vaddr, p_filesz, real))
    elif p_type == 2:
        dyn = p_vaddr

def v2f(v):
    for vaddr, fsz, real in loads:
        if vaddr <= v < vaddr + fsz:
            return real + (v - vaddr)
    return None

doff = v2f(dyn)
tags = []
for k in range(0x2000 // 16):
    tag, val = struct.unpack_from("<QQ", data, doff + k * 16)
    if tag == 0:
        break
    tags.append((tag, val))

def get(*names):
    for t, v in tags:
        if t in names:
            return v
    return None

# Olculen etiketler: JMPREL/PLTRELSZ standart (0x17 / 0x2), strtab 0x61000035 veya 5
jmprel = get(0x17, 0x61000029)
pltsz  = get(0x2, 0x6100002D)
symtab = get(6, 0x61000039)
strtab = get(5, 0x61000035)
print(f"jmprel=0x{jmprel:x} pltrelsz=0x{pltsz:x} symtab=0x{symtab:x} strtab=0x{strtab:x}")

jf, sf, tf = v2f(jmprel), v2f(symtab), v2f(strtab)

def sym_name(idx):
    st_name = struct.unpack_from("<I", data, sf + idx * 24)[0]
    end = data.index(b"\0", tf + st_name)
    return data[tf + st_name:end].decode("utf-8", "replace")

n = pltsz // 24
print(f"{n} JUMP_SLOT taraniyor, 0x{want:x} araniyor...")
for k in range(n):
    r_offset, r_info, _ = struct.unpack_from("<QQq", data, jf + k * 24)
    if r_offset == want:
        print(f"  BULUNDU: index={k}  r_offset=0x{r_offset:x}  sembol={sym_name(r_info >> 32)}")
        break
else:
    print("  Bu RVA bir JUMP_SLOT GOT slotu DEGIL.")
    # RELA (veri relocation) tablosunda mi?
    rela, relasz = get(7, 0x6100002F), get(8, 0x61000031)
    if rela and relasz:
        rf = v2f(rela)
        for k in range(relasz // 24):
            r_offset, r_info, addend = struct.unpack_from("<QQq", data, rf + k * 24)
            if r_offset == want:
                rt = r_info & 0xFFFFFFFF
                si = r_info >> 32
                nm = sym_name(si) if si else "(sembolsuz)"
                print(f"  RELA'da bulundu: tip={rt} sembol={nm} addend=0x{addend:x}")
                break
        else:
            print("  RELA'da da yok.")
