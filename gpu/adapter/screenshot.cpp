// ============================================================================
// psemu: present edilen Vulkan image'ini geri okuyup (readback) BMP olarak
// diske kaydeder. Boylece render ciktisi dogrudan gorulebilir (Vulkan
// swapchain penceresi GDI ile siyah cikardi). Her calistirma ayri bir
// images/run_<zaman>/ klasorune yazar; kareler frame_XXXXX.bmp.
// 4x kucultulur (4K -> 960x540) ki dosyalar makul kalsin.
// ============================================================================
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vma.h"

namespace Libs::Graphics {

static std::string g_run_dir;      // images/run_<zaman>
static std::atomic<uint64_t> g_shot_seq{0};
static std::atomic<uint64_t> g_present_seq{0};

// Ilk cagrida bu calistirmaya ozel klasoru olustur.
static const std::string& EnsureRunDir() {
	static std::once_flag once;
	std::call_once(once, [] {
		CreateDirectoryA("images", nullptr); // ust klasor (varsa sorun degil)
		SYSTEMTIME st;
		GetLocalTime(&st);
		char name[128];
		std::snprintf(name, sizeof(name), "images\\run_%04u%02u%02u_%02u%02u%02u",
		              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		CreateDirectoryA(name, nullptr);
		g_run_dir = name;
		std::fprintf(stderr, "[SHOT] screenshot klasoru: %s\\\n", name);
	});
	return g_run_dir;
}

// Kaynak piksel nasil cozulecek. PS5 ekran yuzeyi 10-BIT PAKETLI olabiliyor
// (olculdu: pixel_format=0x8100000000000000 -> A2R10G10B10_UNORM_PACK32);
// 4 bayti 8-bit kanal sanmak anlamsiz bir desen uretir.
enum class ShotPixelDecode { Bgra8, Rgba8, A2R10G10B10, A2B10G10R10, Unsupported };

static ShotPixelDecode SelectPixelDecode(vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eR8G8B8A8Uint: return ShotPixelDecode::Rgba8;
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb: return ShotPixelDecode::Bgra8;
		case vk::Format::eA2R10G10B10UnormPack32: return ShotPixelDecode::A2R10G10B10;
		case vk::Format::eA2B10G10R10UnormPack32: return ShotPixelDecode::A2B10G10R10;
		default: return ShotPixelDecode::Unsupported;
	}
}

// 4 bayttan (B,G,R) uret. 10-bit paketli formatlarda 32-bit kelime little
// endian okunur: A2R10G10B10 -> A[31:30] R[29:20] G[19:10] B[9:0].
static void DecodePixel(const uint8_t* px, ShotPixelDecode decode, uint8_t* b, uint8_t* g,
                        uint8_t* r) {
	switch (decode) {
		case ShotPixelDecode::Rgba8: *r = px[0]; *g = px[1]; *b = px[2]; return;
		case ShotPixelDecode::Bgra8: *b = px[0]; *g = px[1]; *r = px[2]; return;
		case ShotPixelDecode::A2R10G10B10: {
			uint32_t v = 0;
			std::memcpy(&v, px, sizeof(v));
			*r = static_cast<uint8_t>(((v >> 20u) & 0x3ffu) >> 2u);
			*g = static_cast<uint8_t>(((v >> 10u) & 0x3ffu) >> 2u);
			*b = static_cast<uint8_t>((v & 0x3ffu) >> 2u);
			return;
		}
		case ShotPixelDecode::A2B10G10R10: {
			uint32_t v = 0;
			std::memcpy(&v, px, sizeof(v));
			*b = static_cast<uint8_t>(((v >> 20u) & 0x3ffu) >> 2u);
			*g = static_cast<uint8_t>(((v >> 10u) & 0x3ffu) >> 2u);
			*r = static_cast<uint8_t>((v & 0x3ffu) >> 2u);
			return;
		}
		case ShotPixelDecode::Unsupported: *b = 0; *g = 0; *r = 0; return;
	}
}

// 24-bit, bottom-up BGR BMP yaz (4x downscale).
static void WriteBmpDownscaled(const std::string& path, const uint8_t* bgra, uint32_t w,
                               uint32_t h, ShotPixelDecode decode) {
	constexpr uint32_t F = 4; // kucultme faktoru
	const uint32_t ow = w / F;
	const uint32_t oh = h / F;
	if (ow == 0 || oh == 0) return;
	const uint32_t row_bytes = (ow * 3u + 3u) & ~3u; // 4-byte hizali
	const uint32_t img_bytes = row_bytes * oh;

	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	fh.bfType    = 0x4D42; // 'BM'
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize    = fh.bfOffBits + img_bytes;
	ih.biSize        = sizeof(ih);
	ih.biWidth       = static_cast<LONG>(ow);
	ih.biHeight      = static_cast<LONG>(oh); // pozitif = bottom-up
	ih.biPlanes      = 1;
	ih.biBitCount    = 24;
	ih.biCompression = BI_RGB;
	ih.biSizeImage   = img_bytes;

	FILE* f = std::fopen(path.c_str(), "wb");
	if (f == nullptr) return;
	std::fwrite(&fh, sizeof(fh), 1, f);
	std::fwrite(&ih, sizeof(ih), 1, f);

	std::string row(row_bytes, '\0');
	for (uint32_t oy = 0; oy < oh; oy++) {
		// BMP bottom-up: en alt satir once. Kaynakta ust-sol origin.
		const uint32_t sy = (oh - 1u - oy) * F;
		for (uint32_t ox = 0; ox < ow; ox++) {
			const uint8_t* px = bgra + (static_cast<size_t>(sy) * w + static_cast<size_t>(ox) * F) * 4u;
			uint8_t b = 0, g = 0, r = 0;
			DecodePixel(px, decode, &b, &g, &r);
			row[ox * 3u + 0u] = static_cast<char>(b);
			row[ox * 3u + 1u] = static_cast<char>(g);
			row[ox * 3u + 2u] = static_cast<char>(r);
		}
		std::fwrite(row.data(), row_bytes, 1, f);
	}
	std::fclose(f);
}

// ============================================================================
// psemu tani: guest bellekteki (lineer, RGBA8) bir texture'i BMP olarak dok.
// Amac: sprite-font atlas'i (or. 320x512) gercekten glyph iceriyor mu, yoksa
// bos/saydam mi? Bos ise metin gorunmez olur (PNG decode/upload sorunu);
// dolu ise sorun UV/geometri/blend tarafindadir.
// DEVELOPER_GUIDE geregi dokumler tools/dumps/ altina yazilir.
// ============================================================================
void PsemuDumpGuestTexture(uint64_t addr, uint32_t w, uint32_t h) {
	if (addr == 0 || w == 0 || h == 0 || w > 4096 || h > 4096) return;
	static std::atomic<int> s_dumped{0};
	if (s_dumped.load() >= 10) return;

	const auto* px = reinterpret_cast<const uint8_t*>(addr);
	MEMORY_BASIC_INFORMATION mbi{};
	const uint64_t need = static_cast<uint64_t>(w) * h * 4u;
	if (VirtualQuery(px, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) return;
	const auto* region_end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
	if (px + need > region_end) return; // tek bolgede degil -> atla (guvenli taraf)

	uint64_t nonzero = 0, opaque = 0;
	for (uint64_t i = 0; i < need; i += 4) {
		if (px[i] != 0 || px[i + 1] != 0 || px[i + 2] != 0) nonzero++;
		if (px[i + 3] != 0) opaque++;
	}

	CreateDirectoryA("tools", nullptr);
	CreateDirectoryA("tools\\dumps", nullptr);
	char name[160];
	std::snprintf(name, sizeof(name), "tools\\dumps\\tex_%llx_%ux%u.bmp",
	              static_cast<unsigned long long>(addr), w, h);
	// Kucuk texture'lari kucultmeden yaz (F=1 icin ayri yol): downscale 1x
	// istedigimiz icin gecici olarak 24-bit BMP'yi burada uretiyoruz.
	const uint32_t row_bytes = (w * 3u + 3u) & ~3u;
	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	fh.bfType    = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize    = fh.bfOffBits + row_bytes * h;
	ih.biSize        = sizeof(ih);
	ih.biWidth       = static_cast<LONG>(w);
	ih.biHeight      = static_cast<LONG>(h);
	ih.biPlanes      = 1;
	ih.biBitCount    = 24;
	ih.biCompression = BI_RGB;
	ih.biSizeImage   = row_bytes * h;
	FILE* f = std::fopen(name, "wb");
	if (f == nullptr) return;
	std::fwrite(&fh, sizeof(fh), 1, f);
	std::fwrite(&ih, sizeof(ih), 1, f);
	std::string row(row_bytes, '\0');
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t* src = px + static_cast<size_t>(h - 1u - y) * w * 4u;
		for (uint32_t x = 0; x < w; x++) {
			// RGBA8 -> BGR (alfa'yi siyah zemine carp: glyph'ler gorunsun)
			const uint8_t a = src[x * 4u + 3u];
			row[x * 3u + 0u] = static_cast<char>(src[x * 4u + 2u] * a / 255);
			row[x * 3u + 1u] = static_cast<char>(src[x * 4u + 1u] * a / 255);
			row[x * 3u + 2u] = static_cast<char>(src[x * 4u + 0u] * a / 255);
		}
		std::fwrite(row.data(), row_bytes, 1, f);
	}
	std::fclose(f);
	s_dumped.fetch_add(1);
	std::fprintf(stderr, "[TEXDUMP] %s  rgb_nonzero=%llu/%llu  alpha_nonzero=%llu\n", name,
	             static_cast<unsigned long long>(nonzero),
	             static_cast<unsigned long long>(need / 4), static_cast<unsigned long long>(opaque));
	std::fflush(stderr);
}

// swapchain.cpp WindowPresentFrame'den cagrilir. image present edilecek kare;
// bu noktada eTransferSrcOptimal layout'unda (WindowPrepareFrame'in CopyImage'i
// oraya birakti). Her N present'te bir yakalar, run basina ust sinir uygular.
void PsemuCaptureFrame(GraphicContext* ctx, const VulkanImage* image) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr) return;
	if (image->format == vk::Format::eUndefined) return;

	const uint64_t p = g_present_seq.fetch_add(1);
	// Yakalama araligi ayarlanabilir: PSEMU_SHOT_EVERY=1 her present'i okur.
	// TANI icin gerekli: kareler BIREBIR AYNI mi? Sahne-degisikligi tespiti
	// ayni kareleri diske yazmadigi icin, N present'e karsilik kac BMP
	// olustugu dogrudan "cizim oluyor mu" sorusunu yanitlar.
	static const uint64_t s_every = [] {
		const char*   e = std::getenv("PSEMU_SHOT_EVERY");
		const uint64_t v = (e != nullptr) ? std::strtoull(e, nullptr, 10) : 4ull;
		return v == 0 ? 1ull : v;
	}();
	if ((p % s_every) != 0) return;
	if (g_shot_seq.load() >= 60) return;      // en fazla 60 DISTINCT sahne
	if (p >= 40000ull) return;                // guvenlik ust siniri

	const uint32_t w = image->extent.width;
	const uint32_t h = image->extent.height;
	if (w == 0 || h == 0) return;
	// TANI: bu arac 4 bayti 8-BIT KANAL sayarak yaziyor. Kaynak 10-bit
	// paketli (A2B10G10R10) ya da 16-bit float ise urettigi goruntu
	// ANLAMSIZ olur - yani gordugumuz desen render hatasi degil, ARACIN
	// kendi eseri olabilir. Formati her calistirmada bir kez basiyoruz ki
	// bu ayrim tahmin gerektirmesin.
	{
		static std::atomic<bool> s_logged {false};
		if (!s_logged.exchange(true)) {
			const auto  d    = SelectPixelDecode(image->format);
			const char* name = d == ShotPixelDecode::Rgba8          ? "RGBA8"
			                   : d == ShotPixelDecode::Bgra8        ? "BGRA8"
			                   : d == ShotPixelDecode::A2R10G10B10  ? "A2R10G10B10 (10-bit HDR)"
			                   : d == ShotPixelDecode::A2B10G10R10  ? "A2B10G10R10 (10-bit HDR)"
			                                                        : "DESTEKLENMIYOR";
			std::fprintf(stderr, "[SHOT-FORMAT] sunulan goruntu vk_format=%d (%ux%u) -> cozum: %s\n",
			             static_cast<int>(image->format), w, h, name);
			std::fflush(stderr);
		}
	}
	const uint64_t size = static_cast<uint64_t>(w) * h * 4u;

	VulkanBuffer readback{};
	readback.usage           = vk::BufferUsageFlagBits::eTransferDst;
	readback.memory.property = vk::MemoryPropertyFlagBits::eHostVisible |
	                           vk::MemoryPropertyFlagBits::eHostCoherent;
	VulkanCreateBuffer(ctx, size, &readback);
	if (readback.buffer == nullptr) return;

	{
		CommandBuffer cmd(GraphicContext::QUEUE_GFX);
		cmd.Begin();
		auto vk = cmd.Handle();

		// image su an eTransferSrcOptimal; dogrudan buffer'a kopyala.
		vk::BufferImageCopy region{};
		region.bufferOffset                    = 0;
		region.bufferRowLength                 = 0; // sikisik
		region.bufferImageHeight               = 0;
		region.imageSubresource.aspectMask     = vk::ImageAspectFlagBits::eColor;
		region.imageSubresource.mipLevel       = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount     = 1;
		region.imageOffset                     = vk::Offset3D{0, 0, 0};
		region.imageExtent                     = vk::Extent3D{w, h, 1};
		vk.copyImageToBuffer(image->image, vk::ImageLayout::eTransferSrcOptimal, readback.buffer, 1,
		                     &region);

		cmd.End();
		cmd.Execute();
		cmd.WaitForFence();
	}

	void* data = nullptr;
	VulkanMapMemory(ctx, &readback.memory, &data);
	if (data != nullptr) {
		const auto* px = static_cast<const uint8_t*>(data);
		// Sahne-degisikligi tespiti: 16x16 izgara (256 nokta) luma imzasi cikar,
		// son KAYDEDILEN kareyle karsilastir. Belirgin farkliysa (veya ilk kare)
		// kaydet. Boylece lingering splash 1 kez, sonra Dreaming Sarah, sonra
		// menu... her distinct sahne yakalanir; git-gel de fark uretir.
		static uint8_t s_last_sig[256];
		static bool    s_have_last = false;
		uint8_t sig[256];
		{
			int k = 0;
			for (int gy = 0; gy < 16; gy++) {
				for (int gx = 0; gx < 16; gx++) {
					const uint32_t sx = (static_cast<uint32_t>(gx) * 2u + 1u) * w / 32u;
					const uint32_t sy = (static_cast<uint32_t>(gy) * 2u + 1u) * h / 32u;
					const uint8_t* q = px + (static_cast<size_t>(sy) * w + sx) * 4u;
					// Imza da COZULMUS pikselden uretilmeli: 10-bit paketli
					// yuzeyde ham baytlarin toplami luma degildir ve sahne
					// degisikligini yanlis olcer.
					uint8_t qb = 0, qg = 0, qr = 0;
					DecodePixel(q, SelectPixelDecode(image->format), &qb, &qg, &qr);
					sig[k++] = static_cast<uint8_t>((qb + qg + qr) / 3u); // luma
				}
			}
		}
		uint32_t diff = 0;
		if (s_have_last) {
			for (int i = 0; i < 256; i++) {
				int d = static_cast<int>(sig[i]) - static_cast<int>(s_last_sig[i]);
				diff += static_cast<uint32_t>(d < 0 ? -d : d);
			}
		}
		const bool scene_changed = !s_have_last || diff >= 1200; // ~ort 5/nokta * 256
		if (scene_changed) {
			std::memcpy(s_last_sig, sig, sizeof(sig));
			s_have_last = true;
			const uint64_t seq = g_shot_seq.fetch_add(1);
			char fname[64];
			std::snprintf(fname, sizeof(fname), "\\frame_%05llu.bmp",
			              static_cast<unsigned long long>(seq));
			WriteBmpDownscaled(EnsureRunDir() + fname, px, w, h, SelectPixelDecode(image->format));
			std::fprintf(stderr, "[SHOT] SAHNE kare: %s%s (%ux%u, present#%llu, diff=%u)\n",
			             g_run_dir.c_str(), fname, w, h, static_cast<unsigned long long>(p), diff);
		}
		VulkanUnmapMemory(ctx, &readback.memory);
	}

	VulkanDeleteBuffer(ctx, &readback);
}

} // namespace Libs::Graphics
