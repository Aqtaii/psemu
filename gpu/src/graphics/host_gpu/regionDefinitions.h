#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_

#include "common/common.h"

#include <bitset>

namespace Libs::Graphics {

constexpr uint64_t TRACKER_PAGE_SIZE    = 4ull * 1024ull;
constexpr uint64_t TRACKER_REGION_SIZE  = 4ull * 1024ull * 1024ull;
constexpr uint64_t TRACKER_ADDRESS_SIZE = 1ull << 47u;
constexpr size_t   TRACKER_REGION_PAGES = TRACKER_REGION_SIZE / TRACKER_PAGE_SIZE;

enum class DirtySource { Cpu, Gpu };
using RegionBits = std::bitset<TRACKER_REGION_PAGES>;

// KESKIN NISANCI ISARETI: PageManager'a vaddr=0 ile kimin geldigini bulmak
// icin RegionManager cagirma noktalari buraya kendi etiketini birakiyor.
// pageManager.cpp bu basligi dogrudan, regionManager.h ise regionDefinitions
// uzerinden goruyor; boylece tek degisken iki TU'da paylasiliyor.
inline thread_local const char* g_psemu_region_site = "?";

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
