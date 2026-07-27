// ============================================================================
// psemu - ses cikisi (sceAudioOut*)
// ----------------------------------------------------------------------------
// NEDEN KENDI ARKA UCUMUZ:
// KytyPS5'in gpu/src/libs/audio.cpp'si tam bir sceAudioOut yuzeyi sunuyor ama
// SDL2'ye VE Kyty'nin kendi kernel/pthread + kernel/semaphore katmanina bagli.
// psemu bu ikisini kendi HLE'siyle yapiyor; Kyty'nin surumunu almak cakisirdi.
// Bu yuzden gpu/CMakeLists.txt libs/ altindan yalnizca controller.cpp'yi alir.
// Burasi bagimsiz, kucuk bir waveOut arka ucu: winmm zaten linkli (loader
// timeBeginPeriod icin kullaniyordu), yani YENI BAGIMLILIK YOK.
//
// KACIS KAPISI: PSEMU_NO_AUDIO=1 ile cihaz hic acilmaz ve eski davranisa
// (yalnizca pacing uykusu) donulur. Bu emulatorde tanilarin olcumu bozdugu iki
// olay yasandi; sesi tek degiskenle devre disi birakabilmek sart.
// ============================================================================
#ifndef PSEMU_AUDIO_H
#define PSEMU_AUDIO_H

#include <cstdint>

namespace Psemu::Audio {

// Sony hata kodlari (yalnizca kullandiklarimiz).
constexpr int kErrorInvalidPort = static_cast<int>(0x80260002);

int Init();

// sceAudioOutOpen(user_id, type, index, len, freq, param)
// len  : GRAIN - cagri basina KANAL BASINA ornek sayisi.
// param: dusuk bitler bicim (S16/FLOAT x MONO/STEREO/8CH).
// Basarida > 0 handle doner. 0 DONMEZ: oyunlar 0'i gecersiz sayabiliyor.
int Open(int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param);

// sceAudioOutOutput(handle, ptr) - DIKKAT: uzunluk argumani YOKTUR.
// Ornek sayisi Open'daki len'dir. Gercek donanimda bu cagri tampon tuketilene
// kadar BLOKLAR; ses thread'ini ornekleme hizina pace'leyen sey budur.
int Output(int handle, const void* pcm);

// Output'un okuyacagi TAM paket boyutu (len * kanal * ornek_boyu).
// core.cpp misafir bellegini SafeReadable ile bununla dogrular; yalnizca ilk
// baytlara bakmak commit edilmemis bir sayfayi kaciririr.
// Bilinmeyen handle icin 0 doner.
uint32_t PacketBytes(int handle);

int SetVolume(int handle, uint32_t flag, const int* vol);
int Close(int handle);

// Open cagrilmadan Output geldiginde (bicimi bilmedigimiz durum) kullanilacak
// grain tahmini; core.cpp'nin geri dusus yolunun dogru uyumasi icin.
uint32_t DefaultGrain();

} // namespace Psemu::Audio

#endif // PSEMU_AUDIO_H
