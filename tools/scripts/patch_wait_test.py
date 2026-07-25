# psemu TANI DENEYI (byte-esit, geri alinabilir):
# changeMenu fonksiyonundaki "Wait 0.1" aksiyonunu "Wait 0.0" yapar.
#   changeMenu: ... Wait 0.1 ; Function.Call("menuReload") ; ...
# Eger menu metni bundan sonra CIKARSA -> C2 zamanlayici beklemesi (Wait)
# psemu'da cozulmuyor demektir (gercek bir zamanlama bug'i).
# Aksiyon benzersiz ID ile bulunur: [-1,88,null,1217794483571951,false,[[0,[1,0.1]]]]
import os
import shutil
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
BAK = PATH + ".orig5"
OLD = b'[-1,88,null,1217794483571951,false,[[0,[1,0.1]]]]'
NEW = b'[-1,88,null,1217794483571951,false,[[0,[1,0.0]]]]'
mode = sys.argv[1] if len(sys.argv) > 1 else "status"

raw = open(PATH, "rb").read()
print(f"hedef aksiyon bulundu: {raw.count(OLD)} adet  (byte fark: {len(NEW)-len(OLD)})")

if mode == "apply":
    if raw.count(OLD) == 0:
        print("bulunamadi")
    else:
        shutil.copyfile(PATH, BAK)
        open(PATH, "wb").write(raw.replace(OLD, NEW))
        print(f"YAMALANDI (yedek: {BAK})")
elif mode == "restore":
    if os.path.exists(BAK):
        shutil.copyfile(BAK, PATH)
        os.remove(BAK)
        print("GERI YUKLENDI")
    else:
        print("yedek yok")
