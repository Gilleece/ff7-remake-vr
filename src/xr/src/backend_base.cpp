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
    return PoseMultiply(r.recenter, q.pose);  // undo the recenter: tracking space after recenter -> LOCAL
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
    {
        std::lock_guard lk(statsMutex_);
        stats_ = FrameStats{};
    }
    return Result::Ok;
}

void BackendBase::ShutdownCommon() {
    capture_.Shutdown();
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
    BlitDest dst{t.texture, t.viewFormat, t.width, t.height, t.arraySlice};
    Blitter::Path path{};
    const bool ok = blitter_.Transfer(context_.Get(), src, EyeRect(desc, eye), dst, &path, outW, outH);
    CountPath(statsMutex_, stats_, path);
    return ok;
}

bool BackendBase::TransferQuad(const QuadLayer& q, const EyeTarget& t, uint32_t* outW, uint32_t* outH) {
    BlitSource src;
    src.texture = q.texture;
    src.viewFormat = q.viewFormat;
    src.encoding = q.encoding;
    src.arraySlice = q.arraySlice;
    src.mipLevel = q.mipLevel;
    BlitDest dst{t.texture, t.viewFormat, t.width, t.height, t.arraySlice};
    Blitter::Path path{};
    const bool ok = blitter_.Transfer(context_.Get(), src, QuadRect(q), dst, &path, outW, outH);
    CountPath(statsMutex_, stats_, path);
    if (ok) CountStat(&FrameStats::quadUpdates);
    return ok;
}

void BackendBase::CaptureComposited(const FrameRecord& rec, const SubmitDesc& desc, const LayerImage projection[2], uint32_t eyeW,
                                    uint32_t eyeH) {
    if (!capture_.Active() || eyeW == 0 || eyeH == 0) return;
    constexpr DXGI_FORMAT kViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const float black[4] = {0, 0, 0, 1};
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

bool BackendBase::GetQuadLayerInfo(LayerHandle layer, SwapchainInfo* out) const {
    std::lock_guard lk(quadMutex_);
    const QuadSlot* s = FindQuad(layer);
    if (!s) return false;
    if (out) *out = SwapchainInfo{s->sc.width, s->sc.height, s->sc.format, static_cast<uint32_t>(s->sc.owned ? 1 : s->sc.images.size())};
    return true;
}

}  // namespace ff7vr::xr
