#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
using GuestAvRead = bool (*)(const void *, size_t);
using GuestAvWrite = bool (*)(void *, size_t);
bool GuestAvPlayerCall(std::string_view name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t *result, GuestAvRead read, GuestAvWrite write);
