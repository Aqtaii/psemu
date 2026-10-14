#pragma once
struct GuestHdrToneMapLuminance {float full_frame;float maximum;float minimum;};
static_assert(sizeof(GuestHdrToneMapLuminance)==12);
// Match the existing Kyty SystemService SDR/HDR baseline.
inline constexpr GuestHdrToneMapLuminance GuestHdrToneMapDefaults(){return {80.0f,1000.0f,0.0f};}