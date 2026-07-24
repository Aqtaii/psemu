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

// 24-bit, bottom-up BGR BMP yaz (4x downscale).
static void WriteBmpDownscaled(const std::string& path, const uint8_t* bgra, uint32_t w,
                               uint32_t h, bool source_is_rgba) {
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
			uint8_t b, g, r;
			if (source_is_rgba) { r = px[0]; g = px[1]; b = px[2]; }
			else                { b = px[0]; g = px[1]; r = px[2]; }
			row[ox * 3u + 0u] = static_cast<char>(b);
			row[ox * 3u + 1u] = static_cast<char>(g);
			row[ox * 3u + 2u] = static_cast<char>(r);
		}
		std::fwrite(row.data(), row_bytes, 1, f);
	}
	std::fclose(f);
}

// swapchain.cpp WindowPresentFrame'den cagrilir. image present edilecek kare;
// bu noktada eTransferSrcOptimal layout'unda (WindowPrepareFrame'in CopyImage'i
// oraya birakti). Her N present'te bir yakalar, run basina ust sinir uygular.
void PsemuCaptureFrame(GraphicContext* ctx, const VulkanImage* image) {
	if (ctx == nullptr || image == nullptr || image->image == nullptr) return;
	if (image->format == vk::Format::eUndefined) return;

	const uint64_t p = g_present_seq.fetch_add(1);
	if ((p % 4ull) != 0) return;              // her 4 present'te bir readback
	if (g_shot_seq.load() >= 60) return;      // en fazla 60 DISTINCT sahne
	if (p >= 40000ull) return;                // guvenlik ust siniri

	const uint32_t w = image->extent.width;
	const uint32_t h = image->extent.height;
	if (w == 0 || h == 0) return;
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
					sig[k++] = static_cast<uint8_t>((q[0] + q[1] + q[2]) / 3u); // luma
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
			const bool rgba = (image->format == vk::Format::eR8G8B8A8Unorm ||
			                   image->format == vk::Format::eR8G8B8A8Srgb ||
			                   image->format == vk::Format::eR8G8B8A8Uint);
			const uint64_t seq = g_shot_seq.fetch_add(1);
			char fname[64];
			std::snprintf(fname, sizeof(fname), "\\frame_%05llu.bmp",
			              static_cast<unsigned long long>(seq));
			WriteBmpDownscaled(EnsureRunDir() + fname, px, w, h, rgba);
			std::fprintf(stderr, "[SHOT] SAHNE kare: %s%s (%ux%u, present#%llu, diff=%u)\n",
			             g_run_dir.c_str(), fname, w, h, static_cast<unsigned long long>(p), diff);
		}
		VulkanUnmapMemory(ctx, &readback.memory);
	}

	VulkanDeleteBuffer(ctx, &readback);
}

} // namespace Libs::Graphics
