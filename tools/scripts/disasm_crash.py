from capstone import *
bytes_hex = '8B 8C D8 A0 E7 FF FF 4C 8D AC D8 90 E7 FF FF 8D'
code = bytes.fromhex(bytes_hex.replace(' ', ''))
md = Cs(CS_ARCH_X86, CS_MODE_64)
for i in md.disasm(code, 0x2c5f65):
    print(f'0x{i.address:x}: {i.mnemonic} {i.op_str}')
