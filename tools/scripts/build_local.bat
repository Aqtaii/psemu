@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "PSEMU_LOCAL_SOURCE=%~dp0..\.."
for %%I in ("%PSEMU_LOCAL_SOURCE%") do set "PSEMU_LOCAL_SOURCE=%%~fI"
if /i not "%PSEMU_LOCAL_SOURCE:~0,3%"=="D:\" (
 echo [psemu] Local source and build files must be on D:.
 exit /b 1
)
if not exist "D:\Tools\psemu-build\env.cmd" (
 echo [psemu] Portable tools not found: D:\Tools\psemu-build
 exit /b 1
)
call "D:\Tools\psemu-build\env.cmd"
if errorlevel 1 exit /b 1
set "PSEMU_LOCAL_OUTPUT=%PSEMU_TOOLCHAIN_ROOT%\output"
cmake -S "%PSEMU_LOCAL_SOURCE%" -B "%PSEMU_LOCAL_SOURCE%\build-local" -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=%PSEMU_LOCAL_OUTPUT%"
if errorlevel 1 exit /b 1
cmake --build "%PSEMU_LOCAL_SOURCE%\build-local" --parallel 6 %*
if errorlevel 1 exit /b 1
copy /y "%PSEMU_LOCAL_SOURCE%\gpu\3rdparty\winpthread\bin\libwinpthread-1.dll" "%PSEMU_LOCAL_OUTPUT%\" >nul
if errorlevel 1 exit /b 1
for /r "%PSEMU_TOOLCHAIN_ROOT%\msvc\VC\Redist" %%I in (*.dll) do (
 echo %%~dpI | findstr /i "x64" >nul
 if not errorlevel 1 copy /y "%%I" "%PSEMU_LOCAL_OUTPUT%\" >nul
)
echo [psemu] Local executable: %PSEMU_LOCAL_OUTPUT%\loader.exe
endlocal
exit /b 0
