#ifndef PSEMU_GAME_PROFILE_H_
#define PSEMU_GAME_PROFILE_H_

#include <string>

// ============================================================================
// OYUNA OZEL DAVRANIS PROFILLERI
// ----------------------------------------------------------------------------
// psemu'daki bazi duzeltmeler BELLI BIR OYUNUN BELLI BIR ADRESINE baglidir
// (ornegin "RVA 0x2dfff0'daki tip-kayit fonksiyonu 4 slotluk tabloyu tasiriyor").
// Bu adresler yalnizca o oyunun binary'sinde anlamlidir. Baska bir oyun
// yuklendiginde ayni RVA'da bambaska bir kod bulunur ve duzeltme SESSIZCE
// yanlis registerlari degistirip bellegi bozar - teshis edilmesi cok zor bir
// hata sinifi.
//
// Bu yuzden adrese bagli her duzeltme bir "quirk" bayragina baglidir ve
// bayraklar yalnizca ilgili titleId icin acilir. Taninmayan bir oyun icin TUM
// quirk'ler KAPALIDIR: yeni oyun, eski oyunun yamalarindan etkilenmez.
//
// Yeni bir oyuna ozel duzeltme eklerken: once buraya bir bayrak ekle, sonra
// kodu "if (Game::Profile().quirk_xxx)" ile sar. Ciplak RVA kullanma.
// ============================================================================
namespace Game {

struct Profile {
	std::string title_id; // "PPSA02929" gibi; bilinmiyorsa bos
	std::string name;     // insan okusun diye

	// --- Dreaming Sarah (PPSA02929) ---
	// Tip-kayit fonksiyonu (RVA 0x2dfff0..0x2e0900) 4 slotluk tabloya 5. kaydi
	// yapmaya calisiyor; NULL-base erisimleri dummy buffer'a yonlendiriyoruz.
	bool quirk_c2_type_registration_overflow = false;
	// RVA 0x1654f8'deki texture metadata okumasi cokuyor; sahneyi ilerletmek
	// icin komutu atlayip RSI=1 zorluyoruz.
	bool quirk_texture_meta_recover = false;
	// RVA'ya bagli tani ciktilari (yigin cercevesi dogrulamasi vs.)
	bool quirk_rva_diagnostics = false;

	// Kayit verisi kok klasoru. Oyunlar birbirinin kaydini EZMESIN diye
	// titleId ile ayrilir: "savedata/PPSA02929".
	std::string savedata_root = "savedata";
};

// eboot yolundan titleId'yi cozer (yanindaki sce_sys/param.json, yoksa yoldaki
// PPSAxxxxx deseni) ve uygun profili secer. Program basinda BIR KEZ cagrilir.
void InitProfile(const std::string& eboot_path);

// Aktif profil. InitProfile cagrilmadiysa "bilinmeyen oyun" profili doner
// (tum quirk'ler kapali) - yani guvenli varsayilan.
const Profile& Current();

} // namespace Game

#endif // PSEMU_GAME_PROFILE_H_
