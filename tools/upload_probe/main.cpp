// upload_probe: how fast does the graphics card move data right now?
//
// A small D3D11 program with a device of its own. Each test prints one line; every GPU wait has
// a deadline, so the program cannot hang on a card that has stopped moving data.
//
//   upload_probe [--mb N] [--timeout-ms T] [--rounds R] [test ...]
//
// Tests (default: info up vram down):
//   info          the adapter, its video memory budget and current usage for a new process
//   up            copy N MB from a staging buffer in system memory to a buffer on the card
//                 (the card reads system memory over PCIe; this is what texture uploads do)
//   down          copy N MB from the card to a staging buffer and map it (the card writes over PCIe)
//   vram          copy N MB between two buffers on the card (the card's own memory only)
//   alloc:G       create G GB of buffers on the card, write each once, release them all
//   present:S     open a small window and present to it for S seconds (a flip-model swapchain)
//
// Exit code: 0 all tests finished, 3 a test hit its deadline, 1 no device or a creation failed.
// Example: upload_probe --mb 256 info up vram down

#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using Clock = std::chrono::steady_clock;

struct Ctx {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIAdapter3> adapter;
    UINT bytes = 64u << 20;
    int timeout_ms = 3000;
    bool timed_out = false;
};

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Polls a query until it is done or the deadline passes. Returns false on the deadline.
template <typename T>
bool wait_query(Ctx& c, ID3D11Query* q, T* out, Clock::time_point deadline) {
    c.ctx->Flush();
    for (;;) {
        const HRESULT hr = c.ctx->GetData(q, out, sizeof(T), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) return true;
        if (FAILED(hr)) return false;
        if (Clock::now() >= deadline) return false;
        Sleep(0);
    }
}

// Runs body() between two timestamps and prints the GPU and wall time and the rate.
template <typename F>
bool timed(Ctx& c, const char* name, double bytes, F&& body) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ComPtr<ID3D11Query> dj, t0, t1, ev;
    c.dev->CreateQuery(&qd, &dj);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    c.dev->CreateQuery(&qd, &t0);
    c.dev->CreateQuery(&qd, &t1);
    qd.Query = D3D11_QUERY_EVENT;
    c.dev->CreateQuery(&qd, &ev);
    const auto w0 = Clock::now();
    const auto deadline = w0 + std::chrono::milliseconds(c.timeout_ms);
    c.ctx->Begin(dj.Get());
    c.ctx->End(t0.Get());
    body();
    c.ctx->End(t1.Get());
    c.ctx->End(dj.Get());
    c.ctx->End(ev.Get());
    BOOL done = FALSE;
    if (!wait_query(c, ev.Get(), &done, deadline)) {
        std::printf("%s %u MB: TIMEOUT after %d ms\n", name, static_cast<unsigned>(bytes / (1 << 20)), c.timeout_ms);
        c.timed_out = true;
        return false;
    }
    const double wall = ms_since(w0);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
    UINT64 a = 0, b = 0;
    double gpu = -1.0;
    if (wait_query(c, dj.Get(), &dd, deadline) && wait_query(c, t0.Get(), &a, deadline) &&
        wait_query(c, t1.Get(), &b, deadline) && !dd.Disjoint && dd.Frequency && b > a) {
        gpu = static_cast<double>(b - a) * 1000.0 / static_cast<double>(dd.Frequency);
    }
    std::printf("%s %u MB: gpu %.2f ms (%.2f GB/s), wall %.2f ms (%.2f GB/s)\n", name,
                static_cast<unsigned>(bytes / (1 << 20)), gpu, gpu > 0 ? bytes / gpu / 1e6 : 0.0, wall,
                bytes / wall / 1e6);
    return true;
}

ComPtr<ID3D11Buffer> make_buffer(Ctx& c, UINT bytes, D3D11_USAGE usage, UINT cpu, const void* init = nullptr) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = usage;
    bd.CPUAccessFlags = cpu;
    if (usage == D3D11_USAGE_DEFAULT) {
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    }
    D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
    ComPtr<ID3D11Buffer> b;
    if (FAILED(c.dev->CreateBuffer(&bd, init ? &sd : nullptr, &b))) return nullptr;
    return b;
}

void test_info(Ctx& c) {
    DXGI_ADAPTER_DESC2 d{};
    c.adapter->GetDesc2(&d);
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    c.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    c.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    std::printf("info: %s, dedicated %llu MB; budget local %llu MB (usage %llu MB), non-local %llu MB (usage %llu MB)\n",
                name, static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20),
                static_cast<unsigned long long>(local.Budget >> 20), static_cast<unsigned long long>(local.CurrentUsage >> 20),
                static_cast<unsigned long long>(nonlocal.Budget >> 20),
                static_cast<unsigned long long>(nonlocal.CurrentUsage >> 20));
}

bool test_up(Ctx& c) {
    std::vector<char> fill(c.bytes, 7);
    auto stg = make_buffer(c, c.bytes, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ, fill.data());
    auto dst = make_buffer(c, c.bytes, D3D11_USAGE_DEFAULT, 0);
    if (!stg || !dst) { std::printf("up: buffer creation failed\n"); return false; }
    return timed(c, "up", c.bytes, [&] { c.ctx->CopyResource(dst.Get(), stg.Get()); });
}

bool test_down(Ctx& c) {
    auto src = make_buffer(c, c.bytes, D3D11_USAGE_DEFAULT, 0);
    auto stg = make_buffer(c, c.bytes, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!src || !stg) { std::printf("down: buffer creation failed\n"); return false; }
    return timed(c, "down", c.bytes, [&] { c.ctx->CopyResource(stg.Get(), src.Get()); });
}

bool test_vram(Ctx& c) {
    std::vector<char> fill(c.bytes, 5);
    auto a = make_buffer(c, c.bytes, D3D11_USAGE_DEFAULT, 0, fill.data());
    auto b = make_buffer(c, c.bytes, D3D11_USAGE_DEFAULT, 0);
    if (!a || !b) { std::printf("vram: buffer creation failed\n"); return false; }
    // One copy first so that both buffers are resident before the timed copy.
    c.ctx->CopyResource(b.Get(), a.Get());
    return timed(c, "vram", c.bytes, [&] { c.ctx->CopyResource(b.Get(), a.Get()); });
}

bool test_alloc(Ctx& c, double gb) {
    const UINT chunk = 256u << 20;
    const int n = static_cast<int>(gb * 4.0 + 0.5);
    auto src = make_buffer(c, chunk, D3D11_USAGE_DEFAULT, 0);
    if (!src) { std::printf("alloc: buffer creation failed\n"); return false; }
    std::vector<ComPtr<ID3D11Buffer>> bufs;
    const auto w0 = Clock::now();
    for (int i = 0; i < n; ++i) {
        auto b = make_buffer(c, chunk, D3D11_USAGE_DEFAULT, 0);
        if (!b) break;
        c.ctx->CopyResource(b.Get(), src.Get());
        bufs.push_back(b);
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> ev;
    c.dev->CreateQuery(&qd, &ev);
    c.ctx->End(ev.Get());
    BOOL done = FALSE;
    const bool ok = wait_query(c, ev.Get(), &done, Clock::now() + std::chrono::milliseconds(c.timeout_ms * 4));
    const double made = ms_since(w0);
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    c.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    const size_t made_n = bufs.size();
    bufs.clear();
    c.ctx->Flush();
    std::printf("alloc: %zu of %d buffers of 256 MB written in %.0f ms%s; usage then %llu MB of a %llu MB budget; released\n",
                made_n, n, made, ok ? "" : " (TIMEOUT)", static_cast<unsigned long long>(local.CurrentUsage >> 20),
                static_cast<unsigned long long>(local.Budget >> 20));
    if (!ok) c.timed_out = true;
    return ok;
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

bool test_present(Ctx& c, double seconds) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"upload_probe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"upload_probe", L"upload_probe", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!hwnd) { std::printf("present: no window\n"); return false; }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ComPtr<IDXGIFactory2> f;
    c.adapter->GetParent(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 640;
    sd.Height = 360;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    if (!f || FAILED(f->CreateSwapChainForHwnd(c.dev.Get(), hwnd, &sd, nullptr, nullptr, &sc))) {
        std::printf("present: no swapchain\n");
        DestroyWindow(hwnd);
        return false;
    }
    const auto w0 = Clock::now();
    int frames = 0;
    double worst = 0;
    while (ms_since(w0) < seconds * 1000.0) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        ComPtr<ID3D11Texture2D> bb;
        sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        ComPtr<ID3D11RenderTargetView> rtv;
        c.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
        const float col[4] = {0.1f * static_cast<float>(frames % 10), 0.2f, 0.3f, 1.0f};
        c.ctx->ClearRenderTargetView(rtv.Get(), col);
        const auto p0 = Clock::now();
        sc->Present(1, 0);
        const double pm = ms_since(p0);
        if (pm > worst) worst = pm;
        ++frames;
    }
    std::printf("present: %d frames in %.1f s, slowest Present %.1f ms\n", frames, ms_since(w0) / 1000.0, worst);
    sc.Reset();
    c.ctx->ClearState();
    c.ctx->Flush();
    DestroyWindow(hwnd);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Ctx c;
    int rounds = 1;
    std::vector<std::string> tests;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--mb" && i + 1 < argc) c.bytes = static_cast<UINT>(std::atoi(argv[++i])) << 20;
        else if (a == "--timeout-ms" && i + 1 < argc) c.timeout_ms = std::atoi(argv[++i]);
        else if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
        else tests.push_back(a);
    }
    if (tests.empty()) tests = {"info", "up", "vram", "down"};

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<IDXGIAdapter1> a1;
    if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a1)))) return 1;
    a1.As(&c.adapter);
    const auto d0 = Clock::now();
    if (FAILED(D3D11CreateDevice(a1.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &c.dev, nullptr, &c.ctx))) {
        std::printf("no device\n");
        return 1;
    }
    std::printf("device created in %.1f ms\n", ms_since(d0));

    for (int r = 0; r < rounds; ++r) {
        for (const auto& t : tests) {
            if (t == "info") test_info(c);
            else if (t == "up") test_up(c);
            else if (t == "down") test_down(c);
            else if (t == "vram") test_vram(c);
            else if (t.rfind("alloc:", 0) == 0) test_alloc(c, std::atof(t.c_str() + 6));
            else if (t.rfind("present:", 0) == 0) test_present(c, std::atof(t.c_str() + 8));
            else std::printf("unknown test '%s'\n", t.c_str());
            if (c.timed_out) break;
        }
        if (c.timed_out) break;
    }
    // A timed-out copy may still be queued: leave without waiting for the device's teardown.
    if (c.timed_out) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return 0;
}
