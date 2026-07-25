# psemu: bir event sheet bolgesinin DIGER sheet'leri include edip etmedigine bakar.
# Construct2'de "Include event sheet" aksiyonu sheet adini string olarak tasir.
# Amac: "Launcher" sheet'i menu mantigini iceren "menu" sheet'ini dahil ediyor mu?
SHEETS = {
    "Player": 738856, "Camera": 903110, "portas": 912639, "FalasEEventos": 930811,
    "MenuPausa": 1031259, "menu": 1050939, "volcano_basic": 1128743,
    "menuinitial": 1141644, "GameplayTest": 1144310, "forest_basic": 1144764,
    "Elevator": 1156095, "undergroundpass": 1159551, "Launcher": 1168328,
    "Sons": 1176062, "fireflies": 1212787, "Achievements": 1214574,
    "Ending": 1218887, "fader": 1232939, "SuperGlobal": 1233959,
}

PATH = r"D:\proje\psemu\PPSA02929-app0\data.js"
d = open(PATH, "rb").read().decode("utf-8", "replace")

order = sorted(SHEETS.items(), key=lambda kv: kv[1])


def region(name):
    off = SHEETS[name]
    nxt = None
    for n, o in order:
        if o > off:
            nxt = o
            break
    return d[off:nxt if nxt else off + 20000]


for target in ("Launcher", "menuinitial"):
    seg = region(target)
    print(f'=== "{target}" sheet ({len(seg)} karakter) icinde gecen sheet adlari ===')
    for name in SHEETS:
        if name == target:
            continue
        c = seg.count('"' + name + '"')
        if c:
            print(f"   {name}: {c}")
    print()
