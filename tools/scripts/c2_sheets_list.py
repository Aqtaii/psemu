# psemu: data.js icindeki TUM event sheet tanimlarini (ad + offset) listeler ve
# verilen offsetlerin hangi sheet'e dustugunu soyler.
# Amac: menu yazan eventler (menuWriteLoop / varMenu) hangi sheet'te?
import re
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

# Sheet tanimlari genelde ["Ad",[ ... ]] biciminde ve ad harf/rakam/altcizgi
pat = re.compile(r'\["([A-Za-z_][A-Za-z0-9_ ]{2,30})",\[\[')
sheets = [(m.group(1), m.start()) for m in pat.finditer(d)]
# ayni ad birden fazla kez cikabilir; hepsini goster
print(f"aday sheet/blok sayisi: {len(sheets)}")
for name, off in sheets:
    print(f"  {off:8d}  {name}")

targets = [int(x) for x in sys.argv[1:]] or [1053611, 1057711, 1061555, 1066084, 1128031]
print()
print("=== hedef offsetler hangi bloga dusuyor ===")
for t in targets:
    prev = None
    for name, off in sheets:
        if off <= t:
            prev = (name, off)
        else:
            break
    print(f"  offset {t}: {prev}")
