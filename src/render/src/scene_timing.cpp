#include "scene_timing.h"

#include "timing.h"

#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <format>

namespace ff7vr::render::scene_timing {
namespace {

using Microsoft::WRL::ComPtr;

// One frame's measurement: the disjoint query around it, timestamps at the scene's start (t0),
// the scene's end (t1) and the frame's Present (t2).
struct Slot {
    ComPtr<ID3D11Query> disjoint, t[3];
    bool pending = false;  // issued, not read back yet
    bool haveEnd = false;  // the scene's end was marked (otherwise t1 = t2 and there is no "after")
};

// Everything below except the atomics and the series: presenting thread only.
std::array<Slot, 8> g_slots;
ID3D11Device* g_device = nullptr;  // the device the queries belong to
ID3D11DeviceContext* g_ctx = nullptr;
int g_active = -1;  // slot of the frame being measured
enum class Phase { None, Scene, After } g_phase = Phase::None;
uint32_t g_next = 0;

std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_frames{0}, g_skipped{0}, g_disjoint{0};
std::atomic<float> g_gpuFrameMs{0.0f};          // the newest frame: scene start to Present
std::atomic<uint64_t> g_gpuFrameSamples{0};

Series g_scene{"gpu scene"};
Series g_after{"gpu after the scene until Present"};

void ResetSlots() {
    for (Slot& s : g_slots) s = Slot{};
    g_active = -1;
    g_phase = Phase::None;
    g_next = 0;
}

// Closes the open measurement at this point of the command stream.
void Close() {
    if (g_active < 0 || !g_ctx) return;
    Slot& s = g_slots[static_cast<size_t>(g_active)];
    if (g_phase == Phase::Scene) {
        g_ctx->End(s.t[1].Get());
        s.haveEnd = false;
    }
    g_ctx->End(s.t[2].Get());
    g_ctx->End(s.disjoint.Get());
    s.pending = true;
    g_active = -1;
    g_phase = Phase::None;
    ++g_next;
}

void Collect() {
    if (!g_ctx) return;
    for (Slot& s : g_slots) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 v[3]{};
        if (g_ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        bool ready = true;
        for (int i = 0; i < 3 && ready; ++i) ready = g_ctx->GetData(s.t[i].Get(), &v[i], sizeof(v[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ready) continue;
        s.pending = false;
        if (dj.Disjoint || !dj.Frequency || v[1] < v[0] || v[2] < v[1]) {
            ++g_disjoint;
            continue;
        }
        const double toMs = 1000.0 / double(dj.Frequency);
        g_scene.Add(double(v[1] - v[0]) * toMs);
        if (s.haveEnd) {
            g_after.Add(double(v[2] - v[1]) * toMs);
            g_gpuFrameMs.store(static_cast<float>(double(v[2] - v[0]) * toMs), std::memory_order_relaxed);
            g_gpuFrameSamples.fetch_add(1, std::memory_order_release);
        }
        ++g_frames;
    }
}

}  // namespace

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

void OnPresent(const PresentInfo& p) {
    if (p.device != g_device) {
        // A new device (or the first Present): the queries of the old one are dropped unread.
        ResetSlots();
        g_device = p.device;
    }
    g_ctx = p.context;
    Close();
    Collect();
}

void SceneBegin() {
    if (!g_ctx || !g_device || !g_enabled.load(std::memory_order_relaxed)) return;
    Close();  // a second scene before the Present: the first one's "after" ends here
    // The next slot whose results were read; when all are pending, this frame goes unmeasured
    // rather than waiting for the GPU.
    uint32_t pick = g_next;
    while (g_slots[pick % g_slots.size()].pending && pick - g_next < g_slots.size()) ++pick;
    if (pick - g_next == g_slots.size()) {
        ++g_skipped;
        return;
    }
    g_next = pick;
    const size_t i = g_next % g_slots.size();
    Slot& s = g_slots[i];
    if (!s.disjoint) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(g_device->CreateQuery(&d, &s.disjoint))) return;
        d.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& q : s.t)
            if (FAILED(g_device->CreateQuery(&d, &q))) {
                s = Slot{};
                return;
            }
    }
    g_ctx->Begin(s.disjoint.Get());
    g_ctx->End(s.t[0].Get());
    s.haveEnd = false;
    g_active = static_cast<int>(i);
    g_phase = Phase::Scene;
}

void SceneEnd() {
    if (g_active < 0 || g_phase != Phase::Scene || !g_ctx) return;
    Slot& s = g_slots[static_cast<size_t>(g_active)];
    g_ctx->End(s.t[1].Get());
    s.haveEnd = true;
    g_phase = Phase::After;
}

void PeekMedians(double* sceneMs, size_t* sceneSamples, double* afterMs, size_t* afterSamples) {
    *sceneMs = g_scene.Median(sceneSamples);
    *afterMs = g_after.Median(afterSamples);
}

bool LastTimes(double* sceneMs, double* afterMs) {
    uint64_t n = 0;
    const double scene = g_scene.Last(&n);
    if (n == 0) return false;
    if (sceneMs) *sceneMs = scene;
    if (afterMs) *afterMs = g_after.Last();
    return true;
}

std::vector<std::string> TakeTimingLines() {
    std::vector<std::string> out;
    for (Series* s : {&g_scene, &g_after}) {
        std::string l = s->TakeSummary();
        if (!l.empty()) out.push_back(std::move(l));
    }
    return out;
}

std::string Command(const std::string& args) {
    if (args == "on" || args == "off") {
        g_enabled = args == "on";
        log::info("gpu timing: scene timers {} (dev command)", args);
    } else if (!args.empty() && args != "status") {
        return "err usage: gputime status | on | off";
    }
    return std::format("ok scene timers {}; frames measured {}, skipped (all slots pending) {}, disjoint or invalid {}; period so far: {} | {}",
                       g_enabled.load() ? "on" : "off", g_frames.load(), g_skipped.load(), g_disjoint.load(), g_scene.Peek(), g_after.Peek());
}

}  // namespace ff7vr::render::scene_timing

namespace ff7vr::render {

bool GetGpuFrameTime(float* ms, uint64_t* samples) {
    const uint64_t n = scene_timing::g_gpuFrameSamples.load(std::memory_order_acquire);
    if (samples) *samples = n;
    if (ms) *ms = scene_timing::g_gpuFrameMs.load(std::memory_order_relaxed);
    return n > 0;
}

}  // namespace ff7vr::render
