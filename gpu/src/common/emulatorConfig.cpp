#include "common/emulatorConfig.h"

#include "common/assert.h"

#include <algorithm>
#include <memory>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

KYTY_SUBSYSTEM_INIT(Config) {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Config) {}

KYTY_SUBSYSTEM_DESTROY(Config) {}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);

	*g_config = cfg;
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

bool VulkanValidationEnabled() {
	// TANI ANAHTARI: PSEMU_VK_VALIDATE=1.
	//
	// `shader_validation_enabled` ile BIREBIR ayni desen: yapilandirma alani
	// `false` SABIT ve kodun hicbir yerinden set edilmiyordu, yani Vulkan
	// dogrulama katmanlari da PRATIKTE HIC KOSMAMISTI. spirv-val'i acmak
	// gecenin donum noktasi oldu (moduller sessizce gecersizdi); ayni sinifta
	// ikinci bir kor nokta olmasin.
	static const bool env_on = [] {
		const char* env = std::getenv("PSEMU_VK_VALIDATE");
		return env != nullptr && env[0] == '1';
	}();
	return g_config->vulkan_validation_enabled || env_on;
}

bool ShaderValidationEnabled() {
	// TANI ANAHTARI: PSEMU_SHADER_VALIDATE=1. Yapilandirma alani hicbir
	// yerden set edilmiyordu, yani spirv-val PRATIKTE HIC KOSMUYORDU - mesh
	// modulunun gecerli olup olmadigini bu yuzden hic bilemedik. Gecersiz bir
	// modul surucu tarafindan sessizce hicbir sey cizmeden kabul edilebilir.
	static const bool env_on = [] {
		const char* env = std::getenv("PSEMU_SHADER_VALIDATE");
		return env != nullptr && env[0] == '1';
	}();
	return g_config->shader_validation_enabled || env_on;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return g_config->shader_optimization_type;
}

ShaderLogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

OutputDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

ProfilerDirection GetProfilerDirection() {
	return g_config->profiler_direction;
}

bool SpirvDebugPrintfEnabled() {
	// TANI ANAHTARI: PSEMU_SPIRV_PRINTF=1. VulkanValidationEnabled ile ayni
	// desen - yapilandirma alani hicbir yerden set edilmiyor (Config::Load
	// psemu'da hic cagrilmiyor), yani debug printf PRATIKTE HIC ACILAMIYORDU.
	//
	// Neden simdi gerekli: ekran (framebuffer) kanali bu olcum icin tukendi.
	// Son tuzak, shader'in basinda yazilan telemetri ucgeninin uzerine oyunun
	// kendi bozuk pozisyonlarinin sonradan yazildigini gosterdi - kanal her
	// seferinde sahte "siyah ekran" raporluyordu. Kalan tek soru (s4 = vertex
	// sayisi) icin dogrudan bir GPU->CPU kanali sart.
	static const bool env_on = [] {
		const char* env = std::getenv("PSEMU_SPIRV_PRINTF");
		return env != nullptr && env[0] == '1';
	}();
	return g_config->spirv_debug_printf_enabled || env_on;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool NggRectlistDrawEnabled() {
	return g_config->ngg_rectlist_draw_enabled;
}

} // namespace Config
