# psemu: dil tespiti izini surmek icin string arama.
# Soru: "SYSLANGCC" degerini KIM uretiyor? data.js (event sheet) mi, yoksa
# native kod (eboot.bin) mu? Native ise oradan hangi sistem cagrisiyla
# doldurdugunu bulup HLE'mizi duzeltebiliriz.
NAMES = ["SYSLANGCC", "Dictionary", "AJAX", "WebStorage", "LocalStorage",
         "SpriteFont", "langcode", "SYSLANG", "sceSystemServiceParamGetString",
         "sceSystemServiceParamGetInt", "lang"]

print("=== data.js ===")
d = open(r"D:\proje\psemu\PPSA02929-app0\data.js", "rb").read().decode("utf-8", "replace")
for n in NAMES:
    i = d.find(n)
    print(f"  {n:32} {'offset=' + str(i) if i >= 0 else 'YOK'}")

print()
print("=== eboot.bin ===")
b = open(r"D:\proje\psemu\PPSA02929-app0\eboot.bin", "rb").read()
for n in NAMES:
    i = b.find(n.encode())
    print(f"  {n:32} {'offset=0x%x' % i if i >= 0 else 'YOK'}")

# SYSLANGCC binary'de ise cevresini goster (yakin string'ler ipucu verir)
i = b.find(b"SYSLANGCC")
if i >= 0:
    print()
    print("SYSLANGCC cevresi (eboot.bin):")
    print(repr(b[max(0, i - 160):i + 160]))
