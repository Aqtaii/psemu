import struct

f = open('PPSA02929-app0/eboot.bin', 'rb')
elf_offset = 0x1a0
seg_file_offset = elf_offset + 16384  # 0x41a0

f.seek(seg_file_offset)
data = f.read(0x2e70bc)
f.close()

matches = 0
for i in range(len(data) - 16):
    if data[i] == 0xFF and data[i+1] == 0x25 and data[i+6] == 0x68 and data[i+11] == 0xE9:
        rel_offset = struct.unpack('<i', data[i+2:i+6])[0]
        plt_index = struct.unpack('<i', data[i+7:i+11])[0]
        got_addr = i + 6 + rel_offset
        print(f"Match @ {hex(i)}: rel_offset={hex(rel_offset)}, got_addr={hex(got_addr)}, index={plt_index}")
        matches += 1

print(f"Total matches: {matches}")
