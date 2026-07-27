// ============================================================================
// psemu: Kyty graphics/presentation/window/window.cpp'nin SDL'siz Win32
// karsiligi. Orijinal window.cpp'de 175 SDL referansi vardi ve cogu INPUT +
// SDL game-loop icindi (render icin gereksiz). Burada yalnizca renderer/videoOut
// tarafindan cagrilan pencere yasam-dongusu fonksiyonlarini sagliyoruz.
// Present mantigi swapchain.cpp'de (WindowPrepareFrame/WindowPresentFrame).
// window.cpp CMake'te DERLENMIYOR; bu dosya onun yerini aliyor.
// ============================================================================
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <thread>

#include "common/assert.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/videoOut.h" // VideoOutFlipWindow / Begin/EndVblank
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"
#include "libs/controller.h"

namespace Libs::Graphics {

// TANI: pencere thread'i vblank/flip'e girdiginde damgalanir, cikinca 0
// yapilir. Gozcu thread'i bunu izleyip takilmayi bildiriyor.
static std::atomic<uint64_t> g_flip_enter_ms{0};


// window.cpp'de tanimliydi; window.cpp derlenmedigi icin burada tanimliyoruz.
WindowContext* g_window_ctx = nullptr;

void WindowInit(uint32_t width, uint32_t height) {
	EXIT_IF(g_window_ctx != nullptr);

	g_window_ctx = new WindowContext;
	g_window_ctx->graphic_ctx.screen_width  = width;
	g_window_ctx->graphic_ctx.screen_height = height;
}

void WindowWaitForGraphicInitialized() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);
	while (!g_window_ctx->graphic_initialized) {
		g_window_ctx->graphic_initialized_condvar.Wait(&g_window_ctx->mutex);
	}
}

GraphicContext* WindowGetGraphicContext() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);
	return &g_window_ctx->graphic_ctx;
}

vk::SurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);
	return &g_window_ctx->surface_capabilities->capabilities;
}

// windowInternal.h bunlari bildiriyor; SDL icon -> no-op.
void WindowUpdateIcon() {}

// Pencere basligini CANLI PERFORMANS METRIKLERI ile gunceller (sol ustte
// gorunur). Her present'te cagriliyor; metrics.cpp saniyede en fazla 2 kez
// metin uretiyor, aksi halde hemen donuyor - yani maliyeti yok denecek kadar az.
extern "C" bool PsemuMetricFormat(char* buf, size_t buf_size);

void WindowUpdateTitle() {
	if (g_window_ctx == nullptr || g_window_ctx->window == nullptr) {
		return;
	}
	char text[256];
	if (!PsemuMetricFormat(text, sizeof(text))) {
		return;
	}
	wchar_t wide[256];
	const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide,
	                                  static_cast<int>(sizeof(wide) / sizeof(wide[0])));
	if (n > 0) {
		SetWindowTextW(g_window_ctx->window, wide);
	}
}

static LRESULT CALLBACK PsemuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_KEYDOWN:
		case WM_KEYUP: {
			bool down = (msg == WM_KEYDOWN);
			uint32_t button = 0;
			switch (wp) {
				case VK_SPACE:
				case VK_RETURN: button = ::Libs::Controller::PAD_BUTTON_CROSS; break;
				case VK_ESCAPE: button = ::Libs::Controller::PAD_BUTTON_OPTIONS; break;
				case VK_UP:     button = ::Libs::Controller::PAD_BUTTON_UP; break;
				case VK_DOWN:   button = ::Libs::Controller::PAD_BUTTON_DOWN; break;
				case VK_LEFT:   button = ::Libs::Controller::PAD_BUTTON_LEFT; break;
				case VK_RIGHT:  button = ::Libs::Controller::PAD_BUTTON_RIGHT; break;
				case 'Z':       button = ::Libs::Controller::PAD_BUTTON_SQUARE; break;
				case 'X':       button = ::Libs::Controller::PAD_BUTTON_TRIANGLE; break;
				case 'C':       button = ::Libs::Controller::PAD_BUTTON_CIRCLE; break;
				default: break;
			}
			// Kimlik KEYBOARD_CONTROLLER_ID olmali: GameController::Button
			// "m_active_id == id" olmayan cagrilari sessizce yok sayar. Burada
			// eskiden 1 gonderiliyordu, yani HICBIR tus oyuna ulasmiyordu.
			if (button != 0) {
				::Libs::Controller::ControllerButton(::Libs::Controller::KEYBOARD_CONTROLLER_ID,
				                                     button, down);
				static int s_n = 0;
				if (++s_n <= 20) {
					std::printf("[INPUT] tus vk=0x%02x -> pad butonu 0x%05x %s\n",
					            static_cast<unsigned>(wp), button, down ? "BASILDI" : "birakildi");
					std::fflush(stdout);
				}
			}
			return 0;
		}
		case WM_CLOSE:   DestroyWindow(hwnd); return 0;
		case WM_DESTROY: PostQuitMessage(0);  return 0;
		default:         break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void WindowCreate(WindowContext* ctx) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->window != nullptr);
	EXIT_IF(ctx->graphic_ctx.screen_width == 0);
	EXIT_IF(ctx->graphic_ctx.screen_height == 0);

	int width  = static_cast<int>(ctx->graphic_ctx.screen_width);
	int height = static_cast<int>(ctx->graphic_ctx.screen_height);

	// psemu tani: init'in ARALIKLI olarak kilitlendigi yeri saptamak icin
	// (launch'larin ~%50'si [INIT-MARK] WaitForGraphicInitialized'da donuyordu).
	#define WMARK(x) do { std::fprintf(stderr, "[WIN-MARK] " x "\n"); std::fflush(stderr); } while (0)
	WMARK("WindowCreate: RegisterClassExW oncesi");

	const wchar_t* cls_name = L"PsemuKytyWindow";
	WNDCLASSEXW    wc       = {};
	wc.cbSize        = sizeof(wc);
	wc.lpfnWndProc   = PsemuWndProc;
	wc.hInstance     = GetModuleHandleW(nullptr);
	wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	wc.lpszClassName = cls_name;
	RegisterClassExW(&wc);
	WMARK("WindowCreate: RegisterClassExW SONRASI; CreateWindowExW oncesi");

	// Oyun 4K isteyebilir; ekrana sigmasi icin pencereyi kucult (swapchain
	// yine tam cozunurlukte olur, DWM olcekler). Basitlik icin simdilik dogrudan.
	RECT r = {0, 0, width, height};
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowExW(0, cls_name, L"psemu - PS5", WS_OVERLAPPEDWINDOW,
	                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
	                            nullptr, nullptr, wc.hInstance, nullptr);
	EXIT_IF(hwnd == nullptr);

	WMARK("WindowCreate: CreateWindowExW SONRASI");
	ctx->window        = hwnd;
	ctx->window_hidden = false;
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);
	WMARK("WindowCreate: BITTI");
}

void WindowRun() {
	EXIT_IF(g_window_ctx == nullptr);

	std::fprintf(stderr, "[WIN-MARK] WindowRun: THREAD BASLADI (mutex oncesi)\n");
	std::fflush(stderr);
	g_window_ctx->mutex.Lock();
	{
		std::fprintf(stderr, "[WIN-MARK] WindowRun: mutex ALINDI\n");
		std::fflush(stderr);
		EXIT_IF(g_window_ctx->graphic_initialized);

		WindowCreate(g_window_ctx);
		std::fprintf(stderr, "[WIN-MARK] WindowRun: VulkanCreate oncesi\n");
		std::fflush(stderr);
		VulkanCreate(g_window_ctx);
		std::fprintf(stderr, "[WIN-MARK] WindowRun: VulkanCreate SONRASI\n");
		std::fflush(stderr);

		g_window_ctx->game = nullptr; // psemu: SDL WindowGame/game-loop yok

		g_window_ctx->graphic_initialized = true;
		g_window_ctx->graphic_initialized_condvar.Signal();
	}
	g_window_ctx->mutex.Unlock();

	GraphicsRenderCreateContext();

	// TANI GOZCUSU: pencere thread'i vblank/flip icinde takilirsa mesaj
	// pompasi durur ve Windows pencereyi "Yanit Vermiyor" yapar (kullanici
	// bunu gozlemledi). Takilmanin GERCEKTEN burada olup olmadigini
	// kanitlamak icin ayri bir thread damgayi izliyor.
	std::thread([] {
		bool reported = false;
		for (;;) {
			Sleep(1000);
			const uint64_t t = g_flip_enter_ms.load(std::memory_order_relaxed);
			if (t == 0) {
				reported = false;
				continue;
			}
			const uint64_t dt = GetTickCount64() - t;
			if (dt > 3000 && !reported) {
				reported = true;
				printf("[WIN-WATCH] pencere thread'i vblank/flip icinde %llu ms takildi "
				       "-> mesaj pompasi durdu (pencere 'Yanit Vermiyor')\n",
				       static_cast<unsigned long long>(dt));
				fflush(stdout);
			}
		}
	}).detach();

	// psemu: Kyty'nin SDL GameMainLoop'unun render kismini replike ediyoruz.
	// KRITIK: her frame VideoOutFlipWindow(0) cagrilmali â€” bu, flip queue'yu
	// drain edip WaitForNextVblank + FlipQueue::Flip -> WindowPresentFrame
	// (swapchain'e present) yapar. Onceki minimal GetMessage-only dongusu bunu
	// yapmiyordu, o yuzden flip kuyruga giriyor ama HIC sunulmuyordu (beyaz
	// pencere). Begin/EndVblank vblank event'lerini + sayacini yonetir.
	MSG msg;
	for (;;) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
			if (msg.message == WM_QUIT) {
				return;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		// TANI: pencere thread'i VideoOutFlipWindow icinde ctx->mutex'i
		// bekleyip mesaj pompasina donemiyor mu? Girisi damgaliyoruz;
		// asagidaki gozcu thread'i 3 saniyeden uzun surerse bildiriyor.
		// (Cagri hic donmezse "sonrasinda olc" yaklasimi ise yaramaz.)
		g_flip_enter_ms.store(GetTickCount64(), std::memory_order_relaxed);
		VideoOut::VideoOutBeginVblank();
		// PERF: eskiden micros=0 idi. VideoOutFlipWindow once vblank'i bekler,
		// SONRA flip kuyruguna bakar; timeout 0 ile kuyruk o an bossa hemen
		// false doner ve bir sonraki vblank'e (16.6 ms) kayardik. Oyun karesini
		// vblank'ten hemen sonra gonderdiginde bu, her karede bir periyot
		// kaybettiriyordu -> 60 Hz yerine ~30 fps. Kisa bir bekleme vererek
		// kare hazir olur olmaz sunuyoruz. (Olculdu: submit 0.011 ms, present
		// 0.7 ms; yani kayip zaman burada bekleniyordu.)
		// NOT: 15000 us denendi -> oyun COK ERKEN takildi (iki kosuda da ~536
		// satirda durdu). Sebep muhtemelen flip-queue kilidinin bu dongude
		// surekli mesgul edilip oyun thread'inin ac kalmasi. Bilinen calisan
		// deger (0) geri alindi; vsync kaybi baska yoldan cozulmeli.
		VideoOut::VideoOutFlipWindow(0);
		VideoOut::VideoOutEndVblank();
		g_flip_enter_ms.store(0, std::memory_order_relaxed);
	}
}

} // namespace Libs::Graphics
