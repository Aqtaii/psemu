# CAGRI SAYIMI: oyunun GERCEKTEN cagirdigi fonksiyonlardan hangilerinin
# psemu'da karsiligi yok (yani varsayilan stub'a dusup RAX=0 donuyor).
#
# Neden: import kapsamasi (import_coverage.py) "ismi cozuluyor mu" der;
# bu ise "cagriliyor mu ve implemente mi" der. Cokme-cokme kovalamak yerine
# her asamada bu listeyi kapatmak cok daha hizli - liste her zaman SINIRLI.
#
# Kullanim: python stub_census.py <log dosyasi>
import re
import sys
from collections import Counter

log = sys.argv[1] if len(sys.argv) > 1 else "astro_log.txt"

called = Counter()
for line in open(log, encoding="utf-8", errors="replace"):
    m = re.search(r"\[PLT-HLE\] (\S+)", line)
    if m:
        called[m.group(1)] += 1

src = open("src/core.cpp", encoding="utf-8", errors="replace").read()

impl, stub = [], []
for name, n in called.most_common():
    # NID'i cozulmemis olanlar (icinde # var) zaten isimsiz
    if "#" in name:
        stub.append((name, n, "ISIM COZULEMEDI"))
        continue
    if f'readable_name == "{name}"' in src:
        impl.append((name, n))
    else:
        stub.append((name, n, "stub -> RAX=0"))

print(f"{log}: oyunun cagirdigi {len(called)} farkli fonksiyon")
print(f"  implemente : {len(impl)}")
print(f"  STUB       : {len(stub)}")
print("\n--- STUB olanlar (cagri sayisina gore) ---")
for name, n, why in stub:
    print(f"  {n:>6}x  {name:<45} {why}")
