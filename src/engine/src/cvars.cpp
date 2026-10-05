#include "ff7vr/engine/cvars.h"

#include "engine_internal.h"

#include "ff7vr/core/log.h"

#include <windows.h>

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

void apply_pending() {
    std::vector<std::pair<std::wstring, std::wstring>> work;
    {
        std::lock_guard lock(g_mutex);
        if (g_pending.empty()) return;
        work.swap(g_pending);
    }
    for (auto& [n, v] : work) set_now(n, v);
}

}  // namespace ff7vr::engine::cvar
