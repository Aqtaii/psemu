# Astro Bot ilerleme planı — 10 Ekim 2026

## Doğrulanmış başlangıç
Referans: KytyPS5 307fdcf7ad5d9d15b5966288fcc598ac9dbca188 yerel kaynakları.
Bu karşılaştırma bu commit ile sınırlıdır; KytyPS5 üzerinde Astro Bot oynanabilirliği doğrulanmış değildir.
psemu: a9b2046; BVH decoder değişikliği 9f3375c.
Son gerçek Astro koşusu, önceki shader engellerini geçip CS 0x1eeaf3c1000 / PC 0x2190 üzerindeki IMAGE_BVH_INTERSECT_RAY komutunda durdu. İlk görüntü henüz doğrulanmadı.
Decoder regresyonları geçti; 38075191133 numaralı tam Windows derlemesi plan hazırlanırken devam ediyordu. Decoder eklentisi tek başına çalışabilir BVH desteği sağlamaz.

## Aşamalar ve bitirme koşulları

1. **BVH komutunu ara temsile taşı.** KytyPS5 frontend/translate/Memory.cpp ve ResourceTracking.cpp davranışını bizim ShaderIRMemory.cpp ve kaynak takibine uyarla. Dört canlı descriptor sözcüğü, 11/12 ışın sözcüğü, 32/64 bit düğüm adresi ve EXEC maskesi korunmalı. Normal texture descriptor çözümlemesine girmemeli. Tamamlanma: yakalanan beş DWORD'luk komutun decoder→IR testi; NSA, örtüşen giriş/çıkış registerları ve pasif lane koruması.

2. **GPU konuk bellek adreslemesini kur.** KytyPS5 GetBdaPointer, bufferCache BDA sayfa tablosu ve descriptor bağlamalarını referans al. Önce mevcut Vulkan özelliklerini kontrol et; BDA/shaderInt64 desteği ve etkinleştirmesi uygunsa bu yolu uyarla, alternatif gerekiyorsa aynı semantiği sağlayan SSBO sayfa tablosunu ayrıca tasarla. Sabit 64 KB FLAT penceresini genişletmek tek başına yeterli değil. Eşlemelerin güncellenmesi, GPU komut ömrü, buffer bariyerleri ve eksik sayfa davranışı birlikte ele alınmalı. Tamamlanma: bitişik olmayan backing, sayfa sınırını geçen iki 64 bayt blok, yüksek adres, taşma ve yeniden eşleme testleri; geçersiz GPU erişimi olmaması.

3. **Gerçek BVH kesişim hesabını ekle.** backend/spirv/spirvEmitterBvh.cpp içindeki üçgen ve FP16/FP32 kutu hesaplarını mevcut emitter'a uyarla. Çocuk sıralama, barycentric/triangle-id çıkışları, büyütme katsayısı, ışın mesafe sınırı ve geçersiz girdiler dahil edilmeli. Doğrudan alınan kaynakların telif/lisans bildirimleri korunmalı. Tamamlanma: ShaderRayTracingGpuTests.inc::BvhIntersections içindeki 32 lane ve geniş adres varyantlarının beklenen sonuçları, SPIR-V doğrulaması ve mevcut regresyonlar. Sabit sıfır veya her zaman ıskalama sonucu başarı sayılmaz.

4. **Astro'da BVH engelini geçtiğini doğrula.** Tam Windows derlemesini indir, manifest ve commit doğrula, mevcut loader'ı D: üzerinde yedekle ve yeni sürümü çalıştır. Tamamlanma: aynı shader'ın derlenmesi ve dispatch'in tamamlanması; sonraki log noktası, varsa yeni hata ve ekran durumu kaydedilmiş olmalı. Shader'ın derlenmesi tek başına görüntü alındığı anlamına gelmez.

5. **İlk görüntüye kadar kalan uyumluluk farklarını kapat.** Sonraki gerçek hataları KytyPS5 decoder→IR→SPIR-V ve test zinciriyle karşılaştır. PM4, GDS/DMA, kaynak biçimleri, senkronizasyon ve sunum yolunu hata kanıtına göre ele al. Shader dump'larında kullanılan eksik opcode ailelerini topluca tarayarak aynı sınıftaki hataları birlikte düzelt. Önceki tablo denetiminde VOP1 15, VOPC 82, VOP3 13 upstream girdisi eksikti; bunlar ayrı ayrı Astro gereksinimi veya tamamlanma yüzdesi değildir. AGC fonksiyon imzası farklarını NID ve gerçek çağrı ile doğrula. Tamamlanma: oyundan tanınabilir ilk kare ve tekrarlanabilir açılış.

6. **Menü ve oynanabilirlik.** İlk kareden sonra giriş, kayıt, dosya erişimi, ses, sahne geçişi ve uzun çalışmada bellek yaşam döngüsünü test et. Tamamlanma: üç soğuk açılış, menüye ulaşma, kontrol edilebilir oyun ve en az 10 dakika çökmeden çalışma. Ancak ardından FPS ve yükleme optimizasyonuna geç.

## İlerleme nasıl raporlanacak?
Her aşamada commit, referans kaynak/test, geçen kontroller ve gerçek oyun sonucu kaydedilecek. Başarı sırası: BVH testleri → Astro dispatch → ilk kare → menü → kontrol edilebilir sahne → kararlılık. İlk görüntü için tarih veya yüzde vermek, BVH sonrasında hangi engellerin çıkacağı bilinmediğinden şu anda güvenilir değil.

Dreaming Sarah takılmaları kullanıcı isteğiyle ertelendi. İndirilen paketler, çıkarılan derlemeler ve loglar D:/proje/psemu-codex altında tutulacak. Sarah'nın mevcut sınırsız FPS/doğrudan oyun başlangıcı ayarları korunacak.
