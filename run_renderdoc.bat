@echo off
REM ============================================================
REM  loader.exe'i calistirir; TUM ciktisini (stdout + stderr)
REM  konsolda CANLI gosterir VE "loader_log.txt"e UTF-8 yazar.
REM
REM  Kullanim:
REM    run.bat PPSA02929-app0\eboot.bin
REM
REM  Nasil: 'cmd /c "loader.exe ... 2>&1"' stderr'i OS seviyesinde
REM  stdout'a birlestirir; PowerShell her satiri hem konsola
REM  ([Console]::WriteLine) hem UTF-8 StreamWriter'a (AutoFlush)
REM  yazar -> ANLIK akar, cokme aninda bile dosya tam kalir,
REM  ve dosya UTF-8'dir (grep/analiz temiz; Tee-Object'in UTF-16
REM  + null-byte sorunu YOK).
REM ============================================================
setlocal
set "LOGFILE=loader_log.txt"
if "%PSEMU_DESC_TOLERE%"=="" set "PSEMU_DESC_TOLERE=1"
if "%PSEMU_SHOTS%"=="" set "PSEMU_SHOTS=1"
if "%PSEMU_DEFAULT_HEAP_MB%"=="" set "PSEMU_DEFAULT_HEAP_MB=512"
if "%PSEMU_DMEM_EAGER_MB%"=="" set "PSEMU_DMEM_EAGER_MB=0"
if "%PSEMU_SYS_COMMIT_FLOOR_MB%"=="" set "PSEMU_SYS_COMMIT_FLOOR_MB=256"
if "%PSEMU_MEM_LIMIT_MB%"=="" set "PSEMU_MEM_LIMIT_MB=28672"
if "%PSEMU_MEM_FLOOR_MB%"=="" set "PSEMU_MEM_FLOOR_MB=256"
if "%PSEMU_DIRECT_MEM_MB%"=="" set "PSEMU_DIRECT_MEM_MB=18432"

set "TARGET=%~1"
if /I "%TARGET%"=="astrobot" set "TARGET=D:\proje\psgame\[SuperPSX]-ASTRO.BOT-PPSA21564-USA-Game-v01.007.000-PS5\PPSA21564-app\eboot.bin"
if "%TARGET%"=="" set "TARGET=D:\proje\psgame\Stray\PPSA02100-app\eboot.bin"

set "CAP_PREFIX=stray_frame"
echo %TARGET% | findstr /i "PPSA21564 ASTRO" >nul && set "CAP_PREFIX=astrobot_frame"

set "RENDERDOC_PATH=D:\Renderdoc"
if not exist "captures" mkdir "captures"

echo [RenderDoc] RenderDoc baglaniyor... (D:\Renderdoc\renderdoccmd.exe)
echo [RenderDoc] Hedef: %TARGET%
echo [RenderDoc] Yakalama On Eki: %CAP_PREFIX%
echo [RenderDoc] F12 veya PrintScreen tusuna basarak kare yakalayabilirsiniz.
echo [RenderDoc] Yakalanan kareler captures\ klasorune duser.
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$sw=[System.IO.StreamWriter]::new('%LOGFILE%',$false,[System.Text.Encoding]::UTF8); $sw.AutoFlush=$true; try { cmd /c 'D:\Renderdoc\renderdoccmd.exe capture --working-dir d:\proje\psemu --capture-file d:\proje\psemu\captures\%CAP_PREFIX% d:\proje\psemu\loader.exe \"%TARGET%\" 2>&1' | ForEach-Object { [Console]::WriteLine($_); $sw.WriteLine($_) } } finally { $sw.Close() }"

echo.
echo ============================================================
echo [run_renderdoc.bat] Bitti. Tam log: %LOGFILE%
echo ============================================================
endlocal
