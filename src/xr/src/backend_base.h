// Shared implementation for both backends: device/context, blitter,
// compositor, capture, frame records, quad layer slots, recentering,
// statistics.
#pragma once

#include "capture.h"
#include "compositor.h"
#include "d3d11_blitter.h"
#include "gpu_timer.h"

#include <array>
#include <atomic>
#include <mutex>

namespace ff7vr::xr {

// One swapchain: a runtime swapchain (OpenXR) or an emulated single image (Null).
struct SwapImages {
    uint64_t xr = 0;                       // XrSwapchain handle (OpenXR), 0 when emulated
    std::vector<ID3D11Texture2D*> images;  // runtime-owned images (OpenXR)
    ComPtr<ID3D11Texture2D> owned;         // the emulated image (Null)
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // format the runtime interprets the images as
    bool hasImage = false;                     // an image was released at least once
    uint32_t lastIndex = 0, lastW = 0, lastH = 0;
    View lastView{};  // eye swapchains: raw view the last released image was rendered with
    // OpenXR: an image was acquired but xrWaitSwapchainImage timed out; it
    // stays acquired and the next frame waits for it again instead of acquiring.
    bool waitPending = false;
    uint32_t acquiredIndex = 0;

    ID3D11Texture2D* Image(uint32_t i) const { return owned ? owned.Get() : (i < images.size() ? images[i] : nullptr); }
    LayerImage Last() const { return LayerImage{Image(lastIndex), format, lastW, lastH}; }
};

class BackendBase : public IXrBackend {
public:
    RuntimeInfo GetRuntimeInfo() const override;
    SessionState GetState() const override { return state_.load(std::memory_order_acquire); }
    void Recenter() override { recenterRequest_.store(1, std::memory_order_release); }
    void ResetRecenter() override { recenterRequest_.store(2, std::memory_order_release); }
    void RequestCapture(const CaptureRequest& req) override { capture_.Request(req); }
    std::vector<CaptureResult> WaitForCaptures(uint32_t timeoutMs) override { return capture_.Wait(timeoutMs); }
    FrameStats GetStats() const override;
    std::vector<float> TakeGpuCopyTimes() override { return gpuCopy_.Take(); }
    bool GetQuadLayerInfo(LayerHandle layer, SwapchainInfo* out) const override;
    bool DrawOverlay(const QuadLayer& q, ID3D11Texture2D* target, DXGI_FORMAT targetFormat, ColorEncoding targetEncoding,
                     const Rect& targetRect) override;
    bool GetHiddenAreaMesh(Eye eye, HiddenAreaMesh* out) const override;
    uint32_t HiddenAreaMeshVersion() const override { return hiddenVersion_.load(std::memory_order_acquire); }

protected:
    // Stores an eye's hidden area (empty mesh = none) and bumps the version.
    void SetHiddenAreaMesh(Eye eye, HiddenAreaMesh mesh);

    struct FrameRecord {
        uint64_t id = 0;
        uint64_t epoch = 0;
        int64_t displayTime = 0;
        int64_t period = 0;
        bool shouldRender = false;
        bool begun = false;
        bool ended = false;
        bool orientationValid = false, positionValid = false;
        View raw[2]{};     // views in the runtime's LOCAL space (what goes into the layer)
        Pose rawHead{};    // head pose in LOCAL space
        Pose recenter{};   // recenter transform in effect for this frame
    };
    static constexpr size_t kRing = 8;

    struct QuadSlot {
        bool used = false;
        uint32_t serial = 0;
        SwapImages sc;
    };

    Result InitCommon(const InitDesc& desc);
    void ShutdownCommon();

    // GT: allocate a record for a new frame (id assigned here).
    FrameRecord& NewFrameLocked();
    // Any: find a record by id; nullptr if unknown/overwritten. Caller holds frameMutex_.
    FrameRecord* FindFrameLocked(uint64_t id);

    // GT: consume a pending recenter request using the raw head pose; returns the active recenter pose.
    Pose UpdateRecenter(const Pose& rawHead, bool headValid);
    static View ApplyRecenter(const Pose& recenter, const View& raw);
    static Pose ApplyRecenter(const Pose& recenter, const Pose& raw);
    static View RemoveRecenter(const Pose& recenter, const View& v);
    void FillFrameInfo(const FrameRecord& r, FrameInfo& info) const;

    struct EyeTarget {
        ID3D11Texture2D* texture = nullptr;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0, height = 0;
        uint32_t arraySlice = 0;
    };
    // RT, inside a saved-state scope: copy one eye of `desc` / one quad's new content into `target`.
    bool TransferEye(Eye eye, const SubmitDesc& desc, const EyeTarget& target, uint32_t* outW, uint32_t* outH);
    bool TransferQuad(const QuadLayer& q, const EyeTarget& target, uint32_t* outW, uint32_t* outH);
    static Rect EyeRect(const SubmitDesc& desc, Eye eye);
    static Rect QuadRect(const QuadLayer& q);
    // Raw (pre-recenter) view an eye image was rendered with.
    static View SubmittedView(const FrameRecord& r, const SubmitDesc& desc, int eye);
    // Raw (LOCAL space) pose of a quad in this frame.
    static Pose QuadPoseLocal(const FrameRecord& r, const QuadLayer& q);
    // Validates a SubmitDesc (source regions, quad handles). Logs and returns false on error.
    bool ValidateSubmit(const SubmitDesc& desc);
    void CountStat(uint64_t FrameStats::* field);
    void AddMs(double FrameStats::* field, int64_t sinceNs) {
        const double ms = double(QpcNowNs() - sinceNs) / 1e6;
        std::lock_guard lk(statsMutex_);
        stats_.*field += ms;
    }

    // RT, inside a saved-state scope, between capture_.BeginFrame and EndFrame:
    // if a capture is active, composites each eye (projection image, may be
    // null, then the frame's quads) at eyeW x eyeH and hands it to the capture.
    void CaptureComposited(const FrameRecord& rec, const SubmitDesc& desc, const LayerImage projection[2], uint32_t eyeW,
                           uint32_t eyeH);

    // Quad slots. Mutated on the RT under quadMutex_; read on the RT without it.
    QuadSlot* FindQuad(LayerHandle h);
    const QuadSlot* FindQuad(LayerHandle h) const;
    // Returns a free slot index or -1 (caller holds quadMutex_).
    int FreeQuadSlotLocked() const;
    LayerHandle MakeHandle(int index) const;

    Logger log_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    Blitter blitter_;
    Compositor compositor_;
    CaptureManager capture_;
    D3D11StateBackup stateBackup_;  // RT only
    GpuCopyTimer gpuCopy_;          // RT only (Take: any thread)
    bool gpuTiming_ = false;
    // RT: bracket the copies of one frame (no-ops without InitDesc::gpuTiming).
    void GpuFrameBegin() {
        if (gpuTiming_) gpuCopy_.BeginFrame(device_.Get(), context_.Get());
    }
    void GpuFrameEnd() {
        if (gpuTiming_) gpuCopy_.EndFrame(context_.Get());
    }

    mutable std::mutex infoMutex_;
    RuntimeInfo info_;
    HiddenAreaMesh hidden_[2];  // under infoMutex_
    std::atomic<uint32_t> hiddenVersion_{0};
    std::atomic<SessionState> state_{SessionState::Uninitialized};

    std::mutex frameMutex_;
    std::array<FrameRecord, kRing> ring_{};
    uint64_t nextFrameId_ = 1;
    uint64_t epoch_ = 1;

    mutable std::mutex quadMutex_;
    std::array<QuadSlot, kMaxQuadLayers> quads_{};
    uint32_t layerSerial_ = 0;  // bumped on every Init so handles from an earlier session are stale

    std::atomic<int> recenterRequest_{0};
    Pose recenter_{};  // GT only

    mutable std::mutex statsMutex_;
    FrameStats stats_;
    bool initialized_ = false;

private:
    ComPtr<ID3D11Texture2D> composeTarget_[2];  // R8G8B8A8, viewed as UNORM_SRGB
    uint32_t composeW_[2]{}, composeH_[2]{};
};

std::unique_ptr<IXrBackend> CreateNullBackend();
std::unique_ptr<IXrBackend> CreateOpenXrBackend();

// Scoped D3D11 state save/restore on the render thread.
class ScopedStateBackup {
public:
    ScopedStateBackup(D3D11StateBackup& b, ID3D11DeviceContext* ctx) : b_(b), ctx_(ctx) { b_.Save(ctx_); }
    ~ScopedStateBackup() { b_.Restore(ctx_); }
    ScopedStateBackup(const ScopedStateBackup&) = delete;
    ScopedStateBackup& operator=(const ScopedStateBackup&) = delete;

private:
    D3D11StateBackup& b_;
    ID3D11DeviceContext* ctx_;
};

// Picks a layer swapchain format: the sRGB variant of `hint` (or `hint`
// itself) if offered, otherwise the first offered entry of `preferred`.
DXGI_FORMAT PickLayerFormat(DXGI_FORMAT hint, const std::vector<DXGI_FORMAT>& offered, const DXGI_FORMAT* preferred, size_t count);

}  // namespace ff7vr::xr
