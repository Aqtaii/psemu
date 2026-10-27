#pragma once
#include <cstdint>
// IEEE binary32 classification uses only bits; do not quiet sNaNs or alter MXCSR.
constexpr int GuestFloatIsFinite(uint32_t bits) {return (bits&0x7f800000u)!=0x7f800000u;}
constexpr int GuestFloatIsNaN(uint32_t bits) {return (bits&0x7fffffffu)>0x7f800000u;}
constexpr int GuestFloatIsInf(uint32_t bits) {return (bits&0x7fffffffu)==0x7f800000u;}
