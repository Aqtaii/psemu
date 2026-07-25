# psemu: data.js icindeki Construct2 NESNE TIPI listesini sirayla cikarir ve
# event'lerde kullanilan indeksleri (or. 660 = dil kaynagi, 468 = menu sozlugu)
# isimlendirir. Kayit bicimi: ["tNNN", pluginId, ...]
import re

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

# ["t123",<pluginId>,...  seklindeki tip kayitlarini sirayla topla
pat = re.compile(r'\["(t\d+)",(\d+),')
types = [(m.group(1), int(m.group(2)), m.start()) for m in pat.finditer(d)]
print("bulunan tip kaydi:", len(types))

# Plugin id -> isim tahmini (C2 standart pluginleri; kesin degil, ipucu icin)
HINT = {1: "Sprite?", 9: "SpriteFont?", 15: "TiledBackground?",
        20: "Text?", 24: "Dictionary?", 25: "Array?", 43: "AJAX?"}


def show(idx):
    if 0 <= idx < len(types):
        t, pid, off = types[idx]
        print(f"  indeks {idx}: {t}  pluginId={pid}  {HINT.get(pid,'')}  (offset {off})")
        print("    baglam:", d[max(0, off - 60):off + 160].replace("\n", " "))
    else:
        print(f"  indeks {idx}: LISTE DISI (toplam {len(types)})")


for i in (468, 655, 657, 660):
    show(i)
    print()
