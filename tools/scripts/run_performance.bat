@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Usage: tools\scripts\run_performance.bat fast or profile
if /i "%~1"=="fast" goto :fast
if /i "%~1"=="profile" goto :profile
echo Usage: %~nx0 fast ^| profile
exit /b 2
:fast
set "PSEMU_HLE_PROFILE=0"
set "PSEMU_PLT_TOP=0"
set "PSEMU_PERF_MODE=fast"
goto :run
:profile
set "PSEMU_HLE_PROFILE=1"
set "PSEMU_PLT_TOP=1"
set "PSEMU_PERF_MODE=profile"
:run
pushd "%~dp0..\.." || exit /b 1
if not exist loader.exe (
    echo [psemu] loader.exe not found. Build the emulator first.
    set "PSEMU_RESULT=1"
    goto :done
)
if not exist tools\data mkdir tools\data
if not exist tools\data (
    set "PSEMU_RESULT=1"
    goto :done
)
echo [psemu] Mode: %PSEMU_PERF_MODE%. Log: tools\data\perf_%PSEMU_PERF_MODE%_log.txt
echo [psemu] Close the game window to finish. The existing loader_log.txt is preserved.
loader.exe PPSA02929-app0\eboot.bin >"tools\data\perf_%PSEMU_PERF_MODE%_log.txt" 2>&1
set "PSEMU_RESULT=%errorlevel%"
:done
popd
exit /b %PSEMU_RESULT%
