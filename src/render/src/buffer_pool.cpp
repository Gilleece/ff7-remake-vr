#include "buffer_pool.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <psapi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>

namespace ff7vr::render::buffer_pool {
namespace {

constexpr UINT kPooledBytes = 128;  // the light volumes' vertex buffers (8 float4)
constexpr size_t kRing = 256;
constexpr int kCreateBufferSlot = 3;  // ID3D11Device::CreateBuffer (d3d11.h order)

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);

struct State {
    std::atomic<bool> enabled{true};
    std::atomic<bool> paused{false};  // `bufpool off`: hand out new buffers again (the hook stays)
    std::atomic<bool> installed{false};
    std::atomic<ID3D11Device*> device{nullptr};
    int stereoWindows = 0;  // XR thread only
    hook::VTableHook hook;
    CreateBufferFn original = nullptr;
    uintptr_t exeBase = 0, exeEnd = 0;
    std::mutex mutex;
    ID3D11Device* ringDevice = nullptr;
    std::array<ID3D11Buffer*, kRing> ring{};  // one reference each, held for the process's life
    size_t next = 0;
    std::atomic<uint64_t> served{0}, created{0}, passed{0};
    uint64_t servedTaken = 0, createdTaken = 0;  // XR thread only
};
State* g = new State;  // never freed: the hook and the buffers live until the process ends

bool Pooled(const D3D11_BUFFER_DESC* d, const D3D11_SUBRESOURCE_DATA* init, uintptr_t ret) {
    return d && !init && d->ByteWidth == kPooledBytes && d->Usage == D3D11_USAGE_DYNAMIC && d->BindFlags == D3D11_BIND_VERTEX_BUFFER &&
           d->CPUAccessFlags == D3D11_CPU_ACCESS_WRITE && d->MiscFlags == 0 && ret >= g->exeBase && ret < g->exeEnd;
}

HRESULT STDMETHODCALLTYPE CreateBufferDetour(ID3D11Device* dev, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
                                             ID3D11Buffer** out) {
    const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!out || g->paused.load(std::memory_order_relaxed) || !Pooled(desc, init, ret)) {
        g->passed.fetch_add(1, std::memory_order_relaxed);
        return g->original(dev, desc, init, out);
    }
    std::lock_guard lk(g->mutex);
    if (g->ringDevice != dev) {
        for (ID3D11Buffer*& b : g->ring) {
            if (b) b->Release();
            b = nullptr;
        }
        g->ringDevice = dev;
        g->next = 0;
    }
    ID3D11Buffer*& slot = g->ring[g->next];
    g->next = (g->next + 1) % kRing;
    if (!slot) {
        const HRESULT hr = g->original(dev, desc, nullptr, &slot);
        if (FAILED(hr) || !slot) {
            slot = nullptr;
            return g->original(dev, desc, init, out);
        }
        g->created.fetch_add(1, std::memory_order_relaxed);
    }
    slot->AddRef();  // the caller's reference; the ring keeps its own
    *out = slot;
    g->served.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

void Install(ID3D11Device* device) {
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi))) {
        g->exeBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        g->exeEnd = g->exeBase + mi.SizeOfImage;
    }
    void** vt = *reinterpret_cast<void***>(device);
    if (g->exeBase && g->hook.create(vt, kCreateBufferSlot, reinterpret_cast<void*>(&CreateBufferDetour))) {
        g->original = g->hook.original<CreateBufferFn>();
        device->AddRef();  // the vtable stays in use for the rest of the process
        g->installed = true;
        log::info("buffer pool: the game's {}-byte dynamic vertex buffers come from a ring of {} ([stereo] light_volume_pool = 1)",
                  kPooledBytes, kRing);
    } else {
        g->enabled = false;
        log::warn("buffer pool: the CreateBuffer hook could not be installed; off for this session");
    }
}

}  // namespace

void Configure(const Config& cfg) {
    g->enabled = cfg.get_bool("stereo", "light_volume_pool", true);
    if (!g->enabled) log::info("buffer pool: off ([stereo] light_volume_pool = 0)");
}

void NoteDevice(ID3D11Device* device) {
    if (g->enabled.load(std::memory_order_relaxed) && device) g->device.store(device, std::memory_order_release);
}

std::string TakeWindowLine(bool stereo) {
    if (!g->enabled.load(std::memory_order_relaxed)) return {};
    if (!g->installed.load(std::memory_order_acquire)) {
        ID3D11Device* d = g->device.load(std::memory_order_acquire);
        g->stereoWindows = stereo ? g->stereoWindows + 1 : 0;
        if (d && g->stereoWindows >= 2) Install(d);
        return {};
    }
    const uint64_t served = g->served.load(), created = g->created.load();
    const uint64_t s = served - g->servedTaken, c = created - g->createdTaken;
    g->servedTaken = served;
    g->createdTaken = created;
    if (!s && !c) return {};
    return std::format("buffer pool: {} light volume buffers served from the ring ({} new){}", s, c, g->paused ? " (paused)" : "");
}

std::string Command(const std::string& args) {
    if (args == "off" || args == "on") {
        if (!g->installed) return "err not installed";
        g->paused = args == "off";
        return std::format("ok buffer pool {}", args);
    }
    if (args == "status" || args.empty())
        return std::format("ok {}{}; served {}, ring buffers created {}, other buffers passed on {}",
                           g->installed ? "installed" : (g->enabled ? "waiting for stereo" : "off"), g->paused ? " (paused)" : "",
                           g->served.load(), g->created.load(), g->passed.load());
    return "err bufpool status | on | off";
}

}  // namespace ff7vr::render::buffer_pool
