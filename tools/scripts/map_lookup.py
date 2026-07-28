# loader.map (MSVC linker MAP) kullanarak "loader+0xRVA" adreslerini fonksiyon
# adina cevirir. Watchdog/backtrace ciktisini okunur hale getirmek icin.
#
# Neden MAP: Release yapisi eslesen bir PDB uretmiyor ve optimizasyon
# seviyesini degistirmek riskli (bkz. CMakeLists notu: -O2 emulatoru bozuyor).
# MAP duz metindir ve ek arac gerektirmez.
#
# Kullanim:
#   python map_lookup.py loader.map 0x582008 0x4b3378 ...
#   python map_lookup.py loader.map --line "  [stack ...] loader+0x14a0 loader+0x1406 ..."
import re
import sys

path = sys.argv[1]
args = sys.argv[2:]

if args and args[0] == "--line":
    rvas = [int(x, 16) for x in re.findall(r"loader\+0x([0-9a-fA-F]+)", " ".join(args[1:]))]
else:
    rvas = [int(a, 0) for a in args]

base = None
syms = []  # (rva, name)
with open(path, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        if base is None:
            m = re.search(r"Preferred load address is\s+([0-9a-fA-F]+)", line)
            if m:
                base = int(m.group(1), 16)
                continue
        # " 0001:00000000  ?name  0000000140001000  f  obj"
        m = re.match(r"\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\s", line)
        if m and base is not None:
            va = int(m.group(2), 16)
            if va >= base:
                syms.append((va - base, m.group(1)))

syms.sort()
print(f"{len(syms)} sembol, taban 0x{base:x}\n")


def find(rva):
    lo, hi, best = 0, len(syms) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= rva:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


for r in rvas:
    s = find(r)
    if s is None:
        print(f"  0x{r:<8x} -> (bulunamadi)")
    else:
        print(f"  0x{r:<8x} -> {s[1]}  [+0x{r - s[0]:x}]")
