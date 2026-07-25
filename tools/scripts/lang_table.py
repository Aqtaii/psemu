# psemu: native dil-kodu tablosunu cozer.
#   lang = sceSystemServiceParamGetInt(1)        (bizim HLE: 1 = English US)
#   idx  = int32[ IDX_TBL + lang*4 ]             (rodata)
#   str  = qword[ STR_TBL + idx*8 ]  -> C string (data)
# Amac: bizim dondurdugumuz LANG degeriyle hangi kodun uretildigini gormek.
import struct

PATH = r"D:\proje\psemu\PPSA02929-app0\eboot.bin"
b = open(PATH, "rb").read()

SEGS = [  # (file_offset, vaddr, filesz)
    (0x004000, 0x000000, 0x2e70bc),
    (0x2ec000, 0x2e8000, 0x08b1c1),
    (0x378000, 0x374000, 0x121380),
    (0x49c000, 0x498000, 0x008200),
]


def off(rva):
    for fo, va, sz in SEGS:
        if va <= rva < va + sz:
            return fo + (rva - va)
    return None


IDX_TBL = 0x1de334 + 0x1339cc   # lea rcx,[rip+0x1339cc] (sonraki komut RVA'si)
STR_TBL = 0x1de348 + 0x29af68   # lea rax,[rip+0x29af68]
print(f"indeks tablosu RVA=0x{IDX_TBL:x}  string tablosu RVA=0x{STR_TBL:x}")
print()
print("lang -> idx -> kod")
for lang in range(0, 24):
    o = off(IDX_TBL + lang * 4)
    if o is None:
        continue
    idx = struct.unpack_from("<i", b, o)[0]
    so = off(STR_TBL + idx * 8)
    code = "?"
    if so is not None:
        ptr = struct.unpack_from("<Q", b, so)[0]
        po = off(ptr)
        if po is not None:
            end = b.find(b"\x00", po)
            code = b[po:end].decode("ascii", "replace")
    mark = "   <== bizim HLE bunu donduruyor (LANG=1)" if lang == 1 else ""
    print(f"  {lang:3} -> {idx:3} -> {code!r}{mark}")
