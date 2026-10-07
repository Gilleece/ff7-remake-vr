#include "video_memory.h"

#include "ff7vr/core/log.h"

#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cwctype>
#include <filesystem>
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
// Busy time of this process's copy engines ("GPU Engine ... engtype_Copy", summed over its
// engines, so it can exceed 100 %): the uploads and copies the driver queues for the game.
PDH_HCOUNTER g_engineCounter = nullptr;
bool g_pdhTried = false;

// Warning state (LogReport callers only).
bool g_warned = false;
std::chrono::steady_clock::time_point g_lastWarn{};
double g_minSharedMb = -1;  // lowest shared-memory usage of the game seen since the device appeared
unsigned g_copyBusyBits = 0;  // the last kCopyWindow reports, newest in bit 0: copy engines busy
bool g_copyWarned = false;
std::chrono::steady_clock::time_point g_lastCopyWarn{};
bool g_startChecked = false;

constexpr double kMb = 1024.0 * 1024.0;
// Of DedicatedVideoMemory, all processes together. Measured on a 16 GB card: runs that kept
// their frame rate stayed at 78-80 %; every slow run sat at 95-99 % (Windows does not fill
// the card to 100 %; it starts moving memory out a few hundred MB before).
constexpr double kCardFullFraction = 0.94;
constexpr double kBudgetFraction = 0.95;  // of this process's budget
constexpr double kDemotedMb = 300.0;      // growth of the game's shared-memory usage that means allocations were moved out
constexpr double kDemotedCardFraction = 0.90;  // ... counted only with the card at least this full (a loading screen reached 412 MB at 75 %)
// The slow state after a quick restart (docs/benchmarking.md, "Video memory and slow phases"):
// the game's copy engines are mostly 50-130 % busy (summed, single 10-s reports from 0 to 360 %)
// while frames take 50-170 ms; in normal play they are at 2-6 %. Uploads after a load keep them
// busy for 20-40 s as well, so the warning needs 5 of the last 6 reports (a minute), and the
// all-clear 5 of the last 6 below the mark.
constexpr double kCopyBusyPercent = 40.0;
constexpr int kCopyWindow = 6;    // reports are 10 s apart
constexpr int kCopyBusyNeeded = 5;
// A session that starts this soon after the previous one ended can land in that state.
constexpr double kQuickRestartSeconds = 90.0;

void OpenPdh() {
    g_pdhTried = true;
    if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) {
        g_query = nullptr;
        return;
    }
    if (PdhAddEnglishCounterW(g_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cardCounter) != ERROR_SUCCESS) g_cardCounter = nullptr;
    if (PdhAddEnglishCounterW(g_query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &g_procSharedCounter) != ERROR_SUCCESS)
        g_procSharedCounter = nullptr;
    if (PdhAddEnglishCounterW(g_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_engineCounter) != ERROR_SUCCESS)
        g_engineCounter = nullptr;
    if ((!g_cardCounter && !g_procSharedCounter) || PdhCollectQueryData(g_query) != ERROR_SUCCESS) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
        g_cardCounter = g_procSharedCounter = g_engineCounter = nullptr;
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

// Sum in percent of the counter's instances whose lower-case name starts with `prefix` and
// contains `part`, or -1 when none has a value yet (a rate needs two collections).
double SumInstancesPercent(PDH_HCOUNTER counter, const std::wstring& prefix, const std::wstring& part) {
    if (!counter) return -1;
    DWORD bytes = 0, count = 0;
    const DWORD fmt = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    if (PdhGetFormattedCounterArrayW(counter, fmt, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return -1;
    std::vector<std::byte> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, fmt, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    double sum = -1;
    for (DWORD k = 0; k < count; ++k) {
        std::wstring name = items[k].szName ? items[k].szName : L"";
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name.rfind(prefix, 0) != 0 || name.find(part) == std::wstring::npos || items[k].FmtValue.CStatus != ERROR_SUCCESS) continue;
        sum = std::max(sum, 0.0) + items[k].FmtValue.doubleValue;
    }
    return sum;
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
    // Engine instances: "pid_1234_luid_0x00000000_0x0000ef10_phys_0_eng_4_engtype_copy".
    i.processCopyPercent = SumInstancesPercent(g_engineCounter, std::format(L"pid_{}_", GetCurrentProcessId()) + luid, L"engtype_copy");
}

// Last write time of the newest earlier session's log the mod kept in ff7vr-logs\ next to
// this DLL (log.cpp moves the previous ff7vr.log there at start and keeps its time stamp),
// as seconds before this process was created; -1 when there is none. The log is written at
// least every 10 s while the game runs, so its time is at most that long before the exit.
double SecondsSincePreviousSession() {
    namespace fs = std::filesystem;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&SecondsSincePreviousSession), &self))
        return -1;
    wchar_t path[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)))) return -1;
    std::error_code ec;
    const fs::path archive = fs::path(path).parent_path() / L"ff7vr-logs";
    fs::file_time_type newest{};
    bool found = false;
    for (const auto& e : fs::directory_iterator(archive, ec)) {
        const std::wstring n = e.path().filename().wstring();
        if (!e.is_regular_file(ec) || n.rfind(L"ff7vr-", 0) != 0 || n.size() < 4 || n.substr(n.size() - 4) != L".log" ||
            n.rfind(L"ff7vr-crash-", 0) == 0)
            continue;
        const auto t = e.last_write_time(ec);
        if (!ec && (!found || t > newest)) newest = t, found = true;
    }
    if (!found) return -1;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    // file_time_type on MSVC counts 100 ns ticks since 1601, like FILETIME.
    const long long start = (static_cast<long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const long long last = newest.time_since_epoch().count();
    return double(start - last) / 1e7;
}

// Once, when the device appears: a hint in the log when this session started soon after the
// previous one ended (only seen when the mod is installed by hand: the launcher and the dev
// tools move the logs away and wait before a quick restart themselves).
void CheckQuickRestart() {
    const double s = SecondsSincePreviousSession();
    if (s < 0) return;
    if (s < kQuickRestartSeconds) {
        log::warn("start: this session started {:.0f} s after the previous one's last log line. A game started again within about "
                  "a minute and a half of quitting can be slow for minutes (about 10 frames per second from the load on, the "
                  "graphics card busy at low power); the 'game copy engine' figure in the 'video memory:' lines then stays high. "
                  "If that happens, quit, wait a minute and a half, start again",
                  s);
    } else {
        log::info("start: the previous session's last log line was {:.0f} s before this start", s);
    }
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
        if (!g_startChecked) {
            g_startChecked = true;
            CheckQuickRestart();
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
    std::string copy = i.processCopyPercent >= 0 ? std::format("; game copy engine {:.0f} %", i.processCopyPercent) : std::string();
    return std::format("video memory: game {:.0f} of budget {:.0f} MB ({:.0f} %); {}; game in system memory {:.0f} MB{}", i.localUsageMb,
                       i.localBudgetMb, i.localBudgetMb > 0 ? 100.0 * i.localUsageMb / i.localBudgetMb : 0.0, card, SharedMb(i), copy);
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

    // The copy-bound slow state: the game's copy engines busy report after report while the
    // card is not full (when it is full, the warning above explains the slowness).
    const bool copyBusy = i.processCopyPercent >= kCopyBusyPercent && !cardFull;
    g_copyBusyBits = ((g_copyBusyBits << 1) | (copyBusy ? 1u : 0u)) & ((1u << kCopyWindow) - 1);
    const int busyReports = std::popcount(g_copyBusyBits);
    if (busyReports >= kCopyBusyNeeded && (!g_copyWarned || now - g_lastCopyWarn >= std::chrono::minutes(5))) {
        log::warn("video memory: the game's copy engines were busy in {} of the last {} reports ({:.0f} % now) with the card {:.0f} % full. This is the "
                  "slow state that can follow starting the game again soon after quitting it: frames take 50-170 ms although "
                  "nothing else is wrong, for minutes. Quit, wait a minute and a half, and start again",
                  busyReports, kCopyWindow, i.processCopyPercent, i.dedicatedMb > 0 ? 100.0 * i.cardUsageMb / i.dedicatedMb : 0.0);
        g_copyWarned = true;
        g_lastCopyWarn = now;
    } else if (g_copyWarned && busyReports <= kCopyWindow - kCopyBusyNeeded) {
        log::info("video memory: the game's copy engines are no longer busy ({:.0f} %)", i.processCopyPercent);
        g_copyWarned = false;
    }
}

}  // namespace ff7vr::render::video_memory