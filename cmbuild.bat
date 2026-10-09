@echo off
setlocal EnableExtensions DisableDelayedExpansion
pushd "%~dp0" || exit /b 1

rem Use an existing developer environment, or discover Visual Studio with vswhere.
where cl.exe >nul 2>nul
if not errorlevel 1 goto :tools
if defined PSEMU_VCVARS64 goto :load_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :missing_vs
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PSEMU_VCVARS64=%%I\VC\Auxiliary\Build\vcvars64.bat"
:load_vs
if not defined PSEMU_VCVARS64 goto :missing_vs
if not exist "%PSEMU_VCVARS64%" goto :missing_vs
call "%PSEMU_VCVARS64%"
if errorlevel 1 goto :failed

:tools
for %%T in (cmake.exe ninja.exe clang-cl.exe) do (
    where %%T >nul 2>nul
    if errorlevel 1 (
        echo [psemu] Missing %%T. Add it to PATH and retry.
        goto :failed
    )
)
if not defined VULKAN_SDK (
    echo [psemu] Set VULKAN_SDK to your Vulkan SDK installation.
    goto :failed
)
if not exist "%VULKAN_SDK%\Include\spirv-tools\libspirv.hpp" (
    echo [psemu] Vulkan SDK SPIRV-Tools headers are missing.
    goto :failed
)
for %%L in (SPIRV-Tools SPIRV-Tools-opt SPIRV-Tools-link) do (
    if not exist "%VULKAN_SDK%\Lib\%%L.lib" (
        echo [psemu] Missing Vulkan SDK library %%L.lib.
        goto :failed
    )
)
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :failed
cmake --build build --parallel %*
set "PSEMU_BUILD_RESULT=%errorlevel%"
goto :done

:missing_vs
echo [psemu] Visual Studio C++ tools were not found. Use an x64 Developer Command Prompt or set PSEMU_VCVARS64.
:failed
set "PSEMU_BUILD_RESULT=1"
:done
popd
exit /b %PSEMU_BUILD_RESULT%
