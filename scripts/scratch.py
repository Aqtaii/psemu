import struct

f = open('PPSA02929-app0/eboot.bin', 'rb')
elf_offset = 0x1a0
seg_file_offset = elf_offset + 16384  # 0x41a0

# PLT entry at 0x2E3550:
# FF 25 DA EE 1A 00 = JMP [RIP + 0x1AEEDA]
# JMP is at address 0x2E3550, instruction length = 6 bytes
# RIP at next instruction = 0x2E3550 + 6 = 0x2E3556
# GOT slot address = 0x2E3556 + 0x1AEEDA = 0x492430

got_slot_vaddr = 0x2E3556 + 0x1AEEDA
print(f"GOT slot adresi (sanal): 0x{got_slot_vaddr:06X}")

# GOT slotundaki degeri okuyalim
f.seek(seg_file_offset + got_slot_vaddr)
got_value = struct.unpack('<Q', f.read(8))[0]
print(f"GOT slot icerigi (dosyadan): 0x{got_value:016X}")

# Tum PLT entry'lerinin GOT hedeflerini cikaralim
print("\n=== Tum PLT Entry -> GOT Eslemesi ===")
plt_base = 0x2E34C0  # Biraz geriye gidelim, PLT[0]'i bulmak icin
for i in range(30):
    vaddr = plt_base + (i * 16)
    f.seek(seg_file_offset + vaddr)
    data = f.read(6)
    if data[0] == 0xFF and data[1] == 0x25:
        rel_offset = struct.unpack('<I', data[2:6])[0]
        got_addr = vaddr + 6 + rel_offset
        # GOT'taki degeri oku
        f.seek(seg_file_offset + got_addr)
        got_val = struct.unpack('<Q', f.read(8))[0]
        print(f"  PLT[{i:3d}] @ 0x{vaddr:06X} -> GOT @ 0x{got_addr:06X} = 0x{got_val:016X}")

# Simdi crash'in offset 0x60'ta oldugunu biliyoruz
# RIP = base + 0x60, bytes: 81 3C 0E 85 3E E9 19 74 0B
# 81 3C 0E XX XX XX XX = CMP dword [RSI+RCX], imm32
# RSI=0x0, so RSI+RCX = RCX = 0x34d9a00004
# Bu Windows stack/thread yakinlarinda bir adres - PS5 kodu
# burada sceKernelGetProcessParam veya benzeri bir fonksiyondan 
# donen bir pointer'dan okumaya calisiyor olabilir

print("\n=== Crash Analizi ===")
print("Crash RIP: base + 0x60")
print("Instruction: 81 3C 0E ... = CMP dword [RSI+RCX], imm32")
print("RSI = 0x0, RCX = 0x34d9a00004")
print("Ergo: Okunan adres = RSI + RCX = 0x34d9a00004")
print("Bu adres ne oyun bellegi ne de gecerli bir yapidir")
print()

# CALL 0x2E3550 aslinda bu PLT'nin 8. entry'si (index=8)
# Bu PLT entry'si bir EXTERNAL fonksiyonu cagiriyor!
# Oyun bir harici kutuphaneden (libkernel, libSceLibc vb.) fonksiyon cagiriyor
# ve donus degeri olarak aldigi pointer'dan (RAX veya baska register) okumaya calisiyor

# call chain'i anlamak icin entry point sonrasini inceleyelim
print("=== Entry Point sonrasi akis (0x11E0 - 0x1220) ===")
for vaddr in range(0x11E0, 0x1220, 16):
    f.seek(seg_file_offset + vaddr)
    data = f.read(16)
    print(f"  0x{vaddr:06X}: {data.hex(' ')}")

f.close()
