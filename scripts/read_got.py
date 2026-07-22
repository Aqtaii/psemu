import struct

def read_got():
    with open(r'PPSA02929-app0\eboot.bin', 'rb') as f:
        data = f.read()

    elf_offset = 0x1a0
    
    # Text segment is mapped at vaddr 0x0, which is at file offset 0x1a0 (plus PT_LOAD p_offset which is 0)
    # Actually, we need to map the file exactly as loader.cpp does!
    
    # Parse phdrs
    e_phoff = struct.unpack_from('<Q', data, elf_offset + 0x20)[0]
    e_phnum = struct.unpack_from('<H', data, elf_offset + 0x38)[0]
    e_phentsize = struct.unpack_from('<H', data, elf_offset + 0x36)[0]

    # Map memory
    memory = bytearray(0x800000) # 8MB
    
    for i in range(e_phnum):
        phdr_offset = elf_offset + e_phoff + (i * e_phentsize)
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, phdr_offset)
        if p_type == 1: # PT_LOAD
            abs_offset = elf_offset + p_offset
            copy_size = min(p_filesz, len(data) - abs_offset)
            if copy_size > 0:
                memory[p_vaddr:p_vaddr+copy_size] = data[abs_offset:abs_offset+copy_size]

    # Now read GOT values based on PLT scanning
    pattern = [0xFF, 0x25, -1, -1, -1, -1, 0x68, -1, -1, -1, -1, 0xE9]
    text_size = 0x2e70bc
    
    print("[+] Scanning PLT...")
    matches = 0
    i = 0
    while i < text_size - 16:
        match = True
        for j in range(len(pattern)):
            if pattern[j] != -1 and memory[i+j] != pattern[j]:
                match = False
                break
        
        if match:
            rel_offset = struct.unpack_from('<i', memory, i+2)[0]
            rip_next = i + 6
            got_addr = rip_next + rel_offset
            plt_index = struct.unpack_from('<i', memory, i+7)[0]
            
            got_val = struct.unpack_from('<Q', memory, got_addr)[0]
            print(f"PLT Index: {plt_index:3d} | GOT Addr: 0x{got_addr:x} | Original GOT Value: 0x{got_val:016x}")
            
            matches += 1
            i += 15
        else:
            i += 1

if __name__ == '__main__':
    read_got()
