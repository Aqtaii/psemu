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
#include <cstdlib>

extern "C" void PsemuDumpPltTop();

namespace {

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_present_ticks{0};
std::atomic<uint64_t> g_submit_ticks{0};
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_textures{0};
std::atomic<uint64_t> g_tls_faults{0}; // fs: erisimlerinin VEH'e dusme sayisi
std::atomic<uint64_t> g_plt_calls{0};  // PLT hook dispatch sayisi
std::atomic<uint64_t> g_veh_cycles{0}; // VEH icinde harcanan GERCEK CPU dongusu

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

// core.cpp'deki VEH, her fs: (TLS) erisimi exception'a dustugunde cagirir.
// Donanim FS_BASE calisiyorsa bu sayac ~0 kalmali; buyukse TLS yolu darbogaz.
void PsemuMetricAddTlsFault() { g_tls_faults.fetch_add(1, std::memory_order_relaxed); }

void PsemuMetricAddPltCall() { g_plt_calls.fetch_add(1, std::memory_order_relaxed); }

// VEH handler'inin harcadigi GERCEK CPU dongusu (uyku sayilmaz - bkz. core.cpp
// VehTimer). ~4 GHz'de saniyede 4e9 dongu = bir cekirdegin tamami demektir.
void PsemuMetricAddVehCycles(unsigned long long cycles) {
	g_veh_cycles.fetch_add(cycles, std::memory_order_relaxed);
}

void PsemuMetricAddTextureCount() { g_textures.fetch_add(1, std::memory_order_relaxed); }

// Baslik metnini uretir. En fazla ~2 kez/saniye guncellenmesi icin cagiran
// tarafta zaman kontrolu yapilir (bkz. WindowUpdateTitle).
bool PsemuMetricFormat(char* buf, size_t buf_size) {
	static uint64_t last_qpc = 0, last_frames = 0, last_pres = 0, last_sub = 0, last_draws = 0,
	                last_tls = 0, last_plt = 0, last_veh = 0;

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
	const uint64_t t = g_tls_faults.load(std::memory_order_relaxed);
	const uint64_t pl = g_plt_calls.load(std::memory_order_relaxed);
	const uint64_t vn = g_veh_cycles.load(std::memory_order_relaxed);

	const uint64_t df = f - last_frames;
	const double   fps = (df > 0) ? df / dt : 0.0;
	const double   ms_per_tick = 1000.0 / static_cast<double>(QpcFreq());
	const double   pres_ms = (df > 0) ? (p - last_pres) * ms_per_tick / df : 0.0;
	const double   sub_ms  = (df > 0) ? (s - last_sub) * ms_per_tick / df : 0.0;
	const double   dpf     = (df > 0) ? static_cast<double>(d - last_draws) / df : 0.0;
	// TLS fault'lari saniye basina: kare hizindan bagimsiz olsun ki kare
	// gelmedigi (donma) anlarda da anlamli kalsin.
	const double   tls_ps  = (t - last_tls) / dt;
	const double   plt_ps  = (pl - last_plt) / dt;
	// VEH'in CPU maliyeti: saniyede kac milyar dongu. Tum threadlerin toplami,
	// yani "kac cekirdek dolusu CPU" olarak okunur (~4 GHz'de 1.0 = 1 cekirdek).
	const double   veh_gc  = ((vn - last_veh) / 1e9) / dt;

	std::snprintf(buf, buf_size,
	              "psemu - PS5   |  FPS %.1f  |  kare %.1f ms  |  present %.2f ms  |  "
	              "submit %.2f ms  |  cizim/kare %.0f  |  tex %llu  |  tls %.0f/sn  |  "
	              "plt %.0f/sn  |  veh %.2f Gdongu/sn",
	              fps, (fps > 0.0 ? 1000.0 / fps : 0.0), pres_ms, sub_ms, dpf,
	              static_cast<unsigned long long>(g_textures.load(std::memory_order_relaxed)),
	              tls_ps, plt_ps, veh_gc);

	last_qpc    = now;
	last_frames = f;
	last_pres   = p;
	last_sub    = s;
	last_draws  = d;
	last_tls    = t;
	last_plt    = pl;
	last_veh    = vn;

	// Ayni satiri loga da bas (birkac saniyede bir): pencere basligi yalnizca
	// oyun ayaktayken okunabiliyor, cokme sonrasi analiz icin log sart.
	static double s_since_log = 0.0;
	s_since_log += dt;
	if (s_since_log >= 3.0) {
		s_since_log = 0.0;
		std::printf("[PERF] %s\n", buf);
		std::fflush(stdout);
		// PLT-TOP dokumu buradan (render thread'i) aliniyor. core.cpp'deki
		// bagimsiz profil thread'i denendi ve iki kez erken takilmayla ortustu;
		// bu yol daha once sorunsuz calisiyordu. Yukleme asamasini gormuyoruz,
		// bedeli bu.
		// Varsayilan KAPALI: 4096 girdilik tarama + ~13 satir printf, render
		// thread'inde. Olcum yaparken ac: PSEMU_PLT_TOP=1
		static const bool s_plt_top = [] {
			const char* e = std::getenv("PSEMU_PLT_TOP");
			return e != nullptr && e[0] != '0';
		}();
		if (s_plt_top) {
			PsemuDumpPltTop();
		}
	}
	return true;
}

} // extern "C"
