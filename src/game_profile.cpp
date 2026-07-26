#include "game_profile.h"

#include "logger.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace Game {
namespace {

Profile g_current; // varsayilan: tum quirk'ler kapali

// Yoldan "PPSA21564" / "CUSA12345" gibi bir titleId yakalar.
std::string TitleIdFromPath(const std::string& path) {
	for (size_t i = 0; i + 9 <= path.size(); i++) {
		const bool prefix_ok =
		    (path.compare(i, 4, "PPSA") == 0) || (path.compare(i, 4, "CUSA") == 0);
		if (!prefix_ok) {
			continue;
		}
		bool digits = true;
		for (size_t d = i + 4; d < i + 9; d++) {
			if (std::isdigit(static_cast<unsigned char>(path[d])) == 0) {
				digits = false;
				break;
			}
		}
		if (digits) {
			return path.substr(i, 9);
		}
	}
	return {};
}

// sce_sys/param.json icinden "titleId": "PPSA21564" degerini ceker.
// Tam bir JSON ayristiricisina gerek yok: tek bir alan ariyoruz.
std::string TitleIdFromParamJson(const std::string& eboot_path) {
	const size_t slash = eboot_path.find_last_of("/\\");
	if (slash == std::string::npos) {
		return {};
	}
	const std::string dir = eboot_path.substr(0, slash);

	std::ifstream f(dir + "/sce_sys/param.json", std::ios::binary);
	if (!f) {
		return {};
	}
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string text = ss.str();

	const size_t key = text.find("\"titleId\"");
	if (key == std::string::npos) {
		return {};
	}
	const size_t open = text.find('"', text.find(':', key) + 1);
	if (open == std::string::npos) {
		return {};
	}
	const size_t close = text.find('"', open + 1);
	if (close == std::string::npos) {
		return {};
	}
	return text.substr(open + 1, close - open - 1);
}

} // namespace

void InitProfile(const std::string& eboot_path) {
	std::string id = TitleIdFromParamJson(eboot_path);
	if (id.empty()) {
		id = TitleIdFromPath(eboot_path);
	}

	g_current               = Profile{};
	g_current.title_id      = id;
	g_current.savedata_root = id.empty() ? std::string("savedata") : ("savedata/" + id);

	if (id == "PPSA02929") {
		g_current.name                                = "Dreaming Sarah";
		g_current.quirk_c2_type_registration_overflow = true;
		g_current.quirk_texture_meta_recover          = true;
		g_current.quirk_call_dt_init                  = true;
		g_current.quirk_rva_diagnostics               = true;
	} else if (id == "PPSA21564") {
		g_current.name = "Astro Bot";
		// Henuz oyuna ozel duzeltme yok - hepsi kapali. Dreaming Sarah'in
		// adres bagimli yamalari BURAYA UYGULANMAZ.
	} else {
		g_current.name = id.empty() ? "bilinmeyen oyun" : ("taninmayan: " + id);
	}

	std::stringstream m;
	m << "[PROFIL] titleId=" << (id.empty() ? "(cozulemedi)" : id) << " -> " << g_current.name
	  << " | adres-bagimli yamalar: "
	  << (g_current.quirk_c2_type_registration_overflow ? "tip-kayit " : "")
	  << (g_current.quirk_texture_meta_recover ? "texture-meta " : "")
	  << (g_current.quirk_rva_diagnostics ? "rva-tani " : "")
	  << (g_current.quirk_c2_type_registration_overflow || g_current.quirk_texture_meta_recover ||
	              g_current.quirk_rva_diagnostics
	          ? ""
	          : "YOK (guvenli varsayilan)")
	  << " | kayit klasoru: " << g_current.savedata_root;
	LOG_INFO(m.str());
}

const Profile& Current() {
	return g_current;
}

} // namespace Game

