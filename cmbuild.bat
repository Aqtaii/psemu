@echo off
set VULKAN_SDK=C:\VulkanSDK\1.4.350.0
call "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat"
if not exist build mkdir build
cd build
cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release ..
if %errorlevel% neq 0 exit /b %errorlevel%
ninja
if %errorlevel% neq 0 exit /b %errorlevel%
