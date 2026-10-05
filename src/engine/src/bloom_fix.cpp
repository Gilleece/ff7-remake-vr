#include "bloom_fix.h"

#include "gpu_trace.h"
#include "rhi_command.h"
#include "stereo_device.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <format>

namespace ff7vr::engine::bloom_fix {
namespace {

// Pass object (FRenderingCompositePass subclass): +0xA8 level (int32), +0xAC "first level"
// (bool). Pass context: +0x00 the view (FViewInfo*), +0x28 the RHI command list. View:
// +0x70 the rectangle the first level reads (int32 MinX, MinY, MaxX, MaxY).
constexpr std::size_t kPassFirstLevel = 0xac;
constexpr std::size_t kContextView = 0x00;
constexpr std::size_t kContextCmdList = 0x28;
constexpr std::size_t kViewSourceRect = 0x70;

using ProcessFn = void(__fastcall*)(void* pass, void* context);
hook::InlineHook g_hook;
std::atomic<bool> g_enabled{false};

struct Counters {
    std::atomic<std::uint64_t> passes{0}, queued{0}, queue_failed{0}, applied{0}, missed{0};
};
Counters g_count;

// RHI command carrying the rectangle to the RHI thread. Static ring; the RHI thread is at
// most a frame behind, one command per frame.
struct Command {
    rhi::Command base;
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    std::atomic<bool> pending{false};
};
Command g_cmds[8];
unsigned g_next = 0;  // render thread

// RHI thread state.
struct Armed {
    bool active = false;
    std::int32_t x = 0, y = 0, w = 0, h = 0;
};
Armed g_armed;
ID3D11Texture2D* g_scratch = nullptr;
ID3D11ShaderResourceView* g_scratch_srv = nullptr;
D3D11_TEXTURE2D_DESC g_scratch_desc{};
D3D11_SHADER_RESOURCE_VIEW_DESC g_scratch_srv_desc{};

void execute(void*, rhi::Command* self) {
    auto* c = reinterpret_cast<Command*>(self);
    g_armed = Armed{true, c->x, c->y, c->w, c->h};
    c->pending.store(false, std::memory_order_release);
}

bool read_rect(const std::uint8_t* view, std::int32_t r[4]) {
    __try {
        std::memcpy(r, view + kViewSourceRect, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_pass(void* pass, void* context, bool* first, std::uint8_t** view, void** cmd_list) {
    __try {
        *first = *(static_cast<const std::uint8_t*>(pass) + kPassFirstLevel) != 0;
        *view = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(context) + kContextView);
        *cmd_list = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(context) + kContextCmdList);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall process_detour(void* pass, void* context) {
    ++g_count.passes;
    bool first = false;
    std::uint8_t* view = nullptr;
    void* cmd_list = nullptr;
    std::int32_t r[4]{};
    if (g_enabled.load(std::memory_order_relaxed) && device::active() && read_pass(pass, context, &first, &view, &cmd_list) && first &&
        view && read_rect(view, r) && (r[0] != 0 || r[1] != 0) && r[2] > r[0] && r[3] > r[1]) {
        Command& c = g_cmds[g_next];
        if (!c.pending.load(std::memory_order_acquire)) {
            c.base.execute = &execute;
            c.x = r[0];
            c.y = r[1];
            c.w = r[2] - r[0];
            c.h = r[3] - r[1];
            c.pending.store(true, std::memory_order_release);
            if (rhi::enqueue(cmd_list, &c.base)) {
                ++g_count.queued;
                g_next = (g_next + 1) % (sizeof(g_cmds) / sizeof(g_cmds[0]));
            } else {
                c.pending.store(false, std::memory_order_release);
                ++g_count.queue_failed;
            }
        } else {
            ++g_count.queue_failed;
        }
    }
    g_hook.original<ProcessFn>()(pass, context);
}

void release_scratch() {
    if (g_scratch_srv) g_scratch_srv->Release();
    if (g_scratch) g_scratch->Release();
    g_scratch_srv = nullptr;
    g_scratch = nullptr;
}

// RHI thread: a scratch texture like the input with one mip, and a view of it like the
// engine's view of the input.
bool ensure_scratch(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src, const D3D11_SHADER_RESOURCE_VIEW_DESC& view) {
    if (g_scratch && g_scratch_desc.Width == src.Width && g_scratch_desc.Height == src.Height && g_scratch_desc.Format == src.Format &&
        g_scratch_srv_desc.Format == view.Format)
        return true;
    release_scratch();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = src.Width;
    d.Height = src.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = src.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_scratch)) || !g_scratch) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = view.Format;
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MostDetailedMip = 0;
    v.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_scratch, &v, &g_scratch_srv)) || !g_scratch_srv) {
        release_scratch();
        return false;
    }
    g_scratch_desc = d;
    g_scratch_srv_desc = v;
    log::info("bloom fix: scratch texture {}x{} format {}", d.Width, d.Height, static_cast<int>(d.Format));
    return true;
}

bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    if (!g_armed.active) return false;
    const Armed a = g_armed;
    g_armed.active = false;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) {
        ++g_count.missed;
        return false;
    }
    bool done = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    D3D11_RESOURCE_DIMENSION dim{};
    if (res) res->GetType(&dim);
    if (res && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td{};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        srv->GetDesc(&vd);
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        const bool fits = td.SampleDesc.Count == 1 && a.x >= 0 && a.y >= 0 && static_cast<UINT>(a.x + a.w) <= td.Width &&
                          static_cast<UINT>(a.y + a.h) <= td.Height && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        if (dev && fits && ensure_scratch(dev, td, vd)) {
            const D3D11_BOX box{static_cast<UINT>(a.x), static_cast<UINT>(a.y), 0, static_cast<UINT>(a.x + a.w), static_cast<UINT>(a.y + a.h), 1};
            ctx->CopySubresourceRegion(g_scratch, 0, 0, 0, 0, res, D3D11CalcSubresource(vd.Texture2D.MostDetailedMip, 0, td.MipLevels), &box);
            ctx->PSSetShaderResources(0, 1, &g_scratch_srv);
            original(ctx, count, start, base);
            ctx->PSSetShaderResources(0, 1, &srv);
            done = true;
            ++g_count.applied;
        }
        if (dev) dev->Release();
    }
    if (!done) ++g_count.missed;
    if (res) res->Release();
    srv->Release();
    return done;
}

}  // namespace

bool init(std::uintptr_t reduce_process, bool enabled) {
    if (!reduce_process) {
        log::warn("bloom fix: reduce pass not found; the right eye keeps the left eye's bloom");
        return false;
    }
    if (!g_hook.create(reinterpret_cast<void*>(reduce_process), &process_detour)) return false;
    gpu_trace::set_draw_indexed_override(&on_draw_indexed);
    g_enabled = enabled;
    log::info("bloom fix: hook on the bloom reduce pass installed ({})", enabled ? "on" : "off ([stereo] bloom_fix = 0)");
    return true;
}

void set_enabled(bool on) { g_enabled = on && g_hook.installed(); }
bool enabled() { return g_enabled.load(); }

void frame(ID3D11Texture2D* any_texture) {
    // A rectangle that no draw took this frame (hooks not in place yet) is not carried over.
    g_armed.active = false;
    if (g_enabled.load(std::memory_order_relaxed)) gpu_trace::install_context_hooks(any_texture);
}

std::string status() {
    return std::format("bloom fix {}: reduce passes {} queued {} queue failures {} applied {} missed {}", g_enabled.load() ? "on" : "off",
                       g_count.passes.load(), g_count.queued.load(), g_count.queue_failed.load(), g_count.applied.load(),
                       g_count.missed.load());
}

}  // namespace ff7vr::engine::bloom_fix
