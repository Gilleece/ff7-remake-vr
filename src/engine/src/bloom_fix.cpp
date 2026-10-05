#include "bloom_fix.h"

#if FF7VR_ENGINE_WITH_DLSS
#include "dlss.h"
#endif
#include "fixes.h"
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

// Scratch textures (RHI thread): the size and format of the input they stand in for. Two
// slots, so the bloom input and the ambient occlusion input do not evict each other when
// their formats differ.
struct Scratch {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    std::uint64_t last_use = 0;
};
Scratch g_scratch[2];
std::uint64_t g_use_clock = 0;

// Ambient occlusion fix (RHI thread): the last full-screen draw's render target and viewport.
std::atomic<bool> g_ao_enabled{false};
struct LastFullscreen {
    ID3D11Resource* target = nullptr;  // compared only, no reference held
    D3D11_VIEWPORT vp{};
};
LastFullscreen g_last;
struct AoCounters {
    std::atomic<std::uint64_t> applied{0}, failed{0};
};
AoCounters g_ao;

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

void release_scratch(Scratch& s) {
    if (s.srv) s.srv->Release();
    if (s.tex) s.tex->Release();
    s = Scratch{};
}

// RHI thread: a scratch texture like the input with one mip, and a view of it like the
// engine's view of the input.
Scratch* ensure_scratch(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src, const D3D11_SHADER_RESOURCE_VIEW_DESC& view) {
    ++g_use_clock;
    for (Scratch& s : g_scratch) {
        if (s.tex && s.desc.Width == src.Width && s.desc.Height == src.Height && s.desc.Format == src.Format && s.view.Format == view.Format) {
            s.last_use = g_use_clock;
            return &s;
        }
    }
    Scratch& s = g_scratch[0].last_use <= g_scratch[1].last_use ? g_scratch[0] : g_scratch[1];
    release_scratch(s);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = src.Width;
    d.Height = src.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = src.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &s.tex)) || !s.tex) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = view.Format;
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MostDetailedMip = 0;
    v.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(s.tex, &v, &s.srv)) || !s.srv) {
        release_scratch(s);
        return nullptr;
    }
    s.desc = d;
    s.view = v;
    s.last_use = g_use_clock;
    log::info("post-process fix: scratch texture {}x{} format {}", d.Width, d.Height, static_cast<int>(d.Format));
    return &s;
}

// Runs the draw with shader resource 0 replaced by a scratch copy whose origin holds the
// rectangle (x, y, w, h) of the bound input. False if the input does not fit.
bool draw_with_shifted_input(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, std::int32_t x, std::int32_t y, std::int32_t w,
                             std::int32_t h, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
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
        const bool fits = td.SampleDesc.Count == 1 && x >= 0 && y >= 0 && w > 0 && h > 0 && static_cast<UINT>(x + w) <= td.Width &&
                          static_cast<UINT>(y + h) <= td.Height && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        Scratch* s = (dev && fits) ? ensure_scratch(dev, td, vd) : nullptr;
        if (s) {
            const D3D11_BOX box{static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x + w), static_cast<UINT>(y + h), 1};
            ctx->CopySubresourceRegion(s->tex, 0, 0, 0, 0, res, D3D11CalcSubresource(vd.Texture2D.MostDetailedMip, 0, td.MipLevels), &box);
            ctx->PSSetShaderResources(0, 1, &s->srv);
            original(ctx, count, start, base);
            ctx->PSSetShaderResources(0, 1, &srv);
            done = true;
        }
        if (dev) dev->Release();
    }
    if (res) res->Release();
    return done;
}

// Square Enix's ambient occlusion has the same fault as the bloom: each view's full-size
// setup pass writes at the view's rectangle, and the next pass, a half-size pass at the origin
// of its own target, reads that setup texture relative to the origin, so the right view's
// occlusion was computed from the left view's setup. Recognised on the RHI thread by its
// shape: a full-screen draw at the origin whose input (shader resource 0) is the target the
// previous full-screen draw wrote at a rectangle that does not start at the origin, and whose
// viewport is at most half as wide. Stock passes keep a view at its own rectangle in every
// intermediate target, so they never draw at the origin for the right view.
bool ao_fix(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* target = nullptr;
    if (rtv) {
        rtv->GetResource(&target);
        rtv->Release();
    }
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    const LastFullscreen prev = g_last;
    g_last = LastFullscreen{target, vp};
    if (target) target->Release();  // the pointer is only compared
    if (!target || nvp == 0 || !prev.target || prev.target == target) return false;
    const bool prev_offset = prev.vp.TopLeftX >= 1.0f && prev.vp.TopLeftY == 0.0f && prev.vp.Width >= 2.0f;
    const bool at_origin = vp.TopLeftX == 0.0f && vp.TopLeftY == 0.0f && vp.Width * 2.0f <= prev.vp.Width + 2.0f;
    if (!prev_offset || !at_origin) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11Resource* input = nullptr;
    srv->GetResource(&input);
    bool done = false;
    if (input == prev.target) {
        done = draw_with_shifted_input(ctx, srv, static_cast<std::int32_t>(prev.vp.TopLeftX), 0, static_cast<std::int32_t>(prev.vp.Width),
                                       static_cast<std::int32_t>(prev.vp.Height), count, start, base, original);
        ++(done ? g_ao.applied : g_ao.failed);
    }
    if (input) input->Release();
    srv->Release();
    return done;
}

bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
#if FF7VR_ENGINE_WITH_DLSS
    if (!g_armed.active && dlss::on_draw_indexed(ctx, count, start, base, original)) return true;
#endif
    if (!g_armed.active) {
        // Full-screen passes are one triangle.
        if (count == 3 && device::active()) {
            if (g_ao_enabled.load(std::memory_order_relaxed) && ao_fix(ctx, count, start, base, original)) return true;
            return fixes::ssr_draw(ctx, count, start, base, original);
        }
        return false;
    }
    const Armed a = g_armed;
    g_armed.active = false;
    g_last = LastFullscreen{};
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) {
        ++g_count.missed;
        return false;
    }
    const bool done = draw_with_shifted_input(ctx, srv, a.x, a.y, a.w, a.h, count, start, base, original);
    ++(done ? g_count.applied : g_count.missed);
    srv->Release();
    return done;
}

}  // namespace

bool init(std::uintptr_t reduce_process, bool enabled, bool ao_enabled) {
    gpu_trace::set_draw_indexed_override(&on_draw_indexed);
    g_ao_enabled = ao_enabled;
    log::info("ambient occlusion fix: {}", ao_enabled ? "on" : "off ([stereo] ao_fix = 0)");
    if (!reduce_process) {
        log::warn("bloom fix: reduce pass not found; the right eye keeps the left eye's bloom");
        return false;
    }
    if (!g_hook.create(reinterpret_cast<void*>(reduce_process), &process_detour)) return false;
    g_enabled = enabled;
    log::info("bloom fix: hook on the bloom reduce pass installed ({})", enabled ? "on" : "off ([stereo] bloom_fix = 0)");
    return true;
}

void set_enabled(bool on) { g_enabled = on && g_hook.installed(); }
bool enabled() { return g_enabled.load(); }
void set_ao_enabled(bool on) { g_ao_enabled = on; }
bool ao_enabled() { return g_ao_enabled.load(); }

void frame(ID3D11Texture2D* any_texture) {
    // A rectangle that no draw took this frame (hooks not in place yet) is not carried over.
    g_armed.active = false;
    g_last = LastFullscreen{};
    fixes::ssr_frame();
    bool hooks = g_enabled.load(std::memory_order_relaxed) || g_ao_enabled.load(std::memory_order_relaxed) || fixes::ssr_per_eye();
#if FF7VR_ENGINE_WITH_DLSS
    dlss::frame(any_texture);
    hooks = hooks || dlss::wants_hooks();
#endif
    if (hooks) gpu_trace::install_context_hooks(any_texture);
}

std::string status() {
    return std::format("bloom fix {}: reduce passes {} queued {} queue failures {} applied {} missed {}", g_enabled.load() ? "on" : "off",
                       g_count.passes.load(), g_count.queued.load(), g_count.queue_failed.load(), g_count.applied.load(),
                       g_count.missed.load());
}

std::string ao_status() {
    return std::format("ambient occlusion fix {}: applied {} failed {}", g_ao_enabled.load() ? "on" : "off", g_ao.applied.load(),
                       g_ao.failed.load());
}

}  // namespace ff7vr::engine::bloom_fix
