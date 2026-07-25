import struct
f = open(r"D:\proje\psemu\PPSA02929-app0\eboot.bin","rb")
d = f.read(0x400)
e_phoff  = struct.unpack_from("<Q", d, 0x20)[0]
e_phentsize, e_phnum = struct.unpack_from("<HH", d, 0x36)
print(f"phoff=0x{e_phoff:x} phentsize={e_phentsize} phnum={e_phnum}")
f.seek(e_phoff); ph = f.read(e_phentsize*e_phnum)
segs = []
for i in range(e_phnum):
    o = i*e_phentsize
    p_type, p_flags = struct.unpack_from("<II", ph, o)
    p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from("<QQQQQQ", ph, o+8)
    if p_type == 1 or p_type == 0x6474e551 or p_filesz:
        segs.append((p_type,p_flags,p_offset,p_vaddr,p_filesz,p_memsz))
        print(f"type=0x{p_type:08x} flags={p_flags} offset=0x{p_offset:08x} vaddr=0x{p_vaddr:08x} filesz=0x{p_filesz:x} memsz=0x{p_memsz:x}")

def rva2off(rva):
    for t,fl,off,va,fsz,msz in segs:
        if t == 1 and va <= rva < va+fsz:
            return off + (rva - va)
    return None
for r in (0x191090, 0x1910a6, 0x2df5c6):
    print(f"RVA 0x{r:x} -> dosya offset {hex(rva2off(r)) if rva2off(r) is not None else 'YOK'}")
