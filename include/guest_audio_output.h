#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
using GuestAudioRead = bool (*)(const void *, size_t);
using GuestAudioWrite = bool (*)(void *, size_t);
void GuestAudioRegisterContext(uint64_t context, uint32_t grains);
void GuestAudioDestroyContext(uint64_t context);
void GuestAudioPushPcm(uint64_t context, GuestAudioRead readable);
bool GuestAudioOutputCall(std::string_view name, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t *result,
                          GuestAudioRead readable, GuestAudioWrite writable);