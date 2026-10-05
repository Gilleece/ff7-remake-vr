#include "d3d11_blitter.h"

#include "ff7vr_xr_shaders/blit_ps.h"
#include "ff7vr_xr_shaders/blit_vs.h"

#include <algorithm>
#include <cmath>

namespace ff7vr::xr {

// ---------------------------------------------------------------------------
// State backup
// ---------------------------------------------------------------------------
namespace {
constexpr UINT kUavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;  // 8 (feature level 11.0)

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
}  // namespace

struct UavBackup {
    ID3D11UnorderedAccessView* uav[kUavSlots]{};
};
static thread_local UavBackup t_uav;  // kept out of the header to keep it small

void D3D11StateBackup::Save(ID3D11DeviceContext* ctx) {
    if (saved_) ReleaseAll();
    ctx->IAGetPrimitiveTopology(&topology_);
    ctx->IAGetInputLayout(inputLayout_.ReleaseAndGetAddressOf());
    ctx->IAGetVertexBuffers(0, kVB, vb_, vbStride_, vbOffset_);
    ctx->IAGetIndexBuffer(ib_.ReleaseAndGetAddressOf(), &ibFormat_, &ibOffset_);

    auto saveStage = [](Stage& s, auto getShader, auto getCB, auto getSRV, auto getSamp) {
        s.instanceCount = 256;
        getShader(s);
        getCB(s);
        getSRV(s);
        getSamp(s);
    };
    saveStage(
        vs_, [&](Stage& s) { ID3D11VertexShader* p = nullptr; ctx->VSGetShader(&p, s.instances, &s.instanceCount); s.shader.Attach(p); },
        [&](Stage& s) { ctx->VSGetConstantBuffers(0, kCB, s.cb); }, [&](Stage& s) { ctx->VSGetShaderResources(0, kSRV, s.srv); },
        [&](Stage& s) { ctx->VSGetSamplers(0, kSamp, s.samp); });
    saveStage(
        ps_, [&](Stage& s) { ID3D11PixelShader* p = nullptr; ctx->PSGetShader(&p, s.instances, &s.instanceCount); s.shader.Attach(p); },
        [&](Stage& s) { ctx->PSGetConstantBuffers(0, kCB, s.cb); }, [&](Stage& s) { ctx->PSGetShaderResources(0, kSRV, s.srv); },
        [&](Stage& s) { ctx->PSGetSamplers(0, kSamp, s.samp); });
    saveStage(
        gs_, [&](Stage& s) { ID3D11GeometryShader* p = nullptr; ctx->GSGetShader(&p, s.instances, &s.instanceCount); s.shader.Attach(p); },
        [&](Stage& s) { ctx->GSGetConstantBuffers(0, kCB, s.cb); }, [&](Stage& s) { ctx->GSGetShaderResources(0, kSRV, s.srv); },
        [&](Stage& s) { ctx->GSGetSamplers(0, kSamp, s.samp); });
    saveStage(
        hs_, [&](Stage& s) { ID3D11HullShader* p = nullptr; ctx->HSGetShader(&p, s.instances, &s.instanceCount); s.shader.Attach(p); },
        [&](Stage& s) { ctx->HSGetConstantBuffers(0, kCB, s.cb); }, [&](Stage& s) { ctx->HSGetShaderResources(0, kSRV, s.srv); },
        [&](Stage& s) { ctx->HSGetSamplers(0, kSamp, s.samp); });
    saveStage(
        ds_, [&](Stage& s) { ID3D11DomainShader* p = nullptr; ctx->DSGetShader(&p, s.instances, &s.instanceCount); s.shader.Attach(p); },
        [&](Stage& s) { ctx->DSGetConstantBuffers(0, kCB, s.cb); }, [&](Stage& s) { ctx->DSGetShaderResources(0, kSRV, s.srv); },
        [&](Stage& s) { ctx->DSGetSamplers(0, kSamp, s.samp); });

    ctx->RSGetState(rs_.ReleaseAndGetAddressOf());
    numViewports_ = 0;
    ctx->RSGetViewports(&numViewports_, nullptr);
    numViewports_ = std::min<UINT>(numViewports_, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
    if (numViewports_) ctx->RSGetViewports(&numViewports_, viewports_);
    numScissors_ = 0;
    ctx->RSGetScissorRects(&numScissors_, nullptr);
    numScissors_ = std::min<UINT>(numScissors_, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
    if (numScissors_) ctx->RSGetScissorRects(&numScissors_, scissors_);

    ctx->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv_, dsv_.ReleaseAndGetAddressOf(), 0,
                                                   kUavSlots, t_uav.uav);
    ctx->OMGetBlendState(blend_.ReleaseAndGetAddressOf(), blendFactor_, &sampleMask_);
    ctx->OMGetDepthStencilState(dss_.ReleaseAndGetAddressOf(), &stencilRef_);
    ctx->GetPredication(predicate_.ReleaseAndGetAddressOf(), &predicateValue_);
    saved_ = true;
}

void D3D11StateBackup::Restore(ID3D11DeviceContext* ctx) {
    if (!saved_) return;
    ctx->IASetPrimitiveTopology(topology_);
    ctx->IASetInputLayout(inputLayout_.Get());
    ctx->IASetVertexBuffers(0, kVB, vb_, vbStride_, vbOffset_);
    ctx->IASetIndexBuffer(ib_.Get(), ibFormat_, ibOffset_);

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    if (vs_.shader) vs_.shader.As(&vs);
    if (ps_.shader) ps_.shader.As(&ps);
    if (gs_.shader) gs_.shader.As(&gs);
    if (hs_.shader) hs_.shader.As(&hs);
    if (ds_.shader) ds_.shader.As(&ds);
    ctx->VSSetShader(vs.Get(), vs_.instanceCount ? vs_.instances : nullptr, vs_.instanceCount);
    ctx->PSSetShader(ps.Get(), ps_.instanceCount ? ps_.instances : nullptr, ps_.instanceCount);
    ctx->GSSetShader(gs.Get(), gs_.instanceCount ? gs_.instances : nullptr, gs_.instanceCount);
    ctx->HSSetShader(hs.Get(), hs_.instanceCount ? hs_.instances : nullptr, hs_.instanceCount);
    ctx->DSSetShader(ds.Get(), ds_.instanceCount ? ds_.instances : nullptr, ds_.instanceCount);
    ctx->VSSetConstantBuffers(0, kCB, vs_.cb);
    ctx->PSSetConstantBuffers(0, kCB, ps_.cb);
    ctx->GSSetConstantBuffers(0, kCB, gs_.cb);
    ctx->HSSetConstantBuffers(0, kCB, hs_.cb);
    ctx->DSSetConstantBuffers(0, kCB, ds_.cb);
    ctx->VSSetSamplers(0, kSamp, vs_.samp);
    ctx->PSSetSamplers(0, kSamp, ps_.samp);
    ctx->GSSetSamplers(0, kSamp, gs_.samp);
    ctx->HSSetSamplers(0, kSamp, hs_.samp);
    ctx->DSSetSamplers(0, kSamp, ds_.samp);

    ctx->RSSetState(rs_.Get());
    ctx->RSSetViewports(numViewports_, numViewports_ ? viewports_ : nullptr);
    ctx->RSSetScissorRects(numScissors_, numScissors_ ? scissors_ : nullptr);

    // Render targets before SRVs, so an SRV that aliases one of our temporary RTVs is not nulled.
    UINT numRtv = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtv_[i]) numRtv = i + 1;
    UINT uavStart = numRtv, uavEnd = numRtv;
    for (UINT i = numRtv; i < kUavSlots; ++i)
        if (t_uav.uav[i]) uavEnd = i + 1;
    if (uavEnd > uavStart) {
        UINT counts[kUavSlots];
        std::fill(std::begin(counts), std::end(counts), static_cast<UINT>(-1));
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(numRtv, rtv_, dsv_.Get(), uavStart, uavEnd - uavStart, t_uav.uav + uavStart, counts);
    } else {
        ctx->OMSetRenderTargets(numRtv, numRtv ? rtv_ : nullptr, dsv_.Get());
    }
    ctx->OMSetBlendState(blend_.Get(), blendFactor_, sampleMask_);
    ctx->OMSetDepthStencilState(dss_.Get(), stencilRef_);

    ctx->VSSetShaderResources(0, kSRV, vs_.srv);
    ctx->PSSetShaderResources(0, kSRV, ps_.srv);
    ctx->GSSetShaderResources(0, kSRV, gs_.srv);
    ctx->HSSetShaderResources(0, kSRV, hs_.srv);
    ctx->DSSetShaderResources(0, kSRV, ds_.srv);
    ctx->SetPredication(predicate_.Get(), predicateValue_);
    ReleaseAll();
}

void D3D11StateBackup::ReleaseAll() {
    inputLayout_.Reset();
    for (auto& b : vb_) SafeRelease(b);
    ib_.Reset();
    for (Stage* s : {&vs_, &ps_, &gs_, &hs_, &ds_}) {
        s->shader.Reset();
        for (UINT i = 0; i < s->instanceCount && i < 256; ++i) SafeRelease(s->instances[i]);
        s->instanceCount = 0;
        for (auto& c : s->cb) SafeRelease(c);
        for (auto& v : s->srv) SafeRelease(v);
        for (auto& v : s->samp) SafeRelease(v);
    }
    rs_.Reset();
    for (auto& r : rtv_) SafeRelease(r);
    for (auto& u : t_uav.uav) SafeRelease(u);
    dsv_.Reset();
    blend_.Reset();
    dss_.Reset();
    predicate_.Reset();
    saved_ = false;
}

// ---------------------------------------------------------------------------
// Blitter
// ---------------------------------------------------------------------------
struct BlitConstants {
    float uvOffset[2];
    float uvScale[2];
    uint32_t decodeSrgb;
    uint32_t forceOpaque;
    uint32_t pad[2];
};

bool Blitter::Init(ID3D11Device* device, const Logger* log) {
    log_ = log;
    device_ = device;
    HRESULT hr = device->CreateVertexShader(g_ff7vr_blit_vs, sizeof(g_ff7vr_blit_vs), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(g_ff7vr_blit_ps, sizeof(g_ff7vr_blit_ps), nullptr, &ps_);
    if (FAILED(hr)) {
        log_->Error("blitter: shader creation failed {}", HResultString(hr));
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(BlitConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sd, &pointSampler_);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sd, &linearSampler_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&rd, &rs_);
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&bld, &blend_);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&dsd, &dss_);
    if (FAILED(hr)) {
        log_->Error("blitter: state creation failed {}", HResultString(hr));
        return false;
    }
    return true;
}

void Blitter::Shutdown() {
    ClearCache();
    vs_.Reset();
    ps_.Reset();
    cb_.Reset();
    pointSampler_.Reset();
    linearSampler_.Reset();
    rs_.Reset();
    blend_.Reset();
    dss_.Reset();
    device_.Reset();
}

void Blitter::Forget(ID3D11Texture2D* tex) {
    std::erase_if(rtvs_, [&](const RtvEntry& e) { return e.tex.Get() == tex; });
}

void Blitter::ClearCache() {
    rtvs_.clear();
    temps_.clear();
}

ComPtr<ID3D11ShaderResourceView> Blitter::CreateSrv(ID3D11Texture2D* tex, DXGI_FORMAT fmt, uint32_t slice, uint32_t mip) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    d.Texture2DArray.MostDetailedMip = mip;
    d.Texture2DArray.MipLevels = 1;
    d.Texture2DArray.FirstArraySlice = slice;
    d.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device_->CreateShaderResourceView(tex, &d, &srv);
    if (FAILED(hr)) {
        log_->Error("blitter: CreateShaderResourceView({}) failed {}", DxgiFormatName(fmt), HResultString(hr));
        return nullptr;
    }
    return srv;
}

ID3D11RenderTargetView* Blitter::GetRtv(ID3D11Texture2D* tex, DXGI_FORMAT fmt, uint32_t slice) {
    for (auto& e : rtvs_)
        if (e.tex.Get() == tex && e.format == fmt && e.slice == slice) return e.rtv.Get();
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = fmt;
    d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    d.Texture2DArray.MipSlice = 0;
    d.Texture2DArray.FirstArraySlice = slice;
    d.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = device_->CreateRenderTargetView(tex, &d, &rtv);
    if (FAILED(hr)) {
        log_->Error("blitter: CreateRenderTargetView({}) failed {}", DxgiFormatName(fmt), HResultString(hr));
        return nullptr;
    }
    if (rtvs_.size() >= 16) rtvs_.erase(rtvs_.begin());
    rtvs_.push_back(RtvEntry{tex, fmt, slice, rtv});
    return rtv.Get();
}

ID3D11Texture2D* Blitter::GetTemp(DXGI_FORMAT fmt, uint32_t w, uint32_t h, UINT bind) {
    for (auto& e : temps_)
        if (e.format == fmt && e.w == w && e.h == h && e.bind == bind) return e.tex.Get();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    const HRESULT hr = device_->CreateTexture2D(&d, nullptr, &t);
    if (FAILED(hr)) {
        log_->Error("blitter: temp texture {}x{} {} failed {}", w, h, DxgiFormatName(fmt), HResultString(hr));
        return nullptr;
    }
    if (temps_.size() >= 6) temps_.erase(temps_.begin());
    temps_.push_back(TempEntry{fmt, w, h, bind, t});
    return t.Get();
}

bool Blitter::Transfer(ID3D11DeviceContext* ctx, const BlitSource& src, const Rect& rectIn, const BlitDest& dst, Path* usedPath,
                       uint32_t* outW, uint32_t* outH) {
    if (usedPath) *usedPath = Path::None;
    if (!src.texture || !dst.texture) return false;

    D3D11_TEXTURE2D_DESC sd{};
    src.texture->GetDesc(&sd);
    D3D11_TEXTURE2D_DESC dd{};
    dst.texture->GetDesc(&dd);

    if (src.mipLevel >= sd.MipLevels || src.arraySlice >= sd.ArraySize) {
        log_->Error("blitter: mip {} / slice {} out of range", src.mipLevel, src.arraySlice);
        return false;
    }
    const uint32_t mipW = std::max(1u, sd.Width >> src.mipLevel);
    const uint32_t mipH = std::max(1u, sd.Height >> src.mipLevel);

    // Clamp the rect to the source.
    Rect rect = rectIn;
    if (rect.x < 0) rect.x = 0;
    if (rect.y < 0) rect.y = 0;
    if (static_cast<uint32_t>(rect.x) >= mipW || static_cast<uint32_t>(rect.y) >= mipH) {
        log_->Error("blitter: source rect origin ({},{}) outside {}x{}", rect.x, rect.y, mipW, mipH);
        return false;
    }
    rect.width = std::min(rect.width, mipW - rect.x);
    rect.height = std::min(rect.height, mipH - rect.y);
    if (rect.width == 0 || rect.height == 0) return false;

    // Formats: how the source is read, what the destination means.
    DXGI_FORMAT srcFmt = src.viewFormat != DXGI_FORMAT_UNKNOWN ? src.viewFormat : sd.Format;
    if (IsTypelessFormat(srcFmt)) {
        if (!warnedOnce_) {
            log_->Warn("blitter: source is {} and no viewFormat given; reading it as {}", DxgiFormatName(srcFmt),
                       DxgiFormatName(DefaultTypedFormat(srcFmt)));
            warnedOnce_ = true;
        }
        srcFmt = DefaultTypedFormat(srcFmt);
    }
    if (!IsTypelessFormat(sd.Format) && TypelessFamily(srcFmt) == TypelessFamily(sd.Format)) {
        // A typed resource can only be viewed with its own format.
        srcFmt = sd.Format;
    }
    DXGI_FORMAT dstFmt = dst.viewFormat != DXGI_FORMAT_UNKNOWN ? dst.viewFormat : dd.Format;
    if (IsTypelessFormat(dstFmt)) dstFmt = DefaultTypedFormat(dstFmt);
    if (!IsTypelessFormat(dd.Format)) dstFmt = dd.Format;

    const bool srcBitsSrgb = src.encoding == ColorEncoding::Srgb || IsSrgbFormat(srcFmt);
    const bool dstBitsSrgb = IsSrgbFormat(dstFmt);
    const uint32_t dstW = dst.width ? std::min(dst.width, dd.Width) : dd.Width;
    const uint32_t dstH = dst.height ? std::min(dst.height, dd.Height) : dd.Height;
    const bool fits = rect.width <= dstW && rect.height <= dstH;

    const bool canCopy = fits && sd.SampleDesc.Count == 1 && dd.SampleDesc.Count == 1 &&
                         TypelessFamily(sd.Format) == TypelessFamily(dd.Format) && srcBitsSrgb == dstBitsSrgb &&
                         TypelessFamily(srcFmt) == TypelessFamily(dstFmt);
    if (canCopy) {
        D3D11_BOX box{static_cast<UINT>(rect.x), static_cast<UINT>(rect.y), 0, static_cast<UINT>(rect.x) + rect.width,
                      static_cast<UINT>(rect.y) + rect.height, 1};
        ctx->CopySubresourceRegion(dst.texture, D3D11CalcSubresource(0, dst.arraySlice, dd.MipLevels), 0, 0, 0, src.texture,
                                   D3D11CalcSubresource(src.mipLevel, src.arraySlice, sd.MipLevels), &box);
        if (usedPath) *usedPath = Path::Copy;
        if (outW) *outW = rect.width;
        if (outH) *outH = rect.height;
        return true;
    }

    // ---- shader blit ----
    ID3D11Texture2D* readTex = src.texture;
    uint32_t readSlice = src.arraySlice, readMip = src.mipLevel;
    uint32_t readW = mipW, readH = mipH;
    Rect readRect = rect;
    DXGI_FORMAT readFmt = srcFmt;

    if (sd.SampleDesc.Count > 1) {
        // Resolve the whole slice into a temp texture.
        ID3D11Texture2D* t = GetTemp(srcFmt, sd.Width, sd.Height, D3D11_BIND_SHADER_RESOURCE);
        if (!t) return false;
        ctx->ResolveSubresource(t, 0, src.texture, D3D11CalcSubresource(0, src.arraySlice, sd.MipLevels), srcFmt);
        readTex = t;
        readSlice = 0;
        readMip = 0;
        readW = sd.Width;
        readH = sd.Height;
    } else if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        // Not sampleable: copy the region into an SRV-capable temp of the same family.
        const DXGI_FORMAT fam = TypelessFamily(sd.Format);
        ID3D11Texture2D* t = GetTemp(fam, rect.width, rect.height, D3D11_BIND_SHADER_RESOURCE);
        if (!t) return false;
        D3D11_BOX box{static_cast<UINT>(rect.x), static_cast<UINT>(rect.y), 0, static_cast<UINT>(rect.x) + rect.width,
                      static_cast<UINT>(rect.y) + rect.height, 1};
        ctx->CopySubresourceRegion(t, 0, 0, 0, 0, src.texture, D3D11CalcSubresource(src.mipLevel, src.arraySlice, sd.MipLevels), &box);
        readTex = t;
        readSlice = 0;
        readMip = 0;
        readW = rect.width;
        readH = rect.height;
        readRect = Rect{0, 0, rect.width, rect.height};
        if (fam == sd.Format && !IsTypelessFormat(fam)) readFmt = fam;
    }
    if (!(dd.BindFlags & D3D11_BIND_RENDER_TARGET)) {
        log_->Error("blitter: destination {} has no render-target binding; cannot convert {} -> {}", DxgiFormatName(dd.Format),
                    DxgiFormatName(srcFmt), DxgiFormatName(dstFmt));
        return false;
    }

    const ComPtr<ID3D11ShaderResourceView> srvRef = CreateSrv(readTex, readFmt, readSlice, readMip);
    ID3D11ShaderResourceView* srv = srvRef.Get();
    ID3D11RenderTargetView* rtv = GetRtv(dst.texture, dstFmt, dst.arraySlice);
    if (!srv || !rtv) return false;

    uint32_t w = rect.width, h = rect.height;
    if (!fits) {
        const double s = std::min(double(dstW) / rect.width, double(dstH) / rect.height);
        w = std::max(1u, static_cast<uint32_t>(std::floor(rect.width * s)));
        h = std::max(1u, static_cast<uint32_t>(std::floor(rect.height * s)));
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    BlitConstants c{};
    c.uvOffset[0] = float(readRect.x) / readW;
    c.uvOffset[1] = float(readRect.y) / readH;
    c.uvScale[0] = float(readRect.width) / readW;
    c.uvScale[1] = float(readRect.height) / readH;
    c.decodeSrgb = (src.encoding == ColorEncoding::Srgb && !IsSrgbFormat(readFmt)) ? 1u : 0u;
    c.forceOpaque = 1;
    memcpy(m.pData, &c, sizeof(c));
    ctx->Unmap(cb_.Get(), 0);

    ctx->SetPredication(nullptr, FALSE);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->RSSetState(rs_.Get());
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    // Bind the RTV first: that unbinds the source if it was the current render target,
    // so the SRV bind below is not rejected as a read/write hazard.
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blend_.Get(), bf, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(dss_.Get(), 0);
    ID3D11Buffer* cbs[1] = {cb_.Get()};
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = (fits && w == rect.width && h == rect.height) ? pointSampler_.Get() : linearSampler_.Get();
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    if (usedPath) *usedPath = Path::Blit;
    if (outW) *outW = w;
    if (outH) *outH = h;
    return true;
}

}  // namespace ff7vr::xr
