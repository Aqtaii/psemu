# Pencere ve tam ekran kontrolleri

Oyun penceresi odaktayken **F11** veya **Alt+Enter** ile kenarliksiz tam ekrana gecilir. Ayni kisayol pencere moduna doner. Pencerenin onceki konumu, boyutu ve buyutulmus durumu saklanir. Tam ekran, pencerenin bulundugu monitoru kaplar.

Alt+Enter oyuna onay/ziplama girdisi olarak aktarilmaz. Tusu basili tutmak ardisik tam ekran gecisleri olusturmaz. Alt+F4 mevcut kapatma davranisini korur.

Boyut degisikligi Win32 mesajinda kaydedilir; Vulkan swapchain yenilemesi sunum thread'inde yapilir. Simge durumundayken sifir boyutlu swapchain olusturulmaz. Oyun cozunurlugu veya FPS limiti bu ozellik tarafindan degistirilmez.

Yerel Dreaming Sarah paketi, kurulu oldugu kaydedilmis cb3df70918cc0b159b978a1acbe26f1b814f461e kaynak surumune yalnizca bu pencere degisikligi uygulanarak derlenir. Kaynak kopyasi, araclar, yedekler ve derlemeler D: altindadir. Kullaniciya ait calistirma betikleri korunur. Guncel kaynak agaci da ayni pencere degisikligiyle derlenmistir.

Windows pencere stili yenilemesi ve konum saklama kurallari: [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos), [GetWindowPlacement](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowplacement).

Dogrulama: Sarah paketi ve guncel kaynak agaci Windows derlemelerini gecti. Kullanici F11/Alt+Enter ile tam ekran ve geri donusun calistigini dogruladi. Yerel logda giris/cikis kayitlari var. Kurulu loader yedegi tools/backups/sarah_before_fullscreen_20261101_235552 altindadir.
