import struct
from capstone import *
from capstone.x86 import *

def disassemble_ep():
    with open(r'PPSA02929-app0\eboot.bin', 'rb') as f:
        data = f.read()

    elf_offset = 0x1a0
    e_phoff = struct.unpack_from('<Q', data, elf_offset + 0x20)[0]
    e_phnum = struct.unpack_from('<H', data, elf_offset + 0x38)[0]
    e_phentsize = struct.unpack_from('<H', data, elf_offset + 0x36)[0]

    memory = bytearray(0x800000)
    for i in range(e_phnum):
        phdr_offset = elf_offset + e_phoff + (i * e_phentsize)
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, phdr_offset)
        if p_type == 1:
            abs_offset = elf_offset + p_offset
            copy_size = min(p_filesz, len(data) - abs_offset)
            if copy_size > 0:
                memory[p_vaddr:p_vaddr+copy_size] = data[abs_offset:abs_offset+copy_size]

    ep = 0x11E0
    scan_size = 4096
    chunk = memory[ep:ep+scan_size]
    
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    
    print("--- ENTRY POINT DISASSEMBLY ---")
    for i in md.disasm(bytes(chunk), ep):
        print(f"0x{i.address:X}:\t{i.mnemonic}\t{i.op_str}")

if __name__ == '__main__':
    disassemble_ep()
