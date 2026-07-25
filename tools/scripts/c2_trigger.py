# psemu: menu etiketlerini kuran C2 event'inin TETIKLEYICISINI izler.
# "TextReady" = AJAX istek etiketi; istek nerede yapiliyor, "On completed"
# nerede yakalaniyor ve ardindan hangi aksiyonlar var?
import re

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")


def dump(label, needle, before=260, after=420, limit=6):
    idx = [m.start() for m in re.finditer(re.escape(needle), d)]
    print(f"===== {label}: '{needle}' -> {len(idx)} gecis =====")
    for i in idx[:limit]:
        print(f"--- offset {i} ---")
        print(d[max(0, i - before):i + after].replace("],[", "],\n  ["))
        print()


dump("AJAX etiketi", '"TextReady"')
dump("loadDictionary cagrisi", '"loadDictionary"', before=200, after=300, limit=4)
