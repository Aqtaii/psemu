# ============================================================================
# psemu — PS5 emulator (KytyPS5 GPU port entegrasyonu icin CMake build)
# ----------------------------------------------------------------------------
# Onceki build tek komuttu: cl /EHsc /std:c++17 src\*.cpp /I include.
# Kyty'nin graphics'ini (Vulkan + spirv-tools + ~58k satir) eklemek icin
# gercek bir build sistemine geciyoruz. Bu BASELINE: once psemu'yu oldugu gibi
# CMake ile derleyip loader.exe'nin bozulmadigini dogruluyoruz; Kyty graphics
# sonraki adimlarda ARTIMLI eklenecek.
#
# NOT: KytyPS5 kodu GPL-2.0. Onu dahil ettigimiz an psemu da GPL-2.0 olur.
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(psemu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)          # Kyty C++20 istiyor; psemu C++17 kodu uyumlu
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_definitions(-D_CRT_SECURE_NO_WARNINGS)

# loader.exe repo kokune ciksin ki mevcut run.bat degismeden calissin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

# --- psemu cekirdegi (mevcut kaynaklar) -------------------------------------
file(GLOB PSEMU_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

add_executable(loader ${PSEMU_SOURCES})
target_include_directories(loader PRIVATE ${CMAKE_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(loader PRIVATE /EHsc /utf-8 /bigobj)
    # psemu cekirdegi build.bat'te MSVC /Od (optimizasyonsuz) ile derleniyordu
    # ve oyunu tam boot ediyordu. clang-cl Release -O2 ise dusuk-seviye kodu
    # (VEH exception-skip, runtime trampolinler, FS-segment TLS hotfix,
    # type-punning) bozup oyunu erken cokertiyor (UB, -O2'de patliyor).
    # Optimizasyonu kapatiyoruz; release CRT (-MD, IDL=0) korunuyor ki gpu/SDK
    # lib'leriyle linklenebilsin. Strict-aliasing UB'sini de kapatiyoruz.
    target_compile_options(loader PRIVATE /Od /clang:-fno-strict-aliasing)
endif()

# user32/gdi32 zaten video.cpp icinde #pragma comment(lib,...) ile linkleniyor;
# yine de acikca belirtelim (CMake linkeri icin guvenli).
target_link_libraries(loader PRIVATE user32 gdi32)

# --- Vulkan (SU AN sadece bulunabilirligi dogrulaniyor; henuz linklenmiyor) --
# Kyty graphics eklenince target'a baglanacak. Baseline'i kirmamak icin simdi
# yalnizca tespit ediyoruz.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "[psemu] Vulkan bulundu: ${Vulkan_INCLUDE_DIRS}")
else()
    message(WARNING "[psemu] Vulkan bulunamadi — VULKAN_SDK ayarli mi?")
endif()

# --- GPU stack (KytyPS5 port, GPL-2.0) --------------------------------------
add_subdirectory(gpu)

# Faz 3: psemu_gpu loader'a bagli. WHOLEARCHIVE ile TUM sembol yuzeyini
# dogruladik (84->0 tanimsiz). ARTIK KALDIRILDI: WHOLEARCHIVE tum GPU objelerini
# (+ statik baslaticilari: tracy thread, Kyty subsystem'leri, bellek yon.
# override'lari) zorla dahil edip psemu'nun bellek duzenini bozuyor ve oyun
# erken cokuyordu. Referanssiz link'te linker yalnizca cagrilan objeleri ceker;
# psemu henuz GPU cagirmadigi icin loader.exe eski davranisina doner. GPU'yu
# cagirmaya basladigimizda ilgili objeler ARTIMLI cekilecek.
target_link_libraries(loader PRIVATE psemu_gpu)
