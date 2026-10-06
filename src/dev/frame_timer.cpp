// Present-based frame timer for the benchmark.
//
// Hooks the body of IDXGISwapChain::Present in dxgi.dll (an inline hook, not
// a vtable slot). Overlays and VR injectors such as UEVR usually replace the
// vtable slot instead, so both hooks chain: the game calls the slot, the
// other tool runs its code and calls the original function, which lands in
// this detour. The frame interval measured here is therefore the same whether
// or not something else hooks Present, and nothing here depends on hook
// order. The render module hooks the same body through the same hook library,
// which hooks a function only once; with the render module on, this timer
// replaces the vtable slot instead (and so runs before the render module's hook).
//
// Cost per frame: two QueryPerformanceCounter calls and one store into a
// preallocated array. No allocation, no locks, no I/O on the render thread.
//
// The hook is installed from a worker thread once the game has a visible
// window, so nothing here runs during the game's early start-up.

#include "frame_timer.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>

namespace ff7vr::dev::frame_timer {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

struct Frame {
    std::int64_t start;    // QPC at entry to Present
    std::int64_t end;      // QPC when Present returned
    std::uint32_t sync;    // sync interval passed to the original Present
    std::uint32_t flags;
};

constexpr std::size_t kCapacity = std::size_t{1} << 20;  // 2.4 h at 120 fps, 24 MB

Options g_opts;
hook::InlineHook g_present;
// Used when the body is already hooked through the same hook library (the render module
// hooks it too, and the library hooks a function once): the vtable slot instead.
hook::VTableHook g_present_slot;
std::atomic<PresentFn> g_original{nullptr};
std::atomic<bool> g_hooked{false};
std::int64_t g_freq = 1;

Frame* g_frames = nullptr;
std::atomic<std::size_t> g_count{0};
std::atomic<bool> g_recording{false};
std::atomic<std::uint64_t> g_total{0};          // presents of the tracked swap chain
std::atomic<std::uint64_t> g_other{0};          // presents of other swap chains
std::atomic<IDXGISwapChain*> g_tracked{nullptr};
std::atomic<std::int64_t> g_last_tracked{0};
std::atomic<std::uint32_t> g_bb_w{0}, g_bb_h{0}, g_bb_fmt{0}, g_last_sync{0}, g_last_flags{0}, g_bb_count{0};
thread_local int t_depth = 0;

std::int64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

void read_desc(IDXGISwapChain* sc, bool log_it) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return;
    const bool changed = d.BufferDesc.Width != g_bb_w.load() || d.BufferDesc.Height != g_bb_h.load() ||
                         static_cast<std::uint32_t>(d.BufferDesc.Format) != g_bb_fmt.load();
    g_bb_w = d.BufferDesc.Width;
    g_bb_h = d.BufferDesc.Height;
    g_bb_fmt = static_cast<std::uint32_t>(d.BufferDesc.Format);
    g_bb_count = d.BufferCount;
    if (log_it || changed) {
        log::info("frame_timer: swap chain {} back buffer {}x{} format {} buffers {} swap effect {} windowed {} flags 0x{:x}",
                  static_cast<void*>(sc), d.BufferDesc.Width, d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format),
                  d.BufferCount, static_cast<int>(d.SwapEffect), d.Windowed ? 1 : 0, d.Flags);
    }
}

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const auto original = g_original.load(std::memory_order_acquire);
    // Test presents and nested calls (a hook further out presenting again) are not frames.
    if ((flags & DXGI_PRESENT_TEST) != 0 || t_depth > 0) return original(sc, sync, flags);

    const std::int64_t t0 = qpc();
    // Track one swap chain: the first one that presents, replaced when it has
    // been silent for two seconds (the game recreated its swap chain).
    IDXGISwapChain* tracked = g_tracked.load(std::memory_order_relaxed);
    if (tracked != sc) {
        if (tracked == nullptr || t0 - g_last_tracked.load(std::memory_order_relaxed) > 2 * g_freq) {
            g_tracked.store(sc, std::memory_order_relaxed);
            log::info("frame_timer: tracking swap chain {} (sync interval {}, flags 0x{:x})", static_cast<void*>(sc), sync, flags);
            read_desc(sc, true);
            tracked = sc;
        } else {
            g_other.fetch_add(1, std::memory_order_relaxed);
            return original(sc, sync, flags);
        }
    }

    const UINT used_sync = g_opts.force_sync_interval >= 0 ? static_cast<UINT>(g_opts.force_sync_interval) : sync;
    if (sync != g_last_sync.load(std::memory_order_relaxed) || flags != g_last_flags.load(std::memory_order_relaxed)) {
        g_last_sync = sync;
        g_last_flags = flags;
        log::info("frame_timer: game presents with sync interval {} flags 0x{:x}{}", sync, flags,
                  g_opts.force_sync_interval >= 0 ? std::format(" (replaced by sync interval {})", used_sync) : std::string());
    }

    ++t_depth;
    const HRESULT hr = original(sc, used_sync, flags);
    --t_depth;
    const std::int64_t t1 = qpc();

    const std::uint64_t n = g_total.fetch_add(1, std::memory_order_relaxed) + 1;
    g_last_tracked.store(t1, std::memory_order_relaxed);
    if ((n & 1023) == 0) read_desc(sc, false);

    if (g_recording.load(std::memory_order_acquire)) {
        const std::size_t i = g_count.load(std::memory_order_relaxed);
        if (i < kCapacity) {
            g_frames[i] = Frame{t0, t1, used_sync, flags};
            g_count.store(i + 1, std::memory_order_release);
        }
    }
    return hr;
}

// Finds CDXGISwapChain::Present through a throwaway swap chain on a NULL
// driver device (no GPU work, no visible window).
void* find_present(void*** vtable_out) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ff7vr frame timer", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
    if (!wnd) {
        log::error("frame_timer: CreateWindowEx failed ({})", GetLastError());
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = wnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    void* target = nullptr;
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_NULL, D3D_DRIVER_TYPE_WARP};
    for (D3D_DRIVER_TYPE type : types) {
        IDXGISwapChain* sc = nullptr;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc,
                                                         &dev, nullptr, &ctx);
        if (SUCCEEDED(hr) && sc) {
            *vtable_out = *reinterpret_cast<void***>(sc);
            target = (*vtable_out)[8];
        } else {
            log::warn("frame_timer: dummy swap chain (driver type {}) failed: 0x{:08x}", static_cast<int>(type),
                      static_cast<unsigned>(hr));
        }
        if (sc) sc->Release();
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        if (target) break;
    }
    DestroyWindow(wnd);
    return target;
}

bool process_has_visible_window() {
    struct Ctx { DWORD pid; bool found; } c{GetCurrentProcessId(), false};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* cx = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != cx->pid || !IsWindowVisible(h)) return TRUE;
            RECT r{};
            GetWindowRect(h, &r);
            if ((r.right - r.left) * (r.bottom - r.top) > 10000) {
                cx->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&c));
    return c.found;
}

DWORD WINAPI install_thread(void*) {
    // Wait for the game's window: by then the engine has its D3D device, and
    // nothing here interferes with the start-up.
    for (int i = 0; i < 1200 && !process_has_visible_window(); ++i) Sleep(250);
    void** vtable = nullptr;
    void* target = find_present(&vtable);
    if (!target) {
        log::error("frame_timer: could not find IDXGISwapChain::Present; no frame timing");
        return 1;
    }
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(target), &owner);
    wchar_t name[MAX_PATH] = L"?";
    if (owner) GetModuleFileNameW(owner, name, MAX_PATH);
    // The detour may run as soon as a hook is in place: the original is stored first.
    g_original = reinterpret_cast<PresentFn>(target);
    bool hooked = g_present.create(target, &present_detour, false);
    if (hooked) {
        g_original = g_present.original<PresentFn>();
        hooked = g_present.enable();
        if (!hooked) g_present.remove();
    }
    if (!hooked && vtable && g_present_slot.create(vtable, 8, &present_detour)) {
        g_original = g_present_slot.original<PresentFn>();
        hooked = true;
        log::info("frame_timer: Present's body is already hooked through the same hook library; using the vtable slot");
    }
    if (!hooked) {
        log::error("frame_timer: hooking Present at {} failed", target);
        return 1;
    }
    g_hooked = true;
    log::info("frame_timer: hooked IDXGISwapChain::Present at {} ({}+0x{:x}){}", target, log::narrow(name),
              reinterpret_cast<std::uintptr_t>(target) - reinterpret_cast<std::uintptr_t>(owner),
              g_opts.force_sync_interval >= 0 ? std::format(", forcing sync interval {}", g_opts.force_sync_interval)
                                              : std::string());
    return 0;
}

}  // namespace

bool start(const Options& options) {
    g_opts = options;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    g_frames = static_cast<Frame*>(VirtualAlloc(nullptr, kCapacity * sizeof(Frame), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_frames) {
        log::error("frame_timer: cannot allocate the frame buffer");
        return false;
    }
    HANDLE t = CreateThread(nullptr, 0, install_thread, nullptr, 0, nullptr);
    if (!t) return false;
    SetThreadDescription(t, L"ff7vr frame timer");
    CloseHandle(t);
    log::info("frame_timer: enabled, waiting for the game window");
    return true;
}

std::string status() {
    return std::format("ok hooked={} recording={} frames={} total={} other={} backbuffer={}x{} format={} buffers={} sync={} flags=0x{:x}",
                       g_hooked.load() ? 1 : 0, g_recording.load() ? 1 : 0, g_count.load(), g_total.load(),
                       g_other.load(), g_bb_w.load(), g_bb_h.load(), g_bb_fmt.load(), g_bb_count.load(),
                       g_last_sync.load(), g_last_flags.load());
}

std::string begin_recording() {
    if (!g_hooked) return "err frame timer not hooked";
    g_recording.store(false, std::memory_order_release);
    g_count.store(0, std::memory_order_release);
    g_recording.store(true, std::memory_order_release);
    log::info("frame_timer: recording started");
    return "ok";
}

std::string end_recording(const std::string& csv_path) {
    g_recording.store(false, std::memory_order_release);
    Sleep(50);  // let a Present that is storing right now finish
    const std::size_t n = g_count.load(std::memory_order_acquire);
    FILE* f = nullptr;
    if (_wfopen_s(&f, log::widen(csv_path).c_str(), L"wb") != 0 || !f) return "err cannot open " + csv_path;
    std::fprintf(f, "frame,t_ms,interval_ms,present_ms,sync_interval,flags\n");
    const double to_ms = 1000.0 / static_cast<double>(g_freq);
    for (std::size_t i = 0; i < n; ++i) {
        const Frame& fr = g_frames[i];
        const double t = static_cast<double>(fr.start - g_frames[0].start) * to_ms;
        const double interval = i == 0 ? 0.0 : static_cast<double>(fr.start - g_frames[i - 1].start) * to_ms;
        const double present = static_cast<double>(fr.end - fr.start) * to_ms;
        std::fprintf(f, "%zu,%.4f,%.4f,%.4f,%u,%u\n", i, t, interval, present, fr.sync, fr.flags);
    }
    std::fclose(f);
    log::info("frame_timer: recording stopped, {} frames written to {}", n, csv_path);
    return std::format("ok frames={} backbuffer={}x{}", n, g_bb_w.load(), g_bb_h.load());
}

}  // namespace ff7vr::dev::frame_timer
