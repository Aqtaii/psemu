#include "guest_detour.h"
#include <cstdio>

#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static void* handled_pointer = nullptr;
static void* seen_pointer = nullptr;
static __attribute__((sysv_abi)) bool FreeCallback(void* p) {
    seen_pointer = p;
    return p == handled_pointer;
}
int main() {
    const uint64_t site_address = 0x100000000;
    const uint64_t next = site_address + 5;
    int32_t offset = 123;
    CHECK(GuestDetour::RelativeCallOffset(site_address, next + INT32_MAX, offset) && offset == INT32_MAX);
    CHECK(GuestDetour::RelativeCallOffset(site_address, next - 0x80000000ULL, offset) && offset == INT32_MIN);
    CHECK(!GuestDetour::RelativeCallOffset(site_address, next + 0x80000000ULL, offset));
    CHECK(!GuestDetour::RelativeCallOffset(site_address, next - 0x80000001ULL, offset));
    CHECK(!GuestDetour::RelativeCallOffset(UINT64_MAX - 4, 0, offset));
    // Addresses from the actual failing Dreaming Sarah run.
    CHECK(!GuestDetour::RelativeCallOffset(0x1cbad997818, 0x1cf37e90000, offset));
    CHECK(!GuestDetour::RelativeCallOffset(0x1cbad9075c3, 0x1cf37e80000, offset));
    auto* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(image != nullptr);
    image[0] = 0xe8;
    const int32_t original = 27;
    std::memcpy(image + 1, &original, 4);
    uint8_t snapshot[5];
    std::memcpy(snapshot, image, 5);
    CHECK(!GuestDetour::InstallCall(image, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(image) + 0x400000000ULL), image + 32));
    CHECK(std::memcmp(snapshot, image, 5) == 0);
    auto* trampoline = GuestDetour::AllocateNear(image, 0x80);
    CHECK(trampoline != nullptr);
    CHECK(GuestDetour::RelativeCallOffset(reinterpret_cast<uint64_t>(image), reinterpret_cast<uint64_t>(trampoline), offset));
    CHECK(!GuestDetour::InstallCall(image, trampoline, image + 64));
    CHECK(std::memcmp(snapshot, image, 5) == 0);
    CHECK(GuestDetour::InstallCall(image, trampoline, image + 32));
    int32_t installed;
    std::memcpy(&installed, image + 1, 4);
    CHECK(installed == offset);
    image[0] = 0x90;
    CHECK(!GuestDetour::InstallCall(image, trampoline, image + 32));
    // Execute both branches with the real guest prologue and a synthetic body.
    std::memcpy(image, GuestDetour::FreePrologue, sizeof(GuestDetour::FreePrologue));
    const uint8_t body[] = {
        0x48, 0x89, 0xf8, // mov rax, rdi: validates argument preservation
        0x48, 0x83, 0xc4, 0x58, 0x5b, 0x41, 0x5c,
        0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3
    };
    std::memcpy(image + sizeof(GuestDetour::FreePrologue), body, sizeof(body));
    DWORD protection;
    CHECK(VirtualProtect(image, 0x10000, PAGE_EXECUTE_READWRITE, &protection));
    FlushInstructionCache(GetCurrentProcess(), image, 0x10000);
    auto* bridge = GuestDetour::InstallFreeOwnershipBridge(image, reinterpret_cast<void*>(&FreeCallback));
    CHECK(bridge != nullptr);
    using GuestFree = uintptr_t(__attribute__((sysv_abi)) *)(void*);
    auto release = reinterpret_cast<GuestFree>(image);
    handled_pointer = image + 0x4000;
    release(handled_pointer);
    CHECK(seen_pointer == handled_pointer);
    CHECK(release(image + 0x5000) == reinterpret_cast<uintptr_t>(image + 0x5000));
    CHECK(seen_pointer == image + 0x5000);
    handled_pointer = image + 0x4000;
    CHECK(release(nullptr) == 0);
    CHECK(GuestDetour::InstallFreeOwnershipBridge(image, reinterpret_cast<void*>(&FreeCallback)) == nullptr);
    CHECK(VirtualFree(bridge, 0, MEM_RELEASE));
    CHECK(VirtualFree(trampoline, 0, MEM_RELEASE));
    CHECK(VirtualFree(image, 0, MEM_RELEASE));
    std::puts("PASS: signed rel32 boundaries, actual crash addresses, near allocation and safe patch rejection");
    return 0;
}
