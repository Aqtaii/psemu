#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace Libs::Graphics {
enum class CapturePixelFormat { Bgra8, Rgba8, Bgr10, Rgb10 };
inline std::array<uint8_t,3> CapturePixelBgr(const uint8_t* pixel,CapturePixelFormat format) {
 if(format==CapturePixelFormat::Bgra8)return {pixel[0],pixel[1],pixel[2]};
 if(format==CapturePixelFormat::Rgba8)return {pixel[2],pixel[1],pixel[0]};
 uint32_t packed;std::memcpy(&packed,pixel,sizeof(packed));
 auto channel=[&](uint32_t shift){return uint8_t((((packed>>shift)&1023u)*255u+511u)/1023u);};
 const auto low=channel(0),green=channel(10),high=channel(20);
 return format==CapturePixelFormat::Bgr10 ? std::array<uint8_t,3>{low,green,high} :
                                          std::array<uint8_t,3>{high,green,low};
}
}