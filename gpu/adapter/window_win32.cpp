// ============================================================================
// psemu: Kyty graphics/presentation/window/window.cpp'nin SDL'siz Win32
// karsiligi. Orijinal window.cpp'de 175 SDL referansi vardi ve cogu INPUT +
// SDL game-loop icindi (render icin gereksiz). Burada yalnizca renderer/videoOut
// tarafindan cagrilan pencere yasam-dongusu fonksiyonlarini sagliyoruz.
// Present mantigi swapchain.cpp'de (WindowPrepareFrame/WindowPresentFrame).
// window.cpp CMake'te DERLENMIYOR; bu dosya onun yerini aliyor.
// ============================================================================
#include <windows.h>

#include "common/assert.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"

namespace Libs::Graphics {

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

// windowInternal.h bunlari bildiriyor; SDL icon/title -> no-op / basit Win32.
void WindowUpdateIcon() {}
void WindowUpdateTitle() {}

static LRESULT CALLBACK PsemuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
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

	const wchar_t* cls_name = L"PsemuKytyWindow";
	WNDCLASSEXW    wc       = {};
	wc.cbSize        = sizeof(wc);
	wc.lpfnWndProc   = PsemuWndProc;
	wc.hInstance     = GetModuleHandleW(nullptr);
	wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	wc.lpszClassName = cls_name;
	RegisterClassExW(&wc);

	// Oyun 4K isteyebilir; ekrana sigmasi icin pencereyi kucult (swapchain
	// yine tam cozunurlukte olur, DWM olcekler). Basitlik icin simdilik dogrudan.
	RECT r = {0, 0, width, height};
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowExW(0, cls_name, L"psemu - PS5", WS_OVERLAPPEDWINDOW,
	                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
	                            nullptr, nullptr, wc.hInstance, nullptr);
	EXIT_IF(hwnd == nullptr);

	ctx->window        = hwnd;
	ctx->window_hidden = false;
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);
}

void WindowRun() {
	EXIT_IF(g_window_ctx == nullptr);

	g_window_ctx->mutex.Lock();
	{
		EXIT_IF(g_window_ctx->graphic_initialized);

		WindowCreate(g_window_ctx);
		VulkanCreate(g_window_ctx);

		g_window_ctx->game = nullptr; // psemu: SDL WindowGame/game-loop yok

		g_window_ctx->graphic_initialized = true;
		g_window_ctx->graphic_initialized_condvar.Signal();
	}
	g_window_ctx->mutex.Unlock();

	GraphicsRenderCreateContext();

	// psemu: Kyty'nin SDL GameMainLoop'u yerine minimal Win32 mesaj-pump.
	// Gercek render/present guest'in AGC flip cagrilariyla (videoOut ->
	// WindowPresentFrame) surulur; burasi pencereyi canli/duyarli tutar.
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

} // namespace Libs::Graphics
