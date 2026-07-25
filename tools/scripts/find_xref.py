# psemu: bir RVA'ya (genelde .rodata string'i) RIP-goreli LEA ile referans veren
# kod yerlerini bulur. Kullanim: python find_xref.py <hedef_rva_hex>
# ELF eslemeleri (elf_map.py'den): seg0 off=0x4000 va=0, seg1 off=0x2ec000 va=0x2e8000
import sys

TARGET = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x2f7e14
PATH = r"D:\proje\psemu\PPSA02929-app0\eboot.bin"
b = open(PATH, "rb").read()

TEXT_OFF, TEXT_VA, TEXT_SZ = 0x4000, 0x0, 0x2e70bc
text = b[TEXT_OFF:TEXT_OFF + TEXT_SZ]

# REX.W LEA reg,[rip+disp32] = 48 8D /r  (modrm mod=00, rm=101)
MODRM_RIP = {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D}
REGS = {0x05: "rax", 0x0D: "rcx", 0x15: "rdx", 0x1D: "rbx",
        0x25: "rsp", 0x2D: "rbp", 0x35: "rsi", 0x3D: "rdi"}

hits = []
i = 0
n = len(text)
while True:
    i = text.find(b"\x48\x8d", i)
    if i < 0 or i + 7 > n:
        break
    modrm = text[i + 2]
    if modrm in MODRM_RIP:
        disp = int.from_bytes(text[i + 3:i + 7], "little", signed=True)
        rva_next = TEXT_VA + i + 7          # bir sonraki komutun RVA'si
        if rva_next + disp == TARGET:
            hits.append((TEXT_VA + i, REGS[modrm]))
    i += 1

print(f"hedef RVA 0x{TARGET:x} icin {len(hits)} referans bulundu:")
for rva, reg in hits[:20]:
    print(f"  lea {reg}, [rip+...]  @ RVA 0x{rva:06x}")
