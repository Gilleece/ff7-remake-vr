#include "xr_controller.h"

#include "pause_on_remove.h"

#include "comfort.h"
#include "foveation.h"
#include "menu.h"
#include "ui_battle.h"
#include "video_memory.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <sstream>

namespace ff7vr::render {
namespace {

using Microsoft::WRL::ComPtr;

const char* ModeName(Mode m) { return m == Mode::Stereo ? "stereo" : "screen"; }

const char* BackendName(xr::BackendType b) { return b == xr::BackendType::Null ? "null" : "openxr"; }

int64_t QpcFreq() {
    static const int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}
int64_t MsToQpc(double ms) { return static_cast<int64_t>(ms * double(QpcFreq()) / 1000.0); }

DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

// Rate limiter for repeated log lines: true for the 1st, 2nd, 4th, 8th, ... occurrence.
bool PowerOfTwo(uint64_t n) { return n != 0 && (n & (n - 1)) == 0; }

}  // namespace

XrController& XrController::Get() {
    // Never destroyed: its threads may still exist while the process exits.
    static XrController* c = new XrController();
    return *c;
}

void XrController::Start(const RenderConfig& cfg) {
    if (started_.exchange(true)) return;
    cfg_ = cfg;
    waitOnPresent_ = cfg_.waitOnPresentThread;
    uiOn_ = cfg_.uiLayer;
    uiMirror_ = cfg_.uiMirror;
    depthOn_ = cfg_.depthLayer;
    {
        std::lock_guard lk(pictureMutex_);
        picture_ = ClampPicture(cfg_.picture);
    }
    if (cfg_.stereoTest) mode_ = Mode::Stereo;
    lastStatsQpc_ = QpcNow();
    dev_commands::add("xr-sim",
                      "xr-sim status | head <yaw deg> [pitch deg] [x y z m] | recenter-event [nopose] [delay <frames>] | lose orientation|position "
                      "<frames> | gaze <yaw> <pitch> | gaze off | gaze sweep [radius deg] [period s] | gaze blink <frames> | yawrate <deg/s> | late-yaw <deg> | "
                      "depth: Null backend only, emulate the headset's head pose, its own recenter, lost tracking, an eye tracker, a turning head, "
                      "an offset only late locations see; depth layer counters",
                      [this](std::string_view a) { return SimulateCommand(std::string(a)); });
    dev_commands::add("xr-depth",
                      "xr-depth status | on | off | dump <path prefix>: the scene depth with the projection layer (XR_KHR_composition_layer_depth); "
                      "dump = a capture that also writes <prefix>_depthL.png / _depthR.png",
                      [this](std::string_view a) { return DepthCommand(std::string(a)); });
    thread_ = std::thread([this] { ThreadMain(); });
    if (cfg_.stereoTest) stereoTest_ = std::thread([this] { StereoTestThread(); });
}

// ---------------------------------------------------------------------------
// XR thread
// ---------------------------------------------------------------------------
void XrController::ThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"ff7vr xr");
    log::info("xr: thread started (backend {}, runtime '{}', enabled {})", BackendName(cfg_.backend), cfg_.runtime, cfg_.xrEnabled);
    while (!stop_.load()) {
        const int64_t now = QpcNow();
        if (QpcToMs(now - lastStatsQpc_) >= cfg_.statsIntervalS * 1000.0) {
            LogStats();
            lastStatsQpc_ = now;
        }
        if (!cfg_.xrEnabled) {
            Sleep(100);
            continue;
        }
        {
            std::lock_guard lk(pendingMutex_);
            if (hasPendingRuntime_) {
                cfg_.runtime = pendingRuntime_;
                hasPendingRuntime_ = false;
                failedAttempts_ = 0;
                log::info("xr: runtime '{}' from now on", cfg_.runtime);
            }
        }
        if (stopRequested_.exchange(false)) {
            if (ready_.load()) Teardown(TeardownReason::Restart, "stop requested");
            holdAfterExit_ = true;
            log::info("xr: staying off until 'xr-restart'");
            continue;
        }
        if (ready_.load()) {
            if (restartRequested_.exchange(false)) {
                Teardown(TeardownReason::Restart, "restart requested");
                continue;
            }
            if (deviceChanged_.exchange(false)) {
                Teardown(TeardownReason::DeviceChanged, "the game's D3D11 device changed");
                continue;
            }
            const xr::SessionState st = backend_->GetState();
            if (st == xr::SessionState::Lost) {
                Teardown(TeardownReason::Lost, "the runtime lost the session");
                nextAttemptQpc_ = QpcNow() + MsToQpc(cfg_.retryIntervalS * 1000.0);
                continue;
            }
            if (st == xr::SessionState::ExitRequested) {
                Teardown(TeardownReason::ExitRequested, "the runtime asked the application to exit");
                holdAfterExit_ = !cfg_.reconnectAfterExit;
                if (holdAfterExit_) log::info("xr: staying off until 'xr-restart' ([xr] reconnect_after_exit = 0)");
                nextAttemptQpc_ = QpcNow() + MsToQpc(cfg_.retryIntervalS * 1000.0);
                continue;
            }
            const bool gamePaces = GameThreadPaces();
            if (mode_.load() == Mode::Stereo && gamePaces == xrPacesStereo_) {
                xrPacesStereo_ = !gamePaces;
                static uint64_t switches = 0;
                ++switches;
                if (switches > 32 && !PowerOfTwo(switches)) {
                    // rate limited
                } else if (xrPacesStereo_)
                    log::info("xr: stereo mode, but the game thread is not starting frames: the {} thread paces, the screen layer shows the game",
                              waitOnPresent_.load() ? "present" : "xr");
                else
                    log::info("xr: stereo mode: the game thread paces frames again");
            } else if (mode_.load() == Mode::Screen) {
                xrPacesStereo_ = false;
            }
            if (!gamePaces && !waitOnPresent_.load())
                PaceOnce();
            else
                Sleep(2);
            continue;
        }
        // Not initialised.
        if (restartRequested_.exchange(false)) {
            holdAfterExit_ = false;
            nextAttemptQpc_ = 0;
        }
        deviceChanged_ = false;
        if (holdAfterExit_ || now < nextAttemptQpc_) {
            Sleep(50);
            continue;
        }
        bool haveDevice = false;
        {
            std::lock_guard lk(devMutex_);
            if (seenDevice_) {
                device_ = seenDevice_;
                haveDevice = true;
            }
        }
        if (!haveDevice) {
            Sleep(50);
            continue;
        }
        TryInit();
    }
    log::info("xr: thread stopped");
}

void XrController::TryInit() {
    const bool quiet = failedAttempts_ > 0;  // repeated attempts: the backend's messages go to debug level
    xr::InitDesc d;
    d.backend = cfg_.backend;
    d.device = device_.Get();
    d.log = [quiet](xr::LogLevel level, std::string_view msg) {
        log::Level l = level == xr::LogLevel::Error ? log::Level::Error
                       : level == xr::LogLevel::Warn ? log::Level::Warn
                       : level == xr::LogLevel::Info || level == xr::LogLevel::Notice ? log::Level::Info
                                                                                        : log::Level::Debug;
        if (quiet && level != xr::LogLevel::Notice) l = log::Level::Debug;
        if (log::enabled(l)) log::write(l, std::string("xr: ") + std::string(msg));
    };
    d.runtime = cfg_.runtime;
    d.appName = "ff7-remake-vr";
    d.eyeWidth = cfg_.eyeWidth;
    d.eyeHeight = cfg_.eyeHeight;
    d.resolutionScale = cfg_.resolutionScale;
    d.disableImplicitApiLayers = cfg_.disableImplicitLayers;
    d.disableImplicitApiLayersMatching = cfg_.disableLayersMatching;
    d.enableDebugUtils = cfg_.debugUtils;
    d.gpuTiming = cfg_.gpuTiming;
    d.eyeGaze = foveation::EyeTrackingRequested();  // [foveation] eye_tracking = 1 or auto
    d.null.refreshHz = cfg_.nullRefreshHz;
    d.null.paceToRefresh = cfg_.nullPace;
    d.null.motion = cfg_.nullMotion;
    d.depthLayer = cfg_.depthLayer;

    auto be = xr::CreateBackend(cfg_.backend);
    const int64_t t0 = QpcNow();
    const xr::Result r = be->Init(d);
    const double ms = QpcToMs(QpcNow() - t0);
    if (r != xr::Result::Ok) {
        ++failedAttempts_;
        double delayS = cfg_.retryIntervalS;
        if (r == xr::Result::RuntimeUnavailable) delayS = std::max(delayS, 30.0);
        if (r == xr::Result::Error) delayS *= 2.0;
        const bool hold = r == xr::Result::GraphicsMismatch || r == xr::Result::InvalidArgument;
        if (failedAttempts_ == 1 || r != lastInitResult_) {
            log::warn("xr: no session ({} after {:.0f} ms){}", xr::ToString(r), ms,
                      hold ? "; not retrying (needs 'xr-restart' or a game restart)"
                           : std::format("; the game continues on the desktop, retrying every {:.0f} s", delayS));
        } else if (PowerOfTwo(failedAttempts_)) {
            log::info("xr: still no session after {} attempts ({})", failedAttempts_, xr::ToString(r));
        }
        lastInitResult_ = r;
        if (hold) holdAfterExit_ = true;
        nextAttemptQpc_ = QpcNow() + MsToQpc(delayS * 1000.0);
        return;
    }
    const xr::RuntimeInfo ri = be->GetRuntimeInfo();
    log::info("xr: session created in {:.0f} ms{}: runtime '{}' {}, system '{}', eyes {}x{} {} ({} images), refresh {:.1f} Hz, adapter LUID {:016X}",
              ms, failedAttempts_ ? std::format(" after {} failed attempts", failedAttempts_) : std::string(), ri.runtimeName,
              ri.runtimeVersion, ri.systemName, ri.eyeSwapchain[0].width, ri.eyeSwapchain[0].height,
              xr::DxgiFormatName(ri.eyeSwapchain[0].format), ri.eyeSwapchain[0].imageCount, ri.refreshHz, ri.adapterLuid);
    for (const auto& l : ri.implicitLayers) log::info("xr:   implicit API layer {}", l);
    depthAvailable_ = ri.depthSwapchainFormat != DXGI_FORMAT_UNKNOWN;
    log::info("xr: depth layer {}{}", depthAvailable_.load() ? std::string("available, swapchains ") + xr::DxgiFormatName(ri.depthSwapchainFormat)
                                                             : std::string("not available: ") + ri.depthNote,
              depthAvailable_.load() ? (depthOn_.load() ? ", on" : ", off ([xr] depth_layer = 0)") : "");
    failedAttempts_ = 0;
    lastInitResult_ = xr::Result::Ok;
    {
        std::lock_guard lk(rtMutex_);
        backend_ = std::move(be);
        screenLayer_ = 0;
        screenW_ = screenH_ = 0;
        uiLayer_ = 0;
        uiW_ = uiH_ = uiSrcW_ = uiSrcH_ = 0;
        uiShownLast_ = false;
        menuLayer_ = 0;
        menuPlacement_ = 0;
    }
    {
        std::lock_guard lk(queueMutex_);
        queue_.clear();
    }
    {
        std::lock_guard lk(eyeMutex_);
        eye_ = EyeSetup{};
        eye_.eyeWidth = ri.eyeSwapchain[0].width;
        eye_.eyeHeight = ri.eyeSwapchain[0].height;
        eye_.refreshHz = ri.refreshHz;
        gaze_ = GazeState{};
        gaze_.source = ri.gazeSource;
        gaze_.note = ri.gazeNote;
        eyeValid_ = true;
    }
    sessionStarted_ = false;
    hiddenBackendVersion_ = ~0u;
    lastFrameStats_ = xr::FrameStats{};
    lastStereoQpc_ = 0;
    ready_ = true;
}

bool XrController::GameThreadPaces() const {
    if (mode_.load() != Mode::Stereo) return false;
    const int64_t last = lastGameFrameQpc_.load();
    return last != 0 && QpcToMs(QpcNow() - last) <= kGameIdleMs;
}

bool XrController::WaitOne(bool fromGameThread, xr::FrameInfo* out) {
    const int64_t t0 = QpcNow();
    if (const float snap = snapPendingDeg_.exchange(0.0f, std::memory_order_acq_rel); snap != 0.0f)
        backend_->AddSnapYaw(-snap / xr::kRadToDeg);  // the backend's yaw is +Y (left); applied by this WaitFrame
    const xr::Result r = backend_->WaitFrame(*out);
    const int64_t t1 = QpcNow();
    if (r != xr::Result::Ok) {
        static std::atomic<uint64_t> errors{0};
        if (PowerOfTwo(++errors)) log::warn("xr: WaitFrame: {} (state {}, {} times)", xr::ToString(r), xr::ToString(out->state), errors.load());
        return false;
    }
    if (!out->sessionRunning) return false;
    GetTiming().wait.Add(QpcToMs(t1 - t0));
    if (!sessionStarted_) {
        sessionStarted_ = true;
        if (cfg_.recenterOnStart) backend_->Recenter();
        log::info("xr: frame loop running (state {}, frame wait on the {} thread)", xr::ToString(out->state),
                  fromGameThread ? "game" : (waitOnPresent_.load() ? "present" : "xr"));
    }
    snapYawDeg_.store(-backend_->SnapYaw() * xr::kRadToDeg, std::memory_order_relaxed);
    pause_on_remove::note(out->state, backend_->UserPresence(), mode_.load() == Mode::Stereo);
    lastOrientationValid_ = out->orientationValid;
    lastPositionValid_ = out->positionValid;
    if (!out->orientationValid)
        ++untrackedFrames_;
    else if (!out->positionValid)
        ++threeDofFrames_;
    {
        std::lock_guard lk(eyeMutex_);
        eye_.fov[0] = out->views[0].fov;
        eye_.fov[1] = out->views[1].fov;
        if (out->gaze.available) {
            const xr::GazeSample& g = out->gaze;
            gaze_.available = true;
            gaze_.tracked = g.tracked;
            gaze_.nominal = g.nominal;
            if (g.tracked) gaze_.headDirection = g.direction;
            // Each eye relative to the head (canted displays turn the eyes; most headsets do not).
            for (int e = 0; e < 2; ++e)
                gaze_.eyeFromHead[e] = xr::QuatNormalize(xr::QuatMultiply(xr::QuatConjugate(out->views[e].pose.orientation), out->head.orientation));
            gaze_.ageMs = g.sampleTime > 0 ? double(g.displayTime - g.sampleTime) / 1e6 : -1.0;
            gaze_.frameId = out->frameId;
            ++gaze_.samples;
        }
    }
    if (const uint32_t v = backend_->HiddenAreaMeshVersion(); v != hiddenBackendVersion_) {
        hiddenBackendVersion_ = v;
        xr::HiddenAreaMesh m[2];
        for (int e = 0; e < 2; ++e) backend_->GetHiddenAreaMesh(e == 0 ? xr::Eye::Left : xr::Eye::Right, &m[e]);
        {
            std::lock_guard lk(eyeMutex_);
            hidden_[0] = std::move(m[0]);
            hidden_[1] = std::move(m[1]);
        }
        hiddenVersion_.fetch_add(1);
    }
    {
        std::lock_guard lk(queueMutex_);
        queue_.push_back(Waited{*out, fromGameThread, t1});
    }
    queueCv_.notify_all();
    return true;
}

void XrController::PaceOnce() {
    {
        // One frame at a time: the next xrWaitFrame only after the Present hook took the last one.
        std::unique_lock lk(queueMutex_);
        if (!queue_.empty()) {
            queueCv_.wait_for(lk, std::chrono::milliseconds(20));
            return;
        }
    }
    xr::FrameInfo fi;
    bool got = false;
    {
        std::lock_guard wl(waitMutex_);
        if (!ready_.load() || stopping_.load()) return;
        got = WaitOne(false, &fi);
    }
    if (!got) Sleep(10);  // session not running yet (or idle): poll again shortly
}

void XrController::Teardown(TeardownReason why, const char* text) {
    log::info("xr: ending the session: {}", text);
    const int64_t t0 = QpcNow();
    stopping_ = true;
    std::lock_guard cl(cmdMutex_);
    bool parked = false;
    std::unique_lock<std::mutex> wl(waitMutex_, std::defer_lock);
    if (why != TeardownReason::ProcessExit) {
        wl.lock();  // a game-thread WaitFrame in progress finishes first
        // Shutdown ends the open frames, which uses the immediate context: park the render thread.
        {
            std::unique_lock pl(parkMutex_);
            parkRequested_ = true;
            parkCv_.wait_for(pl, std::chrono::milliseconds(1500), [&] { return parked_; });
            parked = parked_;
        }
    }
    {
        std::unique_lock<std::mutex> rl(rtMutex_, std::defer_lock);
        for (int i = 0; i < 200 && !rl.try_lock(); ++i) Sleep(5);
        ready_ = false;
        depthAvailable_ = false;
        std::lock_guard relocLock(relocMutex_);  // a late location in progress finishes first (never waits on anything)
        if (backend_) {
            backend_->Shutdown();
            backend_.reset();
        }
        screenLayer_ = 0;
        screenW_ = screenH_ = 0;
        uiLayer_ = 0;
        uiW_ = uiH_ = uiSrcW_ = uiSrcH_ = 0;
        uiShownLast_ = false;
        menuLayer_ = 0;
        menuPlacement_ = 0;
        lastStereoQpc_ = 0;
    }
    {
        std::lock_guard lk(queueMutex_);
        queue_.clear();
    }
    queueCv_.notify_all();
    {
        std::lock_guard lk(stereoMutex_);
        stereoQueue_.clear();
    }
    {
        std::lock_guard lk(eyeMutex_);
        eyeValid_ = false;
        gaze_ = GazeState{};
    }
    {
        std::lock_guard pl(parkMutex_);
        parkRequested_ = false;
    }
    parkCv_.notify_all();
    stopping_ = false;
    log::info("xr: session ended in {:.0f} ms{}", QpcToMs(QpcNow() - t0),
              why == TeardownReason::ProcessExit ? " (process exit)" : (parked ? "" : " (render thread was not presenting)"));
}

void XrController::LogStats() {
    Timing& t = GetTiming();
    const size_t frames = t.frameInterval.Count();
    const double seconds = QpcToMs(QpcNow() - lastStatsQpc_) / 1000.0;
    const xr::SessionState st = ready_.load() ? backend_->GetState() : xr::SessionState::Uninitialized;
    if (frames == 0 && !ready_.load()) return;  // nothing happening, keep the log quiet
    log::info("timing: last {:.1f} s, {:.1f} fps; xr {} ({}); mode {}; presents {} submitted screen {} stereo {} held {} without XR frame {} errors {}; "
              "ui layer {} held {} dropped {}",
              seconds, seconds > 0 ? double(frames) / seconds : 0.0, ready_.load() ? BackendName(cfg_.backend) : "off", xr::ToString(st),
              ModeName(mode_.load()), presents_.load(), submittedScreen_.load(), submittedStereo_.load(), heldStereo_.load(),
              presentsWithoutFrame_.load(), submitErrors_.load(), uiSubmitted_.load(), uiHeld_.load(), uiDropped_.load());
    std::string runtimeCalls;
    if (ready_.load()) {
        std::lock_guard rl(rtMutex_);
        if (backend_) {
            for (float ms : backend_->TakeGpuCopyTimes()) t.gpuCopy.Add(ms);
            const xr::FrameStats cur = backend_->GetStats();
            const uint64_t ended = (cur.framesSubmitted + cur.framesSkipped) - (lastFrameStats_.framesSubmitted + lastFrameStats_.framesSkipped);
            if (ended > 0 && cur.framesSubmitted >= lastFrameStats_.framesSubmitted)
                runtimeCalls = std::format("runtime calls per ended frame (CPU, presenting thread): acquire+wait image {:.3f} ms, release image {:.3f} ms, "
                                           "begin frame {:.3f} ms, end frame {:.3f} ms (n {})",
                                           (cur.acquireWaitMs - lastFrameStats_.acquireWaitMs) / double(ended),
                                           (cur.releaseMs - lastFrameStats_.releaseMs) / double(ended),
                                           (cur.beginFrameMs - lastFrameStats_.beginFrameMs) / double(ended),
                                           (cur.endFrameMs - lastFrameStats_.endFrameMs) / double(ended), ended);
            lastFrameStats_ = cur;
        }
    }
    for (Series* s : {&t.frameInterval, &t.presentCall, &t.hook, &t.submit, &t.wait, &t.gpuCopy, &t.gpu}) {
        const std::string line = s->TakeSummary();
        if (!line.empty()) log::info("timing:   {}", line);
    }
    for (const std::string& line : foveation::TakeTimingLines()) log::info("timing:   {}", line);
    // The gamepad path (the loader's XInput proxy and the engine's pad filter), when present.
    for (const char* cmd : {"xinput timing", "controls timing"}) {
        std::string reply;
        if (dev_commands::dispatch(cmd, reply) && reply.starts_with("ok ")) log::info("timing:   controls: {}", reply.substr(3));
    }
    if (!runtimeCalls.empty()) log::info("timing:   {}", runtimeCalls);
    ComPtr<ID3D11Device> dev;
    {
        std::lock_guard lk(devMutex_);
        dev = seenDevice_;
    }
    if (dev) video_memory::LogReport(dev.Get());
}

std::string XrController::VideoMemoryStatus() {
    ComPtr<ID3D11Device> dev;
    {
        std::lock_guard lk(devMutex_);
        dev = seenDevice_;
    }
    if (!dev) return "err no D3D11 device seen yet";
    const std::string l = video_memory::Line(video_memory::Query(dev.Get()));
    return l.empty() ? "err QueryVideoMemoryInfo failed" : "ok " + l;
}

void XrController::StereoTestThread() {
    SetThreadDescription(GetCurrentThread(), L"ff7vr stereo test");
    log::info("render: stereo test: a test thread calls BeginGameFrame and the back buffer is submitted as both eyes");
    while (!stop_.load()) {
        if (QpcNow() < testPauseUntilQpc_.load()) {
            Sleep(5);  // stands in for a game thread blocked by a load
            continue;
        }
        if (mode_.load() == Mode::Stereo && ready_.load()) {
            const StereoFrame f = BeginGameFrame();
            if (!f.stereo) Sleep(5);
        } else {
            Sleep(20);
        }
    }
}

// ---------------------------------------------------------------------------
// Render thread (Present hook)
// ---------------------------------------------------------------------------
void XrController::ParkPoint() {
    std::unique_lock pl(parkMutex_);
    if (!parkRequested_) return;
    parked_ = true;
    parkCv_.notify_all();
    parkCv_.wait_for(pl, std::chrono::seconds(10), [&] { return !parkRequested_; });
    parked_ = false;
}

void XrController::OnResize(IDXGISwapChain*) {
    // Nothing to release: no reference to a back buffer is kept beyond a Present call.
}

bool XrController::EnsureScreenLayer(const PresentInfo& p) {
    if (screenLayer_ && screenW_ == p.width && screenH_ == p.height && screenFmt_ == p.format) return true;
    if (screenLayer_) backend_->DestroyQuadLayer(screenLayer_);
    screenLayer_ = 0;
    const xr::QuadLayerCreateDesc qd{p.width, p.height, TypedFormat(p.format)};
    const xr::Result r = backend_->CreateQuadLayer(qd, &screenLayer_);
    if (r != xr::Result::Ok) {
        static std::atomic<uint64_t> fails{0};
        if (PowerOfTwo(++fails)) log::error("xr: screen layer {}x{} could not be created: {}", p.width, p.height, xr::ToString(r));
        screenLayer_ = 0;
        return false;
    }
    screenW_ = p.width;
    screenH_ = p.height;
    screenFmt_ = p.format;
    xr::SwapchainInfo si;
    backend_->GetQuadLayerInfo(screenLayer_, &si);
    log::info("xr: screen layer {}x{} {} (back buffer {}), {:.2f} x {:.2f} m at {:.2f} m{}", si.width, si.height,
              xr::DxgiFormatName(si.format), xr::DxgiFormatName(p.format), cfg_.screenWidth,
              cfg_.screenWidth * float(p.height) / float(std::max(1u, p.width)), cfg_.screenDistance,
              cfg_.screenFollowHead ? ", head-locked" : "");
    return true;
}

bool XrController::EnsureUiLayer(const UiLayerSource& s) {
    const DXGI_FORMAT fmt = TypedFormat(s.viewFormat);
    if (uiLayer_ && uiSrcW_ == s.width && uiSrcH_ == s.height && uiFmt_ == fmt) return true;
    if (uiLayer_) backend_->DestroyQuadLayer(uiLayer_);
    uiLayer_ = 0;
    uiShownLast_ = false;
    // Layer image: [ui] layer_width wide (never wider than the game's UI texture), the UI's
    // aspect ratio. A larger UI texture is scaled down when it is copied in.
    uint32_t w = cfg_.uiLayerWidth ? std::min(cfg_.uiLayerWidth, s.width) : s.width;
    uint32_t h = static_cast<uint32_t>(std::lround(double(w) * s.height / std::max(1u, s.width)));
    w = std::max(1u, w);
    h = std::max(1u, h);
    const xr::QuadLayerCreateDesc qd{w, h, fmt};
    const xr::Result r = backend_->CreateQuadLayer(qd, &uiLayer_);
    if (r != xr::Result::Ok) {
        static std::atomic<uint64_t> fails{0};
        if (PowerOfTwo(++fails)) log::error("xr: UI layer {}x{} could not be created: {}", w, h, xr::ToString(r));
        uiLayer_ = 0;
        return false;
    }
    uiSrcW_ = s.width;
    uiSrcH_ = s.height;
    uiFmt_ = fmt;
    xr::SwapchainInfo si;
    backend_->GetQuadLayerInfo(uiLayer_, &si);
    uiW_ = si.width;
    uiH_ = si.height;
    log::info("xr: UI layer {}x{} {} for the game's {}x{} {} UI texture", si.width, si.height, xr::DxgiFormatName(si.format), s.width,
              s.height, xr::DxgiFormatName(s.viewFormat));
    return true;
}

bool XrController::MenuQuad(const PresentInfo& p, const Waited& w, xr::QuadLayer* q) {
    if (!menu::IsOpen()) return false;
    bool fresh = false;
    if (!menuLayer_) {
        const xr::QuadLayerCreateDesc qd{1024, 720, DXGI_FORMAT_R8G8B8A8_UNORM};
        if (backend_->CreateQuadLayer(qd, &menuLayer_) != xr::Result::Ok) {
            static std::atomic<uint64_t> fails{0};
            if (PowerOfTwo(++fails)) log::error("xr: settings panel layer could not be created");
            menuLayer_ = 0;
            return false;
        }
        xr::SwapchainInfo si;
        backend_->GetQuadLayerInfo(menuLayer_, &si);
        log::info("xr: settings panel layer {}x{} {}", si.width, si.height, xr::DxgiFormatName(si.format));
        fresh = true;
    }
    menu::PanelFrame f;
    if (!menu::Frame(p.device, p.context, fresh, &f)) return false;
    if (f.placement != menuPlacement_) {
        // Placed when it opens (and after a recenter): head-locked like the HUD panel when that
        // follows the head, otherwise straight ahead of where the head looks now, level, at eye
        // height, and fixed in the room from then on.
        menuPlacement_ = f.placement;
        bool follow = false;
        {
            std::lock_guard lk(uiMutex_);
            follow = cfg_.uiFollowHead;
        }
        menuHeadLocked_ = follow;
        if (follow) {
            menuPose_ = xr::Pose{xr::Quat{}, xr::Vec3{0.0f, 0.0f, -f.distance}};
        } else {
            const xr::Quat yaw = xr::QuatFromAxisAngle(xr::Vec3{0.0f, 1.0f, 0.0f}, xr::QuatYaw(w.info.head.orientation));
            const xr::Vec3 ahead = xr::QuatRotate(yaw, xr::Vec3{0.0f, 0.0f, -f.distance});
            const xr::Vec3& h = w.info.head.position;
            menuPose_ = xr::Pose{yaw, xr::Vec3{h.x + ahead.x, h.y + ahead.y, h.z + ahead.z}};
        }
    }
    q->layer = menuLayer_;
    if (f.texture) {
        q->texture = f.texture;
        q->viewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        q->encoding = xr::ColorEncoding::Srgb;
        q->rect = xr::Rect{0, 0, f.width, f.height};
        q->sourceAlpha = xr::SourceAlpha::Premultiplied;
    }
    q->space = menuHeadLocked_ ? xr::LayerSpace::Head : xr::LayerSpace::World;
    q->pose = menuPose_;
    q->width = f.widthM;
    q->height = f.heightM;
    q->alphaBlend = true;
    return true;
}

// The UI of a redirected frame is in neither eye image, so the desktop window (which shows
// a crop of an eye in stereo) gets it drawn on top: the whole 16:9 UI over the whole window
// (letterboxed if the window has another aspect), blended like the game draws its UI.
void XrController::DrawUiOnWindow(const PresentInfo& p, const PendingUi& ui) {
    if (!uiMirror_.load(std::memory_order_relaxed) || !ui.src.redirected || !ui.texture || !p.backBuffer) return;
    const double sa = double(ui.src.width) / std::max(1u, ui.src.height), wa = double(p.width) / std::max(1u, p.height);
    xr::Rect r{0, 0, p.width, p.height};
    if (wa > sa) {
        r.width = static_cast<uint32_t>(std::lround(p.height * sa));
        r.x = static_cast<int32_t>((p.width - r.width) / 2);
    } else if (wa < sa) {
        r.height = static_cast<uint32_t>(std::lround(p.width / sa));
        r.y = static_cast<int32_t>((p.height - r.height) / 2);
    }
    xr::QuadLayer q;
    q.texture = ui.texture.Get();
    q.viewFormat = ui.src.viewFormat;
    q.encoding = ui.src.encoding;
    q.rect = xr::Rect{0, 0, ui.src.width, ui.src.height};
    q.sourceAlpha = ui.src.alpha;
    const bool linear = p.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!backend_->DrawOverlay(q, p.backBuffer, TypedFormat(p.format), linear ? xr::ColorEncoding::Linear : xr::ColorEncoding::Srgb, r)) {
        static std::atomic<uint64_t> fails{0};
        if (PowerOfTwo(++fails)) log::warn("render: the UI could not be drawn over the desktop window ({} times)", fails.load());
    }
}

void XrController::SubmitOne(const PresentInfo& p, const Waited& w, const PendingUi* ui) {
    xr::SubmitDesc d;
    xr::QuadLayer q;
    xr::QuadLayer uq;
    xr::QuadLayer quads[2];  // back to front: the HUD panel or the virtual screen, then the settings panel
    uint32_t quadCount = 0;
    StereoSubmit s;
    ComPtr<ID3D11Texture2D> stereoTex;
    ComPtr<ID3D11Texture2D> depthTex;
    bool stereo = false;
    {
        std::lock_guard lk(stereoMutex_);
        for (const PendingStereo& ps : stereoQueue_)
            if (ps.submit.frameId == w.info.frameId && ps.texture) {
                stereo = true;
                s = ps.submit;
                stereoTex = ps.texture;
                depthTex = ps.depth;
            }
        // Images for this frame or older ones are done with (a frame ends exactly once).
        std::erase_if(stereoQueue_, [&](const PendingStereo& ps) { return ps.submit.frameId <= w.info.frameId; });
    }
    // No image for this frame, but one shortly before (a hitch, a dropped frame
    // id): show that image again, re-projected with the poses it was rendered
    // with, instead of the back buffer (which holds the desktop mirror in stereo).
    const bool hold = !stereo && mode_.load() == Mode::Stereo && lastStereoQpc_ != 0 &&
                      QpcToMs(QpcNow() - lastStereoQpc_) < kStereoHoldMs;
    if (stereo) {
        d.texture = stereoTex.Get();
        d.viewFormat = s.viewFormat;
        d.encoding = s.encoding;
        for (int e = 0; e < 2; ++e) {
            d.eyes[e].rect = s.eyeRects[e];
            if (s.haveRenderedViews) d.eyes[e].viewOverride = &s.renderedViews[e];
        }
        if (depthTex && depthOn_.load(std::memory_order_relaxed)) {
            d.depth.texture = depthTex.Get();
            d.depth.srvFormat = s.depthSrvFormat;
            d.depth.rect[0] = s.depthRects[0];
            d.depth.rect[1] = s.depthRects[1];
            d.depth.minDepth = 0.0f;
            d.depth.maxDepth = 1.0f;
            d.depth.farZ = s.depthFarZ;
            // Infinite is allowed by the specification, but a runtime that turns nearZ/farZ
            // into a projection (Virtual Desktop's: far * near / (near - far)) gets NaN from it.
            d.depth.nearZ = std::isinf(s.depthNearZ) && cfg_.depthFarM > 0.0f ? cfg_.depthFarM : s.depthNearZ;
            ++depthOffered_;
        }
    } else if (hold) {
        // Both eyes keep their last image (nothing is read from the texture,
        // which only has to pass validation).
        d.texture = p.backBuffer;
        d.viewFormat = TypedFormat(p.format);
        d.eyes[0].update = d.eyes[1].update = false;
    }
    bool uiShown = false;
    if (stereo || hold) {
        // The in-game UI on its own layer, over the eye images: new content when the
        // engine redirected this frame's UI, the last image again when this frame
        // re-shows the last stereo image.
        const bool fresh = stereo && ui && ui->src.redirected && ui->texture;
        if ((fresh && EnsureUiLayer(ui->src)) || (!fresh && hold && uiShownLast_ && uiLayer_)) {
            float dist, size, ox, oy;
            bool follow;
            {
                std::lock_guard lk(uiMutex_);
                dist = cfg_.uiDistance;
                size = cfg_.uiSize;
                ox = cfg_.uiOffsetX;
                oy = cfg_.uiOffsetY;
                follow = cfg_.uiFollowHead;
            }
            size = ui_battle::Height(size);  // [ui] battle_size while a battle is in progress
            uq.layer = uiLayer_;
            if (fresh) {
                uq.texture = ui->texture.Get();
                uq.viewFormat = ui->src.viewFormat;
                uq.encoding = ui->src.encoding;
                uq.rect = xr::Rect{0, 0, ui->src.width, ui->src.height};
                uq.sourceAlpha = ui->src.alpha;
            }
            uq.space = follow ? xr::LayerSpace::Head : xr::LayerSpace::World;
            uq.pose.position = xr::Vec3{ox, oy, -dist};
            uq.height = size;
            uq.width = size * float(uiSrcW_) / float(std::max(1u, uiSrcH_));
            uq.alphaBlend = true;
            quads[quadCount++] = uq;
            uiShown = true;
        }
    }
    if (!stereo && !hold) {
        if (!EnsureScreenLayer(p)) {
            backend_->SkipFrame(w.info.frameId);
            return;
        }
        q.layer = screenLayer_;
        q.texture = p.backBuffer;
        q.viewFormat = TypedFormat(p.format);
        q.encoding = p.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? xr::ColorEncoding::Linear : xr::ColorEncoding::Srgb;
        q.space = cfg_.screenFollowHead ? xr::LayerSpace::Head : xr::LayerSpace::World;
        q.pose.position = xr::Vec3{0.0f, cfg_.screenOffsetY, -cfg_.screenDistance};
        q.width = cfg_.screenWidth;
        q.height = cfg_.screenWidth * float(p.height) / float(std::max(1u, p.width));
        q.adjustPicture = true;
        quads[quadCount++] = q;
    }
    if (xr::QuadLayer mq; MenuQuad(p, w, &mq)) quads[quadCount++] = mq;
    d.quads = quadCount ? quads : nullptr;
    d.quadCount = quadCount;
    {
        std::lock_guard lk(pictureMutex_);
        d.picture = picture_;
    }
    if (stereo) {
        xr::Fov fov[2];
        {
            std::lock_guard lk(eyeMutex_);
            fov[0] = eye_.fov[0];
            fov[1] = eye_.fov[1];
        }
        d.vignette = comfort::Update(fov);
        d.sharpen = comfort::Sharpen();
    }
    if (cfg_.gpuTiming) gpu_.Begin(p.device, p.context);
    if (ui) DrawUiOnWindow(p, *ui);
    const int64_t t0 = QpcNow();
    const xr::Result r = backend_->SubmitFrame(w.info.frameId, d);
    GetTiming().submit.Add(QpcToMs(QpcNow() - t0));
    if (cfg_.gpuTiming) gpu_.End(p.context);
    if (r == xr::Result::Ok) {
        if (uiShown) ++(uq.texture ? uiSubmitted_ : uiHeld_);
        if (stereo) {
            uiShownLast_ = uiShown;
            ++submittedStereo_;
            lastStereoQpc_ = QpcNow();
        } else if (hold) {
            ++heldStereo_;
        } else {
            ++submittedScreen_;
            if (mode_.load() == Mode::Stereo) ++screenInStereo_;
        }
    } else if (PowerOfTwo(++submitErrors_)) {
        log::warn("xr: SubmitFrame({}): {} ({} errors so far)", w.info.frameId, xr::ToString(r), submitErrors_.load());
    }
}

void XrController::OnPresent(const PresentInfo& p) {
    ++presents_;
    if (cfg_.brightnessUpKey || cfg_.brightnessDownKey) PollPictureKeys();
    // The UI texture reported for this frame (before its Present) belongs to this Present only.
    PendingUi ui;
    bool haveUi = false;
    {
        std::lock_guard lk(uiMutex_);
        if (havePendingUi_) {
            ui = std::move(pendingUi_);
            pendingUi_ = PendingUi{};
            havePendingUi_ = false;
            haveUi = true;
        }
    }
    if (haveUi && uiDumpRequested_.load() && ui.texture) {
        std::string path, err;
        {
            std::lock_guard lk(uiMutex_);
            path = uiDumpPath_;
        }
        const bool ok = xr::WriteTexturePng(p.context, ui.texture.Get(), path, &err);
        {
            std::lock_guard lk(uiMutex_);
            uiDumpResult_ = ok ? std::format("ok {} ({}x{} UI in a {} texture, {})", path, ui.src.width, ui.src.height,
                                             xr::DxgiFormatName(ui.src.viewFormat), ui.src.redirected ? "redirected" : "composited")
                               : "err " + err;
            uiDumpRequested_ = false;
        }
        uiDumpCv_.notify_all();
    }
    {
        std::lock_guard lk(devMutex_);
        if (seenDevice_.Get() != p.device) {
            if (seenDevice_) log::warn("render: the main swap chain is on a different D3D11 device now ({})", static_cast<void*>(p.device));
            seenDevice_ = p.device;
            if (device_ && device_.Get() != p.device) deviceChanged_ = true;
        }
    }
    ParkPoint();
    if (!ready_.load(std::memory_order_acquire)) return;
    if (cfg_.gpuTiming) gpu_.Collect(p.context, GetTiming().gpu);

    std::unique_lock rl(rtMutex_);
    if (!ready_.load() || !backend_ || device_.Get() != p.device) return;

    // The screen layer exists whenever a session runs, so a switch to it never
    // has to create a swapchain at that moment (cheap check when it exists).
    EnsureScreenLayer(p);

    if (waitOnPresent_.load() && !GameThreadPaces()) {
        // Frame wait on this thread: blocks the game for pacing. try_lock: never wait behind a teardown.
        std::unique_lock wl(waitMutex_, std::try_to_lock);
        bool empty = false;
        {
            std::lock_guard lk(queueMutex_);
            empty = queue_.empty();
        }
        if (wl.owns_lock() && empty && !stopping_.load()) {
            xr::FrameInfo fi;
            WaitOne(false, &fi);
        }
    }
    if (cfg_.stereoTest && mode_.load() == Mode::Stereo) {
        // Test of the stereo path without the engine: the back buffer is the "side-by-side" image, both eyes.
        uint64_t id = 0;
        {
            std::lock_guard lk(queueMutex_);
            for (const Waited& w : queue_)
                if (w.fromGameThread) {
                    id = w.info.frameId;
                    break;
                }
        }
        const uint32_t drop = testDropEvery_.load();
        if (id && drop && (++testFrames_ % drop) == 0) id = 0;  // test: no image for this frame
        if (id) {
            StereoSubmit s;
            s.frameId = id;
            s.texture = p.backBuffer;
            s.viewFormat = TypedFormat(p.format);
            s.eyeRects[0] = s.eyeRects[1] = xr::Rect{0, 0, p.width, p.height};
            SubmitStereoFrame(s);
        }
    }
    Waited w;
    {
        std::lock_guard lk(queueMutex_);
        if (queue_.empty()) {
            ++presentsWithoutFrame_;
            if (haveUi && ui.src.redirected) {
                ++uiDropped_;
                DrawUiOnWindow(p, ui);
            }
            return;
        }
        w = queue_.front();
        queue_.pop_front();
    }
    queueCv_.notify_all();
    SubmitOne(p, w, haveUi ? &ui : nullptr);
}

// ---------------------------------------------------------------------------
// Engine interface
// ---------------------------------------------------------------------------
bool XrController::GetHiddenArea(int eye, xr::HiddenAreaMesh* out, uint32_t* version) {
    std::lock_guard lk(eyeMutex_);
    if (version) *version = hiddenVersion_.load();
    const xr::HiddenAreaMesh& m = hidden_[eye == 1 ? 1 : 0];
    if (!eyeValid_ || m.indices.size() < 3) return false;
    if (out) *out = m;
    return true;
}

bool XrController::GetGaze(GazeState* out) {
    std::lock_guard lk(eyeMutex_);
    if (out) *out = gaze_;
    return eyeValid_ && gaze_.available;
}

bool XrController::GetEyeSetup(EyeSetup* out) {
    std::lock_guard lk(eyeMutex_);
    if (out) *out = eye_;
    return eyeValid_;
}

StereoFrame XrController::BeginGameFrame() {
    StereoFrame f;
    if (mode_.load() != Mode::Stereo) return f;
    lastGameFrameQpc_ = QpcNow();
    if (!ready_.load() || stopping_.load()) return f;
    {
        // Never block the game when earlier frames are not being presented.
        std::lock_guard lk(queueMutex_);
        const int64_t now = QpcNow();
        const bool stale = std::any_of(queue_.begin(), queue_.end(), [&](const Waited& w) { return QpcToMs(now - w.waitedQpc) > 250.0; });
        if (stale || queue_.size() >= 2) {
            if (PowerOfTwo(++monoFallbacks_)) log::info("xr: game thread renders mono: earlier frames not presented ({} times)", monoFallbacks_.load());
            return f;
        }
    }
    std::lock_guard wl(waitMutex_);
    if (!ready_.load() || stopping_.load()) return f;
    xr::FrameInfo fi;
    bool adopted = false;
    {
        // The XR thread may have waited a frame while it still paced (hand-over): at most two ahead.
        std::lock_guard lk(queueMutex_);
        if (queue_.size() >= 2) return f;
        if (queue_.size() == 1 && !queue_.front().fromGameThread) {
            // Take that frame over instead of waiting another one: a runtime blocks a second
            // xrWaitFrame until this frame is begun, which only a Present does, and when the
            // game's pipeline is idle no Present comes until this game frame is rendered.
            queue_.front().fromGameThread = true;
            fi = queue_.front().info;
            adopted = true;
        }
    }
    if (!adopted && !WaitOne(true, &fi)) return f;
    // The backend's pose filter already holds the views when the orientation is invalid
    // and holds the position when only the position is (fi.orientationValid /
    // positionValid say which), so the values are usable either way. This is the last
    // guard before the engine: a value that is still not a finite unit pose (which the
    // filter should make impossible) is replaced by the last views given to the game.
    auto finitePose = [](const xr::Pose& p) {
        const float v[7] = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y, p.position.z};
        for (float x : v)
            if (!std::isfinite(x)) return false;
        const float n = p.orientation.x * p.orientation.x + p.orientation.y * p.orientation.y + p.orientation.z * p.orientation.z +
                        p.orientation.w * p.orientation.w;
        return n > 0.81f && n < 1.21f;
    };
    if (!(finitePose(fi.views[0].pose) && finitePose(fi.views[1].pose) && finitePose(fi.head))) {
        if (PowerOfTwo(++rejectedGameFrames_))
            log::warn("xr: frame {} has views that are not usable; the last views are given to the game ({} times)", fi.frameId, rejectedGameFrames_);
        if (haveGameViews_) {
            fi.views[0] = lastGameViews_[0];
            fi.views[1] = lastGameViews_[1];
            fi.head = lastGameHead_;
        } else {
            fi.head = xr::Pose{};
            for (int e = 0; e < 2; ++e) fi.views[e].pose = xr::Pose{xr::Quat{}, xr::Vec3{e == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};
        }
    }
    lastGameViews_[0] = fi.views[0];
    lastGameViews_[1] = fi.views[1];
    lastGameHead_ = fi.head;
    haveGameViews_ = true;
    f.stereo = true;
    f.frameId = fi.frameId;
    f.shouldRender = fi.shouldRender;
    f.views[0] = fi.views[0];
    f.views[1] = fi.views[1];
    f.head = fi.head;
    f.predictedDisplayTime = fi.predictedDisplayTime;
    f.predictedDisplayPeriod = fi.predictedDisplayPeriod;
    return f;
}

void XrController::SubmitStereoFrame(const StereoSubmit& s) {
    if (!s.texture) return;
    std::lock_guard lk(stereoMutex_);
    PendingStereo ps;
    ps.submit = s;
    ps.submit.texture = nullptr;
    ps.submit.depthTexture = nullptr;
    ps.texture = s.texture;  // keeps it alive until the Present that submits it
    ps.depth = s.depthTexture;
    // A newer image for the same frame replaces the older one.
    auto same = std::find_if(stereoQueue_.begin(), stereoQueue_.end(), [&](const PendingStereo& e) { return e.submit.frameId == s.frameId; });
    if (same != stereoQueue_.end()) {
        *same = std::move(ps);
        return;
    }
    stereoQueue_.push_back(std::move(ps));
    while (stereoQueue_.size() > 4) stereoQueue_.pop_front();
}

bool XrController::DepthLayerWanted() const {
    return depthOn_.load(std::memory_order_relaxed) && depthAvailable_.load(std::memory_order_relaxed) && ready_.load(std::memory_order_relaxed) &&
           mode_.load(std::memory_order_relaxed) == Mode::Stereo;
}

bool XrController::RelocateViews(uint64_t frameId, xr::View views[2]) {
    // Never wait: a teardown holds this lock only around Shutdown, and the render thread
    // must not stall behind it (the frame then keeps the views it was built with).
    std::unique_lock lk(relocMutex_, std::try_to_lock);
    if (!lk.owns_lock()) {
        ++relocBusy_;
        return false;
    }
    if (!ready_.load(std::memory_order_acquire) || stopping_.load() || !backend_) return false;
    xr::View v[2];
    if (backend_->RelocateViews(frameId, v) != xr::Result::Ok) {
        ++relocFailed_;
        return false;
    }
    views[0] = v[0];
    views[1] = v[1];
    ++relocOk_;
    return true;
}

std::string XrController::DepthCommand(const std::string& args) {
    std::string verb = args, rest;
    if (const size_t sp = args.find(' '); sp != std::string::npos) {
        verb = args.substr(0, sp);
        rest = args.substr(sp + 1);
    }
    if (verb == "on" || verb == "off") {
        depthOn_ = verb == "on";
        log::info("xr: depth layer switched {}", verb);
    } else if (verb == "dump") {
        if (rest.empty()) return "err usage: xr-depth dump <path prefix> [timeout ms] (writes <prefix>_depthL.png / _depthR.png with the eye images)";
        uint32_t timeoutMs = 5000;
        if (const size_t sp = rest.rfind(' '); sp != std::string::npos) {
            uint32_t v = 0;
            const char* end = rest.data() + rest.size();
            const auto [p, ec] = std::from_chars(rest.data() + sp + 1, end, v);
            if (ec == std::errc() && p == end && v > 0) {
                timeoutMs = v;
                rest.resize(sp);
            }
        }
        return Capture(rest, timeoutMs);
    } else if (!verb.empty() && verb != "status") {
        return "err usage: xr-depth status | on | off | dump <path prefix>";
    }
    std::string fmt = "none", note;
    xr::FrameStats st{};
    {
        std::lock_guard lk(rtMutex_);
        if (backend_) {
            const xr::RuntimeInfo ri = backend_->GetRuntimeInfo();
            fmt = ri.depthSwapchainFormat == DXGI_FORMAT_UNKNOWN ? std::string("none") : xr::DxgiFormatName(ri.depthSwapchainFormat);
            note = ri.depthNote;
            st = backend_->GetStats();
        }
    }
    return std::format("ok depth layer {} ({}); swapchains {}{}; frames offered the scene depth {}, depth images written {}, frames ended with depth {}, "
                       "depth images not written {}; nearZ for infinite depth {} m; late view locations ok {} failed {} skipped (busy) {}",
                       depthOn_.load() ? "on" : "off", DepthLayerWanted() ? "in use" : "not in use", fmt, note.empty() ? "" : " (" + note + ")",
                       depthOffered_.load(), st.depthImages, st.depthLayers, st.depthFailures, cfg_.depthFarM, relocOk_.load(), relocFailed_.load(),
                       relocBusy_.load());
}

bool XrController::UiLayerWanted() const {
    return uiOn_.load(std::memory_order_relaxed) && mode_.load(std::memory_order_relaxed) == Mode::Stereo &&
           ready_.load(std::memory_order_acquire) && !stopping_.load(std::memory_order_relaxed);
}

void XrController::SubmitUiLayer(const UiLayerSource& s) {
    if (!s.texture || !s.width || !s.height) return;
    std::lock_guard lk(uiMutex_);
    pendingUi_.src = s;
    pendingUi_.src.texture = nullptr;
    pendingUi_.texture = s.texture;  // until this frame's Present
    havePendingUi_ = true;
}

std::string XrController::UiCommand(const std::string& args) {
    const std::string verb = args.substr(0, args.find(' '));
    std::string rest = verb.size() < args.size() ? args.substr(verb.size() + 1) : std::string();
    auto number = [](const std::string& t, float* out) {
        const char* b = t.data();
        const char* e = t.data() + t.size();
        while (b < e && *b == ' ') ++b;
        const auto [ptr, ec] = std::from_chars(b, e, *out);
        return ec == std::errc();
    };
    const char* usage =
        "err usage: ui status | on | off | dump <png path> | distance <m> | size <m> | battle_size <m> | offset <x m> <y m> | follow <0|1> | mirror <0|1>";
    if (verb.empty() || verb == "status") {
        std::lock_guard lk(uiMutex_);
        return std::format("ok ui layer {} ({}), {:.2f} m high at {:.2f} m, offset {:.2f} {:.2f}, {}, on the window {}; image {}x{} from {}x{}; "
                           "frames {} held {} dropped {}",
                           uiOn_.load() ? "on" : "off", UiLayerWanted() ? "active" : "inactive", cfg_.uiSize, cfg_.uiDistance, cfg_.uiOffsetX,
                           cfg_.uiOffsetY, cfg_.uiFollowHead ? "head-locked" : "world-locked", uiMirror_.load() ? "too" : "no", uiW_, uiH_,
                           uiSrcW_, uiSrcH_, uiSubmitted_.load(), uiHeld_.load(), uiDropped_.load()) +
               "; " + ui_battle::Text();
    }
    if (verb == "battle_size") return ui_battle::SetSize(rest) ? "ok ui " + ui_battle::Text() : std::string(usage);
    if (verb == "on" || verb == "off") {
        uiOn_ = verb == "on";
        log::info("render: UI layer {}", verb);
        return "ok ui " + verb;
    }
    if (verb == "dump") {
        while (!rest.empty() && rest.back() == ' ') rest.pop_back();
        if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
        if (rest.empty()) return usage;
        std::filesystem::path path = std::filesystem::path(log::widen(rest));
        if (path.is_relative()) path = cfg_.captureDir / path;
        std::unique_lock lk(uiMutex_);
        uiDumpPath_ = log::narrow(path.wstring());
        uiDumpResult_.clear();
        uiDumpRequested_ = true;
        if (!uiDumpCv_.wait_for(lk, std::chrono::seconds(5), [&] { return !uiDumpResult_.empty(); })) {
            uiDumpRequested_ = false;
            return "err no UI texture reported within 5 s";
        }
        return uiDumpResult_;
    }
    float v = 0, v2 = 0;
    std::lock_guard lk(uiMutex_);
    const size_t sp = rest.find(' ');
    if (verb == "distance" && number(rest, &v)) {
        cfg_.uiDistance = std::clamp(v, 0.3f, 50.0f);
    } else if (verb == "size" && number(rest, &v)) {
        cfg_.uiSize = std::clamp(v, 0.05f, 50.0f);
    } else if (verb == "offset" && sp != std::string::npos && number(rest, &v) && number(rest.substr(sp + 1), &v2)) {
        cfg_.uiOffsetX = std::clamp(v, -20.0f, 20.0f);
        cfg_.uiOffsetY = std::clamp(v2, -20.0f, 20.0f);
    } else if (verb == "follow" && number(rest, &v)) {
        cfg_.uiFollowHead = v != 0.0f;
    } else if (verb == "mirror" && number(rest, &v)) {
        uiMirror_ = v != 0.0f;
    } else {
        return usage;
    }
    log::info("render: UI layer {:.2f} m high at {:.2f} m, offset {:.2f} {:.2f}, {}", cfg_.uiSize, cfg_.uiDistance, cfg_.uiOffsetX,
              cfg_.uiOffsetY, cfg_.uiFollowHead ? "head-locked" : "world-locked");
    return "ok";
}

// ---------------------------------------------------------------------------
// Picture adjustment
// ---------------------------------------------------------------------------
xr::PictureAdjust ClampPicture(const xr::PictureAdjust& p) {
    auto clamp = [](float v, float lo, float hi, float none) { return std::isfinite(v) ? std::clamp(v, lo, hi) : none; };
    xr::PictureAdjust r;
    r.brightness = clamp(p.brightness, -1.0f, 1.0f, 0.0f);
    r.contrast = clamp(p.contrast, 0.5f, 2.0f, 1.0f);
    r.saturation = clamp(p.saturation, 0.0f, 2.0f, 1.0f);
    r.gamma = clamp(p.gamma, 0.5f, 2.0f, 1.0f);
    r.blackLevel = clamp(p.blackLevel, -0.1f, 0.1f, 0.0f);
    return r;
}

std::string PictureText(const xr::PictureAdjust& p) {
    return std::format("brightness {:.3f} contrast {:.3f} saturation {:.3f} gamma {:.3f} black_level {:.4f} ({})", p.brightness, p.contrast,
                       p.saturation, p.gamma, p.blackLevel, p.IsIdentity() ? "no change" : "applied to the eyes and the virtual screen");
}

xr::PictureAdjust XrController::Picture() {
    std::lock_guard lk(pictureMutex_);
    return picture_;
}

void XrController::UiPlacement(float* distance, float* size, bool* followHead) {
    std::lock_guard lk(uiMutex_);
    if (distance) *distance = cfg_.uiDistance;
    if (size) *size = cfg_.uiSize;
    if (followHead) *followHead = cfg_.uiFollowHead;
}

std::string XrController::PictureCommand(const std::string& args) {
    std::istringstream in(args);
    std::string key, value;
    in >> key >> value;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const char* usage = "err usage: picture status | reset | brightness|contrast|saturation|gamma|black_level <value>";
    std::lock_guard lk(pictureMutex_);
    if (key.empty() || key == "status") return "ok picture " + PictureText(picture_);
    if (key == "reset") {
        picture_ = xr::PictureAdjust{};
    } else {
        float v = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
        if (value.empty() || ec != std::errc() || ptr != value.data() + value.size()) return usage;
        xr::PictureAdjust p = picture_;
        if (key == "brightness") p.brightness = v;
        else if (key == "contrast") p.contrast = v;
        else if (key == "saturation") p.saturation = v;
        else if (key == "gamma") p.gamma = v;
        else if (key == "black_level" || key == "blacklevel" || key == "black") p.blackLevel = v;
        else return usage;
        picture_ = ClampPicture(p);
    }
    const std::string text = PictureText(picture_);
    log::info("render: picture {}", text);
    return "ok picture " + text;
}

void XrController::PollPictureKeys() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focus = pid == GetCurrentProcessId();
    const int keys[2] = {cfg_.brightnessUpKey, cfg_.brightnessDownKey};
    for (int i = 0; i < 2; ++i) {
        const int vk = keys[i];
        const bool down = focus && vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !brightnessKeyDown_[i]) {
            std::string text;
            {
                std::lock_guard lk(pictureMutex_);
                xr::PictureAdjust p = picture_;
                p.brightness += i == 0 ? cfg_.brightnessStep : -cfg_.brightnessStep;
                if (std::fabs(p.brightness) < 1e-4f) p.brightness = 0.0f;  // back to exactly none after steps up and down
                picture_ = ClampPicture(p);
                text = PictureText(picture_);
            }
            log::info("render: picture (brightness key): {}", text);
        }
        brightnessKeyDown_[i] = down;
    }
}

// ---------------------------------------------------------------------------
// Dev commands
// ---------------------------------------------------------------------------
std::string XrController::Status() {
    std::lock_guard cl(cmdMutex_);
    const bool ready = ready_.load();
    const std::string d3d = D3D11Summary();
    std::string xrLine = "xr off";
    if (ready) {
        const xr::RuntimeInfo ri = backend_->GetRuntimeInfo();
        const xr::FrameStats st = backend_->GetStats();
        xrLine = std::format("xr {} state {} runtime '{}' {} system '{}' eyes {}x{} {} refresh {:.1f} Hz luid {:016X}; frames waited {} submitted {} skipped {} "
                             "not-rendered {} discarded {}; copies {} blits {} quad updates {} image wait timeouts {}",
                             BackendName(cfg_.backend), xr::ToString(backend_->GetState()), ri.runtimeName, ri.runtimeVersion, ri.systemName,
                             ri.eyeSwapchain[0].width, ri.eyeSwapchain[0].height, xr::DxgiFormatName(ri.eyeSwapchain[0].format),
                             ri.refreshHz, ri.adapterLuid, st.framesWaited, st.framesSubmitted, st.framesSkipped, st.framesNotRendered,
                             st.framesDiscarded, st.copyPath, st.blitPath, st.quadUpdates, st.imageWaitTimeouts);
        xrLine += std::format("; tracking {} (frames held {}, 3DoF {})",
                              !lastOrientationValid_.load() ? "held" : (lastPositionValid_.load() ? "full" : "3dof"), untrackedFrames_.load(),
                              threeDofFrames_.load());
    } else if (cfg_.xrEnabled) {
        xrLine = std::format("xr not running (last result {}, {} failed attempts{})", xr::ToString(lastInitResult_), failedAttempts_,
                             holdAfterExit_ ? ", waiting for xr-restart" : "");
    }
    const std::string counters =
        std::format("mode {}; presents {} submitted screen {} stereo {} held stereo {} screen in stereo mode {} without XR frame {} submit errors {} "
                    "mono fallbacks {}",
                    ModeName(mode_.load()), presents_.load(), submittedScreen_.load(), submittedStereo_.load(), heldStereo_.load(),
                    screenInStereo_.load(), presentsWithoutFrame_.load(), submitErrors_.load(), monoFallbacks_.load());
    log::info("status: d3d11: {}", d3d);
    log::info("status: threads: {}", ThreadReport());
    log::info("status: {}", xrLine);
    log::info("status: {}", counters);
    {
        std::lock_guard lk(pictureMutex_);
        log::info("status: picture {}", PictureText(picture_));
    }
    if (ready) log::info("status: screen layer for a {}x{} {} back buffer", screenW_, screenH_, xr::DxgiFormatName(screenFmt_));
    log::info("status: {}", UiCommand("status").substr(3));
    log::info("status: {}", foveation::Status());
    Timing& t = GetTiming();
    if (ready)
        for (float ms : backend_->TakeGpuCopyTimes()) t.gpuCopy.Add(ms);
    for (Series* s : {&t.frameInterval, &t.presentCall, &t.hook, &t.submit, &t.wait, &t.gpuCopy, &t.gpu}) {
        const std::string line = s->Peek();
        if (!line.empty()) log::info("status: timing since last report: {}", line);
    }
    return "ok " + xrLine + "; " + counters;
}

std::string XrController::Capture(const std::string& prefix, uint32_t timeoutMs) {
    if (prefix.empty()) return "err usage: capture <path prefix>";
    std::lock_guard cl(cmdMutex_);
    if (!ready_.load() || !backend_) return "err xr session not running";
    std::filesystem::path p = std::filesystem::path(log::widen(prefix));
    if (p.is_relative()) p = cfg_.captureDir / p;
    const std::string path = log::narrow(p.wstring());
    backend_->WaitForCaptures(0);  // drop results of earlier requests
    backend_->RequestCapture(xr::CaptureRequest{path});
    log::info("render: capture requested: {}_L.png / _R.png", path);
    const auto results = backend_->WaitForCaptures(timeoutMs);
    if (results.empty()) return std::format("err no frame submitted within {} ms (request stays queued)", timeoutMs);
    const xr::CaptureResult& r = results.back();
    if (!r.ok) return "err " + r.error;
    return std::format("ok frame={} L={} R={}", r.frameId, r.files[0], r.files[1]);
}

std::string XrController::Recenter() {
    std::lock_guard cl(cmdMutex_);
    if (!ready_.load() || !backend_) return "err xr session not running";
    backend_->Recenter();
    return "ok recenter requested (applied with the next frame that has valid tracking)";
}

std::string XrController::SnapTurn(float degrees) {
    if (!ready_.load()) return "err xr session not running";
    float cur = snapPendingDeg_.load(std::memory_order_relaxed);
    while (!snapPendingDeg_.compare_exchange_weak(cur, cur + degrees, std::memory_order_acq_rel)) {
    }
    return std::format("ok snap turn {:+.1f} deg requested (view {:.1f} deg from the recenter before it)", degrees, SnapYawDeg());
}

std::string XrController::SimulateCommand(const std::string& args) {
    std::lock_guard cl(cmdMutex_);
    if (!ready_.load() || !backend_) return "err xr session not running";
    return backend_->Simulate(args);
}

std::string XrController::Restart() {
    restartRequested_ = true;
    return "ok";
}

std::string XrController::SetModeCommand(const std::string& mode) {
    if (mode == "screen") {
        mode_ = Mode::Screen;
    } else if (mode == "stereo" || mode == "stereo-test") {
        if (mode == "stereo-test" && !cfg_.stereoTest) {
            static std::mutex m;
            std::lock_guard lk(m);
            if (!stereoTest_.joinable()) {
                cfg_.stereoTest = true;
                stereoTest_ = std::thread([this] { StereoTestThread(); });
            }
        }
        mode_ = Mode::Stereo;
    } else {
        return "err usage: mode screen|stereo|stereo-test";
    }
    log::info("render: mode {}", ModeName(mode_.load()));
    return std::string("ok mode ") + ModeName(mode_.load());
}

std::string XrController::Stop() {
    stopRequested_ = true;
    return "ok";
}

std::string XrController::SetRuntime(const std::string& runtime) {
    if (runtime.empty()) return "err usage: xr-runtime auto|virtualdesktop|steamvr|system|inherit|<path to runtime json>";
    {
        std::lock_guard lk(pendingMutex_);
        pendingRuntime_ = runtime;
        hasPendingRuntime_ = true;
    }
    restartRequested_ = true;
    return "ok runtime " + runtime + " (session restarts)";
}

std::string XrController::SetFrameWait(const std::string& where) {
    if (where != "thread" && where != "present") return "err usage: frame-wait thread|present";
    waitOnPresent_ = where == "present";
    log::info("render: frame wait on the {} thread", waitOnPresent_.load() ? "present" : "xr");
    return "ok frame-wait " + where;
}

std::string XrController::StereoTestCommand(const std::string& args) {
    const size_t sp = args.find(' ');
    const std::string verb = args.substr(0, sp);
    uint32_t v = 0;
    if (sp != std::string::npos) {
        const char* b = args.data() + sp + 1;
        const auto [ptr, ec] = std::from_chars(b, args.data() + args.size(), v);
        if (ec != std::errc()) return "err usage: stereo-test pause <ms> | drop <n>";
    } else {
        return "err usage: stereo-test pause <ms> | drop <n>";
    }
    if (verb == "pause") {
        testPauseUntilQpc_ = QpcNow() + MsToQpc(std::min(v, 600000u));
        log::info("render: stereo test: the test thread starts no frames for {} ms", v);
        return std::format("ok paused for {} ms", v);
    }
    if (verb == "drop") {
        testDropEvery_ = v;
        log::info("render: stereo test: {}", v ? std::format("every {}. frame has no stereo image", v) : std::string("no dropped images"));
        return std::format("ok drop every {}", v);
    }
    return "err usage: stereo-test pause <ms> | drop <n>";
}

void XrController::StopForExit() {
    if (!started_.load() || stop_.exchange(true)) return;
    queueCv_.notify_all();
    {
        std::lock_guard pl(parkMutex_);
        parkRequested_ = false;
    }
    parkCv_.notify_all();
    // Our threads end within one frame wait; do not wait for ever on a stuck runtime.
    for (std::thread* t : {&thread_, &stereoTest_}) {
        if (!t->joinable()) continue;
        HANDLE h = static_cast<HANDLE>(t->native_handle());
        if (WaitForSingleObject(h, 2000) == WAIT_OBJECT_0)
            t->join();
        else
            t->detach();
    }
    if (ready_.load()) Teardown(TeardownReason::ProcessExit, "the game is exiting");
}

}  // namespace ff7vr::render
