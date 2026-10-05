#include "gpu_timer.h"

namespace ff7vr::xr {

void GpuCopyTimer::Reset() {
    for (auto& s : slots_) s = Slot{};
    active_ = -1;
    next_ = 0;
    device_ = nullptr;
    std::lock_guard lk(m_);
    results_.clear();
}

void GpuCopyTimer::BeginFrame(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (device != device_) {
        Reset();
        device_ = device;
    }
    Collect(ctx);
    active_ = -1;
    // Next slot whose results are back; when all are still pending this frame is not timed.
    uint32_t pick = next_;
    while (slots_[pick % slots_.size()].pending && pick - next_ < slots_.size()) ++pick;
    if (pick - next_ == slots_.size()) return;
    next_ = pick;
    Slot& s = slots_[next_ % slots_.size()];
    if (!s.disjoint) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(device->CreateQuery(&d, &s.disjoint))) return;
        d.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& q : s.ts)
            if (FAILED(device->CreateQuery(&d, &q))) {
                s = Slot{};
                return;
            }
    }
    s.used = 0;
    ctx->Begin(s.disjoint.Get());
    active_ = static_cast<int>(next_ % slots_.size());
}

void GpuCopyTimer::Before(ID3D11DeviceContext* ctx) {
    if (active_ < 0) return;
    Slot& s = slots_[active_];
    if (s.used >= kPairs) return;
    ctx->End(s.ts[s.used * 2].Get());
}

void GpuCopyTimer::After(ID3D11DeviceContext* ctx) {
    if (active_ < 0) return;
    Slot& s = slots_[active_];
    if (s.used >= kPairs) return;
    ctx->End(s.ts[s.used * 2 + 1].Get());
    ++s.used;
}

void GpuCopyTimer::EndFrame(ID3D11DeviceContext* ctx) {
    if (active_ < 0) return;
    Slot& s = slots_[active_];
    ctx->End(s.disjoint.Get());
    s.pending = true;
    active_ = -1;
    ++next_;
}

void GpuCopyTimer::Collect(ID3D11DeviceContext* ctx) {
    for (auto& s : slots_) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        double total = 0;
        bool ready = true;
        for (int i = 0; i < s.used && ready; ++i) {
            UINT64 a = 0, b = 0;
            if (ctx->GetData(s.ts[i * 2].Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(s.ts[i * 2 + 1].Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                ready = false;
                break;
            }
            if (b >= a && dj.Frequency) total += double(b - a) * 1000.0 / double(dj.Frequency);
        }
        if (!ready) continue;
        s.pending = false;
        if (!dj.Disjoint && s.used > 0) {
            std::lock_guard lk(m_);
            if (results_.size() < 100000) results_.push_back(static_cast<float>(total));
        }
    }
}

std::vector<float> GpuCopyTimer::Take() {
    std::lock_guard lk(m_);
    std::vector<float> r;
    r.swap(results_);
    return r;
}

}  // namespace ff7vr::xr
