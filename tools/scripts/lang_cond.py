# psemu: data.js (Construct2 event sheet) icinde "langcode" atamalarinin
# KOSULLARINI cikarir. Amac: oyunun dili neye gore sectigini bulmak
# (bos langcode -> menu event'i de calismiyor olabilir).
import re

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

idx = [m.start() for m in re.finditer(r'"langcode"', d)]
print("langcode gecis sayisi:", len(idx))
print()

# Her atamanin cevresini gosterir; onundeki kosul blogunu gormek icin genis pencere
for i in idx[:14]:
    lo = max(0, i - 420)
    print(f"---- offset {i} ----")
    print(d[lo:i + 90].replace("],[", "],\n   ["))
    print()
