#include "xinput_proxy.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/log.h"

#include <intrin.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

extern "C" DWORD WINAPI proxy_XInputGetState(DWORD user, XINPUT_STATE* state);

namespace ff7vr::loader::xinput {
namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using EnableFn = void(WINAPI*)(BOOL);
using GetDSoundFn = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
using GetBatteryFn = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
using GetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
using GetStateExFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using WaitGuideFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
using CancelGuideFn = DWORD(WINAPI*)(DWORD);
using PowerOffFn = DWORD(WINAPI*)(DWORD);

struct Real {
    HMODULE module = nullptr;
    std::wstring path;
    GetStateFn get_state = nullptr;
    SetStateFn set_state = nullptr;
    GetCapsFn get_caps = nullptr;
    EnableFn enable = nullptr;
    GetDSoundFn get_dsound = nullptr;
    GetBatteryFn get_battery = nullptr;
    GetKeystrokeFn get_keystroke = nullptr;
    GetStateExFn get_state_ex = nullptr;
    WaitGuideFn wait_guide = nullptr;
    CancelGuideFn cancel_guide = nullptr;
    PowerOffFn power_off = nullptr;
};

Real g_real;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK do_load(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sys[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    std::wstring path = std::wstring(sys) + L"\\xinput1_3.dll";
    // Absolute path: never resolves back to ourselves in the game directory.
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!m) m = LoadLibraryW(path.c_str());
    if (!m) {
        // Fall back to the always-present Windows 8+ XInput (same ABI, by name).
        path = std::wstring(sys) + L"\\xinput1_4.dll";
        m = LoadLibraryW(path.c_str());
    }
    if (!m) return TRUE;
    g_real.module = m;
    g_real.path = path;
    auto by_name = [m](const char* name) { return GetProcAddress(m, name); };
    auto by_ord = [m](WORD ord) { return GetProcAddress(m, MAKEINTRESOURCEA(ord)); };
    g_real.get_state = reinterpret_cast<GetStateFn>(by_name("XInputGetState"));
    g_real.set_state = reinterpret_cast<SetStateFn>(by_name("XInputSetState"));
    g_real.get_caps = reinterpret_cast<GetCapsFn>(by_name("XInputGetCapabilities"));
    g_real.enable = reinterpret_cast<EnableFn>(by_name("XInputEnable"));
    g_real.get_dsound = reinterpret_cast<GetDSoundFn>(by_name("XInputGetDSoundAudioDeviceGuids"));
    g_real.get_battery = reinterpret_cast<GetBatteryFn>(by_name("XInputGetBatteryInformation"));
    g_real.get_keystroke = reinterpret_cast<GetKeystrokeFn>(by_name("XInputGetKeystroke"));
    g_real.get_state_ex = reinterpret_cast<GetStateExFn>(by_ord(100));
    g_real.wait_guide = reinterpret_cast<WaitGuideFn>(by_ord(101));
    g_real.cancel_guide = reinterpret_cast<CancelGuideFn>(by_ord(102));
    g_real.power_off = reinterpret_cast<PowerOffFn>(by_ord(103));
    return TRUE;
}

const Real& real() {
    InitOnceExecuteOnce(&g_once, do_load, nullptr, nullptr);
    return g_real;
}

// Virtual pad (dev input). Guarded by a small lock; reads are rare (per frame).
std::atomic<bool> g_vpad_enabled{false};
SRWLOCK g_vpad_lock = SRWLOCK_INIT;
VirtualPad g_vpad;
std::atomic<DWORD> g_vpad_packet{0};
std::atomic<std::uint64_t> g_get_state_calls{0};

SHORT add_axis(SHORT a, SHORT b) {
    int v = static_cast<int>(a) + static_cast<int>(b);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return static_cast<SHORT>(v);
}

std::atomic<PadFilter> g_pad_filter{nullptr};
std::atomic<StickFilter> g_stick_filter{nullptr};
std::atomic<PadOverride> g_pad_override{nullptr};
std::atomic<std::uint64_t> g_overridden{0};
// What the game received in the last successful poll of any user index (after every filter).
std::atomic<std::uint64_t> g_last_given{0};  // buttons | lt << 16 | rt << 24 | (lx, ly hi bytes) << 32

// Stick deflection of the last successful poll (0..1 each), for the comfort vignette.
std::atomic<float> g_stick_left{0.0f}, g_stick_right{0.0f};

float stick_magnitude(SHORT x, SHORT y) {
    const float m = std::sqrt(float(x) * float(x) + float(y) * float(y)) / 32767.0f;
    return m > 1.0f ? 1.0f : m;
}

DWORD filtered(DWORD user, XINPUT_STATE* state, DWORD rc) {
    const PadOverride o = g_pad_override.load(std::memory_order_relaxed);
    if (o && rc == ERROR_SUCCESS && state) {
        XINPUT_GAMEPAD& g = state->Gamepad;
        if (o(user, &g.wButtons, &g.bLeftTrigger, &g.bRightTrigger, &g.sThumbLX, &g.sThumbLY, &g.sThumbRX, &g.sThumbRY)) {
            g_overridden.fetch_add(1, std::memory_order_relaxed);
            ZeroMemory(&g, sizeof(g));
        }
    }
    if (rc == ERROR_SUCCESS && state && user == 0) {
        g_stick_left.store(stick_magnitude(state->Gamepad.sThumbLX, state->Gamepad.sThumbLY), std::memory_order_relaxed);
        g_stick_right.store(stick_magnitude(state->Gamepad.sThumbRX, state->Gamepad.sThumbRY), std::memory_order_relaxed);
    }
    const PadFilter f = g_pad_filter.load(std::memory_order_relaxed);
    if (f && rc == ERROR_SUCCESS && state) f(user, &state->Gamepad.wButtons);
    const StickFilter sf = g_stick_filter.load(std::memory_order_relaxed);
    if (sf && rc == ERROR_SUCCESS && state)
        sf(user, &state->Gamepad.sThumbLX, &state->Gamepad.sThumbLY, &state->Gamepad.sThumbRX, &state->Gamepad.sThumbRY);
    if (rc == ERROR_SUCCESS && state) {
        const XINPUT_GAMEPAD& g = state->Gamepad;
        g_last_given.store(std::uint64_t(g.wButtons) | std::uint64_t(g.bLeftTrigger) << 16 | std::uint64_t(g.bRightTrigger) << 24 |
                               std::uint64_t(std::uint16_t(g.sThumbLX)) << 32 | std::uint64_t(std::uint16_t(g.sThumbLY)) << 48,
                           std::memory_order_relaxed);
    }
    return rc;
}

// ------------------------------------------------------------------ diagnostics
// Who polls XInput, how often, and which user indexes report a pad. Counters on the call
// path; the log lines are written once (first call, rate after a few seconds) or on a
// connection change, and the dev command `xinput timing` reads the counters for the
// render module's 10-second timing block.
constexpr DWORD kUsers = 4;
struct UserDiag {
    std::atomic<std::uint64_t> calls{0}, ok{0};
    std::atomic<int> connected{-1};  // -1 not polled yet, 0 no pad, 1 pad
    std::atomic<std::int64_t> last_us{0};
    std::atomic<std::int64_t> interval_sum_us{0};
    std::atomic<std::uint64_t> intervals{0};
};
UserDiag g_users[kUsers];
std::atomic<bool> g_diag{false};
std::atomic<std::uint64_t> g_other_user_calls{0}, g_other_thread_calls{0}, g_caller_changes{0};
std::atomic<DWORD> g_poll_thread{0};
std::atomic<void*> g_caller{nullptr};
std::atomic<std::int64_t> g_first_us{0};
std::atomic<bool> g_rate_logged{false};
unsigned char g_entry_bytes[2][16];  // proxy_XInputGetState, the real XInputGetState (at start)
bool g_entry_saved[2] = {false, false};
// The import wrapper (outer_get_state): calls made through it, of them the ones that reached our
// export's original code (a hook on the export may answer by itself).
thread_local int t_outer = 0;
std::atomic<std::uint64_t> g_outer_calls{0}, g_inner_from_outer{0};
std::atomic<GetStateFn> g_entry{nullptr};
std::atomic<bool> g_wrapped{false};

std::int64_t now_us() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<std::int64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<std::int64_t>(c.QuadPart / freq * 1000000 + (c.QuadPart % freq) * 1000000 / freq);
}

HMODULE module_of(const void* p) {
    HMODULE m = nullptr;
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(p), &m))
        return nullptr;
    return m;
}

// "ff7remake_.exe+0x1d2f884", "XINPUT1_3.dll (this mod)+0x1234", "0x7ff... (no module)".
std::string describe(const void* p) {
    const HMODULE m = module_of(p);
    if (!m) return std::format("{:#x} (no module)", reinterpret_cast<std::uintptr_t>(p));
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring name(path, n);
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    std::string who = log::narrow(name);
    if (m == module_of(reinterpret_cast<const void*>(&proxy_XInputGetState))) who += " (this mod)";
    else if (m == g_real.module) who += " (system)";
    return std::format("{}+{:#x}", who, reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(m));
}

// Where the game's own XInput imports point (its import table for xinput1_3.dll).
std::string import_check() {
    auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return "game module not found";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return "no import table";
    std::string out;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        const char* name = reinterpret_cast<const char*>(base + d->Name);
        if (_stricmp(name, "xinput1_3.dll") != 0) continue;
        int i = 0;
        for (auto* t = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + d->FirstThunk); t->u1.Function; ++t, ++i) {
            if (!out.empty()) out += ", ";
            out += std::format("game import {} -> {}", i, describe(reinterpret_cast<const void*>(t->u1.Function)));
        }
    }
    return out.empty() ? "the game does not import xinput1_3.dll" : out;
}

// A jump target, followed through one relay ("jmp [rip+0]; <address>", as hook libraries
// place next to the patched function).
std::string describe_jump(const void* target) {
    std::string out = describe(target);
    const auto* t = static_cast<const unsigned char*>(target);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!module_of(target) && VirtualQuery(target, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE)) && t[0] == 0xFF && t[1] == 0x25 && t[2] == 0 && t[3] == 0 &&
        t[4] == 0 && t[5] == 0) {
        const void* final_target;
        std::memcpy(&final_target, t + 6, sizeof(final_target));
        out += " -> " + describe(final_target);
    }
    return out;
}

// Whether something patched the first bytes of our export or of the real function since start.
std::string entry_check() {
    const void* fns[2] = {reinterpret_cast<const void*>(&proxy_XInputGetState), reinterpret_cast<const void*>(g_real.get_state)};
    const char* names[2] = {"proxy entry", "system XInputGetState"};
    std::string out;
    for (int k = 0; k < 2; ++k) {
        if (!g_entry_saved[k] || !fns[k]) continue;
        const auto* b = static_cast<const unsigned char*>(fns[k]);
        if (!out.empty()) out += ", ";
        if (std::memcmp(b, g_entry_bytes[k], sizeof(g_entry_bytes[k])) == 0) {
            out += std::string(names[k]) + " unpatched";
        } else if (b[0] == 0xE9) {
            std::int32_t rel;
            std::memcpy(&rel, b + 1, 4);
            out += std::format("{} PATCHED, jumps to {}", names[k], describe_jump(b + 5 + rel));
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            std::int32_t rel;
            std::memcpy(&rel, b + 2, 4);
            const void* target;
            std::memcpy(&target, b + 6 + rel, sizeof(target));
            out += std::format("{} PATCHED, jumps to {}", names[k], describe_jump(target));
        } else {
            out += std::format("{} PATCHED (first byte {:#04x})", names[k], b[0]);
        }
    }
    return out;
}

void note_call(DWORD user, DWORD rc, void* caller) {
    if (!g_diag.load(std::memory_order_relaxed)) return;
    const std::int64_t t = now_us();
    const DWORD tid = GetCurrentThreadId();
    DWORD expected = 0;
    if (g_poll_thread.compare_exchange_strong(expected, tid)) {
        g_first_us = t;
        g_caller = caller;
        log::info("controls: the game's first XInputGetState call: user {}, thread {}, called from {}; the system reports {}", user, tid,
                  describe(caller), rc == ERROR_SUCCESS ? std::string("a pad") : rc == ERROR_DEVICE_NOT_CONNECTED ? std::string("no pad") : std::format("error {}", rc));
        log::info("controls: XInput {}; {}", import_check(), entry_check());
    } else {
        if (expected != tid) g_other_thread_calls.fetch_add(1, std::memory_order_relaxed);
        if (g_caller.load(std::memory_order_relaxed) != caller) {
            g_caller = caller;
            if (g_caller_changes.fetch_add(1) < 4) log::info("controls: XInputGetState now called from {} (thread {})", describe(caller), tid);
        }
    }
    if (user < kUsers) {
        UserDiag& u = g_users[user];
        u.calls.fetch_add(1, std::memory_order_relaxed);
        if (rc == ERROR_SUCCESS) u.ok.fetch_add(1, std::memory_order_relaxed);
        const std::int64_t last = u.last_us.exchange(t, std::memory_order_relaxed);
        if (last) {
            u.interval_sum_us.fetch_add(t - last, std::memory_order_relaxed);
            u.intervals.fetch_add(1, std::memory_order_relaxed);
        }
        const int c = rc == ERROR_SUCCESS ? 1 : 0;
        const int was = u.connected.exchange(c, std::memory_order_relaxed);
        if (was != c && (c == 1 || was == 1))
            log::info("controls: XInput user {} {}{}", user, c ? "has a pad" : "lost its pad",
                      c || rc == ERROR_DEVICE_NOT_CONNECTED ? std::string() : std::format(" (error {})", rc));
    } else {
        g_other_user_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (!g_rate_logged.load(std::memory_order_relaxed) && t - g_first_us.load() >= 3000000 && !g_rate_logged.exchange(true)) {
        std::string users;
        DWORD busiest = 0;
        for (DWORD i = 0; i < kUsers; ++i) {
            if (g_users[i].calls.load() > g_users[busiest].calls.load()) busiest = i;
            users += std::format("{}user {} {} calls{}", i ? ", " : "", i, g_users[i].calls.load(), g_users[i].connected.load() == 1 ? " (pad)" : "");
        }
        const UserDiag& b = g_users[busiest];
        const double every = b.intervals.load() ? double(b.interval_sum_us.load()) / double(b.intervals.load()) / 1000.0 : 0.0;
        log::info("controls: the game polls XInput on thread {} every {:.1f} ms (user {}; in {:.1f} s: {}; calls from other threads {}, "
                  "caller changes {})",
                  g_poll_thread.load(), every, busiest, double(t - g_first_us.load()) / 1e6, users, g_other_thread_calls.load(),
                  g_caller_changes.load());
    }
}

// `xinput timing`: the counters since the previous call (one line of the timing block).
std::string timing_line() {
    static std::mutex m;
    static std::uint64_t last_calls[kUsers] = {}, last_ok[kUsers] = {}, last_intervals[kUsers] = {}, last_other = 0, last_threads = 0;
    static std::int64_t last_sum[kUsers] = {};
    std::lock_guard lk(m);
    std::string per_user, pads;
    std::uint64_t total = 0;
    for (DWORD i = 0; i < kUsers; ++i) {
        const UserDiag& u = g_users[i];
        const std::uint64_t calls = u.calls.load(), ok = u.ok.load(), n = u.intervals.load();
        const std::int64_t sum = u.interval_sum_us.load();
        const std::uint64_t dc = calls - last_calls[i], dok = ok - last_ok[i], dn = n - last_intervals[i];
        const std::int64_t ds = sum - last_sum[i];
        total += dc;
        if (dc) {
            per_user += std::format("{}user {} {} ({} with a pad", per_user.empty() ? "" : ", ", i, dc, dok);
            if (dn) per_user += std::format(", every {:.1f} ms", double(ds) / double(dn) / 1000.0);
            per_user += ")";
        }
        if (u.connected.load() == 1) pads += std::format("{}{}", pads.empty() ? "" : " ", i);
        last_calls[i] = calls, last_ok[i] = ok, last_intervals[i] = n, last_sum[i] = sum;
    }
    const std::uint64_t other = g_other_user_calls.load(), threads = g_other_thread_calls.load();
    std::string line = std::format("ok xinput: {} XInputGetState calls{}{}; pads at user {}", total, per_user.empty() ? "" : ": ", per_user,
                                   pads.empty() ? "none" : pads);
    if (other - last_other) line += std::format("; {} calls for user indexes above 3", other - last_other);
    if (threads - last_threads) line += std::format("; {} calls from threads other than {}", threads - last_threads, g_poll_thread.load());
    if (!g_poll_thread.load()) line += "; the game has not called XInputGetState yet";
    static std::uint64_t last_outer = 0, last_inner = 0;
    const std::uint64_t outer = g_outer_calls.load(), inner = g_inner_from_outer.load();
    if (g_wrapped.load() && outer != last_outer && inner - last_inner != outer - last_outer)
        line += std::format("; {} of {} calls reached the system XInput (a hook on the export answered the rest)", inner - last_inner,
                            outer - last_outer);
    last_outer = outer, last_inner = inner;
    const std::string entries = entry_check();
    if (entries.find("PATCHED") != std::string::npos) line += "; " + entries;
    last_other = other, last_threads = threads;
    return line;
}

// What the game receives: the diagnostics, the virtual pad (dev) and the pad filter, applied to
// the result of the call chain below it.
DWORD post_process(DWORD user, XINPUT_STATE* state, DWORD rc, void* caller) {
    g_get_state_calls.fetch_add(1, std::memory_order_relaxed);
    note_call(user, rc, caller);
    if (user != 0 || !g_vpad_enabled.load(std::memory_order_relaxed) || !state) return filtered(user, state, rc);
    VirtualPad v;
    AcquireSRWLockShared(&g_vpad_lock);
    v = g_vpad;
    ReleaseSRWLockShared(&g_vpad_lock);
    if (rc != ERROR_SUCCESS) {
        // No real pad at index 0: present a connected, neutral pad plus the injected state.
        ZeroMemory(state, sizeof(*state));
        rc = ERROR_SUCCESS;
    }
    XINPUT_GAMEPAD& g = state->Gamepad;
    g.wButtons |= v.buttons;
    g.bLeftTrigger = v.lt > g.bLeftTrigger ? v.lt : g.bLeftTrigger;
    g.bRightTrigger = v.rt > g.bRightTrigger ? v.rt : g.bRightTrigger;
    g.sThumbLX = add_axis(g.sThumbLX, v.lx);
    g.sThumbLY = add_axis(g.sThumbLY, v.ly);
    g.sThumbRX = add_axis(g.sThumbRX, v.rx);
    g.sThumbRY = add_axis(g.sThumbRY, v.ry);
    // Packet number must change when the state changes, or XInput users may skip it.
    state->dwPacketNumber += g_vpad_packet.load(std::memory_order_relaxed);
    return filtered(user, state, rc);
}

// The game's import of XInputGetState is pointed at outer_get_state (install_import_wrapper):
// it calls our export through its current entry, so whatever hooked the export (the Steam
// overlay does, to serve Steam Input) runs first, and the filter applies to what that hook
// returns, whether or not it calls the original. The export itself then only forwards.

DWORD merged_get_state(DWORD user, XINPUT_STATE* state, GetStateFn fn, void* caller) {
    const DWORD rc = fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (t_outer) {
        g_inner_from_outer.fetch_add(1, std::memory_order_relaxed);
        return rc;  // outer_get_state finishes the job
    }
    return post_process(user, state, rc, caller);
}

DWORD WINAPI outer_get_state(DWORD user, XINPUT_STATE* state) {
    g_outer_calls.fetch_add(1, std::memory_order_relaxed);
    const GetStateFn entry = g_entry.load(std::memory_order_relaxed);
    ++t_outer;
    const DWORD rc = entry(user, state);
    --t_outer;
    return post_process(user, state, rc, _ReturnAddress());
}

// Points the game's import slot for our XInputGetState export at outer_get_state.
std::string install_import_wrapper() {
    const void* ours = reinterpret_cast<const void*>(&proxy_XInputGetState);
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return "game module not found";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return "no import table";
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "xinput1_3.dll") != 0) continue;
        for (auto* t = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk); t->u1.Function; ++t) {
            if (reinterpret_cast<const void*>(t->u1.Function) != ours) continue;
            g_entry = &proxy_XInputGetState;
            DWORD old = 0;
            if (!VirtualProtect(&t->u1.Function, sizeof(t->u1.Function), PAGE_READWRITE, &old))
                return std::format("VirtualProtect failed ({})", GetLastError());
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&t->u1.Function), reinterpret_cast<void*>(&outer_get_state));
            VirtualProtect(&t->u1.Function, sizeof(t->u1.Function), old, &old);
            g_wrapped = true;
            return std::format("import slot {} now calls the pad filter after any hook on the export",
                               describe(&t->u1.Function));
        }
    }
    return "the game's import of XInputGetState does not point to this DLL";
}

}  // namespace

bool load_real() { return real().module != nullptr; }
std::wstring real_path() { return real().path; }

void set_virtual_pad_enabled(bool enabled) { g_vpad_enabled = enabled; }
bool virtual_pad_enabled() { return g_vpad_enabled; }
void set_virtual_pad(const VirtualPad& s) {
    AcquireSRWLockExclusive(&g_vpad_lock);
    g_vpad = s;
    ReleaseSRWLockExclusive(&g_vpad_lock);
    g_vpad_packet.fetch_add(1);
}
VirtualPad virtual_pad() {
    AcquireSRWLockShared(&g_vpad_lock);
    VirtualPad v = g_vpad;
    ReleaseSRWLockShared(&g_vpad_lock);
    return v;
}
std::uint64_t get_state_calls() { return g_get_state_calls.load(); }
void set_pad_filter(PadFilter filter) { g_pad_filter = filter; }
void set_stick_filter(StickFilter filter) { g_stick_filter = filter; }
void set_pad_override(PadOverride filter) { g_pad_override = filter; }

void stick_magnitudes(float* left, float* right) {
    if (left) *left = g_stick_left.load(std::memory_order_relaxed);
    if (right) *right = g_stick_right.load(std::memory_order_relaxed);
}

namespace {
GetStateFn proxy_entry() { return &proxy_XInputGetState; }
}  // namespace

void register_diagnostics(bool wrap_import) {
    const void* fns[2] = {reinterpret_cast<const void*>(&proxy_XInputGetState), reinterpret_cast<const void*>(real().get_state)};
    for (int k = 0; k < 2; ++k) {
        if (!fns[k]) continue;
        std::memcpy(g_entry_bytes[k], fns[k], sizeof(g_entry_bytes[k]));
        g_entry_saved[k] = true;
    }
    g_diag = true;
    if (wrap_import) log::info("controls: XInput: {}", install_import_wrapper());
    dev_commands::add("xinput", "xinput status | timing: who polls XInput, how often, which user indexes have a pad",
                      [](std::string_view args) -> std::string {
                          if (args.starts_with("timing")) return timing_line();
                          if (args.starts_with("probe")) {
                              // Calls the export's current entry for users 0..3 and reports whether
                              // each call reached the system DLL (a hook may answer by itself).
                              std::string out = "ok";
                              for (DWORD u = 0; u < kUsers; ++u) {
                                  XINPUT_STATE st{};
                                  const std::uint64_t before = g_inner_from_outer.load();
                                  ++t_outer;
                                  const DWORD rc = proxy_entry()(u, &st);
                                  --t_outer;
                                  out += std::format("{} user {}: rc {}, buttons {:#06x}, reached the system XInput {}", u ? ";" : "", u, rc,
                                                     st.Gamepad.wButtons, g_inner_from_outer.load() - before ? "yes" : "no");
                              }
                              return out + "; " + entry_check();
                          }
                          std::string users;
                          for (DWORD i = 0; i < kUsers; ++i)
                              users += std::format("{}user {} calls {} with a pad {}", i ? ", " : "", i, g_users[i].calls.load(), g_users[i].ok.load());
                          void* caller = g_caller.load();
                          const std::uint64_t given = g_last_given.load();
                          return std::format("ok calls {} on thread {}; {}; {}; {}; last caller {}; wrapper calls {}, of them reached the system {}; "
                                             "last state the game received: buttons {:#06x} triggers {} {} left stick {} {}; polls withheld by the "
                                             "settings panel {}",
                                             g_get_state_calls.load(), g_poll_thread.load(), users, import_check(), entry_check(),
                                             caller ? describe(caller) : std::string("none"), g_outer_calls.load(), g_inner_from_outer.load(),
                                             given & 0xFFFF, (given >> 16) & 0xFF, (given >> 24) & 0xFF, std::int16_t((given >> 32) & 0xFFFF),
                                             std::int16_t((given >> 48) & 0xFFFF), g_overridden.load());
                      });
}

}  // namespace ff7vr::loader::xinput

// ------------------------------------------------------------------ exports
// Names are mapped to the real export names and ordinals by xinput1_3.def.
using namespace ff7vr::loader::xinput;

extern "C" {

DWORD WINAPI proxy_XInputGetState(DWORD user, XINPUT_STATE* state) {
    return merged_get_state(user, state, real().get_state, _ReturnAddress());
}

DWORD WINAPI proxy_XInputSetState(DWORD user, XINPUT_VIBRATION* vib) {
    auto fn = real().set_state;
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD rc = fn(user, vib);
    if (rc != ERROR_SUCCESS && user == 0 && virtual_pad_enabled()) return ERROR_SUCCESS;  // virtual pad accepts rumble
    return rc;
}

DWORD WINAPI proxy_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    auto fn = real().get_caps;
    DWORD rc = fn ? fn(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (rc != ERROR_SUCCESS && user == 0 && virtual_pad_enabled() && caps) {
        ZeroMemory(caps, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = -64;
        rc = ERROR_SUCCESS;
    }
    return rc;
}

void WINAPI proxy_XInputEnable(BOOL enable) {
    if (auto fn = real().enable) fn(enable);
}

DWORD WINAPI proxy_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
    auto fn = real().get_dsound;
    return fn ? fn(user, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI proxy_XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info) {
    auto fn = real().get_battery;
    return fn ? fn(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI proxy_XInputGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE ks) {
    auto fn = real().get_keystroke;
    return fn ? fn(user, reserved, ks) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI proxy_XInputGetStateEx(DWORD user, XINPUT_STATE* state) {
    auto fn = real().get_state_ex;
    return merged_get_state(user, state, fn ? fn : real().get_state, _ReturnAddress());
}

DWORD WINAPI proxy_XInputWaitForGuideButton(DWORD user, DWORD flags, void* listen) {
    auto fn = real().wait_guide;
    return fn ? fn(user, flags, listen) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI proxy_XInputCancelGuideButtonWait(DWORD user) {
    auto fn = real().cancel_guide;
    return fn ? fn(user) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI proxy_XInputPowerOffController(DWORD user) {
    auto fn = real().power_off;
    return fn ? fn(user) : ERROR_DEVICE_NOT_CONNECTED;
}

}  // extern "C"
