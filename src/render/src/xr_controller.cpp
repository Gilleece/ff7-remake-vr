#include "xr_controller.h"

#include "ff7vr/core/log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>

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
    if (cfg_.stereoTest) mode_ = Mode::Stereo;
    lastStatsQpc_ = QpcNow();
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
                       : level == xr::LogLevel::Info ? log::Level::Info
                                                     : log::Level::Debug;
        if (quiet) l = log::Level::Debug;
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
    d.null.refreshHz = cfg_.nullRefreshHz;
    d.null.paceToRefresh = cfg_.nullPace;
    d.null.motion = cfg_.nullMotion;

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
    failedAttempts_ = 0;
    lastInitResult_ = xr::Result::Ok;
    {
        std::lock_guard lk(rtMutex_);
        backend_ = std::move(be);
        screenLayer_ = 0;
        screenW_ = screenH_ = 0;
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
        eyeValid_ = true;
    }
    sessionStarted_ = false;
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
    {
        std::lock_guard lk(eyeMutex_);
        eye_.fov[0] = out->views[0].fov;
        eye_.fov[1] = out->views[1].fov;
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
        if (backend_) {
            backend_->Shutdown();
            backend_.reset();
        }
        screenLayer_ = 0;
        screenW_ = screenH_ = 0;
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
    log::info("timing: last {:.1f} s, {:.1f} fps; xr {} ({}); mode {}; presents {} submitted screen {} stereo {} held {} without XR frame {} errors {}",
              seconds, seconds > 0 ? double(frames) / seconds : 0.0, ready_.load() ? BackendName(cfg_.backend) : "off", xr::ToString(st),
              ModeName(mode_.load()), presents_.load(), submittedScreen_.load(), submittedStereo_.load(), heldStereo_.load(),
              presentsWithoutFrame_.load(), submitErrors_.load());
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
    if (!runtimeCalls.empty()) log::info("timing:   {}", runtimeCalls);
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

void XrController::SubmitOne(const PresentInfo& p, const Waited& w) {
    xr::SubmitDesc d;
    xr::QuadLayer q;
    StereoSubmit s;
    ComPtr<ID3D11Texture2D> stereoTex;
    bool stereo = false;
    {
        std::lock_guard lk(stereoMutex_);
        for (const PendingStereo& ps : stereoQueue_)
            if (ps.submit.frameId == w.info.frameId && ps.texture) {
                stereo = true;
                s = ps.submit;
                stereoTex = ps.texture;
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
    } else if (hold) {
        // Both eyes keep their last image (nothing is read from the texture,
        // which only has to pass validation).
        d.texture = p.backBuffer;
        d.viewFormat = TypedFormat(p.format);
        d.eyes[0].update = d.eyes[1].update = false;
    } else {
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
        d.quads = &q;
        d.quadCount = 1;
    }
    if (cfg_.gpuTiming) gpu_.Begin(p.device, p.context);
    const int64_t t0 = QpcNow();
    const xr::Result r = backend_->SubmitFrame(w.info.frameId, d);
    GetTiming().submit.Add(QpcToMs(QpcNow() - t0));
    if (cfg_.gpuTiming) gpu_.End(p.context);
    if (r == xr::Result::Ok) {
        if (stereo) {
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
            return;
        }
        w = queue_.front();
        queue_.pop_front();
    }
    queueCv_.notify_all();
    SubmitOne(p, w);
}

// ---------------------------------------------------------------------------
// Engine interface
// ---------------------------------------------------------------------------
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
    {
        // The XR thread may have waited a frame while it still paced (hand-over): at most two ahead.
        std::lock_guard lk(queueMutex_);
        if (queue_.size() >= 2) return f;
    }
    xr::FrameInfo fi;
    if (!WaitOne(true, &fi)) return f;
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
    ps.texture = s.texture;  // keeps it alive until the Present that submits it
    // A newer image for the same frame replaces the older one.
    auto same = std::find_if(stereoQueue_.begin(), stereoQueue_.end(), [&](const PendingStereo& e) { return e.submit.frameId == s.frameId; });
    if (same != stereoQueue_.end()) {
        *same = std::move(ps);
        return;
    }
    stereoQueue_.push_back(std::move(ps));
    while (stereoQueue_.size() > 4) stereoQueue_.pop_front();
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
    if (ready) log::info("status: screen layer for a {}x{} {} back buffer", screenW_, screenH_, xr::DxgiFormatName(screenFmt_));
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
    if (runtime.empty()) return "err usage: xr-runtime virtualdesktop|steamvr|system|inherit|<path to runtime json>";
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
