#ifndef PSEMU_GFX_TRACE_H_
#define PSEMU_GFX_TRACE_H_
#include <cstdlib>
// Grafik yolundaki TANI isaretleri icin tek anahtar. VARSAYILAN KAPALI:
// bu yollar cok sicak ve printf+fflush tek basina oyunu takilir hale
// getirebiliyor (olculdu: Dreaming Sarah T+2'de durdu). PSEMU_GFX_TRACE=1
inline bool PsemuGfxTrace() {
    static const bool on = [] {
        const char* e = std::getenv("PSEMU_GFX_TRACE");
        return e != nullptr && e[0] == '1';
    }();
    return on;
}
#endif
