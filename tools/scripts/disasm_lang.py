# psemu: verilen bir DONUS ADRESI (RVA) icin cagri noktasini hizalayip
# cevresini soker. ELF eslemesi: dosya offset = RVA + 0x4000 (bkz. elf_map.py).
# Kullanim: python disasm_lang.py [hedef_rva_hex] [geri_bayt] [ileri_bayt]
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DELTA = 0x4000
TARGET = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x2bf7df
BACK = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0xc0
FWD = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x40

f = open(r"D:\proje\psemu\PPSA02929-app0\eboot.bin", "rb")
md = Cs(CS_ARCH_X86, CS_MODE_64)


def dis(start, length):
    f.seek(start + DELTA)
    return list(md.disasm(f.read(length), start))


# TARGET'ta BITEN bir call uretecek hizalamayi bul (dogru komut siniri)
best = None
for s in range(TARGET - BACK, TARGET):
    ins = dis(s, TARGET - s + 8)
    if any(i.address + i.size == TARGET and i.mnemonic == "call" for i in ins):
        best = s
        break

print("hedef RVA:", hex(TARGET), " hizalama:", hex(best) if best else "BULUNAMADI")
if best:
    for i in dis(best, (TARGET + FWD) - best):
        tag = "   <== CAGRI (donus = hedef)" if i.address + i.size == TARGET else ""
        print(f"0x{i.address:06x}: {i.mnemonic:9} {i.op_str}{tag}")
