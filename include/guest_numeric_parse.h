#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
// Guest long is 64-bit; avoid Windows' 32-bit strtol/atol result width.
inline int64_t GuestDecimalInteger(const char* text) {
 return std::strtoll(text,nullptr,10);
}
inline double GuestDecimalFloat(const char* text) {
 return std::strtod(text,nullptr);
}
// Captured Astro XML caller at RVA0x29b856 uses exactly "%x" and uint32_t*.
inline int GuestScanHex32(const char* text,uint32_t* destination) {
 if(!text || !destination)return EOF;
 unsigned int value=0;
 const int assigned=std::sscanf(text,"%x",&value);
 if(assigned==1)*destination=static_cast<uint32_t>(value);
 return assigned;
}

// Captured resident_room.xml caller at RVA0x2846ce uses "%d" and int32_t*.
inline int GuestScanDecimal32(const char* text,int32_t* destination) {
 if(!text || !destination)return EOF;
 int value=0;
 const int assigned=std::sscanf(text,"%d",&value);
 if(assigned==1)*destination=static_cast<int32_t>(value);
 return assigned;
}

inline uint64_t GuestUnsignedInteger(const char* text,char** end,int base) {
 return std::strtoull(text,end,base);
}
