#include "mirror.h"

#include "rhi_command.h"

#include "ff7vr/core/log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ff7vr::engine::mirror {
namespace {

const char kShader[] = R"(
cbuffer Params : register(b0) { float4 uv_rect; float4 texel; };
Texture2D src : register(t0);
SamplerState smp : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    float2 t = float2((id << 1) & 2, id & 2);
    pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    uv = t;
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 s = uv_rect.xy + uv * uv_rect.zw;
    float2 d = texel.xy;  // a quarter of the destination pixel's footprint in source UV
    float3 c = src.SampleLevel(smp, s + float2(-d.x, -d.y), 0).rgb + src.SampleLevel(smp, s + float2(d.x, -d.y), 0).rgb +
               src.SampleLevel(smp, s + float2(-d.x, d.y), 0).rgb + src.SampleLevel(smp, s + float2(d.x, d.y), 0).rgb;
    return float4(c * 0.25, 1);
}
)";

struct Params {
    float uv_rect[4];
    float texel[4];
};

struct Resources {
    ID3D11Device* device = nullptr;  // not owned; identifies the device the objects belong to
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> ds;
    ID3D11Texture2D* srv_tex = nullptr;  // texture the cached SRV was made for
    ComPtr<ID3D11ShaderResourceView> srv;
    ID3D11Texture2D* rtv_tex = nullptr;
    ComPtr<ID3D11RenderTargetView> rtv;
    bool failed = false;
};
// RHI thread only. Allocated once and never freed: releasing D3D11 objects from a static
// destructor at process exit would call into d3d11.dll after it has been detached.
Resources* g_res_ptr = new Resources();

struct MirrorCommand {
    ID3D11Texture2D* eye = nullptr;
    ID3D11Texture2D* back_buffer = nullptr;
    EyeRect left{}, right{};
    Mode mode = Mode::Off;
};

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                      UINT, ID3DBlob**, ID3DBlob**);

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    static D3DCompileFn fn = [] {
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<D3DCompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    if (!fn) {
        log::error("mirror: d3dcompiler_47.dll not available");
        return false;
    }
    ComPtr<ID3DBlob> err;
    HRESULT hr = fn(kShader, sizeof(kShader) - 1, "mirror", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &out, &err);
    if (FAILED(hr)) {
        log::error("mirror: shader {} failed: {}", entry,
                   err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "?");
        return false;
    }
    return true;
}

bool init_resources(ID3D11Device* dev) {
    if (g_res_ptr->device == dev && !g_res_ptr->failed && g_res_ptr->ps) return true;
    if (g_res_ptr->device == dev && g_res_ptr->failed) return false;
    if (g_res_ptr->device && g_res_ptr->device != dev) g_res_ptr = new Resources();  // other device: leak the old objects
    Resources& res = *g_res_ptr;
    res.device = dev;
    ComPtr<ID3DBlob> vsb, psb;
    bool ok = compile("vs", "vs_5_0", vsb) && compile("ps", "ps_5_0", psb);
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &res.vs));
    ok = ok && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &res.ps));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &res.cb));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &res.sampler));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &res.rs));
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bld, &res.blend));
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dsd, &res.ds));
    if (!ok) {
        log::error("mirror: could not create D3D11 resources; mirror disabled");
        res.failed = true;
        return false;
    }
    log::info("mirror: D3D11 resources created");
    return true;
}

// Everything the blit touches, saved and restored around it.
struct SavedState {
    ComPtr<ID3D11InputLayout> il;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> ps_srv;
    ComPtr<ID3D11SamplerState> ps_smp;
    ComPtr<ID3D11Buffer> ps_cb;
    ComPtr<ID3D11RasterizerState> rs;
    UINT num_vp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ComPtr<ID3D11BlendState> blend;
    float blend_factor[4]{};
    UINT sample_mask = 0;
    ComPtr<ID3D11DepthStencilState> dss;
    UINT stencil_ref = 0;

    void save(ID3D11DeviceContext* c) {
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &ps_srv);
        c->PSGetSamplers(0, 1, &ps_smp);
        c->PSGetConstantBuffers(0, 1, &ps_cb);
        c->RSGetState(&rs);
        c->RSGetViewports(&num_vp, vp);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blend_factor, &sample_mask);
        c->OMGetDepthStencilState(&dss, &stencil_ref);
    }
    void restore(ID3D11DeviceContext* c) {
        // Outputs first: binding a resource as a render target unbinds it from shader
        // inputs, so restoring the inputs afterwards reproduces the saved state exactly.
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->IASetInputLayout(il.Get());
        c->IASetPrimitiveTopology(topo);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->HSSetShader(hs.Get(), nullptr, 0);
        c->DSSetShader(ds.Get(), nullptr, 0);
        c->GSSetShader(gs.Get(), nullptr, 0);
        c->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv = ps_srv.Get();
        c->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* smp = ps_smp.Get();
        c->PSSetSamplers(0, 1, &smp);
        ID3D11Buffer* cb = ps_cb.Get();
        c->PSSetConstantBuffers(0, 1, &cb);
        c->RSSetState(rs.Get());
        c->RSSetViewports(num_vp, vp);
        c->OMSetBlendState(blend.Get(), blend_factor, sample_mask);
        c->OMSetDepthStencilState(dss.Get(), stencil_ref);
        for (auto*& r : rtv)
            if (r) r->Release(), r = nullptr;
        if (dsv) dsv->Release(), dsv = nullptr;
    }
};

void blit(const MirrorCommand& cmd) {
    if (!cmd.eye || !cmd.back_buffer) return;
    ComPtr<ID3D11Device> dev;
    cmd.eye->GetDevice(&dev);
    if (!dev || !init_resources(dev.Get())) return;
    Resources& res = *g_res_ptr;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);

    D3D11_TEXTURE2D_DESC sdesc{}, ddesc{};
    cmd.eye->GetDesc(&sdesc);
    cmd.back_buffer->GetDesc(&ddesc);
    if (res.srv_tex != cmd.eye || !res.srv) {
        res.srv.Reset();
        res.srv_tex = cmd.eye;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = typed_view_format(sdesc.Format);
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        if (v.Format == DXGI_FORMAT_UNKNOWN || FAILED(dev->CreateShaderResourceView(cmd.eye, &v, &res.srv))) {
            log::warn("mirror: cannot read eye texture format {}", static_cast<int>(sdesc.Format));
            return;
        }
    }
    if (res.rtv_tex != cmd.back_buffer || !res.rtv) {
        res.rtv.Reset();
        res.rtv_tex = cmd.back_buffer;
        D3D11_RENDER_TARGET_VIEW_DESC v{};
        v.Format = typed_view_format(ddesc.Format);
        v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (v.Format == DXGI_FORMAT_UNKNOWN || FAILED(dev->CreateRenderTargetView(cmd.back_buffer, &v, &res.rtv))) {
            log::warn("mirror: cannot render to back buffer format {}", static_cast<int>(ddesc.Format));
            return;
        }
        log::info("mirror: eye texture {}x{} format {}, back buffer {}x{} format {}", sdesc.Width, sdesc.Height,
                  static_cast<int>(sdesc.Format), ddesc.Width, ddesc.Height, static_cast<int>(ddesc.Format));
    }

    // Source rectangle in pixels.
    double sx = 0, sy = 0, sw = 0, sh = 0;
    switch (cmd.mode) {
        case Mode::Left:
        case Mode::Crop:
            sx = cmd.left.x, sy = cmd.left.y, sw = cmd.left.width, sh = cmd.left.height;
            break;
        case Mode::Right:
            sx = cmd.right.x, sy = cmd.right.y, sw = cmd.right.width, sh = cmd.right.height;
            break;
        case Mode::Both:
            sx = cmd.left.x, sy = cmd.left.y, sw = double(cmd.right.x) + cmd.right.width - cmd.left.x, sh = cmd.left.height;
            break;
        case Mode::Off:
            return;
    }
    if (sw <= 0 || sh <= 0) return;
    const double dw = ddesc.Width, dh = ddesc.Height;
    double vx = 0, vy = 0, vw = dw, vh = dh;
    if (cmd.mode == Mode::Crop) {
        // Fill the window: crop the eye to the window's aspect ratio around its centre.
        const double want = dw / dh;
        if (sw / sh > want) {
            const double nw = sh * want;
            sx += (sw - nw) / 2, sw = nw;
        } else {
            const double nh = sw / want;
            sy += (sh - nh) / 2, sh = nh;
        }
    } else {
        // Letterbox: whole source, aspect preserved.
        const double s = std::min(dw / sw, dh / sh);
        vw = sw * s, vh = sh * s;
        vx = (dw - vw) / 2, vy = (dh - vh) / 2;
    }

    SavedState saved;
    saved.save(ctx.Get());

    const float black[4] = {0, 0, 0, 1};
    if (cmd.mode != Mode::Crop) ctx->ClearRenderTargetView(res.rtv.Get(), black);

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(res.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        Params p{};
        p.uv_rect[0] = static_cast<float>(sx / sdesc.Width);
        p.uv_rect[1] = static_cast<float>(sy / sdesc.Height);
        p.uv_rect[2] = static_cast<float>(sw / sdesc.Width);
        p.uv_rect[3] = static_cast<float>(sh / sdesc.Height);
        p.texel[0] = static_cast<float>(sw / vw / sdesc.Width * 0.25);
        p.texel[1] = static_cast<float>(sh / vh / sdesc.Height * 0.25);
        std::memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(res.cb.Get(), 0);
    }
    // Output first: the eye texture is usually still bound as the scene's render target here,
    // and D3D11 refuses (silently nulls) a shader input that is bound as an output.
    ID3D11RenderTargetView* rtv = res.rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(res.vs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(res.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = res.srv.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = res.sampler.Get();
    ctx->PSSetSamplers(0, 1, &smp);
    ID3D11Buffer* cb = res.cb.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->RSSetState(res.rs.Get());
    D3D11_VIEWPORT vp{static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vw), static_cast<float>(vh), 0, 1};
    ctx->RSSetViewports(1, &vp);
    const float zero[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(res.blend.Get(), zero, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(res.ds.Get(), 0);
    ctx->Draw(3, 0);

    // Unbind our SRV before restoring, so the eye texture is not left bound if the
    // engine had nothing in slot 0.
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    saved.restore(ctx.Get());
}

}  // namespace

const char* to_string(Mode m) {
    switch (m) {
        case Mode::Off: return "off";
        case Mode::Left: return "left";
        case Mode::Right: return "right";
        case Mode::Both: return "both";
        case Mode::Crop: return "crop";
    }
    return "?";
}

bool parse_mode(const std::string& s, Mode& out) {
    if (s == "off" || s == "0") out = Mode::Off;
    else if (s == "left") out = Mode::Left;
    else if (s == "right") out = Mode::Right;
    else if (s == "both") out = Mode::Both;
    else if (s == "crop" || s == "1") out = Mode::Crop;
    else return false;
    return true;
}

DXGI_FORMAT typed_view_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

void draw(ID3D11Texture2D* eye_texture, ID3D11Texture2D* back_buffer, const EyeRect& left, const EyeRect& right,
          Mode mode) {
    if (mode == Mode::Off || !eye_texture || !back_buffer) return;
    MirrorCommand cmd;
    cmd.eye = eye_texture;
    cmd.back_buffer = back_buffer;
    cmd.left = left;
    cmd.right = right;
    cmd.mode = mode;
    blit(cmd);
}

}  // namespace ff7vr::engine::mirror
