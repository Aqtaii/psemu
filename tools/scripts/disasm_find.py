from capstone import *
f = open(r"D:\proje\psemu\PPSA02929-app0\eboot.bin","rb")
START, END = 0x191020, 0x1910d0
f.seek(START); code = f.read(END-START)
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = False
for i in md.disasm(code, START):
    mark = ""
    if i.address == 0x19108b: mark = "   <-- wmemchr CALL buralarda"
    if i.address == 0x1910a1: mark = "   <-- wmemcmp CALL buralarda"
    print(f"0x{i.address:06x}: {i.mnemonic:8} {i.op_str}{mark}")
