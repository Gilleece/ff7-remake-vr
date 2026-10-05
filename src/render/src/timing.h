// Frame timing of the render module: what the mod costs per frame.
//
// Every value is a duration in milliseconds measured with
// QueryPerformanceCounter on the thread that does the work (CPU), or with
// D3D11 timestamp queries (GPU). Samples are collected per reporting period
// and summarised as avg / p50 / p95 / p99 / max; nothing is averaged across
// periods. See docs/render.md for what each series covers.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ff7vr::render {

class Series {
public:
    explicit Series(const char* name) : name_(name) { samples_.reserve(4096); }
    void Add(double ms);
    // "name avg A p50 B p95 C p99 D max E ms (n N)" and resets; "" if no samples.
    std::string TakeSummary();
    // Same summary without resetting.
    std::string Peek() const;
    size_t Count() const;
    // The most recent sample and the number of samples added so far (not reset by TakeSummary).
    double Last(uint64_t* added = nullptr) const;

private:
    static std::string Summarise(const char* name, std::vector<double> v);
    const char* name_;
    mutable std::mutex m_;
    std::vector<double> samples_;
    double last_ = 0;
    uint64_t added_ = 0;
};

struct Timing {
    Series frameInterval{"game frame interval"};   // between consecutive Presents of the main swap chain
    Series presentCall{"IDXGISwapChain::Present"};  // the real Present (driver, vsync, compositor)
    Series hook{"present hook"};                    // our whole Present detour before the real Present
    Series submit{"xr submit"};                     // IXrBackend::SubmitFrame (begin, copies, end frame)
    Series wait{"xr frame wait"};                   // IXrBackend::WaitFrame (xrWaitFrame), on whatever thread calls it
    Series gpuCopy{"gpu copy"};                     // GPU time of the copies into the XR swapchains (timestamps around each copy)
    Series gpu{"gpu submit total"};                 // GPU time between timestamps around the whole submission (copies + what the runtime adds)
};
Timing& GetTiming();

// D3D11 timestamp queries around our own GPU work on the immediate context.
// Results are read a few frames later without stalling.
class GpuTimer {
public:
    void Begin(ID3D11Device* device, ID3D11DeviceContext* ctx);  // starts a measurement if a slot is free
    void End(ID3D11DeviceContext* ctx);
    void Collect(ID3D11DeviceContext* ctx, Series& out);         // non-blocking
    void Reset();                                                // device changed

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, t0, t1;
        bool pending = false;
    };
    std::array<Slot, 6> slots_{};
    ID3D11Device* device_ = nullptr;
    int active_ = -1;
    uint32_t next_ = 0;
};

}  // namespace ff7vr::render
