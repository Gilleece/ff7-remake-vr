// Video memory of the game process as the operating system's video memory
// manager sees it (IDXGIAdapter3::QueryVideoMemoryInfo): the budget it grants
// this process in the card's own memory (local segment) and in shared system
// memory (non-local segment), and what the process uses of each.
//
// The budget is per process and does not shrink fast enough to show other
// programs' use of the card, so the line also has the whole card's usage (the
// "GPU Adapter Memory" performance counter, all processes). When the card is
// full the manager moves some of the game's allocations to system memory: the
// game's shared-memory usage grows by hundreds of MB, the GPU reads those
// resources over PCIe, and frames slow down many times over while the GPU
// draws little power. The timing block logs one line per report and a warning
// when that state is reached.
#pragma once

#include <d3d11.h>

#include <string>

namespace ff7vr::render::video_memory {

struct Info {
    bool valid = false;
    double localBudgetMb = 0, localUsageMb = 0, localReservedMb = 0;
    double nonLocalBudgetMb = 0, nonLocalUsageMb = 0;
    double dedicatedMb = 0;  // DXGI_ADAPTER_DESC::DedicatedVideoMemory
    double cardUsageMb = -1;      // all processes on this adapter; -1 when the counter is not available
    double processSharedMb = -1;  // this process in shared system memory, including allocations moved out of the card; -1 if unknown
};

// Queries the adapter of `device` (the adapter is looked up once per device). Thread-safe.
Info Query(ID3D11Device* device);

// "video memory: game X of budget Y MB (Z %); card ...; game in system memory N MB";
// "" when the query failed.
std::string Line(const Info& i);

// Logs the timing-block line, and a warning (at most once a minute) when the card
// is 94 % full, the game is at 95 % of its budget, or the game's shared-memory
// usage has grown by 300 MB or more over its lowest value while the card is at
// least 90 % full.
void LogReport(ID3D11Device* device);

}  // namespace ff7vr::render::video_memory
