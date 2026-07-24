# ============================================================================
# psemu/gpu — KytyPS5'ten vendor'lanan GPU stack (GPL-2.0)
# ----------------------------------------------------------------------------
# Faz 2b PROBE: once yalnizca `common`'i (temel: log/assert/thread/abi/string)
# statik lib olarak derlemeye calisiyoruz. Kyty clang-cl hedefliyor; MSVC ile
# ilk uyumsuzluklari burada gorup cozecegiz. Graphics/libs sonra eklenecek.
# ============================================================================

set(GPU_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(GPU_TP  ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)

# --- ortak include yollari (Kyty src/CMakeLists'ten) ------------------------
set(GPU_INCLUDES
    ${GPU_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated   # cmake_config.h (Kyty configure_file karsiligi)
    ${GPU_TP}/fmt/include
    ${GPU_TP}/spdlog/include
    ${GPU_TP}/magic_enum/include/magic_enum
    ${GPU_TP}/xxHash
    ${GPU_TP}/VulkanMemoryAllocator/include
    ${GPU_TP}/Vulkan-Headers/include
    ${GPU_TP}/SPIRV-Headers/include
    ${GPU_TP}/tracy/public          # tracy/Tracy.hpp (TRACY_ENABLE yoksa no-op)
    ${GPU_TP}/cpuinfo/include        # cpuinfo.h (systemInfo.cpp)
    ${GPU_TP}/winpthread/include     # pthread.h (Windows pthread emulasyonu)
    ${GPU_TP}/nlohmann_json/single_include  # json.hpp (loader/systemContent.cpp)
    $ENV{VULKAN_SDK}/Include         # SPIRV-Tools C++ API (spirv-tools/...)
)

# fmt'yi header-only kullan (ayri derleme derdi olmasin — probe icin yeterli).
add_compile_definitions(FMT_HEADER_ONLY=1)
# Tracy profiler: Kyty kodu tracy tiplerini (SourceLocationData vb.) bekliyor;
# ENABLE tanimli olmali. ON_DEMAND = bir profiler baglanana kadar neredeyse
# sifir maliyet (bizde kimse baglanmayacak, sorun cikarmaz).
add_compile_definitions(TRACY_ENABLE=1 TRACY_ON_DEMAND=1 TRACY_DELAYED_INIT=1 TRACY_MANUAL_LIFETIME=1)
# winpthread kendi 'timespec'ini tanimlamaya calisiyor; UCRT zaten tanimliyor
# ama _TIMESPEC_DEFINED guard'ini set etmiyor. Onceden tanimlayip winpthread'in
# yeniden tanimini engelliyoruz (UCRT farkli guard kullandigi icin etkilenmez).
add_compile_definitions(_TIMESPEC_DEFINED=1)
# xxHash'i header-only kullan (XXH3_64bits inline gelsin; ayri .c linki gerekmez).
add_compile_definitions(XXH_INLINE_ALL=1)
# Win32 Vulkan surface (vkCreateWin32SurfaceKHR + VkWin32SurfaceCreateInfoKHR).
add_compile_definitions(VK_USE_PLATFORM_WIN32_KHR=1)

# --- kaynak kumeleri --------------------------------------------------------
# common + kernel + graphics (presentation/window HARIC — SDL'e bagli, Win32'ye
# yeniden yazilacak) + sadece gereken libs (agc/gfxdriver/videoout).
file(GLOB_RECURSE GPU_COMMON_SRC   CONFIGURE_DEPENDS ${GPU_SRC}/common/*.cpp)
file(GLOB_RECURSE GPU_KERNEL_SRC   CONFIGURE_DEPENDS ${GPU_SRC}/kernel/*.cpp)
file(GLOB_RECURSE GPU_GRAPHICS_SRC CONFIGURE_DEPENDS ${GPU_SRC}/graphics/*.cpp)

set(GPU_LIBS_SRC
    ${GPU_SRC}/libs/agc.cpp
    ${GPU_SRC}/libs/libGraphicsDriver.cpp
    ${GPU_SRC}/libs/libVideoOut.cpp
    ${GPU_SRC}/libs/libPad.cpp
    ${GPU_SRC}/libs/controller.cpp
)

# loader/'den yalnizca graphics/libs'in ihtiyac duydugu yardimcilar (timer,
# symbolDatabase, systemContent). elf/runtimeLinker/x64emu HARIC (psemu'nun
# kendi loader'iyla cakisir + gereksiz).
set(GPU_LOADER_SRC
    ${GPU_SRC}/loader/timer.cpp
    ${GPU_SRC}/loader/symbolDatabase.cpp
    ${GPU_SRC}/loader/systemContent.cpp
)

set(GPU_ALL_SRC ${GPU_COMMON_SRC} ${GPU_KERNEL_SRC} ${GPU_GRAPHICS_SRC} ${GPU_LIBS_SRC} ${GPU_LOADER_SRC})
# Linux platform dosyalari cikar. presentation'da SADECE SDL'e bagli olanlari
# (window/*, renderDoc) cikar; videoOut.cpp SDL'siz -> DERLE (33 VideoOut +
# DisplayBuffer mantigini saglar; Win32 pencere katmanini ona gore yazacagiz).
list(FILTER GPU_ALL_SRC EXCLUDE REGEX ".*[/\\]platform[/\\]sysLinux.*\\.cpp$")
# window.cpp (175 SDL, input+game-loop) -> window_win32.cpp ile degistirildi.
# vulkanWindow.cpp + swapchain.cpp (SDL -> Win32'ye port edildi) DERLENIR.
list(FILTER GPU_ALL_SRC EXCLUDE REGEX ".*[/\\]presentation[/\\]window[/\\]window\\.cpp$")
list(FILTER GPU_ALL_SRC EXCLUDE REGEX ".*[/\\]presentation[/\\]renderDoc\\.cpp$")

# Tracy client (amalgamated tek dosya) — profiler.h'in tracy tiplerini saglar.
list(APPEND GPU_ALL_SRC ${GPU_TP}/tracy/public/TracyClient.cpp)
# psemu adapteri (cpuinfo/network/renderdoc stub'lari + Win32 pencere katmani).
list(APPEND GPU_ALL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/adapter/stubs.cpp)
list(APPEND GPU_ALL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/adapter/window_win32.cpp)
list(APPEND GPU_ALL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/adapter/init.cpp)
list(APPEND GPU_ALL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/adapter/agc_bridge.cpp)
list(APPEND GPU_ALL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/adapter/screenshot.cpp)

add_library(psemu_gpu STATIC ${GPU_ALL_SRC})
target_include_directories(psemu_gpu PUBLIC ${GPU_INCLUDES})
target_compile_options(psemu_gpu PRIVATE /EHsc /utf-8 /bigobj /W0)
target_compile_definitions(psemu_gpu PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

# --- link bagimliliklari (loader'a propagate olsun diye PUBLIC) --------------
find_package(Vulkan REQUIRED)
target_link_libraries(psemu_gpu PUBLIC
    Vulkan::Vulkan
    "$ENV{VULKAN_SDK}/Lib/SPIRV-Tools.lib"
    "$ENV{VULKAN_SDK}/Lib/SPIRV-Tools-opt.lib"
    "$ENV{VULKAN_SDK}/Lib/SPIRV-Tools-link.lib"
    # winpthread (MinGW prebuilt import lib) — pthread_* + clock_gettime saglar.
    # lld-link C-ABI sembolleri icin GNU import lib'ini okuyabilir; calismazsa
    # shim'e gecilir. Runtime'da libwinpthread-1.dll gerekir (bin/).
    "${GPU_TP}/winpthread/lib/libwinpthread.dll.a"
    # onecore: VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFile2 (Kyty bellek yon.)
    onecore
)
