// xr_smoke: drives the XR layer without the game.
//
// Creates its own D3D11 device, renders a per-eye test pattern into one
// side-by-side texture and pushes it through the chosen backend for N frames.
// Prints what the runtime reports and frame timing statistics, optionally
// captures the eye images to PNG and checks their content. Exit code 0 only if
// every step succeeded.
//
//   xr_smoke --backend null --frames 120 --capture out\null
//   xr_smoke --backend openxr --runtime steamvr --frames 600
//   xr_smoke --help
#include "ff7vr/xr/xr.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "xr_smoke_shaders/pattern_ps.h"
#include "xr_smoke_shaders/pattern_vs.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#pragma warning(push)
#pragma warning(disable : 4996 4244 4456 4457 4701 4703 4100 4505 4127)
#include "stb_image.h"
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace xr = ff7vr::xr;

namespace {

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct SourceFormat {
    const char* name;
    DXGI_FORMAT texture;   // resource format
    DXGI_FORMAT view;      // RTV format / SubmitDesc::viewFormat
    bool linear;           // pattern written as linear light
    const char* note;
};

const SourceFormat kSourceFormats[] = {
    {"r10g10b10a2", DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, false, "sRGB-encoded values in a 10-bit UNORM target"},
    {"bgra8", DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, false, "sRGB-encoded values in an 8-bit UNORM target"},
    {"rgba8", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, "sRGB-encoded values in an 8-bit UNORM target"},
    {"rgba8srgb", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true, "8-bit _SRGB target"},
    {"bgra8typeless", DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, false, "typeless resource viewed as UNORM"},
    {"rgba16f", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "linear light in FP16"},
};

struct Options {
    xr::BackendType backend = xr::BackendType::Null;
    std::string runtime = "virtualdesktop";
    uint32_t frames = 300;
    std::string capturePrefix;  // "" = no capture
    bool captureSet = false;
    const SourceFormat* source = &kSourceFormats[0];
    uint32_t eyeW = 0, eyeH = 0;
    float scale = 1.0f;
    DXGI_FORMAT swapchainFormat = DXGI_FORMAT_UNKNOWN;
    xr::NullMotion motion = xr::NullMotion::YawAndSway;
    bool pace = true;
    bool threaded = false;
    bool alternateEyes = false;
    bool verbose = false;
    bool debugLayer = false;
    bool debugUtils = false;
    bool noImplicitLayers = false;
    bool requireVisible = false;
    uint32_t sessionTimeoutSec = 30;
    uint32_t printViewsEvery = 0;  // 0 = first frame and a few more
    int64_t recenterAt = -1;       // frame index at which Recenter() is called
    bool quad = false;             // submit the left eye image as a quad layer only (no projection layer)
    bool quadOver = false;         // projection layer plus the quad layer on top
};

// Quad used by --quad / --quad-over: the left eye image on a 1.6 m wide panel 2 m ahead.
constexpr float kQuadDistance = 2.0f;
constexpr float kQuadWidth = 1.6f;

void PrintUsage() {
    std::printf(
        "xr_smoke - test app for the ff7vr XR layer\n"
        "\n"
        "  --backend null|openxr       backend (default null)\n"
        "  --runtime NAME              OpenXR runtime: virtualdesktop (default), steamvr, system,\n"
        "                              inherit (use XR_RUNTIME_JSON as set), or a path to a runtime JSON\n"
        "  --frames N                  frames to submit (default 300)\n"
        "  --capture PREFIX            capture first and last frame to PREFIX_f<frame>_L.png / _R.png\n"
        "                              and verify them (default for null: xr_smoke_out\\null)\n"
        "  --no-capture                disable capture\n"
        "  --source-format F           side-by-side source format: r10g10b10a2 (default), bgra8, rgba8,\n"
        "                              rgba8srgb, bgra8typeless, rgba16f\n"
        "  --eye-size WxH              per-eye size (null: emulated swapchain; openxr: swapchain size)\n"
        "  --scale S                   scale of the runtime recommended size (openxr)\n"
        "  --swapchain-format F        rgba8srgb, bgra8srgb, rgba16f, rgb10a2, rgba8, bgra8\n"
        "  --motion static|yaw|sway|yawsway   null backend head motion (default yawsway)\n"
        "  --no-pace                   null backend: do not pace to the refresh rate\n"
        "  --threaded                  WaitFrame on one thread, Begin/Submit on another (engine-like)\n"
        "  --alternate-eyes            update only one eye per frame\n"
        "  --session-timeout SEC       give up if the OpenXR session does not start (default 30)\n"
        "  --require-visible           fail unless the runtime reached VISIBLE or FOCUSED\n"
        "  --no-implicit-layers        disable implicit OpenXR API layers for this process\n"
        "  --debug-utils               log XR_EXT_debug_utils messages\n"
        "  --d3d-debug                 create the D3D11 device with the debug layer\n"
        "  --print-views N             print views every N frames\n"
        "  --recenter-at N             call Recenter() before frame N and check the head pose after it\n"
        "  --quad                      quad layer only: the left eye image on a 1.6 m panel 2 m ahead;\n"
        "                              captures are checked for the panel's position and orientation per eye\n"
        "  --quad-over                 projection layer plus that quad on top\n"
        "  --verbose                   debug log output\n");
}

bool ParseSize(const std::string& s, uint32_t* w, uint32_t* h) {
    const size_t x = s.find_first_of("xX");
    if (x == std::string::npos) return false;
    *w = static_cast<uint32_t>(std::strtoul(s.substr(0, x).c_str(), nullptr, 10));
    *h = static_cast<uint32_t>(std::strtoul(s.substr(x + 1).c_str(), nullptr, 10));
    return *w > 0 && *h > 0;
}

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h" || a == "/?") {
            PrintUsage();
            std::exit(0);
        } else if (a == "--backend") {
            const std::string v = next();
            if (v == "null") o.backend = xr::BackendType::Null;
            else if (v == "openxr") o.backend = xr::BackendType::OpenXR;
            else return false;
        } else if (a == "--runtime") {
            o.runtime = next();
        } else if (a == "--frames") {
            o.frames = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        } else if (a == "--capture") {
            o.capturePrefix = next();
            o.captureSet = true;
        } else if (a == "--no-capture") {
            o.capturePrefix.clear();
            o.captureSet = true;
        } else if (a == "--source-format") {
            const std::string v = next();
            o.source = nullptr;
            for (const auto& f : kSourceFormats)
                if (v == f.name) o.source = &f;
            if (!o.source) return false;
        } else if (a == "--eye-size") {
            if (!ParseSize(next(), &o.eyeW, &o.eyeH)) return false;
        } else if (a == "--scale") {
            o.scale = std::strtof(next().c_str(), nullptr);
        } else if (a == "--swapchain-format") {
            const std::string v = next();
            if (v == "rgba8srgb") o.swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            else if (v == "bgra8srgb") o.swapchainFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            else if (v == "rgba16f") o.swapchainFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            else if (v == "rgb10a2") o.swapchainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            else if (v == "rgba8") o.swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (v == "bgra8") o.swapchainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            else return false;
        } else if (a == "--motion") {
            const std::string v = next();
            if (v == "static") o.motion = xr::NullMotion::Static;
            else if (v == "yaw") o.motion = xr::NullMotion::YawSweep;
            else if (v == "sway") o.motion = xr::NullMotion::Sway;
            else if (v == "yawsway") o.motion = xr::NullMotion::YawAndSway;
            else return false;
        } else if (a == "--no-pace") {
            o.pace = false;
        } else if (a == "--threaded") {
            o.threaded = true;
        } else if (a == "--alternate-eyes") {
            o.alternateEyes = true;
        } else if (a == "--session-timeout") {
            o.sessionTimeoutSec = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        } else if (a == "--require-visible") {
            o.requireVisible = true;
        } else if (a == "--no-implicit-layers") {
            o.noImplicitLayers = true;
        } else if (a == "--debug-utils") {
            o.debugUtils = true;
        } else if (a == "--d3d-debug") {
            o.debugLayer = true;
        } else if (a == "--print-views") {
            o.printViewsEvery = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        } else if (a == "--recenter-at") {
            o.recenterAt = std::strtoll(next().c_str(), nullptr, 10);
        } else if (a == "--quad") {
            o.quad = true;
        } else if (a == "--quad-over") {
            o.quadOver = true;
        } else if (a == "--verbose" || a == "-v") {
            o.verbose = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (o.frames == 0) return false;
    if (!o.captureSet && o.backend == xr::BackendType::Null) o.capturePrefix = "xr_smoke_out\\null";
    return true;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
std::mutex g_printMutex;
const auto g_start = std::chrono::steady_clock::now();
bool g_verbose = false;

void Log(const char* level, std::string_view msg) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    std::lock_guard lk(g_printMutex);
    std::printf("[%8.3f] %-5s %.*s\n", t, level, static_cast<int>(msg.size()), msg.data());
    std::fflush(stdout);
}

template <class... A>
void Info(std::format_string<A...> f, A&&... a) {
    Log("info", std::format(f, std::forward<A>(a)...));
}
template <class... A>
void Fail(std::format_string<A...> f, A&&... a) {
    Log("FAIL", std::format(f, std::forward<A>(a)...));
}

void XrLog(xr::LogLevel level, std::string_view msg) {
    switch (level) {
        case xr::LogLevel::Debug:
            if (g_verbose) Log("xr:d", msg);
            break;
        case xr::LogLevel::Info: Log("xr", msg); break;
        case xr::LogLevel::Warn: Log("xr:W", msg); break;
        case xr::LogLevel::Error: Log("xr:E", msg); break;
    }
}

// ---------------------------------------------------------------------------
// Pattern renderer
// ---------------------------------------------------------------------------
struct PatternConstants {
    uint32_t eyeWidth, eyeHeight, frame, outputLinear;
};

class PatternRenderer {
public:
    bool Init(ID3D11Device* dev, const SourceFormat& fmt, uint32_t eyeW, uint32_t eyeH) {
        fmt_ = &fmt;
        eyeW_ = eyeW;
        eyeH_ = eyeH;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = eyeW * 2;
        d.Height = eyeH;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmt.texture;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = dev->CreateTexture2D(&d, nullptr, &tex_);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = fmt.view;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(tex_.Get(), &rd, &rtv_);
        if (SUCCEEDED(hr)) hr = dev->CreateVertexShader(g_pattern_vs, sizeof(g_pattern_vs), nullptr, &vs_);
        if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_pattern_ps, sizeof(g_pattern_ps), nullptr, &ps_);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(PatternConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&bd, nullptr, &cb_);
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rs, &rs_);
        if (FAILED(hr)) {
            Fail("pattern renderer setup failed: 0x{:08X} (format {})", static_cast<uint32_t>(hr), fmt.name);
            return false;
        }
        return true;
    }

    void Render(ID3D11DeviceContext* ctx, uint32_t frame) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const PatternConstants c{eyeW_, eyeH_, frame, fmt_->linear ? 1u : 0u};
            std::memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(cb_.Get(), 0);
        }
        ID3D11RenderTargetView* rtv = rtv_.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        const D3D11_VIEWPORT vp{0, 0, float(eyeW_ * 2), float(eyeH_), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs_.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer* cb = cb_.Get();
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(3, 0);
        // Leave a recognisable state behind to check that the XR layer restores it.
        marker_ = rtv;
    }

    // After SubmitFrame the pipeline must look exactly as we left it.
    bool CheckStateRestored(ID3D11DeviceContext* ctx) const {
        ComPtr<ID3D11RenderTargetView> rtv;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        ComPtr<ID3D11PixelShader> ps;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ComPtr<ID3D11Buffer> cb;
        ctx->PSGetConstantBuffers(0, 1, &cb);
        D3D11_VIEWPORT vp{};
        UINT n = 1;
        ctx->RSGetViewports(&n, &vp);
        return rtv.Get() == marker_ && ps.Get() == ps_.Get() && cb.Get() == cb_.Get() && n == 1 && vp.Width == float(eyeW_ * 2);
    }

    ID3D11Texture2D* Texture() const { return tex_.Get(); }

private:
    const SourceFormat* fmt_ = nullptr;
    uint32_t eyeW_ = 0, eyeH_ = 0;
    ComPtr<ID3D11Texture2D> tex_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11RasterizerState> rs_;
    ID3D11RenderTargetView* marker_ = nullptr;
};

// ---------------------------------------------------------------------------
// PNG verification
// ---------------------------------------------------------------------------
struct Rgb {
    int r, g, b;
};

bool Near(const Rgb& a, const Rgb& b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

// Checks one captured eye: correct eye, not flipped, colours neither washed out nor too dark.
bool VerifyEyePng(const std::string& path, int eye, uint32_t expectW, uint32_t expectH) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!px) {
        Fail("cannot read {}", path);
        return false;
    }
    auto at = [&](int x, int y) {
        const unsigned char* p = px + (size_t(y) * w + x) * 3;
        return Rgb{p[0], p[1], p[2]};
    };
    bool ok = true;
    auto expect = [&](const char* what, Rgb got, Rgb want, int tol) {
        const bool good = Near(got, want, tol);
        if (!good) {
            Fail("{}: {} is ({},{},{}), expected ({},{},{}) +-{}", path, what, got.r, got.g, got.b, want.r, want.g, want.b, tol);
            ok = false;
        }
        return good;
    };
    if (static_cast<uint32_t>(w) != expectW || static_cast<uint32_t>(h) != expectH) {
        Fail("{}: size {}x{}, expected {}x{}", path, w, h, expectW, expectH);
        ok = false;
    }
    if (w >= 640 && h >= 300) {
        const Rgb green{0, 200, 0};
        const Rgb eyeColour = eye == 0 ? Rgb{110, 40, 40} : Rgb{40, 40, 110};
        expect("orientation marker (top-left)", at(90, 90), green, 2);
        expect("eye colour probe", at(210, 90), eyeColour, 2);
        expect("sRGB 128 probe (gamma)", at(330, 90), Rgb{128, 128, 128}, 2);
        expect("sRGB 188 probe", at(570, 90), Rgb{188, 188, 188}, 2);
        if (Near(at(w - 1 - 90, 90), green, 30)) {
            Fail("{}: marker found top-right: image is mirrored horizontally", path);
            ok = false;
        }
        if (Near(at(90, h - 1 - 90), green, 30)) {
            Fail("{}: marker found bottom-left: image is flipped vertically", path);
            ok = false;
        }
        const Rgb other = eye == 0 ? Rgb{40, 40, 110} : Rgb{110, 40, 40};
        if (Near(at(210, 90), other, 2)) {
            Fail("{}: shows the {} eye", path, eye == 0 ? "right" : "left");
            ok = false;
        }
    } else {
        Info("{}: image smaller than 640x300, content checks skipped", path);
    }
    stbi_image_free(px);
    if (ok) Info("verified {} ({}x{}): {} eye, upright, colours exact", path, w, h, eye == 0 ? "left" : "right");
    return ok;
}

// Projects a point given in tracking space into an eye image (pixels), with
// the math written out independently of the library's compositor.
bool ProjectToEye(const xr::View& v, const xr::Vec3& p, uint32_t w, uint32_t h, double* px, double* py) {
    const xr::Vec3 rel{p.x - v.pose.position.x, p.y - v.pose.position.y, p.z - v.pose.position.z};
    const xr::Vec3 e = xr::QuatRotate(xr::QuatConjugate(v.pose.orientation), rel);  // eye space, -Z forward
    if (e.z >= -1e-4f) return false;
    const double tx = e.x / -e.z, ty = e.y / -e.z;
    const double l = std::tan(v.fov.angleLeft), r = std::tan(v.fov.angleRight);
    const double u = std::tan(v.fov.angleUp), d = std::tan(v.fov.angleDown);
    *px = (tx - l) / (r - l) * w;
    *py = (u - ty) / (u - d) * h;
    return true;
}

// Checks a --quad capture: black outside the panel, panel where the eye's view
// puts it, upright and not mirrored (green marker at its top-left).
bool VerifyQuadPng(const std::string& path, int eye, const xr::View& view, const xr::Pose& quadPose, float qw, float qh) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!px) {
        Fail("cannot read {}", path);
        return false;
    }
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        const unsigned char* p = px + (size_t(y) * w + x) * 3;
        return Rgb{p[0], p[1], p[2]};
    };
    auto lit = [](const Rgb& c) { return c.r + c.g + c.b > 24; };
    bool ok = true;
    // Expected corners (TL, TR, BL, BR) in pixels.
    double cx[4], cy[4];
    const xr::Vec3 local[4] = {{-qw / 2, qh / 2, 0}, {qw / 2, qh / 2, 0}, {-qw / 2, -qh / 2, 0}, {qw / 2, -qh / 2, 0}};
    for (int i = 0; i < 4; ++i) {
        const xr::Vec3 r = xr::QuatRotate(quadPose.orientation, local[i]);
        const xr::Vec3 p{r.x + quadPose.position.x, r.y + quadPose.position.y, r.z + quadPose.position.z};
        if (!ProjectToEye(view, p, w, h, &cx[i], &cy[i])) {
            Fail("{}: quad corner {} behind the eye; test geometry invalid", path, i);
            stbi_image_free(px);
            return false;
        }
    }
    const double ex0 = std::min({cx[0], cx[2]}), ex1 = std::max({cx[1], cx[3]});
    const double ey0 = std::min({cy[0], cy[1]}), ey1 = std::max({cy[2], cy[3]});
    // Measured bounding box of lit pixels.
    int mx0 = w, my0 = h, mx1 = -1, my1 = -1;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2)
            if (lit(at(x, y))) {
                mx0 = std::min(mx0, x);
                mx1 = std::max(mx1, x);
                my0 = std::min(my0, y);
                my1 = std::max(my1, y);
            }
    Info("{}: panel expected x {:.1f}..{:.1f} y {:.1f}..{:.1f}, measured x {}..{} y {}..{}", path, ex0, ex1, ey0, ey1, mx0, mx1, my0, my1);
    const double tol = 4.0;
    const bool fullyVisible = ex0 >= 0 && ey0 >= 0 && ex1 <= w && ey1 <= h;
    if (fullyVisible) {
        if (mx1 < 0 || std::fabs(mx0 - ex0) > tol || std::fabs(mx1 - ex1) > tol || std::fabs(my0 - ey0) > tol || std::fabs(my1 - ey1) > tol) {
            Fail("{}: panel is not where the eye's view puts it", path);
            ok = false;
        }
    } else {
        // Partly outside the view: the visible part must lie inside the expected outline.
        Info("{}: panel partly outside the view; checking containment only", path);
        if (mx1 < 0 || mx0 < ex0 - tol || mx1 > ex1 + tol || my0 < ey0 - tol || my1 > ey1 + tol) {
            Fail("{}: lit pixels outside the expected panel outline", path);
            ok = false;
        }
    }
    if (lit(at(2, 2)) || lit(at(w - 3, h - 3))) {
        Fail("{}: background is not black", path);
        ok = false;
    }
    stbi_image_free(px);
    if (ok) Info("verified {} ({}x{}): {} eye, panel position within {:.0f} px", path, w, h, eye == 0 ? "left" : "right", tol);
    return ok;
}

// Checks that the green orientation marker lands at the panel's top-left.
bool VerifyQuadMarker(const std::string& path, const xr::View& view, const xr::Pose& quadPose, float qw, float qh, uint32_t srcW,
                      uint32_t srcH) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!px) return false;
    auto at = [&](double fx, double fy) {
        const int x = std::clamp(static_cast<int>(fx), 0, w - 1), y = std::clamp(static_cast<int>(fy), 0, h - 1);
        const unsigned char* p = px + (size_t(y) * w + x) * 3;
        return Rgb{p[0], p[1], p[2]};
    };
    auto panelPoint = [&](double sx, double sy, double* ox, double* oy) {
        const float lx = static_cast<float>((sx / srcW - 0.5) * qw), ly = static_cast<float>((0.5 - sy / srcH) * qh);
        const xr::Vec3 r = xr::QuatRotate(quadPose.orientation, xr::Vec3{lx, ly, 0});
        return ProjectToEye(view, xr::Vec3{r.x + quadPose.position.x, r.y + quadPose.position.y, r.z + quadPose.position.z}, w, h, ox,
                            oy);
    };
    bool ok = true;
    const Rgb green{0, 200, 0};
    double x = 0, y = 0;
    if (!panelPoint(90, 90, &x, &y) || !Near(at(x, y), green, 40)) {
        const Rgb c = at(x, y);
        Fail("{}: orientation marker not at the panel's top-left (found {},{},{} at {:.0f},{:.0f})", path, c.r, c.g, c.b, x, y);
        ok = false;
    }
    if (panelPoint(srcW - 90.0, 90, &x, &y) && Near(at(x, y), green, 40)) {
        Fail("{}: marker at the panel's top-right: mirrored", path);
        ok = false;
    }
    if (panelPoint(90, srcH - 90.0, &x, &y) && Near(at(x, y), green, 40)) {
        Fail("{}: marker at the panel's bottom-left: upside down", path);
        ok = false;
    }
    stbi_image_free(px);
    return ok;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
struct Series {
    std::vector<double> v;
    void Add(double x) { v.push_back(x); }
    std::string Summary() const {
        if (v.empty()) return "n/a";
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        double sum = 0;
        for (double x : s) sum += x;
        auto pct = [&](double p) { return s[std::min(s.size() - 1, static_cast<size_t>(p * (s.size() - 1) + 0.5))]; };
        return std::format("min {:.3f}  avg {:.3f}  p50 {:.3f}  p99 {:.3f}  max {:.3f} ms (n={})", s.front(), sum / s.size(), pct(0.5),
                           pct(0.99), s.back(), s.size());
    }
};

double MsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::string FovString(const xr::Fov& f) {
    return std::format("L {:.2f} R {:.2f} U {:.2f} D {:.2f} deg", f.angleLeft * xr::kRadToDeg, f.angleRight * xr::kRadToDeg,
                       f.angleUp * xr::kRadToDeg, f.angleDown * xr::kRadToDeg);
}
std::string PoseString(const xr::Pose& p) {
    return std::format("pos ({:+.4f}, {:+.4f}, {:+.4f}) m  quat ({:+.4f}, {:+.4f}, {:+.4f}, {:+.4f})", p.position.x, p.position.y,
                       p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
}

void PrintRuntimeInfo(const xr::RuntimeInfo& ri) {
    Info("runtime        : {} {}", ri.runtimeName, ri.runtimeVersion);
    Info("runtime json   : {}", ri.runtimeJson.empty() ? std::string("(system default / not applicable)") : ri.runtimeJson);
    Info("system         : {} (vendor 0x{:X})", ri.systemName, ri.vendorId);
    Info("tracking       : orientation {}, position {}", ri.orientationTracking, ri.positionTracking);
    Info("max swapchain  : {}x{}", ri.maxSwapchainWidth, ri.maxSwapchainHeight);
    for (int e = 0; e < 2; ++e)
        Info("eye {} recommended {}x{} (max {}x{}), swapchain {}x{} {} x{} images", e, ri.recommendedWidth[e], ri.recommendedHeight[e],
             ri.maxWidth[e], ri.maxHeight[e], ri.eyeSwapchain[e].width, ri.eyeSwapchain[e].height,
             xr::DxgiFormatName(ri.eyeSwapchain[e].format), ri.eyeSwapchain[e].imageCount);
    std::string fmts;
    for (auto f : ri.runtimeFormats) fmts += std::format("{}{}({})", fmts.empty() ? "" : ", ", xr::DxgiFormatName(f), static_cast<int>(f));
    Info("runtime formats: {}", fmts);
    std::string rates;
    for (float r : ri.availableRefreshHz) rates += std::format("{}{:.1f}", rates.empty() ? "" : ", ", r);
    Info("refresh rate   : {:.2f} Hz (available: {})", ri.refreshHz, rates.empty() ? std::string("not reported") : rates);
    Info("depth layer ext: {}", ri.depthLayerSupported);
    std::string ext;
    for (const auto& e : ri.enabledExtensions) ext += (ext.empty() ? "" : ", ") + e;
    Info("extensions     : {}", ext.empty() ? std::string("-") : ext);
    std::string layers;
    for (const auto& l : ri.apiLayers) layers += (layers.empty() ? "" : ", ") + l;
    Info("api layers     : {}", layers.empty() ? std::string("-") : layers);
    for (const auto& l : ri.implicitLayers) Info("implicit layer : {}", l);
    if (ri.adapterLuid) Info("adapter LUID   : {:016X}", ri.adapterLuid);
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------
struct Run {
    Options opt;
    xr::IXrBackend* be = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    PatternRenderer* pattern = nullptr;
    uint32_t eyeW = 0, eyeH = 0;
    std::vector<std::pair<uint32_t, std::string>> capturePrefixes;
    xr::LayerHandle quadLayer = 0;
    std::vector<std::pair<uint64_t, xr::FrameInfo>> capturedFrames;  // frameId -> views, for quad checks
    std::mutex capturedMutex;

    xr::Pose QuadPose() const {
        xr::Pose p;
        p.position = xr::Vec3{0, 0, -kQuadDistance};
        return p;
    }
    float QuadHeight() const { return kQuadWidth * float(eyeH) / float(eyeW); }

    Series waitMs, intervalMs, submitMs, periodMs;
    std::atomic<uint32_t> submitted{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stateErrors{false};
    bool sawVisible = false;
    std::chrono::steady_clock::time_point lastWaitReturn{};
    uint32_t shouldRenderFalse = 0;

    void PrintViews(const xr::FrameInfo& fi, uint32_t index) {
        Info("frame {} (id {}): state {}, shouldRender {}, display time {} ns, period {:.3f} ms, tracking o:{} p:{}", index, fi.frameId,
             xr::ToString(fi.state), fi.shouldRender, fi.predictedDisplayTime, fi.predictedDisplayPeriod / 1e6, fi.orientationValid,
             fi.positionValid);
        Info("  head  {}", PoseString(fi.head));
        for (int e = 0; e < 2; ++e) Info("  eye {} {}  fov {}", e, PoseString(fi.views[e].pose), FovString(fi.views[e].fov));
        const xr::Vec3 a = fi.views[0].pose.position, b = fi.views[1].pose.position;
        const float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        Info("  eye separation {:.2f} mm", ipd * 1000.0f);
    }

    // Wait step (game thread). Returns false on hard failure.
    bool Wait(xr::FrameInfo& fi) {
        const auto t0 = std::chrono::steady_clock::now();
        const xr::Result r = be->WaitFrame(fi);
        const auto t1 = std::chrono::steady_clock::now();
        if (r != xr::Result::Ok) {
            Fail("WaitFrame: {} (state {})", xr::ToString(r), xr::ToString(fi.state));
            return false;
        }
        if (fi.sessionRunning) {
            waitMs.Add(std::chrono::duration<double, std::milli>(t1 - t0).count());
            if (lastWaitReturn.time_since_epoch().count()) intervalMs.Add(std::chrono::duration<double, std::milli>(t1 - lastWaitReturn).count());
            lastWaitReturn = t1;
            periodMs.Add(fi.predictedDisplayPeriod / 1e6);
            if (fi.state == xr::SessionState::Visible || fi.state == xr::SessionState::Focused) sawVisible = true;
        }
        return true;
    }

    // Render + submit step (render thread). index = submitted-frame index.
    bool RenderAndSubmit(const xr::FrameInfo& fi, uint32_t index) {
        if (xr::Result r = be->BeginFrame(fi.frameId); r != xr::Result::Ok) {
            Fail("BeginFrame({}): {}", fi.frameId, xr::ToString(r));
            return false;
        }
        if (!fi.shouldRender) ++shouldRenderFalse;
        pattern->Render(ctx, index);
        for (const auto& [f, prefix] : capturePrefixes)
            if (f == index) {
                be->RequestCapture(xr::CaptureRequest{prefix});
                std::lock_guard lk(capturedMutex);
                capturedFrames.push_back({fi.frameId, fi});
            }
        xr::SubmitDesc sd;
        sd.texture = opt.quad ? nullptr : pattern->Texture();
        sd.viewFormat = opt.source->view;
        sd.encoding = opt.source->linear ? xr::ColorEncoding::Linear : xr::ColorEncoding::Srgb;
        xr::QuadLayer q;
        if (quadLayer) {
            q.layer = quadLayer;
            q.texture = pattern->Texture();
            q.viewFormat = opt.source->view;
            q.encoding = sd.encoding;
            q.rect = xr::Rect{0, 0, eyeW, eyeH};  // the left eye image
            q.pose = QuadPose();
            q.width = kQuadWidth;
            q.height = QuadHeight();
            sd.quads = &q;
            sd.quadCount = 1;
        }
        if (opt.alternateEyes) {
            sd.eyes[0].update = (index % 2) == 0;
            sd.eyes[1].update = (index % 2) == 1;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const xr::Result r = be->SubmitFrame(fi.frameId, sd);
        submitMs.Add(MsSince(t0));
        if (r != xr::Result::Ok) {
            Fail("SubmitFrame({}): {}", fi.frameId, xr::ToString(r));
            return false;
        }
        if (!pattern->CheckStateRestored(ctx)) {
            Fail("D3D11 pipeline state was not restored by SubmitFrame (frame {})", index);
            stateErrors = true;
            return false;
        }
        return true;
    }

    bool WaitForSession(xr::FrameInfo& fi, std::chrono::steady_clock::time_point started) {
        // Returns true when fi holds a running frame.
        for (;;) {
            if (!Wait(fi)) return false;
            if (fi.sessionRunning) return true;
            if (fi.state == xr::SessionState::ExitRequested || fi.state == xr::SessionState::Lost) {
                Fail("session ended by the runtime (state {})", xr::ToString(fi.state));
                return false;
            }
            if (MsSince(started) > opt.sessionTimeoutSec * 1000.0) {
                Fail("session did not start within {} s (state {})", opt.sessionTimeoutSec, xr::ToString(fi.state));
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Right after Recenter() the head must face -Z (yaw 0) at the origin.
    bool CheckRecentered(const xr::FrameInfo& fi) {
        const float yawDeg = xr::QuatYaw(fi.head.orientation) * xr::kRadToDeg;
        const xr::Vec3 p = fi.head.position;
        const float dist = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (std::fabs(yawDeg) > 0.5f || dist > 0.001f) {
            Fail("after Recenter: head yaw {:.3f} deg, distance from origin {:.4f} m (expected ~0)", yawDeg, dist);
            return false;
        }
        Info("recenter verified: head yaw {:.4f} deg, distance from origin {:.5f} m", yawDeg, dist);
        return true;
    }

    bool ShouldPrint(uint32_t index) const {
        if (opt.printViewsEvery) return index % opt.printViewsEvery == 0;
        return index == 0 || index == opt.frames / 2 || index + 1 == opt.frames;
    }

    bool RunSingleThread() {
        const auto started = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < opt.frames; ++i) {
            xr::FrameInfo fi;
            if (static_cast<int64_t>(i) == opt.recenterAt) be->Recenter();
            if (!WaitForSession(fi, started)) return false;
            if (ShouldPrint(i) || static_cast<int64_t>(i) == opt.recenterAt) PrintViews(fi, i);
            if (static_cast<int64_t>(i) == opt.recenterAt && !CheckRecentered(fi)) return false;
            if (!RenderAndSubmit(fi, i)) return false;
            ++submitted;
        }
        return true;
    }

    bool RunThreaded() {
        std::mutex m;
        std::condition_variable cv;
        std::deque<xr::FrameInfo> q;
        bool gtDone = false;
        const auto started = std::chrono::steady_clock::now();
        std::thread gt([&] {
            for (uint32_t i = 0; i < opt.frames && !failed; ++i) {
                xr::FrameInfo fi;
                if (static_cast<int64_t>(i) == opt.recenterAt) be->Recenter();
                if (!WaitForSession(fi, started)) {
                    failed = true;
                    break;
                }
                if (ShouldPrint(i) || static_cast<int64_t>(i) == opt.recenterAt) PrintViews(fi, i);
                if (static_cast<int64_t>(i) == opt.recenterAt && !CheckRecentered(fi)) {
                    failed = true;
                    break;
                }
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return q.size() < 2 || failed; });  // engine runs at most one frame ahead
                q.push_back(fi);
                cv.notify_all();
            }
            std::lock_guard lk(m);
            gtDone = true;
            cv.notify_all();
        });
        uint32_t index = 0;
        for (;;) {
            xr::FrameInfo fi;
            {
                std::unique_lock lk(m);
                cv.wait(lk, [&] { return !q.empty() || gtDone; });
                if (q.empty()) break;
                fi = q.front();
                q.pop_front();
                cv.notify_all();
            }
            if (failed) {  // drain: end queued frames so the waiting thread is not blocked
                be->SkipFrame(fi.frameId);
                continue;
            }
            if (!RenderAndSubmit(fi, index++)) {
                failed = true;
                cv.notify_all();
                continue;
            }
            ++submitted;
        }
        gt.join();
        return !failed;
    }
};

ComPtr<ID3D11Device> CreateDevice(bool debug, ComPtr<ID3D11DeviceContext>* ctx) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (debug) flags |= D3D11_CREATE_DEVICE_DEBUG;
    ComPtr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, &got,
                                   ctx->ReleaseAndGetAddressOf());
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && debug) {
        Log("warn", "D3D11 debug layer not installed (Graphics Tools optional feature); continuing without it");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev, &got,
                               ctx->ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        Fail("D3D11CreateDevice failed: 0x{:08X}", static_cast<uint32_t>(hr));
        return nullptr;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad))) {
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        Info("D3D11 device: {} (feature level 0x{:X}, LUID {:08X}:{:08X})", name, static_cast<int>(got),
             static_cast<uint32_t>(ad.AdapterLuid.HighPart), ad.AdapterLuid.LowPart);
    }
    return dev;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        PrintUsage();
        return 2;
    }
    g_verbose = opt.verbose;
    Info("xr_smoke: backend {}, {} frames, source {} ({}){}{}", opt.backend == xr::BackendType::Null ? "null" : "openxr", opt.frames,
         opt.source->name, opt.source->note, opt.threaded ? ", threaded" : "", opt.alternateEyes ? ", alternate eyes" : "");

    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Device> dev = CreateDevice(opt.debugLayer, &ctx);
    if (!dev) return 1;

    auto be = xr::CreateBackend(opt.backend);
    xr::InitDesc desc;
    desc.backend = opt.backend;
    desc.device = dev.Get();
    desc.log = XrLog;
    desc.runtime = opt.runtime;
    desc.appName = "xr_smoke";
    desc.eyeWidth = opt.eyeW;
    desc.eyeHeight = opt.eyeH;
    desc.resolutionScale = opt.scale;
    desc.swapchainFormat = opt.swapchainFormat;
    desc.enableDebugUtils = opt.debugUtils;
    desc.disableImplicitApiLayers = opt.noImplicitLayers;
    desc.null.motion = opt.motion;
    desc.null.paceToRefresh = opt.pace;

    const auto tInit = std::chrono::steady_clock::now();
    const xr::Result ir = be->Init(desc);
    if (ir != xr::Result::Ok) {
        Fail("Init: {}", xr::ToString(ir));
        return 1;
    }
    Info("Init took {:.1f} ms", MsSince(tInit));
    const xr::RuntimeInfo ri = be->GetRuntimeInfo();
    PrintRuntimeInfo(ri);

    Run run;
    run.opt = opt;
    run.be = be.get();
    run.ctx = ctx.Get();
    run.eyeW = ri.eyeSwapchain[0].width;
    run.eyeH = ri.eyeSwapchain[0].height;
    if (run.eyeW < 16 || run.eyeH < 16 || ri.eyeSwapchain[1].width != run.eyeW || ri.eyeSwapchain[1].height != run.eyeH) {
        Fail("unexpected eye swapchain sizes");
        return 1;
    }
    PatternRenderer pattern;
    if (!pattern.Init(dev.Get(), *opt.source, run.eyeW, run.eyeH)) return 1;
    run.pattern = &pattern;
    if (opt.quad || opt.quadOver) {
        xr::QuadLayerCreateDesc qd{run.eyeW, run.eyeH, opt.source->texture};
        if (const xr::Result r = be->CreateQuadLayer(qd, &run.quadLayer); r != xr::Result::Ok) {
            Fail("CreateQuadLayer: {}", xr::ToString(r));
            return 1;
        }
        xr::SwapchainInfo si;
        be->GetQuadLayerInfo(run.quadLayer, &si);
        Info("quad layer: {}x{} {} ({} images), {:.2f} x {:.2f} m at {:.1f} m", si.width, si.height, xr::DxgiFormatName(si.format),
             si.imageCount, kQuadWidth, run.QuadHeight(), kQuadDistance);
    }
    Info("source texture: {}x{} {}", run.eyeW * 2, run.eyeH, xr::DxgiFormatName(opt.source->texture));

    if (!opt.capturePrefix.empty()) {
        std::vector<uint32_t> at = {0, opt.frames - 1};
        if (opt.frames == 1) at = {0};
        for (uint32_t f : at) run.capturePrefixes.push_back({f, std::format("{}_f{:04}", opt.capturePrefix, f)});
    }

    const auto tLoop = std::chrono::steady_clock::now();
    const bool loopOk = opt.threaded ? run.RunThreaded() : run.RunSingleThread();
    const double loopMs = MsSince(tLoop);
    bool ok = loopOk;

    // Captures.
    if (!run.capturePrefixes.empty()) {
        const auto results = be->WaitForCaptures(15000);
        size_t expected = 0;
        for (const auto& [f, prefix] : run.capturePrefixes)
            if (f < run.submitted) ++expected;
        if (results.size() != expected) {
            Fail("{} captures finished, {} expected", results.size(), expected);
            ok = false;
        }
        for (const auto& r : results) {
            if (!r.ok) {
                Fail("capture of frame {} failed: {}", r.frameId, r.error);
                ok = false;
                continue;
            }
            if (opt.quad) {
                const xr::FrameInfo* fi = nullptr;
                for (const auto& [id, info] : run.capturedFrames)
                    if (id == r.frameId) fi = &info;
                if (!fi) {
                    Fail("no views recorded for captured frame {}", r.frameId);
                    ok = false;
                    continue;
                }
                for (int e = 0; e < 2; ++e) {
                    ok = VerifyQuadPng(r.files[e], e, fi->views[e], run.QuadPose(), kQuadWidth, run.QuadHeight()) && ok;
                    ok = VerifyQuadMarker(r.files[e], fi->views[e], run.QuadPose(), kQuadWidth, run.QuadHeight(), run.eyeW, run.eyeH) && ok;
                }
                continue;
            }
            // In alternate-eye mode the eye not updated keeps the previous image: still the right eye's content.
            for (int e = 0; e < 2; ++e) ok = VerifyEyePng(r.files[e], e, run.eyeW, run.eyeH) && ok;
        }
    }

    const xr::FrameStats st = be->GetStats();
    const xr::RuntimeInfo ri2 = be->GetRuntimeInfo();
    Info("frames: submitted {} of {} in {:.1f} ms; backend waited {} begun {} submitted {} skipped {} discarded {} not-rendered {}",
         run.submitted.load(), opt.frames, loopMs, st.framesWaited, st.framesBegun, st.framesSubmitted, st.framesSkipped,
         st.framesDiscarded, st.framesNotRendered);
    Info("image transfers: copy {}, shader blit {}; quad updates {}; swapchain image wait timeouts {}", st.copyPath, st.blitPath,
         st.quadUpdates, st.imageWaitTimeouts);
    Info("WaitFrame blocking : {}", run.waitMs.Summary());
    Info("frame interval     : {}", run.intervalMs.Summary());
    Info("SubmitFrame (CPU)  : {}", run.submitMs.Summary());
    Info("predicted period   : {}", run.periodMs.Summary());
    Info("refresh rate       : {:.2f} Hz; final state {}", ri2.refreshHz, xr::ToString(be->GetState()));
    if (run.shouldRenderFalse) Info("frames with shouldRender=false: {}", run.shouldRenderFalse);

    if (opt.requireVisible && !run.sawVisible) {
        Fail("runtime never reported VISIBLE/FOCUSED");
        ok = false;
    }
    if (run.submitted != opt.frames) ok = false;

    const auto tShut = std::chrono::steady_clock::now();
    be->Shutdown();
    Info("Shutdown took {:.1f} ms", MsSince(tShut));
    be.reset();

    if (ok)
        Info("RESULT: PASS");
    else
        Log("FAIL", "RESULT: FAIL");
    return ok ? 0 : 1;
}
