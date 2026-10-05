#include "d3d11_hooks.h"

#include "timing.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/xr/xr.h"

#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <vector>

namespace ff7vr::render {
namespace {

using Microsoft::WRL::ComPtr;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using D3D11CreateDeviceFn = decltype(&D3D11CreateDevice);
using D3D11CreateDeviceAndSwapChainFn = decltype(&D3D11CreateDeviceAndSwapChain);

// vtable slots
constexpr size_t kPresent = 8, kResizeBuffers = 13, kPresent1 = 22, kResizeBuffers1 = 39;
constexpr size_t kCreateSwapChain = 10, kCreateSwapChainForHwnd = 15;
constexpr size_t kCreateDeferredContext = 27;

HookCallbacks g_cb;
int64_t g_qpcFreq = 1;

// One swap chain class (normally only DXGI's own).
struct VtableHooks {
    void** vtable = nullptr;
    hook::VTableHook present, present1, resize, resize1;
};
std::mutex g_vtMutex;
std::array<std::unique_ptr<VtableHooks>, 4> g_vt;
std::atomic<int> g_vtCount{0};

hook::InlineHook g_createSwapChain, g_createSwapChainForHwnd, g_createDeferred, g_createDevice, g_createDeviceAndSwapChain;

thread_local int t_depth = 0;  // > 0 while inside one of our detours (nested calls go straight through)

// ---- facts ----
std::atomic<DWORD> g_deviceCreateTid{0};
std::atomic<uint32_t> g_deferredContexts{0};
void* g_deferredTarget = nullptr;  // ID3D11Device::CreateDeferredContext of the probe device (hooked)
std::atomic<uint32_t> g_resizeCount{0};
std::atomic<DWORD> g_presentTid{0};
std::mutex g_factsMutex;
std::string g_factsLine = "no Present seen yet";

// ---- thread names announced through the debugger exception ----
struct NamedThread {
    DWORD tid;
    char name[48];
};
SRWLOCK g_namesLock = SRWLOCK_INIT;
std::array<NamedThread, 256> g_names{};
size_t g_nameCount = 0;

LONG CALLBACK ThreadNameVeh(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != 0x406D1388 || r->NumberParameters < 3 || r->ExceptionInformation[0] != 0x1000)
        return EXCEPTION_CONTINUE_SEARCH;
    const char* name = reinterpret_cast<const char*>(r->ExceptionInformation[1]);
    DWORD tid = static_cast<DWORD>(r->ExceptionInformation[2]);
    if (tid == static_cast<DWORD>(-1)) tid = GetCurrentThreadId();
    if (!name) return EXCEPTION_CONTINUE_SEARCH;
    AcquireSRWLockExclusive(&g_namesLock);
    size_t slot = g_nameCount;
    for (size_t i = 0; i < g_nameCount; ++i)
        if (g_names[i].tid == tid) slot = i;
    if (slot < g_names.size()) {
        g_names[slot].tid = tid;
        __try {
            strncpy_s(g_names[slot].name, name, _TRUNCATE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_names[slot].name[0] = 0;
        }
        if (slot == g_nameCount) ++g_nameCount;
    }
    ReleaseSRWLockExclusive(&g_namesLock);
    return EXCEPTION_CONTINUE_SEARCH;  // the engine's own handler continues execution
}

// ---- swap chain tracking ----
struct TrackedSwapchain {
    IDXGISwapChain* sc = nullptr;  // identity only, not referenced
    HWND window = nullptr;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int64_t lastPresent = 0;
    uint64_t presents = 0;
    UINT lastSync = ~0u, lastFlags = ~0u;
    bool logged = false;
};
// Only touched on presenting threads under g_trackMutex (uncontended in practice).
std::mutex g_trackMutex;
std::vector<TrackedSwapchain> g_tracked;
IDXGISwapChain* g_main = nullptr;
int64_t g_lastMainPresent = 0;

std::string FormatName(DXGI_FORMAT f) { return std::format("{} ({})", xr::DxgiFormatName(f), static_cast<int>(f)); }

void LogSwapchainFacts(IDXGISwapChain* sc, const DXGI_SWAP_CHAIN_DESC& d, ID3D11Device* dev, ID3D11Texture2D* bb) {
    const DWORD tid = GetCurrentThreadId();
    DWORD windowTid = d.OutputWindow ? GetWindowThreadProcessId(d.OutputWindow, nullptr) : 0;
    log::info("d3d11: swap chain {}: {}x{} {}, {} buffers, swap effect {}, flags 0x{:X}, usage 0x{:X}, {} , window {} (thread {})",
              static_cast<void*>(sc), d.BufferDesc.Width, d.BufferDesc.Height, FormatName(d.BufferDesc.Format), d.BufferCount,
              static_cast<int>(d.SwapEffect), d.Flags, static_cast<unsigned>(d.BufferUsage), d.Windowed ? "windowed" : "fullscreen",
              static_cast<void*>(d.OutputWindow), windowTid);
    ComPtr<IDXGISwapChain1> sc1;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        DXGI_SWAP_CHAIN_DESC1 d1{};
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fd{};
        sc1->GetDesc1(&d1);
        sc1->GetFullscreenDesc(&fd);
        log::info("d3d11:   desc1: scaling {}, alpha mode {}, stereo {}, fullscreen desc: windowed {}, refresh {}/{}", static_cast<int>(d1.Scaling),
                  static_cast<int>(d1.AlphaMode), d1.Stereo ? 1 : 0, fd.Windowed ? 1 : 0, fd.RefreshRate.Numerator,
                  fd.RefreshRate.Denominator);
    }
    if (bb) {
        D3D11_TEXTURE2D_DESC td{};
        bb->GetDesc(&td);
        log::info("d3d11:   back buffer texture: {}x{} {}, bind 0x{:X}, misc 0x{:X}, usage {}, samples {}", td.Width, td.Height,
                  FormatName(td.Format), td.BindFlags, td.MiscFlags, static_cast<int>(td.Usage), td.SampleDesc.Count);
    }
    std::string adapter = "?";
    uint64_t luid = 0;
    if (dev) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> ad;
        DXGI_ADAPTER_DESC ade{};
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&ade))) {
            adapter = log::narrow(ade.Description);
            luid = (static_cast<uint64_t>(static_cast<uint32_t>(ade.AdapterLuid.HighPart)) << 32) | ade.AdapterLuid.LowPart;
        }
        ComPtr<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(&ctx);
        BOOL mtProtected = FALSE;
        ComPtr<ID3D11Multithread> mt;
        if (ctx && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt)))) mtProtected = mt->GetMultithreadProtected();
        void* deferredFn = (*reinterpret_cast<void***>(dev))[kCreateDeferredContext];
        log::info("d3d11:   device {}: adapter '{}', LUID {:08X}:{:08X}, feature level 0x{:X}, creation flags 0x{:X}, multithread protected {}; "
                  "CreateDeferredContext is {} ({})",
                  static_cast<void*>(dev), adapter, static_cast<uint32_t>(luid >> 32), static_cast<uint32_t>(luid),
                  static_cast<int>(dev->GetFeatureLevel()), dev->GetCreationFlags(), mtProtected ? 1 : 0,
                  module::describe(reinterpret_cast<uintptr_t>(deferredFn)),
                  deferredFn == g_deferredTarget ? "hooked, deferred contexts are counted" : "NOT the hooked function, the count below is incomplete");
    }
    const std::string ptName = ThreadName(tid), wtName = ThreadName(windowTid), dtName = ThreadName(g_deviceCreateTid.load());
    log::info("d3d11:   threads: Present on {} '{}', window owned by {} '{}', device created on {} '{}', main thread is {}; "
              "deferred contexts created so far: {}",
              tid, ptName, windowTid, wtName, g_deviceCreateTid.load(), dtName, tid == windowTid ? "the presenting thread" : "another thread",
              g_deferredContexts.load());
    std::lock_guard lk(g_factsMutex);
    g_factsLine = std::format("backbuffer {}x{} {} buffers {} swapeffect {} {} adapter '{}' luid {:016X} window_tid {}", d.BufferDesc.Width,
                              d.BufferDesc.Height, xr::DxgiFormatName(d.BufferDesc.Format), d.BufferCount, static_cast<int>(d.SwapEffect),
                              d.Windowed ? "windowed" : "fullscreen", adapter, luid, windowTid);
}

// Called on every Present before the real one. Returns the main-swapchain flag and fills `t`.
bool TrackPresent(IDXGISwapChain* sc, UINT sync, UINT flags, int64_t now, DXGI_SWAP_CHAIN_DESC* outDesc, bool* firstTime) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return false;
    *outDesc = d;
    std::lock_guard lk(g_trackMutex);
    TrackedSwapchain* t = nullptr;
    for (auto& e : g_tracked)
        if (e.sc == sc) t = &e;
    if (!t) {
        // Forget swap chains that have been silent for 10 s (released or replaced).
        std::erase_if(g_tracked, [&](const TrackedSwapchain& e) { return now - e.lastPresent > 10 * g_qpcFreq; });
        if (g_main && std::none_of(g_tracked.begin(), g_tracked.end(), [](const TrackedSwapchain& e) { return e.sc == g_main; }))
            g_main = nullptr;
        g_tracked.push_back(TrackedSwapchain{sc});
        t = &g_tracked.back();
    }
    const bool resized = t->width != d.BufferDesc.Width || t->height != d.BufferDesc.Height || t->format != d.BufferDesc.Format;
    if (resized && t->presents)
        log::info("d3d11: swap chain {} is now {}x{} {} ({})", static_cast<void*>(sc), d.BufferDesc.Width, d.BufferDesc.Height,
                  xr::DxgiFormatName(d.BufferDesc.Format), d.Windowed ? "windowed" : "fullscreen");
    t->window = d.OutputWindow;
    t->width = d.BufferDesc.Width;
    t->height = d.BufferDesc.Height;
    t->format = d.BufferDesc.Format;
    t->lastPresent = now;
    ++t->presents;
    if (sync != t->lastSync || flags != t->lastFlags) {
        log::info("d3d11: swap chain {} presents with sync interval {} flags 0x{:X}", static_cast<void*>(sc), sync, flags);
        t->lastSync = sync;
        t->lastFlags = flags;
    }
    *firstTime = !t->logged;
    t->logged = true;

    // Main swap chain: the largest one with a visible window among those that
    // presented during the last second. Normally there is exactly one.
    IDXGISwapChain* best = nullptr;
    uint64_t bestArea = 0;
    for (const auto& e : g_tracked) {
        if (now - e.lastPresent > g_qpcFreq) continue;
        const bool visible = e.window && IsWindowVisible(e.window);
        const uint64_t area = uint64_t(e.width) * e.height * (visible ? 2 : 1);
        if (area > bestArea) {
            bestArea = area;
            best = e.sc;
        }
    }
    if (best != g_main) {
        log::info("d3d11: main swap chain is now {} ({} swap chain(s) presenting)", static_cast<void*>(best), g_tracked.size());
        g_main = best;
        g_lastMainPresent = 0;
    }
    return sc == g_main;
}

void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags, int64_t t0) {
    DXGI_SWAP_CHAIN_DESC d{};
    bool first = false;
    if (!TrackPresent(sc, sync, flags, t0, &d, &first)) return;
    Timing& tm = GetTiming();
    if (g_lastMainPresent) tm.frameInterval.Add(QpcToMs(t0 - g_lastMainPresent));
    g_lastMainPresent = t0;
    const DWORD tid = GetCurrentThreadId();
    if (const DWORD prev = g_presentTid.exchange(tid); prev != tid) {
        const DWORD windowTid = d.OutputWindow ? GetWindowThreadProcessId(d.OutputWindow, nullptr) : 0;
        log::info("d3d11: the main swap chain is now presented from thread {} '{}' ({}; before: {})", tid, ThreadName(tid),
                  tid == windowTid ? "the window's thread, i.e. the game thread" : "not the window's thread", prev);
    }

    ComPtr<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return;  // not a D3D11 swap chain
    ComPtr<ID3D11Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (first) LogSwapchainFacts(sc, d, dev.Get(), bb.Get());
    if (!bb || !g_cb.onPresent) return;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    PresentInfo pi;
    pi.swapchain = sc;
    pi.device = dev.Get();
    pi.context = ctx.Get();
    pi.backBuffer = bb.Get();
    pi.format = d.BufferDesc.Format;
    pi.width = d.BufferDesc.Width;
    pi.height = d.BufferDesc.Height;
    pi.window = d.OutputWindow;
    pi.syncInterval = sync;
    pi.flags = flags;
    pi.entryQpc = t0;
    g_cb.onPresent(pi);
    // bb, ctx and dev are released here: no reference to a back buffer outlives the Present call.
}

// Runs our Present work and keeps an exception in it from reaching the game.
void SafeOnPresent(IDXGISwapChain* sc, UINT sync, UINT flags, int64_t t0) {
    try {
        OnPresent(sc, sync, flags, t0);
    } catch (const std::exception& e) {
        log::error("render: exception in Present hook: {}", e.what());
    } catch (...) {
        log::error("render: unknown exception in Present hook");
    }
}

VtableHooks* FindVtable(void* self) {
    void** vt = *reinterpret_cast<void***>(self);
    const int n = g_vtCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_vt[i] && g_vt[i]->vtable == vt) return g_vt[i].get();
    return n > 0 ? g_vt[0].get() : nullptr;  // an object we never saw: use the first class's originals
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const auto orig = FindVtable(sc)->present.original<PresentFn>();
    if (t_depth > 0 || (flags & DXGI_PRESENT_TEST)) return orig(sc, sync, flags);
    ++t_depth;
    const int64_t t0 = QpcNow();
    SafeOnPresent(sc, sync, flags, t0);
    const int64_t t1 = QpcNow();
    const HRESULT hr = orig(sc, sync, flags);
    const int64_t t2 = QpcNow();
    --t_depth;
    if (sc == g_main) {
        GetTiming().hook.Add(QpcToMs(t1 - t0));
        GetTiming().presentCall.Add(QpcToMs(t2 - t1));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    const auto orig = FindVtable(sc)->present1.original<Present1Fn>();
    if (t_depth > 0 || (flags & DXGI_PRESENT_TEST)) return orig(sc, sync, flags, params);
    ++t_depth;
    const int64_t t0 = QpcNow();
    SafeOnPresent(sc, sync, flags, t0);
    const int64_t t1 = QpcNow();
    const HRESULT hr = orig(sc, sync, flags, params);
    const int64_t t2 = QpcNow();
    --t_depth;
    if (sc == g_main) {
        GetTiming().hook.Add(QpcToMs(t1 - t0));
        GetTiming().presentCall.Add(QpcToMs(t2 - t1));
    }
    return hr;
}

void BeforeResize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt) {
    ++g_resizeCount;
    log::info("d3d11: ResizeBuffers on swap chain {} (thread {}): {} buffers {}x{} {}", static_cast<void*>(sc), GetCurrentThreadId(), count,
              w, h, xr::DxgiFormatName(fmt));
    if (g_cb.onResize) {
        try {
            g_cb.onResize(sc);
        } catch (...) {
        }
    }
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    const auto orig = FindVtable(sc)->resize.original<ResizeBuffersFn>();
    if (t_depth > 0) return orig(sc, count, w, h, fmt, flags);
    ++t_depth;
    BeforeResize(sc, count, w, h, fmt);
    const HRESULT hr = orig(sc, count, w, h, fmt, flags);
    --t_depth;
    if (FAILED(hr)) log::error("d3d11: ResizeBuffers failed 0x{:08X}", static_cast<uint32_t>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE ResizeBuffers1Detour(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                               const UINT* masks, IUnknown* const* queues) {
    const auto orig = FindVtable(sc)->resize1.original<ResizeBuffers1Fn>();
    if (t_depth > 0) return orig(sc, count, w, h, fmt, flags, masks, queues);
    ++t_depth;
    BeforeResize(sc, count, w, h, fmt);
    const HRESULT hr = orig(sc, count, w, h, fmt, flags, masks, queues);
    --t_depth;
    if (FAILED(hr)) log::error("d3d11: ResizeBuffers1 failed 0x{:08X}", static_cast<uint32_t>(hr));
    return hr;
}

// Hooks the Present/ResizeBuffers slots of this swap chain's vtable if that vtable is new.
void HookSwapchainClass(IDXGISwapChain* sc) {
    if (!sc) return;
    void** vt = *reinterpret_cast<void***>(sc);
    std::lock_guard lk(g_vtMutex);
    const int n = g_vtCount.load();
    for (int i = 0; i < n; ++i)
        if (g_vt[i]->vtable == vt) return;
    if (n >= static_cast<int>(g_vt.size())) {
        log::warn("d3d11: too many swap chain classes; swap chain {} is not hooked", static_cast<void*>(sc));
        return;
    }
    auto h = std::make_unique<VtableHooks>();
    h->vtable = vt;
    if (!h->present.create(vt, kPresent, &PresentDetour)) return;
    h->resize.create(vt, kResizeBuffers, &ResizeBuffersDetour);
    // Present1 / ResizeBuffers1 exist when the class implements the newer interfaces through the same vtable.
    ComPtr<IDXGISwapChain1> sc1;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && *reinterpret_cast<void***>(sc1.Get()) == vt)
        h->present1.create(vt, kPresent1, &Present1Detour);
    ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && *reinterpret_cast<void***>(sc3.Get()) == vt)
        h->resize1.create(vt, kResizeBuffers1, &ResizeBuffers1Detour);
    log::info("d3d11: hooked swap chain class (vtable {}): Present{}, ResizeBuffers{}", module::describe(reinterpret_cast<uintptr_t>(vt)),
              h->present1.installed() ? ", Present1" : "", h->resize1.installed() ? ", ResizeBuffers1" : "");
    g_vt[n] = std::move(h);
    g_vtCount.store(n + 1, std::memory_order_release);
}

HRESULT STDMETHODCALLTYPE CreateSwapChainDetour(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    const HRESULT hr = g_createSwapChain.original<CreateSwapChainFn>()(f, dev, d, out);
    if (t_depth == 0 && d) {
        log::info("d3d11: IDXGIFactory::CreateSwapChain on thread {} '{}': {}x{} {}, {} buffers, swap effect {}, flags 0x{:X} -> 0x{:08X}",
                  GetCurrentThreadId(), ThreadName(GetCurrentThreadId()), d->BufferDesc.Width, d->BufferDesc.Height,
                  xr::DxgiFormatName(d->BufferDesc.Format), d->BufferCount, static_cast<int>(d->SwapEffect), d->Flags,
                  static_cast<uint32_t>(hr));
    }
    if (SUCCEEDED(hr) && out && *out) HookSwapchainClass(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndDetour(IDXGIFactory2* f, IUnknown* dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** out) {
    const HRESULT hr = g_createSwapChainForHwnd.original<CreateSwapChainForHwndFn>()(f, dev, wnd, d, fd, o, out);
    if (t_depth == 0 && d) {
        log::info("d3d11: IDXGIFactory2::CreateSwapChainForHwnd on thread {} '{}': {}x{} {}, {} buffers, swap effect {}, flags 0x{:X} -> 0x{:08X}",
                  GetCurrentThreadId(), ThreadName(GetCurrentThreadId()), d->Width, d->Height, xr::DxgiFormatName(d->Format),
                  d->BufferCount, static_cast<int>(d->SwapEffect), d->Flags, static_cast<uint32_t>(hr));
    }
    if (SUCCEEDED(hr) && out && *out) HookSwapchainClass(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateDeferredContextDetour(ID3D11Device* dev, UINT flags, ID3D11DeviceContext** out) {
    const HRESULT hr = g_createDeferred.original<CreateDeferredContextFn>()(dev, flags, out);
    const uint32_t n = ++g_deferredContexts;
    if (n <= 8 || (n & (n - 1)) == 0)
        log::info("d3d11: CreateDeferredContext #{} on thread {} '{}' -> 0x{:08X}", n, GetCurrentThreadId(), ThreadName(GetCurrentThreadId()),
                  static_cast<uint32_t>(hr));
    return hr;
}

std::string FeatureLevels(const D3D_FEATURE_LEVEL* fl, UINT n) {
    std::string s;
    for (UINT i = 0; i < n && fl; ++i) s += std::format("{}0x{:X}", s.empty() ? "" : ",", static_cast<int>(fl[i]));
    return s.empty() ? "default" : s;
}

HRESULT WINAPI D3D11CreateDeviceDetour(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl,
                                       UINT nfl, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    const HRESULT hr = g_createDevice.original<D3D11CreateDeviceFn>()(adapter, type, sw, flags, fl, nfl, sdk, dev, got, ctx);
    if (t_depth == 0 && type != D3D_DRIVER_TYPE_NULL) {
        g_deviceCreateTid = GetCurrentThreadId();
        log::info("d3d11: D3D11CreateDevice on thread {} '{}': adapter {}, driver type {}, flags 0x{:X}, feature levels {} -> 0x{:08X}, got 0x{:X}",
                  GetCurrentThreadId(), ThreadName(GetCurrentThreadId()), static_cast<void*>(adapter), static_cast<int>(type), flags,
                  FeatureLevels(fl, nfl), static_cast<uint32_t>(hr), got ? static_cast<int>(*got) : 0);
    }
    return hr;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChainDetour(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                                   const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk, const DXGI_SWAP_CHAIN_DESC* scd,
                                                   IDXGISwapChain** sc, ID3D11Device** dev, D3D_FEATURE_LEVEL* got,
                                                   ID3D11DeviceContext** ctx) {
    ++t_depth;  // the device creation inside is reported once, here
    const HRESULT hr = g_createDeviceAndSwapChain.original<D3D11CreateDeviceAndSwapChainFn>()(adapter, type, sw, flags, fl, nfl, sdk, scd,
                                                                                               sc, dev, got, ctx);
    --t_depth;
    if (t_depth == 0 && type != D3D_DRIVER_TYPE_NULL) {
        g_deviceCreateTid = GetCurrentThreadId();
        log::info("d3d11: D3D11CreateDeviceAndSwapChain on thread {} '{}': driver type {}, flags 0x{:X}, feature levels {} -> 0x{:08X}",
                  GetCurrentThreadId(), ThreadName(GetCurrentThreadId()), static_cast<int>(type), flags, FeatureLevels(fl, nfl),
                  static_cast<uint32_t>(hr));
    }
    if (SUCCEEDED(hr) && sc && *sc) HookSwapchainClass(*sc);
    return hr;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

}  // namespace

int64_t QpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double QpcToMs(int64_t ticks) { return double(ticks) * 1000.0 / double(g_qpcFreq); }

std::string ThreadName(DWORD tid) {
    if (tid == 0) return {};
    std::string name;
    if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
            name = log::narrow(desc);
            LocalFree(desc);
        }
        CloseHandle(h);
    }
    if (!name.empty()) return name;
    AcquireSRWLockShared(&g_namesLock);
    for (size_t i = 0; i < g_nameCount; ++i)
        if (g_names[i].tid == tid) name = g_names[i].name;
    ReleaseSRWLockShared(&g_namesLock);
    return name;
}

std::string ThreadReport() {
    struct T {
        DWORD tid;
        double cpuMs;
    };
    std::vector<T> threads;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                const auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
                threads.push_back(T{te.th32ThreadID, double(ft(k) + ft(u)) / 10000.0});
            }
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    std::sort(threads.begin(), threads.end(), [](const T& a, const T& b) { return a.cpuMs > b.cpuMs; });
    std::string out = std::format("{} threads; busiest:", threads.size());
    const DWORD present = g_presentTid.load(), device = g_deviceCreateTid.load();
    for (size_t i = 0; i < threads.size() && i < 10; ++i) {
        const T& t = threads[i];
        const std::string name = ThreadName(t.tid);
        out += std::format(" {}{}{}{} {:.0f}s;", t.tid, name.empty() ? "" : " '" + name + "'", t.tid == present ? " [presents]" : "",
                           t.tid == device ? " [created the device]" : "", t.cpuMs / 1000.0);
    }
    return out;
}

std::string D3D11Summary() {
    const DWORD tid = g_presentTid.load();
    const std::string name = ThreadName(tid);
    std::lock_guard lk(g_factsMutex);
    return g_factsLine + std::format(" present_tid {} ('{}') hooked_classes {} deferred_contexts {} resizes {}", tid, name, g_vtCount.load(),
                                     g_deferredContexts.load(), g_resizeCount.load());
}

bool InstallD3D11Hooks(const HookCallbacks& callbacks) {
    g_cb = callbacks;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    AddVectoredExceptionHandler(1, ThreadNameVeh);

    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) {
        log::error("render: d3d11.dll not loadable ({})", GetLastError());
        return false;
    }
    // Throwaway device and swap chain on a hidden window, to find the vtables.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ff7vr_d3d11_probe";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"ff7vr probe", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) {
        log::error("render: probe window creation failed ({})", GetLastError());
        return false;
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
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // every swap effect is implemented by the same DXGI class
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const auto create = reinterpret_cast<D3D11CreateDeviceAndSwapChainFn>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    HRESULT hr = E_FAIL;
    // The NULL driver needs no GPU and loads no display driver this early in the game's start-up.
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_NULL, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE}) {
        hr = create ? create(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx) : E_FAIL;
        if (SUCCEEDED(hr)) break;
    }
    bool ok = false;
    if (FAILED(hr)) {
        log::error("render: probe D3D11CreateDeviceAndSwapChain failed 0x{:08X}", static_cast<uint32_t>(hr));
    } else {
        HookSwapchainClass(sc.Get());
        ok = g_vtCount.load() > 0;
        // Factory methods, to see every swap chain the game creates (and hook a new class if one appears).
        ComPtr<IDXGIFactory2> factory;
        if (SUCCEEDED(sc->GetParent(IID_PPV_ARGS(&factory)))) {
            void** fvt = *reinterpret_cast<void***>(factory.Get());
            g_createSwapChain.create(fvt[kCreateSwapChain], &CreateSwapChainDetour);
            g_createSwapChainForHwnd.create(fvt[kCreateSwapChainForHwnd], &CreateSwapChainForHwndDetour);
        }
        void** dvt = *reinterpret_cast<void***>(dev.Get());
        g_deferredTarget = dvt[kCreateDeferredContext];
        g_createDeferred.create(g_deferredTarget, &CreateDeferredContextDetour);
    }
    ctx.Reset();
    sc.Reset();
    dev.Reset();
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (void* p = GetProcAddress(d3d11, "D3D11CreateDevice")) g_createDevice.create(p, &D3D11CreateDeviceDetour);
    if (void* p = GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")) g_createDeviceAndSwapChain.create(p, &D3D11CreateDeviceAndSwapChainDetour);
    log::info("render: D3D11 hooks installed ({})", ok ? "Present hooked" : "Present NOT hooked");
    return ok;
}

}  // namespace ff7vr::render
