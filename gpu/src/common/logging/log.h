#ifndef KYTY_COMMON_LOGGING_LOG_H_
#define KYTY_COMMON_LOGGING_LOG_H_

#include "common/common.h"
#include "common/subsystems.h"

#include <fmt/color.h>
#include <fmt/printf.h>
#include <string_view>

namespace Log {

KYTY_SUBSYSTEM_DEFINE(Log);

enum class Direction { Silent, Console, File };

Direction GetDirection();

// PERFORMANS ANAHTARI.
// LOGF her cagrisinda once fmt::sprintf ile bir std::string kurar (heap
// tahsisi + bicimlendirme), sonra kilit alip yazar. Bazi LOGF'ler cok sicak:
// "RenderColorTarget" HER CIZIM icin (kare basina ~20 satir), "Equeue wait
// received" her beklemede, swapchain uyarilari her karede. Bayragi makro
// SEVIYESINDE kontrol ediyoruz ki bicimlendirme maliyeti de olusmasin.
// Varsayilan: KAPALI. Acmak icin: PSEMU_GPU_LOG=1
extern bool g_log_enabled;
inline bool Enabled() {
	return g_log_enabled;
}
void      Write(std::string_view text);
void      Write(fmt::text_style style, std::string_view text);
void      WriteFatal(std::string_view text);
void      WriteFatal(fmt::text_style style, std::string_view text);
void      Flush();

namespace Color {

inline constexpr auto Default       = fmt::text_style {};
inline constexpr auto Red           = fmt::fg(fmt::terminal_color::red);
inline constexpr auto Green         = fmt::fg(fmt::terminal_color::green);
inline constexpr auto Yellow        = fmt::fg(fmt::terminal_color::yellow);
inline constexpr auto Magenta       = fmt::fg(fmt::terminal_color::magenta);
inline constexpr auto Cyan          = fmt::fg(fmt::terminal_color::cyan);
inline constexpr auto White         = fmt::fg(fmt::terminal_color::white);
inline constexpr auto BrightRed     = fmt::fg(fmt::terminal_color::bright_red);
inline constexpr auto BrightGreen   = fmt::fg(fmt::terminal_color::bright_green);
inline constexpr auto BrightYellow  = fmt::fg(fmt::terminal_color::bright_yellow);
inline constexpr auto BrightMagenta = fmt::fg(fmt::terminal_color::bright_magenta);
inline constexpr auto BrightWhite   = fmt::fg(fmt::terminal_color::bright_white);

} // namespace Color

} // namespace Log

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF(...)                                                                                  \
	do {                                                                                           \
		if (::Log::Enabled()) ::Log::Write(::fmt::sprintf(__VA_ARGS__));                            \
	} while (0)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		if (::Log::Enabled()) ::Log::Write((style), ::fmt::sprintf(__VA_ARGS__));                   \
	} while (0)

#endif /* KYTY_COMMON_LOGGING_LOG_H_ */
