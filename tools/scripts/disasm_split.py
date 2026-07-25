from capstone import *
DELTA = 0x4000
f = open(r"D:\proje\psemu\PPSA02929-app0\eboot.bin","rb")
md = Cs(CS_ARCH_X86, CS_MODE_64)

def dis(rva_start, length):
    f.seek(rva_start + DELTA)
    return list(md.disasm(f.read(length), rva_start))

# 0x191090 ve 0x1910a6'da BITEN call'lari verecek hizalamayi bul
best = None
for s in range(0x190f00, 0x191090):
    ins = dis(s, 0x191090 - s + 4)
    ends = {i.address + i.size: i for i in ins}
    if 0x191090 in ends and ends[0x191090].mnemonic == "call":
        best = s
        break
print("hizalama RVA:", hex(best) if best else "BULUNAMADI")
if best:
    for i in dis(best, 0x191130 - best):
        tag = ""
        if i.mnemonic == "call": tag = "   <=== CALL"
        print(f"0x{i.address:06x}: {i.mnemonic:9} {i.op_str}{tag}")
