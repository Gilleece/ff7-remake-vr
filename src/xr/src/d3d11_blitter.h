// D3D11 copy / shader-blit path and immediate-context state save/restore.
#pragma once

#include "xr_common.h"

#include <vector>

namespace ff7vr::xr {

// Saves and restores every piece of immediate-context pipeline state that our
// blits (or an OpenXR runtime using the same context) may change.
class D3D11StateBackup {
public:
    void Save(ID3D11DeviceContext* ctx);
    void Restore(ID3D11DeviceContext* ctx);

private:
    static constexpr UINT kVB = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    static constexpr UINT kCB = 2;   // constant buffer slots we save per stage
    static constexpr UINT kSRV = 2;  // SRV slots we save per stage
    static constexpr UINT kSamp = 2;

    bool saved_ = false;
    // IA
    D3D11_PRIMITIVE_TOPOLOGY topology_{};
    ComPtr<ID3D11InputLayout> inputLayout_;
    ID3D11Buffer* vb_[kVB]{};
    UINT vbStride_[kVB]{}, vbOffset_[kVB]{};
    ComPtr<ID3D11Buffer> ib_;
    DXGI_FORMAT ibFormat_{};
    UINT ibOffset_ = 0;
    // shaders (+ class instances)
    struct Stage {
        ComPtr<ID3D11DeviceChild> shader;
        ID3D11ClassInstance* instances[256]{};
        UINT instanceCount = 0;
        ID3D11Buffer* cb[kCB]{};
        ID3D11ShaderResourceView* srv[kSRV]{};
        ID3D11SamplerState* samp[kSamp]{};
    } vs_, ps_, gs_, hs_, ds_;
    // RS
    ComPtr<ID3D11RasterizerState> rs_;
    UINT numViewports_ = 0;
    D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT numScissors_ = 0;
    D3D11_RECT scissors_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    // OM
    ID3D11RenderTargetView* rtv_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ComPtr<ID3D11DepthStencilView> dsv_;
    ComPtr<ID3D11BlendState> blend_;
    FLOAT blendFactor_[4]{};
    UINT sampleMask_ = 0;
    ComPtr<ID3D11DepthStencilState> dss_;
    UINT stencilRef_ = 0;
    // Predication
    ComPtr<ID3D11Predicate> predicate_;
    BOOL predicateValue_ = FALSE;

    void ReleaseAll();
};

// What happens to the alpha channel on the way into the destination.
enum class BlitAlpha {
    Opaque,                 // alpha = 1 (a plain copy may keep the source's alpha bits; opaque layers ignore them)
    Premultiplied,          // kept as is (source is premultiplied)
    Straight,               // rgb *= alpha (straight -> premultiplied)
    PremultipliedInverted,  // alpha = 1 - alpha (Unreal's inverted coverage -> premultiplied)
};

struct BlitSource {
    ID3D11Texture2D* texture = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // UNKNOWN = texture format
    ColorEncoding encoding = ColorEncoding::Srgb;
    uint32_t arraySlice = 0;
    uint32_t mipLevel = 0;
    BlitAlpha alpha = BlitAlpha::Opaque;
    // Picture adjustment for an opaque transfer (null or identity: none). A source that
    // could be copied as it is goes through the shader instead while it is set.
    const PictureAdjust* picture = nullptr;
};

struct BlitDest {
    ID3D11Texture2D* texture = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // format the runtime interprets the image as
    uint32_t width = 0, height = 0;                // usable size of the destination
    uint32_t arraySlice = 0;
    // Overlay drawing (BlendOver): the destination rectangle starts at (x, y) and is
    // exactly width x height (the source is stretched to it).
    int32_t x = 0, y = 0;
    // How the destination stores colour when its view is not an _SRGB format: Linear
    // (OpenXR swapchains) or Srgb (a game back buffer holding gamma-encoded values).
    ColorEncoding encoding = ColorEncoding::Linear;
};

class Blitter {
public:
    enum class Path { None, Copy, Blit };

    bool Init(ID3D11Device* device, const Logger* log);
    void Shutdown();

    // Moves src rect into dst at (0,0). If the rect fits in dst it is copied 1:1
    // and *outW/*outH = rect size; if larger it is scaled down to fit (aspect
    // preserved), with *outW/*outH the written size.
    // Caller must have saved the context state (D3D11StateBackup).
    bool Transfer(ID3D11DeviceContext* ctx, const BlitSource& src, const Rect& rect, const BlitDest& dst, Path* usedPath,
                  uint32_t* outW, uint32_t* outH);

    // Drops cached render-target views of `tex` (call before releasing a
    // destination texture). Source textures are never cached: their views are
    // created per transfer and released before Transfer returns, so the host
    // can resize or release its textures (a swap chain's ResizeBuffers fails
    // while any reference to a back buffer is alive).
    void Forget(ID3D11Texture2D* tex);
    void ClearCache();

    // Draws src rect over dst's rectangle (x, y, width, height), stretched, blended as
    // premultiplied alpha (dst = src + dst * (1 - src.a)). With dst.encoding Srgb and a
    // non-sRGB view the blend happens on gamma-encoded values, like a game's own UI.
    // Caller must have saved the context state.
    bool BlendOver(ID3D11DeviceContext* ctx, const BlitSource& src, const Rect& rect, const BlitDest& dst);

private:
    struct RtvEntry {
        ComPtr<ID3D11Texture2D> tex;
        DXGI_FORMAT format;
        uint32_t slice;
        ComPtr<ID3D11RenderTargetView> rtv;
    };
    struct TempEntry {
        DXGI_FORMAT format;
        uint32_t w, h, bind;
        ComPtr<ID3D11Texture2D> tex;
    };

    ComPtr<ID3D11ShaderResourceView> CreateSrv(ID3D11Texture2D* tex, DXGI_FORMAT fmt, uint32_t slice, uint32_t mip);
    ID3D11RenderTargetView* GetRtv(ID3D11Texture2D* tex, DXGI_FORMAT fmt, uint32_t slice);
    ID3D11Texture2D* GetTemp(DXGI_FORMAT fmt, uint32_t w, uint32_t h, UINT bind);
    // Fills the picture curve for `p` unless it already holds it. False without the texture.
    bool UpdateCurve(ID3D11DeviceContext* ctx, const PictureAdjust& p);

    static constexpr uint32_t kCurveSize = 4096;
    ComPtr<ID3D11Texture1D> curve_;  // R32_FLOAT, see blit.hlsl
    ComPtr<ID3D11ShaderResourceView> curveSrv_;
    PictureAdjust curveFor_{};
    bool curveValid_ = false;

    const Logger* log_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> pointSampler_, linearSampler_;
    ComPtr<ID3D11RasterizerState> rs_;
    ComPtr<ID3D11BlendState> blend_, blendOver_;
    ComPtr<ID3D11DepthStencilState> dss_;
    std::vector<RtvEntry> rtvs_;
    std::vector<TempEntry> temps_;
    bool warnedOnce_ = false;
};

}  // namespace ff7vr::xr
