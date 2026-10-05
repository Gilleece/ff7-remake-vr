#include "foveation.h"

#include "timing.h"
#include "xr_controller.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/xr/xr.h"

#include <d3d11.h>
#include <wrl/client.h>

#pragma warning(push, 0)
#include <nvapi.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <format>
#include <mutex>
#include <sstream>
#include <vector>

namespace ff7vr::render::foveation {
namespace {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- settings

enum class Corners { Off, Coarse, Cull };
enum class Passes { Scene, NoGBuffer, All };

struct Settings {
    bool enabled = true;
    std::string preset = "quality";
    // Ring radii as fractions of half the eye's width, measured from the eye's
    // optical centre as angle tangents (so a ring is a circle of constant angle
    // from the view axis, whatever the FOV's asymmetry).
    float radius[3] = {0.70f, 0.90f, 1.15f};
    // Shading rate between radius[0] and [1], between [1] and [2], beyond [2].
    NV_PIXEL_SHADING_RATE rate[3] = {NV_PIXEL_X1_PER_2X1_RASTER_PIXELS, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS};
    Corners corners = Corners::Coarse;
    Passes passes = Passes::Scene;
    std::vector<int> skipFormats{35};  // DXGI formats of render target 0 that never get the mask (35: velocity)
};

struct Preset {
    const char* name;
    float radius[3];
    NV_PIXEL_SHADING_RATE rate[3];
};
// Radii are fractions of half the eye width from the optical centre (1.0 reaches about
// the image edge on the nasal side of a typical headset eye, which extends further on
// the temporal side and to about 1.6 in the corners).
constexpr Preset kPresets[] = {
    {"quality", {0.70f, 0.90f, 1.15f}, {NV_PIXEL_X1_PER_2X1_RASTER_PIXELS, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS}},
    {"balanced", {0.55f, 0.80f, 1.05f}, {NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS}},
    {"performance", {0.45f, 0.65f, 0.90f}, {NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS}},
};

struct RateName {
    const char* name;
    NV_PIXEL_SHADING_RATE rate;
    int pixels;  // raster pixels per shading sample
};
constexpr RateName kRates[] = {
    {"1x1", NV_PIXEL_X1_PER_RASTER_PIXEL, 1},       {"2x1", NV_PIXEL_X1_PER_2X1_RASTER_PIXELS, 2},
    {"1x2", NV_PIXEL_X1_PER_1X2_RASTER_PIXELS, 2},  {"2x2", NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, 4},
    {"4x2", NV_PIXEL_X1_PER_4X2_RASTER_PIXELS, 8},  {"2x4", NV_PIXEL_X1_PER_2X4_RASTER_PIXELS, 8},
    {"4x4", NV_PIXEL_X1_PER_4X4_RASTER_PIXELS, 16}, {"cull", NV_PIXEL_X0_CULL_RASTER_PIXELS, 0},
};

const char* RateText(NV_PIXEL_SHADING_RATE r) {
    for (const auto& e : kRates)
        if (e.rate == r) return e.name;
    return "?";
}
int RatePixels(NV_PIXEL_SHADING_RATE r) {
    for (const auto& e : kRates)
        if (e.rate == r) return e.pixels;
    return 1;
}
bool ParseRate(std::string_view s, NV_PIXEL_SHADING_RATE* out) {
    for (const auto& e : kRates)
        if (s == e.name) {
            *out = e.rate;
            return true;
        }
    return false;
}
const char* CornersText(Corners c) { return c == Corners::Off ? "off" : c == Corners::Cull ? "cull" : "coarse"; }

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::vector<std::string> Split(const std::string& s, const char* seps = " ,\t") {
    std::vector<std::string> out;
    size_t b = s.find_first_not_of(seps);
    while (b != std::string::npos) {
        const size_t e = s.find_first_of(seps, b);
        out.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
        b = e == std::string::npos ? e : s.find_first_not_of(seps, e);
    }
    return out;
}
bool ParseFloat(const std::string& s, float* out) {
    float v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return false;
    *out = v;
    return true;
}

bool ApplyPreset(Settings& s, const std::string& name) {
    if (name == "off") {
        s.enabled = false;
        s.preset = "off";
        return true;
    }
    for (const auto& p : kPresets)
        if (name == p.name) {
            s.enabled = true;
            s.preset = p.name;
            std::copy(std::begin(p.radius), std::end(p.radius), s.radius);
            std::copy(std::begin(p.rate), std::end(p.rate), s.rate);
            return true;
        }
    return false;
}

std::string Describe(const Settings& s) {
    if (!s.enabled) return "off";
    return std::format("preset {}: 1x1 inside {:.2f}, {} to {:.2f}, {} to {:.2f}, {} beyond; hidden area {}; passes {}{}", s.preset, s.radius[0],
                       RateText(s.rate[0]), s.radius[1], RateText(s.rate[1]), s.radius[2], RateText(s.rate[2]), CornersText(s.corners),
                       s.passes == Passes::All ? "all (size rule only)" : s.passes == Passes::NoGBuffer ? "scene without the G-buffer pass" : "scene",
                       s.skipFormats.empty() ? std::string() : std::format(", skipping {} render target format(s)", s.skipFormats.size()));
}

// ---------------------------------------------------------------- state

std::mutex g_settingsMutex;
Settings g_settings;                       // under g_settingsMutex
std::atomic<uint32_t> g_settingsVersion{1};
std::atomic<bool> g_enabled{false};        // settings say on
std::atomic<bool> g_unsupported{false};    // NVAPI or the GPU cannot do it: off for the session
std::atomic<ID3D11DeviceContext*> g_ctx{nullptr};
// The main swap chain's device and immediate context, from the Present hook (not referenced:
// the game's device lives as long as the process).
std::atomic<ID3D11Device*> g_seenDevice{nullptr};
std::atomic<ID3D11DeviceContext*> g_seenCtx{nullptr};
std::atomic<bool> g_traceRequested{false};
// The scene markers keep running (GPU timing of the scene only, no mask) after `fov off` in a
// session that had it on, so on and off can be compared in one run.
std::atomic<bool> g_measure{false};
std::atomic<bool> g_simulateUnsupported{false};  // [debug] foveation_unsupported: test of the fallback

// Indices in the shading-rate surface.
constexpr uint8_t kIndexFull = 0, kIndexHidden = 4;

// Context thread only (the RHI thread), unless noted.
struct State {
    enum class Init { NotYet, Ok, Failed } init = Init::NotYet;
    ID3D11Device* device = nullptr;  // not referenced: the game's device outlives the session
    Settings settings;
    uint32_t settingsVersion = 0;
    // Layout of the last stereo scene and the surface built for it.
    FoveationEye eyes[2]{};
    bool haveLayout = false;
    uint32_t layoutW = 0, layoutH = 0;  // side-by-side extent of the eye rects
    uint32_t hiddenVersion = ~0u;
    bool hiddenPresent = false;
    ComPtr<ID3D11Texture2D> surface;
    ComPtr<ID3D11NvShadingRateResourceView> surfaceView;
    struct Retired {
        ComPtr<ID3D11Texture2D> surface;
        ComPtr<ID3D11NvShadingRateResourceView> view;
        int presents = 0;
    };
    std::vector<Retired> retired;  // replaced surfaces, released after a few Presents
    uint32_t tilesX = 0, tilesY = 0;
    double workFraction = 1.0;  // pixel shader invocations relative to full rate, inside the eye rects
    std::string surfaceText;
    // Per frame.
    bool open = false;      // inside the scene window, mask allowed
    bool timing = false;    // inside the scene window, scene timer running
    bool vrsOn = false;     // variable rate shading currently enabled on the context
    bool viewBound = false; // our surface is bound on the context
    uint64_t frames = 0, bindings = 0, matched = 0;
    uint64_t bindingsThisFrame = 0, matchedThisFrame = 0;
    int64_t cpuTicksThisFrame = 0;
    GpuTimer sceneTimer, afterTimer;
    bool afterRunning = false;
    // Trace of one frame.
    struct TraceEvent {
        uint32_t w = 0, h = 0;
        int fmt = 0;
        uint32_t rtvs = 0;
        bool dsv = false, inWindow = false, applied = false;
        const char* note = "";
        int query = -1;
    };
    bool tracing = false, traceReadPending = false;
    std::vector<TraceEvent> trace;
    std::vector<ComPtr<ID3D11Query>> traceQueries;
    ComPtr<ID3D11Query> traceDisjoint;
    int traceQueryNext = 0;
    int traceEndQuery = -1;
    int traceWaitPresents = 0;
};
State g;
std::mutex g_statusMutex;  // guards the copies below, for Status() from the pipe thread
std::string g_statusLine = "not initialised";

Series g_cpuSeries{"foveation context hooks (CPU per frame)"};
Series g_sceneSeries{"gpu scene (foveation window)"};
Series g_afterSeries{"gpu after the scene until Present"};

// ---------------------------------------------------------------- hooks
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                             UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using ClearStateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
constexpr size_t kOMSetRenderTargets = 33, kOMSetRenderTargetsAndUavs = 34, kClearState = 110;
hook::VTableHook g_omHook, g_omUavHook, g_clearHook;

void LogOff(const std::string& why) {
    log::info("foveation: off for this session: {}", why);
    g_unsupported = true;
    std::lock_guard lk(g_statusMutex);
    g_statusLine = "off: " + why;
}

bool SetRates(ID3D11DeviceContext* ctx, bool on) {
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC vp[NV_MAX_NUM_VIEWPORTS]{};
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC d{};
    d.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    d.numViewports = 0;  // 0 switches it off for every viewport
    d.pViewports = vp;
    if (on) {
        // The engine binds one viewport per eye; every slot gets the same table.
        for (auto& v : vp) {
            v.enableVariablePixelShadingRate = true;
            for (auto& r : v.shadingRateTable) r = NV_PIXEL_X1_PER_RASTER_PIXEL;
            v.shadingRateTable[1] = g.settings.rate[0];
            v.shadingRateTable[2] = g.settings.rate[1];
            v.shadingRateTable[3] = g.settings.rate[2];
            v.shadingRateTable[kIndexHidden] = g.settings.corners == Corners::Cull     ? NV_PIXEL_X0_CULL_RASTER_PIXELS
                                               : g.settings.corners == Corners::Coarse ? NV_PIXEL_X1_PER_4X4_RASTER_PIXELS
                                                                                       : g.settings.rate[2];
        }
        d.numViewports = NV_MAX_NUM_VIEWPORTS;
        if (!g.viewBound) {
            const NvAPI_Status st = NvAPI_D3D11_RSSetShadingRateResourceView(ctx, g.surfaceView.Get());
            if (st != NVAPI_OK) {
                LogOff(std::format("NvAPI_D3D11_RSSetShadingRateResourceView failed ({})", static_cast<int>(st)));
                return false;
            }
            g.viewBound = true;
        }
    }
    const NvAPI_Status st = NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &d);
    if (st != NVAPI_OK) {
        LogOff(std::format("NvAPI_D3D11_RSSetViewportsPixelShadingRates failed ({})", static_cast<int>(st)));
        return false;
    }
    g.vrsOn = on;
    return true;
}

void SwitchOff(ID3D11DeviceContext* ctx) {
    if (g.vrsOn) SetRates(ctx, false);
    g.vrsOn = false;
}

// Size and format of the texture a render target view writes, cached on the view.
struct ViewFacts {
    uint32_t w = 0, h = 0;
    int fmt = 0;
};
// {6C1D7A43-2B7E-4F0A-9E47-5D3C1F0B8A21}
constexpr GUID kViewFactsGuid = {0x6c1d7a43, 0x2b7e, 0x4f0a, {0x9e, 0x47, 0x5d, 0x3c, 0x1f, 0x0b, 0x8a, 0x21}};

ViewFacts Facts(ID3D11RenderTargetView* rtv) {
    ViewFacts f{};
    UINT size = sizeof(f);
    if (SUCCEEDED(rtv->GetPrivateData(kViewFactsGuid, &size, &f)) && size == sizeof(f)) return f;
    f = ViewFacts{};
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    rtv->GetDesc(&vd);
    ComPtr<ID3D11Resource> res;
    rtv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (res && SUCCEEDED(res.As(&tex))) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        UINT mip = 0;
        switch (vd.ViewDimension) {
            case D3D11_RTV_DIMENSION_TEXTURE2D: mip = vd.Texture2D.MipSlice; break;
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: mip = vd.Texture2DArray.MipSlice; break;
            default: break;
        }
        f.w = std::max(1u, td.Width >> mip);
        f.h = std::max(1u, td.Height >> mip);
        f.fmt = static_cast<int>(vd.Format != DXGI_FORMAT_UNKNOWN ? vd.Format : td.Format);
    }
    rtv->SetPrivateData(kViewFactsGuid, sizeof(f), &f);
    return f;
}

bool SizeMatches(const ViewFacts& f) {
    // The scene targets are the eye target's size, rounded up to a multiple of 4 by the engine.
    return g.haveLayout && f.w >= g.layoutW && f.w <= g.layoutW + 16 && f.h >= g.layoutH && f.h <= g.layoutH + 16;
}

int NextTraceQuery(ID3D11DeviceContext* ctx) {
    if (g.traceQueryNext >= static_cast<int>(g.traceQueries.size())) return -1;
    const int i = g.traceQueryNext++;
    ctx->End(g.traceQueries[i].Get());
    return i;
}

void OnTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g.open && !g.vrsOn && !g.tracing) return;
    const int64_t t0 = QpcNow();
    bool want = false;
    ViewFacts f{};
    const char* note = "";
    if (n > 0 && rtvs && rtvs[0]) {
        f = Facts(rtvs[0]);
        if (g.open) {
            ++g.bindingsThisFrame;
            want = SizeMatches(f);
            uint32_t colour = 0;
            for (UINT i = 0; i < n; ++i)
                if (rtvs[i]) ++colour;
            if (want && g.settings.passes == Passes::NoGBuffer && colour >= 3) {
                want = false;
                note = "G-buffer pass skipped";
            }
            if (want && std::find(g.settings.skipFormats.begin(), g.settings.skipFormats.end(), f.fmt) != g.settings.skipFormats.end()) {
                want = false;
                note = "format skipped";
            }
            if (want) ++g.matchedThisFrame;
        }
    } else if (g.open) {
        note = "no colour target";
    }
    if (want != g.vrsOn) SetRates(ctx, want);
    if (g.tracing && g.trace.size() < 2000) {
        State::TraceEvent e;
        e.w = f.w;
        e.h = f.h;
        e.fmt = f.fmt;
        for (UINT i = 0; i < n && rtvs; ++i)
            if (rtvs[i]) ++e.rtvs;
        e.dsv = dsv != nullptr;
        e.inWindow = g.open;
        e.applied = g.vrsOn;
        e.note = note;
        e.query = NextTraceQuery(ctx);
        g.trace.push_back(e);
    }
    g.cpuTicksThisFrame += QpcNow() - t0;
}

void STDMETHODCALLTYPE OMSetRenderTargetsDetour(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    g_omHook.original<OMSetRenderTargetsFn>()(c, n, rtvs, dsv);
    if (c == g_ctx.load(std::memory_order_relaxed)) OnTargets(c, n, rtvs, dsv);
}

void STDMETHODCALLTYPE OMSetRenderTargetsAndUavsDetour(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                       ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavs,
                                                       ID3D11UnorderedAccessView* const* uav, const UINT* counts) {
    g_omUavHook.original<OMSetRenderTargetsAndUavsFn>()(c, n, rtvs, dsv, uavStart, uavs, uav, counts);
    if (c == g_ctx.load(std::memory_order_relaxed) && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) OnTargets(c, n, rtvs, dsv);
}

void STDMETHODCALLTYPE ClearStateDetour(ID3D11DeviceContext* c) {
    g_clearHook.original<ClearStateFn>()(c);
    if (c == g_ctx.load(std::memory_order_relaxed)) {
        // The driver may reset its shading-rate state with the rest: bind again on next use.
        g.viewBound = false;
        if (g.vrsOn) SetRates(c, false);
    }
}

// ---------------------------------------------------------------- the surface

// Tangent space -> pixel position in the eye's rect.
struct EyeMap {
    float x0, y0, w, h, sx, sy, ox, oy;
    void ToPixel(float tx, float ty, float* px, float* py) const {
        const float nx = sx * tx + ox, ny = sy * ty + oy;
        *px = x0 + (nx + 1.0f) * 0.5f * w;
        *py = y0 + (1.0f - ny) * 0.5f * h;
    }
    // Ring distance of a pixel position: tangent distance from the view axis in units of
    // half the eye's width (1 / projection x scale).
    float Distance(float px, float py) const {
        const float nx = (px - x0) / w * 2.0f - 1.0f, ny = 1.0f - (py - y0) / h * 2.0f;
        const float dx = nx - ox, dy = (ny - oy) * sx / sy;
        return std::sqrt(dx * dx + dy * dy);
    }
};

EyeMap MapOf(const FoveationEye& e) {
    return EyeMap{float(e.rect.x), float(e.rect.y), float(e.rect.width), float(e.rect.height), e.projScaleX, e.projScaleY, e.projOffsetX, e.projOffsetY};
}

bool SameLayout(const FoveationEye a[2], const FoveationEye b[2]) {
    for (int e = 0; e < 2; ++e) {
        if (a[e].rect.x != b[e].rect.x || a[e].rect.y != b[e].rect.y || a[e].rect.width != b[e].rect.width || a[e].rect.height != b[e].rect.height)
            return false;
        // Projection offsets carry the temporal AA jitter (a fraction of a pixel): ignore it.
        if (std::fabs(a[e].projOffsetX - b[e].projOffsetX) > 0.004f || std::fabs(a[e].projOffsetY - b[e].projOffsetY) > 0.004f ||
            std::fabs(a[e].projScaleX - b[e].projScaleX) > 0.002f || std::fabs(a[e].projScaleY - b[e].projScaleY) > 0.002f)
            return false;
    }
    return true;
}

// Marks the tiles of `eye` that lie entirely inside the hidden area mesh.
void MarkHidden(std::vector<uint8_t>& tiles, uint32_t tilesX, uint32_t tilesY, const EyeMap& m, const xr::HiddenAreaMesh& mesh) {
    constexpr int kSamples = 3;  // 3x3 sample points per tile: hidden only when all are inside the mesh
    std::vector<uint16_t> hits(size_t(tilesX) * tilesY, 0);
    std::vector<float> px(mesh.xy.size() / 2), py(mesh.xy.size() / 2);
    for (size_t i = 0; i < px.size(); ++i) m.ToPixel(mesh.xy[2 * i], mesh.xy[2 * i + 1], &px[i], &py[i]);
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
        const float ax = px[a], ay = py[a], bx = px[b], by = py[b], cx = px[c], cy = py[c];
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (std::fabs(area) < 1e-3f) continue;
        const int tx0 = std::max(0, int(std::floor(std::min({ax, bx, cx}) / 16.0f)));
        const int tx1 = std::min(int(tilesX) - 1, int(std::floor(std::max({ax, bx, cx}) / 16.0f)));
        const int ty0 = std::max(0, int(std::floor(std::min({ay, by, cy}) / 16.0f)));
        const int ty1 = std::min(int(tilesY) - 1, int(std::floor(std::max({ay, by, cy}) / 16.0f)));
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx)
                for (int s = 0; s < kSamples * kSamples; ++s) {
                    // Samples at the tile's corners, edge middles and centre (pixel centres).
                    const float sx = float(tx) * 16.0f + 0.5f + float(s % kSamples) * 7.5f;
                    const float sy = float(ty) * 16.0f + 0.5f + float(s / kSamples) * 7.5f;
                    const float w0 = (bx - ax) * (sy - ay) - (by - ay) * (sx - ax);
                    const float w1 = (cx - bx) * (sy - by) - (cy - by) * (sx - bx);
                    const float w2 = (ax - cx) * (sy - cy) - (ay - cy) * (sx - cx);
                    const bool inside = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                    if (inside) hits[size_t(ty) * tilesX + tx] |= uint16_t(1u << s);
                }
    }
    constexpr uint16_t kAll = (1u << (kSamples * kSamples)) - 1;
    for (size_t i = 0; i < hits.size(); ++i)
        if (hits[i] == kAll) tiles[i] = kIndexHidden;
}

// Unbinds the current surface and keeps it alive for a few frames: the driver may still
// use it for commands it has not processed yet (NVAPI does not hold a reference).
void RetireSurface(ID3D11DeviceContext* ctx) {
    if (g.viewBound) {
        NvAPI_D3D11_RSSetShadingRateResourceView(ctx, nullptr);
        g.viewBound = false;
    }
    if (g.surfaceView) g.retired.push_back(State::Retired{g.surface, g.surfaceView, 0});
    g.surface.Reset();
    g.surfaceView.Reset();
}

bool BuildSurface(ID3D11DeviceContext* ctx) {
    const uint32_t right = std::max(g.eyes[0].rect.x + g.eyes[0].rect.width, g.eyes[1].rect.x + g.eyes[1].rect.width);
    const uint32_t bottom = std::max(g.eyes[0].rect.y + g.eyes[0].rect.height, g.eyes[1].rect.y + g.eyes[1].rect.height);
    g.layoutW = right;
    g.layoutH = bottom;
    // Covers render targets up to 16 pixels larger than the eye rects (the engine rounds up).
    const uint32_t tilesX = (right + 16 + 15) / 16, tilesY = (bottom + 16 + 15) / 16;
    std::vector<uint8_t> tiles(size_t(tilesX) * tilesY, kIndexFull);
    const Settings& s = g.settings;
    // Per eye: pixel count at each index, for the work estimate.
    double count[2][5]{};
    bool hiddenUsed = false;
    // Per eye: tiles entirely inside its hidden area mesh.
    std::vector<uint8_t> hidden[2];
    for (int e = 0; e < 2; ++e) {
        xr::HiddenAreaMesh mesh;
        uint32_t ver = 0;
        if (s.corners != Corners::Off && XrController::Get().GetHiddenArea(e, &mesh, &ver)) {
            hidden[e].assign(tiles.size(), kIndexFull);
            MarkHidden(hidden[e], tilesX, tilesY, MapOf(g.eyes[e]), mesh);
        }
    }
    const EyeMap maps[2] = {MapOf(g.eyes[0]), MapOf(g.eyes[1])};
    for (uint32_t ty = 0; ty < tilesY; ++ty)
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            // A tile can overlap both eyes when the eye width is not a multiple of 16: it then
            // takes the finer rate of the two and is hidden only if both eyes hide it.
            const float x0 = float(tx) * 16.0f, y0 = float(ty) * 16.0f;
            uint8_t index = 0xFF;
            int owner = -1;
            for (int e = 0; e < 2; ++e) {
                const auto& r = g.eyes[e].rect;
                const float rx0 = float(r.x), ry0 = float(r.y), rx1 = float(r.x + r.width), ry1 = float(r.y + r.height);
                if (x0 + 16.0f <= rx0 || x0 >= rx1 || y0 + 16.0f <= ry0 || y0 >= ry1) continue;
                uint8_t t = kIndexHidden;
                if (hidden[e].empty() || hidden[e][size_t(ty) * tilesX + tx] != kIndexHidden) {
                    // Ring from the tile centre, clamped into this eye's rect.
                    const float cx = std::clamp(x0 + 8.0f, rx0, rx1 - 1.0f), cy = std::clamp(y0 + 8.0f, ry0, ry1 - 1.0f);
                    const float d = maps[e].Distance(cx, cy);
                    t = d < s.radius[0] ? 0 : d < s.radius[1] ? 1 : d < s.radius[2] ? 2 : 3;
                }
                if (index == 0xFF || (t != kIndexHidden && (index == kIndexHidden || t < index))) index = t;
                if (x0 + 8.0f >= rx0 && x0 + 8.0f < rx1 && y0 + 8.0f >= ry0 && y0 + 8.0f < ry1) owner = e;
            }
            if (index == 0xFF) continue;  // outside both eyes: never drawn
            tiles[size_t(ty) * tilesX + tx] = index;
            if (index == kIndexHidden) hiddenUsed = true;
            if (owner >= 0) count[owner][index] += 256.0;
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = tilesX;
    td.Height = tilesY;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{tiles.data(), tilesX, 0};
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = g.device->CreateTexture2D(&td, &init, &tex);
    if (FAILED(hr)) {
        LogOff(std::format("shading-rate surface {}x{} could not be created (0x{:08X})", tilesX, tilesY, static_cast<uint32_t>(hr)));
        return false;
    }
    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = DXGI_FORMAT_R8_UINT;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;
    ComPtr<ID3D11NvShadingRateResourceView> view;
    const NvAPI_Status st = NvAPI_D3D11_CreateShadingRateResourceView(g.device, tex.Get(), &vd, &view);
    if (st != NVAPI_OK) {
        LogOff(std::format("NvAPI_D3D11_CreateShadingRateResourceView failed ({})", static_cast<int>(st)));
        return false;
    }
    SwitchOff(ctx);
    RetireSurface(ctx);
    g.surface = tex;
    g.surfaceView = view;
    g.tilesX = tilesX;
    g.tilesY = tilesY;
    // Relative pixel shader work inside the eye rects, and the share of each region.
    const int px[5] = {1, RatePixels(s.rate[0]), RatePixels(s.rate[1]), RatePixels(s.rate[2]),
                       s.corners == Corners::Cull ? 0 : s.corners == Corners::Coarse ? 16 : RatePixels(s.rate[2])};
    double total = 0, work = 0, share[5]{};
    for (int e = 0; e < 2; ++e)
        for (int i = 0; i < 5; ++i) {
            total += count[e][i];
            share[i] += count[e][i];
            work += px[i] ? count[e][i] / px[i] : 0.0;
        }
    g.workFraction = total > 0 ? work / total : 1.0;
    std::string centres;
    for (int e = 0; e < 2; ++e) {
        float cx = 0, cy = 0;
        MapOf(g.eyes[e]).ToPixel(0, 0, &cx, &cy);
        centres += std::format("{}{} eye rect {},{} {}x{}, optical centre at ({:.0f}, {:.0f}) = {:.1f} % / {:.1f} % of the eye", e ? "; " : "",
                               e ? "right" : "left", g.eyes[e].rect.x, g.eyes[e].rect.y, g.eyes[e].rect.width, g.eyes[e].rect.height, cx, cy,
                               100.0 * (cx - g.eyes[e].rect.x) / g.eyes[e].rect.width, 100.0 * (cy - g.eyes[e].rect.y) / g.eyes[e].rect.height);
    }
    g.surfaceText = std::format("surface {}x{} tiles for {}x{}; pixels: full {:.1f} %, ring 1 {:.1f} %, ring 2 {:.1f} %, outside {:.1f} %, hidden {:.1f} %{}; "
                                "pixel shading work {:.1f} % of full rate",
                                tilesX, tilesY, g.layoutW, g.layoutH, 100 * share[0] / total, 100 * share[1] / total, 100 * share[2] / total,
                                100 * share[3] / total, 100 * share[4] / total, hiddenUsed ? "" : " (no hidden area mesh)", 100 * g.workFraction);
    log::info("foveation: {}; {}", g.surfaceText, centres);
    return true;
}

bool EnsureInit(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (g.init == State::Init::Ok) return true;
    if (g.init == State::Init::Failed) return false;
    g.init = State::Init::Failed;
    NvAPI_Status st = NvAPI_Initialize();
    if (st != NVAPI_OK) {
        LogOff(std::format("NVAPI is not available ({}; not an NVIDIA GPU or driver)", static_cast<int>(st)));
        return false;
    }
    NV_D3D1x_GRAPHICS_CAPS caps{};
    st = NvAPI_D3D1x_GetGraphicsCapabilities(device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
    if (g_simulateUnsupported.load()) caps.bVariablePixelRateShadingSupported = 0;
    if (st != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
        LogOff(st != NVAPI_OK ? std::format("NvAPI_D3D1x_GetGraphicsCapabilities failed ({})", static_cast<int>(st))
                              : std::string("the GPU or driver does not support variable rate shading"));
        return false;
    }
    void** vt = *reinterpret_cast<void***>(ctx);
    if (!g_omHook.create(vt, kOMSetRenderTargets, &OMSetRenderTargetsDetour) ||
        !g_omUavHook.create(vt, kOMSetRenderTargetsAndUavs, &OMSetRenderTargetsAndUavsDetour) ||
        !g_clearHook.create(vt, kClearState, &ClearStateDetour)) {
        LogOff("the device context hooks could not be installed");
        return false;
    }
    NvAPI_ShortString ver{};
    NvU32 driver = 0;
    NvAPI_SYS_GetDriverAndBranchVersion(&driver, ver);
    g.device = device;
    g_ctx = ctx;
    g.init = State::Init::Ok;
    log::info("foveation: variable rate shading available (driver {}.{:02}, {}), context hooks installed", driver / 100, driver % 100, ver);
    return true;
}

void TakeSettings() {
    const uint32_t v = g_settingsVersion.load();
    if (v == g.settingsVersion) return;
    {
        std::lock_guard lk(g_settingsMutex);
        g.settings = g_settings;
    }
    g.settingsVersion = v;
    g.haveLayout = false;  // the next scene builds a new surface (the old one is retired then)
}

void StartTrace(ID3D11DeviceContext* ctx) {
    g.trace.clear();
    g.traceQueryNext = 0;
    if (g.traceQueries.empty()) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP, 0};
        g.traceQueries.resize(2048);
        for (auto& q : g.traceQueries)
            if (FAILED(g.device->CreateQuery(&d, &q))) {
                g.traceQueries.clear();
                break;
            }
        d.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        g.device->CreateQuery(&d, &g.traceDisjoint);
    }
    if (g.traceDisjoint) ctx->Begin(g.traceDisjoint.Get());
    g.tracing = true;
    State::TraceEvent e;
    e.note = "scene begin";
    e.inWindow = true;
    e.query = NextTraceQuery(ctx);
    g.trace.push_back(e);
}

void FinishTrace(ID3D11DeviceContext* ctx) {
    g.traceEndQuery = NextTraceQuery(ctx);
    if (g.traceDisjoint) ctx->End(g.traceDisjoint.Get());
    g.tracing = false;
    g.traceReadPending = true;
    g.traceWaitPresents = 0;
}

void ReadTrace(ID3D11DeviceContext* ctx) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (!g.traceDisjoint || ctx->GetData(g.traceDisjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        if (++g.traceWaitPresents > 30) {
            g.traceReadPending = false;
            log::warn("foveation: trace timings not available");
        }
        return;
    }
    g.traceReadPending = false;
    std::vector<uint64_t> ts(g.traceQueryNext, 0);
    for (int i = 0; i < g.traceQueryNext; ++i) ctx->GetData(g.traceQueries[i].Get(), &ts[i], sizeof(uint64_t), 0);
    const double toMs = dj.Disjoint || !dj.Frequency ? 0.0 : 1000.0 / double(dj.Frequency);
    log::info("foveation: trace of one stereo frame, {} render target bindings from the scene's start to Present ({}):", g.trace.size(),
              Describe(g.settings));
    // Consecutive bindings of the same target are merged; GPU time runs from a binding to the next different one.
    double inWindow = 0, applied = 0, total = 0;
    for (size_t i = 0; i < g.trace.size();) {
        size_t j = i + 1;
        auto same = [&](const State::TraceEvent& a, const State::TraceEvent& b) {
            return a.w == b.w && a.h == b.h && a.fmt == b.fmt && a.rtvs == b.rtvs && a.dsv == b.dsv && a.inWindow == b.inWindow &&
                   a.applied == b.applied && std::string_view(a.note) == b.note;
        };
        while (j < g.trace.size() && same(g.trace[i], g.trace[j])) ++j;
        const int q0 = g.trace[i].query;
        const int q1 = j < g.trace.size() ? g.trace[j].query : g.traceEndQuery;
        double ms = -1;
        if (q0 >= 0 && q1 >= 0 && toMs > 0 && ts[q1] >= ts[q0]) ms = double(ts[q1] - ts[q0]) * toMs;
        const auto& e = g.trace[i];
        if (ms > 0) {
            total += ms;
            if (e.inWindow) inWindow += ms;
            if (e.applied) applied += ms;
        }
        log::info("foveation:   #{:<4} x{:<3} {:>5}x{:<5} {:<30} rt {} {} {} {}{}  gpu {:.3f} ms", i, j - i, e.w, e.h,
                  e.fmt ? std::format("{} ({})", xr::DxgiFormatName(static_cast<DXGI_FORMAT>(e.fmt)), e.fmt) : std::string("-"), e.rtvs, e.dsv ? "+depth" : "      ",
                  e.inWindow ? "scene" : "after", e.applied ? "VRS" : "full", *e.note ? std::string("  (") + e.note + ")" : "", ms);
        i = j;
    }
    log::info("foveation: trace totals: {:.3f} ms GPU from scene start to Present, {:.3f} ms inside the scene window, {:.3f} ms with the mask on",
              total, inWindow, applied);
}

void SetStatusLine() {
    if (g_unsupported.load()) return;  // keeps the reason LogOff wrote
    std::lock_guard lk(g_statusMutex);
    g_statusLine = std::format("{}; {} stereo frames, render target bindings in the scene window {} of which with the mask {}; {}",
                               g.init == State::Init::Ok ? "active" : "waiting for the device", g.frames, g.bindings, g.matched,
                               g.surface ? g.surfaceText : std::string("no surface yet"));
}

}  // namespace

void Configure(const Config& c) {
    Settings s;
    // Default on with the quality preset: in captures at headset resolution it was not
    // distinguishable from full-rate shading outside the outermost ring (docs/render.md).
    const std::string preset = Lower(c.get_string("foveation", "preset", "quality"));
    if (!ApplyPreset(s, preset)) log::warn("foveation: [foveation] preset = '{}' is unknown; using quality", preset);
    // After the preset (which switches it on): enabled = 0 always wins.
    s.enabled = c.get_bool("foveation", "enabled", true) && preset != "off";
    // Explicit values override the preset's.
    const std::string radii = c.get_string("foveation", "radii", "");
    if (!radii.empty()) {
        const auto parts = Split(radii);
        float r[3];
        if (parts.size() == 3 && ParseFloat(parts[0], &r[0]) && ParseFloat(parts[1], &r[1]) && ParseFloat(parts[2], &r[2]) && r[0] <= r[1] &&
            r[1] <= r[2]) {
            std::copy(r, r + 3, s.radius);
            s.preset = "custom";
        } else {
            log::warn("foveation: [foveation] radii = '{}' needs three increasing numbers; ignored", radii);
        }
    }
    const std::string rates = Lower(c.get_string("foveation", "rates", ""));
    if (!rates.empty()) {
        const auto parts = Split(rates);
        NV_PIXEL_SHADING_RATE r[3];
        if (parts.size() == 3 && ParseRate(parts[0], &r[0]) && ParseRate(parts[1], &r[1]) && ParseRate(parts[2], &r[2])) {
            std::copy(r, r + 3, s.rate);
            s.preset = "custom";
        } else {
            log::warn("foveation: [foveation] rates = '{}' needs three of 1x1, 2x1, 1x2, 2x2, 4x2, 2x4, 4x4; ignored", rates);
        }
    }
    const std::string corners = Lower(c.get_string("foveation", "hidden_area", "coarse"));
    s.corners = corners == "off" ? Corners::Off : corners == "cull" ? Corners::Cull : Corners::Coarse;
    const std::string passes = Lower(c.get_string("foveation", "passes", "scene"));
    s.passes = passes == "all" ? Passes::All : passes == "no-gbuffer" ? Passes::NoGBuffer : Passes::Scene;
    // Default: the velocity buffer (R16G16_UNORM, 35): temporal data read per pixel by
    // temporal AA and motion blur, and almost free to shade at full rate.
    s.skipFormats.clear();
    for (const auto& f : Split(c.get_string("foveation", "skip_formats", "35"))) {
        int v = 0;
        auto [p, ec] = std::from_chars(f.data(), f.data() + f.size(), v);
        if (ec == std::errc() && p == f.data() + f.size()) s.skipFormats.push_back(v);
    }
    {
        std::lock_guard lk(g_settingsMutex);
        g_settings = s;
    }
    g_settingsVersion.fetch_add(1);
    g_enabled = s.enabled;
    g_measure = s.enabled;
    g_simulateUnsupported = c.get_bool("debug", "foveation_unsupported", false);
    log::info("foveation: {}", Describe(s));
}

bool Wanted() {
    return (g_enabled.load(std::memory_order_relaxed) || g_measure.load(std::memory_order_relaxed)) && !g_unsupported.load(std::memory_order_relaxed);
}

void OnPresent(const PresentInfo& p) {
    if (g.init == State::Init::NotYet) {
        // NVAPI and the context hooks wait for the first stereo scene: screen mode and
        // the game without stereo never load or hook anything for this feature.
        g_seenDevice = p.device;
        g_seenCtx = p.context;
        return;
    }
    ID3D11DeviceContext* ctx = p.context;
    if (g.init != State::Init::Ok || ctx != g_ctx.load()) return;  // off, or another device: leave it alone
    // Nothing after the scene of a stereo frame, and nothing of this module, is shaded coarsely.
    g.open = false;
    if (g.timing) {
        g.sceneTimer.End(ctx);
        g.timing = false;
    }
    SwitchOff(ctx);
    if (g.afterRunning) {
        g.afterTimer.End(ctx);
        g.afterRunning = false;
    }
    if (g.tracing) FinishTrace(ctx);
    if (g.traceReadPending) ReadTrace(ctx);
    for (auto& r : g.retired) ++r.presents;
    std::erase_if(g.retired, [](const State::Retired& r) { return r.presents > 8; });
    g.sceneTimer.Collect(ctx, g_sceneSeries);
    g.afterTimer.Collect(ctx, g_afterSeries);
    if (g.cpuTicksThisFrame || g.bindingsThisFrame) {
        g_cpuSeries.Add(QpcToMs(g.cpuTicksThisFrame));
        g.bindings += g.bindingsThisFrame;
        g.matched += g.matchedThisFrame;
    }
    g.cpuTicksThisFrame = 0;
    g.bindingsThisFrame = g.matchedThisFrame = 0;
    static uint32_t presents = 0;
    if ((++presents & 127) == 0) SetStatusLine();
}

void SceneBegin(const FoveationEye eyes[2]) {
    if (g.init == State::Init::NotYet && Wanted() && g_seenCtx.load()) EnsureInit(g_seenDevice.load(), g_seenCtx.load());
    if (g.init != State::Init::Ok || g_unsupported.load()) return;
    ID3D11DeviceContext* ctx = g_ctx.load();
    TakeSettings();
    if (g.afterRunning) {
        g.afterTimer.End(ctx);
        g.afterRunning = false;
    }
    g.timing = true;
    g.sceneTimer.Begin(g.device, ctx);
    if (!g.settings.enabled) {
        SwitchOff(ctx);
        return;
    }
    uint32_t hiddenVersion = 0;
    XrController::Get().GetHiddenArea(0, nullptr, &hiddenVersion);
    if (!g.haveLayout || !SameLayout(g.eyes, eyes) || !g.surface || hiddenVersion != g.hiddenVersion) {
        std::copy(eyes, eyes + 2, g.eyes);
        g.hiddenVersion = hiddenVersion;
        g.haveLayout = BuildSurface(ctx);
        if (!g.haveLayout) return;
    }
    ++g.frames;
    g.open = true;
    if (g_traceRequested.exchange(false)) StartTrace(ctx);
    // The render target bound right now may already be a scene target.
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    OnTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    for (auto* r : rtvs)
        if (r) r->Release();
    if (dsv) dsv->Release();
}

void SceneEnd() {
    if (g.init != State::Init::Ok || !g.timing) return;
    ID3D11DeviceContext* ctx = g_ctx.load();
    g.timing = false;
    g.sceneTimer.End(ctx);
    g.afterTimer.Begin(g.device, ctx);
    g.afterRunning = true;
    if (g.tracing) {
        State::TraceEvent e;
        e.note = "scene end";
        e.query = NextTraceQuery(ctx);
        g.trace.push_back(e);
    }
    if (g.settings.passes == Passes::All) return;  // the mask stays on for matching targets until Present
    g.open = false;
    SwitchOff(ctx);
}

std::string Status() {
    std::string line;
    {
        std::lock_guard lk(g_statusMutex);
        line = g_statusLine;
    }
    Settings s;
    {
        std::lock_guard lk(g_settingsMutex);
        s = g_settings;
    }
    return std::format("foveation {}; {}", Describe(s), line);
}

std::string Command(const std::string& argsIn) {
    const auto a = Split(Lower(argsIn));
    auto update = [](auto&& change) {
        std::string err;
        {
            std::lock_guard lk(g_settingsMutex);
            Settings s = g_settings;
            err = change(s);
            if (err.empty()) {
                g_settings = s;
                g_enabled = s.enabled;
                g_measure = true;
            }
        }
        if (!err.empty()) return "err " + err;
        g_settingsVersion.fetch_add(1);
        return "ok " + Status();
    };
    if (a.empty() || a[0] == "status") {
        SetStatusLine();
        return "ok " + Status();
    }
    if (a[0] == "on" || a[0] == "off")
        return update([&](Settings& s) {
            s.enabled = a[0] == "on";
            return std::string();
        });
    if (a[0] == "preset" && a.size() == 2)
        return update([&](Settings& s) { return ApplyPreset(s, a[1]) ? std::string() : "unknown preset " + a[1]; });
    if (a[0] == "radii" && a.size() == 4)
        return update([&](Settings& s) {
            float r[3];
            if (!ParseFloat(a[1], &r[0]) || !ParseFloat(a[2], &r[1]) || !ParseFloat(a[3], &r[2]) || r[0] > r[1] || r[1] > r[2])
                return std::string("radii need three increasing numbers");
            std::copy(r, r + 3, s.radius);
            s.preset = "custom";
            return std::string();
        });
    if (a[0] == "rates" && a.size() == 4)
        return update([&](Settings& s) {
            NV_PIXEL_SHADING_RATE r[3];
            if (!ParseRate(a[1], &r[0]) || !ParseRate(a[2], &r[1]) || !ParseRate(a[3], &r[2])) return std::string("rates: 1x1 2x1 1x2 2x2 4x2 2x4 4x4");
            std::copy(r, r + 3, s.rate);
            s.preset = "custom";
            return std::string();
        });
    if (a[0] == "hidden" && a.size() == 2)
        return update([&](Settings& s) {
            if (a[1] != "off" && a[1] != "coarse" && a[1] != "cull") return std::string("hidden off|coarse|cull");
            s.corners = a[1] == "off" ? Corners::Off : a[1] == "cull" ? Corners::Cull : Corners::Coarse;
            return std::string();
        });
    if (a[0] == "passes" && a.size() == 2)
        return update([&](Settings& s) {
            if (a[1] != "scene" && a[1] != "all" && a[1] != "no-gbuffer") return std::string("passes scene|no-gbuffer|all");
            s.passes = a[1] == "all" ? Passes::All : a[1] == "no-gbuffer" ? Passes::NoGBuffer : Passes::Scene;
            return std::string();
        });
    if (a[0] == "skip")
        return update([&](Settings& s) {
            s.skipFormats.clear();
            for (size_t i = 1; i < a.size(); ++i) {
                int v = 0;
                auto [p, ec] = std::from_chars(a[i].data(), a[i].data() + a[i].size(), v);
                if (ec != std::errc()) return std::string("skip <dxgi format numbers...>");
                s.skipFormats.push_back(v);
            }
            return std::string();
        });
    if (a[0] == "trace") {
        g_traceRequested = true;
        return "ok the next stereo frame's render target bindings and GPU times go to ff7vr.log";
    }
    if (a[0] == "timing") {
        std::string out = "ok";
        for (Series* s : {&g_sceneSeries, &g_afterSeries, &g_cpuSeries}) {
            const std::string l = s->TakeSummary();
            if (!l.empty()) {
                log::info("timing:   {}", l);
                out += "; " + l;
            }
        }
        return out;
    }
    return "err usage: fov status | on | off | preset quality|balanced|performance|off | radii <r1> <r2> <r3> | rates <a> <b> <c> | "
           "hidden off|coarse|cull | passes scene|no-gbuffer|all | skip [dxgi formats] | trace | timing";
}

std::vector<std::string> TakeTimingLines() {
    std::vector<std::string> out;
    for (Series* s : {&g_sceneSeries, &g_afterSeries, &g_cpuSeries}) {
        std::string l = s->TakeSummary();
        if (!l.empty()) out.push_back(std::move(l));
    }
    return out;
}

}  // namespace ff7vr::render::foveation
