// psemu tarafindan uretildi (Kyty'nin CMake configure_file ciktisinin karsiligi).
// config.h bunu, KYTY_*_* enum makrolari tanimlandiktan SONRA include eder.
// Kyty yalnizca GCC/CLANG taniyor; MSVC yok. clang-cl ~ CLANG oldugu icin
// simdilik CLANG isaretliyoruz (MSVC ile derleme denemesinde uyumsuzluklari
// olcecegiz; gerekirse tum build clang-cl'e gecer).
#ifndef KYTY_CMAKE_CONFIG_H_
#define KYTY_CMAKE_CONFIG_H_

#define KYTY_VERSION  "0.1.0-psemu"
#define KYTY_ENDIAN   KYTY_ENDIAN_LITTLE
#define KYTY_PLATFORM KYTY_PLATFORM_WINDOWS
#define KYTY_COMPILER KYTY_COMPILER_CLANG
#define KYTY_LINKER   KYTY_LINKER_LLD_LINK
#define KYTY_BUILD    KYTY_BUILD_RELEASE

#endif /* KYTY_CMAKE_CONFIG_H_ */
