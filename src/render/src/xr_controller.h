// Runs the XR session inside the game: creates the backend when a device is
// known, retries when no headset is available, paces frames, submits the
// screen layer (or the engine's stereo image) from the Present hook, and
// tears the session down when the runtime loses it or asks to exit.
//
// Threads:
//   * "ff7vr xr" thread (ours): backend Init/Shutdown, frame pacing
//     (WaitFrame) in screen mode, statistics.
//   * RT (the thread that presents): BeginFrame/SubmitFrame, quad layer
//     creation, every use of the immediate context.
//   * GT (engine, stereo mode only): WaitFrame through BeginGameFrame.
//   * dev pipe thread: status, capture, recenter.
// WaitFrame callers are serialised by waitMutex_. Shutdown needs the
// immediate context, so the XR thread first parks the RT inside its Present
// hook (park protocol), then shuts down, then releases it.
#pragma once

#include "d3d11_hooks.h"
#include "timing.h"

#include "ff7vr/render/render.h"
#include "ff7vr/xr/xr.h"

#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ff7vr::render {

struct RenderConfig {
    // [render]
    double statsIntervalS = 10.0;
    bool gpuTiming = true;
    std::filesystem::path captureDir;
    bool stereoTest = false;  // [render] mode = stereo-test
    // [xr]
    bool xrEnabled = true;
    xr::BackendType backend = xr::BackendType::OpenXR;
    std::string runtime = "virtualdesktop";
    float resolutionScale = 1.0f;
    uint32_t eyeWidth = 0, eyeHeight = 0;
    bool disableImplicitLayers = false;
    bool debugUtils = false;
    double retryIntervalS = 5.0;
    bool reconnectAfterExit = false;
    bool waitOnPresentThread = false;  // [xr] frame_wait = present
    float nullRefreshHz = 90.0f;
    bool nullPace = true;
    xr::NullMotion nullMotion = xr::NullMotion::Static;
    // [screen]
    float screenDistance = 2.5f;
    float screenWidth = 2.4f;
    float screenOffsetY = 0.0f;
    bool screenFollowHead = false;
    bool recenterOnStart = true;
};

class XrController {
public:
    static XrController& Get();

    void Start(const RenderConfig& cfg);
    // Process exit: stops the XR thread and shuts the session down (bounded time).
    void StopForExit();

    // RT callbacks from the D3D11 hooks.
    void OnPresent(const PresentInfo& p);
    void OnResize(IDXGISwapChain* sc);

    // Dev commands (any thread).
    std::string Status();
    std::string Capture(const std::string& prefix, uint32_t timeoutMs);
    std::string Recenter();
    std::string Restart();
    std::string SetModeCommand(const std::string& mode);

    // Engine interface (see render.h).
    void SetMode(Mode m) { mode_.store(m); }
    Mode GetMode() const { return mode_.load(); }
    bool GetEyeSetup(EyeSetup* out);
    StereoFrame BeginGameFrame();
    void SubmitStereoFrame(const StereoSubmit& s);

private:
    struct Waited {
        xr::FrameInfo info;
        bool fromGameThread = false;
        int64_t waitedQpc = 0;
    };
    enum class TeardownReason { Lost, ExitRequested, DeviceChanged, Restart, ProcessExit };

    void ThreadMain();
    void TryInit();
    void Teardown(TeardownReason why, const char* text);
    void PaceOnce();
    bool WaitOne(bool fromGameThread, xr::FrameInfo* out);  // caller holds waitMutex_
    void LogStats();
    void StereoTestThread();

    // RT helpers (caller holds rtMutex_).
    bool EnsureScreenLayer(const PresentInfo& p);
    void SubmitOne(const PresentInfo& p, const Waited& w);
    void ParkPoint();

    RenderConfig cfg_;
    std::thread thread_;
    std::thread stereoTest_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};

    // Device seen by the Present hook.
    std::mutex devMutex_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;      // the device the session runs on
    Microsoft::WRL::ComPtr<ID3D11Device> seenDevice_;  // the device of the main swap chain
    std::atomic<bool> deviceChanged_{false};

    // Backend. Published under rtMutex_; frame calls only while ready_.
    std::unique_ptr<xr::IXrBackend> backend_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stopping_{false};
    std::mutex rtMutex_;    // RT XR work vs. publish/teardown
    std::mutex waitMutex_;  // one WaitFrame at a time
    std::mutex cmdMutex_;   // dev commands vs. teardown
    int64_t nextAttemptQpc_ = 0;
    uint32_t failedAttempts_ = 0;
    xr::Result lastInitResult_ = xr::Result::Ok;
    bool holdAfterExit_ = false;
    bool sessionStarted_ = false;  // reached a running frame since Init (for recenter on start)
    std::atomic<bool> restartRequested_{false};

    // Park protocol.
    std::mutex parkMutex_;
    std::condition_variable parkCv_;
    bool parkRequested_ = false;
    bool parked_ = false;

    // Waited frames not yet ended (oldest first).
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Waited> queue_;

    // Stereo images recorded by SubmitStereoFrame, consumed by the Present hook. A queue:
    // the engine's render thread may record frame N+1 before the thread that presents
    // (the RHI thread in this game) has presented frame N.
    struct PendingStereo {
        StereoSubmit submit;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    };
    std::mutex stereoMutex_;
    std::deque<PendingStereo> stereoQueue_;
    std::atomic<Mode> mode_{Mode::Screen};

    // Screen layer (RT).
    xr::LayerHandle screenLayer_ = 0;
    uint32_t screenW_ = 0, screenH_ = 0;
    DXGI_FORMAT screenFmt_ = DXGI_FORMAT_UNKNOWN;

    // Eye setup snapshot for the engine.
    std::mutex eyeMutex_;
    EyeSetup eye_{};
    bool eyeValid_ = false;

    // Counters.
    std::atomic<uint64_t> presents_{0}, submittedScreen_{0}, submittedStereo_{0}, presentsWithoutFrame_{0}, submitErrors_{0};
    std::atomic<uint64_t> monoFallbacks_{0};
    GpuTimer gpu_;
    int64_t lastStatsQpc_ = 0;
    xr::FrameStats lastFrameStats_{};  // backend counters at the previous report (XR thread)
    std::string lastPresentInfo_;
};

}  // namespace ff7vr::render
