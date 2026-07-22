import struct

def find_all_magics():
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

    # Entry point fonksiyonu 0x11E0'da basliyor
    # Fonksiyonun sonunu bulmak icin RET (C3) aramasi yapalim
    # Ancak oncelikle tum "cmp dword [...], imm32" pattern'lerini bulalim
    # Pattern: 81 3C XX YY YY YY YY (cmp dword [reg+reg], imm32)
    # veya:    81 3E YY YY YY YY (cmp dword [rsi], imm32)
    
    ep = 0x11E0
    # Fonksiyon buyuk olabilir, 4KB tarayalim
    scan_size = 4096
    chunk = memory[ep:ep+scan_size]
    
    magics = []
    
    for i in range(len(chunk) - 7):
        # Pattern 1: 81 3C 32 XX XX XX XX (cmp dword [rdx+rsi], imm32)
        # Pattern 2: 81 3C 16 XX XX XX XX (cmp dword [rsi+rdx], imm32)
        # Pattern 3: 81 3E XX XX XX XX    (cmp dword [rsi], imm32)
        
        if chunk[i] == 0x81:
            if chunk[i+1] == 0x3C and chunk[i+2] in (0x32, 0x16):
                magic = struct.unpack_from('<I', chunk, i+3)[0]
                addr = ep + i
                magics.append((addr, magic))
            elif chunk[i+1] == 0x3E:
                magic = struct.unpack_from('<I', chunk, i+2)[0]
                addr = ep + i
                magics.append((addr, magic))
    
    # Tekrarlardan arindir (ayni magic birden fazla yerde bulunabilir)
    seen = set()
    unique_magics = []
    for addr, magic in magics:
        if magic not in seen:
            seen.add(magic)
            unique_magics.append((addr, magic))
            print(f"  [0x{addr:04X}] Magic: 0x{magic:08X}")
    
    print(f"\nToplam {len(unique_magics)} benzersiz (unique) sihirli sayi bulundu!")
    print("\nC++ dizisi olarak:")
    print("uint32_t all_magics[] = {")
    for _, magic in unique_magics:
        print(f"    0x{magic:08X},")
    print("};")

if __name__ == '__main__':
    find_all_magics()
