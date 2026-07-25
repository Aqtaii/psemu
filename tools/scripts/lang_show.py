# psemu: lang0.json'dan belirli anahtarlarin DEGERINI gosterir.
# "menu0" = ana menu etiketleri mi? (varMenu = Dictionary.Get("menu0"))
import re

PATH = r"D:\proje\psemu\PPSA02929-app0\lang0.json"
d = open(PATH, "rb").read().decode("utf-8", "replace")
sep = "|" + "|"

for k in ("menu0", "menu4", "menu5", "menu6"):
    i = d.find('"' + k + sep)
    if i < 0:
        print(f"{k}: YOK")
        continue
    j = d.find('"', i + 1 + len(k) + len(sep))
    print(f"{k}: {d[i:j+1]}")
