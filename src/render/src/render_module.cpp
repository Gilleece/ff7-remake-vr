// Entry points of the render module: configuration, hooks, dev commands.

#include "d3d11_hooks.h"
#include "foveation.h"
#include "xr_controller.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>

namespace ff7vr::render {
namespace {

std::atomic<bool> g_started{false};
hook::InlineHook g_exitProcess, g_terminateProcess;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

RenderConfig LoadConfig(const StartupContext& ctx) {
    const Config& c = *ctx.config;
    RenderConfig r;
    r.statsIntervalS = std::clamp(c.get_float("render", "stats_interval", r.statsIntervalS), 1.0, 3600.0);
    r.gpuTiming = c.get_bool("render", "gpu_timing", r.gpuTiming);
    const std::string dir = c.get_string("render", "capture_dir", "");
    r.captureDir = dir.empty() ? ctx.dll_dir / L"ff7vr-captures" : std::filesystem::path(log::widen(dir));
    if (r.captureDir.is_relative()) r.captureDir = ctx.dll_dir / r.captureDir;
    r.stereoTest = Lower(c.get_string("render", "mode", "screen")) == "stereo-test";

    r.xrEnabled = c.get_bool("xr", "enabled", r.xrEnabled);
    const std::string backend = Lower(c.get_string("xr", "backend", "openxr"));
    if (backend == "null") {
        r.backend = xr::BackendType::Null;
    } else if (backend != "openxr") {
        log::warn("render: [xr] backend = '{}' is unknown; using openxr", backend);
    }
    r.runtime = c.get_string("xr", "runtime", "auto");  // auto: the first runtime found that has a headset
    r.resolutionScale = static_cast<float>(std::clamp(c.get_float("xr", "resolution_scale", r.resolutionScale), 0.1, 4.0));
    r.eyeWidth = static_cast<uint32_t>(std::clamp<long long>(c.get_int("xr", "eye_width", 0), 0, 16384));
    r.eyeHeight = static_cast<uint32_t>(std::clamp<long long>(c.get_int("xr", "eye_height", 0), 0, 16384));
    {
        // 0/none: keep every implicit layer; 1/all: disable all of them; otherwise a
        // comma-separated list of name parts. Default "reshade": ReShade's XR layer
        // would load a second ReShade into a game that may already run one as dxgi.dll.
        const std::string v = Lower(c.get_string("xr", "disable_implicit_layers", "reshade"));
        if (v == "1" || v == "all" || v == "true") {
            r.disableImplicitLayers = true;
        } else if (!(v.empty() || v == "0" || v == "none" || v == "false")) {
            size_t b = 0;
            while (b <= v.size()) {
                size_t e = v.find(',', b);
                if (e == std::string::npos) e = v.size();
                std::string part = v.substr(b, e - b);
                part.erase(0, part.find_first_not_of(' '));
                while (!part.empty() && part.back() == ' ') part.pop_back();
                if (!part.empty()) r.disableLayersMatching.push_back(part);
                b = e + 1;
            }
        }
    }
    r.debugUtils = c.get_bool("xr", "debug_utils", r.debugUtils);
    r.retryIntervalS = std::clamp(c.get_float("xr", "retry_interval", r.retryIntervalS), 0.5, 600.0);
    r.reconnectAfterExit = c.get_bool("xr", "reconnect_after_exit", r.reconnectAfterExit);
    const std::string wait = Lower(c.get_string("xr", "frame_wait", "thread"));
    r.waitOnPresentThread = wait == "present";
    r.nullRefreshHz = static_cast<float>(std::clamp(c.get_float("xr", "null_refresh_hz", r.nullRefreshHz), 10.0, 500.0));
    r.nullPace = c.get_bool("xr", "null_pace", r.nullPace);
    const std::string motion = Lower(c.get_string("xr", "null_motion", "static"));
    r.nullMotion = motion == "yaw"       ? xr::NullMotion::YawSweep
                   : motion == "sway"    ? xr::NullMotion::Sway
                   : motion == "yawsway" ? xr::NullMotion::YawAndSway
                                         : xr::NullMotion::Static;

    r.screenDistance = static_cast<float>(std::clamp(c.get_float("screen", "distance", r.screenDistance), 0.3, 50.0));
    r.screenWidth = static_cast<float>(std::clamp(c.get_float("screen", "width", r.screenWidth), 0.1, 100.0));
    r.screenOffsetY = static_cast<float>(std::clamp(c.get_float("screen", "offset_y", r.screenOffsetY), -10.0, 10.0));
    r.screenFollowHead = c.get_bool("screen", "follow_head", r.screenFollowHead);
    r.recenterOnStart = c.get_bool("screen", "recenter_on_start", r.recenterOnStart);

    r.uiLayer = c.get_bool("ui", "layer", r.uiLayer);
    r.uiDistance = static_cast<float>(std::clamp(c.get_float("ui", "distance", r.uiDistance), 0.3, 50.0));
    r.uiSize = static_cast<float>(std::clamp(c.get_float("ui", "size", r.uiSize), 0.05, 50.0));
    r.uiOffsetX = static_cast<float>(std::clamp(c.get_float("ui", "offset_x", r.uiOffsetX), -20.0, 20.0));
    r.uiOffsetY = static_cast<float>(std::clamp(c.get_float("ui", "offset_y", r.uiOffsetY), -20.0, 20.0));
    r.uiFollowHead = c.get_bool("ui", "follow_head", r.uiFollowHead);
    r.uiLayerWidth = static_cast<uint32_t>(std::clamp<long long>(c.get_int("ui", "layer_width", r.uiLayerWidth), 0, 8192));
    r.uiMirror = c.get_bool("ui", "mirror", r.uiMirror);

    xr::PictureAdjust pic;
    pic.brightness = static_cast<float>(c.get_float("picture", "brightness", pic.brightness));
    pic.contrast = static_cast<float>(c.get_float("picture", "contrast", pic.contrast));
    pic.saturation = static_cast<float>(c.get_float("picture", "saturation", pic.saturation));
    pic.gamma = static_cast<float>(c.get_float("picture", "gamma", pic.gamma));
    pic.blackLevel = static_cast<float>(c.get_float("picture", "black_level", pic.blackLevel));
    r.picture = ClampPicture(pic);
    r.brightnessUpKey = static_cast<int>(std::clamp<long long>(c.get_int("controls", "brightness_up_key", 0), 0, 255));
    r.brightnessDownKey = static_cast<int>(std::clamp<long long>(c.get_int("controls", "brightness_down_key", 0), 0, 255));
    r.brightnessStep = static_cast<float>(std::clamp(c.get_float("controls", "brightness_step", r.brightnessStep), 0.005, 0.5));
    return r;
}

void OnPresentCb(const PresentInfo& p) {
    foveation::OnPresent(p);  // first: switches variable rate shading off before any of our own work
    XrController::Get().OnPresent(p);
}
void OnResizeCb(IDXGISwapChain* sc) { XrController::Get().OnResize(sc); }

// The game leaves through ExitProcess (or TerminateProcess on itself). Ending the
// XR session there, while every thread still exists, lets the runtime see a clean exit.
void ShutdownForExit(const char* how) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    log::info("render: {}: ending the XR session", how);
    // Bounded: a runtime that hangs must not keep the game from exiting.
    HANDLE t = CreateThread(
        nullptr, 0,
        [](void*) -> DWORD {
            XrController::Get().StopForExit();
            return 0;
        },
        nullptr, 0, nullptr);
    if (t) {
        if (WaitForSingleObject(t, 4000) != WAIT_OBJECT_0) log::warn("render: XR shutdown did not finish within 4 s; exiting anyway");
        CloseHandle(t);
    }
}

void WINAPI ExitProcessDetour(UINT code) {
    ShutdownForExit("ExitProcess");
    g_exitProcess.original<void(WINAPI*)(UINT)>()(code);
}

BOOL WINAPI TerminateProcessDetour(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) ShutdownForExit("TerminateProcess");
    return g_terminateProcess.original<BOOL(WINAPI*)(HANDLE, UINT)>()(process, code);
}

void RegisterCommands() {
    dev_commands::add("status", "log the render and XR status; reply with a summary", [](std::string_view) {
        return XrController::Get().Status();
    });
    dev_commands::add("capture", "capture <path prefix> [timeout ms]: write what each eye sees to <prefix>_L.png / _R.png",
                      [](std::string_view args) {
                          std::string a(args);
                          uint32_t timeout = 5000;
                          // Optional trailing timeout: "capture C:\dir\shot 8000"
                          const size_t sp = a.find_last_of(' ');
                          if (sp != std::string::npos) {
                              uint32_t v = 0;
                              const char* b = a.data() + sp + 1;
                              auto [ptr, ec] = std::from_chars(b, a.data() + a.size(), v);
                              if (ec == std::errc() && ptr == a.data() + a.size()) {
                                  timeout = std::clamp(v, 100u, 60000u);
                                  a.resize(sp);
                              }
                          }
                          while (!a.empty() && a.back() == ' ') a.pop_back();
                          if (a.size() >= 2 && a.front() == '"' && a.back() == '"') a = a.substr(1, a.size() - 2);
                          return XrController::Get().Capture(a, timeout);
                      });
    dev_commands::add("vram", "vram: the game's video memory usage against the budget Windows grants it (card and shared system memory)",
                      [](std::string_view) { return XrController::Get().VideoMemoryStatus(); });
    dev_commands::add("recenter","recenter: the current head position and yaw become the origin", [](std::string_view) {
        return XrController::Get().Recenter();
    });
    dev_commands::add("xr-restart", "xr-restart: end the XR session (if any) and start a new one now", [](std::string_view) {
        return XrController::Get().Restart();
    });
    dev_commands::add("mode", "mode screen|stereo|stereo-test: switch the presentation mode", [](std::string_view args) {
        return XrController::Get().SetModeCommand(Lower(std::string(args)));
    });
    dev_commands::add("xr-stop", "xr-stop: end the XR session and stay off until xr-restart (hooks and timing keep running)",
                      [](std::string_view) { return XrController::Get().Stop(); });
    dev_commands::add("xr-runtime", "xr-runtime <selection>: use another OpenXR runtime ([xr] runtime values) and reconnect",
                      [](std::string_view args) {
                          std::string a(args);
                          while (!a.empty() && a.back() == ' ') a.pop_back();
                          if (a.size() >= 2 && a.front() == '"' && a.back() == '"') a = a.substr(1, a.size() - 2);
                          return XrController::Get().SetRuntime(a);
                      });
    dev_commands::add("frame-wait", "frame-wait thread|present: where the frame wait runs in screen mode",
                      [](std::string_view args) { return XrController::Get().SetFrameWait(Lower(std::string(args))); });
    dev_commands::add("stereo-test",
                      "stereo-test pause <ms> | drop <n>: in mode stereo-test, stop starting frames for <ms> (like a game thread "
                      "blocked by a load) or leave every n-th frame without a stereo image (0 = off)",
                      [](std::string_view args) { return XrController::Get().StereoTestCommand(Lower(std::string(args))); });
    dev_commands::add("fov",
                      "fov status | on | off | preset quality|balanced|performance|off | radii <r1> <r2> <r3> | rates <a> <b> <c> | hidden "
                      "off|coarse|cull | passes scene|no-gbuffer|all | skip [dxgi formats] | trace | timing: fixed foveated rendering in stereo",
                      [](std::string_view args) { return foveation::Command(std::string(args)); });
    dev_commands::add("ui",
                      "ui status | on | off | dump <png path> | distance <m> | size <m> | offset <x m> <y m> | follow <0|1> | mirror <0|1>: the in-game "
                      "UI's own layer in stereo",
                      [](std::string_view args) {
                          std::string a(args);
                          while (!a.empty() && a.back() == ' ') a.pop_back();
                          const size_t sp = a.find(' ');
                          // Lower-case the verb only: a dump path keeps its case.
                          const std::string verb = Lower(a.substr(0, sp));
                          return XrController::Get().UiCommand(sp == std::string::npos ? verb : verb + a.substr(sp));
                      });
    dev_commands::add("picture",
                      "picture status | reset | brightness|contrast|saturation|gamma|black_level <value>: colour adjustment of the eye "
                      "images and the virtual screen (not the UI layer)",
                      [](std::string_view args) { return XrController::Get().PictureCommand(std::string(args)); });
}

}  // namespace

bool start(const StartupContext& ctx) {
    if (g_started.exchange(true)) return true;
    const RenderConfig cfg = LoadConfig(ctx);
    log::info("render: xr {} (backend {}, runtime '{}', frame wait on the {} thread), screen {:.2f} m wide at {:.2f} m, captures in {}",
              cfg.xrEnabled ? "enabled" : "disabled", cfg.backend == xr::BackendType::Null ? "null" : "openxr", cfg.runtime,
              cfg.waitOnPresentThread ? "present" : "xr", cfg.screenWidth, cfg.screenDistance, log::narrow(cfg.captureDir.wstring()));
    log::info("render: UI layer in stereo {}: {:.2f} m high at {:.2f} m, offset {:.2f} {:.2f}, {}, image width {}", cfg.uiLayer ? "on" : "off",
              cfg.uiSize, cfg.uiDistance, cfg.uiOffsetX, cfg.uiOffsetY, cfg.uiFollowHead ? "head-locked" : "world-locked", cfg.uiLayerWidth);
    log::info("render: picture {}; brightness keys up {} down {} (step {:.3f})", PictureText(cfg.picture), cfg.brightnessUpKey,
              cfg.brightnessDownKey, cfg.brightnessStep);
    foveation::Configure(*ctx.config);
    HookCallbacks cb;
    cb.onPresent = &OnPresentCb;
    cb.onResize = &OnResizeCb;
    if (!InstallD3D11Hooks(cb)) {
        log::error("render: D3D11 hooks failed; the render module stays off");
        return false;
    }
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        if (void* p = GetProcAddress(k32, "ExitProcess")) g_exitProcess.create(p, &ExitProcessDetour);
        if (void* p = GetProcAddress(k32, "TerminateProcess")) g_terminateProcess.create(p, &TerminateProcessDetour);
    }
    RegisterCommands();
    XrController::Get().Start(cfg);
    return true;
}

void stop() {
    // Process exit with the loader lock held: nothing safe to do here. The
    // session is ended earlier from the ExitProcess hook.
}

void SetMode(Mode mode) { XrController::Get().SetMode(mode); }
Mode GetMode() { return XrController::Get().GetMode(); }
bool GetEyeSetup(EyeSetup* out) { return XrController::Get().GetEyeSetup(out); }
StereoFrame BeginGameFrame() { return XrController::Get().BeginGameFrame(); }
void SubmitStereoFrame(const StereoSubmit& submit) { XrController::Get().SubmitStereoFrame(submit); }
bool UiLayerWanted() { return g_started.load(std::memory_order_relaxed) && XrController::Get().UiLayerWanted(); }
bool UiDumpRequested() { return g_started.load(std::memory_order_relaxed) && XrController::Get().UiDumpRequested(); }
void SubmitUiLayer(const UiLayerSource& source) { XrController::Get().SubmitUiLayer(source); }
bool FoveationWanted() { return g_started.load(std::memory_order_relaxed) && foveation::Wanted(); }
void FoveationSceneBegin(const FoveationEye eyes[2]) { foveation::SceneBegin(eyes); }
void FoveationSceneEnd() { foveation::SceneEnd(); }

}  // namespace ff7vr::render
