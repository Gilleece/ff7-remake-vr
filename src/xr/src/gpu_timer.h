// GPU time of the library's own copies (D3D11 timestamp queries), summed per
// frame and read back a few frames later without stalling.
#pragma once

#include "xr_common.h"

#include <array>
#include <mutex>
#include <vector>

namespace ff7vr::xr {

class GpuCopyTimer {
public:
    // RT. Call BeginFrame/EndFrame around a frame's work and Before/After around each copy.
    void BeginFrame(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void Before(ID3D11DeviceContext* ctx);
    void After(ID3D11DeviceContext* ctx);
    void EndFrame(ID3D11DeviceContext* ctx);
    void Reset();
    // Any thread: per-frame totals (ms) collected so far.
    std::vector<float> Take();

private:
    static constexpr int kPairs = 6;  // copies per frame we can time
    struct Slot {
        ComPtr<ID3D11Query> disjoint;
        std::array<ComPtr<ID3D11Query>, kPairs * 2> ts;
        int used = 0;
        bool pending = false;
    };
    void Collect(ID3D11DeviceContext* ctx);

    std::array<Slot, 6> slots_{};
    int active_ = -1;
    uint32_t next_ = 0;
    ID3D11Device* device_ = nullptr;
    std::mutex m_;
    std::vector<float> results_;
};

}  // namespace ff7vr::xr
