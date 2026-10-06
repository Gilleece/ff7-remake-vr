#include "video_memory.h"

#include "ff7vr/core/log.h"

#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <format>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ff7vr::render::video_memory {
namespace {

std::mutex g_m;
ID3D11Device* g_device = nullptr;  // identity only, not a reference
ComPtr<IDXGIAdapter3> g_adapter;
double g_dedicatedMb = 0;
LUID g_luid{};
bool g_lookedUp = false;

// Whole-card usage of all processes: the "GPU Adapter Memory" performance counter
// (the one Task Manager shows), read through PDH. No elevation needed.
PDH_HQUERY g_query = nullptr;
PDH_HCOUNTER g_counter = nullptr;
bool g_pdhTried = false;

// Warning state (LogReport callers only).
bool g_warned = false;
std::chrono::steady_clock::time_point g_lastWarn{};
double g_minNonLocalMb = -1;  // lowest shared-memory usage seen since the device appeared

constexpr double kMb = 1024.0 * 1024.0;
constexpr double kCardFullFraction = 0.97;  // of DedicatedVideoMemory, all processes together
constexpr double kBudgetFraction = 0.95;    // of this process's budget
constexpr double kDemotedMb = 384.0;        // shared-memory growth that means allocations were moved out of the card

void OpenPdh() {
    g_pdhTried = true;
    if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) {
        g_query = nullptr;
        return;
    }
    if (PdhAddEnglishCounterW(g_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_counter) != ERROR_SUCCESS ||
        PdhCollectQueryData(g_query) != ERROR_SUCCESS) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
        g_counter = nullptr;
        log::info("video memory: the whole card's usage is not available (performance counter 'GPU Adapter Memory' missing)");
    }
}

// MB of the card in use by all processes, or -1. Caller holds g_m.
double CardUsageMb() {
    if (!g_pdhTried) OpenPdh();
    if (!g_query || PdhCollectQueryData(g_query) != ERROR_SUCCESS) return -1;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_LARGE, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return -1;
    std::vector<std::byte> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_LARGE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    // Instance names look like "luid_0x00000000_0x0000EF10_phys_0" (high part, low part).
    std::wstring want = std::format(L"luid_0x{:08x}_0x{:08x}_", static_cast<uint32_t>(g_luid.HighPart), g_luid.LowPart);
    double mb = -1;
    for (DWORD k = 0; k < count; ++k) {
        std::wstring name = items[k].szName ? items[k].szName : L"";
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name.rfind(want, 0) != 0 || items[k].FmtValue.CStatus != ERROR_SUCCESS) continue;
        mb = std::max(mb, 0.0) + double(items[k].FmtValue.largeValue) / kMb;
    }
    return mb;
}

}  // namespace

Info Query(ID3D11Device* device) {
    Info i;
    if (!device) return i;
    std::lock_guard lk(g_m);
    if (device != g_device || !g_lookedUp) {
        g_device = device;
        g_lookedUp = true;
        g_adapter.Reset();
        g_dedicatedMb = 0;
        g_minNonLocalMb = -1;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter.As(&g_adapter))) {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(adapter->GetDesc(&d))) {
                g_dedicatedMb = double(d.DedicatedVideoMemory) / kMb;
                g_luid = d.AdapterLuid;
                log::info("video memory: adapter '{}', {:.0f} MB dedicated, {:.0f} MB shared system memory", log::narrow(d.Description),
                          g_dedicatedMb, double(d.SharedSystemMemory) / kMb);
            }
        } else {
            log::warn("video memory: IDXGIAdapter3 not available; no budget reports");
        }
    }
    if (!g_adapter) return i;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
    if (FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return i;
    if (FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) return i;
    i.valid = true;
    i.localBudgetMb = double(local.Budget) / kMb;
    i.localUsageMb = double(local.CurrentUsage) / kMb;
    i.localReservedMb = double(local.CurrentReservation) / kMb;
    i.nonLocalBudgetMb = double(nonLocal.Budget) / kMb;
    i.nonLocalUsageMb = double(nonLocal.CurrentUsage) / kMb;
    i.dedicatedMb = g_dedicatedMb;
    i.cardUsageMb = CardUsageMb();
    return i;
}

std::string Line(const Info& i) {
    if (!i.valid) return {};
    std::string card = i.cardUsageMb >= 0 ? std::format("card {:.0f} of {:.0f} MB in use by all processes ({:.0f} %)", i.cardUsageMb, i.dedicatedMb,
                                                        i.dedicatedMb > 0 ? 100.0 * i.cardUsageMb / i.dedicatedMb : 0.0)
                                          : std::format("card {:.0f} MB", i.dedicatedMb);
    return std::format("video memory: game {:.0f} of budget {:.0f} MB ({:.0f} %); {}; game in shared system memory {:.0f} MB", i.localUsageMb,
                       i.localBudgetMb, i.localBudgetMb > 0 ? 100.0 * i.localUsageMb / i.localBudgetMb : 0.0, card, i.nonLocalUsageMb);
}

void LogReport(ID3D11Device* device) {
    const Info i = Query(device);
    const std::string l = Line(i);
    if (l.empty()) return;
    log::info("timing:   {}", l);
    double base = 0;
    {
        std::lock_guard lk(g_m);
        if (g_minNonLocalMb < 0 || i.nonLocalUsageMb < g_minNonLocalMb) g_minNonLocalMb = i.nonLocalUsageMb;
        base = g_minNonLocalMb;
    }
    const bool cardFull = i.cardUsageMb >= 0 && i.dedicatedMb > 0 && i.cardUsageMb >= kCardFullFraction * i.dedicatedMb;
    const bool overBudget = i.localBudgetMb > 0 && i.localUsageMb >= kBudgetFraction * i.localBudgetMb;
    const bool demoted = i.nonLocalUsageMb - base >= kDemotedMb;
    const bool bad = cardFull || overBudget || demoted;
    const auto now = std::chrono::steady_clock::now();
    if (bad && (!g_warned || now - g_lastWarn >= std::chrono::seconds(60))) {
        std::string why = cardFull ? std::format("the card's memory is full ({:.0f} of {:.0f} MB, all programs together)", i.cardUsageMb, i.dedicatedMb)
                          : overBudget ? std::format("the game uses {:.0f} of the {:.0f} MB Windows grants it", i.localUsageMb, i.localBudgetMb)
                                       : std::string("");
        if (demoted)
            why += std::format("{}{:.0f} MB of the game's memory now sits in system memory (was {:.0f} MB)", why.empty() ? "" : "; ",
                               i.nonLocalUsageMb, base);
        log::warn("video memory: {}. Windows moves memory between the card and system memory now and frames slow down many times over; "
                  "lower the per-eye resolution (or DLSS input_scale) or close other programs that use the graphics card",
                  why);
        g_lastWarn = now;
        g_warned = true;
    } else if (!bad && g_warned) {
        log::info("video memory: back within the card");
        g_warned = false;
    }
}

}  // namespace ff7vr::render::video_memory
