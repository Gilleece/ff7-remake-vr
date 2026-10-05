#include "timing.h"

#include <algorithm>
#include <format>

namespace ff7vr::render {

Timing& GetTiming() {
    static Timing t;
    return t;
}

void Series::Add(double ms) {
    std::lock_guard lk(m_);
    if (samples_.size() < 1'000'000) samples_.push_back(ms);
}

size_t Series::Count() const {
    std::lock_guard lk(m_);
    return samples_.size();
}

std::string Series::Summarise(const char* name, std::vector<double> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * double(v.size() - 1) + 0.5))]; };
    return std::format("{}: avg {:.3f} p50 {:.3f} p95 {:.3f} p99 {:.3f} max {:.3f} ms (n {})", name, sum / double(v.size()), pct(0.5),
                       pct(0.95), pct(0.99), v.back(), v.size());
}

std::string Series::TakeSummary() {
    std::vector<double> v;
    {
        std::lock_guard lk(m_);
        v.swap(samples_);
        samples_.reserve(4096);
    }
    return Summarise(name_, std::move(v));
}

std::string Series::Peek() const {
    std::vector<double> v;
    {
        std::lock_guard lk(m_);
        v = samples_;
    }
    return Summarise(name_, std::move(v));
}

void GpuTimer::Reset() {
    for (auto& s : slots_) s = Slot{};
    device_ = nullptr;
    active_ = -1;
    next_ = 0;
}

void GpuTimer::Begin(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (device != device_) {
        Reset();
        device_ = device;
    }
    active_ = -1;
    Slot& s = slots_[next_ % slots_.size()];
    if (s.pending) return;  // results not read yet: skip this frame rather than stall
    if (!s.disjoint) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(device->CreateQuery(&d, &s.disjoint))) return;
        d.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(device->CreateQuery(&d, &s.t0)) || FAILED(device->CreateQuery(&d, &s.t1))) {
            s = Slot{};
            return;
        }
    }
    ctx->Begin(s.disjoint.Get());
    ctx->End(s.t0.Get());
    active_ = static_cast<int>(next_ % slots_.size());
}

void GpuTimer::End(ID3D11DeviceContext* ctx) {
    if (active_ < 0) return;
    Slot& s = slots_[active_];
    ctx->End(s.t1.Get());
    ctx->End(s.disjoint.Get());
    s.pending = true;
    active_ = -1;
    ++next_;
}

void GpuTimer::Collect(ID3D11DeviceContext* ctx, Series& out) {
    for (auto& s : slots_) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(s.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(s.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        s.pending = false;
        if (!dj.Disjoint && dj.Frequency && b >= a) out.Add(double(b - a) * 1000.0 / double(dj.Frequency));
    }
}

}  // namespace ff7vr::render
