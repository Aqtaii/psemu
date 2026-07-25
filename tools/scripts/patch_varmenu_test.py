# psemu TANI DENEYI (geri alinabilir):
# data.js'te  varMenu = Dictionary.Get("menu0")  ifadesini AYNI UZUNLUKTA bir
# literal string ile degistirir. Amac: varMenu'nun bos kalmasi mi (Get basarisiz)
# yoksa SpriteFont'un cizmemesi mi sorun?
#   apply   -> yedek al + yamala
#   restore -> geri yukle
import os
import shutil
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
BAK = PATH + ".orig2"
mode = sys.argv[1] if len(sys.argv) > 1 else "status"

raw = open(PATH, "rb").read()
OLD = b'[20,468,52,false,null,[[2,"menu0"]]]'
text = b"New game|Continue|Options"
NEW = b'[2,"' + text + b'"]'
pad = len(OLD) - len(NEW)
if pad > 0:
    NEW = b'[2,"' + text + b" " * pad + b'"]'

print(f"OLD ({len(OLD)}): {OLD.decode()}")
print(f"NEW ({len(NEW)}): {NEW.decode()}")
print(f"data.js icinde OLD sayisi: {raw.count(OLD)}")

if mode == "apply":
    if len(NEW) != len(OLD):
        print("HATA: uzunluklar esit degil, yamalanmadi")
    elif raw.count(OLD) == 0:
        print("yamalanacak ifade yok")
    else:
        shutil.copyfile(PATH, BAK)
        open(PATH, "wb").write(raw.replace(OLD, NEW))
        print(f"YAMALANDI ({raw.count(OLD)} yer). yedek: {BAK}")
elif mode == "restore":
    if os.path.exists(BAK):
        shutil.copyfile(BAK, PATH)
        os.remove(BAK)
        print("GERI YUKLENDI")
    else:
        print("yedek yok")
