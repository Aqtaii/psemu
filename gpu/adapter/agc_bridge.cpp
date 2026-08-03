// ============================================================================
// psemu: oyunun AGC / VideoOut / Graphics cagrilarini Kyty'nin
// implementasyonlarina yonlendiren kopru.
//
// Kyty'nin KENDI SymbolDatabase'ini kullanir: kayit fonksiyonlari
// (InitGraphicsDriver_1 + InitVideoOut_1) NID -> fonksiyon-adresi tablosunu
// doldurur. Oyun bir AGC fonksiyonu cagirinca psemu, suffix'siz raw NID ile
// bu tabloda arar; bulursa Kyty fonksiyonunu SysV ABI ile (arg'lar VEH
// CONTEXT'inden) cagirir. Boylece oyunun DCB'sini Kyty'nin agc.cpp'si
// PM4 olarak insa eder; GraphicsDriverSubmitDcb Kyty'nin komut islemcisine
// gider -> Vulkan cizim.
//
// psemu cekirdegi gpu include yollarina sahip olmadigi icin bu dosya gpu/
// altinda derlenir; agc.cpp yalnizca extern "C" PsemuKytyAgcCall'u cagirir.
// ============================================================================
#include <windows.h>

#include <cstdlib> // getenv (PSEMU_NO_KYTY_EXTRA)

#include "common/abi.h"            // KYTY_SYSV_ABI
#include "loader/symbolDatabase.h" // Loader::SymbolDatabase / FindByNid

// Kyty'nin top-level kayit fonksiyonlari (namespace Libs).
#include "libs/controller.h"

namespace Libs {
void InitGraphicsDriver_1(Loader::SymbolDatabase* s);
void InitVideoOut_1(Loader::SymbolDatabase* s);
void InitPad_1(Loader::SymbolDatabase* s);
// Asagidakiler EKLENDI - gerekce icin EnsureDb()'deki nota bakiniz.
void InitFont_1(Loader::SymbolDatabase* s);
void InitAmpr_1(Loader::SymbolDatabase* s);
} // namespace Libs

// ----------------------------------------------------------------------------
// avpriv_vga16_font: Kyty'nin libFont.cpp'si bunu FFmpeg'den (libavutil) aliyor
// ve GERCEK font yuklenemedigi durumda YEDEK bitmap glif kaynagi olarak
// kullaniyor (8x16, 256 karakter = 4096 bayt). Vendored agacimizda FFmpeg YOK.
//
// Tabloyu SIFIR biraktik. Bilerek: uydurma glif verisi uretmek yanlis
// karakterler cizdirir ve "yazi tipi calisiyor" yanilgisi yaratir. Sifir
// tabloda bu yedek yol BOS glif uretir - yani oyunun kendi fontlari
// yuklendiginde metin normal cikar, yalnizca YEDEK yola dusuldugunde
// gorunmez olur. Gercek tabloya ihtiyac olursa buraya konabilir.
extern "C" const uint8_t avpriv_vga16_font[4096] = {};

namespace {
Loader::SymbolDatabase* g_kyty_db = nullptr;

void EnsureDb() {
	if (g_kyty_db != nullptr) {
		return;
	}
	auto* db = new Loader::SymbolDatabase;
	Libs::InitGraphicsDriver_1(db); // AGC Dcb + Graphics + GraphicsDriver
	Libs::InitVideoOut_1(db);       // sceVideoOut*
	Libs::InitPad_1(db);            // scePad*

	// ------------------------------------------------------------------
	// libFont ve libAmpr DE KAYDEDILIYOR.
	//
	// Bu kopru yalnizca UC kutuphane kaydediyordu; oysa vendored Kyty
	// agacinda libFont (70 fonksiyon) ve libAmpr (106) ZATEN UYGULANMIS.
	// psemu onlara hic yonlendirmedigi icin ayni fonksiyonlar kendi bos
	// stub'imiza dusuyor ve RAX=0 donuyordu. [HLE-EKSIK] tanisiyla olculdu:
	//     sceFontOpenFontMemory / sceFontCreateLibraryWithEdition /
	//     sceFontOpenFontSet      -> govdesi yok
	//     sceAmprCommandBufferConstructor / AprCommandBufferConstructor /
	//     CommandBufferSetBuffer / CommandBufferGetNumCommands -> govdesi yok
	// Kyty'de bunlarin karsiliklari NID'leriyle kayitli (Font::FontOpenFontMemory,
	// Ampr::CommandBufferConstructor, ...).
	//
	// Yonlendirme kapisi (src/agc.cpp) "Kyty DB'de NID varsa Kyty'ye ver"
	// diyor ve HLE zincirinden ONCE calisiyor; dolayisiyla kayit yeterli,
	// ayrica psemu'daki elle yazilmis font stub'i da otomatik devre disi
	// kaliyor (o zincire hic girilmiyor).
	//
	// KACIS KAPISI: PSEMU_NO_KYTY_EXTRA=1 -> eski davranis (yalnizca uc
	// kutuphane). Yeni bir yuzey aciyoruz; tek degiskenle geri alinabilmeli.
	const char* no_extra = getenv("PSEMU_NO_KYTY_EXTRA");
	if (no_extra == nullptr || no_extra[0] != '1') {
		Libs::InitFont_1(db); // sceFont*
		Libs::InitAmpr_1(db); // sceAmpr*

	}

	Libs::Controller::ControllerSubsystem::Instance()->Init(nullptr);
	g_kyty_db = db;
}
} // namespace

// Kyty veritabaninda bu NID var mi diye bakar (fonksiyonu cagirmadan).
extern "C" bool PsemuKytyHasNid(const char* nid) {
	EnsureDb();
	const Loader::SymbolRecord* rec = g_kyty_db->FindByNid(nid, Loader::SymbolType::Func);
	return (rec != nullptr && rec->vaddr != 0);
}

// Oyunun (suffix'siz) NID'siyle Kyty fonksiyonunu bulur ve SysV ABI ile cagirir.
extern "C" bool PsemuKytyAgcCall(const char* nid, CONTEXT* ctx) {
	EnsureDb();
	const Loader::SymbolRecord* rec = g_kyty_db->FindByNid(nid, Loader::SymbolType::Func);
	if (rec == nullptr || rec->vaddr == 0) {
		return false;
	}
	// SysV ABI: ilk 6 tamsayi/pointer arg RDI,RSI,RDX,RCX,R8,R9; 7.+ arg STACK'te.
	// Bazi fonksiyonlar >6 arg alir (or. sceVideoOutRegisterBuffers2 8 arg;
	// 'option' = arg8 stack'te). Oyunun stack'inden ([Rsp+8]=arg7, [Rsp+16]=arg8,
	// ...; [Rsp]=donus adresi) 4 ek arg okuyup geciyoruz (10 arg toplam; fazlasi
	// fonksiyon tarafindan zararsizca yok sayilir). Kyty fonksiyonu KYTY_SYSV_ABI
	// oldugu icin clang-cl arg'lari dogru (SysV) yerlestirir.
	const uint64_t* stk = reinterpret_cast<const uint64_t*>(ctx->Rsp);
	uint64_t a7 = 0, a8 = 0, a9 = 0, a10 = 0;
	if (stk != nullptr) {
		a7  = stk[1];
		a8  = stk[2];
		a9  = stk[3];
		a10 = stk[4];
	}
	using Fn = uint64_t(KYTY_SYSV_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
	                                    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
	Fn fn    = reinterpret_cast<Fn>(rec->vaddr);
	ctx->Rax = fn(ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx->Rcx, ctx->R8, ctx->R9, a7, a8, a9, a10);
	return true;
}

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/textureCache.h"

extern "C" void PsemuMarkCpuModified(uint64_t vaddr, uint64_t size) {
	if (Libs::Graphics::g_render_ctx != nullptr) {
		auto* bc = Libs::Graphics::g_render_ctx->GetBufferCache();
		if (bc != nullptr) {
			bc->PublishImageBacking(vaddr, size);
		}
	}
}
