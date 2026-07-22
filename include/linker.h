#pragma once
#include <cstdint>
#include <stddef.h>

class Linker {
public:
    // PLT (Procedure Linkage Table) taramasi yaparak PS5 kutuphanelerini (PRX)
    // bizim yazdigimiz HLE Stub fonksiyonlara yönlendirir.
    static void ResolveImports(uint8_t* base_addr, size_t text_size);
};
