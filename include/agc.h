#pragma once
#include <windows.h>
#include <string>

// ============================================================================
// AGC (libSceAgc) HLE-GPU emulasyonu
// ----------------------------------------------------------------------------
// PPSA02929 (ve tum PS5 baslikleri) render'i AGC ile yapar: oyun bir Draw
// Command Buffer (DCB) insa eder, icine cizim/register/flip komutlari yazar.
// Ham PM4 paketlerini yorumlamak yerine HLE yaklasimi kullaniyoruz: AGC API
// cagrilarini (zaten PLT-hook ile yakaliyoruz) dogrudan kendi render'imiza
// ceviriyoruz. 2D bir oyun oldugu icin her cizim 6-index'li dokulu bir quad.
//
// M1 (su an): flip'i oyun-guduml yapmak (sceAgcDcbSetFlip -> Video::Flip) ve
// render-state'i (shader/draw/register kurulumu) yakalayip loglamak. Boylece
// bir sonraki adimda (clear rengi, sprite raster) neyi implemente edecegimizi
// tahminle degil kanitla belirliyoruz.
// ============================================================================

namespace Agc {

// `name` bu modulun ele aldigi bir AGC fonksiyonuysa: ctx'ten argumanlari okur,
// HLE isini yapar, ctx->Rax'i ayarlar ve true doner. Degilse hicbir yan etki
// birakmadan false doner (dispatch zinciri devam etsin diye).
// nid: suffix'li raw NID (or. "B+aG9DUnTKA#A#B"). name: cozulmus okunabilir ad.
bool Dispatch(const std::string& nid, const std::string& name, CONTEXT* ctx);

} // namespace Agc
