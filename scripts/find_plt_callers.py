import struct
from capstone import *

def find_plt_callers():
    with open(r'PPSA02929-app0\eboot.bin', 'rb') as f:
        data = f.read()

    elf_offset = 0x1a0
    e_phoff = struct.unpack_from('<Q', data, elf_offset + 0x20)[0]
    e_phnum = struct.unpack_from('<H', data, elf_offset + 0x38)[0]
    e_phentsize = struct.unpack_from('<H', data, elf_offset + 0x36)[0]

    text_offset = 0
    text_size = 0
    for i in range(e_phnum):
        phdr_offset = elf_offset + e_phoff + (i * e_phentsize)
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from('<IIQQQQ', data, phdr_offset)
        if p_type == 1 and p_vaddr == 0:
            text_offset = elf_offset + p_offset
            text_size = p_filesz
            break

    plt_start = 0x2e34d0
    plt_end = 0x2e4dd0 + 16

    chunk = data[text_offset:text_offset+text_size]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = True

    plt_callers = {}

    print("Scanning for calls to PLT...")
    for i in md.disasm(chunk, 0):
        if i.mnemonic == 'call':
            try:
                target = int(i.op_str, 16)
                if plt_start <= target <= plt_end:
                    plt_idx = (target - plt_start) // 16
                    if plt_idx not in plt_callers:
                        plt_callers[plt_idx] = []
                    plt_callers[plt_idx].append(i.address)
            except ValueError:
                pass

    print(f"Found calls to {len(plt_callers)} distinct PLT entries.")
    
    # Sort by number of callers
    for idx, callers in sorted(plt_callers.items(), key=lambda x: len(x[1]), reverse=True)[:20]:
        print(f"PLT Index {idx}: {len(callers)} callers. Example callers: {[hex(x) for x in callers[:5]]}")

if __name__ == '__main__':
    find_plt_callers()
