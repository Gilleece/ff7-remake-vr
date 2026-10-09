// Null backend: no runtime. Emulates a Quest 3 class headset with a fixed
// asymmetric FOV, a fixed IPD and a scripted, deterministic head motion. Eye
// and quad layer images are copied into emulated swapchain textures exactly
// like the OpenXR backend does; captures composite them the way a runtime
// would (see QUAD LAYERS in xr.h), so PNGs show what the user would see.
#include "backend_base.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// A plausible hidden area for the emulated headset: everything outside an ellipse
// centred on the view axis whose semi-axes are 5 % larger than the FOV's larger
// half-extent in each direction. That keeps the middle of every edge visible and
// hides the image corners, the region a real headset's lenses cannot show. The mesh
// fills the space between the ellipse and the image border (tangent space).
HiddenAreaMesh EmulatedHiddenArea(const Fov& fov) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const float a = 1.05f * std::max(-l, r), b = 1.05f * std::max(u, -d);
    // Radial projection of a direction from the axis onto the image border; edge 0..3 = right, top, left, bottom.
    auto border = [&](float x, float y, int* edge) {
        float t = 1e30f;
        if (x > 0 && r / x < t) t = r / x, *edge = 0;
        if (y > 0 && u / y < t) t = u / y, *edge = 1;
        if (x < 0 && l / x < t) t = l / x, *edge = 2;
        if (y < 0 && d / y < t) t = d / y, *edge = 3;
        return std::pair{x * t, y * t};
    };
    const float corners[4][2] = {{r, u}, {l, u}, {l, d}, {r, d}};  // corner after edge i (counter-clockwise)
    HiddenAreaMesh m;
    auto add = [&](float x, float y) {
        m.xy.push_back(x);
        m.xy.push_back(y);
        return static_cast<uint32_t>(m.xy.size() / 2 - 1);
    };
    constexpr int kSegments = 96;
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = kTwoPi * float(i) / kSegments, a1 = kTwoPi * float(i + 1) / kSegments;
        int e0 = 0, e1 = 0;
        const auto [qx0, qy0] = border(std::cos(a0), std::sin(a0), &e0);
        const auto [qx1, qy1] = border(std::cos(a1), std::sin(a1), &e1);
        // Ellipse points, clamped to the border where the ellipse lies outside the image.
        float px0 = a * std::cos(a0), py0 = b * std::sin(a0), px1 = a * std::cos(a1), py1 = b * std::sin(a1);
        if (px0 * px0 + py0 * py0 > qx0 * qx0 + qy0 * qy0) px0 = qx0, py0 = qy0;
        if (px1 * px1 + py1 * py1 > qx1 * qx1 + qy1 * qy1) px1 = qx1, py1 = qy1;
        const uint32_t p0 = add(px0, py0), p1 = add(px1, py1), q0 = add(qx0, qy0), q1 = add(qx1, qy1);
        m.indices.insert(m.indices.end(), {p0, q0, q1, p0, q1, p1});
        if (e0 != e1) {
            const uint32_t c = add(corners[e0][0], corners[e0][1]);
            m.indices.insert(m.indices.end(), {q0, c, q1});
        }
    }
    return m;
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
        // Emulated depth swapchains (XR_KHR_composition_layer_depth): D32_FLOAT images,
        // typeless like a runtime's, readable for captures.
        depthFormat_ = DXGI_FORMAT_UNKNOWN;
        depthOff_ = false;
        for (int e = 0; e < 2; ++e) {
            depth_[e] = SwapImages{};
            depthValid_[e] = false;
        }
        if (desc.depthLayer) {
            bool ok = true;
            for (int e = 0; e < 2 && ok; ++e) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = opt_.eyeWidth;
                d.Height = opt_.eyeHeight;
                d.MipLevels = 1;
                d.ArraySize = 1;
                d.Format = DXGI_FORMAT_R32_TYPELESS;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
                const HRESULT hr = device_->CreateTexture2D(&d, nullptr, depth_[e].owned.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    log_.Warn("null backend: depth image {}x{} failed {}; no depth layer", d.Width, d.Height, HResultString(hr));
                    ok = false;
                    break;
                }
                depth_[e].width = d.Width;
                depth_[e].height = d.Height;
                depth_[e].format = DXGI_FORMAT_D32_FLOAT;
            }
            if (ok) depthFormat_ = DXGI_FORMAT_D32_FLOAT;
            else for (auto& d : depth_) d = SwapImages{};
        }

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
            info_.depthLayerSupported = desc.requestDepthExtension;
            info_.depthSwapchainFormat = depthFormat_;
            if (depthFormat_ == DXGI_FORMAT_UNKNOWN) info_.depthNote = desc.depthLayer ? "depth images could not be created" : "depth layer not wanted";
            info_.lastPredictedDisplayPeriod = Period();
            gazeEnabled_ = desc.eyeGaze;
            if (gazeEnabled_) {
                info_.gazeSource = "simulated";
                info_.gazeNote = "Null backend, driven by 'xr-sim gaze'";
            }
        }
        if (gazeEnabled_) log_.Info("eye gaze: source simulated (not tracked until 'xr-sim gaze <yaw> <pitch>' or 'xr-sim gaze sweep')");
        if (opt_.hiddenArea) {
            Pose none;
            View v[2];
            ComputeViews(none, v);
            for (int e = 0; e < 2; ++e) SetHiddenAreaMesh(e == 0 ? Eye::Left : Eye::Right, EmulatedHiddenArea(v[e].fov));
        } else {
            SetHiddenAreaMesh(Eye::Left, {});
            SetHiddenAreaMesh(Eye::Right, {});
        }
        nextDeadline_ = 0;
        {
            std::lock_guard sl(simMutex_);
            sim_ = Sim{};
        }
        origin_ = Pose{};
        originChange_ = false;
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
        for (auto& d : depth_) {
            if (d.owned) blitter_.Forget(d.owned.Get());
            d = SwapImages{};
        }
        depthFormat_ = DXGI_FORMAT_UNKNOWN;
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
        {
            std::lock_guard lk(frameMutex_);
            FrameRecord& r = NewFrameLocked();
            r.displayTime = QpcNowNs() + period;
            r.period = period;
            r.shouldRender = true;
            Sim sim;
            {
                std::lock_guard sl(simMutex_);
                sim = sim_;
                sim_.recenterEventRequested = false;
                if (sim_.loseOrientation) --sim_.loseOrientation;
                if (sim_.losePosition) --sim_.losePosition;
                if (sim_.gazeBlink) --sim_.gazeBlink;
            }
            // The emulated tracker: scripted motion, then the head pose set with `head`.
            const NullMotion motion = sim.motion >= 0 ? static_cast<NullMotion>(sim.motion) : opt_.motion;
            const Pose tracker = PoseMultiply(PoseMultiply(sim.head, YawRatePose(sim, QpcNowNs())), HeadPose(r.id, motion));
            if (sim.recenterEventRequested) {
                // Like a runtime's own recenter: the current leveled head becomes the new
                // LOCAL origin, from a change time a few frames ahead (midway between two
                // display times, as the OpenXR specification recommends).
                Pose level;
                level.orientation = QuatFromAxisAngle(Vec3{0, 1, 0}, QuatYaw(tracker.orientation));
                level.position = tracker.position;
                originChange_ = true;
                newOrigin_ = level;
                originChangeTime_ = r.displayTime + int64_t(sim.recenterDelayFrames) * period - period / 2;
                const Pose inPrevious = PoseMultiply(PoseInverse(origin_), level);
                NoteLocalSpaceChange(originChangeTime_, sim.recenterPoseValid, sim.recenterPoseValid ? inPrevious : Pose{}, QpcNowNs());
                log_.Info("null backend: runtime recenter simulated at frame {}: LOCAL space change event, change time in {} frames, pose of the "
                          "new origin {} (yaw {:.1f} deg, position ({:.3f}, {:.3f}, {:.3f}) m in the old space)",
                          r.id, sim.recenterDelayFrames, sim.recenterPoseValid ? "given" : "not given",
                          QuatYaw(inPrevious.orientation) * kRadToDeg, inPrevious.position.x, inPrevious.position.y, inPrevious.position.z);
            }
            if (originChange_ && r.displayTime >= originChangeTime_) {
                origin_ = newOrigin_;
                originChange_ = false;
            }
            const Pose rawHead = PoseMultiply(PoseInverse(origin_), tracker);
            ComputeViews(rawHead, r.raw);
            r.rawHead = rawHead;
            bool o = true, p = true;
            const float nan = std::numeric_limits<float>::quiet_NaN();
            if (sim.loseOrientation) {
                // Undefined values, as a runtime may return them with the flags cleared.
                o = p = false;
                for (Pose* q : {&r.rawHead, &r.raw[0].pose, &r.raw[1].pose}) *q = Pose{Quat{nan, nan, nan, nan}, Vec3{nan, nan, nan}};
            } else if (sim.losePosition) {
                p = false;
                for (Pose* q : {&r.rawHead, &r.raw[0].pose, &r.raw[1].pose}) q->position = Vec3{nan, nan, nan};
            }
            ApplySpaceChange(r.displayTime, r.id, r.rawHead, o && p);
            SanitizePoses(r, o, p, o, p);
            r.recenter = UpdateRecenter(r.rawHead, r.orientationValid);
            r.layerRecenter = recenter_;
            lastRawHead_ = r.rawHead;
            lastTracker_ = tracker;
            lastRecenter_ = r.recenter;
            lastO_ = r.orientationValid;
            lastP_ = r.positionValid;
            lastId_ = r.id;
            FillFrameInfo(r, info);
            if (gazeEnabled_) SimulatedGaze(sim, r, &info.gaze);
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

    // The emulated tracker located again now: the scripted motion of the frame, the
    // `head` pose, the yaw rate at the current time and the `late-yaw` test offset
    // (which only late locations see), in the frame's LOCAL space and recenter.
    Result RelocateViews(uint64_t frameId, View outViews[2]) override {
        if (!initialized_) return Result::NotInitialized;
        Sim sim;
        {
            std::lock_guard sl(simMutex_);
            sim = sim_;
        }
        std::lock_guard lk(frameMutex_);
        FrameRecord* r = FindFrameLocked(frameId);
        if (!r) return Result::CallOrder;
        if (!(r->orientationValid && r->positionValid) || sim.loseOrientation || sim.losePosition) return Result::Error;
        const NullMotion motion = sim.motion >= 0 ? static_cast<NullMotion>(sim.motion) : opt_.motion;
        const Pose late = QuatPose(QuatFromAxisAngle(Vec3{0, 1, 0}, float(sim.lateYawDeg) * kDegToRad));
        const Pose tracker = PoseMultiply(PoseMultiply(PoseMultiply(sim.head, YawRatePose(sim, QpcNowNs())), late), HeadPose(r->id, motion));
        const Pose rawHead = PoseMultiply(PoseInverse(origin_), tracker);
        View raw[2];
        ComputeViews(rawHead, raw);
        for (int e = 0; e < 2; ++e) outViews[e] = ApplyRecenter(r->recenter, raw[e]);
        ++relocations_;
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
            GpuFrameBegin();
            LayerImage projection[2]{};
            if (desc.texture) {
                for (int e = 0; e < 2; ++e) {
                    EyeImage& img = eyes_[e];
                    const bool update = desc.eyes[e].update || !img.hasImage;
                    if (update) {
                        uint32_t w = 0, h = 0;
                        const EyeTarget t{img.texture.Get(), format_, opt_.eyeWidth, opt_.eyeHeight, 0};
                        const bool copied = TransferEye(static_cast<Eye>(e), desc, t, &w, &h);
                        if (copied) {
                            img.hasImage = true;
                            img.w = w;
                            img.h = h;
                            img.view = SubmittedView(rec, desc, e);
                            if (e == 0) {
                                // What the runtime would be told the image was rendered with, against
                                // the frame's own (frame wait) location: `xr-sim status`.
                                std::lock_guard lk(frameMutex_);
                                lastSubmittedYaw_ = QuatYaw(img.view.pose.orientation) * kRadToDeg;
                                lastSubmittedFrameYaw_ = QuatYaw(rec.raw[0].pose.orientation) * kRadToDeg;
                                lastSubmittedId_ = rec.id;
                            }
                        } else {
                            ok = false;
                            depthValid_[e] = false;
                        }
                        UpdateDepthEye(e, desc, copied, img.w, img.h, [&](SwapImages& ds, auto&& transfer) {
                            const EyeTarget dt{ds.owned.Get(), depthFormat_, ds.width, ds.height, 0};
                            const bool done = transfer(dt);
                            ds.hasImage = ds.hasImage || done;
                            return done;
                        });
                    }
                    // Alternate-eye mode: an eye that is not updated shows its previous image.
                    if (img.hasImage) projection[e] = LayerImage{img.texture.Get(), format_, img.w, img.h};
                }
                // Same rule as the OpenXR backend: no projection layer without real poses.
                if (!rec.posesUsable && !(desc.eyes[0].viewOverride && desc.eyes[1].viewOverride)) projection[0] = projection[1] = LayerImage{};
            }
            // A runtime would get depth chained to both views (OpenXR backend: same rule).
            const bool depthLayer = projection[0].texture && projection[1].texture && depthValid_[0] && depthValid_[1] &&
                                    depthFormat_ != DXGI_FORMAT_UNKNOWN && !depthOff_.load();
            for (uint32_t i = 0; i < desc.quadCount; ++i) {
                const QuadLayer& q = desc.quads[i];
                QuadSlot* s = FindQuad(q.layer);
                if (!s || !q.texture) continue;
                uint32_t w = 0, h = 0;
                const EyeTarget t{s->sc.owned.Get(), s->sc.format, s->sc.width, s->sc.height, 0};
                if (TransferQuad(desc, q, t, &w, &h)) {
                    s->sc.hasImage = true;
                    s->sc.lastW = w;
                    s->sc.lastH = h;
                } else {
                    ok = false;
                }
            }
            GpuFrameEnd();
            CaptureComposited(rec, desc, projection, opt_.eyeWidth, opt_.eyeHeight);
            if (depthLayer) {
                CaptureDepthImages();
                CountStat(&FrameStats::depthLayers);
                lastDepthNear_ = depthNear_[0];
                lastDepthFar_ = depthFar_[0];
            }
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

    // Commands (any thread):
    //   status                                   emulated tracker, LOCAL origin, head, recenter, flags
    //   head <yaw deg> [pitch deg] [x y z m]      the emulated head pose (on top of [xr] null_motion)
    //   recenter-event [nopose] [delay <frames>]  the runtime's own recenter: LOCAL moves to the current
    //                                            leveled head; event with or without poseInPreviousSpace
    //   lose orientation|position <frames>       the next frames report that part invalid (values NaN)
    //   gaze <yaw> <pitch> | off | sweep [radius deg] [period s] | blink <frames> | status
    //                                            the simulated eye tracker (with InitDesc::eyeGaze): a fixed gaze in
    //                                            head space (yaw + right, pitch + up), not tracked, a circle around
    //                                            the view axis, or a short loss of tracking
    std::string Simulate(std::string_view command) override {
        std::vector<std::string> a;
        {
            std::string cur;
            for (char c : command) {
                if (c == ' ' || c == '\t') {
                    if (!cur.empty()) a.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            if (!cur.empty()) a.push_back(std::move(cur));
        }
        auto num = [](const std::string& s, double* out) {
            const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), *out);
            return ec == std::errc() && p == s.data() + s.size();
        };
        const char* usage =
            "err usage: xr-sim status | head <yaw deg> [pitch deg] [x y z m] | recenter-event [nopose] [delay <frames>] | lose "
            "orientation|position <frames> | gaze <yaw> <pitch> | gaze off | gaze sweep [radius deg] [period s] | gaze blink <frames> | "
            "motion static|yaw|sway|yawsway|ini | focus 0|1 | presence 0|1|off | yawrate <deg/s> | late-yaw <deg> | depth";
        if (!initialized_) return "err null backend not initialised";
        if (a.empty() || a[0] == "status") {
            std::lock_guard lk(frameMutex_);
            std::lock_guard sl(simMutex_);
            auto text = [](const Pose& p) {
                return std::format("yaw {:.1f} pitch {:.1f} pos ({:.3f} {:.3f} {:.3f})", QuatYaw(p.orientation) * kRadToDeg,
                                   std::asin(std::clamp(2.0f * (p.orientation.w * p.orientation.x - p.orientation.y * p.orientation.z), -1.0f, 1.0f)) *
                                       kRadToDeg,
                                   p.position.x, p.position.y, p.position.z);
            };
            const Pose seen = ApplyRecenter(lastRecenter_, lastRawHead_);
            return std::format("ok frame {} tracker head {} | LOCAL origin {}{} | head in LOCAL {} | recenter {} | head seen by the game {} | "
                               "orientation {} position {} | lose orientation {} position {} frames | last image submitted for frame {}: left eye "
                               "rendered at yaw {:.3f} deg (LOCAL), the frame wait located it at {:.3f} deg | yaw rate {:.1f} deg/s, late yaw {:.2f} deg, "
                               "late locations {}",
                               lastId_, text(lastTracker_), text(origin_), originChange_ ? " (change pending)" : "", text(lastRawHead_),
                               text(lastRecenter_), text(seen), lastO_ ? "valid" : "INVALID", lastP_ ? "valid" : "INVALID", sim_.loseOrientation,
                               sim_.losePosition, lastSubmittedId_, lastSubmittedYaw_, lastSubmittedFrameYaw_, sim_.yawRateDegS, sim_.lateYawDeg,
                               relocations_.load());
        }
        if (a[0] == "head") {
            double v[5]{};
            const size_t n = a.size() - 1;
            if (!(n == 1 || n == 2 || n == 4 || n == 5)) return usage;
            for (size_t i = 0; i < n; ++i)
                if (!num(a[i + 1], &v[i])) return usage;
            const double yaw = v[0], pitch = (n == 2 || n == 5) ? v[1] : 0.0;
            const double* xyz = n == 4 ? &v[1] : n == 5 ? &v[2] : nullptr;
            Pose p;
            p.orientation = QuatMultiply(QuatFromAxisAngle(Vec3{0, 1, 0}, float(yaw) * kDegToRad), QuatFromAxisAngle(Vec3{1, 0, 0}, float(pitch) * kDegToRad));
            if (xyz) p.position = Vec3{float(xyz[0]), float(xyz[1]), float(xyz[2])};
            {
                std::lock_guard sl(simMutex_);
                sim_.head = p;
            }
            log_.Info("null backend: emulated head yaw {:.1f} deg, pitch {:.1f} deg, position ({:.3f}, {:.3f}, {:.3f}) m", yaw, pitch, p.position.x,
                      p.position.y, p.position.z);
            return std::format("ok head yaw {:.1f} pitch {:.1f} position {:.3f} {:.3f} {:.3f}", yaw, pitch, p.position.x, p.position.y, p.position.z);
        }
        if (a[0] == "motion" && a.size() == 2) {
            int m = -1;
            if (a[1] == "static") m = int(NullMotion::Static);
            else if (a[1] == "yaw") m = int(NullMotion::YawSweep);
            else if (a[1] == "sway") m = int(NullMotion::Sway);
            else if (a[1] == "yawsway") m = int(NullMotion::YawAndSway);
            else if (a[1] != "ini") return "err usage: xr-sim motion static|yaw|sway|yawsway|ini";
            {
                std::lock_guard sl(simMutex_);
                sim_.motion = m;
            }
            const NullMotion now = m >= 0 ? static_cast<NullMotion>(m) : opt_.motion;
            log_.Info("null backend: scripted head motion now {}", MotionName(now));
            return std::format("ok motion {}", MotionName(now));
        }
        if (a[0] == "yawrate" && a.size() == 2) {
            // The head turns at a constant rate with the wall clock: every location (the
            // frame wait's and a late one) sees the yaw of the moment it is made.
            double rate = 0;
            if (!num(a[1], &rate) || std::fabs(rate) > 3600) return usage;
            {
                std::lock_guard sl(simMutex_);
                sim_.yawRateDegS = rate;
                sim_.yawRateStartNs = QpcNowNs();
            }
            log_.Info("null backend: head yaw rate {:.1f} deg/s", rate);
            return std::format("ok head yaw rate {:.1f} deg/s (from 0 now)", rate);
        }
        if (a[0] == "late-yaw" && a.size() == 2) {
            // Test of late view updates: only RelocateViews sees this extra yaw.
            double deg = 0;
            if (!num(a[1], &deg) || std::fabs(deg) > 90) return usage;
            {
                std::lock_guard sl(simMutex_);
                sim_.lateYawDeg = deg;
            }
            log_.Info("null backend: late locations add {:.2f} deg of yaw", deg);
            return std::format("ok late locations add {:.2f} deg of yaw ({} late locations so far)", deg, relocations_.load());
        }
        if (a[0] == "depth") {
            const FrameStats st = GetStats();
            return std::format("ok depth images {} ({}), frames with a depth layer {}, depth images not written {}, last nearZ {} farZ {}",
                               depthFormat_ == DXGI_FORMAT_UNKNOWN ? "none" : DxgiFormatName(depthFormat_), st.depthImages, st.depthLayers,
                               st.depthFailures, lastDepthNear_, lastDepthFar_);
        }
        if (a[0] == "focus" && a.size() == 2) {
            // The runtime's session state: focus 0 = VISIBLE (headset off, the runtime's menu), 1 = FOCUSED.
            const bool f = a[1] == "1";
            state_ = f ? SessionState::Focused : SessionState::Visible;
            log_.Info("null backend: session state {} simulated", f ? "FOCUSED" : "VISIBLE");
            return std::string("ok session ") + (f ? "FOCUSED" : "VISIBLE");
        }
        if (a[0] == "presence" && a.size() == 2) {
            // XR_EXT_user_presence: presence 0 | 1 | off (off = the extension's events never came).
            simPresence_ = a[1] == "off" ? -1 : a[1] == "1" ? 1 : 0;
            log_.Info("null backend: user presence {} simulated", a[1]);
            return "ok user presence " + a[1];
        }
        if (a[0] == "recenter-event") {
            bool poseValid = true;
            double delay = 3;
            for (size_t i = 1; i < a.size(); ++i) {
                if (a[i] == "nopose") {
                    poseValid = false;
                } else if (a[i] == "delay" && i + 1 < a.size() && num(a[i + 1], &delay) && delay >= 0 && delay <= 1000) {
                    ++i;
                } else {
                    return usage;
                }
            }
            std::lock_guard sl(simMutex_);
            sim_.recenterEventRequested = true;
            sim_.recenterPoseValid = poseValid;
            sim_.recenterDelayFrames = static_cast<uint32_t>(delay);
            return std::format("ok runtime recenter with the next frame (pose {}, change time {} frames ahead)", poseValid ? "given" : "not given",
                               sim_.recenterDelayFrames);
        }
        if (a[0] == "lose" && a.size() == 3) {
            double frames = 0;
            if (!num(a[2], &frames) || frames < 0 || frames > 1e6) return usage;
            std::lock_guard sl(simMutex_);
            if (a[1] == "orientation")
                sim_.loseOrientation = static_cast<uint32_t>(frames);
            else if (a[1] == "position")
                sim_.losePosition = static_cast<uint32_t>(frames);
            else
                return usage;
            log_.Info("null backend: the next {} frames report the {} invalid", static_cast<uint32_t>(frames), a[1]);
            return std::format("ok the next {} frames report the {} invalid", static_cast<uint32_t>(frames), a[1]);
        }
        if (a[0] == "gaze") {
            const char* gazeUsage =
                "err usage: xr-sim gaze <yaw deg, + right> <pitch deg, + up> | gaze off | gaze sweep [radius deg] [period s] | gaze blink <frames> | "
                "gaze status";
            if (!gazeEnabled_) return "err no simulated eye tracker in this session ([foveation] eye_tracking = 0 when the session started)";
            if (a.size() == 1 || a[1] == "status") {
                std::lock_guard lk(frameMutex_);
                std::lock_guard sl(simMutex_);
                const Vec3& d = lastGaze_.direction;
                return std::format("ok gaze {} ({}), direction ({:.3f}, {:.3f}, {:.3f}) = yaw {:.1f} pitch {:.1f} deg, blink {} frames",
                                   sim_.gazeMode == 0 ? "off" : sim_.gazeMode == 1 ? "fixed" : "sweep", lastGaze_.tracked ? "tracked" : "not tracked",
                                   d.x, d.y, d.z, std::atan2(d.x, -d.z) * kRadToDeg, std::asin(std::clamp(d.y, -1.0f, 1.0f)) * kRadToDeg,
                                   sim_.gazeBlink);
            }
            std::string reply;
            {
                std::lock_guard sl(simMutex_);
                if (a[1] == "off" && a.size() == 2) {
                    sim_.gazeMode = 0;
                    reply = "ok gaze not tracked";
                } else if (a[1] == "sweep" && a.size() <= 4) {
                    double radius = 15, period = 4;
                    if ((a.size() > 2 && (!num(a[2], &radius) || radius < 0 || radius > 60)) ||
                        (a.size() > 3 && (!num(a[3], &period) || period < 0.1 || period > 600)))
                        return gazeUsage;
                    sim_.gazeMode = 2;
                    sim_.sweepRadiusDeg = radius;
                    sim_.sweepPeriodS = period;
                    reply = std::format("ok gaze sweeps a circle of {:.1f} deg around the view axis every {:.2f} s", radius, period);
                } else if (a[1] == "blink" && a.size() == 3) {
                    double frames = 0;
                    if (!num(a[2], &frames) || frames < 0 || frames > 1e6) return gazeUsage;
                    sim_.gazeBlink = static_cast<uint32_t>(frames);
                    reply = std::format("ok the next {} frames report the gaze not tracked", sim_.gazeBlink);
                } else if (a.size() == 3) {
                    double yaw = 0, pitch = 0;
                    if (!num(a[1], &yaw) || !num(a[2], &pitch) || std::fabs(yaw) > 80 || std::fabs(pitch) > 80) return gazeUsage;
                    sim_.gazeMode = 1;
                    sim_.gazeYawDeg = yaw;
                    sim_.gazePitchDeg = pitch;
                    reply = std::format("ok gaze yaw {:.1f} pitch {:.1f} deg (head space)", yaw, pitch);
                } else {
                    return gazeUsage;
                }
            }
            log_.Info("null backend: {}", reply.substr(3));
            return reply;
        }
        return usage;
    }

private:
    struct Sim {
        Pose head{};
        bool recenterEventRequested = false;
        bool recenterPoseValid = true;
        uint32_t recenterDelayFrames = 3;
        uint32_t loseOrientation = 0, losePosition = 0;
        // Eye gaze (with InitDesc::eyeGaze): 0 not tracked, 1 fixed direction, 2 circle.
        int gazeMode = 0;
        double gazeYawDeg = 0, gazePitchDeg = 0;            // mode 1; yaw positive to the right, pitch positive up
        double sweepRadiusDeg = 15, sweepPeriodS = 4;        // mode 2
        uint32_t gazeBlink = 0;                              // the next frames report the gaze not tracked
        int motion = -1;                                     // `motion`: replaces [xr] null_motion (-1 = the ini's)
        double yawRateDegS = 0;                              // `yawrate`: head yaw grows with the wall clock
        int64_t yawRateStartNs = 0;
        double lateYawDeg = 0;                               // `late-yaw`: extra yaw seen by RelocateViews only
    };
    static Pose QuatPose(const Quat& q) {
        Pose p;
        p.orientation = q;
        return p;
    }
    static Pose YawRatePose(const Sim& sim, int64_t nowNs) {
        if (sim.yawRateDegS == 0) return Pose{};
        const double deg = sim.yawRateDegS * double(nowNs - sim.yawRateStartNs) / 1e9;
        return QuatPose(QuatFromAxisAngle(Vec3{0, 1, 0}, float(std::fmod(deg, 360.0)) * kDegToRad));
    }
    std::atomic<uint64_t> relocations_{0};
    float lastSubmittedYaw_ = 0, lastSubmittedFrameYaw_ = 0;  // under frameMutex_
    uint64_t lastSubmittedId_ = 0;
    float lastDepthNear_ = 0, lastDepthFar_ = 0;
    bool gazeEnabled_ = false;
    std::atomic<int> simPresence_{-1};

public:
    int UserPresence() const override { return simPresence_.load(); }

private:
    GazeSample lastGaze_{};  // GT, read by `gaze status` under frameMutex_

    static Vec3 GazeDirection(double yawDeg, double pitchDeg) {
        const double y = yawDeg * kDegToRad, p = pitchDeg * kDegToRad;
        return Vec3{float(std::sin(y) * std::cos(p)), float(std::sin(p)), float(-std::cos(y) * std::cos(p))};
    }

    // GT, under frameMutex_: the simulated eye tracker for frame r.
    void SimulatedGaze(const Sim& sim, const FrameRecord& r, GazeSample* out) {
        *out = GazeSample{};
        out->available = true;
        out->displayTime = r.displayTime;
        if (sim.gazeMode != 0 && sim.gazeBlink == 0) {
            double yaw = sim.gazeYawDeg, pitch = sim.gazePitchDeg;
            if (sim.gazeMode == 2) {
                const double t = static_cast<double>(r.id) / opt_.refreshHz;
                const double a = 2.0 * 3.141592653589793 * t / sim.sweepPeriodS;
                yaw = sim.sweepRadiusDeg * std::cos(a);
                pitch = sim.sweepRadiusDeg * std::sin(a);
            }
            out->tracked = true;
            out->nominal = true;
            out->direction = GazeDirection(yaw, pitch);
            // Like a tracker that cannot predict: the newest sample, taken now, older than the display time.
            out->sampleTime = QpcNowNs();
        }
        lastGaze_ = *out;
    }
    std::mutex simMutex_;
    Sim sim_{};  // under simMutex_
    // GT (read by `status` under frameMutex_): the emulated LOCAL origin in tracker space.
    Pose origin_{};
    bool originChange_ = false;
    Pose newOrigin_{};
    int64_t originChangeTime_ = 0;
    Pose lastRawHead_{}, lastTracker_{}, lastRecenter_{};
    bool lastO_ = true, lastP_ = true;
    uint64_t lastId_ = 0;

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
    Pose HeadPose(uint64_t frameId, NullMotion motion) const {
        const float t = static_cast<float>(static_cast<double>(frameId) / opt_.refreshHz);
        Pose p;
        const bool yaw = motion == NullMotion::YawSweep || motion == NullMotion::YawAndSway;
        const bool sway = motion == NullMotion::Sway || motion == NullMotion::YawAndSway;
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
