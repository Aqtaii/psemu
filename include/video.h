#pragma once
#include <cstdint>

// ============================================================
// VideoOut sunum katmani
// ============================================================
// Oyun sceVideoOutRegisterBuffers2 ile bize KENDI framebuffer'larinin
// adreslerini verir; sceVideoOutSubmitFlip ile "sunu ekrana bas" der.
// Burada bir Win32 penceresi acip o bellegi dogrudan ekrana blit ediyoruz.
//
// NOT: Oyun bu buffer'lari GPU shader'lariyla dolduruyorsa (GNM emulasyonu
// olmadan) icerik bos/bozuk gorunur - bu beklenen durumdur. Amac once
// sunum zincirinin (pencere + flip) calistigini dogrulamaktir.
namespace Video {

// Pencereyi olusturur (ilk cagrida). Tekrar cagrilirsa boyut gunceller.
void Init(uint32_t width, uint32_t height);

// RegisterBuffers2'den gelen framebuffer bilgisi
void SetAttribute(uint32_t width, uint32_t height, uint32_t pitch_in_pixel,
                  uint64_t pixel_format, uint32_t tiling_mode);
void RegisterBuffer(int index, void* addr);

// SubmitFlip: verilen indeksteki buffer'i ekrana bas
void Flip(int index);

// Simdiye kadar yapilan flip sayisi (GetFlipStatus icin)
uint64_t GetFlipCount();

} // namespace Video
