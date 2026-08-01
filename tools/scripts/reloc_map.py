# RELATIVE relocation haritasi + vtable/RTTI cozumleyici.
#
# NEDEN: eboot'ta vtable'lar DISKTE SIFIR duruyor; gercek degerler yukleme
# aninda R_X86_64_RELATIVE relocation'lariyla yaziliyor. Bu yuzden bir vtable
# adresini dogrudan okumaya calisinca hep 0 goruluyor ve "typeinfo=0x0"
# ciktisi aliniyor (olculdu: vtable 0x88f5f78 -> tum slotlar 0x0).
#
# Ayrica ters yonde de yanilticiydi: RELA tablosunun ICINDEKI addend alanlari
# fonksiyon adresleri gibi gorunuyor, oyle ki "0x410870 su vtable'da geciyor"
# diye YANLIS sonuc cikariliyordu - oysa orasi relocation kaydiydi, vtable
# degil.
#
# Kullanim:
#   python reloc_map.py <eboot.bin>                 -> tablo ozeti
#   python reloc_map.py <eboot.bin> --vt 0x88f5f78  -> vtable + RTTI adi
#   python reloc_map.py <eboot.bin> --who 0x410870  -> bu adres hangi slot(lar)a yaziliyor
import struct
import sys

R_X86_64_RELATIVE = 8


def load(path):
    data = open(path, "rb").read()
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
    loads = []
    for i in range(pn):
        o = elf_off + ph + i * 56
        pt, _, po, pv, _, pf = struct.unpack_from("<IIQQQQ", data, o)
        if pt == 1:
            loads.append((pv, pf, sm.get(i, elf_off + po)))
    return data, loads


def make_v2f(loads):
    def v2f(v):
        for pv, pf, r in loads:
            if pv <= v < pv + pf:
                return r + (v - pv)
        return None
    return v2f


def build(path):
    """r_offset -> addend haritasi (yalnizca RELATIVE)."""
    data, loads = load(path)
    v2f = make_v2f(loads)
    # RELA tablosunu bul: 24 baytlik adimlarla r_info==8 olan uzun bir dizi.
    # Tablonun kendisi de bir PT_LOAD icinde; en uzun kesintisiz diziyi seciyoruz.
    best = (0, 0, 0)  # (uzunluk, dosya_ofs, adet)
    for pv, pf, r in loads:
        off = r
        end = r + pf - 24
        while off < end:
            if struct.unpack_from("<Q", data, off + 8)[0] == R_X86_64_RELATIVE:
                start = off
                n = 0
                while off < end and \
                        struct.unpack_from("<Q", data, off + 8)[0] == R_X86_64_RELATIVE:
                    off += 24
                    n += 1
                if n > best[0]:
                    best = (n, start, n)
            else:
                off += 8
    n, start, _ = best
    rel = {}
    for k in range(n):
        o = start + k * 24
        r_off, _, add = struct.unpack_from("<QQQ", data, o)
        rel[r_off] = add
    return data, loads, v2f, rel


def cstr(data, v2f, v):
    f = v2f(v)
    if f is None:
        return None
    e = data.find(b"\x00", f, f + 400)
    return data[f:e].decode("utf-8", "replace")


def main():
    path = sys.argv[1]
    data, loads, v2f, rel = build(path)
    print(f"{len(rel)} RELATIVE relocation cozuldu")

    args = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == "--vt":
            vt = int(args[i + 1], 0)
            i += 2
            top = rel.get(vt - 16, 0)
            ti = rel.get(vt - 8)
            print(f"vtable 0x{vt:x}: offset-to-top=0x{top:x} typeinfo=0x{ti:x}"
                  if ti else f"vtable 0x{vt:x}: typeinfo YOK (reloc kaydi yok)")
            if ti:
                nm = rel.get(ti + 8)
                print(f"  sinif adi = {cstr(data, v2f, nm)!r}" if nm else
                      "  ad isaretcisi reloc'ta yok")
            for s in range(12):
                v = rel.get(vt + s * 8)
                if v is not None:
                    print(f"  slot {s}: 0x{v:x}")
        elif args[i] == "--who":
            t = int(args[i + 1], 0)
            i += 2
            hits = [o for o, a in rel.items() if a == t]
            print(f"0x{t:x} su {len(hits)} adrese yaziliyor: " +
                  " ".join(f"0x{h:x}" for h in sorted(hits)[:16]))
        else:
            i += 1


if __name__ == "__main__":
    main()
