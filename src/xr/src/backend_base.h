// Shared implementation for both backends: device/context, blitter, capture,
// frame records, recentering, statistics.
#pragma once

#include "capture.h"
#include "d3d11_blitter.h"

#include <array>
#include <atomic>
#include <mutex>

namespace ff7vr::xr {

class BackendBase : public IXrBackend {
public:
    RuntimeInfo GetRuntimeInfo() const override;
    SessionState GetState() const override { return state_.load(std::memory_order_acquire); }
    void Recenter() override { recenterRequest_.store(1, std::memory_order_release); }
    void ResetRecenter() override { recenterRequest_.store(2, std::memory_order_release); }
    void RequestCapture(const CaptureRequest& req) override { capture_.Request(req); }
    std::vector<CaptureResult> WaitForCaptures(uint32_t timeoutMs) override { return capture_.Wait(timeoutMs); }
    FrameStats GetStats() const override;

protected:
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
        Pose recenter{};   // recenter transform in effect for this frame
    };
    static constexpr size_t kRing = 8;

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
    void FillFrameInfo(const FrameRecord& r, const Pose& rawHead, FrameInfo& info) const;

    // RT, inside a saved-state scope: move one eye of `desc` into an eye image and capture it if requested.
    struct EyeTarget {
        ID3D11Texture2D* texture = nullptr;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0, height = 0;
        uint32_t arraySlice = 0;
    };
    bool TransferEye(Eye eye, const SubmitDesc& desc, const EyeTarget& target, uint32_t* outW, uint32_t* outH);
    static Rect EyeRect(const SubmitDesc& desc, Eye eye);
    // Raw (pre-recenter) view an eye image was rendered with.
    static View SubmittedView(const FrameRecord& r, const SubmitDesc& desc, int eye);
    // Validates a SubmitDesc against the source texture. Logs and returns false on error.
    bool ValidateSubmit(const SubmitDesc& desc);
    void CountStat(uint64_t FrameStats::* field);

    Logger log_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    Blitter blitter_;
    CaptureManager capture_;
    D3D11StateBackup stateBackup_;  // RT only

    mutable std::mutex infoMutex_;
    RuntimeInfo info_;
    std::atomic<SessionState> state_{SessionState::Uninitialized};

    std::mutex frameMutex_;
    std::array<FrameRecord, kRing> ring_{};
    uint64_t nextFrameId_ = 1;
    uint64_t epoch_ = 1;

    std::atomic<int> recenterRequest_{0};
    Pose recenter_{};  // GT only
    Pose lastRawHead_{};

    mutable std::mutex statsMutex_;
    FrameStats stats_;
    bool initialized_ = false;
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

}  // namespace ff7vr::xr
