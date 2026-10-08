#include "ff7vr/engine/cvars.h"

#include "engine_internal.h"

#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

namespace ff7vr::engine::cvar {
namespace {

constexpr std::size_t kFindConsoleVariable = 18;  // IConsoleManager
constexpr std::size_t kGetFlags = 3;              // IConsoleObject
constexpr std::size_t kSet = 12;                  // IConsoleVariable
constexpr std::size_t kGetInt = 13;
constexpr std::size_t kGetFloat = 14;
constexpr unsigned kSetByConsole = 0x09000000;

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name);
using GetFlagsFn = unsigned(__fastcall*)(void* self);
using SetFn = void(__fastcall*)(void* self, const wchar_t* value, unsigned set_by);
using GetIntFn = int(__fastcall*)(void* self);
using GetFloatFn = float(__fastcall*)(void* self);

std::mutex g_mutex;
std::vector<std::pair<std::wstring, std::wstring>> g_pending;

void* manager() {
    const Addresses& a = addresses();
    if (!a.ConsoleManager || !a.ConsoleManagerVtable) return nullptr;
    void* m = *a.ConsoleManager;
    if (!m || *static_cast<std::uintptr_t*>(m) != a.ConsoleManagerVtable) return nullptr;
    return m;
}

template <class Fn>
Fn vfn(void* obj, std::size_t slot) {
    return reinterpret_cast<Fn>((*static_cast<void***>(obj))[slot]);
}

// SEH-guarded calls (no C++ objects with destructors in these frames).
void* find_guarded(void* mgr, const wchar_t* name) {
    __try {
        return vfn<FindFn>(mgr, kFindConsoleVariable)(mgr, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool read_guarded(void* var, int* i, float* f, unsigned* flags) {
    __try {
        *i = vfn<GetIntFn>(var, kGetInt)(var);
        *f = vfn<GetFloatFn>(var, kGetFloat)(var);
        *flags = vfn<GetFlagsFn>(var, kGetFlags)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_guarded(void* var, const wchar_t* value) {
    __try {
        vfn<SetFn>(var, kSet)(var, value, kSetByConsole);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* find(std::wstring_view name) {
    void* m = manager();
    if (!m) return nullptr;
    const std::wstring n(name);
    return find_guarded(m, n.c_str());
}

bool set_now(std::wstring_view name, std::wstring_view value) {
    void* var = find(name);
    if (!var) {
        log::warn("cvar: {} not found", log::narrow(name));
        return false;
    }
    const std::wstring v(value);
    if (!set_guarded(var, v.c_str())) {
        log::warn("cvar: setting {} failed", log::narrow(name));
        return false;
    }
    int i = 0;
    float f = 0;
    unsigned flags = 0;
    read_guarded(var, &i, &f, &flags);
    log::info("cvar: {} = {} (now {} / {}, flags {:#x})", log::narrow(name), log::narrow(value), i, f, flags);
    return true;
}

}  // namespace

bool available() { return manager() != nullptr; }

std::optional<Value> get(std::wstring_view name) {
    void* var = find(name);
    if (!var) return std::nullopt;
    Value v;
    if (!read_guarded(var, &v.i, &v.f, &v.flags)) return std::nullopt;
    return v;
}

bool set(std::wstring_view name, std::wstring_view value) {
    if (on_game_thread()) return set_now(name, value);
    if (!find(name)) return false;
    std::lock_guard lock(g_mutex);
    g_pending.emplace_back(std::wstring(name), std::wstring(value));
    return true;
}

namespace {
std::vector<std::pair<std::wstring, std::wstring>> g_pending_holds;  // g_mutex; an empty value releases
void apply_holds(std::vector<std::pair<std::wstring, std::wstring>>& holds);
}  // namespace

void apply_pending() {
    std::vector<std::pair<std::wstring, std::wstring>> work, holds;
    {
        std::lock_guard lock(g_mutex);
        if (g_pending.empty() && g_pending_holds.empty()) return;
        work.swap(g_pending);
        holds.swap(g_pending_holds);
    }
    for (auto& [n, v] : work) set_now(n, v);
    if (!holds.empty()) apply_holds(holds);
}

namespace {
std::vector<std::pair<std::wstring, std::wstring>> g_stereo_overrides;  // set before the engine starts
std::vector<std::pair<std::wstring, std::wstring>> g_saved;             // game thread
bool g_overrides_on = false;
}  // namespace

void set_stereo_overrides(std::vector<std::pair<std::wstring, std::wstring>> overrides) {
    g_stereo_overrides = std::move(overrides);
}

void stereo_overrides(bool on) {
    if (on == g_overrides_on) return;
    g_overrides_on = on;
    if (on) {
        g_saved.clear();
        for (const auto& [name, value] : g_stereo_overrides) {
            const auto cur = get(name);
            if (!cur) {
                log::warn("cvar: {} not found ([stereo_cvars])", log::narrow(name));
                continue;
            }
            // Integer variables accept the float text ("2" for 2.0).
            g_saved.emplace_back(name, log::widen(std::format("{}", cur->f)));
            set_now(name, value);
        }
    } else {
        for (const auto& [name, value] : g_saved) set_now(name, value);
        g_saved.clear();
    }
}

void hold_in_stereo(std::wstring_view name, std::wstring_view value) {
    if (value.empty()) return;
    std::lock_guard lock(g_mutex);
    g_pending_holds.emplace_back(std::wstring(name), std::wstring(value));
}

void release_in_stereo(std::wstring_view name) {
    std::lock_guard lock(g_mutex);
    g_pending_holds.emplace_back(std::wstring(name), std::wstring());
}

namespace {
bool same_name(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// Game thread.
void apply_holds(std::vector<std::pair<std::wstring, std::wstring>>& holds) {
    for (auto& [name, value] : holds) {
        std::erase_if(g_stereo_overrides, [&](const auto& kv) { return same_name(kv.first, name); });
        if (value.empty()) {
            const auto it = std::find_if(g_saved.begin(), g_saved.end(), [&](const auto& kv) { return same_name(kv.first, name); });
            if (it != g_saved.end()) {
                if (g_overrides_on) set_now(it->first, it->second);
                g_saved.erase(it);
            }
            continue;
        }
        g_stereo_overrides.emplace_back(name, value);
        if (!g_overrides_on) continue;
        const bool saved = std::any_of(g_saved.begin(), g_saved.end(), [&](const auto& kv) { return same_name(kv.first, name); });
        if (!saved) {
            const auto cur = get(name);
            if (!cur) {
                log::warn("cvar: {} not found (graphics profile)", log::narrow(name));
                continue;
            }
            g_saved.emplace_back(name, log::widen(std::format("{}", cur->f)));
        }
        set_now(name, value);
    }
}
}  // namespace

}  // namespace ff7vr::engine::cvar
