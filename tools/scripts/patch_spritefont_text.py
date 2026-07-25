# psemu TANI DENEYI (geri alinabilir):
# Launcher layout'undaki SpriteFont (tip 529) instance'inin BASLANGIC METNINI
# doldurur. Boylece menuWriteLoop/SetText hic calismasa bile nesnede metin olur.
#   -> Metin EKRANDA gorunurse: cizim yolu SAGLAM, sorun SetText'in cagrilmamasi.
#   -> Yine gorunmezse : SpriteFont CIZIM yolu bozuk (texture/atlas/pipeline).
#   apply / restore
import os
import re
import shutil
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
BAK = PATH + ".orig3"
TEXT = "New game|Continue|Options"
mode = sys.argv[1] if len(sys.argv) > 1 else "status"

d = open(PATH, "rb").read().decode("utf-8", "surrogateescape")

# SpriteFont instance plugin verisi: ...],529,<iid>,[[-1]],[],[14,14,"<charset>","",1,0,...
m = re.search(r'(\]\],529,\d+,\[\[-1\]\],\[\],\[14,14,")(.*?)(","")', d, re.S)
if not m:
    print("SpriteFont instance kalibi bulunamadi")
    sys.exit(1)

charset_len = len(m.group(2))
print(f"bulundu: charset {charset_len} karakter, baslangic metni bos")

if mode == "apply":
    shutil.copyfile(PATH, BAK)
    new = d[:m.end(3) - 2] + TEXT + d[m.end(3) - 2:]
    #                  ^ son iki karakter '""' -> aralarina metni koy
    open(PATH, "wb").write(new.encode("utf-8", "surrogateescape"))
    print(f'YAMALANDI: baslangic metni = "{TEXT}"  (yedek: {BAK})')
elif mode == "restore":
    if os.path.exists(BAK):
        shutil.copyfile(BAK, PATH)
        os.remove(BAK)
        print("GERI YUKLENDI")
    else:
        print("yedek yok")
