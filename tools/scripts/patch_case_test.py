# psemu TANI DENEYI (geri alinabilir):
# data.js'te fonksiyon TANIMI "MenuReload" (buyuk M), CAGRI ise "menuReload"
# (kucuk m). Construct2 fonksiyon adlarini harf-duyarSIZ eslestirir. Eger
# psemu'da bu eslesme bozuksa menu etiketleri hic yazilmaz.
# Bu script cagriyi "MenuReload" yaparak (tek bayt) hipotezi test eder.
#   python patch_case_test.py apply    -> yedek al + yamala
#   python patch_case_test.py restore  -> yedekten geri yukle
import shutil
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
BAK = PATH + ".orig"
mode = sys.argv[1] if len(sys.argv) > 1 else "status"

data = open(PATH, "rb").read()
call = b'"menuReload"'
defn = b'"MenuReload"'

print(f'cagri  {call.decode()}: {data.count(call)} adet')
print(f'tanim  {defn.decode()}: {data.count(defn)} adet')

if mode == "apply":
    if data.count(call) == 0:
        print("yamalanacak cagri yok (zaten yamalanmis olabilir)")
    else:
        shutil.copyfile(PATH, BAK)
        out = data.replace(call, defn)
        open(PATH, "wb").write(out)
        print(f"YAMALANDI: {data.count(call)} cagri -> MenuReload  (yedek: {BAK})")
elif mode == "restore":
    import os
    if os.path.exists(BAK):
        shutil.copyfile(BAK, PATH)
        os.remove(BAK)
        print("GERI YUKLENDI (yedek silindi)")
    else:
        print("yedek yok")
