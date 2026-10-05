// ff7vr loader: the xinput1_3.dll proxy that ff7remake_.exe loads from its own
// directory (End\Binaries\Win64). DllMain does nothing but start one thread;
// all work happens on that thread, after the loader lock is released.

#include "dev_input.h"
#include "startup.h"
#include "xinput_proxy.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/crash.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"

#include "ff7vr_buildinfo.h"

#include <windows.h>

#include <filesystem>
#include <system_error>

namespace {

using namespace ff7vr;

constexpr char kVersion[] = "0.1.0";

Config g_config;
HANDLE g_bootstrap = nullptr;

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
    log::info("config: {} ({})", log::narrow((dll_dir / L"ff7vr.ini").wstring()),
              g_config.loaded() ? "loaded" : "not found, using defaults");
    for (const auto& line : g_config.dump()) log::info("config:   {}", line);
}

DWORD WINAPI bootstrap(void*) {
    auto self = module::self();
    std::filesystem::path dll_dir = self.path.parent_path();

    g_config.load(dll_dir / L"ff7vr.ini");
    auto level = log::parse_level(g_config.get_string("log", "level", "info"), log::Level::Info);
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

    if (loader::xinput::load_real())
        log::info("xinput: forwarding to {}", log::narrow(loader::xinput::real_path()));
    else
        log::warn("xinput: real xinput1_3.dll not found; gamepad calls report 'not connected'");
    loader::xinput::set_virtual_pad_enabled(g_config.get_bool("dev", "virtual_pad", false));
    if (loader::xinput::virtual_pad_enabled()) log::info("xinput: virtual pad enabled on user index 0");

    hook::init();

    auto game = module::main_module();
    StartupContext ctx;
    ctx.config = &g_config;
    ctx.dll_dir = dll_dir;
    ctx.game_base = game.base;
    ctx.game_size = game.size;
    ctx.is_game = _wcsicmp(game.path.filename().c_str(), L"ff7remake_.exe") == 0;
    loader::start_modules(ctx);

    // Debug aid for testing the crash path in the real game: [debug] crash_after_seconds=N
    if (auto secs = g_config.get_int("debug", "crash_after_seconds", 0); secs > 0) {
        log::warn("debug: deliberate crash in {} s", secs);
        Sleep(static_cast<DWORD>(secs * 1000));
        log::warn("debug: crashing now");
        volatile int* p = nullptr;
        *p = 1;
    }

    log::info("ff7vr: initialised, idle");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
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
