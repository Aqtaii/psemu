# Portable local Windows build

Tools, downloads, caches and temporary files live in D:/Tools/psemu-build. No Visual Studio system installer, MSI installation, Vulkan installer/helper or registry registration was run. MSVC/Windows SDK payloads came from the official Microsoft Visual Studio manifest and were extracted with the audited portablemsvc source 92554c094a1d00abef1cfb610877edcb1eb325f3 using its Python MSI backend. Vulkan SDK embedded 7z archives were extracted directly; Helpers were excluded.

Versions: LLVM/clang-cl/lld 20.1.8, MSVC headers/libraries 14.44.35207, Windows SDK 10.0.26100.0, Vulkan SDK 1.4.350.0, CMake 4.4.4, Ninja 1.13.2. installed-tools.json and portablemsvc.lock under the tool root record package provenance.

From D:/proje/psemu run tools\scripts\build_local.bat. It activates process-local environment variables, uses six parallel compilation jobs, writes the CMake cache under build-local and the executable/DLLs under D:/Tools/psemu-build/output. The existing project-root loader.exe remains unchanged. CMake accepts an explicit CMAKE_RUNTIME_OUTPUT_DIRECTORY; its default remains the project root.

Measured on Ryzen 5 5600X: first complete 138-step build 98.33 seconds, unchanged-source repeat 0.97 seconds (Ninja: no work to do). The repeat is not a measurement of a changed-source rebuild.

Validation: bounded string copy with windows.h min/max macros enabled, save-transfer errors and numeric parser tests passed locally. The built executable passed --fiber-self-test with native NID lookup, 64 yields/resumes, preserved stack sentinel and termination without loading a game. Astro first-frame verification is separate and remains pending.
