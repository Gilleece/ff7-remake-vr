#pragma once
// Thread-safe line logger.
//
// Every line is written to the file with one WriteFile call as soon as it is
// logged (no user-space buffering), so it survives a crash or a kill of the
// process. Format:
//   2026-10-05 12:34:56.789 [tid 1234] INFO  message
//
// Usage:
//   ff7vr::log::init(L"C:\\...\\ff7vr.log");
//   FF7VR_LOG_INFO("module base {:#x} size {}", base, size);
//   ff7vr::log::info("plain {}", 42);

#include <format>
#include <string>
#include <string_view>

namespace ff7vr::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Fatal = 5 };

// Opens (truncates) the log file. Safe to call more than once; later calls are
// ignored. Lines logged before init are kept in memory and written on init.
bool init(const std::wstring& path, Level min_level = Level::Info);
void shutdown();

void set_level(Level level);
Level level();
bool enabled(Level level);
Level parse_level(std::string_view name, Level fallback);
const std::wstring& path();

// Writes one already formatted message. Thread-safe.
void write(Level level, std::string_view message);

// Crash path: writes without taking the logger lock if the lock cannot be
// acquired within timeout_ms (the crashing thread may hold it).
void write_crash(std::string_view message, unsigned timeout_ms = 2000);

template <class... Args>
void logf(Level lvl, std::format_string<Args...> fmt, Args&&... args) {
    if (!enabled(lvl)) return;
    write(lvl, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args> void trace(std::format_string<Args...> f, Args&&... a) { logf(Level::Trace, f, std::forward<Args>(a)...); }
template <class... Args> void debug(std::format_string<Args...> f, Args&&... a) { logf(Level::Debug, f, std::forward<Args>(a)...); }
template <class... Args> void info(std::format_string<Args...> f, Args&&... a) { logf(Level::Info, f, std::forward<Args>(a)...); }
template <class... Args> void warn(std::format_string<Args...> f, Args&&... a) { logf(Level::Warn, f, std::forward<Args>(a)...); }
template <class... Args> void error(std::format_string<Args...> f, Args&&... a) { logf(Level::Error, f, std::forward<Args>(a)...); }

// UTF-16 to UTF-8 helper, handy for logging paths.
std::string narrow(std::wstring_view w);
std::wstring widen(std::string_view s);

}  // namespace ff7vr::log

#define FF7VR_LOG_TRACE(...) ::ff7vr::log::trace(__VA_ARGS__)
#define FF7VR_LOG_DEBUG(...) ::ff7vr::log::debug(__VA_ARGS__)
#define FF7VR_LOG_INFO(...)  ::ff7vr::log::info(__VA_ARGS__)
#define FF7VR_LOG_WARN(...)  ::ff7vr::log::warn(__VA_ARGS__)
#define FF7VR_LOG_ERROR(...) ::ff7vr::log::error(__VA_ARGS__)
