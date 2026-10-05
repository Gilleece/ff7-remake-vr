// Null backend: no runtime. Emulates a Quest 3 class headset with a fixed
// asymmetric FOV, a fixed IPD and a scripted, deterministic head motion. Eye
// and quad layer images are copied into emulated swapchain textures exactly
// like the OpenXR backend does; captures composite them the way a runtime
// would (see QUAD LAYERS in xr.h), so PNGs show what the user would see.
#include "backend_base.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace ff7vr::xr {
namespace {

constexpr float kTwoPi = 6.283185307179586f;

const char* MotionName(NullMotion m) {
    switch (m) {
        case NullMotion::Static: return "static";
        case NullMotion::YawSweep: return "yaw sweep";
        case NullMotion::Sway: return "sway";
        case NullMotion::YawAndSway: return "yaw sweep + sway";
    }
    return "?";
}

// Sleeps with sub-millisecond accuracy (high resolution waitable timer, then a
// short spin), so the emulated vsync does not inherit the 15.6 ms system tick.
void SleepPrecise(int64_t ns) {
    const int64_t end = QpcNowNs() + ns;
    if (ns > 1500000) {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (t) {
            LARGE_INTEGER due;
            due.QuadPart = -((ns - 1000000) / 100);  // relative, 100 ns units; wake 1 ms early
            if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
        } else {
            std::this_thread::sleep_for(std::chrono::nanoseconds(ns - 2000000));
        }
    }
    while (QpcNowNs() < end) std::this_thread::yield();
}

class NullBackend final : public BackendBase {
public:
    ~NullBackend() override { Shutdown(); }

    BackendType Type() const override { return BackendType::Null; }

    Result Init(const InitDesc& desc) override {
        if (initialized_) Shutdown();
        if (const Result r = InitCommon(desc); r != Result::Ok) {
            ShutdownCommon();
            return r;
        }
        opt_ = desc.null;
        if (opt_.eyeWidth == 0 || opt_.eyeHeight == 0 || opt_.eyeWidth > 16384 || opt_.eyeHeight > 16384 || !(opt_.refreshHz > 1.0f)) {
            log_.Error("null backend: invalid options ({}x{} @ {} Hz)", opt_.eyeWidth, opt_.eyeHeight, opt_.refreshHz);
            ShutdownCommon();
            return Result::InvalidArgument;
        }
        if (desc.eyeWidth && desc.eyeHeight) {
            opt_.eyeWidth = desc.eyeWidth;
            opt_.eyeHeight = desc.eyeHeight;
        } else if (desc.resolutionScale > 0.0f && desc.resolutionScale != 1.0f) {
            opt_.eyeWidth = std::max(16u, static_cast<uint32_t>(std::lround(opt_.eyeWidth * desc.resolutionScale)));
            opt_.eyeHeight = std::max(16u, static_cast<uint32_t>(std::lround(opt_.eyeHeight * desc.resolutionScale)));
        }
        DXGI_FORMAT fmt = desc.swapchainFormat != DXGI_FORMAT_UNKNOWN ? desc.swapchainFormat : opt_.swapchainFormat;
        if (IsTypelessFormat(fmt)) fmt = DefaultTypedFormat(fmt);
        for (int e = 0; e < 2; ++e) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = opt_.eyeWidth;
            d.Height = opt_.eyeHeight;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = TypelessFamily(fmt);  // like runtime swapchain images: typeless where possible
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            const HRESULT hr = device_->CreateTexture2D(&d, nullptr, eyes_[e].texture.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                log_.Error("null backend: eye texture {}x{} {} failed {}", d.Width, d.Height, DxgiFormatName(fmt), HResultString(hr));
                ShutdownCommon();
                return Result::Error;
            }
            eyes_[e].hasImage = false;
        }
        format_ = fmt;

        {
            std::lock_guard lk(infoMutex_);
            info_ = RuntimeInfo{};
            info_.backend = BackendType::Null;
            info_.runtimeName = "Null";
            info_.runtimeVersion = "1.0.0";
            info_.systemName = "Null HMD (Quest 3 class)";
            info_.maxSwapchainWidth = info_.maxSwapchainHeight = 16384;
            for (int e = 0; e < 2; ++e) {
                info_.recommendedWidth[e] = opt_.eyeWidth;
                info_.recommendedHeight[e] = opt_.eyeHeight;
                info_.maxWidth[e] = info_.maxHeight[e] = 16384;
                info_.eyeSwapchain[e] = SwapchainInfo{opt_.eyeWidth, opt_.eyeHeight, fmt, 1};
            }
            info_.refreshHz = opt_.refreshHz;
            info_.availableRefreshHz = {opt_.refreshHz};
            info_.runtimeFormats = {fmt};
            info_.orientationTracking = info_.positionTracking = true;
            info_.lastPredictedDisplayPeriod = Period();
        }
        nextDeadline_ = 0;
        initialized_ = true;
        state_ = SessionState::Focused;
        log_.Info("null backend: {}x{} per eye, {} Hz, IPD {:.1f} mm, swapchain {}, motion {}", opt_.eyeWidth, opt_.eyeHeight,
                  opt_.refreshHz, opt_.ipdMetres * 1000.0f, DxgiFormatName(fmt), MotionName(opt_.motion));
        return Result::Ok;
    }

    void Shutdown() override {
        if (!initialized_) return;
        blitter_.ClearCache();
        {
            std::lock_guard lk(quadMutex_);
            quads_ = {};
        }
        for (auto& e : eyes_) e = EyeImage{};
        initialized_ = false;
        ShutdownCommon();
    }

    Result WaitFrame(FrameInfo& info) override {
        info = FrameInfo{};
        info.state = state_.load();
        if (!initialized_) return Result::NotInitialized;
        const int64_t period = Period();
        if (opt_.paceToRefresh) {
            const int64_t now = QpcNowNs();
            if (nextDeadline_ == 0 || nextDeadline_ < now - period) nextDeadline_ = now;
            const int64_t wait = nextDeadline_ - now;
            if (wait > 0) SleepPrecise(wait);
            nextDeadline_ += period;
        }
        Pose rawHead;
        {
            std::lock_guard lk(frameMutex_);
            FrameRecord& r = NewFrameLocked();
            r.displayTime = QpcNowNs() + period;
            r.period = period;
            r.shouldRender = true;
            r.orientationValid = r.positionValid = true;
            rawHead = HeadPose(r.id);
            ComputeViews(rawHead, r.raw);
            r.rawHead = rawHead;
            r.recenter = UpdateRecenter(rawHead, true);
            FillFrameInfo(r, info);
        }
        CountStat(&FrameStats::framesWaited);
        return Result::Ok;
    }

    Result BeginFrame(uint64_t frameId) override {
        if (!initialized_) return Result::NotInitialized;
        std::lock_guard lk(frameMutex_);
        FrameRecord* r = FindFrameLocked(frameId);
        if (!r) return Stale(frameId);
        if (r->begun) return Result::CallOrder;
        r->begun = true;
        CountStat(&FrameStats::framesBegun);
        return Result::Ok;
    }

    Result RelocateViews(uint64_t frameId, View outViews[2]) override {
        if (!initialized_) return Result::NotInitialized;
        std::lock_guard lk(frameMutex_);
        FrameRecord* r = FindFrameLocked(frameId);
        if (!r) return Result::CallOrder;
        for (int e = 0; e < 2; ++e) outViews[e] = ApplyRecenter(r->recenter, r->raw[e]);
        return Result::Ok;
    }

    Result SubmitFrame(uint64_t frameId, const SubmitDesc& desc) override {
        if (!initialized_) return Result::NotInitialized;
        FrameRecord rec;
        {
            std::lock_guard lk(frameMutex_);
            FrameRecord* r = FindFrameLocked(frameId);
            if (!r) return Stale(frameId);
            if (r->ended) return Result::CallOrder;
            if (!r->begun) {
                r->begun = true;
                CountStat(&FrameStats::framesBegun);
            }
            r->ended = true;
            rec = *r;
        }
        if (!ValidateSubmit(desc)) return Result::InvalidArgument;

        bool ok = true;
        {
            ScopedStateBackup backup(stateBackup_, context_.Get());
            capture_.BeginFrame(frameId);
            LayerImage projection[2]{};
            if (desc.texture) {
                for (int e = 0; e < 2; ++e) {
                    EyeImage& img = eyes_[e];
                    const bool update = desc.eyes[e].update || !img.hasImage;
                    if (update) {
                        uint32_t w = 0, h = 0;
                        const EyeTarget t{img.texture.Get(), format_, opt_.eyeWidth, opt_.eyeHeight, 0};
                        if (TransferEye(static_cast<Eye>(e), desc, t, &w, &h)) {
                            img.hasImage = true;
                            img.w = w;
                            img.h = h;
                            img.view = SubmittedView(rec, desc, e);
                        } else {
                            ok = false;
                        }
                    }
                    // Alternate-eye mode: an eye that is not updated shows its previous image.
                    if (img.hasImage) projection[e] = LayerImage{img.texture.Get(), format_, img.w, img.h};
                }
            }
            for (uint32_t i = 0; i < desc.quadCount; ++i) {
                const QuadLayer& q = desc.quads[i];
                QuadSlot* s = FindQuad(q.layer);
                if (!s || !q.texture) continue;
                uint32_t w = 0, h = 0;
                const EyeTarget t{s->sc.owned.Get(), s->sc.format, s->sc.width, s->sc.height, 0};
                if (TransferQuad(q, t, &w, &h)) {
                    s->sc.hasImage = true;
                    s->sc.lastW = w;
                    s->sc.lastH = h;
                } else {
                    ok = false;
                }
            }
            CaptureComposited(rec, desc, projection, opt_.eyeWidth, opt_.eyeHeight);
            capture_.EndFrame();
        }
        CountStat(&FrameStats::framesSubmitted);
        return ok ? Result::Ok : Result::Error;
    }

    Result CreateQuadLayer(const QuadLayerCreateDesc& desc, LayerHandle* out) override {
        if (out) *out = 0;
        if (!initialized_) return Result::NotInitialized;
        if (!out || desc.width == 0 || desc.height == 0 || desc.width > 16384 || desc.height > 16384) return Result::InvalidArgument;
        static const DXGI_FORMAT kOffered[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                               DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_FORMAT_R10G10B10A2_UNORM,
                                               DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM};
        const std::vector<DXGI_FORMAT> offered(std::begin(kOffered), std::end(kOffered));
        const DXGI_FORMAT fmt = PickLayerFormat(desc.sourceFormatHint, offered, kOffered, std::size(kOffered));
        D3D11_TEXTURE2D_DESC d{};
        d.Width = desc.width;
        d.Height = desc.height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = TypelessFamily(fmt);
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        const HRESULT hr = device_->CreateTexture2D(&d, nullptr, &tex);
        if (FAILED(hr)) {
            log_.Error("null backend: quad layer {}x{} {} failed {}", desc.width, desc.height, DxgiFormatName(fmt), HResultString(hr));
            return Result::Error;
        }
        std::lock_guard lk(quadMutex_);
        const int idx = FreeQuadSlotLocked();
        if (idx < 0) {
            log_.Error("null backend: no free quad layer slot (max {})", kMaxQuadLayers);
            return Result::Error;
        }
        QuadSlot& s = quads_[idx];
        s = QuadSlot{};
        s.used = true;
        s.sc.owned = tex;
        s.sc.width = desc.width;
        s.sc.height = desc.height;
        s.sc.format = fmt;
        *out = MakeHandle(idx);
        log_.Info("null backend: quad layer 0x{:X} {}x{} {}", *out, desc.width, desc.height, DxgiFormatName(fmt));
        return Result::Ok;
    }

    void DestroyQuadLayer(LayerHandle layer) override {
        std::lock_guard lk(quadMutex_);
        QuadSlot* s = FindQuad(layer);
        if (!s) return;
        blitter_.Forget(s->sc.owned.Get());
        *s = QuadSlot{};
    }

    Result SkipFrame(uint64_t frameId) override {
        if (!initialized_) return Result::NotInitialized;
        std::lock_guard lk(frameMutex_);
        FrameRecord* r = FindFrameLocked(frameId);
        if (!r) return Stale(frameId);
        if (r->ended) return Result::CallOrder;
        r->begun = r->ended = true;
        CountStat(&FrameStats::framesSkipped);
        return Result::Ok;
    }

private:
    struct EyeImage {
        ComPtr<ID3D11Texture2D> texture;
        bool hasImage = false;
        uint32_t w = 0, h = 0;
        View view{};
    };

    int64_t Period() const { return static_cast<int64_t>(std::llround(1e9 / opt_.refreshHz)); }

    Result Stale(uint64_t frameId) {
        if (frameId == 0 || frameId >= nextFrameId_) {
            log_.Error("unknown frameId {}", frameId);
            return Result::CallOrder;
        }
        return Result::Ok;  // old frame that fell out of the ring: ignored
    }

    // Deterministic head pose for a frame: a function of frameId only.
    Pose HeadPose(uint64_t frameId) const {
        const float t = static_cast<float>(static_cast<double>(frameId) / opt_.refreshHz);
        Pose p;
        const bool yaw = opt_.motion == NullMotion::YawSweep || opt_.motion == NullMotion::YawAndSway;
        const bool sway = opt_.motion == NullMotion::Sway || opt_.motion == NullMotion::YawAndSway;
        if (yaw) p.orientation = QuatFromAxisAngle(Vec3{0, 1, 0}, 30.0f * kDegToRad * std::sin(kTwoPi * t / 8.0f));
        if (sway) {
            p.position.x = 0.03f * std::sin(kTwoPi * t / 4.0f);
            p.position.y = 0.02f * std::sin(kTwoPi * t / 3.0f);
            p.position.z = 0.01f * std::sin(kTwoPi * t / 5.0f);
        }
        return p;
    }

    void ComputeViews(const Pose& head, View out[2]) const {
        const float half = opt_.ipdMetres * 0.5f;
        const Fov& l = opt_.fovLeftEye;
        const Fov fovs[2] = {l, Fov{-l.angleRight, -l.angleLeft, l.angleUp, l.angleDown}};
        for (int e = 0; e < 2; ++e) {
            Pose eyeLocal;
            eyeLocal.position.x = e == 0 ? -half : half;
            out[e].pose = PoseMultiply(head, eyeLocal);
            out[e].fov = fovs[e];
        }
    }

    NullOptions opt_{};
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    EyeImage eyes_[2];
    int64_t nextDeadline_ = 0;
};

}  // namespace

std::unique_ptr<IXrBackend> CreateNullBackend() { return std::make_unique<NullBackend>(); }

}  // namespace ff7vr::xr
