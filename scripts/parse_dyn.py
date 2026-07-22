import struct

def parse_dynamic():
    with open(r'PPSA02929-app0\eboot.bin', 'rb') as f:
        data = f.read()

    elf_offset = 0x1a0
    e_phoff = struct.unpack_from('<Q', data, elf_offset + 0x20)[0]
    e_phnum = struct.unpack_from('<H', data, elf_offset + 0x38)[0]
    e_phentsize = struct.unpack_from('<H', data, elf_offset + 0x36)[0]

    dynamic_offset = 0
    dynamic_size = 0
    dynamic_vaddr = 0

    for i in range(e_phnum):
        phdr_offset = elf_offset + e_phoff + (i * e_phentsize)
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, phdr_offset)
        if p_type == 2: # PT_DYNAMIC
            dynamic_offset = elf_offset + p_offset
            dynamic_size = min(p_filesz, len(data) - dynamic_offset)
            dynamic_vaddr = p_vaddr
            break

    if dynamic_offset == 0:
        print("PT_DYNAMIC not found!")
        return

    print("--- DYNAMIC SECTION ---")
    DT_INIT = 12
    DT_FINI = 13
    DT_INIT_ARRAY = 25
    DT_FINI_ARRAY = 26
    DT_INIT_ARRAYSZ = 27
    DT_FINI_ARRAYSZ = 28
    
    # PS4/PS5 specific tags?
    DT_SCE_ENTRY = 0x61000027 # maybe? 
    
    tags = {
        12: "DT_INIT",
        13: "DT_FINI",
        25: "DT_INIT_ARRAY",
        26: "DT_FINI_ARRAY",
        27: "DT_INIT_ARRAYSZ",
        28: "DT_FINI_ARRAYSZ",
        16: "DT_SYMBOLIC",
        0x6100002F: "DT_SCE_MODULE_INFO"
    }

    for i in range(0, dynamic_size, 16):
        d_tag, d_val = struct.unpack_from('<QQ', data, dynamic_offset + i)
        if d_tag == 0: # DT_NULL
            break
        print(f"TAG {d_tag}: 0x{d_val:X}")

if __name__ == '__main__':
    parse_dynamic()
