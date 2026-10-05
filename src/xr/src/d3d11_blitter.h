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

struct BlitSource {
    ID3D11Texture2D* texture = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // UNKNOWN = texture format
    ColorEncoding encoding = ColorEncoding::Srgb;
    uint32_t arraySlice = 0;
    uint32_t mipLevel = 0;
};

struct BlitDest {
    ID3D11Texture2D* texture = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // format the runtime interprets the image as
    uint32_t width = 0, height = 0;                // usable size of the destination
    uint32_t arraySlice = 0;
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

    // Drops cached views referring to `tex` (call before releasing a texture the cache may hold).
    void Forget(ID3D11Texture2D* tex);
    void ClearCache();

private:
    struct SrvEntry {
        ComPtr<ID3D11Texture2D> tex;
        DXGI_FORMAT format;
        uint32_t slice, mip;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
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

    ID3D11ShaderResourceView* GetSrv(ID3D11Texture2D* tex, DXGI_FORMAT fmt, uint32_t slice, uint32_t mip);
    ID3D11RenderTargetView* GetRtv(ID3D11Texture2D* tex, DXGI_FORMAT fmt, uint32_t slice);
    ID3D11Texture2D* GetTemp(DXGI_FORMAT fmt, uint32_t w, uint32_t h, UINT bind);

    const Logger* log_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> pointSampler_, linearSampler_;
    ComPtr<ID3D11RasterizerState> rs_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11DepthStencilState> dss_;
    std::vector<SrvEntry> srvs_;
    std::vector<RtvEntry> rtvs_;
    std::vector<TempEntry> temps_;
    bool warnedOnce_ = false;
};

}  // namespace ff7vr::xr
