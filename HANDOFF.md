# psemu — GPU Bring-up Devir Notu (2026-07-23)

PS5 HLE emülatörü. Hedef oyun: **PPSA02929 "Dreaming Sarah"** (Construct 2), `PPSA02929-app0\eboot.bin`.
KytyPS5'in GPL-2.0 GPU stack'i (AGC + PM4 + PSSL/RDNA2→SPIR-V + Vulkan) `gpu/` altında port edildi.

## Şu an ÇALIŞAN
- Oyun tam boot oluyor, **çökmüyor**.
- Kyty GPU stack entegre + Vulkan (Intel Arc B580) init.
- **RATALAIKA GAMES splash MÜKEMMEL render oluyor** (screenshot ile doğrulandı — `images/run_.../frame_00000.bmp`).
- Oyun ilerliyor: intro → save-data kontrolü (8 slot) → string/locale → title/menü mantığı. 14k+ frame render ediyor.
- **Flip-event pacing çalışıyor** (Kyty-native equeue): `sceKernelCreateEqueue`→Kyty `KernelCreateEqueue` (core.cpp:1252), `sceKernelWaitEqueue`→Kyty `KernelWaitEqueue` (core.cpp:1259). Oyun input POLL etmiyor (PadReadState sadece ~9× erken) — intro input-gated DEĞİL.
- **Screenshot sistemi** (`gpu/adapter/screenshot.cpp`): present edilen Vulkan frame'ini geri okuyup `images/run_<zaman>/frame_XXXXX.bmp` olarak kaydeder (4x küçültme, **sahne-değişikliği tespiti** — her distinct sahneyi yakalar). WindowPresentFrame'den çağrılır (swapchain.cpp:~484).

## TEK BLOKER: baskın sprite/yazı shader'ının V#'leri SIFIR
Dreaming Sarah içeriği (yazı/sprite) render OLMUYOR → git-gel/üçgen-kesik/karanlık görünüyor.

**Kesin teşhis (`[VSHARP0]` diag, shader.cpp:648):**
- İki VS shader var:
  - **ÇALIŞAN** quad shader: V# = `b8103190 0010022c 00000004 0004022c`, base geçerli, stride=16, numrec=4, **format=0x40=k32_32Float**. Düzgün render (title-image quad, splash).
  - **BOZUK** baskın shader (46314 draw, 3 attr): V# = `00000000 00000000 00000000 00000000` (base=0 stride=0 **numrec=0** format=0). `buffer_ptr` GEÇERLİ (or. 0x1C91C80CB70) ama işaret ettiği memory SIFIR → numrec=0 → 0 vertex → hiçbir şey çizilmiyor.
- Sayfa `page_state=MEM_COMMIT prot=PAGE_READONLY` (Kyty PageManager write-tracking). V# offset'i sıfır ama **çevrede gerçek veri VAR** (buffer_ptr+156dw / −8dw'de nonzero).

**Eleme:**
- RDNA2 disasm (`buffer_load_format_xy v0,v0,s0,s10; dfmt=0 nfmt=0 formatted=1`) → **shader çevirisi DOĞRU**, format V#'den gelir. "Shaderları RDNA2'ye göre çevir" fikri geçersiz — Kyty zaten doğru çeviriyor.
- `Format()` bitleri doğru okuyor (shaderBindings.h:64, bits[18:12]) — encoding bug'ı değil.
- `vertex_buffer_reg` AGC metadata'sından (shaderVertexMetadata.cpp:54) — register seçimi muhtemelen doğru.
- **Denendi ama İŞE YARAMADI:** readonly-page write fix (core.cpp ~1130, committed-readonly WRITE fault'ta VirtualProtect RW). Sadece 3× tetiklendi, V#'ler hâlâ sıfır. Zararsız (çökme yok), kaldı.

**En güçlü 2 hipotez (V# neden sıfır):**
1. **Stale/yanlış user_sgpr pointer** — buffer_ptr geri dönüştürülmüş/eski bir scratch slot'unu gösteriyor (artık sıfır); gerçek V#'ler çevredeki nonzero veri (+156dw). Kyty'nin `gs_user_sgpr` snapshot'ı yanlış/eski DCB SET_SH_REG durumunu okuyor olabilir.
2. **GPU-üretimli V#'ler** — oyun V# tablosunu GPU ile yazıyor, biz CPU'dan coherency-sync'siz okuyoruz.

## SONRAKİ ADIMLAR (öncelik sırası)
1. **Screenshot ile Dreaming Sarah'yı gör:** çalıştır, sahne gelene kadar bekle, `images/run_<en-yeni>/` içindeki kareleri PNG'ye çevirip incele (bkz. aşağıda). Gerçek render halini gör.
2. **V# neden sıfır bul:**
   - buffer_ptr'a (or. 0x1C91C80CB70) **write-watchpoint/write-log** koy → oyun V#'leri yazıyor mu, NEREDEN?
   - `user_sgpr[vertex_buffer_reg]` değerini DCB'nin gerçekte SET_SH_REG ile set ettiğiyle karşılaştır (stale mı?).
   - Çevredeki nonzero veriyi (buffer_ptr+156dw) dök → geçerli bir V# mi (base/stride/numrec/format)? Öyleyse pointer stale = hipotez 1 doğru.
3. **KytyPS5 ile karşılaştır:** scratchpad'deki KytyPS5 klonunda AYNI oyunu çalıştır; Kyty aynı shader için hangi buffer_ptr/V# okuyor? Fark bizim entegrasyonda.

## Diagnostic'ler (iş bitince TEMİZLE)
- `[VSHARP0]` — `gpu/src/graphics/shader/shader.cpp:648` (V# dump + page state).
- `[SHOT]` — `gpu/adapter/screenshot.cpp` (screenshot; kalabilir, faydalı).
- `[MEM-RO->RW]` — `src/core.cpp` ~1130 (readonly write fix; zararsız).
- `PsemuNotifyKytyFlip()` — `src/core.cpp:4059` boş stub (Kyty-native equeue kullanıldığı için gereksiz; `gpu/.../videoOut.cpp` no-op çağırıyor).

## Build & Çalıştırma
- **Build:** `scratchpad/cmbuild.bat` (clang-cl). Working dir `D:\proje\psemu`.
  - `"permission denied" loader.exe` hatası → önce çalışan loader'ı durdur: PowerShell `Get-Process loader -EA SilentlyContinue | Stop-Process -Force`, sonra tekrar build.
- **Çalıştır:** kullanıcı `run.bat PPSA02929-app0\eboot.bin` → `loader_log.txt` üretir.
- **Screenshot'ları görmek:** BMP'ler `images/run_<zaman>/`. Read tool BMP okuyamaz → PNG'ye çevir:
  ```powershell
  Add-Type -AssemblyName System.Drawing
  $img=[System.Drawing.Image]::FromFile("D:\proje\psemu\images\run_XXX\frame_00000.bmp")
  $img.Save("$env:TEMP\f0.png",[System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
  ```
  Sonra PNG'yi Read ile aç.

## Önemli kısıtlar
- `PPSA02929-app0/`, `eboot.bin`, `loader.exe` GitHub'a **ASLA** push edilmez (telif + artifact; .gitignore'da).
- psemu artık **GPL-2.0** (Kyty vendored).
- Kanıta-dayalı debug: psemu fix'i önermeden önce disassembly/watchpoint/KytyPS5 ile DOĞRULA, tahmin etme.

## Anahtar dosyalar
- `src/core.cpp` — psemu VEH, PLT-hook HLE dispatch, memory, equeue routing, Pad HLE.
- `src/agc.cpp` + `gpu/adapter/agc_bridge.cpp` — AGC→Kyty routing (NID→Kyty func, SysV trampoline).
- `gpu/adapter/window_win32.cpp` — Win32 pencere + WindowRun (VideoOutFlipWindow ile present drain).
- `gpu/adapter/init.cpp` — Kyty subsystem init.
- `gpu/adapter/screenshot.cpp` — screenshot.
- `gpu/src/graphics/shader/shader.cpp` — VS/PS input info, V# okuma (ShaderApplyAttribSemantics:613, ShaderGetInputInfoVS:738).
- `gpu/src/graphics/host_gpu/renderer/shaders.cpp` — GetInputFormat (fmt=0 handling:337), pipeline.
- Detaylı geçmiş: `.claude` memory dosyaları (özellikle `gpu_port_kyty.md`).
