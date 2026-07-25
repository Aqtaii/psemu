from capstone import *

with open(r'D:\proje\psemu\PPSA02929-app0\eboot.bin', 'rb') as f:
    f.seek(0x2c5f00)
    code = f.read(0x100)

md = Cs(CS_ARCH_X86, CS_MODE_64)
for i in md.disasm(code, 0x2c5f00):
    print(f'0x{i.address:x}: {i.mnemonic} {i.op_str}')
