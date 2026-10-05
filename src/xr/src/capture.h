// Per-eye PNG capture. Reads back the eye images on the render thread and
// encodes PNGs on a worker thread.
#pragma once

#include "d3d11_blitter.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace ff7vr::xr {

class CaptureManager {
public:
    bool Init(ID3D11Device* device, const Logger* log, Blitter* blitter);
    void Shutdown();

    // Any thread.
    void Request(const CaptureRequest& req);
    bool HasPending() const { return pendingCount_.load(std::memory_order_acquire) > 0; }
    std::vector<CaptureResult> Wait(uint32_t timeoutMs);

    // Render thread, inside a saved D3D11 state scope. Start a capture for this
    // frame if one is pending; then CaptureEye for each eye; then EndFrame.
    bool BeginFrame(uint64_t frameId);
    bool Active() const { return active_; }  // render thread: a capture was started for this frame
    // Reads `tex` (interpreted as viewFormat, linear-light semantics of that format) region (0,0,w,h).
    void CaptureEye(ID3D11DeviceContext* ctx, Eye eye, ID3D11Texture2D* tex, DXGI_FORMAT viewFormat, uint32_t w, uint32_t h);
    void EndFrame();

private:
    struct Image {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgb;  // tightly packed RGB8 (sRGB encoded)
        std::string error;
    };
    struct Job {
        uint64_t frameId = 0;
        std::string prefix;
        Image eyes[2];
    };

    void WorkerMain();
    static bool WritePng(const std::string& pathUtf8, const Image& img, std::string* err);

    const Logger* log_ = nullptr;
    Blitter* blitter_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Texture2D> rt_[2];       // R8G8B8A8_TYPELESS, viewed as UNORM_SRGB
    ComPtr<ID3D11Texture2D> staging_[2];  // R8G8B8A8_UNORM_SRGB staging
    uint32_t rtW_[2]{}, rtH_[2]{};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<CaptureRequest> requests_;
    std::atomic<int> pendingCount_{0};  // requested but not finished
    std::deque<Job> jobs_;
    std::vector<CaptureResult> results_;
    bool stop_ = false;
    std::thread worker_;

    // Current frame capture (render thread only).
    bool active_ = false;
    Job current_;
};

}  // namespace ff7vr::xr
