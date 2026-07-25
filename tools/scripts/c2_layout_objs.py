# psemu: "Launcher" layout'unda hangi nesne TIPLERI ornekleniyor?
# C2 layout instance kaydi: [[x,y,z,w,h,...],<tip_index>,<iid>,...]
# Amac: SpriteFont (tip 529) Launcher layout'unda VAR MI? Yoksa SetText no-op olur
# ve hicbir sey cizilmez.
import re
from collections import Counter

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

# Launcher layout'u: ["Launcher",1280,720,true,"Launcher",...] ile baslar
start = d.find('["Launcher",1280,720')
# bir sonraki layout tanimina kadar (kaba): sonraki '],["' + tirnakli ad + ',<sayi>,<sayi>,'
nxt = d.find('",1280,720', start + 20)
end = nxt if nxt > 0 else start + 30000
seg = d[start:end]
print(f"Launcher layout bolgesi: {start}..{start+len(seg)} ({len(seg)} karakter)")

# instance kaliplari: "]],<tip>,<iid>,[" seklinde tip indeksi gelir
types = [int(m.group(1)) for m in re.finditer(r"\]\],(\d{1,4}),\d+,\[", seg)]
cnt = Counter(types)
print(f"bulunan instance sayisi: {len(types)}, benzersiz tip: {len(cnt)}")
print("en cok kullanilan tipler:", cnt.most_common(12))
print()
for t in (529, 468, 476, 548, 660):
    print(f"  tip {t} Launcher layout'unda: {'VAR (%d adet)' % cnt[t] if cnt[t] else 'YOK'}")
