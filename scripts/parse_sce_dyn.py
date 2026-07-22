import struct

def parse_dyn():
    with open(r'PPSA02929-app0\eboot.bin', 'rb') as f:
        data = f.read()

    elf_offset = 0x1a0
    e_phoff = struct.unpack_from('<Q', data, elf_offset + 0x20)[0]
    e_phnum = struct.unpack_from('<H', data, elf_offset + 0x38)[0]
    e_phentsize = struct.unpack_from('<H', data, elf_offset + 0x36)[0]

    dynamic_vaddr = 0
    dynamic_size = 0
    pt_loads = []

    for i in range(e_phnum):
        phdr_offset = elf_offset + e_phoff + (i * e_phentsize)
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from('<IIQQQQ', data, phdr_offset)
        if p_type == 1: # PT_LOAD
            pt_loads.append((p_vaddr, elf_offset + p_offset, p_filesz))
        elif p_type == 0x61000000 or p_type == 2: # PT_DYNAMIC
            dynamic_vaddr = p_vaddr
            dynamic_size = p_filesz

    if dynamic_vaddr == 0:
        print("No DYNAMIC segment found.")
        return

    print(f"Dynamic segment vaddr 0x{dynamic_vaddr:X}, size 0x{dynamic_size:X}")

    # Find the PT_LOAD that contains this vaddr
    dyn_file_offset = 0
    for vaddr, offset, filesz in pt_loads:
        if vaddr <= dynamic_vaddr < vaddr + filesz:
            dyn_file_offset = offset + (dynamic_vaddr - vaddr)
            break

    if dyn_file_offset == 0:
        print("Dynamic segment vaddr not found in any PT_LOAD!")
        return

    print(f"Dynamic segment mapped to file offset 0x{dyn_file_offset:X}")

    if dyn_file_offset + dynamic_size > len(data):
        print("Dynamic segment extends past end of file!")
        # Let's read what we can
        dynamic_size = len(data) - dyn_file_offset
        if dynamic_size <= 0:
            return

    for i in range(0, dynamic_size, 16):
        d_tag, d_val = struct.unpack_from('<QQ', data, dyn_file_offset + i)
        if d_tag == 0:
            break
        print(f"TAG 0x{d_tag:X}: 0x{d_val:X}")

if __name__ == '__main__':
    parse_dyn()
