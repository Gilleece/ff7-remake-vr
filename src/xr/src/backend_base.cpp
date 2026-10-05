#include "backend_base.h"

#include <algorithm>

namespace ff7vr::xr {

std::unique_ptr<IXrBackend> CreateBackend(BackendType type) {
    switch (type) {
        case BackendType::Null: return CreateNullBackend();
        case BackendType::OpenXR: return CreateOpenXrBackend();
    }
    return nullptr;
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

bool BackendBase::ValidateSubmit(const SubmitDesc& desc) {
    if (!desc.texture) return true;
    if (desc.reserved) {
        log_.Error("SubmitFrame: SubmitDesc::reserved must be null");
        return false;
    }
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
        if (r.x < 0 || r.y < 0 || r.width == 0 || r.height == 0 || uint64_t(r.x) + r.width > w || uint64_t(r.y) + r.height > h) {
            log_.Error("SubmitFrame: eye {} rect ({},{} {}x{}) outside the {}x{} source", e, r.x, r.y, r.width, r.height, w, h);
            return false;
        }
    }
    return true;
}

RuntimeInfo BackendBase::GetRuntimeInfo() const {
    std::lock_guard lk(infoMutex_);
    return info_;
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
    capture_.Init(device_.Get(), &log_, &blitter_);
    {
        std::lock_guard lk(frameMutex_);
        ring_ = {};
        nextFrameId_ = 1;
        ++epoch_;
    }
    recenter_ = Pose{};
    recenterRequest_ = 0;
    {
        std::lock_guard lk(statsMutex_);
        stats_ = FrameStats{};
    }
    return Result::Ok;
}

void BackendBase::ShutdownCommon() {
    capture_.Shutdown();
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

Pose BackendBase::UpdateRecenter(const Pose& rawHead, bool headValid) {
    const int req = recenterRequest_.exchange(0, std::memory_order_acq_rel);
    if (req == 2) {
        recenter_ = Pose{};
        log_.Info("recenter reset");
    } else if (req == 1) {
        if (headValid) {
            const float yaw = QuatYaw(rawHead.orientation);
            recenter_.orientation = QuatFromAxisAngle(Vec3{0, 1, 0}, yaw);
            recenter_.position = rawHead.position;
            log_.Info("recentered: yaw {:.1f} deg, position ({:.3f}, {:.3f}, {:.3f}) m", yaw * kRadToDeg, rawHead.position.x,
                      rawHead.position.y, rawHead.position.z);
        } else {
            recenterRequest_.store(1);  // retry when tracking is valid
        }
    }
    return recenter_;
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

void BackendBase::FillFrameInfo(const FrameRecord& r, const Pose& rawHead, FrameInfo& info) const {
    info.frameId = r.id;
    info.sessionRunning = true;
    info.shouldRender = r.shouldRender;
    info.predictedDisplayTime = r.displayTime;
    info.predictedDisplayPeriod = r.period;
    info.orientationValid = r.orientationValid;
    info.positionValid = r.positionValid;
    for (int e = 0; e < 2; ++e) info.views[e] = ApplyRecenter(r.recenter, r.raw[e]);
    info.head = ApplyRecenter(r.recenter, rawHead);
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

bool BackendBase::TransferEye(Eye eye, const SubmitDesc& desc, const EyeTarget& t, uint32_t* outW, uint32_t* outH) {
    BlitSource src;
    src.texture = desc.texture;
    src.viewFormat = desc.viewFormat;
    src.encoding = desc.encoding;
    src.arraySlice = desc.arraySlice;
    src.mipLevel = desc.mipLevel;
    BlitDest dst;
    dst.texture = t.texture;
    dst.viewFormat = t.viewFormat;
    dst.width = t.width;
    dst.height = t.height;
    dst.arraySlice = t.arraySlice;
    Blitter::Path path{};
    const bool ok = blitter_.Transfer(context_.Get(), src, EyeRect(desc, eye), dst, &path, outW, outH);
    {
        std::lock_guard lk(statsMutex_);
        if (path == Blitter::Path::Copy) ++stats_.copyPath;
        if (path == Blitter::Path::Blit) ++stats_.blitPath;
    }
    if (ok) capture_.CaptureEye(context_.Get(), eye, t.texture, t.viewFormat, *outW, *outH);
    return ok;
}

}  // namespace ff7vr::xr
