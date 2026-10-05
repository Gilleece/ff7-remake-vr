#include "gpu_trace.h"

#include "engine_internal.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <d3d11.h>
#include <windows.h>

#include <DirectXPackedVector.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ff7vr::engine::gpu_trace {
namespace {

// ------------------------------------------------------------------ pooled render target names
// FRenderTargetPool::FindFreeElement(RHICmdList, const FPooledRenderTargetDesc&,
// TRefCountPtr<IPooledRenderTarget>& Out, const TCHAR* DebugName, ...), render thread.
// IPooledRenderTarget: vtable, then FSceneRenderTargetItem {TargetableTexture +0x08,
// ShaderResourceTexture +0x10}. FRHITexture vtable slot 7 is GetNativeResource().
constexpr std::size_t kNativeResourceSlot = 7;

using FindFreeElementFn = std::uint64_t(__fastcall*)(void*, void*, void*, void**, const wchar_t*, std::uint64_t,
                                                      std::uint64_t, std::uint64_t);
hook::InlineHook g_find_hook;
std::mutex g_names_mutex;
std::unordered_map<void*, std::string> g_names;  // native resource -> latest pool names, newest first

void* native_of(void* rhi_texture) {
    __try {
        if (!rhi_texture) return nullptr;
        auto fn = reinterpret_cast<void*(__fastcall*)(void*)>((*static_cast<void***>(rhi_texture))[kNativeResourceSlot]);
        return fn(rhi_texture);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool pooled_textures(void** out_ref, void** targetable, void** shader_resource) {
    __try {
        void* pooled = out_ref ? *out_ref : nullptr;
        if (!pooled) return false;
        *targetable = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pooled) + 0x08);
        *shader_resource = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pooled) + 0x10);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void remember_name(void* native, const wchar_t* name) {
    if (!native || !name) return;
    std::string n;
    for (const wchar_t* p = name; *p && n.size() < 64; ++p) n += (*p < 128 && *p > 32) ? static_cast<char>(*p) : '_';
    std::lock_guard lock(g_names_mutex);
    std::string& s = g_names[native];
    if (s == n || s.rfind(n + "|", 0) == 0) return;
    s = s.empty() ? n : n + "|" + s;
    if (s.size() > 120) s.resize(120);
}

std::uint64_t __fastcall find_free_element_detour(void* self, void* cmd, void* desc, void** out, const wchar_t* name,
                                                  std::uint64_t a6, std::uint64_t a7, std::uint64_t a8) {
    const std::uint64_t r = g_find_hook.original<FindFreeElementFn>()(self, cmd, desc, out, name, a6, a7, a8);
    void* t = nullptr;
    void* s = nullptr;
    if (pooled_textures(out, &t, &s)) {
        try {
            remember_name(native_of(t), name);
            if (s != t) remember_name(native_of(s), name);
        } catch (...) {
        }
    }
    return r;
}

std::string name_of(void* native) {
    std::lock_guard lock(g_names_mutex);
    auto it = g_names.find(native);
    return it == g_names.end() ? std::string() : it->second;
}

// ------------------------------------------------------------------ trace state
enum class State : int { Idle, Armed, Recording };
std::atomic<State> g_state{State::Idle};
std::string g_prefix;                      // set while Idle, read on the RHI thread
std::uint32_t g_dump_from = 1, g_dump_to = 0;  // read-back range (seq), empty by default
bool g_dump_fullscreen = false;  // read back every full-screen pass (at most 6 vertices, target at least 512 wide)
std::uint32_t g_dump_scale = 2;
std::string g_last_result = "no trace yet";
std::mutex g_result_mutex;

// ID3D11DeviceContext vtable slots (d3d11.h order).
enum Slot : std::size_t {
    kDrawIndexed = 12,
    kDraw = 13,
    kDrawIndexedInstanced = 20,
    kDrawInstanced = 21,
    kDispatch = 41,
    kCopySubresourceRegion = 46,
    kCopyResource = 47,
    kClearRenderTargetView = 50,
    kClearUnorderedAccessViewFloat = 52,
    kClearDepthStencilView = 53,
    kResolveSubresource = 57,
};

// Inline hooks on the functions the context's vtable points to, so calls through any
// interface pointer of the context are seen.
struct Hooks {
    hook::InlineHook draw_indexed, draw, draw_indexed_instanced, draw_instanced, dispatch, copy_region, copy, clear_rtv,
        clear_uav_float, clear_dsv, resolve;
};
Hooks* g_hooks = new Hooks();  // never destroyed (slots are restored explicitly)

ID3D11DeviceContext* g_ctx = nullptr;  // RHI thread while recording
ID3D11Device* g_dev = nullptr;
std::uint32_t g_seq = 0;
std::vector<std::string> g_lines;
std::vector<ID3D11Query*> g_stamps;  // one per event, plus the start
ID3D11Query* g_disjoint = nullptr;
std::uint32_t g_dumps = 0;
std::uint32_t g_dump_failures = 0;
bool g_in_hook = false;  // our own read-backs call the context too

struct TexInfo {
    UINT w = 0, h = 0, mips = 0, array = 0, samples = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool tex2d = false;
    UINT buffer_bytes = 0;
};

TexInfo info_of(ID3D11Resource* r) {
    TexInfo t;
    if (!r) return t;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        t.w = d.Width;
        t.h = d.Height;
        t.mips = d.MipLevels;
        t.array = d.ArraySize;
        t.samples = d.SampleDesc.Count;
        t.fmt = d.Format;
        t.tex2d = true;
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        t.buffer_bytes = d.ByteWidth;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d{};
        static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
        t.w = d.Width;
        t.h = d.Height;
        t.array = d.Depth;
        t.fmt = d.Format;
    }
    return t;
}

std::string describe(ID3D11Resource* r, UINT mip = 0) {
    if (!r) return "-";
    const TexInfo t = info_of(r);
    std::string s;
    if (t.buffer_bytes) s = std::format("{} buf{}", static_cast<void*>(r), t.buffer_bytes);
    else s = std::format("{} {}x{} f{}{}{}", static_cast<void*>(r), t.w, t.h, static_cast<int>(t.fmt), t.mips > 1 ? std::format(" m{}", mip) : "",
                         t.array > 1 ? std::format(" a{}", t.array) : "");
    const std::string n = name_of(r);
    if (!n.empty()) s += " [" + n + "]";
    return s;
}

template <class View>
std::string describe_view(View* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    const std::string s = describe(r);
    if (r) r->Release();
    return s;
}

std::string state_summary(bool compute) {
    std::string s;
    ID3D11DeviceContext* c = g_ctx;
    if (!compute) {
        ID3D11RenderTargetView* rtv[8]{};
        ID3D11DepthStencilView* dsv = nullptr;
        c->OMGetRenderTargets(8, rtv, &dsv);
        for (int i = 0; i < 8; ++i) {
            if (!rtv[i]) continue;
            D3D11_RENDER_TARGET_VIEW_DESC d{};
            rtv[i]->GetDesc(&d);
            ID3D11Resource* r = nullptr;
            rtv[i]->GetResource(&r);
            s += std::format(" | rt{} {}", i, describe(r, d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? d.Texture2D.MipSlice : 0));
            if (r) r->Release();
            rtv[i]->Release();
        }
        if (dsv) {
            s += " | ds " + describe_view(dsv);
            dsv->Release();
        }
        UINT nvp = 1;
        D3D11_VIEWPORT vp{};
        c->RSGetViewports(&nvp, &vp);
        if (nvp) s += std::format(" | vp {} {} {} {}", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
        ID3D11PixelShader* ps = nullptr;
        ID3D11VertexShader* vs = nullptr;
        c->PSGetShader(&ps, nullptr, nullptr);
        c->VSGetShader(&vs, nullptr, nullptr);
        s += std::format(" | vs {} ps {}", static_cast<void*>(vs), static_cast<void*>(ps));
        if (ps) ps->Release();
        if (vs) vs->Release();
        ID3D11ShaderResourceView* srv[16]{};
        c->PSGetShaderResources(0, 16, srv);
        for (int i = 0; i < 16; ++i) {
            if (!srv[i]) continue;
            s += std::format(" | t{} {}", i, describe_view(srv[i]));
            srv[i]->Release();
        }
        ID3D11Buffer* cb[4]{};
        c->PSGetConstantBuffers(0, 4, cb);
        for (int i = 0; i < 4; ++i) {
            if (!cb[i]) continue;
            D3D11_BUFFER_DESC d{};
            cb[i]->GetDesc(&d);
            s += std::format(" | cb{} {} {}", i, static_cast<void*>(cb[i]), d.ByteWidth);
            cb[i]->Release();
        }
    } else {
        ID3D11ComputeShader* cs = nullptr;
        c->CSGetShader(&cs, nullptr, nullptr);
        s += std::format(" | cs {}", static_cast<void*>(cs));
        if (cs) cs->Release();
        ID3D11UnorderedAccessView* uav[8]{};
        c->CSGetUnorderedAccessViews(0, 8, uav);
        for (int i = 0; i < 8; ++i) {
            if (!uav[i]) continue;
            s += std::format(" | u{} {}", i, describe_view(uav[i]));
            uav[i]->Release();
        }
        ID3D11ShaderResourceView* srv[16]{};
        c->CSGetShaderResources(0, 16, srv);
        for (int i = 0; i < 16; ++i) {
            if (!srv[i]) continue;
            s += std::format(" | t{} {}", i, describe_view(srv[i]));
            srv[i]->Release();
        }
        ID3D11Buffer* cb[4]{};
        c->CSGetConstantBuffers(0, 4, cb);
        for (int i = 0; i < 4; ++i) {
            if (!cb[i]) continue;
            D3D11_BUFFER_DESC d{};
            cb[i]->GetDesc(&d);
            s += std::format(" | cb{} {} {}", i, static_cast<void*>(cb[i]), d.ByteWidth);
            cb[i]->Release();
        }
    }
    return s;
}

// ------------------------------------------------------------------ read-backs
float to_unit(float x) {
    if (!(x > 0)) return 0;
    x = x / (1.0f + x);
    return std::pow(x, 1.0f / 2.2f);
}

bool convert_row(const std::uint8_t* src, DXGI_FORMAT f, UINT w, std::uint8_t* dst) {
    using namespace DirectX::PackedVector;
    for (UINT x = 0; x < w; ++x) {
        float c[4]{0, 0, 0, 1};
        bool unorm = false;
        std::uint8_t u8[4]{};
        switch (f) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
                const auto* h = reinterpret_cast<const HALF*>(src) + 4 * x;
                for (int i = 0; i < 4; ++i) c[i] = XMConvertHalfToFloat(h[i]);
                break;
            }
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_TYPELESS: {
                const auto* p = reinterpret_cast<const float*>(src) + 4 * x;
                for (int i = 0; i < 4; ++i) c[i] = p[i];
                break;
            }
            case DXGI_FORMAT_R11G11B10_FLOAT: {
                XMFLOAT3PK pk;
                pk.v = reinterpret_cast<const std::uint32_t*>(src)[x];
                DirectX::XMFLOAT3 v;
                DirectX::XMStoreFloat3(&v, XMLoadFloat3PK(&pk));
                c[0] = v.x;
                c[1] = v.y;
                c[2] = v.z;
                break;
            }
            case DXGI_FORMAT_R16_FLOAT:
            case DXGI_FORMAT_R16_TYPELESS:
                c[0] = c[1] = c[2] = XMConvertHalfToFloat(reinterpret_cast<const HALF*>(src)[x]);
                break;
            case DXGI_FORMAT_R16G16_FLOAT: {
                const auto* h = reinterpret_cast<const HALF*>(src) + 2 * x;
                c[0] = XMConvertHalfToFloat(h[0]);
                c[1] = XMConvertHalfToFloat(h[1]);
                c[2] = 0;
                break;
            }
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R32_TYPELESS:
                c[0] = c[1] = c[2] = reinterpret_cast<const float*>(src)[x];
                break;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                std::memcpy(u8, src + 4 * x, 4);
                unorm = true;
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: {
                const std::uint8_t* p = src + 4 * x;
                u8[0] = p[2];
                u8[1] = p[1];
                u8[2] = p[0];
                u8[3] = p[3];
                unorm = true;
                break;
            }
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                const std::uint32_t v = reinterpret_cast<const std::uint32_t*>(src)[x];
                u8[0] = static_cast<std::uint8_t>((v & 0x3ff) >> 2);
                u8[1] = static_cast<std::uint8_t>(((v >> 10) & 0x3ff) >> 2);
                u8[2] = static_cast<std::uint8_t>(((v >> 20) & 0x3ff) >> 2);
                u8[3] = static_cast<std::uint8_t>(((v >> 30) & 3) * 85);
                unorm = true;
                break;
            }
            case DXGI_FORMAT_R8_UNORM:
            case DXGI_FORMAT_R8_TYPELESS:
                u8[0] = u8[1] = u8[2] = src[x];
                u8[3] = 255;
                unorm = true;
                break;
            default:
                return false;
        }
        std::uint8_t* d = dst + 4 * x;
        if (unorm) {
            std::memcpy(d, u8, 4);
        } else {
            for (int i = 0; i < 3; ++i) d[i] = static_cast<std::uint8_t>(std::clamp(to_unit(c[i]), 0.0f, 1.0f) * 255.0f + 0.5f);
            d[3] = static_cast<std::uint8_t>(std::clamp(c[3], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return true;
}

UINT bytes_per_pixel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 8;
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_TYPELESS: return 2;
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_TYPELESS: return 1;
        default: return 4;
    }
}

// Copies mip `mip` of a 2D texture (array slice 0) to the CPU and writes it, downscaled.
void dump(ID3D11Resource* r, UINT mip, std::uint32_t seq) {
    const TexInfo t = info_of(r);
    if (!t.tex2d || t.samples != 1) return;
    const UINT w = std::max(1u, t.w >> mip), h = std::max(1u, t.h >> mip);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = t.fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &staging)) || !staging) {
        ++g_dump_failures;
        return;
    }
    g_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, r, D3D11CalcSubresource(mip, 0, t.mips), nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        staging->Release();
        ++g_dump_failures;
        return;
    }
    const UINT s = std::max(1u, g_dump_scale);
    const UINT ow = w / s, oh = h / s;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(w) * 4);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(ow) * oh * 4);
    bool ok = ow && oh;
    for (UINT y = 0; ok && y < oh; ++y) {
        const auto* src = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y * s) * m.RowPitch;
        if (!convert_row(src, t.fmt, w, row.data())) {
            ok = false;
            break;
        }
        for (UINT x = 0; x < ow; ++x) std::memcpy(&out[(static_cast<std::size_t>(y) * ow + x) * 4], &row[static_cast<std::size_t>(x) * s * 4], 4);
    }
    g_ctx->Unmap(staging, 0);
    staging->Release();
    if (!ok) {
        ++g_dump_failures;
        return;
    }
    const std::string path = std::format("{}_{:05}.rgba", g_prefix, seq);
    std::ofstream f(path, std::ios::binary);
    const std::uint32_t hdr[4] = {0x31445447u /* GTD1 */, ow, oh, static_cast<std::uint32_t>(t.fmt)};
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (f) ++g_dumps;
    else ++g_dump_failures;
}

// Contents of a constant buffer (first `max_bytes`), as floats; releases cb.
std::string cb_contents(ID3D11Buffer* cb, UINT max_bytes) {
    if (!cb) return {};
    D3D11_BUFFER_DESC d{};
    cb->GetDesc(&d);
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = std::min<UINT>(d.ByteWidth, max_bytes);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr;
    std::string s;
    if (SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &st)) && st) {
        D3D11_BOX box{0, 0, 0, sd.ByteWidth, 1, 1};
        g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, cb, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const auto* f = static_cast<const float*>(m.pData);
            for (UINT i = 0; i < sd.ByteWidth / 4; ++i) s += std::format("{}{:g}", i % 4 == 0 ? (i ? " | " : "") : " ", f[i]);
            g_ctx->Unmap(st, 0);
        }
        st->Release();
    }
    cb->Release();
    return s;
}

// ------------------------------------------------------------------ event recording
void stamp() {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query* q = nullptr;
    if (SUCCEEDED(g_dev->CreateQuery(&qd, &q)) && q) g_ctx->End(q);
    g_stamps.push_back(q);
}

// Called before the original call; returns the event's sequence number.
std::uint32_t begin_event(const std::string& what, bool compute, bool with_state) {
    const std::uint32_t seq = ++g_seq;
    std::string line = std::format("{} {}", seq, what);
    if (with_state) line += state_summary(compute);
    g_lines.push_back(std::move(line));
    return seq;
}

// Called after the original call: timestamp, read-back of the output for events in range.
UINT output_width(bool compute) {
    UINT w = 0;
    ID3D11Resource* r = nullptr;
    if (compute) {
        ID3D11UnorderedAccessView* uav = nullptr;
        g_ctx->CSGetUnorderedAccessViews(0, 1, &uav);
        if (uav) {
            uav->GetResource(&r);
            uav->Release();
        }
    } else {
        ID3D11RenderTargetView* rtv = nullptr;
        g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            rtv->GetResource(&r);
            rtv->Release();
        }
    }
    if (r) {
        w = info_of(r).w;
        r->Release();
    }
    return w;
}

void end_event(std::uint32_t seq, bool compute, UINT vertices = 0) {
    stamp();
    bool wanted = seq >= g_dump_from && seq <= g_dump_to;
    if (!wanted && g_dump_fullscreen && (compute || vertices <= 6)) wanted = output_width(compute) >= 512;
    if (!wanted) return;
    std::string& line = g_lines.back();
    ID3D11Buffer* cbs[3]{};
    if (compute) {
        g_ctx->CSGetConstantBuffers(0, 1, cbs);
        const std::string cb = cb_contents(cbs[0], 1024);
        if (!cb.empty()) line += "\n    cs cb0: " + cb;
    } else {
        g_ctx->PSGetConstantBuffers(0, 1, cbs);
        const std::string cb = cb_contents(cbs[0], 1024);
        if (!cb.empty()) line += "\n    ps cb0: " + cb;
        g_ctx->VSGetConstantBuffers(0, 3, cbs);
        for (int i = 0; i < 3; ++i) {
            const std::string v = cb_contents(cbs[i], 128);
            if (!v.empty()) line += std::format("\n    vs cb{}: {}", i, v);
        }
    }
    if (!compute) {
        ID3D11RenderTargetView* rtv = nullptr;
        g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            D3D11_RENDER_TARGET_VIEW_DESC d{};
            rtv->GetDesc(&d);
            ID3D11Resource* r = nullptr;
            rtv->GetResource(&r);
            if (r) {
                dump(r, d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? d.Texture2D.MipSlice : 0, seq);
                r->Release();
            }
            rtv->Release();
        }
    } else {
        ID3D11UnorderedAccessView* uav = nullptr;
        g_ctx->CSGetUnorderedAccessViews(0, 1, &uav);
        if (uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
            uav->GetDesc(&d);
            ID3D11Resource* r = nullptr;
            uav->GetResource(&r);
            if (r) {
                dump(r, d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D ? d.Texture2D.MipSlice : 0, seq);
                r->Release();
            }
            uav->Release();
        }
    }
}

// RAII guard: our own context calls inside a hook are not recorded.
struct Scope {
    bool active;
    Scope() : active(!g_in_hook && g_state.load(std::memory_order_relaxed) == State::Recording) {
        if (active) g_in_hook = true;
    }
    ~Scope() {
        if (active) g_in_hook = false;
    }
};

using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using CopyRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                               const D3D11_BOX*);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearUavFloatFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT[4]);
using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);

std::atomic<DrawIndexedOverride> g_draw_override{nullptr};

void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    Scope s;
    std::uint32_t seq = 0;
    if (s.active) seq = begin_event(std::format("DrawIndexed {} {} {}", n, start, base), false, true);
    const auto original = g_hooks->draw_indexed.original<DrawIndexedFn>();
    const DrawIndexedOverride o = g_draw_override.load(std::memory_order_acquire);
    if (!o || !o(c, n, start, base, original)) original(c, n, start, base);
    if (s.active) end_event(seq, false, n);
}
void STDMETHODCALLTYPE draw(ID3D11DeviceContext* c, UINT n, UINT start) {
    Scope s;
    std::uint32_t seq = 0;
    if (s.active) seq = begin_event(std::format("Draw {} {}", n, start), false, true);
    g_hooks->draw.original<DrawFn>()(c, n, start);
    if (s.active) end_event(seq, false, n);
}
void STDMETHODCALLTYPE draw_indexed_instanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, INT base, UINT start_inst) {
    Scope s;
    std::uint32_t seq = 0;
    if (s.active) seq = begin_event(std::format("DrawIndexedInstanced {} x{} {} {}", n, inst, start, base), false, true);
    g_hooks->draw_indexed_instanced.original<DrawIndexedInstancedFn>()(c, n, inst, start, base, start_inst);
    if (s.active) end_event(seq, false, n * inst);
}
void STDMETHODCALLTYPE draw_instanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, UINT start_inst) {
    Scope s;
    std::uint32_t seq = 0;
    if (s.active) seq = begin_event(std::format("DrawInstanced {} x{}", n, inst), false, true);
    g_hooks->draw_instanced.original<DrawInstancedFn>()(c, n, inst, start, start_inst);
    if (s.active) end_event(seq, false, n * inst);
}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    Scope s;
    std::uint32_t seq = 0;
    if (s.active) seq = begin_event(std::format("Dispatch {} {} {}", x, y, z), true, true);
    g_hooks->dispatch.original<DispatchFn>()(c, x, y, z);
    if (s.active) end_event(seq, true);
}
void STDMETHODCALLTYPE copy_region(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource* src,
                                   UINT ssub, const D3D11_BOX* box) {
    Scope s;
    if (s.active) {
        std::string b = box ? std::format(" box {} {} {} {}", box->left, box->top, box->right, box->bottom) : "";
        begin_event(std::format("CopySubresourceRegion dst {} sub{} at {} {} <- src {} sub{}{}", describe(dst), dsub, x, y, describe(src), ssub, b),
                    false, false);
    }
    g_hooks->copy_region.original<CopyRegionFn>()(c, dst, dsub, x, y, z, src, ssub, box);
    if (s.active) stamp();
}
std::atomic<CopyResourceOverride> g_copy_override{nullptr};

void STDMETHODCALLTYPE copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    Scope s;
    if (s.active) begin_event(std::format("CopyResource dst {} <- src {}", describe(dst), describe(src)), false, false);
    const auto original = g_hooks->copy.original<CopyFn>();
    const CopyResourceOverride o = g_copy_override.load(std::memory_order_acquire);
    if (!o || !o(c, dst, src, original)) original(c, dst, src);
    if (s.active) stamp();
}
void STDMETHODCALLTYPE clear_rtv(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
    Scope s;
    if (s.active) begin_event(std::format("ClearRTV {} ({} {} {} {})", describe_view(v), col[0], col[1], col[2], col[3]), false, false);
    g_hooks->clear_rtv.original<ClearRtvFn>()(c, v, col);
    if (s.active) stamp();
}
void STDMETHODCALLTYPE clear_uav_float(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* v, const FLOAT col[4]) {
    Scope s;
    if (s.active) begin_event(std::format("ClearUAVFloat {}", describe_view(v)), false, false);
    g_hooks->clear_uav_float.original<ClearUavFloatFn>()(c, v, col);
    if (s.active) stamp();
}
void STDMETHODCALLTYPE clear_dsv(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT flags, FLOAT depth, UINT8 stencil) {
    Scope s;
    if (s.active) begin_event(std::format("ClearDSV {} flags {} depth {} stencil {}", describe_view(v), flags, depth, stencil), false, false);
    g_hooks->clear_dsv.original<ClearDsvFn>()(c, v, flags, depth, stencil);
    if (s.active) stamp();
}
void STDMETHODCALLTYPE resolve(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dsub, ID3D11Resource* src, UINT ssub, DXGI_FORMAT f) {
    Scope s;
    if (s.active) begin_event(std::format("Resolve dst {} <- src {}", describe(dst), describe(src)), false, false);
    g_hooks->resolve.original<ResolveFn>()(c, dst, dsub, src, ssub, f);
    if (s.active) stamp();
}

bool install(ID3D11DeviceContext* c) {
    void** vt = *reinterpret_cast<void***>(c);
    Hooks& h = *g_hooks;
    return h.draw_indexed.create(vt[kDrawIndexed], &draw_indexed) && h.draw.create(vt[kDraw], &draw) &&
           h.draw_indexed_instanced.create(vt[kDrawIndexedInstanced], &draw_indexed_instanced) &&
           h.draw_instanced.create(vt[kDrawInstanced], &draw_instanced) && h.dispatch.create(vt[kDispatch], &dispatch) &&
           h.copy_region.create(vt[kCopySubresourceRegion], &copy_region) && h.copy.create(vt[kCopyResource], &copy) &&
           h.clear_rtv.create(vt[kClearRenderTargetView], &clear_rtv) &&
           h.clear_uav_float.create(vt[kClearUnorderedAccessViewFloat], &clear_uav_float) &&
           h.clear_dsv.create(vt[kClearDepthStencilView], &clear_dsv) && h.resolve.create(vt[kResolveSubresource], &resolve);
}

void uninstall() {
    Hooks& h = *g_hooks;
    for (hook::InlineHook* x : {&h.draw_indexed, &h.draw, &h.draw_indexed_instanced, &h.draw_instanced, &h.dispatch, &h.copy_region, &h.copy,
                                &h.clear_rtv, &h.clear_uav_float, &h.clear_dsv, &h.resolve})
        x->remove();
}

// The hooks stay installed once they are in place (a few loads per call when no trace runs).
std::atomic<int> g_installed{0};  // 0 not yet, 1 installed, -1 failed

bool ensure_installed(ID3D11Texture2D* texture) {
    const int st = g_installed.load();
    if (st != 0) return st > 0;
    ID3D11Device* dev = nullptr;
    texture->GetDevice(&dev);
    if (!dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    bool ok = ctx && install(ctx);
    if (!ok) uninstall();
    g_installed = ok ? 1 : -1;
    log::info("gpu: immediate context hooks {}", ok ? "installed" : "could not be installed");
    if (ctx) ctx->Release();
    dev->Release();
    return ok;
}

// RHI thread: ends the trace and writes the log.
void finish() {
    g_state = State::Idle;
    // GPU times between consecutive events (the first stamp is the start of the trace).
    std::vector<double> us(g_stamps.size(), -1.0);
    double total_us = -1;
    if (g_disjoint) {
        g_ctx->End(g_disjoint);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        const auto t0 = GetTickCount64();
        while (g_ctx->GetData(g_disjoint, &dj, sizeof(dj), 0) == S_FALSE && GetTickCount64() - t0 < 2000) Sleep(1);
        if (!dj.Disjoint && dj.Frequency) {
            std::vector<UINT64> t(g_stamps.size(), 0);
            for (std::size_t i = 0; i < g_stamps.size(); ++i) {
                if (!g_stamps[i]) continue;
                while (g_ctx->GetData(g_stamps[i], &t[i], sizeof(UINT64), 0) == S_FALSE && GetTickCount64() - t0 < 4000) Sleep(0);
            }
            for (std::size_t i = 1; i < t.size(); ++i)
                if (t[i] && t[i - 1]) us[i] = static_cast<double>(t[i] - t[i - 1]) * 1e6 / static_cast<double>(dj.Frequency);
            if (t.size() > 1 && t.front() && t.back())
                total_us = static_cast<double>(t.back() - t.front()) * 1e6 / static_cast<double>(dj.Frequency);
        }
        g_disjoint->Release();
        g_disjoint = nullptr;
    }
    for (ID3D11Query* q : g_stamps)
        if (q) q->Release();
    std::string result;
    {
        std::ofstream f(g_prefix + ".txt", std::ios::binary);
        f << std::format("# one engine frame, {} events, gpu total {:.1f} us, {} read-backs ({} failed)\n", g_lines.size(), total_us,
                         g_dumps, g_dump_failures);
        f << "# seq call ... | gpu_us = GPU time since the previous event\n";
        for (std::size_t i = 0; i < g_lines.size(); ++i) {
            const double t = i + 1 < us.size() ? us[i + 1] : -1.0;
            f << g_lines[i] << std::format(" | gpu_us {:.1f}\n", t);
        }
        result = f ? std::format("ok wrote {}.txt: {} events, gpu {:.1f} us, {} read-backs", g_prefix, g_lines.size(), total_us, g_dumps)
                   : "err could not write " + g_prefix + ".txt";
    }
    g_stamps.clear();
    g_lines.clear();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    g_ctx = nullptr;
    g_dev = nullptr;
    log::info("gpu trace: {}", result);
    std::lock_guard lock(g_result_mutex);
    g_last_result = result;
}

}  // namespace

void frame_boundary(ID3D11Texture2D* texture) {
    const State st = g_state.load();
    if (st == State::Idle || !texture) return;
    try {
        if (st == State::Armed) {
            ID3D11Device* dev = nullptr;
            texture->GetDevice(&dev);
            if (!dev) return;
            ID3D11DeviceContext* ctx = nullptr;
            dev->GetImmediateContext(&ctx);
            if (!ensure_installed(texture)) {
                ctx->Release();
                dev->Release();
                g_state = State::Idle;
                std::lock_guard lock(g_result_mutex);
                g_last_result = "err could not hook the immediate context";
                return;
            }
            g_dev = dev;
            g_ctx = ctx;
            g_seq = 0;
            g_dumps = 0;
            g_dump_failures = 0;
            g_lines.clear();
            g_stamps.clear();
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(dev->CreateQuery(&qd, &g_disjoint))) g_disjoint = nullptr;
            if (g_disjoint) ctx->Begin(g_disjoint);
            stamp();
            g_state = State::Recording;
        } else if (st == State::Recording) {
            finish();
        }
    } catch (...) {
        g_state = State::Idle;
    }
}

bool install_context_hooks(ID3D11Texture2D* texture) { return texture && ensure_installed(texture); }

void set_draw_indexed_override(DrawIndexedOverride fn) { g_draw_override.store(fn, std::memory_order_release); }
void set_copy_resource_override(CopyResourceOverride fn) { g_copy_override.store(fn, std::memory_order_release); }

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    if (a.empty() || a[0] == "status") {
        std::lock_guard lock(g_result_mutex);
        return std::format("ok state {} names {} hook {} | last: {}", static_cast<int>(g_state.load()), g_names.size(),
                           g_find_hook.installed() ? "on" : "off", g_last_result);
    }
    if (a[0] == "names" && a.size() == 2 && a[1] == "on") {
        if (g_find_hook.installed()) return "ok names already on";
        if (!addresses().FindFreeElement) return "err FRenderTargetPool::FindFreeElement not found";
        if (!g_find_hook.create(reinterpret_cast<void*>(addresses().FindFreeElement), &find_free_element_detour))
            return "err hook failed";
        return "ok recording pooled render target names";
    }
    if (a[0] == "trace" && a.size() >= 2) {
        if (g_state.load() != State::Idle) return "err a trace is already armed or running";
        g_prefix = a[1];
        g_dump_from = 1;
        g_dump_to = 0;
        g_dump_fullscreen = false;
        g_dump_scale = 2;
        try {
            for (std::size_t i = 2; i + 1 < a.size(); ++i) {
                if (a[i] == "dump" && a[i + 1] == "fullscreen") {
                    g_dump_fullscreen = true;
                    ++i;
                } else if (a[i] == "dump" && i + 2 < a.size()) {
                    g_dump_from = static_cast<std::uint32_t>(std::stoul(a[i + 1]));
                    g_dump_to = static_cast<std::uint32_t>(std::stoul(a[i + 2]));
                    i += 2;
                } else if (a[i] == "scale") {
                    g_dump_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::stoul(a[i + 1])), 1, 16);
                    ++i;
                }
            }
        } catch (...) {
            return "err usage: gpu trace <prefix> [dump <from> <to> | dump fullscreen] [scale <n>]";
        }
        {
            std::lock_guard lock(g_result_mutex);
            g_last_result = "armed";
        }
        g_state = State::Armed;
        return "ok armed: the next stereo frame is traced (see gpu status)";
    }
    return "err usage: gpu status | gpu names on | gpu trace <prefix> [dump <from> <to> | dump fullscreen] [scale <n>]";
}

}  // namespace ff7vr::engine::gpu_trace
