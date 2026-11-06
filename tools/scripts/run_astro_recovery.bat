@echo off
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0..\.." || exit /b 1
if not exist loader.exe (
 echo [psemu] loader.exe bulunamadi.
 popd
 exit /b 1
)
if not exist "PPSA21564-app\eboot.bin" (
 echo [psemu] PPSA21564-app\eboot.bin bulunamadi.
 popd
 exit /b 1
)
set "TEMP=D:\Tools\psemu-build\temp"
set "TMP=%TEMP%"
set "APPDATA=D:\Tools\psemu-build\state\AppData"
set "LOCALAPPDATA=D:\Tools\psemu-build\state\LocalAppData"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%APPDATA%" mkdir "%APPDATA%"
if not exist "%LOCALAPPDATA%" mkdir "%LOCALAPPDATA%"
set "PSEMU_ASTRO_FAST_TEST="
set "PSEMU_NATIVE_CLOCK=1"
set "PSEMU_NATIVE_SEMA=1"
set "PSEMU_NATIVE_HTTP=1"
set "PSEMU_ASTRO_STATIC_TLS=1"
set "PSEMU_DMEM_FILE_BACKED=1"
set "PSEMU_NATIVE_FIBER=1"
set "PSEMU_NATIVE_PLT=1"
set "PSEMU_NATIVE_RWLOCK=1"
set "PSEMU_ASTRO_NULL_RESOURCE_PROBE=1"
set "PSEMU_ASTRO_MESH_CAPTURE_FAILURE=1"
set "PSEMU_ASTRO_MESH=1"
set "PSEMU_GPU_SCALAR_READS=1"
set "PSEMU_DRAW_DIAGNOSTICS=0"
set "PSEMU_SHOTS=1"
set "PSEMU_GPU_LOG=0"
set "PSEMU_GFX_TRACE=0"
set "PSEMU_HLE_PROFILE=0"
set "PSEMU_PLT_TOP=0"
set "PSEMU_UNCAP_FPS="
set "PSEMU_ASTRO_SCENE_PROBE="
set "PSEMU_ASTRO_PRODUCER_BUFFER_PROBE="
set "PSEMU_ASTRO_MESH_OUTPUT_PROBE="
set "PSEMU_ASTRO_COVERAGE_PROBE="
set "PSEMU_ASTRO_PIXEL_PROBE="
set "PSEMU_ASTRO_DEPTH_PROBE="
set "PSEMU_ASTRO_PROBE_GATE="
if not exist tools\data mkdir tools\data
for /f %%I in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "_PSEMU_ASTRO_RUN=%%I"
set "_PSEMU_ASTRO_LOG=tools\data\astro_recovery_%_PSEMU_ASTRO_RUN%.txt"
echo [psemu] Astro normal acilis. Log: %_PSEMU_ASTRO_LOG%
echo [psemu] Testi bitirmek icin oyun penceresini kapatin.
loader.exe PPSA21564-app\eboot.bin >"%_PSEMU_ASTRO_LOG%" 2>&1
set "_PSEMU_ASTRO_RESULT=%errorlevel%"
popd
exit /b %_PSEMU_ASTRO_RESULT%
