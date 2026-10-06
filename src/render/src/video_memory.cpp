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
// The game's own share in system memory comes from "GPU Process Memory ... Shared Usage":
// unlike DXGI's non-local usage it includes allocations the memory manager moved out of the
// card (measured: 400-680 MB in the counter against 77-143 MB from DXGI in a slow run).
PDH_HQUERY g_query = nullptr;
PDH_HCOUNTER g_cardCounter = nullptr;
PDH_HCOUNTER g_procSharedCounter = nullptr;
bool g_pdhTried = false;

// Warning state (LogReport callers only).
bool g_warned = false;
std::chrono::steady_clock::time_point g_lastWarn{};
double g_minSharedMb = -1;  // lowest shared-memory usage of the game seen since the device appeared

constexpr double kMb = 1024.0 * 1024.0;
// Of DedicatedVideoMemory, all processes together. Measured on a 16 GB card: runs that kept
// their frame rate stayed at 78-80 %; every slow run sat at 95-99 % (Windows does not fill
// the card to 100 %; it starts moving memory out a few hundred MB before).
constexpr double kCardFullFraction = 0.94;
constexpr double kBudgetFraction = 0.95;  // of this process's budget
constexpr double kDemotedMb = 300.0;      // growth of the game's shared-memory usage that means allocations were moved out
constexpr double kDemotedCardFraction = 0.90;  // ... counted only with the card at least this full (a loading screen reached 412 MB at 75 %)

void OpenPdh() {
    g_pdhTried = true;
    if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) {
        g_query = nullptr;
        return;
    }
    if (PdhAddEnglishCounterW(g_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cardCounter) != ERROR_SUCCESS) g_cardCounter = nullptr;
    if (PdhAddEnglishCounterW(g_query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &g_procSharedCounter) != ERROR_SUCCESS)
        g_procSharedCounter = nullptr;
    if ((!g_cardCounter && !g_procSharedCounter) || PdhCollectQueryData(g_query) != ERROR_SUCCESS) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
        g_cardCounter = g_procSharedCounter = nullptr;
        log::info("video memory: the whole card's usage is not available (performance counters 'GPU Adapter Memory' missing)");
    }
}

// Sum in MB of the counter's instances whose lower-case name starts with `prefix`, or -1.
double SumInstancesMb(PDH_HCOUNTER counter, const std::wstring& prefix) {
    if (!counter) return -1;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return -1;
    std::vector<std::byte> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    double mb = -1;
    for (DWORD k = 0; k < count; ++k) {
        std::wstring name = items[k].szName ? items[k].szName : L"";
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name.rfind(prefix, 0) != 0 || items[k].FmtValue.CStatus != ERROR_SUCCESS) continue;
        mb = std::max(mb, 0.0) + double(items[k].FmtValue.largeValue) / kMb;
    }
    return mb;
}

// Fills the counter-based fields of `i`. Caller holds g_m.
void ReadCounters(Info& i) {
    if (!g_pdhTried) OpenPdh();
    if (!g_query || PdhCollectQueryData(g_query) != ERROR_SUCCESS) return;
    // Instance names: "luid_0x00000000_0x0000ef10_phys_0" (adapter, high part then low part)
    // and "pid_1234_luid_0x00000000_0x0000ef10_phys_0" (process on that adapter).
    const std::wstring luid = std::format(L"luid_0x{:08x}_0x{:08x}_", static_cast<uint32_t>(g_luid.HighPart), g_luid.LowPart);
    i.cardUsageMb = SumInstancesMb(g_cardCounter, luid);
    i.processSharedMb = SumInstancesMb(g_procSharedCounter, std::format(L"pid_{}_", GetCurrentProcessId()) + luid);
}

// The game's memory in system memory: the counter when available (it includes what was moved
// out of the card), otherwise DXGI's non-local usage.
double SharedMb(const Info& i) { return i.processSharedMb >= 0 ? i.processSharedMb : i.nonLocalUsageMb; }

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
        g_minSharedMb = -1;
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
    ReadCounters(i);
    return i;
}

std::string Line(const Info& i) {
    if (!i.valid) return {};
    std::string card = i.cardUsageMb >= 0 ? std::format("card {:.0f} of {:.0f} MB in use by all processes ({:.0f} %)", i.cardUsageMb, i.dedicatedMb,
                                                        i.dedicatedMb > 0 ? 100.0 * i.cardUsageMb / i.dedicatedMb : 0.0)
                                          : std::format("card {:.0f} MB", i.dedicatedMb);
    return std::format("video memory: game {:.0f} of budget {:.0f} MB ({:.0f} %); {}; game in system memory {:.0f} MB", i.localUsageMb,
                       i.localBudgetMb, i.localBudgetMb > 0 ? 100.0 * i.localUsageMb / i.localBudgetMb : 0.0, card, SharedMb(i));
}

void LogReport(ID3D11Device* device) {
    const Info i = Query(device);
    const std::string l = Line(i);
    if (l.empty()) return;
    log::info("timing:   {}", l);
    const double shared = SharedMb(i);
    double base = 0;
    {
        std::lock_guard lk(g_m);
        if (g_minSharedMb < 0 || shared < g_minSharedMb) g_minSharedMb = shared;
        base = g_minSharedMb;
    }
    const bool cardFull = i.cardUsageMb >= 0 && i.dedicatedMb > 0 && i.cardUsageMb >= kCardFullFraction * i.dedicatedMb;
    const bool overBudget = i.localBudgetMb > 0 && i.localUsageMb >= kBudgetFraction * i.localBudgetMb;
    // Uploads during loading also raise it for a while; it means moved-out memory only when the card is nearly full too.
    const bool cardNearlyFull = i.cardUsageMb >= 0 && i.dedicatedMb > 0 && i.cardUsageMb >= kDemotedCardFraction * i.dedicatedMb;
    const bool demoted = cardNearlyFull && shared - base >= kDemotedMb;
    const bool bad = cardFull || overBudget || demoted;
    const auto now = std::chrono::steady_clock::now();
    if (bad && (!g_warned || now - g_lastWarn >= std::chrono::seconds(60))) {
        std::string why = cardFull ? std::format("the card's memory is full ({:.0f} of {:.0f} MB, all programs together)", i.cardUsageMb, i.dedicatedMb)
                          : overBudget ? std::format("the game uses {:.0f} of the {:.0f} MB Windows grants it", i.localUsageMb, i.localBudgetMb)
                                       : std::string("");
        if (demoted)
            why += std::format("{}{:.0f} MB of the game's memory sits in system memory (lowest so far {:.0f} MB)", why.empty() ? "" : "; ", shared, base);
        log::warn("video memory: {}. Windows moves the game's memory between the card and system memory and frames slow down many times "
                  "over; lower the per-eye resolution or the DLSS input_scale, or close other programs that use the graphics card",
                  why);
        g_lastWarn = now;
        g_warned = true;
    } else if (!bad && g_warned) {
        log::info("video memory: back within the card");
        g_warned = false;
    }
}

}  // namespace ff7vr::render::video_memory