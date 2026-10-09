#include "scene_depth.h"

#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/core/pattern.h"

#include <windows.h>

#include <atomic>
#include <format>

namespace ff7vr::engine::scene_depth {
namespace {

// FDeferredShadingSceneRenderer::Render's UI loop: `lea rcx, [GSceneRenderTargets]` is the
// first argument of FSceneRenderTargets::BeginRenderingInGameUI (tools/re/signatures.json,
// "GSceneRenderTargets").
constexpr const char* kPattern = "48 8D 0D ?? ?? ?? ?? 4C 69 C0 B0 28 00 00 48 8B D6 4C 03 87 D0 00 00 00 E8 ?? ?? ?? ?? 84 C0";
constexpr std::uintptr_t kExpectRva = 0x5923ad0;
// How far into FSceneRenderTargets members are looked at (pointer-sized steps).
constexpr std::size_t kScanBytes = 0x800;
// FD3D11Texture2D: the native ID3D11Resource* (= what vtable slot 7, GetNativeResource,
// returns; stereo_abi.h FD3D11Texture2D_Resource).
constexpr std::size_t kNativeResource = 0xA0;

std::uint8_t* g_targets = nullptr;  // GSceneRenderTargets
std::atomic<int> g_offset{-1};      // member offset of the scene depth (render thread writes)
std::uint64_t g_found = 0, g_lost = 0, g_failures = 0, g_next_warn = 1;  // render thread
std::atomic<std::uint64_t> g_acquired{0};
DXGI_FORMAT g_last_format = DXGI_FORMAT_UNKNOWN;
std::uint32_t g_last_w = 0, g_last_h = 0;

std::uintptr_t g_exe_lo = 0, g_exe_hi = 0;  // the game image (set by init)
bool in_exe(std::uintptr_t a) { return a >= g_exe_lo && a < g_exe_hi; }
bool in_system_module(std::uintptr_t a) { return !in_exe(a) && module::from_address(a); }

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<std::uintptr_t>(p) + n <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// The native resource behind a pooled render target's targetable texture, or null. Only
// for an RHI texture of the same class as the eye texture (its vtable `rhi_vtable`), so no
// member that is something else is ever used. Every pointer is checked with VirtualQuery
// before it is read: an access violation, even a handled one, would reach the crash
// handler's first-chance report.
void* native_of_pooled(void* pooled, const void* rhi_vtable) {
    if (!pooled || !readable(pooled, 0x18)) return nullptr;
    void* rhi = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pooled) + 0x08);
    if (!rhi || !readable(rhi, kNativeResource + 8) || *static_cast<void**>(rhi) != rhi_vtable) return nullptr;
    void* native = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(rhi) + kNativeResource);
    return native && readable(native, 8) ? native : nullptr;
}

// A D3D11 texture 2D (referenced) behind `native` (readable), or null.
ID3D11Texture2D* as_texture(void* native) {
    if (!native) return nullptr;
    const std::uintptr_t vt = *static_cast<std::uintptr_t*>(native);
    if (!in_system_module(vt)) return nullptr;  // COM objects live in system modules
    ID3D11Texture2D* t = nullptr;
    if (FAILED(static_cast<IUnknown*>(native)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t)))) return nullptr;
    return t;
}

DXGI_FORMAT depth_srv_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The scene depth at member offset `off` for a width x height scene (referenced), or null.
// The texture found last frame and the pointer chain it was found through: while the
// chain is unchanged the pooled target still holds it, and nothing is queried again.
struct Cached {
    void* pooled = nullptr;
    void* rhi = nullptr;
    void* native = nullptr;
    ID3D11Texture2D* texture = nullptr;  // not referenced: alive while the chain is unchanged
    D3D11_TEXTURE2D_DESC desc{};
    std::uint32_t w = 0, h = 0;
};
Cached g_cache;  // render thread

ID3D11Texture2D* depth_at(std::size_t off, std::uint32_t w, std::uint32_t h, const void* rhi_vtable, D3D11_TEXTURE2D_DESC* out) {
    void* pooled = *reinterpret_cast<void**>(g_targets + off);
    if (pooled && pooled == g_cache.pooled && g_cache.w == w && g_cache.h == h &&
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pooled) + 0x08) == g_cache.rhi &&
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(g_cache.rhi) + kNativeResource) == g_cache.native) {
        g_cache.texture->AddRef();
        if (out) *out = g_cache.desc;
        return g_cache.texture;
    }
    ID3D11Texture2D* t = as_texture(native_of_pooled(pooled, rhi_vtable));
    if (!t) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    const bool ok = (d.BindFlags & D3D11_BIND_DEPTH_STENCIL) && (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && d.SampleDesc.Count == 1 &&
                    d.Width >= w && d.Height >= h && d.Width <= w + 64 && d.Height <= h + 64 && depth_srv_format(d.Format) != DXGI_FORMAT_UNKNOWN;
    if (!ok) {
        t->Release();
        return nullptr;
    }
    if (out) *out = d;
    return t;
}

// Remembers the texture at member `off` (just found there) for the next frames.
void remember(std::size_t off, ID3D11Texture2D* t, const D3D11_TEXTURE2D_DESC& d, std::uint32_t w, std::uint32_t h) {
    void* pooled = *reinterpret_cast<void**>(g_targets + off);
    void* rhi = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pooled) + 0x08);
    g_cache = Cached{pooled, rhi, *reinterpret_cast<void**>(static_cast<std::uint8_t*>(rhi) + kNativeResource), t, d, w, h};
}

void warn(const std::string& what) {
    if (++g_failures >= g_next_warn) {
        g_next_warn *= 2;
        log::warn("scene depth: {} ({} times); no depth layer for these frames", what, g_failures);
    }
}

}  // namespace

void init(std::uintptr_t base, bool known) {
    {
        const module::Info exe = module::main_module();
        g_exe_lo = exe.base;
        g_exe_hi = exe.end();
    }
    const auto r = pattern::scan_module(base, kPattern, pattern::Sections::Executable, 2);
    if (!r.unique()) {
        log::warn("scene depth: GSceneRenderTargets: {} matches; no depth layer", r.matches.size());
        return;
    }
    const std::uintptr_t addr = pattern::rip(r.first(), 3, 7);
    if (known && addr - base != kExpectRva) {
        log::warn("scene depth: GSceneRenderTargets at 0x{:x}, expected 0x{:x} for this build; no depth layer", addr - base, kExpectRva);
        return;
    }
    g_targets = reinterpret_cast<std::uint8_t*>(addr);
    log::info("scene depth: GSceneRenderTargets at +0x{:x}", addr - base);
}

ID3D11Texture2D* acquire(std::uint32_t w, std::uint32_t h, const void* rhi_vtable, DXGI_FORMAT* srv_format) {
    if (!g_targets || !w || !h || !rhi_vtable || !in_exe(reinterpret_cast<std::uintptr_t>(rhi_vtable))) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    ID3D11Texture2D* t = nullptr;
    const int known = g_offset.load(std::memory_order_relaxed);
    if (known >= 0) t = depth_at(static_cast<std::size_t>(known), w, h, rhi_vtable, &d);
    if (t && g_cache.texture != t) remember(static_cast<std::size_t>(known), t, d, w, h);
    if (!t) {
        g_cache = Cached{};
        if (known >= 0) ++g_lost;
        if (!readable(g_targets, kScanBytes)) {
            warn("the scene render targets are not readable");
            return nullptr;
        }
        std::string all;
        int first = -1;
        for (std::size_t off = 0; off < kScanBytes; off += 8) {
            D3D11_TEXTURE2D_DESC c{};
            if (ID3D11Texture2D* x = depth_at(off, w, h, rhi_vtable, &c)) {
                if (first < 0) {
                    first = static_cast<int>(off);
                    t = x;
                    d = c;
                } else {
                    x->Release();
                }
                all += std::format(" +0x{:x} ({}x{} format {})", off, c.Width, c.Height, static_cast<int>(c.Format));
            }
        }
        if (first < 0) {
            warn(std::format("no depth-stencil target of {}x{} among the scene render targets", w, h));
            return nullptr;
        }
        g_offset = first;
        remember(static_cast<std::size_t>(first), t, d, w, h);
        ++g_found;
        log::info("scene depth: member +0x{:x} of the scene render targets, {}x{} DXGI format {} (depth-stencil targets of that size:{})", first,
                  d.Width, d.Height, static_cast<int>(d.Format), all);
    }
    g_last_format = d.Format;
    g_last_w = d.Width;
    g_last_h = d.Height;
    ++g_acquired;
    if (srv_format) *srv_format = depth_srv_format(d.Format);
    return t;
}

std::string status() {
    return std::format("scene depth: render targets {}, member {}, last {}x{} DXGI format {}, handed over {} times, searches {}, lost {}, failures {}",
                       g_targets ? "found" : "not found", g_offset.load() >= 0 ? std::format("+0x{:x}", g_offset.load()) : std::string("not found"),
                       g_last_w, g_last_h, static_cast<int>(g_last_format), g_acquired.load(), g_found, g_lost, g_failures);
}

}  // namespace ff7vr::engine::scene_depth
