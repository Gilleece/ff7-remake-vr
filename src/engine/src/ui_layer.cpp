// The game's in-game UI on its own layer in stereo (see ff7vr/engine/ui_layer.h).
//
// How this build draws the UI (docs/re/engine.md, "In-game UI"):
//
//   FDeferredShadingSceneRenderer::Render (render thread), after the scene, if
//   ViewFamily flag 0x80 (+0x3C) is set, for each view:
//       if (BeginRenderingInGameUI(SceneContext, RHICmdList, View)) {   // allocates the pooled
//           ... the Renderer module's UI render delegates draw into it ...  // target, binds it with
//           EndRenderingInGameUI(SceneContext, RHICmdList);               // a clear, sets the viewport
//       }
//   then post-processing for each view; every material shader that has the
//   InGameUITexture parameter binds
//       (View.Family->flags & 0x80) && InGameUIRenderTarget ? InGameUIRenderTarget->ShaderResourceTexture
//                                                           : the engine's empty fallback texture
//   and composites it over the view.
//
// While the render module's UI layer is active and the view is a stereo eye:
//   BeginRenderingInGameUI: the first eye of the family draws the UI as usual; a later eye
//       of the same family is refused (returns false: the engine skips the pass), so the UI
//       is drawn once per frame.
//   EndRenderingInGameUI: after the engine's own code, the family's flag 0x80 is cleared,
//       so post-processing binds the empty fallback texture (no UI in the eye images), and
//       an RHI command is appended that hands the UI texture to the render module on the
//       presenting thread. It runs after the UI pass has executed on the D3D11 context and
//       before the frame's Present, so the texture holds exactly this frame's UI.
// Everything else (mono frames, screen mode, the plain game) runs the engine's code unchanged.
//
// Scene markers for foveated rendering (render.h, "FIXED FOVEATED RENDERING"), only while
// the render module wants them:
//   FDeferredShadingSceneRenderer::Render: for a stereo view family (two views, left and
//       right eye) an RHI command is appended before the engine's own work that hands the
//       render module both eyes' view rects and projections (FoveationSceneBegin).
//   BeginRenderingInGameUI / FPostProcessing::Process / the end of Render: the first of
//       them appends FoveationSceneEnd. So the scene's draws are marked, and the UI pass,
//       post-processing and everything after are not.

#include "ff7vr/engine/ui_layer.h"

#include "rhi_command.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/core/pattern.h"
#include "ff7vr/engine/stereo_abi.h"

#if FF7VR_ENGINE_WITH_RENDER
#include "ff7vr/render/render.h"
#endif

#include <d3d11.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace ff7vr::engine {
namespace {

// ------------------------------------------------------------------ signatures
// Names, patterns and rules as in tools/re/signatures.json; expect = value for file version 1.0.0.7.
enum class Rule { Match, I32, U8, Call };
struct Sig {
    const char* name;
    const char* pattern;
    Rule rule;
    int offset;
    std::int64_t expect;
};

constexpr const char* kBeginPattern =
    "40 55 53 56 57 41 57 48 8D AC 24 F0 FE FF FF 48 81 EC 10 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 00 01 00 00 45 33 FF "
    "4C 8D 89 10 01 00 00";
constexpr Sig kBegin{"FSceneRenderTargets::BeginRenderingInGameUI", kBeginPattern, Rule::Match, 0, 0x2541670};
constexpr Sig kUiTarget{"FSceneRenderTargets::InGameUIRenderTarget", kBeginPattern, Rule::I32, 45, 0x110};
constexpr Sig kEnd{"FSceneRenderTargets::EndRenderingInGameUI",
                   "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 66 0F 6F 05 ?? ?? ?? ?? 48 8B F2 66 0F 6F "
                   "0D ?? ?? ?? ?? 48 8B 5A 30 F3 0F 7F 40 C8 48 83 C3 07 66 0F 6F 05 ?? ?? ?? ?? F3 0F 7F 40 E8 F3 0F 7F 48 D8 48 8B "
                   "81 10 01 00 00",
                   Rule::Match, 0, 0x2541910};
constexpr Sig kUiSize{"FSceneRenderTargets::InGameUISize", "89 87 40 02 00 00 89 8F 44 02 00 00", Rule::I32, 2, 0x240};
// FDeferredShadingSceneRenderer::Render: `test byte ptr [rdi+4Ch], 80h; jz` skips the UI
// pass. The renderer's ViewFamily copy starts at +0x10, so the flag is at family +0x3C.
constexpr Sig kFamilyFlag{"FSceneViewFamily in-game UI flag", "F6 47 4C 80 0F 84", Rule::U8, 2, 0x4c};
constexpr std::size_t kRendererViewFamily = 0x10;
constexpr std::uint8_t kFamilyFlagBit = 0x80;
// The same flag tested by every InGameUITexture binding (`test byte ptr [r14+3Ch], 80h`, about 320 sites).
constexpr const char* kBindingSitePattern = "41 F6 46 3C 80";
constexpr Sig kStereoPass{"FSceneView::StereoPass", "83 BF ?? ?? ?? ?? 00 74 0E 48 8B 01", Rule::I32, 2, 0x970};
// Scene markers (tools/re/signatures.json).
constexpr Sig kRender{"FDeferredShadingSceneRenderer::Render", "40 55 53 56 57 41 56 48 8D AC 24 80 FD FF FF", Rule::Match, 0, 0x21e64a0};
// Its call in Render's per-view loop (`call rel32` at +14 of the pattern).
constexpr Sig kProcess{"FPostProcessing::Process",
                       "48 8D 0D ?? ?? ?? ?? 4C 03 87 D0 00 00 00 E8 ?? ?? ?? ?? FF C3 3B 9F D8 00 00 00 7C D1 48 8B CE", Rule::Call, 14,
                       0x251c230};
// The UI loop in Render walks Views: `imul r8, rax, 28B0h` ... `add r8, [rdi+0D0h]`.
constexpr const char* kViewsPattern = "48 8D 0D ?? ?? ?? ?? 4C 69 C0 B0 28 00 00 48 8B D6 4C 03 87 D0 00 00 00 E8 ?? ?? ?? ?? 84 C0";
constexpr Sig kViewStride{"FViewInfo size", kViewsPattern, Rule::I32, 10, 0x28b0};
constexpr Sig kRendererViews{"FSceneRenderer::Views", kViewsPattern, Rule::I32, 20, 0xd0};
// IPooledRenderTarget: FSceneRenderTargetItem at +8 {TargetableTexture, ShaderResourceTexture, ...}
// (EndRenderingInGameUI reads both).
constexpr std::size_t kPooledTargetable = 0x08;

// ------------------------------------------------------------------ state
struct Offsets {
    std::size_t uiTarget = 0, uiSize = 0, familyFlags = 0, stereoPass = 0;
};
Offsets g_off;
hook::InlineHook g_beginHook, g_endHook;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_oncePerFrame{true};

// Render thread only (both hooks run there, in this order, inside one UI pass).
struct Pass {
    void* sceneTargets = nullptr;
    std::uint8_t* family = nullptr;  // set when this pass is redirected
    bool report = false;
};
Pass g_pass;

// Counters for `uihook status`.
std::atomic<std::uint64_t> g_passes{0}, g_redirected{0}, g_skipped{0}, g_reports{0}, g_reportFailures{0};
std::atomic<std::uint32_t> g_lastW{0}, g_lastH{0};
std::atomic<int> g_lastFormat{0};
std::atomic<int> g_projScans{0};  // `uihook proj`: views still to scan for projection matrices

// ------------------------------------------------------------------ scene markers
struct MarkOffsets {
    std::size_t views = 0, viewStride = 0, stereoPass = 0;
    int rect = -1, proj = -1;  // inside FViewInfo, found at the first stereo frame
};
MarkOffsets g_mark;
hook::InlineHook g_renderHook, g_processHook;
std::atomic<bool> g_marksReady{false};
bool g_layoutSearched = false;  // render thread
bool g_sceneOpen = false;       // render thread: a begin was appended and no end yet
std::atomic<std::uint64_t> g_marksBegun{0}, g_marksEnded{0};

void close_scene(void* cmdList);

using BeginFn = bool(__fastcall*)(void* sceneTargets, void* cmdList, void* view);
using EndFn = void(__fastcall*)(void* sceneTargets, void* cmdList);

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<std::uintptr_t>(p) + n <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool layer_wanted() {
#if FF7VR_ENGINE_WITH_RENDER
    return render::UiLayerWanted();
#else
    return false;
#endif
}

bool dump_requested() {
#if FF7VR_ENGINE_WITH_RENDER
    return render::UiDumpRequested();
#else
    return false;
#endif
}

// ------------------------------------------------------------------ the RHI command
struct ReportCommand : rhi::Command {
    void* rhiTexture = nullptr;  // FRHITexture2D* (targetable texture of the pooled UI target)
    std::uint32_t width = 0, height = 0;
    bool redirected = false;
};
// The command list only links our memory; a command executes within a frame or two.
constexpr int kRing = 16;
ReportCommand g_ring[kRing];
int g_ringNext = 0;

void execute_report(void*, rhi::Command* self) {
    // Presenting thread (the RHI thread), between the UI pass and the frame's Present.
    auto* c = static_cast<ReportCommand*>(self);
    if (!c->rhiTexture) return;
    auto* native = *reinterpret_cast<ID3D11Texture2D**>(static_cast<std::uint8_t*>(c->rhiTexture) + ue::offsets::FD3D11Texture2D_Resource);
    if (!native) return;
    D3D11_TEXTURE2D_DESC d{};
    native->GetDesc(&d);
    g_lastFormat = static_cast<int>(d.Format);
#if FF7VR_ENGINE_WITH_RENDER
    render::UiLayerSource s;
    s.texture = native;
    s.width = std::min<std::uint32_t>(c->width ? c->width : d.Width, d.Width);
    s.height = std::min<std::uint32_t>(c->height ? c->height : d.Height, d.Height);
    // The engine creates the UI target with TexCreate_SRGB: a typeless BGRA texture written
    // through an sRGB view, so it holds linear values when read through the sRGB view.
    switch (d.Format) {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            s.viewFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            s.encoding = xr::ColorEncoding::Linear;
            break;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            s.viewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            s.encoding = xr::ColorEncoding::Linear;
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            s.viewFormat = d.Format;
            s.encoding = xr::ColorEncoding::Linear;
            break;
        default:
            s.viewFormat = d.Format;
            s.encoding = xr::ColorEncoding::Srgb;
            break;
    }
    // Unreal's convention for this target: cleared to (0,0,0,1), composite = scene * a + rgb.
    s.alpha = xr::SourceAlpha::PremultipliedInverted;
    s.redirected = c->redirected;
    render::SubmitUiLayer(s);
#endif
}

void report(void* cmdList, void* sceneTargets, bool redirected) {
    auto* st = static_cast<std::uint8_t*>(sceneTargets);
    void* pooled = *reinterpret_cast<void**>(st + g_off.uiTarget);
    if (!pooled || !readable(pooled, kPooledTargetable + 8)) return;
    void* tex = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(pooled) + kPooledTargetable);
    if (!tex) return;
    ReportCommand& c = g_ring[g_ringNext];
    g_ringNext = (g_ringNext + 1) % kRing;
    c.execute = &execute_report;
    c.rhiTexture = tex;
    c.width = *reinterpret_cast<std::uint32_t*>(st + g_off.uiSize);
    c.height = *reinterpret_cast<std::uint32_t*>(st + g_off.uiSize + 4);
    c.redirected = redirected;
    g_lastW = c.width;
    g_lastH = c.height;
    if (rhi::enqueue(cmdList, &c))
        ++g_reports;
    else
        ++g_reportFailures;
}

// Development aid (`uihook proj`): finds perspective projection matrices in a view the UI
// pass receives and logs their field of view. In a mono frame that is the game camera's
// projection, the one the game places world-anchored UI elements (markers, names) with.
// Looks for this engine's reversed-Z infinite form [xs 0 0 0; 0 ys 0 0; ox oy 0 1; 0 0 n 0].
void log_projections(const std::uint8_t* view) {
    constexpr std::size_t kScan = 0x2000;
    if (!readable(view, kScan)) return;
    const std::int32_t pass = *reinterpret_cast<const std::int32_t*>(view + g_off.stereoPass);
    int found = 0;
    for (std::size_t off = 0; off + 64 <= kScan && found < 8; off += 16) {
        const float* m = reinterpret_cast<const float*>(view + off);
        if (m[1] != 0 || m[2] != 0 || m[3] != 0 || m[4] != 0 || m[6] != 0 || m[7] != 0 || m[10] != 0 || m[11] != 1.0f || m[12] != 0 ||
            m[13] != 0 || m[15] != 0)
            continue;
        if (!(m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f && m[14] > 0.0f && m[14] < 1000.0f)) continue;
        const double l = (-1.0 - m[8]) / m[0], r = (1.0 - m[8]) / m[0];  // tangents (left negative)
        const double d = (-1.0 - m[9]) / m[5], u = (1.0 - m[9]) / m[5];
        constexpr double kDeg = 57.29577951308232;
        log::info("ui: view (stereo pass {}) +0x{:x}: projection xs {:.5f} ys {:.5f} offset {:.4f} {:.4f} near {:.2f}: horizontal {:.2f} deg "
                  "(left {:.2f} right {:.2f}), vertical {:.2f} deg (up {:.2f} down {:.2f}), aspect {:.4f}",
                  pass, off, m[0], m[5], m[8], m[9], m[14], (std::atan(r) - std::atan(l)) * kDeg, std::atan(l) * kDeg, std::atan(r) * kDeg,
                  (std::atan(u) - std::atan(d)) * kDeg, std::atan(u) * kDeg, std::atan(d) * kDeg, (r - l) / (u - d));
        ++found;
    }
    if (!found) log::info("ui: view (stereo pass {}): no projection matrix found in the first 0x{:x} bytes", pass, kScan);
}

// ------------------------------------------------------------------ hooks (render thread)
bool __fastcall begin_detour(void* sceneTargets, void* cmdList, void* view) {
    const auto original = g_beginHook.original<BeginFn>();
    g_pass = Pass{};
    if (g_sceneOpen) close_scene(cmdList);  // the scene is done: the UI pass gets no foveation
    if (!g_ready.load(std::memory_order_relaxed) || !view) return original(sceneTargets, cmdList, view);
    ++g_passes;
    auto* v = static_cast<std::uint8_t*>(view);
    if (g_projScans.load(std::memory_order_relaxed) > 0) {
        g_projScans.fetch_sub(1);
        log_projections(v);
    }
    auto* family = *reinterpret_cast<std::uint8_t**>(v);
    const std::int32_t pass = *reinterpret_cast<std::int32_t*>(v + g_off.stereoPass);
    const bool redirect = family && pass != 0 && layer_wanted();
    if (redirect && g_oncePerFrame.load(std::memory_order_relaxed) && !(family[g_off.familyFlags] & kFamilyFlagBit)) {
        // An earlier eye of this view family drew the UI already (we cleared the flag after
        // it). The UI does not depend on the eye, so once per frame is enough.
        ++g_skipped;
        return false;
    }
    const bool r = original(sceneTargets, cmdList, view);
    if (r) {
        g_pass.sceneTargets = sceneTargets;
        g_pass.family = redirect ? family : nullptr;
        g_pass.report = redirect || dump_requested();
    }
    return r;
}

void __fastcall end_detour(void* sceneTargets, void* cmdList) {
    g_endHook.original<EndFn>()(sceneTargets, cmdList);
    if (g_pass.sceneTargets != sceneTargets) return;
    if (g_pass.family) {
        // Post-processing now binds the empty fallback texture instead of the UI.
        g_pass.family[g_off.familyFlags] &= static_cast<std::uint8_t>(~kFamilyFlagBit);
        ++g_redirected;
    }
    if (g_pass.report) report(cmdList, sceneTargets, g_pass.family != nullptr);
    g_pass = Pass{};
}

// ------------------------------------------------------------------ scene markers
#if FF7VR_ENGINE_WITH_RENDER
struct MarkCommand : rhi::Command {
    bool begin = false;
    render::FoveationEye eyes[2]{};
};
constexpr int kMarkRing = 32;
MarkCommand g_marks[kMarkRing];
int g_markNext = 0;

void execute_mark(void*, rhi::Command* self) {
    // Presenting thread (RHI thread), in the frame's command order.
    auto* c = static_cast<MarkCommand*>(self);
    if (c->begin)
        render::FoveationSceneBegin(c->eyes);
    else
        render::FoveationSceneEnd();
}

bool append_mark(void* cmdList, bool begin, const render::FoveationEye* eyes) {
    MarkCommand& c = g_marks[g_markNext];
    g_markNext = (g_markNext + 1) % kMarkRing;
    c.execute = &execute_mark;
    c.begin = begin;
    if (eyes) std::copy(eyes, eyes + 2, c.eyes);
    return rhi::enqueue(cmdList, &c);
}

// This engine's reversed-Z infinite projection: [xs 0 0 0; 0 ys 0 0; ox oy 0 1; 0 0 n 0].
bool is_projection(const float* m) {
    return m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[6] == 0 && m[7] == 0 && m[10] == 0 && m[11] == 1.0f && m[12] == 0 &&
           m[13] == 0 && m[15] == 0 && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f && m[14] > 0.0f && m[14] < 1000.0f &&
           std::fabs(m[8]) < 1.0f && std::fabs(m[9]) < 1.0f;
}

// Finds, once, where FViewInfo keeps the view rect and the projection matrix: the first
// FIntRect that reads (0, 0, W, H) in the left eye and (W, 0, 2W, H) in the right eye, and
// the first matrix of the projection's form in both. Logged; the markers stay off if either
// is missing.
void find_view_layout(const std::uint8_t* left, const std::uint8_t* right) {
    g_layoutSearched = true;
    const std::size_t span = g_mark.viewStride - 64;
    if (!readable(left, span) || !readable(right, span)) return;
    for (std::size_t o = 0; o + 16 <= span && g_mark.rect < 0; o += 4) {
        const auto* a = reinterpret_cast<const std::int32_t*>(left + o);
        const auto* b = reinterpret_cast<const std::int32_t*>(right + o);
        if (a[0] == 0 && a[1] == 0 && a[2] >= 64 && a[2] <= 16384 && a[3] >= 64 && a[3] <= 16384 && b[0] == a[2] && b[1] == 0 &&
            b[2] == 2 * a[2] && b[3] == a[3])
            g_mark.rect = static_cast<int>(o);
    }
    for (std::size_t o = 0; o + 64 <= span && g_mark.proj < 0; o += 16)
        if (is_projection(reinterpret_cast<const float*>(left + o)) && is_projection(reinterpret_cast<const float*>(right + o)))
            g_mark.proj = static_cast<int>(o);
    if (g_mark.rect < 0 || g_mark.proj < 0) {
        log::warn("foveation: view rect (+0x{:x}) or projection (+0x{:x}) not found in the eye views; no foveated rendering", g_mark.rect,
                  g_mark.proj);
        return;
    }
    const float* m = reinterpret_cast<const float*>(left + g_mark.proj);
    const auto* r = reinterpret_cast<const std::int32_t*>(left + g_mark.rect);
    log::info("foveation: eye views: rect at +0x{:x} ({}x{}), projection at +0x{:x} (left eye scale {:.4f} {:.4f}, axis at NDC {:.4f} {:.4f})",
              g_mark.rect, r[2], r[3], g_mark.proj, m[0], m[5], m[8], m[9]);
}

// Both eyes of a stereo view family, or false.
bool stereo_eyes(void* renderer, render::FoveationEye out[2]) {
    auto* r = static_cast<std::uint8_t*>(renderer);
    auto* views = *reinterpret_cast<std::uint8_t**>(r + g_mark.views);
    const std::int32_t num = *reinterpret_cast<std::int32_t*>(r + g_mark.views + 8);
    if (!views || num != 2) return false;
    const std::uint8_t* v[2] = {views, views + g_mark.viewStride};
    if (*reinterpret_cast<const std::int32_t*>(v[0] + g_mark.stereoPass) != 1 || *reinterpret_cast<const std::int32_t*>(v[1] + g_mark.stereoPass) != 2)
        return false;
    if (!g_layoutSearched) find_view_layout(v[0], v[1]);
    if (g_mark.rect < 0 || g_mark.proj < 0) return false;
    for (int e = 0; e < 2; ++e) {
        const auto* rc = reinterpret_cast<const std::int32_t*>(v[e] + g_mark.rect);
        const float* m = reinterpret_cast<const float*>(v[e] + g_mark.proj);
        if (rc[2] <= rc[0] || rc[3] <= rc[1] || rc[0] < 0 || rc[1] < 0 || !(m[0] > 0.0f) || !(m[5] > 0.0f)) return false;
        out[e].rect = xr::Rect{rc[0], rc[1], static_cast<std::uint32_t>(rc[2] - rc[0]), static_cast<std::uint32_t>(rc[3] - rc[1])};
        out[e].projScaleX = m[0];
        out[e].projScaleY = m[5];
        out[e].projOffsetX = m[8];
        out[e].projOffsetY = m[9];
    }
    return true;
}

void close_scene(void* cmdList) {
    g_sceneOpen = false;
    if (append_mark(cmdList, false, nullptr)) ++g_marksEnded;
}

using RenderFn = void(__fastcall*)(void* renderer, void* cmdList);
using ProcessFn = void(__fastcall*)(void* self, void* cmdList, void* view, void* velocity);

void __fastcall render_detour(void* renderer, void* cmdList) {
    if (g_sceneOpen) close_scene(cmdList);
    render::FoveationEye eyes[2];
    if (g_marksReady.load(std::memory_order_relaxed) && render::FoveationWanted() && stereo_eyes(renderer, eyes) && append_mark(cmdList, true, eyes)) {
        g_sceneOpen = true;
        ++g_marksBegun;
    }
    g_renderHook.original<RenderFn>()(renderer, cmdList);
    if (g_sceneOpen) close_scene(cmdList);  // no UI pass and no post-processing ran
}

void __fastcall process_detour(void* self, void* cmdList, void* view, void* velocity) {
    if (g_sceneOpen) close_scene(cmdList);
    g_processHook.original<ProcessFn>()(self, cmdList, view, velocity);
}

void start_scene_marks(std::uintptr_t base, bool known);
#else
void close_scene(void*) {}
#endif

// ------------------------------------------------------------------ start-up
bool resolve(std::uintptr_t base, bool known, const Sig& s, std::int64_t* out, std::string* why) {
    const auto r = pattern::scan_module(base, s.pattern, pattern::Sections::Executable, 2);
    if (!r.unique()) {
        *why = std::format("{}: {} matches", s.name, r.matches.size());
        return false;
    }
    std::int64_t v = 0;
    switch (s.rule) {
        case Rule::Match: v = static_cast<std::int64_t>(r.first() + s.offset - base); break;
        case Rule::I32: v = *reinterpret_cast<const std::int32_t*>(r.first() + s.offset); break;
        case Rule::U8: v = *reinterpret_cast<const std::uint8_t*>(r.first() + s.offset); break;
        case Rule::Call:
            v = static_cast<std::int64_t>(r.first() + s.offset + 5 + *reinterpret_cast<const std::int32_t*>(r.first() + s.offset + 1) - base);
            break;
    }
    if (known && v != s.expect) {
        *why = std::format("{}: 0x{:x}, expected 0x{:x} for this build", s.name, v, s.expect);
        return false;
    }
    log::debug("ui: {} = 0x{:x}", s.name, v);
    *out = v;
    return true;
}

std::string status() {
    return std::format("ok ui hooks {}; once per frame {}; UI passes {} redirected {} skipped (second eye) {} reports {} failed {}; last UI "
                       "target {}x{} DXGI format {}; foveation scene markers {}, scenes begun {} ended {}",
                       g_ready.load() ? "installed" : "off", g_oncePerFrame.load() ? "on" : "off", g_passes.load(), g_redirected.load(),
                       g_skipped.load(), g_reports.load(), g_reportFailures.load(), g_lastW.load(), g_lastH.load(), g_lastFormat.load(),
                       g_marksReady.load() ? "installed" : "off", g_marksBegun.load(), g_marksEnded.load());
}

#if FF7VR_ENGINE_WITH_RENDER
void start_scene_marks(std::uintptr_t base, bool known) {
    std::string why;
    std::int64_t render = 0, process = 0, stride = 0, views = 0, pass = 0;
    if (!(resolve(base, known, kRender, &render, &why) && resolve(base, known, kProcess, &process, &why) &&
          resolve(base, known, kViewStride, &stride, &why) && resolve(base, known, kRendererViews, &views, &why) &&
          resolve(base, known, kStereoPass, &pass, &why))) {
        log::warn("foveation: scene markers not available: {} (no foveated rendering)", why);
        return;
    }
    if (!rhi::verify_layout(base)) {
        log::warn("foveation: RHI command list layout not confirmed (no foveated rendering)");
        return;
    }
    g_mark.views = static_cast<std::size_t>(views);
    g_mark.viewStride = static_cast<std::size_t>(stride);
    g_mark.stereoPass = static_cast<std::size_t>(pass);
    if (!g_renderHook.create(reinterpret_cast<void*>(base + render), &render_detour) ||
        !g_processHook.create(reinterpret_cast<void*>(base + process), &process_detour)) {
        log::warn("foveation: scene marker hooks could not be installed (no foveated rendering)");
        return;
    }
    g_marksReady = true;
    log::info("foveation: scene markers installed (Render +0x{:x}, FPostProcessing::Process +0x{:x}, views at renderer +0x{:x}, 0x{:x} bytes each)",
              render, process, views, stride);
}
#endif

}  // namespace

bool start_ui_layer(const StartupContext& ctx) {
    if (!ctx.is_game || !ctx.config) return false;
    if (!ctx.config->get_bool("stereo", "enabled", true)) return false;  // nothing to do without the stereo device (same default as engine.cpp)
#if !FF7VR_ENGINE_WITH_RENDER
    log::info("ui: the render module is not part of this build; the UI stays in the eye images");
    return false;
#else
    const std::uintptr_t base = ctx.game_base ? ctx.game_base : module::main_module().base;
    const bool known = ctx.game_size == ue::rva_1_0_0_7::SizeOfImage && module::timestamp(base) == ue::rva_1_0_0_7::TimeDateStamp;
    const auto t0 = GetTickCount64();
    // Independent of the UI layer: foveated rendering needs only the scene markers. They
    // pass straight through while it is off ([foveation] enabled, `fov on|off`).
    start_scene_marks(base, known);
    std::string why;
    std::int64_t begin = 0, end = 0, uiTarget = 0, uiSize = 0, flag = 0, stereoPass = 0, endTarget = 0;
    bool ok = resolve(base, known, kBegin, &begin, &why) && resolve(base, known, kUiTarget, &uiTarget, &why) &&
              resolve(base, known, kEnd, &end, &why) && resolve(base, known, kUiSize, &uiSize, &why) &&
              resolve(base, known, kFamilyFlag, &flag, &why) && resolve(base, known, kStereoPass, &stereoPass, &why);
    if (ok) {
        // EndRenderingInGameUI must read the same member (its pattern ends with `mov rax,[rcx+disp32]`).
        endTarget = *reinterpret_cast<const std::int32_t*>(base + end + 0x4b + 3);
        if (endTarget != uiTarget) {
            ok = false;
            why = std::format("EndRenderingInGameUI reads +0x{:x}, BeginRenderingInGameUI +0x{:x}", endTarget, uiTarget);
        }
    }
    if (ok) {
        // The flag the renderer tests must be the one every UI binding tests.
        const auto sites = pattern::scan_module(base, kBindingSitePattern, pattern::Sections::Executable, 512);
        if (flag - static_cast<std::int64_t>(kRendererViewFamily) != 0x3c || sites.matches.size() < 64) {
            ok = false;
            why = std::format("in-game UI flag at renderer +0x{:x}, {} binding sites testing family +0x3c", flag, sites.matches.size());
        }
    }
    if (!ok) {
        log::warn("ui: the UI layer is not available: {} (the UI stays in the eye images)", why);
        return false;
    }
    g_off.uiTarget = static_cast<std::size_t>(uiTarget);
    g_off.uiSize = static_cast<std::size_t>(uiSize);
    g_off.familyFlags = static_cast<std::size_t>(flag) - kRendererViewFamily;
    g_off.stereoPass = static_cast<std::size_t>(stereoPass);
    g_oncePerFrame = ctx.config->get_bool("ui", "once_per_frame", true);
    if (!rhi::verify_layout(base)) {
        log::warn("ui: RHI command list layout not confirmed; the UI layer is not available");
        return false;
    }
    if (!g_beginHook.create(reinterpret_cast<void*>(base + begin), &begin_detour) ||
        !g_endHook.create(reinterpret_cast<void*>(base + end), &end_detour)) {
        log::warn("ui: hooks could not be installed; the UI layer is not available");
        return false;
    }
    dev_commands::add("uihook", "uihook status | once <0|1> | proj: engine side of the UI layer (counters; one UI pass per frame in stereo; log the views' projections)",
                      [](std::string_view args) -> std::string {
                          std::string a(args);
                          while (!a.empty() && a.back() == ' ') a.pop_back();
                          if (a.empty() || a == "status") return status();
                          if (a == "proj") {
                              g_projScans = 2;
                              return "ok the projections of the next two views the UI pass receives go to ff7vr.log";
                          }
                          if (a == "once 0" || a == "once 1") {
                              g_oncePerFrame = a.back() == '1';
                              return status();
                          }
                          return "err usage: uihook status | once <0|1> | proj";
                      });
    g_ready = true;
    log::info("ui: UI layer hooks installed in {} ms (BeginRenderingInGameUI +0x{:x}, EndRenderingInGameUI +0x{:x}, UI target member +0x{:x}, "
              "family flag +0x{:x} bit 0x{:x}, one UI pass per frame {})",
              GetTickCount64() - t0, begin, end, uiTarget, g_off.familyFlags, kFamilyFlagBit, g_oncePerFrame.load() ? "on" : "off");
    return true;
#endif
}

}  // namespace ff7vr::engine
