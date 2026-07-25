# psemu: lang0.json (Construct2 c2array) icindeki "anahtar||deger" girdilerini
# listeler. Amac: loadDictionary'nin sozluge ekledigi anahtarlarin SIRASINI ve
# konumunu gormek - ozellikle menu etiketlerini tasiyan "menu0" nerede?
import re

PATH = r"D:\proje\psemu\PPSA02929-app0\lang0.json"
d = open(PATH, "rb").read().decode("utf-8", "replace")
n = len(d)

sep = "|" + "|"                      # "||" (kabuk yorumlamasindan kacinmak icin)
pat = re.compile(r'\[\[\"(.*?)' + re.escape(sep))
keys = pat.findall(d)

print("lang0.json boyut:", n, " girdi sayisi:", len(keys))
print()
for k in ("menu0", "menu1", "menu4", "menu5", "menu6"):
    needle = '"' + k + sep
    i = d.find(needle)
    order = [j for j, e in enumerate(keys) if e == k]
    if i >= 0:
        print(f"{k:7} offset={i:7d} ({100.0*i/n:5.1f}%)  girdi_sirasi={order}")
    else:
        print(f"{k:7} YOK")
print()
print("ilk 5 anahtar:", keys[:5])
print("son 5 anahtar:", keys[-5:])
