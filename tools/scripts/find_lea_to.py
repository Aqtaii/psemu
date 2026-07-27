# Verilen RVA'yi "lea reg,[rip+disp]" ile ADRESLEYEN kod yerlerini bulur.
# vtable adresini bir nesneye yazan YAPICIYI bulmak icin: derleyici
# "lea rax,[rip+X]; mov [rdi],rax" uretir. Relocation aramasi (find_addend.py)
# bunu bulamaz cunku PIC kodda adres komutun icinde hesaplanir.
# Kullanim: python find_lea_to.py <eboot.bin> 0x88ea758
import struct
import sys

data = open(sys.argv[1], "rb").read()
target = int(sys.argv[2], 0)

elf_off, sm = 0, {}
if data[:4] != b"\x7fELF":
    for i in range(0, 65536, 4):
        if data[i:i + 4] == b"\x7fELF":
            elf_off = i
            break
    for s in range(struct.unpack_from("<H", data, 0x18)[0]):
        fl, fo, es, ds = struct.unpack_from("<QQQQ", data, 0x20 + s * 32)
        if fl & 0x800 and es == ds:
            sm[fl >> 20] = fo

ph = struct.unpack_from("<Q", data, elf_off + 32)[0]
pn = struct.unpack_from("<H", data, elf_off + 56)[0]

hits = []
for i in range(pn):
    o = elf_off + ph + i * 56
    pt, fl, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    if pt != 1 or not (fl & 1):   # PT_LOAD + PF_X
        continue
    r = sm.get(i, elf_off + po)
    seg = data[r:r + pf]
    n = len(seg)
    k = 0
    while k + 7 <= n:
        # REX.W (48/4C) + 8D + modrm(mod=00, rm=101) = lea r64,[rip+disp32]
        # 8D = lea (adresi al), 8B = mov (icerigi oku). Ikisi de
        # mod=00 rm=101 ile rip-goreli. Global bir TEKIL isaretcisini kimin
        # OKUDUGUNU bulmak icin 8B de sart.
        if seg[k] in (0x48, 0x4C) and seg[k + 1] in (0x8D, 0x8B) and \
           (seg[k + 2] & 0xC7) == 0x05:
            d = struct.unpack_from("<i", seg, k + 3)[0]
            if pv + k + 7 + d == target:
                reg = ((seg[k] & 0x04) << 1) | ((seg[k + 2] >> 3) & 7)
                names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                         "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
                kind = "lea" if seg[k + 1] == 0x8D else "mov"
                hits.append((pv + k, f"{kind} {names[reg]}"))
        k += 1

print(f"0x{target:x} adresini lea/mov ile kullanan {len(hits)} yer:")
for a, reg in hits[:30]:
    print(f"  RVA 0x{a:x}   (lea {reg})")
