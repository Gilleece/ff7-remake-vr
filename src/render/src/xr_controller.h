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
#include <vector>

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
    std::string runtime = "auto";
    float resolutionScale = 1.0f;
    uint32_t eyeWidth = 0, eyeHeight = 0;
    bool disableImplicitLayers = false;              // all of them
    std::vector<std::string> disableLayersMatching;  // or those whose name contains one of these
    bool debugUtils = false;
    double retryIntervalS = 5.0;
    bool reconnectAfterExit = false;
    bool waitOnPresentThread = false;  // [xr] frame_wait = present
    float nullRefreshHz = 90.0f;
    bool nullPace = true;
    xr::NullMotion nullMotion = xr::NullMotion::Static;
    // [screen]
    // 1.8 m at 2 m: about 48 x 28 degrees for 16:9, the whole image (HUD corners
    // included) within a small eye movement, at about the headset's focal distance.
    float screenDistance = 2.0f;
    float screenWidth = 1.8f;
    float screenOffsetY = 0.0f;
    bool screenFollowHead = false;
    bool recenterOnStart = true;
    // [ui] The in-game UI's own layer in stereo (docs/render.md, "UI layer"). Defaults
    // follow the community UEVR profile for this game (UI_Distance 3.0, UI_Size 2.0,
    // UI_FollowView false): a 16:9 UI 3.56 x 2 m at 3 m covers about 61 x 37 degrees.
    bool uiLayer = true;
    float uiDistance = 3.0f;
    float uiSize = 2.0f;  // height in metres; the width follows the UI's aspect ratio
    float uiOffsetX = 0.0f, uiOffsetY = 0.0f;
    bool uiFollowHead = false;
    uint32_t uiLayerWidth = 1920;  // width of the layer image; 0 = the game's UI texture width
    bool uiMirror = true;          // draw the UI over the desktop window too (it is not in the eye images)
    // [picture] Colour adjustment of the eye images and the virtual screen (not the UI
    // layer); defaults change nothing. See xr::PictureAdjust and docs/render.md.
    xr::PictureAdjust picture{};
    // [controls] brightness_up_key / brightness_down_key (virtual-key codes, 0 = none) and
    // brightness_step: change [picture] brightness while the game window has the focus.
    int brightnessUpKey = 0, brightnessDownKey = 0;
    float brightnessStep = 0.05f;
};

// Limits of the [picture] keys (also applied to the `picture` command).
xr::PictureAdjust ClampPicture(const xr::PictureAdjust& p);
std::string PictureText(const xr::PictureAdjust& p);

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
    // `vram` dev command: the process's video memory usage and budget now.
    std::string VideoMemoryStatus();
    std::string Capture(const std::string& prefix, uint32_t timeoutMs);
    std::string Recenter();
    // Snap turn: turns the views by `degrees` (positive = right) on top of the recenter, from
    // the next frame. Any thread, lock-free (the pad's poll thread calls it).
    std::string SnapTurn(float degrees);
    // Any thread, lock-free: the snap turn in effect for the latest frame (degrees, positive = right).
    float SnapYawDeg() const { return snapYawDeg_.load(std::memory_order_relaxed); }
    std::string Restart();
    std::string SetModeCommand(const std::string& mode);
    std::string StereoTestCommand(const std::string& args);
    std::string Stop();
    std::string SetRuntime(const std::string& runtime);
    std::string SetFrameWait(const std::string& where);
    // xr-sim: the Null backend's headset simulation (head pose, runtime recenter, tracking loss).
    std::string SimulateCommand(const std::string& args);

    // Engine interface (see render.h).
    void SetMode(Mode m) { mode_.store(m); }
    Mode GetMode() const { return mode_.load(); }
    bool GetEyeSetup(EyeSetup* out);
    StereoFrame BeginGameFrame();
    void SubmitStereoFrame(const StereoSubmit& s);
    bool UiLayerWanted() const;
    // Any thread. The runtime's hidden area of an eye (cached from the backend; false: none).
    // `version` changes whenever the mesh does; `out` may be null.
    bool GetHiddenArea(int eye, xr::HiddenAreaMesh* out, uint32_t* version);
    // Any thread. The newest eye gaze (see EYE GAZE in xr.h), from the last waited frame.
    struct GazeState {
        bool available = false;  // a gaze source exists in the running session
        bool tracked = false;
        bool nominal = false;
        xr::Vec3 headDirection{0.0f, 0.0f, -1.0f};  // VIEW space
        xr::Quat eyeFromHead[2]{};  // turns a VIEW space direction into each eye's space
        double ageMs = 0;           // display time minus sample time (how much older the sample is than the frame's display); -1 = unknown
        uint64_t frameId = 0;
        uint64_t samples = 0;       // frames sampled in this session
        std::string source;         // RuntimeInfo::gazeSource
        std::string note;           // RuntimeInfo::gazeNote
    };
    bool GetGaze(GazeState* out);
    bool UiDumpRequested() const { return uiDumpRequested_.load(); }
    void SubmitUiLayer(const UiLayerSource& s);
    std::string UiCommand(const std::string& args);
    // `picture status | reset | <key> <value>` (any thread).
    std::string PictureCommand(const std::string& args);
    // Any thread: the picture adjustment and the UI panel's placement as set now.
    xr::PictureAdjust Picture();
    void UiPlacement(float* distance, float* size, bool* followHead);

private:
    void PollPictureKeys();  // RT, once per Present
    // UI texture reported for the coming Present (presenting thread).
    struct PendingUi {
        UiLayerSource src;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    };
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
    bool EnsureUiLayer(const UiLayerSource& s);
    // The settings panel's quad while it is open (false: closed or no layer).
    bool MenuQuad(const PresentInfo& p, const Waited& w, xr::QuadLayer* q);
    void SubmitOne(const PresentInfo& p, const Waited& w, const PendingUi* ui);
    void DrawUiOnWindow(const PresentInfo& p, const PendingUi& ui);
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
    std::atomic<float> snapYawDeg_{0.0f};
    std::atomic<float> snapPendingDeg_{0.0f};  // requested, handed to the backend before the next WaitFrame
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
    std::atomic<bool> stopRequested_{false};    // xr-stop: end the session and stay off until xr-restart
    std::atomic<bool> waitOnPresent_{false};    // [xr] frame_wait = present (switchable with frame-wait)
    std::mutex pendingMutex_;
    std::string pendingRuntime_;                // xr-runtime: applied by the XR thread before the next attempt
    bool hasPendingRuntime_ = false;

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

    // Automatic fallback in stereo mode (see docs/render.md, "Switching").
    // The game thread paces while it calls BeginGameFrame; after kGameIdleMs
    // without a call the XR thread paces instead, so the runtime keeps getting
    // frames (screen layer) while the game thread is blocked.
    static constexpr double kGameIdleMs = 100.0;
    // A frame without a stereo image re-shows the last stereo image if that is
    // younger than this (hitches), otherwise it shows the screen layer.
    static constexpr double kStereoHoldMs = 300.0;
    std::atomic<int64_t> lastGameFrameQpc_{0};  // last BeginGameFrame call in stereo mode
    bool xrPacesStereo_ = false;                // XR thread: it currently paces in stereo mode (for logging)
    int64_t lastStereoQpc_ = 0;                 // RT: last frame ended with a new stereo image (0 = none this session)
    bool GameThreadPaces() const;
    // Stereo test controls (dev commands).
    std::atomic<int64_t> testPauseUntilQpc_{0};
    std::atomic<uint32_t> testDropEvery_{0};
    uint64_t testFrames_ = 0;  // RT

    // Screen layer (RT).
    xr::LayerHandle screenLayer_ = 0;
    uint32_t screenW_ = 0, screenH_ = 0;
    DXGI_FORMAT screenFmt_ = DXGI_FORMAT_UNKNOWN;

    // UI layer. Placement may change at run time (`ui` command): read under uiMutex_.
    std::mutex uiMutex_;
    std::atomic<bool> uiOn_{true};
    std::atomic<bool> uiMirror_{true};
    std::atomic<bool> uiDumpRequested_{false};
    std::string uiDumpPath_;      // under uiMutex_
    std::string uiDumpResult_;    // under uiMutex_
    std::condition_variable uiDumpCv_;
    bool havePendingUi_ = false;  // presenting thread, under uiMutex_
    PendingUi pendingUi_;         // presenting thread, under uiMutex_
    xr::LayerHandle uiLayer_ = 0;  // RT
    uint32_t uiW_ = 0, uiH_ = 0, uiSrcW_ = 0, uiSrcH_ = 0;
    DXGI_FORMAT uiFmt_ = DXGI_FORMAT_UNKNOWN;
    bool uiShownLast_ = false;  // RT: the last stereo frame showed the UI quad
    std::atomic<uint64_t> uiSubmitted_{0}, uiHeld_{0}, uiDropped_{0};
    std::string uiLastSource_;  // RT, for status

    // Settings panel layer (RT): created at the first open, placed again when the panel opens.
    xr::LayerHandle menuLayer_ = 0;
    uint64_t menuPlacement_ = 0;
    xr::Pose menuPose_{};
    bool menuHeadLocked_ = false;

    // Picture adjustment, changeable at run time (`picture` command, brightness keys).
    std::mutex pictureMutex_;
    xr::PictureAdjust picture_{};  // under pictureMutex_
    bool brightnessKeyDown_[2]{};  // RT

    // Tracking of the last waited frame (the backend's pose filter, see xr.h POSE VALIDITY).
    std::atomic<bool> lastOrientationValid_{true}, lastPositionValid_{true};
    std::atomic<uint64_t> untrackedFrames_{0}, threeDofFrames_{0};
    // Game thread: the last views handed to the engine (a last guard against unusable values).
    xr::View lastGameViews_[2]{};
    xr::Pose lastGameHead_{};
    bool haveGameViews_ = false;
    uint64_t rejectedGameFrames_ = 0;

    // Eye setup snapshot for the engine.
    std::mutex eyeMutex_;
    EyeSetup eye_{};
    bool eyeValid_ = false;
    xr::HiddenAreaMesh hidden_[2];       // under eyeMutex_
    uint32_t hiddenBackendVersion_ = ~0u;  // waiting threads, under waitMutex_
    std::atomic<uint32_t> hiddenVersion_{0};
    GazeState gaze_{};  // under eyeMutex_

    // Counters.
    std::atomic<uint64_t> presents_{0}, submittedScreen_{0}, submittedStereo_{0}, presentsWithoutFrame_{0}, submitErrors_{0};
    std::atomic<uint64_t> monoFallbacks_{0};
    std::atomic<uint64_t> heldStereo_{0};      // frames that re-showed the last stereo image
    std::atomic<uint64_t> screenInStereo_{0};  // frames ended with the screen layer while in stereo mode
    GpuTimer gpu_;
    int64_t lastStatsQpc_ = 0;
    xr::FrameStats lastFrameStats_{};  // backend counters at the previous report (XR thread)
    std::string lastPresentInfo_;
};

}  // namespace ff7vr::render
