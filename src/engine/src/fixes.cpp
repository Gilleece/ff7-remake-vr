#include "fixes.h"

#include "engine_internal.h"

#include "gpu_trace.h"
#include "shaders.h"
#include "stereo_device.h"

#include <d3d11_1.h>

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/engine/cvars.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <format>
#include <mutex>

namespace ff7vr::engine::fixes {
namespace {

// Byte patches with a known original and patched value.
std::optional<bool> read_patch(const std::uint8_t* p, std::uint8_t off, std::uint8_t on) {
    if (!p) return std::nullopt;
    if (*p == off) return false;
    if (*p == on) return true;
    return std::nullopt;
}

bool write_patch(std::uint8_t* p, std::uint8_t off, std::uint8_t on, bool enable, const char* what) {
    const auto cur = read_patch(p, off, on);
    if (!cur) {
        log::warn("fixes: {} patch site not available", what);
        return false;
    }
    if (*cur == enable) return true;
    const std::uint8_t v = enable ? on : off;
    if (!hook::write_memory(p, &v, 1)) {
        log::warn("fixes: writing the {} patch failed", what);
        return false;
    }
    log::info("fixes: {} patch {}", what, enable ? "on" : "off (game default)");
    return true;
}

bool g_sysres_active = false;
std::int32_t g_sysres_game[2]{};     // the game's value before the first override
std::int32_t g_sysres_written[2]{};  // what we wrote last

}  // namespace

std::optional<bool> light_patch() { return read_patch(addresses().LightSortKeyImm, 0x40, 0x60); }
bool set_light_patch(bool on) { return write_patch(addresses().LightSortKeyImm, 0x40, 0x60, on, "light sort-key"); }

namespace {
std::mutex g_light_mutex;
bool g_light_wanted = true;
bool g_light_stereo = false;
bool update_light_patch() {  // g_light_mutex held
    return set_light_patch(g_light_wanted && g_light_stereo);
}
}  // namespace

bool set_light_fix(bool wanted) {
    std::lock_guard lock(g_light_mutex);
    g_light_wanted = wanted;
    return update_light_patch();
}

bool light_fix_wanted() {
    std::lock_guard lock(g_light_mutex);
    return g_light_wanted;
}

void light_fix_stereo(bool stereo_active) {
    std::lock_guard lock(g_light_mutex);
    g_light_stereo = stereo_active;
    if (g_light_wanted || light_patch().value_or(false)) update_light_patch();
}

// 0x75 = jne rel8, 0xEB = jmp rel8 (same displacement).
std::optional<bool> view_rect_patch() { return read_patch(addresses().ViewRectOverrideJump, 0x75, 0xEB); }
bool set_view_rect_patch(bool on) {
    return write_patch(addresses().ViewRectOverrideJump, 0x75, 0xEB, on, "windowed-fullscreen view rect");
}

void apply_system_resolution(std::int32_t width, std::int32_t height) {
    std::int32_t* r = addresses().GSystemResolution;
    if (!r || width <= 0 || height <= 0) return;
    if (!g_sysres_active) {
        g_sysres_game[0] = r[0];
        g_sysres_game[1] = r[1];
        g_sysres_active = true;
        log::info("fixes: GSystemResolution {}x{} -> {}x{} while stereo renders (scene buffers cover the eye target)",
                  r[0], r[1], width, height);
    } else if (r[0] != g_sysres_written[0] || r[1] != g_sysres_written[1]) {
        // The game changed it (window resize): that is its value now.
        g_sysres_game[0] = r[0];
        g_sysres_game[1] = r[1];
        if (r[0] == width && r[1] == height) return;
    }
    r[0] = width;
    r[1] = height;
    g_sysres_written[0] = width;
    g_sysres_written[1] = height;
}

void restore_system_resolution() {
    std::int32_t* r = addresses().GSystemResolution;
    if (!r || !g_sysres_active) return;
    g_sysres_active = false;
    if (r[0] == g_sysres_written[0] && r[1] == g_sysres_written[1]) {
        r[0] = g_sysres_game[0];
        r[1] = g_sysres_game[1];
    }
    log::info("fixes: GSystemResolution back to {}x{}", r[0], r[1]);
}

bool system_resolution_overridden() { return g_sysres_active; }

namespace {
std::mutex g_vr_window_mutex;
std::string g_vr_window_size = "1280x720";  // r.SetRes size while VR renders; empty = keep
std::string g_vr_window_restore;            // r.SetRes value that puts the game's mode back
}  // namespace

void set_vr_window_size(const std::string& size) {
    std::lock_guard lock(g_vr_window_mutex);
    g_vr_window_size = (size == "0" || size == "off") ? std::string() : size;
}

void vr_window_enter() {
    std::int32_t* r = addresses().GSystemResolution;
    if (!r) return;
    std::string size, restore;
    {
        std::lock_guard lock(g_vr_window_mutex);
        if (g_vr_window_size.empty() || !g_vr_window_restore.empty()) return;
        // The game's own values: before the stereo override, or the ones remembered by it.
        const std::int32_t w = g_sysres_active ? g_sysres_game[0] : r[0];
        const std::int32_t h = g_sysres_active ? g_sysres_game[1] : r[1];
        const std::int32_t mode = r[2];
        if (mode != 0 && mode != 1) return;  // already a window
        restore = std::format("{}x{}{}", w, h, mode == 0 ? "f" : "wf");
        size = g_vr_window_size;
        g_vr_window_restore = restore;
    }
    log::info("fixes: game window {} -> {}w while VR renders", restore, size);
    cvar::set(L"r.SetRes", log::widen(size + "w"));
}

void vr_window_leave() {
    std::string restore;
    {
        std::lock_guard lock(g_vr_window_mutex);
        restore.swap(g_vr_window_restore);
    }
    if (restore.empty()) return;
    log::info("fixes: game window back to {}", restore);
    cvar::set(L"r.SetRes", log::widen(restore));
}

std::string vr_window_status() {
    std::lock_guard lock(g_vr_window_mutex);
    std::int32_t* r = addresses().GSystemResolution;
    return std::format("vr window {} (restore '{}'), window mode {}", g_vr_window_size.empty() ? "off" : g_vr_window_size,
                       g_vr_window_restore, r ? r[2] : -1);
}

// ------------------------------------------------------------------ reflections per eye
namespace {
std::atomic<bool> g_ssr_on{false};
std::atomic<int> g_ssr_poison{0};  // test (`ssr poison 1`): fill the skipped half with a loud colour (2: the run's own half, the control)
std::atomic<std::uint64_t> g_ssr_limited{0}, g_ssr_extra{0}, g_ssr_frames{0};
// Columns added on the far side of the middle when a run or copy is limited to one view's
// half: with r.ScreenPercentage below 100 the engine's rounded view rectangles can cross the
// middle by a pixel or two (a 2059-wide left view in a 4116-wide target).
constexpr UINT kSsrMargin = 16;
int g_ssr_index = 0;  // RHI thread: reflection runs seen this frame
// RHI thread: the engine's rasterizer state -> the same state with the scissor test on.
struct RsPair {
    ID3D11RasterizerState* engine = nullptr;  // compared only
    ID3D11RasterizerState* scissor = nullptr;  // owned
};
RsPair g_rs_cache[8];
unsigned g_rs_next = 0;

bool is_hzb(ID3D11ShaderResourceView* srv) {
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    bool yes = false;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        yes = (d.Format == DXGI_FORMAT_R16_FLOAT || d.Format == DXGI_FORMAT_R16_TYPELESS) && d.MipLevels >= 4;
    }
    r->Release();
    return yes;
}

ID3D11RasterizerState* scissor_state(ID3D11DeviceContext* ctx, ID3D11RasterizerState* engine) {
    for (const RsPair& p : g_rs_cache)
        if (p.scissor && p.engine == engine) return p.scissor;
    D3D11_RASTERIZER_DESC d{};
    if (engine) {
        engine->GetDesc(&d);
    } else {
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.ScissorEnable = TRUE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11RasterizerState* s = nullptr;
    if (dev) {
        dev->CreateRasterizerState(&d, &s);
        dev->Release();
    }
    if (!s) return nullptr;
    RsPair& slot = g_rs_cache[g_rs_next];
    g_rs_next = (g_rs_next + 1) % (sizeof(g_rs_cache) / sizeof(g_rs_cache[0]));
    if (slot.scissor) slot.scissor->Release();
    slot = RsPair{engine, s};
    return s;
}

// The scene-colour-sized inputs each reflection run of this frame read (compared only).
// Square Enix copies the whole scene colour once per view into the texture that view's next
// reflection run reads as the previous frame's colour.
ID3D11Resource* g_ssr_inputs[2][4]{};
std::atomic<std::uint64_t> g_ssr_copies_halved{0};

void note_ssr_inputs(int index, ID3D11ShaderResourceView* const* srvs, const D3D11_TEXTURE2D_DESC& target) {
    int n = 0;
    for (int i = 0; i < 16 && n < 4; ++i) {
        if (!srvs[i]) continue;
        ID3D11Resource* r = nullptr;
        srvs[i]->GetResource(&r);
        if (!r) continue;
        D3D11_RESOURCE_DIMENSION dim{};
        r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
            if (d.Width == target.Width && d.Height == target.Height && d.MipLevels == 1 && d.SampleDesc.Count == 1 &&
                (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS))
                g_ssr_inputs[index][n++] = r;
        }
        r->Release();
    }
}

void poison_half(ID3D11DeviceContext* ctx, ID3D11Resource* dst, const D3D11_TEXTURE2D_DESC& d, bool left) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11RenderTargetView* rtv = nullptr;
    if (dev && (d.BindFlags & D3D11_BIND_RENDER_TARGET)) {
        D3D11_RENDER_TARGET_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(dst, &v, &rtv);
    }
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (rtv && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
        const LONG half = static_cast<LONG>(d.Width / 2);
        const D3D11_RECT r{left ? 0 : half, 0, left ? half : static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
        const float magenta[4] = {50.0f, 0.0f, 50.0f, 1.0f};
        ctx1->ClearView(rtv, magenta, &r, 1);
        ctx1->Release();
    }
    if (rtv) rtv->Release();
    if (dev) dev->Release();
}

// The per-view copy of the scene colour: only the half the view's reflections read.
bool ssr_copy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src, gpu_trace::CopyResourceFn original) {
    if (!g_ssr_on.load(std::memory_order_relaxed) || g_ssr_index == 0 || !dst || !src || !device::active()) return false;
    int view = -1;
    for (int k = 0; k < 2 && view < 0; ++k)
        for (ID3D11Resource* r : g_ssr_inputs[k])
            if (r && r == dst) view = k;
    if (view < 0) return false;
    D3D11_RESOURCE_DIMENSION ds{}, ss{};
    dst->GetType(&ds);
    src->GetType(&ss);
    if (ds != D3D11_RESOURCE_DIMENSION_TEXTURE2D || ss != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC dd{}, sd{};
    static_cast<ID3D11Texture2D*>(dst)->GetDesc(&dd);
    static_cast<ID3D11Texture2D*>(src)->GetDesc(&sd);
    if (dd.Width != sd.Width || dd.Height != sd.Height || dd.Format != sd.Format || dd.MipLevels != 1 || sd.MipLevels != 1 ||
        dd.ArraySize != 1 || sd.ArraySize != 1 || dd.Width * 2 < dd.Height * 3)
        return false;
    const bool right_half = (view == 1) != device::settings().swap_rects.load();
    const UINT half = dd.Width / 2;
    const D3D11_BOX box{right_half ? half - std::min(half, kSsrMargin) : 0, 0, 0, right_half ? dd.Width : std::min(dd.Width, half + kSsrMargin),
                        dd.Height, 1};
    ctx->CopySubresourceRegion(dst, 0, box.left, 0, 0, src, 0, &box);
    if (const int p = g_ssr_poison.load(std::memory_order_relaxed); p == 1 || p == 3) poison_half(ctx, dst, dd, p == 3 ? !right_half : right_half);
    (void)original;
    ++g_ssr_copies_halved;
    return true;
}
}  // namespace

// ------------------------------------------------------------------ right-eye reflections
namespace {
// The run of the view whose rectangle does not start at the origin computes that view's
// reflections at the origin of the target (it reads its inputs at its own rectangle, but
// writes relative to the origin) and leaves its own half at zero, which is the half its
// composite reads. The fix runs that draw into a scratch target of half the width (so only
// the half at the origin is computed) and copies the result into the view's own half.
// 0 off, 1 the right view's run into a scratch target, 2 no screen-space reflections in
// either eye (each run is replaced by clearing its target, so both eyes match), 3 the right
// view's run drawn in place at its own rectangle with the pass's pixel shader patched
// (shaders.h): the shader then derives every position from the pixel's position in the
// target, which is right for any view rectangle.
std::atomic<int> g_ssr_fix{0};
std::atomic<ID3D11PixelShader*> g_ssr_ps{nullptr};  // the reflection runs' pixel shader, last seen
// Mode 3, RHI thread: the patched shader for this frame's runs (referenced) and the engine's
// shader it belongs to (compared only). Without it a frame falls back to mode 2.
ID3D11PixelShader* g_ssr_patched = nullptr;
ID3D11PixelShader* g_ssr_patched_of = nullptr;
bool g_ssr_frame_patched = false;
std::atomic<std::uint64_t> g_ssr_inplace{0}, g_ssr_no_patch{0};
std::atomic<bool> g_ssr_no_patch_logged{false};
// Tests (`ssr test equal`, `halves`, `shift`): armed for the next frame, result kept as text.
std::atomic<int> g_ssr_test{0};  // 1 equivalence at the origin, 2 the two halves compared, 3 reference for the run in place
std::mutex g_ssr_test_mutex;
std::string g_ssr_test_result = "no test yet";
ID3D11Resource* g_ssr_last_target = nullptr;  // RHI thread, compared only: this frame's reflection target

void set_test_result(std::string r) {
    log::info("fixes: reflections test: {}", r);
    std::lock_guard lk(g_ssr_test_mutex);
    g_ssr_test_result = std::move(r);
}
std::atomic<std::uint64_t> g_ssr_fixed{0}, g_ssr_fix_failed{0}, g_ssr_cleared{0};
struct SsrScratch {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
    unsigned idle_frames = 0;
};
SsrScratch g_ssr_scratch;  // RHI thread

void release_ssr_scratch() {
    if (g_ssr_scratch.rtv) g_ssr_scratch.rtv->Release();
    if (g_ssr_scratch.tex) g_ssr_scratch.tex->Release();
    g_ssr_scratch = SsrScratch{};
}

SsrScratch* ensure_ssr_scratch(ID3D11DeviceContext* ctx, UINT width, UINT height, DXGI_FORMAT format, DXGI_FORMAT view_format) {
    SsrScratch& s = g_ssr_scratch;
    s.idle_frames = 0;
    if (s.tex && s.desc.Width == width && s.desc.Height == height && s.desc.Format == format && s.view_format == view_format) return &s;
    release_ssr_scratch();
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &s.tex)) && s.tex;
    if (ok) {
        D3D11_RENDER_TARGET_VIEW_DESC v{};
        v.Format = view_format;
        v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ok = SUCCEEDED(dev->CreateRenderTargetView(s.tex, &v, &s.rtv)) && s.rtv;
    }
    dev->Release();
    if (!ok) {
        release_ssr_scratch();
        return nullptr;
    }
    s.desc = d;
    s.view_format = view_format;
    log::info("fixes: right-eye reflections scratch target {}x{} format {}", width, height, static_cast<int>(format));
    return &s;
}

// Where the right view's rectangle starts in the target. Half the width at full resolution;
// with r.ScreenPercentage below 100 the engine rounds the scaled rectangles (a 4116-wide
// target has the right view at x 2059 or 2060), so the x actually used is taken from the
// right view's composite, the next draw that reads the reflections, and remembered for the
// following frames. RHI thread.
UINT g_ssr_right_x = 0, g_ssr_right_x_width = 0;
std::atomic<std::uint64_t> g_ssr_moved{0}, g_ssr_unread{0};
std::atomic<int> g_ssr_last_x{-1};
struct SsrPending {
    ID3D11Resource* target = nullptr;       // referenced
    ID3D11RenderTargetView* rtv = nullptr;  // referenced
    UINT sub = 0, x = 0, width = 0, height = 0;
};
SsrPending g_ssr_pending;

void clear_ssr_pending() {
    if (g_ssr_pending.rtv) g_ssr_pending.rtv->Release();
    if (g_ssr_pending.target) g_ssr_pending.target->Release();
    g_ssr_pending = SsrPending{};
}

void copy_ssr_result(ID3D11DeviceContext* ctx, const SsrPending& p, UINT x) {
    const UINT w = std::min(g_ssr_scratch.desc.Width, p.width - x);
    const D3D11_BOX box{0, 0, 0, w, std::min(g_ssr_scratch.desc.Height, p.height), 1};
    ctx->CopySubresourceRegion(p.target, p.sub, x, 0, 0, g_ssr_scratch.tex, 0, &box);
}

// Runs the reflection draw of the view in the right half into the scratch target and copies
// the result to that view's rectangle in the engine's target. False if it could not (nothing
// drawn).
bool ssr_draw_shifted(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base,
                      void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT)) {
    clear_ssr_pending();
    ID3D11RenderTargetView* engine_rtv = nullptr;
    ID3D11DepthStencilView* engine_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &engine_rtv, &engine_dsv);
    bool done = false;
    ID3D11Resource* target = nullptr;
    if (engine_rtv) engine_rtv->GetResource(&target);
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    if (engine_rtv) engine_rtv->GetDesc(&vd);
    if (target && !engine_dsv && vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td{};
        static_cast<ID3D11Texture2D*>(target)->GetDesc(&td);
        const UINT half = td.Width / 2;
        // A little wider than half, for a right view that starts left of the middle.
        SsrScratch* s = ensure_ssr_scratch(ctx, std::min(td.Width, half + kSsrMargin), td.Height, td.Format, vd.Format);
        if (s) {
            ctx->OMSetRenderTargets(1, &s->rtv, nullptr);
            // Viewport, scissor and inputs stay the engine's: the scratch target clips the
            // full-width triangle to the part at the origin.
            original(ctx, count, start, base);
            ctx->OMSetRenderTargets(1, &engine_rtv, nullptr);
            const UINT x = (g_ssr_right_x_width == td.Width && g_ssr_right_x > 0 && g_ssr_right_x < td.Width) ? g_ssr_right_x : half;
            g_ssr_pending = SsrPending{target, engine_rtv, D3D11CalcSubresource(vd.Texture2D.MipSlice, 0, td.MipLevels), x, td.Width, td.Height};
            target = nullptr;  // references now held by g_ssr_pending
            engine_rtv = nullptr;
            copy_ssr_result(ctx, g_ssr_pending, x);
            g_ssr_last_x = static_cast<int>(x);
            done = true;
        }
    }
    if (target) target->Release();
    if (engine_rtv) engine_rtv->Release();
    if (engine_dsv) engine_dsv->Release();
    ++(done ? g_ssr_fixed : g_ssr_fix_failed);
    return done;
}
// Copies a region of an RGBA16F texture into CPU memory (a staging copy, mapped: stalls).
bool read_rgba16(ID3D11DeviceContext* ctx, ID3D11Resource* src, UINT sub, const D3D11_BOX& box, std::vector<std::uint16_t>& out) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = box.right - box.left;
    d.Height = box.bottom - box.top;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st)) && st;
    dev->Release();
    if (!ok) return false;
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, sub, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        out.resize(static_cast<std::size_t>(d.Width) * d.Height * 4);
        for (UINT y = 0; y < d.Height; ++y)
            std::memcpy(out.data() + static_cast<std::size_t>(y) * d.Width * 4, static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch,
                        static_cast<std::size_t>(d.Width) * 8);
        ctx->Unmap(st, 0);
    }
    st->Release();
    return ok;
}

float h2f(std::uint16_t h) { return DirectX::PackedVector::XMConvertHalfToFloat(h); }

// Test 1: the run at the origin drawn again with the patched shader into the scratch target;
// for a view whose rectangle starts at the origin the two must be identical bit for bit.
void ssr_equivalence_test(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base,
                          void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT), ID3D11PixelShader* patched) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* target = nullptr;
    if (rtv) rtv->GetResource(&target);
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    if (rtv) rtv->GetDesc(&vd);
    std::string result = "err could not run";
    if (target && vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td{};
        static_cast<ID3D11Texture2D*>(target)->GetDesc(&td);
        const UINT half = td.Width / 2;
        SsrScratch* sc = ensure_ssr_scratch(ctx, std::min(td.Width, half + kSsrMargin), td.Height, td.Format, vd.Format);
        if (sc) {
            ID3D11PixelShader* engine_ps = nullptr;
            ID3D11ClassInstance* inst[16]{};
            UINT ninst = 16;
            ctx->PSGetShader(&engine_ps, inst, &ninst);
            ID3D11RasterizerState* rs = nullptr;
            ctx->RSGetState(&rs);
            UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_RECT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            ctx->RSGetScissorRects(&nsc, saved);
            ctx->RSSetState(scissor_state(ctx, rs));
            const D3D11_RECT r{0, 0, static_cast<LONG>(half), static_cast<LONG>(td.Height)};
            ctx->RSSetScissorRects(1, &r);
            ctx->OMSetRenderTargets(1, &sc->rtv, nullptr);
            ctx->PSSetShader(patched, nullptr, 0);
            original(ctx, count, start, base);
            ctx->PSSetShader(engine_ps, inst, ninst);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->RSSetState(rs);
            ctx->RSSetScissorRects(nsc, nsc ? saved : nullptr);
            if (rs) rs->Release();
            if (engine_ps) engine_ps->Release();
            for (UINT i = 0; i < ninst && i < 16; ++i)
                if (inst[i]) inst[i]->Release();
            std::vector<std::uint16_t> a, b;
            const D3D11_BOX box{0, 0, 0, half, td.Height, 1};
            const UINT sub = D3D11CalcSubresource(vd.Texture2D.MipSlice, 0, td.MipLevels);
            if (read_rgba16(ctx, target, sub, box, a) && read_rgba16(ctx, sc->tex, 0, box, b) && a.size() == b.size()) {
                std::size_t differ = 0, nonzero = 0;
                float maxd = 0;
                for (std::size_t i = 0; i < a.size(); i += 4) {
                    const bool nz = a[i] || a[i + 1] || a[i + 2] || a[i + 3];
                    nonzero += nz ? 1 : 0;
                    if (std::memcmp(&a[i], &b[i], 8) != 0) {
                        ++differ;
                        for (int c = 0; c < 4; ++c) maxd = std::max(maxd, std::fabs(h2f(a[i + c]) - h2f(b[i + c])));
                    }
                }
                result = std::format("equivalence at the origin ({}x{}): pixels differing {} of {} (max difference {}), non-zero in the engine's run {}",
                                     half, td.Height, differ, a.size() / 4, maxd, nonzero);
            } else {
                result = "err read-back failed";
            }
        }
    }
    if (target) target->Release();
    if (rtv) rtv->Release();
    set_test_result(result);
}

// Test 3 (`ssr test shift`), right after the run drawn in place with the patched shader: the
// same view's reflections computed the way the game computes a view at the origin, as a
// reference. The game's own shader is drawn into the scratch target with every full-size input
// replaced by a copy holding this view's part at the origin and a copy of the view's constants
// with its rectangle moved to the origin (rows 58.w, 121.x, 123.w, 124.x of the view buffer:
// ScreenPositionScaleBias, ViewRectMin and their previous-frame twins; docs/re/engine.md).
// Everything else (matrices, hierarchical depth, noise, frame index) is the view's own, so
// the two results must agree pixel for pixel.
struct RefCopy {
    ID3D11Texture2D* whole = nullptr;  // depth-stencil inputs: copied whole first (no partial copies of those)
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};

void release_ref(RefCopy& c) {
    if (c.srv) c.srv->Release();
    if (c.tex) c.tex->Release();
    if (c.whole) c.whole->Release();
    c = RefCopy{};
}

// A copy of the input bound at `slot` whose origin holds the part from x on; nullptr if the
// input is not a full-size 2D texture of W x H.
ID3D11ShaderResourceView* shifted_input(ID3D11DeviceContext* ctx, ID3D11Device* dev, UINT slot, UINT x, UINT W, UINT H, RefCopy& c) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(slot, 1, &srv);
    if (!srv) return nullptr;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    srv->GetDesc(&vd);
    srv->Release();
    D3D11_RESOURCE_DIMENSION dim{};
    if (res) res->GetType(&dim);
    ID3D11ShaderResourceView* out = nullptr;
    if (res && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td{};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
        if (td.Width == W && td.Height == H && td.ArraySize == 1 && td.SampleDesc.Count == 1 && vd.Texture2D.MostDetailedMip == 0) {
            const bool ds = (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
            D3D11_TEXTURE2D_DESC want = td;
            want.MipLevels = 1;
            want.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            want.MiscFlags = 0;
            want.CPUAccessFlags = 0;
            want.Usage = D3D11_USAGE_DEFAULT;
            bool ok = SUCCEEDED(dev->CreateTexture2D(&want, nullptr, &c.tex)) && c.tex;
            if (ok && ds) {
                D3D11_TEXTURE2D_DESC w = td;  // CopyResource needs the same mip count
                w.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                w.MiscFlags = 0;
                ok = SUCCEEDED(dev->CreateTexture2D(&w, nullptr, &c.whole)) && c.whole;
            }
            if (ok) {
                D3D11_SHADER_RESOURCE_VIEW_DESC v = vd;
                v.Texture2D.MipLevels = 1;
                ok = SUCCEEDED(dev->CreateShaderResourceView(c.tex, &v, &c.srv)) && c.srv;
            }
            if (ok) {
                ID3D11Resource* from = res;
                if (ds) {
                    ctx->CopyResource(c.whole, res);
                    from = c.whole;
                }
                const D3D11_BOX box{x, 0, 0, W, H, 1};
                ctx->CopySubresourceRegion(c.tex, 0, 0, 0, 0, from, D3D11CalcSubresource(0, 0, td.MipLevels), &box);
                out = c.srv;
            } else {
                release_ref(c);
            }
        }
    }
    if (res) res->Release();
    return out;
}

void ssr_reference_test(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base,
                        void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT), ID3D11PixelShader* engine_ps) {
    std::string result = "err could not run";
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* target = nullptr;
    if (rtv) rtv->GetResource(&target);
    D3D11_RENDER_TARGET_VIEW_DESC rvd{};
    if (rtv) rtv->GetDesc(&rvd);
    ID3D11Buffer* view_cb = nullptr;
    ctx->PSGetConstantBuffers(1, 1, &view_cb);
    D3D11_BUFFER_DESC bd{};
    if (view_cb) view_cb->GetDesc(&bd);
    if (dev && target && view_cb && bd.ByteWidth >= 128 * 16 && rvd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC td{};
        static_cast<ID3D11Texture2D*>(target)->GetDesc(&td);
        // The view's constants, read back (a stall; this is a test).
        std::vector<float> c(bd.ByteWidth / 4);
        bool ok = false;
        {
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = bd.ByteWidth;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Buffer* st = nullptr;
            if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &st)) && st) {
                ctx->CopyResource(st, view_cb);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                    std::memcpy(c.data(), m.pData, bd.ByteWidth);
                    ctx->Unmap(st, 0);
                    ok = true;
                }
                st->Release();
            }
        }
        const UINT x = ok ? static_cast<UINT>(c[121 * 4]) : 0;  // ViewRectMin.x
        const UINT vw = ok ? static_cast<UINT>(c[122 * 4]) : 0;  // ViewSizeAndInvSize.x
        const UINT half = td.Width / 2;
        SsrScratch* sc = ok && x > 0 && x < td.Width ? ensure_ssr_scratch(ctx, std::min(td.Width, half + kSsrMargin), td.Height, td.Format, rvd.Format) : nullptr;
        if (sc) {
            const float sx = c[58 * 4 + 3] - c[123 * 4 + 3];  // what the ScreenPositionScaleBias offsets lose, for the log
            const float bias = c[58 * 4 + 3] - static_cast<float>(x) / static_cast<float>(td.Width);
            c[58 * 4 + 3] = bias;   // ScreenPositionScaleBias.w: the view's left edge at u 0
            c[121 * 4] = 0.0f;      // ViewRectMin.x
            c[123 * 4 + 3] -= static_cast<float>(x) / static_cast<float>(td.Width);  // the previous frame's ScreenPositionScaleBias.w
            c[124 * 4] -= static_cast<float>(x);  // the previous frame's ViewRectMin.x
            (void)sx;
            D3D11_BUFFER_DESC nd{};
            nd.ByteWidth = bd.ByteWidth;
            nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA init{c.data(), 0, 0};
            ID3D11Buffer* ref_cb = nullptr;
            dev->CreateBuffer(&nd, &init, &ref_cb);
            RefCopy copies[16]{};
            ID3D11ShaderResourceView* kept[16]{};
            ctx->PSGetShaderResources(0, 16, kept);
            unsigned shifted = 0;
            for (UINT k = 0; k < 16; ++k)
                if (ID3D11ShaderResourceView* v = shifted_input(ctx, dev, k, x, td.Width, td.Height, copies[k])) {
                    ctx->PSSetShaderResources(k, 1, &v);
                    ++shifted;
                }
            ID3D11PixelShader* bound_ps = nullptr;
            ID3D11ClassInstance* inst[16]{};
            UINT ninst = 16;
            ctx->PSGetShader(&bound_ps, inst, &ninst);
            ID3D11RasterizerState* rs = nullptr;
            ctx->RSGetState(&rs);
            UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_RECT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            ctx->RSGetScissorRects(&nsc, saved);
            ctx->RSSetState(scissor_state(ctx, rs));
            const D3D11_RECT r{0, 0, static_cast<LONG>(half), static_cast<LONG>(td.Height)};
            ctx->RSSetScissorRects(1, &r);
            ctx->OMSetRenderTargets(1, &sc->rtv, nullptr);
            ctx->PSSetShader(engine_ps, nullptr, 0);
            if (ref_cb) ctx->PSSetConstantBuffers(1, 1, &ref_cb);
            const float clear[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(sc->rtv, clear);
            if (ref_cb) original(ctx, count, start, base);
            ctx->PSSetConstantBuffers(1, 1, &view_cb);
            ctx->PSSetShader(bound_ps, inst, ninst);
            ctx->PSSetShaderResources(0, 16, kept);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->RSSetState(rs);
            ctx->RSSetScissorRects(nsc, nsc ? saved : nullptr);
            if (rs) rs->Release();
            if (bound_ps) bound_ps->Release();
            for (UINT i = 0; i < ninst && i < 16; ++i)
                if (inst[i]) inst[i]->Release();
            for (auto* v : kept)
                if (v) v->Release();
            for (auto& cp : copies) release_ref(cp);
            if (ref_cb) ref_cb->Release();
            // The view's part of the engine's target (drawn in place) against the reference.
            const UINT w = std::min({vw ? vw : td.Width - x, td.Width - x, half});
            std::vector<std::uint16_t> a, b;
            const D3D11_BOX in_place{x, 0, 0, x + w, td.Height, 1};
            const D3D11_BOX at_origin{0, 0, 0, w, td.Height, 1};
            const UINT sub = D3D11CalcSubresource(rvd.Texture2D.MipSlice, 0, td.MipLevels);
            if (!ref_cb) {
                result = "err constant buffer copy failed";
            } else if (read_rgba16(ctx, target, sub, in_place, a) && read_rgba16(ctx, sc->tex, 0, at_origin, b) && a.size() == b.size()) {
                std::size_t differ = 0, nz_a = 0, nz_b = 0, over = 0;
                float maxd = 0;
                double sum_a = 0, sum_b = 0;
                for (std::size_t i = 0; i < a.size(); i += 4) {
                    nz_a += (a[i] || a[i + 1] || a[i + 2] || a[i + 3]) ? 1 : 0;
                    nz_b += (b[i] || b[i + 1] || b[i + 2] || b[i + 3]) ? 1 : 0;
                    sum_a += h2f(a[i + 3]);
                    sum_b += h2f(b[i + 3]);
                    if (std::memcmp(&a[i], &b[i], 8) != 0) {
                        ++differ;
                        float d = 0;
                        for (int k = 0; k < 4; ++k) d = std::max(d, std::fabs(h2f(a[i + k]) - h2f(b[i + k])));
                        maxd = std::max(maxd, d);
                        over += d > 0.01f ? 1 : 0;
                    }
                }
                const double n = static_cast<double>(a.size() / 4);
                result = std::format("reference (the view's part moved to the origin, the game's shader; {} inputs moved) against the run in place "
                                     "(x {}, {}x{}): pixels differing {} ({:.4f} %), of them by more than 0.01: {}, max difference {}; non-zero in place "
                                     "{:.2f} % reference {:.2f} %; mean alpha in place {:.4f} reference {:.4f}",
                                     shifted, x, w, td.Height, differ, 100.0 * differ / n, over, maxd, 100.0 * nz_a / n, 100.0 * nz_b / n, sum_a / n,
                                     sum_b / n);
            } else {
                result = "err read-back failed";
            }
        }
    }
    if (view_cb) view_cb->Release();
    if (target) target->Release();
    if (rtv) rtv->Release();
    if (dev) dev->Release();
    set_test_result(result);
}

// Test 2: the reflection target once both runs have written it, the two halves compared
// pixel for pixel (meaningful when both views are the same camera: `stereo ipd 0` with the
// fixed host). The statistics are computed on a worker thread.
void ssr_halves_test(ID3D11DeviceContext* ctx, ID3D11Resource* target, UINT right_x) {
    D3D11_TEXTURE2D_DESC td{};
    static_cast<ID3D11Texture2D*>(target)->GetDesc(&td);
    const UINT w = std::min(right_x, td.Width - right_x) - kSsrMargin;  // left of the right run's margin
    auto pixels = std::make_shared<std::vector<std::uint16_t>>();
    const D3D11_BOX box{0, 0, 0, td.Width, td.Height, 1};
    if (!read_rgba16(ctx, target, 0, box, *pixels)) {
        set_test_result("err read-back failed");
        return;
    }
    const UINT W = td.Width, H = td.Height;
    std::thread([pixels, W, H, w, right_x] {
        const auto& p = *pixels;
        auto luma = [&](UINT x, UINT y) {
            const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
            return (h2f(p[i]) + h2f(p[i + 1]) + h2f(p[i + 2])) / 3.0;
        };
        auto alpha = [&](UINT x, UINT y) { return static_cast<double>(h2f(p[(static_cast<std::size_t>(y) * W + x) * 4 + 3])); };
        struct Acc {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, mad = 0;
            std::size_t n = 0, nza = 0, nzb = 0;
            void add(double a, double b) {
                sa += a;
                sb += b;
                saa += a * a;
                sbb += b * b;
                sab += a * b;
                mad += std::fabs(a - b);
                nza += a != 0 ? 1 : 0;
                nzb += b != 0 ? 1 : 0;
                ++n;
            }
            double corr() const {
                const double va = saa / n - (sa / n) * (sa / n), vb = sbb / n - (sb / n) * (sb / n);
                return va > 0 && vb > 0 ? (sab / n - (sa / n) * (sb / n)) / std::sqrt(va * vb) : 0.0;
            }
        };
        Acc l, a, lb, ab;
        constexpr UINT B = 8;
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < w; ++x) {
                l.add(luma(x, y), luma(x + right_x, y));
                a.add(alpha(x, y), alpha(x + right_x, y));
            }
        for (UINT by = 0; by + B <= H; by += B)
            for (UINT bx = 0; bx + B <= w; bx += B) {
                double s0 = 0, s1 = 0, t0 = 0, t1 = 0;
                for (UINT y = by; y < by + B; ++y)
                    for (UINT x = bx; x < bx + B; ++x) {
                        s0 += luma(x, y);
                        s1 += luma(x + right_x, y);
                        t0 += alpha(x, y);
                        t1 += alpha(x + right_x, y);
                    }
                lb.add(s0 / (B * B), s1 / (B * B));
                ab.add(t0 / (B * B), t1 / (B * B));
            }
        set_test_result(std::format(
            "halves {}x{} (right at x {}): non-zero left {:.2f} % right {:.2f} %; colour mean left {:.4f} right {:.4f}, mean |difference| {:.4f}, "
            "correlation {:.3f} (8x8 blocks {:.3f}); alpha mean left {:.4f} right {:.4f}, mean |difference| {:.4f}, correlation {:.3f} (8x8 blocks {:.3f})",
            w, H, right_x, 100.0 * a.nza / a.n, 100.0 * a.nzb / a.n, l.sa / l.n, l.sb / l.n, l.mad / l.n, l.corr(), lb.corr(), a.sa / a.n,
            a.sb / a.n, a.mad / a.n, a.corr(), ab.corr()));
    }).detach();
}
}  // namespace

void ssr_before_draw(ID3D11DeviceContext* ctx) {
    if (g_ssr_test.load(std::memory_order_relaxed) == 2 && g_ssr_last_target && g_ssr_index >= 2) {
        // Test 2 at the right view's composite: the first draw after both runs that reads the
        // target with its viewport away from the origin.
        D3D11_VIEWPORT vp{};
        UINT nvp = 1;
        ctx->RSGetViewports(&nvp, &vp);
        if (nvp && vp.TopLeftX > 0) {
            ID3D11ShaderResourceView* srvs[16]{};
            ctx->PSGetShaderResources(0, 16, srvs);
            bool reads = false;
            for (auto* v : srvs) {
                if (!v) continue;
                ID3D11Resource* r = nullptr;
                v->GetResource(&r);
                reads = reads || r == g_ssr_last_target;
                if (r) r->Release();
                v->Release();
            }
            if (reads) {
                g_ssr_test = 0;
                ssr_halves_test(ctx, g_ssr_last_target, static_cast<UINT>(vp.TopLeftX));
            }
        }
    }
    if (!g_ssr_pending.target) return;
    ID3D11ShaderResourceView* srvs[16]{};
    ctx->PSGetShaderResources(0, 16, srvs);
    bool reads = false;
    for (auto* v : srvs) {
        if (!v) continue;
        if (!reads) {
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            reads = r == g_ssr_pending.target;
            if (r) r->Release();
        }
        v->Release();
    }
    if (!reads) return;
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    const UINT x = nvp ? static_cast<UINT>(vp.TopLeftX) : 0;
    if (x > 0 && x < g_ssr_pending.width) {
        g_ssr_right_x = x;
        g_ssr_right_x_width = g_ssr_pending.width;
        if (x != g_ssr_pending.x) {
            copy_ssr_result(ctx, g_ssr_pending, x);
            g_ssr_last_x = static_cast<int>(x);
            ++g_ssr_moved;
        }
    }
    if (g_ssr_poison.load(std::memory_order_relaxed) == 2) {
        // Test: fill the right view's own part with a loud colour (it must show in that eye).
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
            const D3D11_RECT r{static_cast<LONG>(g_ssr_last_x.load()), 0, static_cast<LONG>(g_ssr_pending.width),
                               static_cast<LONG>(g_ssr_pending.height)};
            const float magenta[4] = {50.0f, 0.0f, 50.0f, 1.0f};
            ctx1->ClearView(g_ssr_pending.rtv, magenta, &r, 1);
            ctx1->Release();
        }
    }
    clear_ssr_pending();
}

void set_ssr_per_eye(bool on) {
    static bool registered = false;
    if (!registered) {
        registered = true;
        gpu_trace::set_copy_resource_override(&ssr_copy);
    }
    g_ssr_on = on;
    log::info("fixes: reflections per eye {}", on ? "on" : "off");
}
bool ssr_per_eye() { return g_ssr_on.load(); }
void set_ssr_poison(int mode) { g_ssr_poison = mode; }
void set_ssr_fix(int mode) {
    mode = std::clamp(mode, 0, 3);
    g_ssr_fix = mode;
    log::info("fixes: right-eye reflections fix {}", mode == 0   ? "off ([stereo] ssr_fix = 0)"
                                                     : mode == 1 ? "on, moved into place ([stereo] ssr_fix = 1)"
                                                     : mode == 2 ? "replaced: no screen-space reflections in either eye ([stereo] ssr_fix = 2)"
                                                                 : "on, drawn in place with the patched shader ([stereo] ssr_fix = 3)");
}
bool ssr_fix() { return g_ssr_fix.load() != 0; }
int ssr_fix_mode() { return g_ssr_fix.load(); }
std::string ssr_test(const std::string& what) {
    if (what == "equal") g_ssr_test = 1;
    else if (what == "halves") g_ssr_test = 2;
    else if (what == "shift") g_ssr_test = 3;
    else if (what != "result") return "err usage: ssr test equal | halves | shift | result";
    std::lock_guard lk(g_ssr_test_mutex);
    return std::string(what == "result" ? "ok " : "ok armed for the next frame; last result: ") + g_ssr_test_result;
}
bool ssr_wants_hooks() { return g_ssr_on.load(std::memory_order_relaxed) || g_ssr_fix.load(std::memory_order_relaxed) != 0; }

bool ssr_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base,
              void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT)) {
    const bool per_eye = g_ssr_on.load(std::memory_order_relaxed);
    const int fix_mode = g_ssr_fix.load(std::memory_order_relaxed);
    const bool fix = fix_mode == 1;
    if (count != 3 || (!per_eye && fix_mode == 0)) return false;
    ID3D11RenderTargetView* rtvs[2]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(2, rtvs, &dsv);
    const bool shape = rtvs[0] && !rtvs[1] && !dsv;
    D3D11_TEXTURE2D_DESC td{};
    if (rtvs[0]) {
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim{};
        if (r) r->GetType(&dim);
        if (r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
        if (r) r->Release();
    }
    for (auto* v : rtvs)
        if (v) v->Release();
    if (dsv) dsv->Release();
    if (!shape || (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && td.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS) ||
        td.Width * 2 < td.Height * 3 || td.Width < 128)
        return false;
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    if (nvp == 0 || vp.TopLeftX != 0.0f || vp.TopLeftY != 0.0f || vp.Width != static_cast<float>(td.Width) ||
        vp.Height != static_cast<float>(td.Height))
        return false;
    ID3D11ShaderResourceView* srvs[16]{};
    ctx->PSGetShaderResources(0, 16, srvs);
    bool hzb = false;
    for (auto* s : srvs)
        if (s && !hzb) hzb = is_hzb(s);
    const int index = hzb ? g_ssr_index++ : -1;
    if (index >= 0 && index < 2) {
        note_ssr_inputs(index, srvs, td);
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        g_ssr_ps = ps;  // compared and looked up only
        if (index == 0) {
            // Mode 3 is decided once per frame, for both runs, so that a frame without the
            // patched shader falls back to mode 2 in both eyes.
            if (ps != g_ssr_patched_of || !g_ssr_patched) {
                if (g_ssr_patched) g_ssr_patched->Release();
                g_ssr_patched = shaders::patched_for(ps);
                g_ssr_patched_of = ps;
            }
            g_ssr_frame_patched = g_ssr_patched != nullptr;
            ID3D11RenderTargetView* rtv = nullptr;
            ctx->OMGetRenderTargets(1, &rtv, nullptr);
            g_ssr_last_target = nullptr;
            if (rtv) {
                rtv->GetResource(&g_ssr_last_target);
                if (g_ssr_last_target) g_ssr_last_target->Release();  // compared only; the engine holds it
                rtv->Release();
            }
        } else if (ps != g_ssr_patched_of) {
            g_ssr_frame_patched = false;  // another shader than the first run's: not patched
        }
        if (ps) ps->Release();
    }
    for (auto* s : srvs)
        if (s) s->Release();
    if (!hzb) return false;
    if (index >= 2) {
        ++g_ssr_extra;
        return false;
    }
    if (fix_mode == 3 && !g_ssr_frame_patched) {
        ++g_ssr_no_patch;
        if (!g_ssr_no_patch_logged.exchange(true))
            log::warn("fixes: reflections: no patched shader for the reflection pass; both eyes without screen-space reflections (as ssr_fix = 2)");
    }
    if (fix_mode == 2 || (fix_mode == 3 && !g_ssr_frame_patched)) {
        // Both eyes without screen-space reflections: the run's target is cleared instead of
        // drawn, so the composite adds nothing in either eye (the colour copies stay as they are).
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        ID3D11DeviceContext1* ctx1 = nullptr;
        bool cleared = false;
        if (rtv && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
            const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            ctx1->ClearView(rtv, zero, nullptr, 0);
            ctx1->Release();
            cleared = true;
        }
        if (rtv) rtv->Release();
        if (cleared) {
            ++g_ssr_cleared;
            return true;
        }
        return false;
    }
    // Views are rendered left eye first; `stereo swap` puts the left eye into the right half.
    const bool right_half = (index == 1) != device::settings().swap_rects.load();
    const LONG half = static_cast<LONG>(td.Width / 2);
    const LONG margin = static_cast<LONG>(kSsrMargin);
    const LONG width = static_cast<LONG>(td.Width);
    // The run's part, a few columns past the middle (kSsrMargin), and the rest.
    D3D11_RECT rect{right_half ? half - margin : 0, 0, right_half ? width : half + margin, static_cast<LONG>(td.Height)};
    const D3D11_RECT rest{right_half ? 0 : half + margin, 0, right_half ? half - margin : width, static_cast<LONG>(td.Height)};
    const LONG right_x = (g_ssr_right_x_width == td.Width && g_ssr_right_x > 0) ? static_cast<LONG>(g_ssr_right_x) : half;

    ID3D11RasterizerState* engine_rs = nullptr;
    ctx->RSGetState(&engine_rs);
    D3D11_RASTERIZER_DESC ed{};
    if (engine_rs) engine_rs->GetDesc(&ed);
    UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ctx->RSGetScissorRects(&nsc, saved);

    if (fix_mode == 3 && right_half && g_ssr_patched) {
        // The run of the view in the right half, in place with the patched shader: limited to
        // that half (a few columns past the middle for a rounded rectangle). What it writes left
        // of the right view is never read: the left view's composite has run already.
        D3D11_RECT r = rect;
        if (engine_rs && ed.ScissorEnable && nsc > 0) {
            r.left = std::max(r.left, saved[0].left);
            r.top = std::max(r.top, saved[0].top);
            r.right = std::min(r.right, saved[0].right);
            r.bottom = std::min(r.bottom, saved[0].bottom);
        }
        ID3D11RasterizerState* rs = engine_rs && ed.ScissorEnable ? engine_rs : scissor_state(ctx, engine_rs);
        if (rs && r.right > r.left && r.bottom > r.top) {
            ID3D11PixelShader* engine_ps = nullptr;
            ID3D11ClassInstance* inst[16]{};
            UINT ninst = 16;
            ctx->PSGetShader(&engine_ps, inst, &ninst);
            ctx->RSSetState(rs);
            ctx->RSSetScissorRects(1, &r);
            ctx->PSSetShader(g_ssr_patched, nullptr, 0);
            original(ctx, count, start, base);
            ctx->PSSetShader(engine_ps, inst, ninst);
            if (g_ssr_test.load(std::memory_order_relaxed) == 3) {
                g_ssr_test = 0;
                ssr_reference_test(ctx, count, start, base, original, engine_ps);
            }
            if (engine_ps) engine_ps->Release();
            for (UINT i = 0; i < ninst && i < 16; ++i)
                if (inst[i]) inst[i]->Release();
            if (const int pm = g_ssr_poison.load(std::memory_order_relaxed); pm == 1 || pm == 2) {
                // Test: 1 fills everything left of this run's part with a loud colour (must not
                // show: the right view's composite reads only its own part), 2 fills this run's
                // part (control: must show in the right eye).
                ID3D11RenderTargetView* rtv = nullptr;
                ctx->OMGetRenderTargets(1, &rtv, nullptr);
                ID3D11DeviceContext1* ctx1 = nullptr;
                if (rtv && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
                    const D3D11_RECT other = pm == 2 ? r : D3D11_RECT{0, 0, r.left, static_cast<LONG>(td.Height)};
                    const float magenta[4] = {50.0f, 0.0f, 50.0f, 1.0f};
                    if (other.right > other.left) ctx1->ClearView(rtv, magenta, &other, 1);
                    ctx1->Release();
                }
                if (rtv) rtv->Release();
            }
            ctx->RSSetState(engine_rs);
            ctx->RSSetScissorRects(nsc, nsc ? saved : nullptr);
            if (engine_rs) engine_rs->Release();
            ++g_ssr_inplace;
            return true;
        }
    }
    if (fix && right_half) {
        // The right view's run writes at the origin: run it into the scratch target and
        // copy the result into the right half. An engine scissor moves with it.
        const bool engine_scissor = engine_rs && ed.ScissorEnable && nsc > 0;
        if (engine_scissor) {
            const D3D11_RECT shifted{saved[0].left - right_x, saved[0].top, saved[0].right - right_x, saved[0].bottom};
            ctx->RSSetScissorRects(1, &shifted);
        }
        const bool done = ssr_draw_shifted(ctx, count, start, base, original);
        if (engine_scissor) ctx->RSSetScissorRects(nsc, saved);
        if (done) {
            if (g_ssr_poison.load(std::memory_order_relaxed) == 1) {
                // Test: fill the part left of the right view, which this run no longer writes,
                // with a loud colour (mode 2, the right view's own part, is filled once the
                // result is in place: ssr_before_draw).
                ID3D11RenderTargetView* rtv = nullptr;
                ctx->OMGetRenderTargets(1, &rtv, nullptr);
                ID3D11DeviceContext1* ctx1 = nullptr;
                if (rtv && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
                    const D3D11_RECT other{0, 0, std::min(half, static_cast<LONG>(g_ssr_last_x.load())), static_cast<LONG>(td.Height)};
                    const float magenta[4] = {50.0f, 0.0f, 50.0f, 1.0f};
                    ctx1->ClearView(rtv, magenta, &other, 1);
                    ctx1->Release();
                }
                if (rtv) rtv->Release();
            }
            if (engine_rs) engine_rs->Release();
            ++g_ssr_limited;
            return true;
        }
    }
    if (!per_eye) {
        if (engine_rs) engine_rs->Release();
        return false;
    }
    if (engine_rs && ed.ScissorEnable && nsc > 0) {
        // The engine already scissors this draw: keep the overlap.
        rect.left = std::max(rect.left, saved[0].left);
        rect.top = std::max(rect.top, saved[0].top);
        rect.right = std::min(rect.right, saved[0].right);
        rect.bottom = std::min(rect.bottom, saved[0].bottom);
    }
    ID3D11RasterizerState* rs = engine_rs && ed.ScissorEnable ? engine_rs : scissor_state(ctx, engine_rs);
    if (!rs || rect.right <= rect.left || rect.bottom <= rect.top) {
        if (engine_rs) engine_rs->Release();
        return false;
    }
    ctx->RSSetState(rs);
    ctx->RSSetScissorRects(1, &rect);
    original(ctx, count, start, base);
    if (!right_half && g_ssr_test.load(std::memory_order_relaxed) == 1) {
        g_ssr_test = 0;
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ID3D11PixelShader* patched = shaders::patched_for(ps);
        if (ps) ps->Release();
        if (patched) {
            ssr_equivalence_test(ctx, count, start, base, original, patched);
            patched->Release();
        } else {
            set_test_result("err no patched shader for the reflection pass");
        }
    }
    if (const int pm = g_ssr_poison.load(std::memory_order_relaxed); pm == 1 || pm == 2) {
        // Test: fill the half this run skipped with a loud colour. If a later pass read the
        // reflections outside its own eye's half, it would show in the eye images.
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (rtv && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
            const D3D11_RECT other = pm == 2 ? rect : rest;
            const float magenta[4] = {50.0f, 0.0f, 50.0f, 1.0f};
            ctx1->ClearView(rtv, magenta, &other, 1);
            ctx1->Release();
        }
        if (rtv) rtv->Release();
    }
    ctx->RSSetState(engine_rs);
    ctx->RSSetScissorRects(nsc, nsc ? saved : nullptr);
    if (engine_rs) engine_rs->Release();
    ++g_ssr_limited;
    return true;
}

// ------------------------------------------------------------------ unread hierarchical depth chain
namespace {
std::atomic<int> g_hzb_mode{0};
// HZB occlusion culling (r.HZBOcclusion 1) reads a hierarchical depth chain; the game runs with
// 0. Checked on the game thread every 120 frames; while it is not 0 nothing is left out.
std::atomic<bool> g_hzb_occlusion_off{false};
unsigned g_hzb_check = 0;  // game thread
std::atomic<std::uint64_t> g_hzb_chains{0}, g_hzb_skipped{0};
ID3D11Resource* g_hzb_furthest = nullptr;  // RHI thread, compared only: the chain of the view being built
ID3D11Resource* g_hzb_closest = nullptr;

bool hzb_texture(ID3D11RenderTargetView* v, ID3D11Resource** res, UINT* mip) {
    *res = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    v->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    bool ok = false;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        ok = (d.Format == DXGI_FORMAT_R16_FLOAT || d.Format == DXGI_FORMAT_R16_TYPELESS) && d.MipLevels >= 4 && d.ArraySize == 1;
    }
    *res = r;
    r->Release();  // compared only; the engine holds it
    *mip = vd.Texture2D.MipSlice;
    return ok;
}

void hzb_fill(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, float value) {
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
        const float v[4] = {value, value, value, value};
        ctx1->ClearView(rtv, v, nullptr, 0);
        ctx1->Release();
    }
}
}  // namespace

void set_hzb_skip(int mode) {
    g_hzb_mode = std::clamp(mode, 0, 4);
    log::info("fixes: unread hierarchical depth chain: {}", mode == 0 ? "built (game default)" : mode == 1 ? "mips 1+ not built" : "test fill");
}
int hzb_skip() { return g_hzb_mode.load(); }

void hzb_tick() {
    if (g_hzb_mode.load(std::memory_order_relaxed) == 0 || g_hzb_check-- != 0) return;
    g_hzb_check = 120;
    const auto v = cvar::get(L"r.HZBOcclusion");
    const bool off = v && v->i == 0;
    if (off != g_hzb_occlusion_off.exchange(off))
        log::info("fixes: r.HZBOcclusion {}: the unread hierarchical depth chain is {}", v ? v->i : -1,
                  off ? "left out (hzb_skip)" : "built (HZB occlusion may read it)");
}

bool hzb_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT)) {
    const int mode = g_hzb_mode.load(std::memory_order_relaxed);
    if (mode == 0 || count != 3 || !g_hzb_occlusion_off.load(std::memory_order_relaxed)) return false;
    ID3D11RenderTargetView* rtvs[2]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(2, rtvs, &dsv);
    bool done = false;
    ID3D11Resource *r0 = nullptr, *r1 = nullptr;
    UINT m0 = 0, m1 = 0;
    if (!dsv && rtvs[0] && hzb_texture(rtvs[0], &r0, &m0)) {
        if (rtvs[1] && hzb_texture(rtvs[1], &r1, &m1) && m0 == 0 && m1 == 0 && r0 != r1) {
            // Mip 0 of both chains of one view (one draw, two targets): the first target is the
            // chain nothing reads (docs/engine-module.md, "Hierarchical depth").
            g_hzb_furthest = r0;
            g_hzb_closest = r1;
            ++g_hzb_chains;
            if (mode == 2) {  // test: the whole chain filled with near depth instead
                original(ctx, count, start, base);
                hzb_fill(ctx, rtvs[0], 1.0f);
                done = true;
            }
        } else if (!rtvs[1] && m0 >= 1) {
            if (r0 == g_hzb_furthest) {
                if (mode == 2 || mode == 3) hzb_fill(ctx, rtvs[0], mode == 2 ? 1.0f : 0.0f);
                if (mode != 4) {
                    ++g_hzb_skipped;
                    done = true;
                }
            } else if (r0 == g_hzb_closest && mode == 4) {
                // Control: the chain that is read, filled with near depth (must show in the picture).
                hzb_fill(ctx, rtvs[0], 1.0f);
                done = true;
            }
        }
    }
    for (auto* v : rtvs)
        if (v) v->Release();
    if (dsv) dsv->Release();
    return done;
}

std::string hzb_status() {
    return std::format("hierarchical depth: mode {} ({}), r.HZBOcclusion {}, view chains seen {}, mips not built {}", g_hzb_mode.load(),
                       g_hzb_mode.load() == 0 ? "off" : g_hzb_mode.load() == 1 ? "skip" : "test",
                       g_hzb_occlusion_off.load() ? "0" : "not 0 or not read yet (nothing left out)", g_hzb_chains.load(), g_hzb_skipped.load());
}

void ssr_frame() {
    g_hzb_furthest = g_hzb_closest = nullptr;
    if (g_ssr_index) ++g_ssr_frames;
    g_ssr_index = 0;
    for (auto& v : g_ssr_inputs)
        for (auto& r : v) r = nullptr;
    if (g_ssr_pending.target) {  // no draw read the result this frame (it stays where it was copied)
        ++g_ssr_unread;
        clear_ssr_pending();
    }
    // The scratch target goes after about ten seconds without a reflection run to fix.
    if (g_ssr_scratch.tex && ++g_ssr_scratch.idle_frames > 900) {
        release_ssr_scratch();
        log::info("fixes: right-eye reflections scratch target released (unused)");
    }
}

std::string ssr_shader_dump(const std::string& prefix) {
    ID3D11PixelShader* ps = g_ssr_ps.load();
    if (!ps) return "err no reflection run seen yet";
    return shaders::dump(ps, prefix);
}

std::string ssr_status() {
    return std::format("reflections per eye {}: runs limited {} in {} frames, extra runs left alone {}, colour copies halved {}; "
                       "right-eye fix {}: applied {} failed {} at x {} (moved {}, not read {}); runs cleared (ssr_fix 2) {}; "
                       "drawn in place with the patched shader (ssr_fix 3) {}, frames without it {}",
                       g_ssr_on.load() ? "on" : "off", g_ssr_limited.load(), g_ssr_frames.load(), g_ssr_extra.load(), g_ssr_copies_halved.load(),
                       g_ssr_fix.load() == 0   ? "off"
                       : g_ssr_fix.load() == 1 ? "on (moved into place)"
                       : g_ssr_fix.load() == 2 ? "replaced (both eyes without)"
                                               : "on (patched shader, in place)",
                       g_ssr_fixed.load(), g_ssr_fix_failed.load(), g_ssr_last_x.load(), g_ssr_moved.load(), g_ssr_unread.load(), g_ssr_cleared.load(),
                       g_ssr_inplace.load(), g_ssr_no_patch.load());
}

}  // namespace ff7vr::engine::fixes
