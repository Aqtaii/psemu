@echo off
echo Derleme deneniyor...

:: Visual Studio Ortami Yukleme (Otomatik Arama)
set VCVARS_PATH=""

if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set VCVARS_PATH="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    set VCVARS_PATH="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set VCVARS_PATH="C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set VCVARS_PATH="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
)

if not %VCVARS_PATH%=="" (
    echo [+] Visual Studio ortami bulundu, ayarlar yukleniyor...
    call %VCVARS_PATH% >nul
)

:: CL (Visual Studio MSVC) kontrolu
where cl >nul 2>nul
if %errorlevel%==0 (
    echo [+] cl.exe derleyicisi aktif. (Visual Studio)
    echo Derleniyor: cl /EHsc /std:c++17 src\*.cpp /I include /link /out:loader.exe
    cl /EHsc /std:c++17 src\*.cpp /I include /link /out:loader.exe
    if %errorlevel%==0 (
        echo [+] Basariyla derlendi! "loader.exe" olusturuldu.
        if exist *.obj del *.obj
        goto :end
    ) else (
        echo [-] Derleme basarisiz oldu.
        goto :end
    )
)

echo [-] HATA: Sisteminizde cl.exe (Visual Studio) calistirilamadi!
echo Lutfen Visual Studio icerisinden 'C++ masaustu gelistirme' bilesenini kurdugunuzdan emin olun.

:end
echo.
