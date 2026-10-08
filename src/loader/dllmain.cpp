// ff7vr loader: the xinput1_3.dll proxy that ff7remake_.exe loads from its own
// directory (End\Binaries\Win64). DllMain reads ff7vr.ini, adds -d3d11 to the
// command line if needed (the engine has not read it yet) and starts one thread;
// all other work happens on that thread, after the loader lock is released.

#include "cmdline.h"
#include "dev_input.h"
#include "startup.h"
#include "xinput_proxy.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/graphics_profile.h"
#include "ff7vr/core/crash.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"

#include "ff7vr_buildinfo.h"

#include <windows.h>

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace {

using namespace ff7vr;

constexpr char kVersion[] = "0.1.0";

Config g_config;
std::vector<std::string> g_profile_log;  // [graphics] profile, logged once the log is open
HANDLE g_bootstrap = nullptr;

// Result of the Direct3D 11 check made in DllMain, logged once the log is open.
struct D3D11Check {
    log::Level level = log::Level::Info;
    std::string message;
    std::wstring original;  // command line before the change, empty when unchanged
} g_d3d11;

std::filesystem::path dll_directory(HMODULE module) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(module, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::filesystem::path(buf).parent_path();
}

// Reads ff7vr.ini with plain Win32 file calls (this runs under the loader lock).
void load_config_early(const std::filesystem::path& ini) {
    HANDLE f = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(f);
    g_config.load_from_string(text);
    g_profile_log = graphics_profile::apply(g_config);
}

// The mod's D3D11 hooks need the engine to choose Direct3D 11; without a
// graphics option on its command line it chooses Direct3D 12. The engine reads
// the command line only after every DLL the exe imports is initialised, so the
// option can still be added here. Runs in DllMain, before the engine starts.
void ensure_d3d11() {
    auto exe = module::main_module();
    if (_wcsicmp(exe.path.filename().c_str(), L"ff7remake_.exe") != 0) return;
    if (!g_config.get_bool("loader", "force_d3d11", true)) {
        g_d3d11.message = "d3d11: [loader] force_d3d11 = 0, command line left as it is";
        return;
    }
    if (!g_config.get_bool("render", "enabled", true) && !g_config.get_bool("stereo", "enabled", true)) {
        g_d3d11.message = "d3d11: [render] and [stereo] are off, command line left as it is";
        return;
    }
    std::wstring option;
    switch (loader::cmdline::find_d3d_choice(GetCommandLineW(), &option)) {
        case loader::cmdline::D3DChoice::D3D11:
            g_d3d11.message = std::format("d3d11: the command line already has {}", log::narrow(option));
            return;
        case loader::cmdline::D3DChoice::Other:
            g_d3d11.level = log::Level::Warn;
            g_d3d11.message = std::format(
                "d3d11: the command line asks for {}; left as it is. The mod needs Direct3D 11: remove {} from the "
                "game's launch options",
                log::narrow(option), log::narrow(option));
            return;
        case loader::cmdline::D3DChoice::None:
            break;
    }
    std::wstring before = GetCommandLineW();
    std::string detail;
    if (loader::cmdline::append_argument(L"-d3d11", &detail)) {
        g_d3d11.original = std::move(before);
        g_d3d11.message = std::format("d3d11: added -d3d11 to the command line so the game uses Direct3D 11 ({})", detail);
    } else {
        g_d3d11.level = log::Level::Warn;
        g_d3d11.message = std::format(
            "d3d11: could not add -d3d11 to the command line ({}). The game will use Direct3D 12 and the mod stays "
            "inactive: add -d3d11 to the game's launch options in Steam",
            detail);
    }
}

void log_identity(const std::filesystem::path& dll_dir) {
    auto self = module::self();
    auto game = module::main_module();
    std::error_code ec;
    auto exe_size = std::filesystem::file_size(game.path, ec);

    log::info("ff7vr {} loaded (commit {}, built {} {})", kVersion, FF7VR_GIT_COMMIT, FF7VR_BUILD_TIME_UTC,
              FF7VR_BUILD_TYPE);
    log::info("dll: {} base 0x{:x} size 0x{:x}", log::narrow(self.path.wstring()), self.base, self.size);
    log::info("exe: {}", log::narrow(game.path.wstring()));
    log::info("exe: file version {}, file size {} bytes, PE timestamp 0x{:08x}",
              module::file_version(game.path), ec ? 0ull : static_cast<unsigned long long>(exe_size),
              module::timestamp(game.base));
    log::info("exe: module base 0x{:x} size 0x{:x} ({} MB)", game.base, game.size, game.size >> 20);
    log::info("process: pid {}, command line: {}", GetCurrentProcessId(), log::narrow(GetCommandLineW()));
    // Variables that tell how the game was started and which XR runtime it will use.
    for (const wchar_t* name : {L"XR_RUNTIME_JSON", L"SteamAppId", L"SteamGameId", L"SteamClientLaunch"}) {
        wchar_t value[1024] = {};
        DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
        log::info("env: {}={}", log::narrow(name), n > 0 && n < std::size(value) ? log::narrow(value) : "(unset)");
    }
    log::info("config: {} ({})", log::narrow((dll_dir / L"ff7vr.ini").wstring()),
              g_config.loaded() ? "loaded" : "not found, using defaults");
    for (const auto& line : g_profile_log) log::info("{}", line);
    for (const auto& line : g_config.dump()) log::info("config:   {}", line);
}

DWORD WINAPI bootstrap(void*) {
    auto self = module::self();
    std::filesystem::path dll_dir = self.path.parent_path();

    auto level = log::parse_level(g_config.get_string("log", "level", "info"), log::Level::Info);
    // Earlier sessions' logs and crash dumps go to ff7vr-logs\ (only when the mod is installed
    // by hand; the launcher and the dev tools move them away after every session).
    auto kept = log::archive_previous(dll_dir.wstring(), static_cast<int>(g_config.get_int("log", "keep_sessions", 5)));
    log::init((dll_dir / L"ff7vr.log").wstring(), level);

    if (g_config.get_bool("crash", "enabled", true)) {
        crash::Options co;
        co.dump_dir = dll_dir;
        co.first_chance = g_config.get_bool("crash", "first_chance", true);
        co.max_reports = static_cast<int>(g_config.get_int("crash", "max_reports", 8));
        co.max_dumps = static_cast<int>(g_config.get_int("crash", "max_dumps", 2));
        co.full_memory_dump = g_config.get_bool("crash", "full_memory", false);
        crash::install(co);
    }

    log_identity(dll_dir);
    for (const auto& line : kept) log::info("{}", line);
    if (!g_d3d11.original.empty()) log::info("process: command line at start: {}", log::narrow(g_d3d11.original));
    if (!g_d3d11.message.empty()) log::write(g_d3d11.level, g_d3d11.message);

    if (loader::xinput::load_real())
        log::info("xinput: forwarding to {}", log::narrow(loader::xinput::real_path()));
    else
        log::warn("xinput: real xinput1_3.dll not found; gamepad calls report 'not connected'");
    loader::xinput::set_virtual_pad_enabled(g_config.get_bool("dev", "virtual_pad", false));
    if (loader::xinput::virtual_pad_enabled()) log::info("xinput: virtual pad enabled on user index 0");
    loader::xinput::register_diagnostics(g_config.get_bool("controls", "pad_after_hooks", true));

    hook::init();

    auto game = module::main_module();
    StartupContext ctx;
    ctx.config = &g_config;
    ctx.dll_dir = dll_dir;
    ctx.game_base = game.base;
    ctx.game_size = game.size;
    ctx.is_game = _wcsicmp(game.path.filename().c_str(), L"ff7remake_.exe") == 0;
    loader::start_modules(ctx);
    log::info("ff7vr: initialised, idle");

    // Debug aid for testing the crash path in the real game: [debug] crash_after_seconds=N
    if (auto secs = g_config.get_int("debug", "crash_after_seconds", 0); secs > 0) {
        log::warn("debug: deliberate crash in {} s", secs);
        Sleep(static_cast<DWORD>(secs * 1000));
        log::warn("debug: crashing now");
        volatile int* p = nullptr;
        *p = 1;
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            // Before the engine starts: the settings, then the graphics API option.
            if (auto dir = dll_directory(module); !dir.empty()) load_config_early(dir / L"ff7vr.ini");
            ensure_d3d11();
            // The thread starts running only after the loader lock is released.
            g_bootstrap = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
            if (g_bootstrap) {
                SetThreadDescription(g_bootstrap, L"ff7vr bootstrap");
                CloseHandle(g_bootstrap);
            }
            break;
        case DLL_PROCESS_DETACH:
            // reserved != nullptr: process is exiting; other threads are gone.
            if (reserved != nullptr) {
                ff7vr::loader::stop_modules();
                ff7vr::log::write(ff7vr::log::Level::Info, "ff7vr: process exiting");
            }
            break;
        default:
            break;
    }
    return TRUE;
}
