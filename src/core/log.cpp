#include "ff7vr/core/log.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace ff7vr::log {
namespace {

// A CRITICAL_SECTION rather than std::mutex so the crash path can use
// TryEnterCriticalSection with a timeout loop.
struct State {
    CRITICAL_SECTION cs;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::vector<std::string> pending;  // lines logged before init
    State() { InitializeCriticalSectionAndSpinCount(&cs, 1000); }
};

State& state() {
    static State s;  // constructed on first use; never destroyed explicitly
    return s;
}

std::atomic<int> g_level{static_cast<int>(Level::Info)};

const char* level_name(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
    }
    return "?????";
}

std::string format_line(const char* lvl, std::string_view message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string line = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [tid {}] {} ",
                                   t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                                   t.wMilliseconds, GetCurrentThreadId(), lvl);
    line.append(message);
    // Strip trailing newlines from the message, then add exactly one CRLF.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    line.append("\r\n");
    return line;
}

void raw_write(HANDLE f, const std::string& line) {
    DWORD written = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void debugger_echo(const std::string& line) {
    if (IsDebuggerPresent()) OutputDebugStringA(line.c_str());
}

}  // namespace

bool init(const std::wstring& path, Level min_level) {
    State& s = state();
    EnterCriticalSection(&s.cs);
    if (s.file != INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&s.cs);
        return true;
    }
    g_level = static_cast<int>(min_level);
    // FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE so harness scripts can
    // tail, copy or even delete the file while the game runs.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&s.cs);
        return false;
    }
    s.file = f;
    s.path = path;
    for (const auto& line : s.pending) raw_write(f, line);
    s.pending.clear();
    s.pending.shrink_to_fit();
    LeaveCriticalSection(&s.cs);
    return true;
}

void shutdown() {
    State& s = state();
    EnterCriticalSection(&s.cs);
    if (s.file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(s.file);
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&s.cs);
}

void set_level(Level l) { g_level = static_cast<int>(l); }
Level level() { return static_cast<Level>(g_level.load()); }
bool enabled(Level l) { return static_cast<int>(l) >= g_level.load(std::memory_order_relaxed); }
const std::wstring& path() { return state().path; }

Level parse_level(std::string_view n, Level fallback) {
    auto eq = [&](std::string_view b) {
        if (n.size() != b.size()) return false;
        for (size_t i = 0; i < n.size(); ++i)
            if ((n[i] | 0x20) != (b[i] | 0x20)) return false;
        return true;
    };
    if (eq("trace")) return Level::Trace;
    if (eq("debug")) return Level::Debug;
    if (eq("info")) return Level::Info;
    if (eq("warn") || eq("warning")) return Level::Warn;
    if (eq("error")) return Level::Error;
    return fallback;
}

void write(Level lvl, std::string_view message) {
    if (!enabled(lvl)) return;
    std::string line = format_line(level_name(lvl), message);
    State& s = state();
    EnterCriticalSection(&s.cs);
    if (s.file != INVALID_HANDLE_VALUE)
        raw_write(s.file, line);
    else if (s.pending.size() < 4096)
        s.pending.push_back(line);
    LeaveCriticalSection(&s.cs);
    debugger_echo(line);
}

void write_crash(std::string_view message, unsigned timeout_ms) {
    std::string line = format_line(level_name(Level::Fatal), message);
    State& s = state();
    bool locked = false;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!(locked = TryEnterCriticalSection(&s.cs) != FALSE) && GetTickCount64() < deadline) Sleep(1);
    // Without the lock we still write: a garbled line beats a missing one.
    if (s.file != INVALID_HANDLE_VALUE) {
        raw_write(s.file, line);
        FlushFileBuffers(s.file);
    }
    if (locked) LeaveCriticalSection(&s.cs);
    debugger_echo(line);
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace ff7vr::log
