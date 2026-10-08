#include "distortion_fix.h"

#include "rhi_command.h"
#include "stereo_device.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace ff7vr::engine::distortion_fix {
namespace {

// The composite function: rcx the RHI command list, rdx the scene render targets, r8 the
// view (FViewInfo*), r9 the blurred distortion target. View: +0x70 the rectangle the
// post-processing passes read the view from (int32 MinX, MinY, MaxX, MaxY), +0xA0 the view
// rectangle. Extra stack arguments are passed through unchanged.
constexpr std::size_t kViewSourceRect = 0x70;
constexpr std::size_t kViewRect = 0xa0;

using CompositeFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*);
hook::InlineHook g_hook;
std::atomic<bool> g_enabled{false};

struct Counters {
    std::atomic<std::uint64_t> composites{0}, queued{0}, queue_failed{0}, applied{0}, shifted{0}, full_view{0}, mismatch{0},
        rect_differs{0}, failed{0};
};
Counters g_count;

// RHI command carrying the view's rectangle to the RHI thread (static ring; at most two
// composites per frame and the RHI thread at most a frame behind).
struct Command {
    rhi::Command base;
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    bool apply = false;
    std::atomic<bool> pending{false};
};
Command g_cmds[8];
unsigned g_next = 0;  // render thread

struct Armed {
    bool active = false;
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    bool apply = false;  // false: the draw is only measured (fix off)
};
Armed g_armed;  // RHI thread

// The replacement DrawRectangle constants (RHI thread).
ID3D11Buffer* g_cb = nullptr;
UINT g_cb_size = 0;

// Pixel shader invocations of the composite per eye (pipeline statistics queries), the
// proof that each view's composite covers its own eye and only that.
struct Probe {
    ID3D11Query* query = nullptr;
    bool pending = false;
};
Probe g_probe[2];
std::atomic<std::uint64_t> g_ps_invocations[2]{};
std::atomic<std::int64_t> g_eye_pixels[2]{};
bool g_logged_first = false;
std::atomic<bool> g_opaque{false};

// Runs the draw, without blending in the opaque test.
void draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4]{};
    UINT mask = 0;
    const bool opaque = g_opaque.load(std::memory_order_relaxed);
    if (opaque) {
        ctx->OMGetBlendState(&bs, factor, &mask);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }
    original(ctx, count, start, base);
    if (opaque) {
        ctx->OMSetBlendState(bs, factor, mask);
        if (bs) bs->Release();
    }
}

void execute(void*, rhi::Command* self) {
    auto* c = reinterpret_cast<Command*>(self);
    g_armed = Armed{true, c->x, c->y, c->w, c->h, c->apply};
    c->pending.store(false, std::memory_order_release);
}

bool read_rects(const std::uint8_t* view, std::int32_t source[4], std::int32_t rect[4]) {
    __try {
        std::memcpy(source, view + kViewSourceRect, 16);
        std::memcpy(rect, view + kViewRect, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall composite_detour(void* cmd_list, void* targets, void* view, void* blurred, void* a5, void* a6) {
    ++g_count.composites;
    std::int32_t r[4]{}, vr[4]{};
    if (g_hook.installed() && device::active() && cmd_list && view &&
        read_rects(static_cast<const std::uint8_t*>(view), r, vr) && r[2] > r[0] && r[3] > r[1]) {
        if (std::memcmp(r, vr, sizeof(r)) != 0) ++g_count.rect_differs;
        Command& c = g_cmds[g_next];
        if (!c.pending.load(std::memory_order_acquire)) {
            c.base.execute = &execute;
            c.x = r[0];
            c.y = r[1];
            c.w = r[2] - r[0];
            c.h = r[3] - r[1];
            c.apply = g_enabled.load(std::memory_order_relaxed);
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
    g_hook.original<CompositeFn>()(cmd_list, targets, view, blurred, a5, a6);
}

bool target_size(ID3D11DeviceContext* ctx, UINT* w, UINT* h) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    rtv->Release();
    D3D11_RESOURCE_DIMENSION dim{};
    if (r) r->GetType(&dim);
    const bool ok = r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    if (ok) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        *w = d.Width;
        *h = d.Height;
    }
    if (r) r->Release();
    return ok;
}

// A dynamic constant buffer of `size` bytes holding UE's DrawRectangle parameters
// (PosScaleBias, UVScaleBias, InvTargetSizeAndTextureSize) for the view's rectangle drawn
// into a viewport of the rectangle's size: no position bias.
ID3D11Buffer* rectangle_constants(ID3D11DeviceContext* ctx, UINT size, const Armed& a, UINT tw, UINT th) {
    if (size < 48 || size > 4096) return nullptr;
    if (!g_cb || g_cb_size != size) {
        if (g_cb) g_cb->Release();
        g_cb = nullptr;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return nullptr;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = size;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT hr = dev->CreateBuffer(&d, nullptr, &g_cb);
        dev->Release();
        if (FAILED(hr) || !g_cb) {
            g_cb = nullptr;
            return nullptr;
        }
        g_cb_size = size;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return nullptr;
    std::vector<float> v(size / sizeof(float), 0.0f);
    const float w = static_cast<float>(a.w), h = static_cast<float>(a.h);
    const float c[12] = {w, h, 0.0f, 0.0f,  // PosScaleBias: size, position (the viewport does the offset)
                         w, h, static_cast<float>(a.x), static_cast<float>(a.y),  // UVScaleBias: the view's rectangle
                         1.0f / w, 1.0f / h, 1.0f / static_cast<float>(tw), 1.0f / static_cast<float>(th)};
    std::memcpy(v.data(), c, sizeof(c));
    std::memcpy(m.pData, v.data(), size);
    ctx->Unmap(g_cb, 0);
    return g_cb;
}

void begin_probe(ID3D11DeviceContext* ctx, int eye) {
    Probe& p = g_probe[eye];
    if (p.pending) return;
    if (!p.query) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        if (FAILED(dev->CreateQuery(&qd, &p.query))) p.query = nullptr;
        dev->Release();
        if (!p.query) return;
    }
    ctx->Begin(p.query);
}

void end_probe(ID3D11DeviceContext* ctx, int eye) {
    Probe& p = g_probe[eye];
    if (!p.query || p.pending) return;
    ctx->End(p.query);
    p.pending = true;
}

}  // namespace

bool init(std::uintptr_t composite, bool enabled) {
    if (!composite) {
        log::warn("distortion fix: composite function not found; with heat haze or refraction on screen the right eye shows the left view's composite");
        return false;
    }
    if (!g_hook.create(reinterpret_cast<void*>(composite), &composite_detour)) return false;
    g_enabled = enabled;
    log::info("distortion fix: hook on the distortion composite installed ({})", enabled ? "on" : "off ([stereo] distortion_fix = 0)");
    return true;
}

void set_enabled(bool on) { g_enabled = on && g_hook.installed(); }
bool enabled() { return g_enabled.load(); }
bool wants_hooks() { return g_hook.installed(); }

void frame() {
    g_armed.active = false;
    ID3D11DeviceContext* ctx = nullptr;
    for (int eye = 0; eye < 2; ++eye) {
        Probe& p = g_probe[eye];
        if (!p.pending || !p.query) continue;
        if (!ctx) {
            ID3D11Device* dev = nullptr;
            p.query->GetDevice(&dev);
            if (!dev) continue;
            dev->GetImmediateContext(&ctx);
            dev->Release();
            if (!ctx) continue;
        }
        D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
        const HRESULT hr = ctx->GetData(p.query, &s, sizeof(s), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            g_ps_invocations[eye] = s.PSInvocations;
            p.pending = false;
        }
    }
    if (ctx) ctx->Release();
}

bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    if (!g_armed.active) return false;
    const Armed a0 = g_armed;
    g_armed.active = false;
    if (!device::active()) return false;
    // The composite's shape: one rectangle (3 or 6 indices) into a target of the scene
    // buffer's size with a viewport covering the whole target.
    UINT tw = 0, th = 0;
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nvp, vps);
    if ((count != 3 && count != 6) || nvp == 0 || !target_size(ctx, &tw, &th) || vps[0].TopLeftX != 0.0f || vps[0].TopLeftY != 0.0f ||
        std::abs(vps[0].Width - static_cast<float>(tw)) > 1.0f || std::abs(vps[0].Height - static_cast<float>(th)) > 1.0f || a0.x < 0 ||
        a0.y < 0 || static_cast<UINT>(a0.x) >= tw || static_cast<UINT>(a0.y) >= th) {
        ++g_count.mismatch;
        return false;
    }
    Armed a = a0;
    a.w = std::min<std::int32_t>(a.w, static_cast<std::int32_t>(tw) - a.x);
    a.h = std::min<std::int32_t>(a.h, static_cast<std::int32_t>(th) - a.y);
    if (a.x == 0 && a.y == 0 && static_cast<UINT>(a.w) >= tw && static_cast<UINT>(a.h) >= th) {
        ++g_count.full_view;  // the view covers the whole target (mono): nothing to correct
        return false;
    }
    const bool offset = a.x != 0 || a.y != 0;
    const int eye = a.x == 0 ? 0 : 1;
    g_eye_pixels[eye] = static_cast<std::int64_t>(a.w) * a.h;
    if (!a.apply) {  // fix off: the engine's draw as it is, measured
        begin_probe(ctx, eye);
        draw(ctx, count, start, base, original);
        end_probe(ctx, eye);
        return true;
    }

    ID3D11Buffer* engine_cb = nullptr;
    ID3D11Buffer* ours = nullptr;
    if (offset) {
        ctx->VSGetConstantBuffers(0, 1, &engine_cb);
        D3D11_BUFFER_DESC bd{};
        if (engine_cb) engine_cb->GetDesc(&bd);
        ours = engine_cb ? rectangle_constants(ctx, bd.ByteWidth, a, tw, th) : nullptr;
        if (!ours) {
            if (engine_cb) engine_cb->Release();
            ++g_count.failed;
            return false;
        }
    }
    ID3D11RasterizerState* rs = nullptr;
    ctx->RSGetState(&rs);
    D3D11_RASTERIZER_DESC rd{};
    if (rs) rs->GetDesc(&rd);
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    if (rd.ScissorEnable) ctx->RSGetScissorRects(&nsc, scissors);

    D3D11_VIEWPORT vp = vps[0];
    vp.TopLeftX = static_cast<float>(a.x);
    vp.TopLeftY = static_cast<float>(a.y);
    vp.Width = static_cast<float>(a.w);
    vp.Height = static_cast<float>(a.h);
    ctx->RSSetViewports(1, &vp);
    if (rd.ScissorEnable) {
        const D3D11_RECT sc{a.x, a.y, a.x + a.w, a.y + a.h};
        ctx->RSSetScissorRects(1, &sc);
    }
    if (ours) ctx->VSSetConstantBuffers(0, 1, &ours);

    begin_probe(ctx, eye);
    draw(ctx, count, start, base, original);
    end_probe(ctx, eye);

    ctx->RSSetViewports(nvp, vps);
    if (rd.ScissorEnable) ctx->RSSetScissorRects(nsc, scissors);
    if (ours) ctx->VSSetConstantBuffers(0, 1, &engine_cb);
    if (engine_cb) engine_cb->Release();
    if (rs) rs->Release();
    ++g_count.applied;
    if (offset) ++g_count.shifted;
    if (!g_logged_first && offset) {
        g_logged_first = true;
        log::info("distortion fix: first correction: view {} {} {}x{} in a {}x{} target (vertex constants replaced, {} bytes)", a.x, a.y,
                  a.w, a.h, tw, th, g_cb_size);
    }
    return true;
}

void set_opaque(bool on) { g_opaque = on; }

std::string status() {
    return std::format(
        "distortion fix {}: composites {} queued {} queue failures {} applied {} (offset views {}) whole-target views {} shape mismatches {} "
        "failures {} source/view rect differ {}; composite pixels shaded per eye (last): left {} of {}, right {} of {}",
        g_enabled.load() ? "on" : "off", g_count.composites.load(), g_count.queued.load(), g_count.queue_failed.load(),
        g_count.applied.load(), g_count.shifted.load(), g_count.full_view.load(), g_count.mismatch.load(), g_count.failed.load(),
        g_count.rect_differs.load(), g_ps_invocations[0].load(), g_eye_pixels[0].load(), g_ps_invocations[1].load(), g_eye_pixels[1].load());
}

}  // namespace ff7vr::engine::distortion_fix
