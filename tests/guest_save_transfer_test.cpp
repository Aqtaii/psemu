#include "guest_save_transfer.h"
#include <cassert>
#include <string>
int main() {
    assert(GuestSaveTransferStatus(1,"PPSA01325","savedata",false)==static_cast<int32_t>(0x809f0008u));
    assert(GuestSaveTransferStatus(1,"PPSA01325","savedata",true)==static_cast<int32_t>(0x809f0005u));
    assert(GuestSaveTransferStatus(-1,"PPSA01325","savedata",false)==static_cast<int32_t>(0x809f0011u));
    for (const auto bad : {"", ".", "..", "../data", "a/b", "a\\b", "D:foo"})
        assert(!GuestSaveComponent(bad,32));
    assert(!GuestSaveComponent(std::string(32,'a'),32));
    assert(!GuestSaveComponent(std::string("a\0b",3),32));
    assert(GuestSaveComponent("save-slot_1",32));
    assert(GuestSaveTransferStatus(1,"PPSA01325","../data",false)==static_cast<int32_t>(0x809f0000u));
}
