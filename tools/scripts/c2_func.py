# psemu: bir C2 "Function" adinin TANIMINI (On function) ve CAGRILARINI ayirir.
#   tanim  : [470,35,...,[[1,[2,"<ad>"]]]]      (On function)
#   cagri  : [470,70,...,[[1,[2,"<ad>"]]],...]  (Call function)
import re
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")
name = sys.argv[1] if len(sys.argv) > 1 else "changeMenu"

defs, calls, others = [], [], []
for m in re.finditer(re.escape('"' + name + '"'), d):
    i = m.start()
    pre = d[max(0, i - 60):i]
    if re.search(r"\[470,35,", pre):
        defs.append(i)
    elif re.search(r"\[470,70,", pre):
        calls.append(i)
    else:
        others.append(i)

print(f"'{name}': tanim={len(defs)} cagri={len(calls)} diger={len(others)}")
print("  tanim offsetleri:", defs[:5])
print("  cagri offsetleri:", calls[:10])
print("  diger offsetleri:", others[:10])
for i in defs[:2]:
    print()
    print(f"--- TANIM @ {i} ---")
    print(d[max(0, i - 120):i + 520].replace("],[", "],\n["))
