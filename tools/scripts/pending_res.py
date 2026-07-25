# psemu: oyunun "Background loading resource X" START/DONE ciftlerini eslestirir.
# Amac: menu oncesi ses/gorsel onyuklemesinde TAMAMLANMAYAN kaynak var mi?
# (Log satirlari birbirine karistigi icin isimleri uzantida kesiyoruz.)
import re
import sys

PATH = sys.argv[1] if len(sys.argv) > 1 else r"D:\proje\psemu\loader_log.txt"
txt = open(PATH, "r", encoding="utf-8", errors="replace").read()

EXT = r"(?:ogg|flac|wav|png|json|js)"
start = re.findall(r"\[START\] Background loading resource ([\w/\-\.]+?\." + EXT + r")", txt)
done = re.findall(r"\[DONE\]\s+Background loading resource ([\w/\-\.]+?\." + EXT + r")", txt)

s_set, d_set = set(start), set(done)
print(f"START benzersiz: {len(s_set)}   DONE benzersiz: {len(d_set)}")
pending = sorted(s_set - d_set)
print(f"TAMAMLANMAYAN: {len(pending)}")
for p in pending:
    print("  -", p)

# Uzantiya gore dagilim (hangi tur takiliyor?)
from collections import Counter
print()
print("tamamlanmayanlarin uzanti dagilimi:", Counter(p.rsplit(".", 1)[-1] for p in pending))
print("DONE olanlarin uzanti dagilimi   :", Counter(p.rsplit(".", 1)[-1] for p in d_set))
