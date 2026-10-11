#include "upload_check.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ff7vr::render::upload_check {
namespace {

using Clock = std::chrono::steady_clock;

constexpr UINT kBytes = 256 * 1024;
constexpr auto kEvery = std::chrono::seconds(10);
constexpr double kSlowGBps = 0.5;  // normal: 3-14 GB/s (2-3 next to a busy game); the slow state: 0.02-0.3
constexpr int kSlowWindows = 2;

struct State {
    std::atomic<bool> enabled{true};
    // Present thread only
    ID3D11Device* device = nullptr;
    ComPtr<ID3D11Buffer> staging, target;
    ComPtr<ID3D11Query> disjoint, t0, t1;
    bool pending = false;
    Clock::time_point next{};
    Clock::time_point issued{};
    bool failed = false;
    // shared with the XR thread and commands
    std::mutex mutex;
    std::vector<double> window;  // GB/s of the checks finished since the last window
    double latest = -1;
    double latestMs = -1;
    uint64_t checks = 0, slowChecks = 0, lostChecks = 0;
    std::atomic<int> requested{0};
    // XR thread only
    int slowRun = 0;
    bool warned = false;
};
State* g = new State;

bool Create(ID3D11Device* dev) {
    g->staging.Reset();
    g->target.Reset();
    g->disjoint.Reset();
    g->t0.Reset();
    g->t1.Reset();
    g->pending = false;
    std::vector<uint8_t> fill(kBytes);
    for (size_t i = 0; i < fill.size(); ++i) fill[i] = static_cast<uint8_t>(i * 131u);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kBytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA init{fill.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &init, &g->staging))) return false;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
    bd.BindFlags = 0;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g->target))) return false;
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(dev->CreateQuery(&qd, &g->disjoint))) return false;
    qd.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(dev->CreateQuery(&qd, &g->t0)) || FAILED(dev->CreateQuery(&qd, &g->t1))) return false;
    g->device = dev;
    return true;
}

}  // namespace

void Configure(const Config& cfg) {
    g->enabled = cfg.get_bool("log", "upload_check", true);
    if (!g->enabled) log::info("upload check: off ([log] upload_check = 0)");
}

void Frame(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (!g->enabled.load(std::memory_order_relaxed) || !device || !ctx || g->failed) return;
    const Clock::time_point now = Clock::now();
    if (g->device != device) {
        if (!Create(device)) {
            g->failed = true;
            log::warn("upload check: could not create its buffers or queries; off for this session");
            return;
        }
        g->next = now + kEvery;
    }
    if (g->pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (ctx->GetData(g->disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(g->t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(g->t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            g->pending = false;
            std::lock_guard lk(g->mutex);
            if (!dj.Disjoint && dj.Frequency && b > a) {
                const double ms = double(b - a) * 1000.0 / double(dj.Frequency);
                const double gbps = double(kBytes) / (ms / 1000.0) / 1e9;
                g->window.push_back(gbps);
                g->latest = gbps;
                g->latestMs = ms;
                ++g->checks;
                if (gbps < kSlowGBps) ++g->slowChecks;
            } else {
                ++g->lostChecks;
            }
        } else if (now - g->issued > std::chrono::seconds(5)) {
            g->pending = false;  // never finished (device removed, queries reset): try again later
            std::lock_guard lk(g->mutex);
            ++g->lostChecks;
        }
        return;
    }
    const bool asked = g->requested.load(std::memory_order_relaxed) > 0;
    if (!asked && now < g->next) return;
    if (asked) g->requested.fetch_sub(1);
    g->next = now + kEvery;
    ctx->Begin(g->disjoint.Get());
    ctx->End(g->t0.Get());
    ctx->CopyResource(g->target.Get(), g->staging.Get());
    ctx->End(g->t1.Get());
    ctx->End(g->disjoint.Get());
    g->pending = true;
    g->issued = now;
}

std::string TakeWindowLine() {
    if (!g->enabled.load(std::memory_order_relaxed)) return {};
    std::vector<double> w;
    {
        std::lock_guard lk(g->mutex);
        w.swap(g->window);
    }
    if (w.empty()) return {};
    const double worst = *std::min_element(w.begin(), w.end());
    if (worst < kSlowGBps) {
        if (++g->slowRun >= kSlowWindows && !g->warned) {
            g->warned = true;
            log::warn("upload check: the graphics card reads system memory at {:.2f} GB/s (normally 3-14 GB/s) in {} checks in a row. This is "
                      "the slow state of the card's PCIe transfers: frames slow down several times over and stay slow; it outlasts the game "
                      "and ends with the next start of the game. Quit the game, wait a minute and start it again",
                      worst, g->slowRun);
        }
    } else {
        if (g->warned) log::info("upload check: uploads to the graphics card are fast again ({:.2f} GB/s)", worst);
        g->slowRun = 0;
        g->warned = false;
    }
    return std::format("upload check: {} KB from system memory to the card at {:.2f} GB/s{}", kBytes / 1024, worst,
                       w.size() > 1 ? std::format(" (slowest of {})", w.size()) : std::string());
}

double LatestGBps() {
    std::lock_guard lk(g->mutex);
    return g->latest;
}

std::string Command(const std::string& args) {
    if (args == "now") {
        if (!g->enabled) return "err off ([log] upload_check = 0)";
        g->requested.fetch_add(1);
        return "ok a check starts at the next Present";
    }
    if (args == "status" || args.empty()) {
        std::lock_guard lk(g->mutex);
        if (!g->enabled) return "ok off ([log] upload_check = 0)";
        return std::format("ok latest {:.3f} GB/s ({:.3f} ms for {} KB); checks {}, slow {}, lost {}", g->latest, g->latestMs, kBytes / 1024,
                           g->checks, g->slowChecks, g->lostChecks);
    }
    return "err upload status | now";
}

}  // namespace ff7vr::render::upload_check
