# psemu: Launcher layout'undaki SpriteFont (tip 529) instance kaydini cikarir.
# C2 instance bicimi (kabaca):
#   [[x,y,z,w,h,angle,opacity,hotX,hotY,...],<tip>,<iid>,[...],[...],[plugin verisi]]
# Amac: nesne gorunur mu, opaklik/boyut makul mu, baslangic metni var mi?
import re

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

start = d.find('["Launcher",1280,720')
nxt = d.find('",1280,720', start + 20)
seg = d[start:nxt if nxt > 0 else start + 30000]

for m in re.finditer(r"\]\],(529),\d+,\[", seg):
    i = m.start()
    lo = seg.rfind("[[", max(0, i - 400), i)
    print("--- SpriteFont (529) instance kaydi ---")
    print(seg[lo:i + 700].replace("],[", "],\n["))
    break
else:
    print("529 instance bulunamadi")
