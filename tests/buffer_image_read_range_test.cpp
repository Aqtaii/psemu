#include "graphics/host_gpu/renderer/bufferImageReadRanges.h"
#include <array>
#include <cstdio>
using namespace Libs::Graphics;
#define CHECK(x) do { if(!(x)) { std::printf("FAIL line %d\\n",__LINE__); return 1; } } while(0)
int main() {
 // Astro captured cache page 0x11e79268000+0x8000 overlaps only the last
 // 4 KiB of the sampled 8 MiB image. Writes in the following 28 KiB do not.
 constexpr uint64_t image=0x11e78a69000ull,bytes=0x800000,end=image+bytes;
 std::array<GpuWrittenBytes,1> outside{{{end,0x7000}}};
 CHECK(GpuWritesAreOutsideImage(image,bytes,outside));
 std::array<GpuWrittenBytes,1> last_byte{{{end-1,1}}};
 CHECK(!GpuWritesAreOutsideImage(image,bytes,last_byte));
 std::array<GpuWrittenBytes,1> crosses{{{end-1,2}}};
 CHECK(!GpuWritesAreOutsideImage(image,bytes,crosses));
 std::array<GpuWrittenBytes,2> mixed{{outside[0],{image,4}}};
 CHECK(!GpuWritesAreOutsideImage(image,bytes,mixed));
 std::array<GpuWrittenBytes,2> adjacent{{{image-4,4},outside[0]}};
 CHECK(GpuWritesAreOutsideImage(image,bytes,adjacent));
 CHECK(!GpuWritesAreOutsideImage(image,bytes,{}));
 CHECK(!GpuWritesAreOutsideImage(0,bytes,outside));
 CHECK(!GpuWritesAreOutsideImage(image,0,outside));
 CHECK(!GpuWritesAreOutsideImage(UINT64_MAX-1,4,outside));
 std::array<GpuWrittenBytes,1> invalid{{{UINT64_MAX-1,4}}};
 CHECK(!GpuWritesAreOutsideImage(image,bytes,invalid));
 invalid[0]={end,0};
 CHECK(!GpuWritesAreOutsideImage(image,bytes,invalid));
 std::puts("PASS: exact GPU writes, shared image edge pages, mixed writes and invalid ranges");
}
