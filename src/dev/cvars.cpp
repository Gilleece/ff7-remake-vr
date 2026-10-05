// Read-only access to console variables and GSystemResolution, so a
// benchmark run can record the render settings it actually ran with.
//
// Uses the engine's own IConsoleManager::FindConsoleVariable (vtable slot 18)
// and IConsoleVariable::GetFloat (slot 14). Both addresses are found by
// signature; the console manager's vtable is checked against the signature
// result before anything is called, so a different game build fails cleanly.
// See docs/re/engine.md, section "Console variables".

#include "cvars.h"

#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/core/pattern.h"

#include <windows.h>

#include <format>
#include <mutex>

namespace ff7vr::dev::cvars {
namespace {

std::once_flag g_once;
void** g_singleton = nullptr;   // IConsoleManager**
void* g_vftable = nullptr;      // FConsoleManager vtable
int* g_system_resolution = nullptr;
std::string g_error;

std::uintptr_t find(std::uintptr_t base, const char* what, const char* sig, std::size_t disp, std::size_t len) {
    const auto r = pattern::scan_module(base, sig);
    if (!r.unique()) {
        g_error += std::format("{} signature: {} matches; ", what, r.matches.size());
        return 0;
    }
    return pattern::rip(r.first(), disp, len);
}

void resolve() {
    const auto base = module::main_module().base;
    g_singleton = reinterpret_cast<void**>(find(base, "IConsoleManager::Singleton",
        "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 4C 8D 0D ?? ?? ?? ?? 0F 57 D2 89 7C 24 20",
        3, 7));
    g_vftable = reinterpret_cast<void*>(find(base, "FConsoleManager::vftable", "48 8D 05 ?? ?? ?? ?? 48 89 5C 24 40 48 89 07", 3, 7));
    g_system_resolution = reinterpret_cast<int*>(find(base, "GSystemResolution", "81 3D ?? ?? ?? ?? 80 07 00 00", 2, 10));
    log::info("cvars: console manager singleton {} vtable {} GSystemResolution {}{}", static_cast<void*>(g_singleton),
              g_vftable, static_cast<void*>(g_system_resolution), g_error.empty() ? "" : " (" + g_error + ")");
}

// Calls a virtual function through a vtable slot, inside SEH so a bad pointer
// returns false instead of taking the game down.
bool find_and_read(void* mgr, const wchar_t* name, float& out, bool& exists) {
    __try {
        using FindFn = void*(__fastcall*)(void*, const wchar_t*);
        auto find_fn = reinterpret_cast<FindFn>((*reinterpret_cast<void***>(mgr))[18]);
        void* var = find_fn(mgr, name);
        exists = var != nullptr;
        if (!var) return true;
        using GetFloatFn = float(__fastcall*)(void*);
        auto get_float = reinterpret_cast<GetFloatFn>((*reinterpret_cast<void***>(var))[14]);
        out = get_float(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// IConsoleVariable::Set(const TCHAR* value, EConsoleVariableFlags set_by) is slot 12,
// GetFlags slot 3. Returns false if anything faulted.
bool find_and_set(void* mgr, const wchar_t* name, const wchar_t* value, unsigned set_by, float& now, unsigned& flags,
                  bool& exists) {
    __try {
        using FindFn = void*(__fastcall*)(void*, const wchar_t*);
        auto find_fn = reinterpret_cast<FindFn>((*reinterpret_cast<void***>(mgr))[18]);
        void* var = find_fn(mgr, name);
        exists = var != nullptr;
        if (!var) return true;
        void** vt = *reinterpret_cast<void***>(var);
        using SetFn = void(__fastcall*)(void*, const wchar_t*, unsigned);
        reinterpret_cast<SetFn>(vt[12])(var, value, set_by);
        using GetFlagsFn = unsigned(__fastcall*)(void*);
        flags = reinterpret_cast<GetFlagsFn>(vt[3])(var);
        using GetFloatFn = float(__fastcall*)(void*);
        now = reinterpret_cast<GetFloatFn>(vt[14])(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* manager() {
    std::call_once(g_once, resolve);
    void* mgr = g_singleton ? *g_singleton : nullptr;
    if (!mgr || !g_vftable || *reinterpret_cast<void**>(mgr) != g_vftable) return nullptr;
    return mgr;
}

}  // namespace

std::string set(const std::string& name, const std::string& value) {
    void* mgr = manager();
    if (!mgr) return "err console manager not available";
    // ECVF_SetByConsole (0x09000000): the highest priority in 4.18, so the
    // game's own settings code (lower priority) cannot change it back.
    constexpr unsigned kSetByConsole = 0x09000000u;
    float now = 0;
    unsigned flags = 0;
    bool exists = false;
    if (!find_and_set(mgr, log::widen(name).c_str(), log::widen(value).c_str(), kSetByConsole, now, flags, exists))
        return "err fault while setting " + name;
    if (!exists) return "err no console variable " + name;
    log::info("cvars: set {} = {} (now {:g}, set-by 0x{:08x})", name, value, now, flags & 0xFF000000u);
    return std::format("ok {}={:g} setby=0x{:08x}", name, now, flags & 0xFF000000u);
}

std::string read(const std::vector<std::string>& names) {
    std::call_once(g_once, resolve);
    std::string out = "ok";
    if (g_system_resolution) {
        out += std::format(" GSystemResolution={}x{}", g_system_resolution[0], g_system_resolution[1]);
    }
    void* mgr = g_singleton ? *g_singleton : nullptr;
    if (!mgr || !g_vftable || *reinterpret_cast<void**>(mgr) != g_vftable) {
        if (names.empty()) return out;
        return "err console manager not available (" + (g_error.empty() ? std::string("not created yet or vtable mismatch") : g_error) + ")";
    }
    for (const auto& n : names) {
        float v = 0;
        bool exists = false;
        if (!find_and_read(mgr, log::widen(n).c_str(), v, exists)) {
            out += " " + n + "=error";
        } else if (!exists) {
            out += " " + n + "=missing";
        } else {
            out += std::format(" {}={:g}", n, v);
        }
    }
    return out;
}

}  // namespace ff7vr::dev::cvars
