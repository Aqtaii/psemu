#include "graphics/host_gpu/hostMemory.h"

#include <cinttypes>
#include <cstdio>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace Libs::Graphics {
namespace {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool IsAccessible(DWORD protect, HostMemoryAccess access) {
	constexpr DWORD blocked  = PAGE_NOACCESS | PAGE_GUARD;
	constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
	                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if (access == HostMemoryAccess::Mapped) {
		return (protect & PAGE_GUARD) == 0;
	}
	return (protect & blocked) == 0 && (protect & readable) != 0;
}
#endif

} // namespace

// Bolge yuruyusu ust siniri. 262144 tur ~1 GB'lik sayfa-granuler alani
// kapsar; gercek kullanimda (bitisik yuzeyler) birkac tur yeter, yani bu
// yalnizca patolojik parcalanmaya karsi bir emniyet supabi.
static constexpr uint64_t KYTY_HOSTMEM_MAX_REGIONS = 262144;

bool HostMemoryQueryRange(uint64_t addr, uint64_t requested_size, HostMemoryAccess access,
                          uint64_t* accessible_size) {
	if (accessible_size == nullptr) {
		return false;
	}
	*accessible_size = 0;
	if (addr == 0 || requested_size == 0) {
		return false;
	}

	const auto end     = UINT64_MAX - addr < requested_size ? UINT64_MAX : addr + requested_size;
	uint64_t   current = addr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// TANI/SINIR: bu yuruyus BOLGE BOLGE ilerliyor. Bitisik bellekte 1-2 tur
	// surer, AMA psemu misafir sayfalarini HATA ANINDA tek tek commit ediyor;
	// boyle bir alanda her sayfa AYRI BIR BOLGE olur ve buyuk bir aralik
	// milyonlarca VirtualQuery demektir. Ust siniri yoktu.
	//
	// Aralikli erken takilmanin watchdog imzasi tam bunu gosteriyor:
	//   RIP=<ntdll> RCX=0xffffffffffffffff (NtQueryVirtualMemory'nin surec
	//   tanitici argumani), RDX=<misafir adres> ve adres her orneklemede
	//   MB'larca ILERLIYOR. Yani surec asili degil, dev bir araligi tariyor.
	//   Cagiran yigin: Agc::Dispatch -> EnsureKytyGraphicsInit -> thread::join
	//
	// Once OLCUYORUZ: esik asilirsa durumu basiyoruz. Sinir asilirsa o ana
	// kadar erisilebilir bulunan miktarla donuyoruz (kismi sonuc), boylece
	// surec sonsuza kadar taramaz.
	uint64_t iters = 0;
	while (current < end) {
		if (++iters > KYTY_HOSTMEM_MAX_REGIONS) {
			static uint32_t s_n = 0;
			if (s_n++ < 16) {
				std::printf("[HOSTMEM-SINIR] QueryRange(addr=0x%" PRIx64 ", size=0x%" PRIx64
				            ") %" PRIu64 " bolgede sinira takildi; erisilebilir=0x%" PRIx64 "\n",
				            addr, requested_size, iters, current - addr);
				std::fflush(stdout);
			}
			break;
		}
		MEMORY_BASIC_INFORMATION region {};
		if (::VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(current)), &region,
		                   sizeof(region)) == 0) {
			break;
		}
		const auto begin  = reinterpret_cast<uint64_t>(region.BaseAddress);
		auto       finish = begin + region.RegionSize;
		if (finish < begin) {
			finish = UINT64_MAX;
		}
		if (finish <= current || (region.State & MEM_COMMIT) == 0 ||
		    !IsAccessible(region.Protect, access)) {
			break;
		}
		current = finish < end ? finish : end;
	}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	auto* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) {
		return false;
	}
	char line[1024] = {};
	while (current < end && std::fgets(line, sizeof(line), maps) != nullptr) {
		uint64_t begin          = 0;
		uint64_t finish         = 0;
		char     permissions[5] = {};
		if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &begin, &finish, permissions) != 3 ||
		    finish <= current) {
			continue;
		}
		if (begin > current) {
			break;
		}
		const bool allowed = access == HostMemoryAccess::Mapped || permissions[0] == 'r';
		if (!allowed) {
			break;
		}
		current = finish < end ? finish : end;
	}
	std::fclose(maps);
#else
	(void)access;
#endif

	*accessible_size = current - addr;
	return *accessible_size != 0;
}

bool HostMemoryQueryReadable(uint64_t addr, uint64_t requested_size, uint64_t* readable_size) {
	return HostMemoryQueryRange(addr, requested_size, HostMemoryAccess::Read, readable_size);
}

bool HostMemoryIsReadable(uint64_t addr) {
	return HostMemoryRangeIsReadable(addr, 1);
}

bool HostMemoryRangeIsReadable(uint64_t addr, uint64_t size) {
	if (addr == 0 || size == 0 || UINT64_MAX - addr < size) {
		return false;
	}
	uint64_t readable_size = 0;
	return HostMemoryQueryReadable(addr, size, &readable_size) && readable_size >= size;
}

} // namespace Libs::Graphics
