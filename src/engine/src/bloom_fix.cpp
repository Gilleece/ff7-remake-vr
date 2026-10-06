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
#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
        // A view rectangle scaled by r.ScreenPercentage or the render scale can overhang the
        // buffer by a few pixels (2059 wide at x 2059 in a 4116-wide buffer): copy the part
        // that exists.
        if (x >= 0 && y >= 0 && static_cast<UINT>(x) < td.Width && static_cast<UINT>(y) < td.Height) {
            w = std::min(w, static_cast<std::int32_t>(td.Width) - x);
            h = std::min(h, static_cast<std::int32_t>(td.Height) - y);
        }
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

// With Luma (a ReShade add-on that replaces the game's tonemapping shader) loaded, the right
// view's tonemapping pass writes the left view's image: Luma's shader reads its input relative
// to the origin (docs/render.md, "ReShade and Luma"). This runs that draw with its input 0
// shifted like the bloom fix. Recognised by shape: a full-screen draw at a viewport starting
// at the middle of an R16G16B16A16 target, input 0 of
// the target's size and input 1 between a quarter and a half of the target's width and
// height (the bloom result, half the view). The game's own shader reads at the view's rectangle, so this must only run with
// Luma: mode auto (default) applies it while Luma's add-on module is loaded.
enum ShiftMode { kShiftOff = 0, kShiftOn = 1, kShiftAuto = 2 };
std::atomic<int> g_tonemap_mode{kShiftAuto};
std::atomic<bool> g_luma_loaded{false};
unsigned g_luma_check = 0;  // RHI thread: frames until the next module check
std::atomic<bool> g_tonemap_shift{false};
std::atomic<std::uint64_t> g_tonemap_shifted{0};

// Luma's own upscaling (DLSS) in stereo. Luma takes the game's anti-aliasing draw for its DLSS
// and reads the draw's viewport as the render resolution and the target's size as the output
// resolution. Each view's viewport is half of the double-wide target, so Luma concludes the
// game renders at 50 % with dynamic resolution, and its replacement shaders then scale every
// view's coordinates to the whole target (Luma's constant "DrewUpscaling" with the output
// resolution 2 x eye width): the right eye showed a squeezed quarter of the image. While the
// mod renders in stereo, Luma's calls into NVIDIA's NGX library (the driver's _nvngx.dll) to
// create or evaluate a DLSS feature are refused, so Luma falls back to the game's own
// anti-aliasing pass as it does on hardware without DLSS. Calls from anywhere else (the
// mod's own DLSS) pass. [stereo] luma_dlss = 1 lets Luma's calls through (comparison).
using NgxCreateFn = int(__cdecl*)(void* ctx, int feature, void* params, void** handle);
using NgxEvaluateFn = int(__cdecl*)(void* ctx, const void* handle, const void* params, void* callback);
constexpr int kNgxResultFail = static_cast<int>(0xBAD00000u);
hook::InlineHook g_ngx_create, g_ngx_evaluate;
std::atomic<HMODULE> g_luma_module{nullptr};
std::atomic<bool> g_luma_dlss_allowed{false};
struct NgxCounters {
    std::atomic<std::uint64_t> create_luma{0}, create_refused{0}, evaluate_luma{0}, evaluate_refused{0}, other{0};
};
NgxCounters g_ngx;

bool from_luma(void* return_address) {
    HMODULE m = nullptr;
    const HMODULE luma = g_luma_module.load(std::memory_order_relaxed);
    return luma &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(return_address), &m) &&
           m == luma;
}
bool refuse_luma_call() { return !g_luma_dlss_allowed.load(std::memory_order_relaxed) && device::active(); }

int __cdecl ngx_create_detour(void* ctx, int feature, void* params, void** handle) {
    if (from_luma(_ReturnAddress())) {
        ++g_ngx.create_luma;
        if (refuse_luma_call()) {
            ++g_ngx.create_refused;
            if (handle) *handle = nullptr;
            return kNgxResultFail;
        }
    } else {
        ++g_ngx.other;
    }
    return g_ngx_create.original<NgxCreateFn>()(ctx, feature, params, handle);
}
int __cdecl ngx_evaluate_detour(void* ctx, const void* handle, const void* params, void* callback) {
    if (from_luma(_ReturnAddress())) {
        ++g_ngx.evaluate_luma;
        if (refuse_luma_call()) {
            ++g_ngx.evaluate_refused;
            return kNgxResultFail;
        }
    } else {
        ++g_ngx.other;
    }
    return g_ngx_evaluate.original<NgxEvaluateFn>()(ctx, handle, params, callback);
}

// RHI thread: hooks NGX's D3D11 create and evaluate entry points once Luma and NGX are loaded.
void hook_ngx_for_luma(HMODULE luma) {
    g_luma_module = luma;
    if (!luma || g_ngx_evaluate.installed()) return;
    HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
    if (!ngx) return;
    void* create = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D11_CreateFeature"));
    void* evaluate = reinterpret_cast<void*>(GetProcAddress(ngx, "NVSDK_NGX_D3D11_EvaluateFeature"));
    if (!create || !evaluate) {
        log::warn("Luma DLSS in stereo: NGX entry points not found; Luma's DLSS may break the right eye");
        return;
    }
    const bool ok = g_ngx_create.create(create, &ngx_create_detour) && g_ngx_evaluate.create(evaluate, &ngx_evaluate_detour);
    log::info("Luma DLSS in stereo: {}", ok ? (g_luma_dlss_allowed.load() ? "hooks on NGX in place, Luma's calls allowed ([stereo] luma_dlss = 1)"
                                                                          : "hooks on NGX in place, Luma's calls refused while stereo renders")
                                             : "hooking NGX failed");
}

void update_tonemap_shift() {  // RHI thread, once per stereo frame
    if (g_luma_check-- == 0) {
        g_luma_check = 600;
        const HMODULE module = GetModuleHandleW(L"Luma-Final Fantasy VII Remake.addon");
        const bool luma = module != nullptr;
        if (luma != g_luma_loaded.exchange(luma))
            log::info("tonemap input shift: Luma add-on {}", luma ? "loaded (the shift applies in mode auto)" : "not loaded");
        hook_ngx_for_luma(module);
    }
    const int m = g_tonemap_mode.load(std::memory_order_relaxed);
    g_tonemap_shift = m == kShiftOn || (m == kShiftAuto && g_luma_loaded.load(std::memory_order_relaxed));
}

bool tonemap_shift(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    if (nvp == 0 || vp.TopLeftX < 1.0f || vp.TopLeftY != 0.0f) return false;
    const auto size_of = [](ID3D11View* v, UINT* w, UINT* h, DXGI_FORMAT* f) {
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim{};
        if (r) r->GetType(&dim);
        bool ok = r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D;
        if (ok) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
            *w = d.Width;
            *h = d.Height;
            *f = d.Format;
        }
        if (r) r->Release();
        return ok;
    };
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    UINT tw = 0, th = 0;
    DXGI_FORMAT tf{};
    const bool rt_ok = rtv && size_of(rtv, &tw, &th, &tf);
    if (rtv) rtv->Release();
    if (!rt_ok || (tf != DXGI_FORMAT_R16G16B16A16_FLOAT && tf != DXGI_FORMAT_R16G16B16A16_TYPELESS) ||
        std::abs(static_cast<float>(tw) - 2.0f * vp.TopLeftX) > 8.0f)  // the right view starts at the middle
        return false;
    ID3D11ShaderResourceView* srvs[2]{};
    ctx->PSGetShaderResources(0, 2, srvs);
    UINT w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    DXGI_FORMAT f0{}, f1{};
    const bool shape = srvs[0] && srvs[1] && size_of(srvs[0], &w0, &h0, &f0) && size_of(srvs[1], &w1, &h1, &f1) && w0 == tw && h0 == th &&
                       w1 * 2 <= tw + 4 && h1 * 2 <= th + 4 && w1 * 4 >= tw && h1 * 4 >= th;  // the bloom result: about half the view
    bool done = false;
    if (shape)
        done = draw_with_shifted_input(ctx, srvs[0], static_cast<std::int32_t>(vp.TopLeftX), 0, static_cast<std::int32_t>(vp.Width),
                                       static_cast<std::int32_t>(vp.Height), count, start, base, original);
    for (auto* s : srvs)
        if (s) s->Release();
    if (done) ++g_tonemap_shifted;
    return done;
}

bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    fixes::ssr_before_draw(ctx);
    if (count == 3 && g_tonemap_shift.load(std::memory_order_relaxed) && device::active() && tonemap_shift(ctx, count, start, base, original))
        return true;
#if FF7VR_ENGINE_WITH_DLSS
    if (!g_armed.active && dlss::on_draw_indexed(ctx, count, start, base, original)) return true;
#endif
    if (!g_armed.active) {
        // Full-screen passes are one triangle.
        if (count == 3 && device::active()) {
            if (fixes::hzb_draw(ctx, count, start, base, original)) return true;
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
    update_tonemap_shift();
    bool hooks = g_enabled.load(std::memory_order_relaxed) || g_ao_enabled.load(std::memory_order_relaxed) || fixes::ssr_wants_hooks() ||
                 fixes::hzb_skip() != 0;
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

void set_tonemap_shift(int mode) {
    g_tonemap_mode = std::clamp(mode, 0, 2);
    g_luma_check = 0;
}
std::string tonemap_shift_status() {
    static const char* names[] = {"off", "on", "auto"};
    return std::format("tonemap input shift {} ({}; Luma add-on {}): applied {}", names[g_tonemap_mode.load()],
                       g_tonemap_shift.load() ? "active" : "inactive", g_luma_loaded.load() ? "loaded" : "not loaded", g_tonemap_shifted.load());
}

void set_luma_dlss_allowed(bool allowed) { g_luma_dlss_allowed = allowed; }
std::string luma_dlss_status() {
    return std::format("Luma DLSS in stereo {} (hooks {}): Luma create calls {} refused {}, evaluate calls {} refused {}, other callers {}",
                       g_luma_dlss_allowed.load() ? "allowed" : "refused", g_ngx_evaluate.installed() ? "on" : "off",
                       g_ngx.create_luma.load(), g_ngx.create_refused.load(), g_ngx.evaluate_luma.load(), g_ngx.evaluate_refused.load(),
                       g_ngx.other.load());
}

std::string ao_status() {
    return std::format("ambient occlusion fix {}: applied {} failed {}", g_ao_enabled.load() ? "on" : "off", g_ao.applied.load(),
                       g_ao.failed.load());
}

}  // namespace ff7vr::engine::bloom_fix
