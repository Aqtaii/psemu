# psemu TANI DENEYI v2 (BYTE-ESIT, geri alinabilir):
# SpriteFont instance'inin baslangic metnini doldurur AMA dosya boyutunu
# degistirmez: charset'ten 25 ASCII karakter cikarilir, 25 karakterlik metin
# eklenir. (v1'de dosya buyuyunce oyun ESKI boyutu okuyup JSON'u kesti ->
# __cxa_throw. Bu psemu tarafinda ayri bir bug; testi etkilemesin diye byte-esit.)
# Not: charset'ten karakter cikarmak glyph indekslerini kaydirir; metin YANLIS
# glyph'lerle cizilebilir - bu test icin onemli degil, amac "hic ciziyor mu".
import os
import re
import shutil
import sys

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
BAK = PATH + ".orig4"
TEXT = "New game|Continue|Options"          # 25 karakter
DROP = "0123456789.,;:?!-_~#&()[]"          # 24 karakter... asagida dogrulaniyor
mode = sys.argv[1] if len(sys.argv) > 1 else "status"

d = open(PATH, "rb").read().decode("utf-8", "surrogateescape")
m = re.search(r'(\]\],529,\d+,\[\[-1\]\],\[\],\[14,14,")(.*?)(",")(")', d, re.S)
if not m:
    print("kalip bulunamadi")
    sys.exit(1)

charset = m.group(2)
print(f"charset: {len(charset)} karakter, metin uzunlugu: {len(TEXT)}")

# TEXT'te GECMEYEN ASCII karakterleri charset'ten sec (byte-esit olmali)
avail = [c for c in charset if ord(c) < 128 and c not in TEXT]
need = len(TEXT)
if len(avail) < need:
    print(f"yeterli ASCII yok ({len(avail)} < {need})")
    sys.exit(1)
drop = set(avail[:need])
new_charset = "".join(c for c in charset if c not in drop)
print(f"cikarilan: {''.join(sorted(drop))!r} -> yeni charset {len(new_charset)}")

if mode == "apply":
    shutil.copyfile(PATH, BAK)
    new = d[:m.start(2)] + new_charset + m.group(3) + TEXT + d[m.end(4) - 1:]
    old_b = d.encode("utf-8", "surrogateescape")
    new_b = new.encode("utf-8", "surrogateescape")
    print(f"eski byte: {len(old_b)}  yeni byte: {len(new_b)}  fark: {len(new_b)-len(old_b)}")
    if len(new_b) != len(old_b):
        print("UYARI: boyut esit degil, yine de yaziliyor (test)")
    open(PATH, "wb").write(new_b)
    print(f'YAMALANDI: baslangic metni = "{TEXT}" (yedek: {BAK})')
elif mode == "restore":
    if os.path.exists(BAK):
        shutil.copyfile(BAK, PATH)
        os.remove(BAK)
        print("GERI YUKLENDI")
    else:
        print("yedek yok")
