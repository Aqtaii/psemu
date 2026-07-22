import struct
from capstone import *

def check_module_start():
    with open(r'PPSA02929-app0\eboot.bin', 'rb') as f:
        data = f.read()

    elf_offset = 0x1a0
    e_phoff = struct.unpack_from('<Q', data, elf_offset + 0x20)[0]
    e_phnum = struct.unpack_from('<H', data, elf_offset + 0x38)[0]
    e_phentsize = struct.unpack_from('<H', data, elf_offset + 0x36)[0]

    text_offset = 0
    for i in range(e_phnum):
        phdr_offset = elf_offset + e_phoff + (i * e_phentsize)
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from('<IIQQQQ', data, phdr_offset)
        if p_type == 1 and p_vaddr == 0:
            text_offset = elf_offset + p_offset
            break

    chunk = data[text_offset+0x11E0:text_offset+0x1400]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for i in md.disasm(chunk, 0x11E0):
        if i.mnemonic in ('call', 'ret'):
            print(f"0x{i.address:X}:\t{i.mnemonic}\t{i.op_str}")

if __name__ == '__main__':
    check_module_start()
