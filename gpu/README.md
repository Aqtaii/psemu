# psemu/gpu — GPU emülasyonu (KytyPS5'ten türetilmiştir)

Bu dizindeki `src/` (common, kernel, graphics, libs) ve `3rdparty/` kodu
**[KytyPS5](https://github.com/InoriRus/Kyty)** projesinden vendor'lanmıştır ve
**GPL-2.0** lisansı altındadır. KytyPS5 açık kaynaklı bir PlayStation 5
emülatörüdür; buradaki AGC (libSceAgc) API'si, PM4 komut işlemcisi, Vulkan host
backend'i ve PSSL→SPIR-V shader recompiler'ı ondan alınmıştır.

Bu kodun dahil edilmesiyle **tüm psemu projesi GPL-2.0** olmuştur (bkz. kök
`LICENSE`). Orijinal telif ve lisans başlıkları dosyalarda korunmuştur.

## İçerik
- `src/common`   — Kyty yardımcıları (log, assert, thread, string, memory, ABI)
- `src/kernel`   — Kyty kernel soyutlamaları (pthread, eventQueue, memory) — psemu ile köprülenir
- `src/graphics` — guest_gpu (PM4), host_gpu (Vulkan renderer), shader (PSSL→SPIR-V), presentation
- `src/libs`     — AGC / GraphicsDriver / VideoOut HLE API'leri
- `3rdparty`     — fmt, spdlog, magic_enum, xxHash, VMA, Vulkan-Headers, SPIRV-Headers, winpthread, cpuinfo, stb

## psemu'ya entegrasyon notları
- **Presentation/window**: Kyty SDL2 kullanıyor; psemu'nun Win32 penceresi +
  `vkCreateWin32SurfaceKHR` ile değiştirilecek (SDL2 vendor'lanMADI).
- **SPIRV-Tools / glslang**: Vulkan SDK'nın prebuilt kütüphaneleri kullanılır.
- **json / ffmpeg / LibAtrac9 / tracy**: graphics çekirdeği için gereksiz, atlandı.
- Guest bellek: Kyty guest pointer'ı doğrudan host pointer olarak kullanır —
  psemu ile aynı model (`g_dmem_base_addr + phys` fiziksel descriptor'lar için).
