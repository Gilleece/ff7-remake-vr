// Software layer compositor: draws what a VR runtime would show in one eye,
// from the layers of a frame. Used by the Null backend for every frame and by
// the OpenXR backend for captures.
#pragma once

#include "xr_common.h"

#include <vector>

namespace ff7vr::xr {

// An image as the runtime would read it: the region (0,0,width,height) of
// `texture`, read through `format` (the format the runtime interprets).
struct LayerImage {
    ID3D11Texture2D* texture = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
};

class Compositor {
public:
    bool Init(ID3D11Device* device, const Logger* log);
    void Shutdown();
    // Drops cached views of `tex` (call before a cached render target is released).
    void Forget(ID3D11Texture2D* tex);

    // Starts an eye image: binds (0,0,w,h) of `target` (viewed as `format`)
    // and clears it. Caller has saved the context state.
    bool Begin(ID3D11DeviceContext* ctx, ID3D11Texture2D* target, DXGI_FORMAT format, uint32_t w, uint32_t h, const float clear[4]);
    // Projection layer view: the image stretched over the eye's whole field of view.
    bool DrawFullView(ID3D11DeviceContext* ctx, const LayerImage& img);
    // Quad layer: `quadPose` (centre, image facing +Z) and the eye pose are in
    // the same space. Perspective-correct, clipped at the near plane.
    bool DrawQuad(ID3D11DeviceContext* ctx, const LayerImage& img, const View& eye, const Pose& quadPose, float widthM, float heightM,
                  bool alphaBlend);
    void End(ID3D11DeviceContext* ctx);

private:
    bool Draw(ID3D11DeviceContext* ctx, const LayerImage& img, const float corners[4][4], bool alphaBlend);

    struct RtvEntry {
        ComPtr<ID3D11Texture2D> tex;
        DXGI_FORMAT format;
        ComPtr<ID3D11RenderTargetView> rtv;
    };

    const Logger* log_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rs_;
    ComPtr<ID3D11BlendState> opaque_, alpha_;
    ComPtr<ID3D11DepthStencilState> dss_;
    std::vector<RtvEntry> rtvs_;  // our own targets only (never a host texture)
};

}  // namespace ff7vr::xr
