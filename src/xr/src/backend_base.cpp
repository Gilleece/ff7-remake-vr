#include "backend_base.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace ff7vr::xr {

std::unique_ptr<IXrBackend> CreateBackend(BackendType type) {
    switch (type) {
        case BackendType::Null: return CreateNullBackend();
        case BackendType::OpenXR: return CreateOpenXrBackend();
    }
    return nullptr;
}

DXGI_FORMAT PickLayerFormat(DXGI_FORMAT hint, const std::vector<DXGI_FORMAT>& offered, const DXGI_FORMAT* preferred, size_t count) {
    auto has = [&](DXGI_FORMAT f) { return f != DXGI_FORMAT_UNKNOWN && std::find(offered.begin(), offered.end(), f) != offered.end(); };
    if (hint != DXGI_FORMAT_UNKNOWN) {
        if (IsTypelessFormat(hint)) hint = DefaultTypedFormat(hint);
        const DXGI_FORMAT srgb = SrgbVariant(hint);
        // The game's back buffer holds sRGB-encoded values in a UNORM format:
        // the sRGB variant has the same bits and the right meaning (plain copy).
        if (has(srgb) && srgb != hint) return srgb;
        if (IsSrgbFormat(hint) && has(hint)) return hint;
    }
    for (size_t i = 0; i < count; ++i)
        if (has(preferred[i])) return preferred[i];
    return offered.empty() ? DXGI_FORMAT_UNKNOWN : offered.front();
}

void BackendBase::CountStat(uint64_t FrameStats::* field) {
    std::lock_guard lk(statsMutex_);
    ++(stats_.*field);
}

View BackendBase::SubmittedView(const FrameRecord& r, const SubmitDesc& desc, int eye) {
    const View* o = desc.eyes[eye].viewOverride;
    if (o) return RemoveRecenter(r.recenter, *o);
    return r.raw[eye];
}

Pose BackendBase::QuadPoseLocal(const FrameRecord& r, const QuadLayer& q) {
    if (q.space == LayerSpace::Head) return PoseMultiply(r.rawHead, q.pose);
    return PoseMultiply(r.layerRecenter, q.pose);  // undo the recenter: tracking space after recenter -> LOCAL
}

static bool RegionInside(const Rect& r, uint32_t w, uint32_t h) {
    return r.x >= 0 && r.y >= 0 && r.width != 0 && r.height != 0 && uint64_t(r.x) + r.width <= w && uint64_t(r.y) + r.height <= h;
}

bool BackendBase::ValidateSubmit(const SubmitDesc& desc) {
    if (desc.texture) {
        D3D11_TEXTURE2D_DESC d{};
        desc.texture->GetDesc(&d);
        if (desc.mipLevel >= d.MipLevels || desc.arraySlice >= d.ArraySize) {
            log_.Error("SubmitFrame: mip {} / slice {} out of range ({} mips, {} slices)", desc.mipLevel, desc.arraySlice, d.MipLevels,
                       d.ArraySize);
            return false;
        }
        const uint32_t w = std::max(1u, d.Width >> desc.mipLevel);
        const uint32_t h = std::max(1u, d.Height >> desc.mipLevel);
        for (int e = 0; e < 2; ++e) {
            const Rect r = EyeRect(desc, static_cast<Eye>(e));
            if (!RegionInside(r, w, h)) {
                log_.Error("SubmitFrame: eye {} rect ({},{} {}x{}) outside the {}x{} source", e, r.x, r.y, r.width, r.height, w, h);
                return false;
            }
        }
    }
    if (desc.quadCount && !desc.quads) {
        log_.Error("SubmitFrame: quadCount {} with no quads", desc.quadCount);
        return false;
    }
    if (desc.quadCount > kMaxQuadLayers) {
        log_.Error("SubmitFrame: {} quads (max {})", desc.quadCount, kMaxQuadLayers);
        return false;
    }
    for (uint32_t i = 0; i < desc.quadCount; ++i) {
        const QuadLayer& q = desc.quads[i];
        if (!FindQuad(q.layer)) {
            log_.Error("SubmitFrame: quad {} has an unknown or stale layer handle 0x{:X}", i, q.layer);
            return false;
        }
        if (!(q.width > 0.0f) || !(q.height > 0.0f)) {
            log_.Error("SubmitFrame: quad {} has size {}x{} m", i, q.width, q.height);
            return false;
        }
        if (q.texture) {
            D3D11_TEXTURE2D_DESC d{};
            q.texture->GetDesc(&d);
            if (q.mipLevel >= d.MipLevels || q.arraySlice >= d.ArraySize) {
                log_.Error("SubmitFrame: quad {} mip {} / slice {} out of range", i, q.mipLevel, q.arraySlice);
                return false;
            }
            const Rect r = QuadRect(q);
            const uint32_t w = std::max(1u, d.Width >> q.mipLevel);
            const uint32_t h = std::max(1u, d.Height >> q.mipLevel);
            if (!RegionInside(r, w, h)) {
                log_.Error("SubmitFrame: quad {} rect ({},{} {}x{}) outside the {}x{} source", i, r.x, r.y, r.width, r.height, w, h);
                return false;
            }
        }
    }
    return true;
}

RuntimeInfo BackendBase::GetRuntimeInfo() const {
    std::lock_guard lk(infoMutex_);
    return info_;
}

bool BackendBase::GetHiddenAreaMesh(Eye eye, HiddenAreaMesh* out) const {
    const int e = eye == Eye::Right ? 1 : 0;
    std::lock_guard lk(infoMutex_);
    if (hidden_[e].indices.size() < 3) return false;
    if (out) *out = hidden_[e];
    return true;
}

void BackendBase::SetHiddenAreaMesh(Eye eye, HiddenAreaMesh mesh) {
    const int e = eye == Eye::Right ? 1 : 0;
    {
        std::lock_guard lk(infoMutex_);
        hidden_[e] = std::move(mesh);
    }
    hiddenVersion_.fetch_add(1, std::memory_order_acq_rel);
}

FrameStats BackendBase::GetStats() const {
    std::lock_guard lk(statsMutex_);
    return stats_;
}

Result BackendBase::InitCommon(const InitDesc& desc) {
    log_.Set(desc.log);
    if (!desc.device) {
        log_.Error("Init: no ID3D11Device");
        return Result::InvalidArgument;
    }
    device_ = desc.device;
    device_->GetImmediateContext(context_.ReleaseAndGetAddressOf());
    if (!blitter_.Init(device_.Get(), &log_)) return Result::Error;
    if (!compositor_.Init(device_.Get(), &log_)) return Result::Error;
    capture_.Init(device_.Get(), &log_, &blitter_);
    gpuTiming_ = desc.gpuTiming;
    gpuCopy_.Reset();
    {
        std::lock_guard lk(frameMutex_);
        ring_ = {};
        nextFrameId_ = 1;
        ++epoch_;
    }
    {
        std::lock_guard lk(quadMutex_);
        quads_ = {};
        ++layerSerial_;
    }
    recenter_ = Pose{};
    recenterRequest_ = 0;
    track_ = PoseTrack{};
    track_.state = -1;
    spaceChange_ = SpaceChange{};
    {
        std::lock_guard lk(statsMutex_);
        stats_ = FrameStats{};
    }
    return Result::Ok;
}

void BackendBase::ShutdownCommon() {
    capture_.Shutdown();
    gpuCopy_.Reset();
    {
        std::lock_guard lk(quadMutex_);
        quads_ = {};
        ++layerSerial_;
    }
    for (int e = 0; e < 2; ++e) {
        composeTarget_[e].Reset();
        composeW_[e] = composeH_[e] = 0;
    }
    compositor_.Shutdown();
    blitter_.Shutdown();
    context_.Reset();
    device_.Reset();
    state_ = SessionState::Uninitialized;
}

BackendBase::FrameRecord& BackendBase::NewFrameLocked() {
    const uint64_t id = nextFrameId_++;
    FrameRecord& r = ring_[id % kRing];
    r = FrameRecord{};
    r.id = id;
    r.epoch = epoch_;
    return r;
}

BackendBase::FrameRecord* BackendBase::FindFrameLocked(uint64_t id) {
    if (id == 0) return nullptr;
    FrameRecord& r = ring_[id % kRing];
    return r.id == id ? &r : nullptr;
}

void BackendBase::AddSnapYaw(float radians) {
    float cur = snapRequest_.load(std::memory_order_relaxed);
    while (!snapRequest_.compare_exchange_weak(cur, cur + radians, std::memory_order_acq_rel)) {
    }
}

Pose BackendBase::UpdateRecenter(const Pose& rawHead, bool headValid) {
    const int req = recenterRequest_.exchange(0, std::memory_order_acq_rel);
    const float snapBefore = snapYaw_.load(std::memory_order_relaxed);
    if (req == 2) {
        recenter_ = Pose{};
        snapYaw_.store(0.0f, std::memory_order_release);
        snapRequest_.store(0.0f, std::memory_order_release);
        log_.Info("recenter reset");
    } else if (req == 1) {
        if (headValid) {
            // The current heading becomes forward: a snap turn in effect or pending ends here.
            snapYaw_.store(0.0f, std::memory_order_release);
            snapRequest_.store(0.0f, std::memory_order_release);
            const float yaw = QuatYaw(rawHead.orientation);
            recenter_.orientation = QuatFromAxisAngle(Vec3{0, 1, 0}, yaw);
            recenter_.position = rawHead.position;
            log_.Info("recentered: yaw {:.1f} deg, position ({:.3f}, {:.3f}, {:.3f}) m", yaw * kRadToDeg, rawHead.position.x,
                      rawHead.position.y, rawHead.position.z);
        } else {
            recenterRequest_.store(1);  // retry when tracking is valid
        }
    }
    const float add = snapRequest_.exchange(0.0f, std::memory_order_acq_rel);
    if (add != 0.0f) {
        float y = snapYaw_.load(std::memory_order_relaxed) + add;
        constexpr float kPi = 3.14159265f;
        while (y > kPi) y -= 2.0f * kPi;
        while (y <= -kPi) y += 2.0f * kPi;
        snapYaw_.store(y, std::memory_order_release);
        log_.Info("snap turn: {:+.1f} deg -> view turned {:.1f} deg from the recenter (was {:.1f})", add * kRadToDeg, y * kRadToDeg,
                  snapBefore * kRadToDeg);
    }
    const float snap = snapYaw_.load(std::memory_order_relaxed);
    if (snap == 0.0f) return recenter_;
    // Views = inverse(recenter * turn) * raw = Yaw(snap) * (recenter^-1 * raw): the view turns by +snap (left).
    Pose turn;
    turn.orientation = QuatFromAxisAngle(Vec3{0, 1, 0}, -snap);
    return PoseMultiply(recenter_, turn);
}

// ---- pose validity ----

namespace {

bool Finite(const Quat& q) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return n > 0.81f && n < 1.21f;  // a unit quaternion (runtimes may report zeros when invalid)
}
bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < 1e4f && std::fabs(v.y) < 1e4f && std::fabs(v.z) < 1e4f; }
bool Finite(const Fov& f) {
    return std::isfinite(f.angleLeft) && std::isfinite(f.angleRight) && std::isfinite(f.angleUp) && std::isfinite(f.angleDown) &&
           f.angleRight > f.angleLeft && f.angleUp > f.angleDown;
}
std::string PoseText(const Pose& p) {
    return std::format("yaw {:.1f} deg, position ({:.3f}, {:.3f}, {:.3f}) m", QuatYaw(p.orientation) * kRadToDeg, p.position.x, p.position.y,
                       p.position.z);
}
const char* TrackStateName(int s) {
    switch (s) {
        case 0: return "tracked";
        case 1: return "orientation only (3DoF): position held";
        case 2: return "not tracked: last views held";
        default: return "not seen yet";
    }
}

}  // namespace

void BackendBase::SanitizePoses(FrameRecord& r, bool viewO, bool viewP, bool headO, bool headP) {
    PoseTrack& t = track_;
    // The orientation counts only if the views and the head agree it is valid and every
    // value is a number; the position only on top of a valid orientation.
    bool o = viewO && headO && Finite(r.rawHead.orientation);
    for (int e = 0; e < 2 && o; ++e) o = Finite(r.raw[e].pose.orientation);
    bool p = o && viewP && headP && Finite(r.rawHead.position);
    for (int e = 0; e < 2 && p; ++e) p = Finite(r.raw[e].pose.position);

    // Field of view: from the runtime when it is sane, else the last one, else 45 degrees each way.
    for (int e = 0; e < 2; ++e) {
        if (Finite(r.raw[e].fov)) continue;
        r.raw[e].fov = t.haveUsed ? t.lastRaw[e].fov : Fov{-0.785f, 0.785f, 0.785f, -0.785f};
    }

    if (o && p) {
        t.haveFull = true;
        t.fullHeadPosition = r.rawHead.position;
        const Pose inv = PoseInverse(r.rawHead);
        for (int e = 0; e < 2; ++e) t.eyeInHead[e] = PoseMultiply(inv, r.raw[e].pose);
    } else if (o) {
        // 3DoF: the runtime's orientation at the last fully tracked head position.
        r.rawHead.position = t.haveFull ? t.fullHeadPosition : Vec3{};
        for (int e = 0; e < 2; ++e) {
            Vec3 offset = t.haveFull ? t.eyeInHead[e].position : Vec3{e == 0 ? -0.032f : 0.032f, 0.0f, 0.0f};
            const Vec3 d = QuatRotate(r.rawHead.orientation, offset);
            r.raw[e].pose.position = Vec3{r.rawHead.position.x + d.x, r.rawHead.position.y + d.y, r.rawHead.position.z + d.z};
        }
    } else if (t.haveUsed) {
        r.raw[0] = t.lastRaw[0];
        r.raw[1] = t.lastRaw[1];
        r.rawHead = t.lastHead;
    } else {
        // Nothing seen yet: a neutral head at the origin looking ahead.
        r.rawHead = Pose{};
        for (int e = 0; e < 2; ++e) {
            r.raw[e].pose = Pose{};
            r.raw[e].pose.position.x = e == 0 ? -0.032f : 0.032f;
        }
    }
    if (o) {
        t.haveUsed = true;
        t.lastRaw[0] = r.raw[0];
        t.lastRaw[1] = r.raw[1];
        t.lastHead = r.rawHead;
    }
    r.orientationValid = o;
    r.positionValid = p;
    r.posesUsable = o || t.haveUsed;

    const int state = o && p ? 0 : o ? 1 : 2;
    if (state != t.state) {
        if (t.state != -1) ++t.transitions;
        const uint64_t frames = r.id - t.stateSince;
        if (t.state != -1 && (t.transitions <= 32 || (t.transitions & (t.transitions - 1)) == 0)) {
            const std::string detail = state == 1 ? std::format(" at ({:.3f}, {:.3f}, {:.3f}) m", r.rawHead.position.x, r.rawHead.position.y,
                                                                r.rawHead.position.z)
                                       : state == 2 ? (t.haveUsed ? std::string(" (") + PoseText(r.rawHead) + ")" : std::string(" (none yet: neutral pose)"))
                                                    : std::string();
            const auto level = state == 0 ? LogLevel::Info : LogLevel::Warn;
            log_.Write(level, std::format("tracking: {}{} from frame {} (after {} frames {}; runtime flags views o{} p{} head o{} p{}; change {})",
                                          TrackStateName(state), detail, r.id, frames, TrackStateName(t.state), viewO ? 1 : 0, viewP ? 1 : 0,
                                          headO ? 1 : 0, headP ? 1 : 0, t.transitions));
        } else if (t.state == -1 && state != 0) {
            log_.Warn("tracking: first frame {}: {}", r.id, TrackStateName(state));
        }
        t.state = state;
        t.stateSince = r.id;
    }
}

// ---- runtime recenter (LOCAL space change) ----

void BackendBase::NoteLocalSpaceChange(int64_t changeTime, bool poseValid, const Pose& pose, int64_t now) {
    const bool replaced = spaceChange_.pending;
    spaceChange_.pending = true;
    spaceChange_.changeTime = changeTime;
    spaceChange_.poseValid = poseValid;
    spaceChange_.pose = pose;
    spaceChange_.receivedNs = now;
    ++spaceChange_.count;
    log_.Debug("LOCAL space change pending (change time {}, pose {}{})", changeTime, poseValid ? PoseText(pose) : std::string("unknown"),
               replaced ? ", replaces one not applied yet" : "");
}

bool BackendBase::ApplySpaceChange(int64_t displayTime, uint64_t frameId, const Pose& rawHead, bool headValid) {
    if (!spaceChange_.pending || displayTime < spaceChange_.changeTime) return false;
    spaceChange_.pending = false;
    const Pose old = recenter_;
    // The runtime moved the LOCAL origin to the user's leveled head (OpenXR: "the
    // current leveled head space becomes the new LOCAL space"), so the user now faces
    // the origin's forward. This library's recenter was taken against the old origin:
    // applied on top of the new one it would turn and shift the view away again.
    // Recomposing it with poseInPreviousSpace would keep the old forward direction,
    // which undoes the user's recenter; it is cleared instead. World-locked quads
    // (screen, UI) are placed relative to it, so they come back in front of the user.
    recenter_ = Pose{};
    snapYaw_.store(0.0f, std::memory_order_release);
    snapRequest_.store(0.0f, std::memory_order_release);
    if (spaceChange_.poseValid) {
        // Poses the filter may still repeat (held or 3DoF frames) move into the new space.
        const Pose inv = PoseInverse(spaceChange_.pose);
        for (int e = 0; e < 2; ++e) track_.lastRaw[e].pose = PoseMultiply(inv, track_.lastRaw[e].pose);
        track_.lastHead = PoseMultiply(inv, track_.lastHead);
        Pose full;
        full.position = track_.fullHeadPosition;
        track_.fullHeadPosition = PoseMultiply(inv, full).position;
    }
    const double ms = double(QpcNowNs() - spaceChange_.receivedNs) / 1e6;
    log_.Info("runtime recentred its LOCAL space (event {} of this session, applied at frame {}, {:.0f} ms after the event; pose of the new origin in the "
              "old space {}): recenter offset was {} -> cleared; head in the new space {}{}",
              spaceChange_.count, frameId, ms, spaceChange_.poseValid ? PoseText(spaceChange_.pose) : std::string("not given by the runtime"),
              PoseText(old), PoseText(rawHead), headValid ? "" : " (not tracked: held pose)");
    return true;
}

Pose BackendBase::ApplyRecenter(const Pose& recenter, const Pose& raw) { return PoseMultiply(PoseInverse(recenter), raw); }

View BackendBase::ApplyRecenter(const Pose& recenter, const View& raw) {
    View v = raw;
    v.pose = ApplyRecenter(recenter, raw.pose);
    return v;
}

View BackendBase::RemoveRecenter(const Pose& recenter, const View& v) {
    View r = v;
    r.pose = PoseMultiply(recenter, v.pose);
    return r;
}

void BackendBase::FillFrameInfo(const FrameRecord& r, FrameInfo& info) const {
    info.frameId = r.id;
    info.sessionRunning = true;
    info.shouldRender = r.shouldRender;
    info.predictedDisplayTime = r.displayTime;
    info.predictedDisplayPeriod = r.period;
    info.orientationValid = r.orientationValid;
    info.positionValid = r.positionValid;
    for (int e = 0; e < 2; ++e) info.views[e] = ApplyRecenter(r.recenter, r.raw[e]);
    info.head = ApplyRecenter(r.recenter, r.rawHead);
    info.state = state_.load();
}

Rect BackendBase::EyeRect(const SubmitDesc& desc, Eye eye) {
    Rect r = desc.eyes[static_cast<int>(eye)].rect;
    if (r.width == 0 || r.height == 0) {
        D3D11_TEXTURE2D_DESC d{};
        desc.texture->GetDesc(&d);
        const uint32_t w = std::max(1u, d.Width >> desc.mipLevel);
        const uint32_t h = std::max(1u, d.Height >> desc.mipLevel);
        r.width = w / 2;
        r.height = h;
        r.x = eye == Eye::Left ? 0 : static_cast<int32_t>(w / 2);
        r.y = 0;
    }
    return r;
}

Rect BackendBase::QuadRect(const QuadLayer& q) {
    Rect r = q.rect;
    if (r.width == 0 || r.height == 0) {
        D3D11_TEXTURE2D_DESC d{};
        q.texture->GetDesc(&d);
        r = Rect{0, 0, std::max(1u, d.Width >> q.mipLevel), std::max(1u, d.Height >> q.mipLevel)};
    }
    return r;
}

static void CountPath(std::mutex& m, FrameStats& s, Blitter::Path path) {
    std::lock_guard lk(m);
    if (path == Blitter::Path::Copy) ++s.copyPath;
    if (path == Blitter::Path::Blit) ++s.blitPath;
}

bool BackendBase::TransferEye(Eye eye, const SubmitDesc& desc, const EyeTarget& t, uint32_t* outW, uint32_t* outH) {
    BlitSource src;
    src.texture = desc.texture;
    src.viewFormat = desc.viewFormat;
    src.encoding = desc.encoding;
    src.arraySlice = desc.arraySlice;
    src.mipLevel = desc.mipLevel;
    src.picture = &desc.picture;
    if (!desc.vignette.IsNone()) {
        const int e = eye == Eye::Left ? 0 : 1;
        src.vignette[0] = desc.vignette.strength;
        src.vignette[1] = desc.vignette.radius;
        src.vignette[2] = desc.vignette.softness;
        src.vignetteCentre[0] = desc.vignette.centre[e][0];
        src.vignetteCentre[1] = desc.vignette.centre[e][1];
    }
    src.sharpen = desc.sharpen;
    BlitDest dst{t.texture, t.viewFormat, t.width, t.height, t.arraySlice};
    Blitter::Path path{};
    if (gpuTiming_) gpuCopy_.Before(context_.Get());
    const bool ok = blitter_.Transfer(context_.Get(), src, EyeRect(desc, eye), dst, &path, outW, outH);
    if (gpuTiming_) gpuCopy_.After(context_.Get());
    CountPath(statsMutex_, stats_, path);
    return ok;
}

bool BackendBase::TransferQuad(const SubmitDesc& desc, const QuadLayer& q, const EyeTarget& t, uint32_t* outW, uint32_t* outH) {
    BlitSource src;
    src.texture = q.texture;
    src.viewFormat = q.viewFormat;
    src.encoding = q.encoding;
    src.arraySlice = q.arraySlice;
    src.mipLevel = q.mipLevel;
    // The layer image holds premultiplied alpha (see SourceAlpha).
    src.alpha = !q.alphaBlend                                         ? BlitAlpha::Opaque
                : q.sourceAlpha == SourceAlpha::Straight              ? BlitAlpha::Straight
                : q.sourceAlpha == SourceAlpha::PremultipliedInverted ? BlitAlpha::PremultipliedInverted
                                                                      : BlitAlpha::Premultiplied;
    if (q.adjustPicture && !q.alphaBlend) src.picture = &desc.picture;
    BlitDest dst{t.texture, t.viewFormat, t.width, t.height, t.arraySlice};
    Blitter::Path path{};
    if (gpuTiming_) gpuCopy_.Before(context_.Get());
    const bool ok = blitter_.Transfer(context_.Get(), src, QuadRect(q), dst, &path, outW, outH);
    if (gpuTiming_) gpuCopy_.After(context_.Get());
    CountPath(statsMutex_, stats_, path);
    if (ok) CountStat(&FrameStats::quadUpdates);
    return ok;
}

void BackendBase::CaptureComposited(const FrameRecord& rec, const SubmitDesc& desc, const LayerImage projection[2], uint32_t eyeW,
                                    uint32_t eyeH) {
    if (!capture_.Active() || eyeW == 0 || eyeH == 0) return;
    if (capture_.WantsRaw()) {
        // The bytes the runtime is handed, before any view conversion: each eye's
        // swapchain image, each quad's image, and the frame's source texture.
        ID3D11DeviceContext* ctx = context_.Get();
        capture_.CaptureRaw(ctx, projection[0].texture, 0, "_rawL.png");
        capture_.CaptureRaw(ctx, projection[1].texture, 0, "_rawR.png");
        for (uint32_t i = 0; i < desc.quadCount; ++i) {
            const QuadSlot* s = FindQuad(desc.quads[i].layer);
            if (s && s->sc.hasImage) capture_.CaptureRaw(ctx, s->sc.Last().texture, 0, std::format("_rawquad{}.png", i));
        }
        if (desc.texture) capture_.CaptureRaw(ctx, desc.texture, desc.arraySlice, "_src.png");
    }
    constexpr DXGI_FORMAT kViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const float black[4] = {0, 0, 0, 1};
    // The compose targets are eye-sized (78 MB each at 4608x4224) and only needed
    // for this frame: released on every way out, created again by the next capture.
    struct ReleaseComposeTargets {
        BackendBase* self;
        ~ReleaseComposeTargets() {
            for (int e = 0; e < 2; ++e) {
                if (self->composeTarget_[e]) self->compositor_.Forget(self->composeTarget_[e].Get());
                self->composeTarget_[e].Reset();
                self->composeW_[e] = self->composeH_[e] = 0;
            }
        }
    } releaseComposeTargets{this};
    for (int e = 0; e < 2; ++e) {
        if (!composeTarget_[e] || composeW_[e] != eyeW || composeH_[e] != eyeH) {
            if (composeTarget_[e]) compositor_.Forget(composeTarget_[e].Get());
            composeTarget_[e].Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = eyeW;
            d.Height = eyeH;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            const HRESULT hr = device_->CreateTexture2D(&d, nullptr, &composeTarget_[e]);
            if (FAILED(hr)) {
                log_.Error("capture: compose target {}x{} failed {}", eyeW, eyeH, HResultString(hr));
                return;
            }
            composeW_[e] = eyeW;
            composeH_[e] = eyeH;
        }
        ID3D11DeviceContext* ctx = context_.Get();
        if (!compositor_.Begin(ctx, composeTarget_[e].Get(), kViewFormat, eyeW, eyeH, black)) return;
        if (projection[e].texture) compositor_.DrawFullView(ctx, projection[e]);
        for (uint32_t i = 0; i < desc.quadCount; ++i) {
            const QuadLayer& q = desc.quads[i];
            const QuadSlot* s = FindQuad(q.layer);
            if (!s || !s->sc.hasImage) continue;
            compositor_.DrawQuad(ctx, s->sc.Last(), rec.raw[e], QuadPoseLocal(rec, q), q.width, q.height, q.alphaBlend);
        }
        compositor_.End(ctx);
        capture_.CaptureEye(ctx, static_cast<Eye>(e), composeTarget_[e].Get(), kViewFormat, eyeW, eyeH);
    }
}

// ---- quad slots ----

LayerHandle BackendBase::MakeHandle(int index) const { return ((layerSerial_ & 0xFFFFFFu) << 8) | static_cast<uint32_t>(index + 1); }

int BackendBase::FreeQuadSlotLocked() const {
    for (size_t i = 0; i < quads_.size(); ++i)
        if (!quads_[i].used) return static_cast<int>(i);
    return -1;
}

BackendBase::QuadSlot* BackendBase::FindQuad(LayerHandle h) {
    const uint32_t idx = (h & 0xFFu);
    if (idx == 0 || idx > quads_.size()) return nullptr;
    QuadSlot& s = quads_[idx - 1];
    if (!s.used || h != MakeHandle(static_cast<int>(idx - 1))) return nullptr;
    return &s;
}

const BackendBase::QuadSlot* BackendBase::FindQuad(LayerHandle h) const { return const_cast<BackendBase*>(this)->FindQuad(h); }

bool BackendBase::DrawOverlay(const QuadLayer& q, ID3D11Texture2D* target, DXGI_FORMAT targetFormat, ColorEncoding targetEncoding,
                              const Rect& targetRect) {
    if (!context_ || !q.texture || !target || targetRect.width == 0 || targetRect.height == 0) return false;
    BlitSource src;
    src.texture = q.texture;
    src.viewFormat = q.viewFormat;
    src.encoding = q.encoding;
    src.arraySlice = q.arraySlice;
    src.mipLevel = q.mipLevel;
    src.alpha = q.sourceAlpha == SourceAlpha::Straight              ? BlitAlpha::Straight
                : q.sourceAlpha == SourceAlpha::PremultipliedInverted ? BlitAlpha::PremultipliedInverted
                                                                      : BlitAlpha::Premultiplied;
    BlitDest dst;
    dst.texture = target;
    dst.viewFormat = targetFormat;
    dst.x = targetRect.x;
    dst.y = targetRect.y;
    dst.width = targetRect.width;
    dst.height = targetRect.height;
    dst.encoding = targetEncoding;
    D3D11StateBackup state;
    state.Save(context_.Get());
    const bool ok = blitter_.BlendOver(context_.Get(), src, q.rect, dst);
    state.Restore(context_.Get());
    // Views of the target are cached by the blitter; a swap chain's back buffer must not stay referenced.
    blitter_.Forget(target);
    return ok;
}

bool BackendBase::GetQuadLayerInfo(LayerHandle layer, SwapchainInfo* out) const {
    std::lock_guard lk(quadMutex_);
    const QuadSlot* s = FindQuad(layer);
    if (!s) return false;
    if (out) *out = SwapchainInfo{s->sc.width, s->sc.height, s->sc.format, static_cast<uint32_t>(s->sc.owned ? 1 : s->sc.images.size())};
    return true;
}

}  // namespace ff7vr::xr
