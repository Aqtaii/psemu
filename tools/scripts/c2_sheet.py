# psemu: data.js icindeki EVENT SHEET tanimlarini bulur.
# C2 bicimi: event sheet listesi ["SheetAdi",[ ...eventler... ]] seklindedir.
# Amac: "Launcher" sheet'inin ilk eventlerini gorup layout gecisini bulmak.
import re
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

want = sys.argv[1] if len(sys.argv) > 1 else "Launcher"

# sheet tanimi: ["Ad",[[0,  -> event dizisi hemen ardindan gelir
pat = re.compile(r'\["' + re.escape(want) + r'",\[\[0,')
hits = [m.start() for m in pat.finditer(d)]
print(f'"{want}" sheet tanimi bulundu: {len(hits)} -> {hits[:5]}')
for i in hits[:2]:
    print(f"--- sheet @ {i} ---")
    print(d[i:i + 1400].replace("],[", "],\n["))
    print()

if not hits:
    # alternatif: sheet adi + herhangi bir dizi
    pat2 = re.compile(r'\["' + re.escape(want) + r'",\[')
    hits2 = [m.start() for m in pat2.finditer(d)]
    print("alternatif eslesmeler:", hits2[:10])
    for i in hits2[:3]:
        print(f"--- alt @ {i} ---")
        print(d[i:i + 700].replace("],[", "],\n["))
        print()
