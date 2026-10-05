#include "compositor.h"

#include "ff7vr_xr_shaders/quad_ps.h"
#include "ff7vr_xr_shaders/quad_vs.h"

#include <algorithm>
#include <cstring>

namespace ff7vr::xr {
namespace {

struct QuadConstants {
    float corners[4][4];
    float uvOffset[2];
    float uvScale[2];
    uint32_t decodeSrgb;
    uint32_t useAlpha;
    uint32_t pad[2];
};
static_assert(sizeof(QuadConstants) % 16 == 0);

// Near plane for the compositor's projection, metres. Anything closer is clipped.
constexpr float kNearZ = 0.05f;

void Transform(const Mat4& m, const Vec3& p, float out[4]) {
    for (int r = 0; r < 4; ++r) out[r] = m.m[r][0] * p.x + m.m[r][1] * p.y + m.m[r][2] * p.z + m.m[r][3];
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

}  // namespace

bool Compositor::Init(ID3D11Device* device, const Logger* log) {
    log_ = log;
    device_ = device;
    HRESULT hr = device->CreateVertexShader(g_ff7vr_quad_vs, sizeof(g_ff7vr_quad_vs), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(g_ff7vr_quad_ps, sizeof(g_ff7vr_quad_ps), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(QuadConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sd, &sampler_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // quads are visible from both sides
    rd.DepthClipEnable = TRUE;      // near-plane clipping
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&rd, &rs_);
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&bld, &opaque_);
    bld.RenderTarget[0].BlendEnable = TRUE;
    // Layer images hold premultiplied alpha, which is what OpenXR runtimes blend
    // (XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT is never set).
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&bld, &alpha_);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&dsd, &dss_);
    if (FAILED(hr)) {
        log_->Error("compositor: init failed {}", HResultString(hr));
        return false;
    }
    return true;
}

void Compositor::Shutdown() {
    rtvs_.clear();
    vs_.Reset();
    ps_.Reset();
    cb_.Reset();
    sampler_.Reset();
    rs_.Reset();
    opaque_.Reset();
    alpha_.Reset();
    dss_.Reset();
    device_.Reset();
}

void Compositor::Forget(ID3D11Texture2D* tex) {
    std::erase_if(rtvs_, [&](const RtvEntry& e) { return e.tex.Get() == tex; });
}

bool Compositor::Begin(ID3D11DeviceContext* ctx, ID3D11Texture2D* target, DXGI_FORMAT format, uint32_t w, uint32_t h,
                       const float clear[4]) {
    if (!vs_ || !target) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    for (auto& e : rtvs_)
        if (e.tex.Get() == target && e.format == format) rtv = e.rtv.Get();
    if (!rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC d{};
        d.Format = format;
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> v;
        const HRESULT hr = device_->CreateRenderTargetView(target, &d, &v);
        if (FAILED(hr)) {
            log_->Error("compositor: CreateRenderTargetView({}) failed {}", DxgiFormatName(format), HResultString(hr));
            return false;
        }
        if (rtvs_.size() >= 8) rtvs_.erase(rtvs_.begin());
        rtvs_.push_back(RtvEntry{target, format, v});
        rtv = v.Get();
    }
    ctx->SetPredication(nullptr, FALSE);
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->RSSetState(rs_.Get());
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetDepthStencilState(dss_.Get(), 0);
    ID3D11Buffer* cbs[1] = {cb_.Get()};
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* s = sampler_.Get();
    ctx->PSSetSamplers(0, 1, &s);
    return true;
}

bool Compositor::Draw(ID3D11DeviceContext* ctx, const LayerImage& img, const float corners[4][4], bool alphaBlend) {
    if (!img.texture || img.width == 0 || img.height == 0) return false;
    D3D11_TEXTURE2D_DESC td{};
    img.texture->GetDesc(&td);
    DXGI_FORMAT fmt = img.format != DXGI_FORMAT_UNKNOWN ? img.format : td.Format;
    if (IsTypelessFormat(fmt)) fmt = DefaultTypedFormat(fmt);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = 1;
    // Created per draw and released right after: a cached view would keep the texture alive.
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device_->CreateShaderResourceView(img.texture, &sd, &srv);
    if (FAILED(hr)) {
        log_->Error("compositor: CreateShaderResourceView({}) failed {}", DxgiFormatName(fmt), HResultString(hr));
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    QuadConstants c{};
    std::memcpy(c.corners, corners, sizeof(c.corners));
    c.uvOffset[0] = c.uvOffset[1] = 0.0f;
    c.uvScale[0] = float(img.width) / float(td.Width);
    c.uvScale[1] = float(img.height) / float(td.Height);
    c.decodeSrgb = 0;  // the image is read through the format the runtime interprets
    c.useAlpha = alphaBlend ? 1u : 0u;
    std::memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(cb_.Get(), 0);
    const float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(alphaBlend ? alpha_.Get() : opaque_.Get(), bf, 0xFFFFFFFFu);
    ID3D11ShaderResourceView* v = srv.Get();
    ctx->PSSetShaderResources(0, 1, &v);
    ctx->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    return true;
}

bool Compositor::DrawFullView(ID3D11DeviceContext* ctx, const LayerImage& img) {
    const float corners[4][4] = {{-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}};
    return Draw(ctx, img, corners, false);
}

bool Compositor::DrawQuad(ID3D11DeviceContext* ctx, const LayerImage& img, const View& eye, const Pose& quadPose, float widthM,
                          float heightM, bool alphaBlend) {
    const Mat4 vp = Multiply(Projection_ColumnVector(eye.fov, kNearZ, 0.0f, false), ViewMatrix_ColumnVector(eye.pose));
    const float hw = widthM * 0.5f, hh = heightM * 0.5f;
    const Vec3 local[4] = {{-hw, hh, 0}, {hw, hh, 0}, {-hw, -hh, 0}, {hw, -hh, 0}};
    float corners[4][4];
    for (int i = 0; i < 4; ++i) {
        const Vec3 r = QuatRotate(quadPose.orientation, local[i]);
        const Vec3 p{r.x + quadPose.position.x, r.y + quadPose.position.y, r.z + quadPose.position.z};
        Transform(vp, p, corners[i]);
    }
    return Draw(ctx, img, corners, alphaBlend);
}

void Compositor::End(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* none = nullptr;
    ctx->OMSetRenderTargets(1, &none, nullptr);
}

}  // namespace ff7vr::xr
