# psemu TEST (geri alinabilir): oyunu MENUYU ATLAYIP dogrudan bir oyun-ici
# layout'ta baslatir. C2 data.js'te baslangic layout'u project[1]'dir:
#     {"project": [null,"Launcher", ...
# ONEMLI: dosya BOYUTU ayni kalmali - oyun/psemu dosyayi SABIT boyutla okuyor
# (buyuyunce JSON kesilip __cxa_throw uretiyor). Bu yuzden kisa ismi JSON'da
# gecerli olan bosluklarla dolduruyoruz.
#   apply <LayoutAdi>   /   restore
import os
import shutil
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
BAK = PATH + ".origSL"
mode = sys.argv[1] if len(sys.argv) > 1 else "status"
target = sys.argv[2] if len(sys.argv) > 2 else "M0T0"

raw = open(PATH, "rb").read()
ANCHOR = b'{"project": [null,'
i = raw.find(ANCHOR)
if i < 0:
    print("project basligi bulunamadi")
    sys.exit(1)
j = i + len(ANCHOR)
end = raw.find(b",", j)
cur = raw[j:end]
print(f"mevcut baslangic layout kaydi: {cur.decode()} ({len(cur)} byte)")

new = b'"' + target.encode() + b'"'
if len(new) > len(cur):
    print(f"HATA: '{target}' cok uzun ({len(new)} > {len(cur)})")
    sys.exit(1)
new = new + b" " * (len(cur) - len(new))   # JSON'da bosluk gecerli

if mode == "apply":
    shutil.copyfile(PATH, BAK)
    out = raw[:j] + new + raw[end:]
    assert len(out) == len(raw), "boyut degisti!"
    open(PATH, "wb").write(out)
    print(f"YAMALANDI -> {new.decode()!r}  (boyut ayni: {len(out)})  yedek: {BAK}")
elif mode == "restore":
    if os.path.exists(BAK):
        shutil.copyfile(BAK, PATH)
        os.remove(BAK)
        print("GERI YUKLENDI")
    else:
        print("yedek yok")
