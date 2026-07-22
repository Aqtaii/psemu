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

#include "common/abi.h"            // KYTY_SYSV_ABI
#include "loader/symbolDatabase.h" // Loader::SymbolDatabase / FindByNid

// Kyty'nin top-level kayit fonksiyonlari (namespace Libs).
namespace Libs {
void InitGraphicsDriver_1(Loader::SymbolDatabase* s);
void InitVideoOut_1(Loader::SymbolDatabase* s);
} // namespace Libs

namespace {
Loader::SymbolDatabase* g_kyty_db = nullptr;

void EnsureDb() {
	if (g_kyty_db != nullptr) {
		return;
	}
	auto* db = new Loader::SymbolDatabase;
	Libs::InitGraphicsDriver_1(db); // AGC Dcb + Graphics + GraphicsDriver
	Libs::InitVideoOut_1(db);       // sceVideoOut*
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
