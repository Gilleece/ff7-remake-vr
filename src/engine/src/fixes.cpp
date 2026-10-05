#include "fixes.h"

#include "engine_internal.h"

#include "stereo_device.h"

#include <d3d11_1.h>

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/engine/cvars.h"

#include <algorithm>
#include <atomic>
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
}  // namespace

void set_ssr_per_eye(bool on) {
    g_ssr_on = on;
    log::info("fixes: reflections per eye {}", on ? "on" : "off");
}
bool ssr_per_eye() { return g_ssr_on.load(); }
void set_ssr_poison(int mode) { g_ssr_poison = mode; }

bool ssr_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base,
              void(STDMETHODCALLTYPE* original)(ID3D11DeviceContext*, UINT, UINT, INT)) {
    if (count != 3 || !g_ssr_on.load(std::memory_order_relaxed)) return false;
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
    for (auto* s : srvs) {
        if (s && !hzb) hzb = is_hzb(s);
        if (s) s->Release();
    }
    if (!hzb) return false;
    const int index = g_ssr_index++;
    if (index >= 2) {
        ++g_ssr_extra;
        return false;
    }
    // Views are rendered left eye first; `stereo swap` puts the left eye into the right half.
    const bool right_half = (index == 1) != device::settings().swap_rects.load();
    const LONG half = static_cast<LONG>(td.Width / 2);
    D3D11_RECT rect{right_half ? half : 0, 0, right_half ? static_cast<LONG>(td.Width) : half, static_cast<LONG>(td.Height)};

    ID3D11RasterizerState* engine_rs = nullptr;
    ctx->RSGetState(&engine_rs);
    D3D11_RASTERIZER_DESC ed{};
    if (engine_rs) engine_rs->GetDesc(&ed);
    UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ctx->RSGetScissorRects(&nsc, saved);
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
    if (g_ssr_poison.load(std::memory_order_relaxed)) {
        // Test: fill the half this run skipped with a loud colour. If a later pass read the
        // reflections outside its own eye's half, it would show in the eye images.
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (rtv && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
            const bool own = g_ssr_poison.load() == 2;
            const bool left = own ? !right_half : right_half;
            const D3D11_RECT other{left ? 0 : half, 0, left ? half : static_cast<LONG>(td.Width), static_cast<LONG>(td.Height)};
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

void ssr_frame() {
    if (g_ssr_index) ++g_ssr_frames;
    g_ssr_index = 0;
}

std::string ssr_status() {
    return std::format("reflections per eye {}: runs limited {} in {} frames, extra runs left alone {}", g_ssr_on.load() ? "on" : "off",
                       g_ssr_limited.load(), g_ssr_frames.load(), g_ssr_extra.load());
}

}  // namespace ff7vr::engine::fixes
