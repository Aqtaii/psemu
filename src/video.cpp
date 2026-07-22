#include "video.h"
#include "logger.h"

#include <windows.h>
#include <mutex>
#include <sstream>
#include <vector>

// Pencere (user32) ve blit (gdi32) API'leri icin; build.bat'i degistirmeden
// baglanti kutuphanelerini burada belirtiyoruz.
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace Video {

namespace {

constexpr int kMaxBuffers = 16;

std::mutex   g_mutex;
HWND         g_hwnd            = nullptr;
HANDLE       g_window_thread   = nullptr;
bool         g_init_done       = false;

uint32_t     g_width           = 1920;
uint32_t     g_height          = 1080;
uint32_t     g_pitch_in_pixel  = 1920;
uint64_t     g_pixel_format    = 0;
uint32_t     g_tiling_mode     = 0;

void*        g_buffers[kMaxBuffers] = {};
uint64_t     g_flip_count      = 0;

// Kayitli framebuffer'i pencereye blit eder. Hem SubmitFlip'ten hem de
// periyodik "debug present"ten cagirilir.
void PresentBuffer(HWND hwnd, void* src, uint32_t w, uint32_t h, uint32_t pitch);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            // Pencereyi kapatmak emulasyonu da sonlandirsin
            LOG_INFO("[VIDEO] Pencere kapatildi, emulator sonlandiriliyor.");
            ExitProcess(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            // Tanimli bir durum goster: siyaha boya (aksi halde pencere
            // hic boyanmadigi icin BEYAZ/cop gorunuyordu).
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Pencere KENDI thread'inde olusturulup mesaj dongusu orada donmeli;
// oyun thread'leri VEH icinde bloke olabildigi icin ayri thread sart.
DWORD WINAPI WindowThreadProc(LPVOID) {
    const wchar_t* kClass = L"PsemuVideoOut";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // Oyun 4K isteyebiliyor; pencereyi ekrana sigacak sekilde kucult.
    // (Blit zaten StretchDIBits ile client alanina olceklendiriliyor.)
    uint32_t win_w = g_width;
    uint32_t win_h = g_height;
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    while (win_w > static_cast<uint32_t>(screen_w) * 8 / 10 ||
           win_h > static_cast<uint32_t>(screen_h) * 8 / 10) {
        win_w /= 2;
        win_h /= 2;
        if (win_w < 320 || win_h < 240) break;
    }

    RECT r = { 0, 0, static_cast<LONG>(win_w), static_cast<LONG>(win_h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, kClass, L"psemu - PS5",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        LOG_ERROR("[VIDEO] Pencere olusturulamadi!");
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hwnd = hwnd;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    std::stringstream ss;
    ss << "[VIDEO] Pencere acildi: " << win_w << "x" << win_h
       << " (oyun buffer'i " << g_width << "x" << g_height << ")";
    LOG_INFO(ss.str());

    // DEBUG PRESENT: oyun SubmitFlip cagirmasa bile framebuffer'da NE OLDUGUNU
    // gorebilmek icin periyodik olarak buffer #0'i ekrana bas. Boylece "oyun
    // hic ciziyor mu?" sorusunu tahminle degil gozle cevaplayabiliyoruz.
    SetTimer(hwnd, 1, 100, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == hwnd) {
            void* src; uint32_t w, h, pitch;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                src = g_buffers[0];
                w = g_width; h = g_height;
                pitch = g_pitch_in_pixel ? g_pitch_in_pixel : g_width;
            }
            if (src) PresentBuffer(hwnd, src, w, h, pitch);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void PresentBuffer(HWND hwnd, void* src, uint32_t w, uint32_t h, uint32_t pitch) {
    if (!hwnd || !src || !w || !h) return;

    // Ilk birkac sunumda buffer'in GERCEKTEN ne icerdigini raporla:
    // hepsi 0 ise oyun hic cizmemis demektir (GNM emulasyonu yok).
    static int s_report = 0;
    if (s_report < 3) {
        s_report++;
        const uint32_t* px = reinterpret_cast<const uint32_t*>(src);
        bool all_zero = true;
        for (int i = 0; i < 4096; i++) { if (px[i] != 0) { all_zero = false; break; } }
        std::stringstream ss;
        ss << "[VIDEO] Sunum #" << s_report << ": ilk 4 piksel = "
           << std::hex << px[0] << " " << px[1] << " " << px[2] << " " << px[3]
           << std::dec << (all_zero ? "  (ilk 4096 piksel TAMAMEN SIFIR -> oyun cizmemis)"
                                    : "  (SIFIR DEGIL -> icerik var!)");
        LOG_INFO(ss.str());
    }

    // 32-bit BGRA varsayimi (PS4/PS5 yaygin formati) - DIB ile ayni duzen.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(pitch);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(h); // negatif = top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(hwnd);
    if (!hdc) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc,
                  0, 0, rc.right - rc.left, rc.bottom - rc.top,
                  0, 0, static_cast<int>(w), static_cast<int>(h),
                  src, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, hdc);
}

} // namespace

void Init(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (width)  g_width  = width;
    if (height) g_height = height;
    if (g_init_done) return;
    g_init_done = true;
    g_window_thread = CreateThread(nullptr, 0, WindowThreadProc, nullptr, 0, nullptr);
}

void SetAttribute(uint32_t width, uint32_t height, uint32_t pitch_in_pixel,
                  uint64_t pixel_format, uint32_t tiling_mode) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (width)  g_width  = width;
        if (height) g_height = height;
        g_pitch_in_pixel = pitch_in_pixel ? pitch_in_pixel : g_width;
        g_pixel_format   = pixel_format;
        g_tiling_mode    = tiling_mode;
    }
    std::stringstream ss;
    ss << "[VIDEO] Buffer ozellikleri: " << width << "x" << height
       << " pitch=" << pitch_in_pixel
       << " format=0x" << std::hex << pixel_format
       << " tiling=" << std::dec << tiling_mode;
    LOG_INFO(ss.str());
}

void RegisterBuffer(int index, void* addr) {
    if (index < 0 || index >= kMaxBuffers) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_buffers[index] = addr;
    }
    std::stringstream ss;
    ss << "[VIDEO] Framebuffer #" << index << " kaydedildi: 0x"
       << std::hex << reinterpret_cast<uint64_t>(addr);
    LOG_INFO(ss.str());
}

uint64_t GetFlipCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_flip_count;
}

void Flip(int index) {
    HWND     hwnd;
    void*    src;
    uint32_t w, h, pitch;
    uint64_t n;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        n     = ++g_flip_count;
        hwnd  = g_hwnd;
        src   = (index >= 0 && index < kMaxBuffers) ? g_buffers[index] : nullptr;
        w     = g_width;
        h     = g_height;
        pitch = g_pitch_in_pixel ? g_pitch_in_pixel : g_width;
    }

    if (n <= 5) {
        std::stringstream ss;
        ss << "[VIDEO] Flip #" << n << " buffer=" << index
           << " src=0x" << std::hex << reinterpret_cast<uint64_t>(src);
        LOG_INFO(ss.str());
    }

    if (hwnd == nullptr || src == nullptr) return;
    PresentBuffer(hwnd, src, w, h, pitch);
}

} // namespace Video
