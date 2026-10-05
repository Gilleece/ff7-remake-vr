#include "xinput_proxy.h"

#include "ff7vr/core/log.h"

#include <xinput.h>

#include <atomic>
#include <mutex>

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

DWORD merged_get_state(DWORD user, XINPUT_STATE* state, GetStateFn fn) {
    g_get_state_calls.fetch_add(1, std::memory_order_relaxed);
    DWORD rc = fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (user != 0 || !g_vpad_enabled.load(std::memory_order_relaxed) || !state) return rc;
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
    return rc;
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

}  // namespace ff7vr::loader::xinput

// ------------------------------------------------------------------ exports
// Names are mapped to the real export names and ordinals by xinput1_3.def.
using namespace ff7vr::loader::xinput;

extern "C" {

DWORD WINAPI proxy_XInputGetState(DWORD user, XINPUT_STATE* state) {
    return merged_get_state(user, state, real().get_state);
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
    return merged_get_state(user, state, fn ? fn : real().get_state);
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
