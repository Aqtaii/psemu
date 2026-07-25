// ============================================================================
// psemu: CANLI PERFORMANS METRIKLERI
// ----------------------------------------------------------------------------
// Olcumleri toplayip pencere BASLIGINDA (sol ust) gosterir. Vulkan icine metin
// cizmek buyuk is oldugu icin baslik cubugu kullaniliyor: bedava, her present'te
// guncelleniyor ve oyunu yavaslatmiyor.
// Gosterilenler:
//   FPS      : son ~0.5 saniyedeki gercek kare hizi
//   present  : WindowPresentFrame suresi (acquire+blit+present), kare basina ort.
//   submit   : PM4 komut tamponu isleme suresi, kare basina ort.
//   draw     : kare basina cizim cagrisi
//   tex      : olusturulmus texture sayisi (sizinti/yeniden olusturma takibi)
// ============================================================================
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_present_ticks{0};
std::atomic<uint64_t> g_submit_ticks{0};
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_textures{0};

int64_t QpcFreq() {
	static int64_t f = [] {
		LARGE_INTEGER x;
		QueryPerformanceFrequency(&x);
		return static_cast<int64_t>(x.QuadPart);
	}();
	return f;
}

} // namespace

extern "C" {

void PsemuMetricAddPresent(uint64_t ticks) {
	g_present_ticks.fetch_add(ticks, std::memory_order_relaxed);
	g_frames.fetch_add(1, std::memory_order_relaxed);
}

void PsemuMetricAddSubmit(uint64_t ticks) {
	g_submit_ticks.fetch_add(ticks, std::memory_order_relaxed);
}

void PsemuMetricAddDraw() { g_draws.fetch_add(1, std::memory_order_relaxed); }

void PsemuMetricAddTextureCount() { g_textures.fetch_add(1, std::memory_order_relaxed); }

// Baslik metnini uretir. En fazla ~2 kez/saniye guncellenmesi icin cagiran
// tarafta zaman kontrolu yapilir (bkz. WindowUpdateTitle).
bool PsemuMetricFormat(char* buf, size_t buf_size) {
	static uint64_t last_qpc = 0, last_frames = 0, last_pres = 0, last_sub = 0, last_draws = 0;

	LARGE_INTEGER now_li;
	QueryPerformanceCounter(&now_li);
	const uint64_t now = static_cast<uint64_t>(now_li.QuadPart);
	if (last_qpc == 0) {
		last_qpc = now;
		return false;
	}
	const double dt = static_cast<double>(now - last_qpc) / static_cast<double>(QpcFreq());
	if (dt < 0.5) {
		return false; // saniyede en fazla 2 guncelleme
	}

	const uint64_t f = g_frames.load(std::memory_order_relaxed);
	const uint64_t p = g_present_ticks.load(std::memory_order_relaxed);
	const uint64_t s = g_submit_ticks.load(std::memory_order_relaxed);
	const uint64_t d = g_draws.load(std::memory_order_relaxed);

	const uint64_t df = f - last_frames;
	const double   fps = (df > 0) ? df / dt : 0.0;
	const double   ms_per_tick = 1000.0 / static_cast<double>(QpcFreq());
	const double   pres_ms = (df > 0) ? (p - last_pres) * ms_per_tick / df : 0.0;
	const double   sub_ms  = (df > 0) ? (s - last_sub) * ms_per_tick / df : 0.0;
	const double   dpf     = (df > 0) ? static_cast<double>(d - last_draws) / df : 0.0;

	std::snprintf(buf, buf_size,
	              "psemu - PS5   |  FPS %.1f  |  kare %.1f ms  |  present %.2f ms  |  "
	              "submit %.2f ms  |  cizim/kare %.0f  |  tex %llu",
	              fps, (fps > 0.0 ? 1000.0 / fps : 0.0), pres_ms, sub_ms, dpf,
	              static_cast<unsigned long long>(g_textures.load(std::memory_order_relaxed)));

	last_qpc    = now;
	last_frames = f;
	last_pres   = p;
	last_sub    = s;
	last_draws  = d;
	return true;
}

} // extern "C"
