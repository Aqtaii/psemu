# .eh_frame_hdr'dan KESIN fonksiyon sinirlarini cikarir.
#
# NEDEN: psdis.py'nin iki sezgisi de buyuk fonksiyonlarda YANILIYOR:
#   -func : fonksiyonlar arasi 0xCC dolgusuna bakar; dolgu yoksa ya da
#           fonksiyon 0x20000'den buyukse yanlis adres verir. Astro Bot'ta
#           0x7075ead icin "0x70742d0" dedi; oysa o adres KISA bir
#           ilklendiricinin basi ve kayit blogunu icermiyor. Bu yuzden bir
#           tur boyunca YANLIS FONKSIYONU olctum.
#   -back : yalnizca 0x400 bayt geriye "push rbp; mov rbp,rsp" arar; buyuk
#           fonksiyonlarda hic bulamaz.
#
# PT_GNU_EH_FRAME icindeki .eh_frame_hdr, ikili arama tablosu olarak
# (fonksiyon_baslangici, FDE) ciftlerini SIRALI tutar - yani fonksiyon
# baslarinin kesin listesi. Sezgi yok.
#
# Kullanim:
#   python func_bounds.py <eboot.bin>            -> tablo ozeti
#   python func_bounds.py <eboot.bin> 0x7075ead  -> o RVA'yi iceren fonksiyon
import struct
import sys

data = open(sys.argv[1], "rb").read()

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

# SELF'te PT_GNU_EH_FRAME'in KENDI dosya ofseti guvenilir degil; adresi
# iceren PT_LOAD uzerinden cevirmek gerekiyor (psdis.py'nin v2f'i ile ayni
# yontem). Bunu atlayinca basligin ilk baytlari cop okunuyor ve kodlama
# baytlari anlamsiz cikiyor (olculdu: 0xfd).
loads = []
hdr_v = None
for i in range(pn):
    o = elf_off + ph + i * 56
    pt, _, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
    if pt == 1:  # PT_LOAD
        loads.append((pv, pf, sm.get(i, elf_off + po)))
    elif pt == 0x6474E550:  # PT_GNU_EH_FRAME
        hdr_v = pv

if hdr_v is None:
    print("PT_GNU_EH_FRAME yok")
    sys.exit(1)


def v2f(v):
    for pv, pf, r in loads:
        if pv <= v < pv + pf:
            return r + (v - pv)
    return None


hdr_f = v2f(hdr_v)
if hdr_f is None:
    print(f"eh_frame_hdr vaddr 0x{hdr_v:x} hicbir PT_LOAD icinde degil")
    sys.exit(1)


def read_enc(off, enc, base_v):
    """Yalnizca pratikte gorulen kodlamalari destekler."""
    fmt = enc & 0x0F
    if fmt == 0x03:      # udata4
        v = struct.unpack_from("<I", data, off)[0]; n = 4
    elif fmt == 0x0B:    # sdata4
        v = struct.unpack_from("<i", data, off)[0]; n = 4
    elif fmt == 0x04:    # udata8
        v = struct.unpack_from("<Q", data, off)[0]; n = 8
    else:
        raise ValueError(f"desteklenmeyen kodlama 0x{enc:02x}")
    app = enc & 0x70
    if app == 0x30:      # datarel (eh_frame_hdr basina gore)
        v += base_v
    elif app == 0x10:    # pcrel
        v += base_v + (off - hdr_f)
    return v & 0xFFFFFFFFFFFFFFFF, n


p = hdr_f
ver, eh_enc, cnt_enc, tbl_enc = data[p], data[p + 1], data[p + 2], data[p + 3]
p += 4
_, n = read_enc(p, eh_enc, hdr_v); p += n
count, n = read_enc(p, cnt_enc, hdr_v); p += n

starts = []
for k in range(count):
    loc, n1 = read_enc(p, tbl_enc, hdr_v)
    _, n2 = read_enc(p + n1, tbl_enc, hdr_v)
    starts.append(loc)
    p += n1 + n2

starts.sort()
print(f"eh_frame_hdr: surum={ver} tablo_kodlama=0x{tbl_enc:02x} "
      f"{count} fonksiyon (RVA 0x{starts[0]:x} .. 0x{starts[-1]:x})")

if len(sys.argv) > 2:
    import bisect
    for a in sys.argv[2:]:
        rva = int(a, 0)
        i = bisect.bisect_right(starts, rva) - 1
        if i < 0:
            print(f"  0x{rva:x} -> tablonun disinda")
        else:
            end = starts[i + 1] if i + 1 < len(starts) else None
            span = f"0x{end - starts[i]:x}" if end else "?"
            print(f"  0x{rva:x} -> fonksiyon 0x{starts[i]:x}  "
                  f"[+0x{rva - starts[i]:x}, boyut ~{span}]")
