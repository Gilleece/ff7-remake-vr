#include "ui_align.h"

#include "stereo_device.h"
#include "ue_math.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/pattern.h"

#if FF7VR_ENGINE_WITH_RENDER
#include "ff7vr/render/render.h"
#endif

#include <atomic>
#include <cmath>
#include <format>
#include <mutex>

namespace ff7vr::engine::ui_align {
namespace {

using namespace ue;

// bool ULocalPlayer::GetProjectionData(FViewport*, EStereoscopicPass, FSceneViewProjectionData&) const
// (tools/re/signatures.json; docs/re/engine.md section 5).
constexpr const char* kPattern = "40 55 53 56 57 41 54 41 56 48 8D AC 24 38 F8 FF FF";
constexpr std::uintptr_t kExpectRva = 0x3019240;
// FSceneViewProjectionData in this build: ViewOrigin +0, ViewRotationMatrix +0x10,
// ProjectionMatrix +0x50 (docs/re/engine.md section 5).
constexpr std::size_t kViewOrigin = 0x00, kViewRotation = 0x10, kProjection = 0x50;

using GetProjectionDataFn = bool (*)(const void*, void*, std::int32_t, std::uint8_t*);
hook::InlineHook g_hook;
std::atomic<bool> g_ready{false};

struct Base {
    bool valid = false;
    FRotator game_rot{};
    FVector game_loc{};
    float game_fov = 0;
    FRotator base_level{};
    FVector base_loc{};
    HostPose head{};
    double upm = 100.0;
    bool positional = true;
};
std::mutex g_mutex;
Base g_base;

// For `uihook align`.
std::atomic<std::uint64_t> g_full{0}, g_aligned{0}, g_skipped{0};
struct Last {
    float l = 0, r = 0, t = 0, b = 0;  // panel tangents
    float fov = 0, pitch = 0;          // game camera
    double offset_cm = 0;              // projection origin from the game camera
    int mode = -1;
};
Last g_last;

math::Vec vec(const FVector& v) { return math::Vec{v.X, v.Y, v.Z}; }
math::Vec sub(const math::Vec& a, const math::Vec& b) { return math::Vec{a.x - b.x, a.y - b.y, a.z - b.z}; }
math::Vec axis_combo(const math::Vec& a, const math::Vec& fwd, const math::Vec& right, const math::Vec& up) {
    return math::Vec{a.x * fwd.x + a.y * right.x + a.z * up.x, a.x * fwd.y + a.y * right.y + a.z * up.y,
                     a.x * fwd.z + a.y * right.z + a.z * up.z};
}

#if FF7VR_ENGINE_WITH_RENDER
// Replaces the full pass's view with the panel's (see the header).
void align(std::uint8_t* data) {
    render::UiPanelPlacement p;
    if (!render::UiPanel(&p) || p.align != 1 || !device::active()) {
        ++g_skipped;
        return;
    }
    Base b;
    {
        std::lock_guard lk(g_mutex);
        b = g_base;
    }
    if (!b.valid || !(p.distance > 0.0f) || !(p.width > 0.0f) || !(p.height > 0.0f)) {
        ++g_skipped;
        return;
    }
    // The projection's frame in the world at the last stereo frame: its origin is the head
    // (the midpoint of the eyes) mapped through the eye base as the eyes are
    // (math::compose_eye); its axes are the panel's. A head-locked panel turns with the
    // head; a world-locked one keeps the eye base's axes, and the head's offset from the
    // headset's origin makes the frustum off-centre, so a marker stays on the line from the
    // head to its object when the head leans.
    math::Quat qp = math::quat_from_rotator(b.base_level);
    math::Vec pp = vec(b.base_loc);
    if (b.positional) pp = math::add(pp, math::rotate(qp, math::scale(math::xr_to_ue_position(b.head.position), b.upm)));
    double hx = 0, hy = 0, hz = 0;  // the head in the panel's space (headset's space, metres)
    if (p.headLocked) {
        qp = math::normalize(math::mul(qp, math::xr_to_ue_quat(b.head.orientation)));
    } else {
        hx = b.head.position.x;
        hy = b.head.position.y;
        hz = b.head.position.z;
    }
    // ... relative to the game camera of that frame ...
    const math::Quat qgi = math::conjugate(math::quat_from_rotator(b.game_rot));
    const math::Quat q_rel = math::normalize(math::mul(qgi, qp));
    const math::Vec p_rel = math::rotate(qgi, sub(pp, vec(b.game_loc)));
    // ... applied to the game camera of this call (axes from its view rotation matrix:
    // columns 0, 1, 2 of the upper 3x3 are right, up and forward).
    float* vr = reinterpret_cast<float*>(data + kViewRotation);
    const math::Vec right{vr[0], vr[4], vr[8]}, up{vr[1], vr[5], vr[9]}, fwd{vr[2], vr[6], vr[10]};
    const float* origin = reinterpret_cast<const float*>(data + kViewOrigin);
    const math::Vec cam{origin[0], origin[1], origin[2]};
    const math::Vec pf = axis_combo(math::rotate(q_rel, math::Vec{1, 0, 0}), fwd, right, up);
    const math::Vec pr = axis_combo(math::rotate(q_rel, math::Vec{0, 1, 0}), fwd, right, up);
    const math::Vec pu = axis_combo(math::rotate(q_rel, math::Vec{0, 0, 1}), fwd, right, up);
    const math::Vec po = math::add(cam, axis_combo(p_rel, fwd, right, up));
    for (const double v : {pf.x, pf.y, pf.z, pr.x, pr.y, pr.z, pu.x, pu.y, pu.z, po.x, po.y, po.z})
        if (!std::isfinite(v)) {
            ++g_skipped;
            return;
        }
    // The panel's edges as tangents from the head (left and down negative).
    const double dist = p.distance + hz;  // the panel's plane is at z = -distance
    if (!(dist > 0.05)) {
        ++g_skipped;
        return;
    }
    const double l = (p.offsetX - p.width * 0.5 - hx) / dist, r = (p.offsetX + p.width * 0.5 - hx) / dist;
    const double t = (p.offsetY + p.height * 0.5 - hy) / dist, bt = (p.offsetY - p.height * 0.5 - hy) / dist;
    float* proj = reinterpret_cast<float*>(data + kProjection);
    const float near_plane = proj[14] > 0.0f && std::isfinite(proj[14]) ? proj[14] : 10.0f;
    HostFov fov;
    fov.angleLeft = static_cast<float>(std::atan(l));
    fov.angleRight = static_cast<float>(std::atan(r));
    fov.angleUp = static_cast<float>(std::atan(t));
    fov.angleDown = static_cast<float>(std::atan(bt));
    FMatrix m{};
    math::stereo_projection(fov, near_plane, m);

    float* o = reinterpret_cast<float*>(data + kViewOrigin);
    o[0] = static_cast<float>(po.x);
    o[1] = static_cast<float>(po.y);
    o[2] = static_cast<float>(po.z);
    // FInverseRotationMatrix(rotation) * the view planes: column 0 right, 1 up, 2 forward.
    const double cols[3][3] = {{pr.x, pr.y, pr.z}, {pu.x, pu.y, pu.z}, {pf.x, pf.y, pf.z}};
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 3; ++c) vr[i * 4 + c] = static_cast<float>(cols[c][i]);
        vr[i * 4 + 3] = 0.0f;
    }
    vr[12] = vr[13] = vr[14] = 0.0f;
    vr[15] = 1.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) proj[i * 4 + j] = m.M[i][j];
    ++g_aligned;
    {
        std::lock_guard lk(g_mutex);
        g_last.l = static_cast<float>(l);
        g_last.r = static_cast<float>(r);
        g_last.t = static_cast<float>(t);
        g_last.b = static_cast<float>(bt);
        g_last.fov = b.game_fov;
        g_last.pitch = b.game_rot.Pitch;
        const math::Vec d = sub(po, cam);
        g_last.offset_cm = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        g_last.mode = p.align;
    }
}
#endif

bool detour(const void* local_player, void* viewport, std::int32_t pass, std::uint8_t* data) {
    const bool ok = g_hook.original<GetProjectionDataFn>()(local_player, viewport, pass, data);
#if FF7VR_ENGINE_WITH_RENDER
    if (ok && pass == 0 && data) {
        ++g_full;
        align(data);
    }
#endif
    return ok;
}

}  // namespace

void start(std::uintptr_t base, bool known) {
    const auto r = pattern::scan_module(base, kPattern, pattern::Sections::Executable, 2);
    if (!r.unique()) {
        log::warn("ui: ULocalPlayer::GetProjectionData: {} matches; UI markers are not aligned with the panel ([ui] align)", r.matches.size());
        return;
    }
    const std::uintptr_t rva = r.first() - base;
    if (known && rva != kExpectRva) {
        log::warn("ui: ULocalPlayer::GetProjectionData at +0x{:x}, expected +0x{:x} for this build; UI markers are not aligned", rva, kExpectRva);
        return;
    }
    if (!g_hook.create(reinterpret_cast<void*>(r.first()), &detour)) {
        log::warn("ui: GetProjectionData hook could not be installed; UI markers are not aligned");
        return;
    }
    g_ready = true;
    log::info("ui: marker alignment hook installed (ULocalPlayer::GetProjectionData +0x{:x})", rva);
}

void publish(const FRotator& game_rot, const FVector& game_loc, float game_fov_deg, const FRotator& base_level, const FVector& base_loc,
             const HostPose& head, double units_per_metre, bool positional) {
    {
        std::lock_guard lk(g_mutex);
        g_base.valid = true;
        g_base.game_rot = game_rot;
        g_base.game_loc = game_loc;
        g_base.game_fov = game_fov_deg;
        g_base.base_level = base_level;
        g_base.base_loc = base_loc;
        g_base.head = head;
        g_base.upm = units_per_metre;
        g_base.positional = positional;
    }
#if FF7VR_ENGINE_WITH_RENDER
    render::SetGameFov(game_fov_deg);
#endif
}

Snapshot snapshot() {
    std::lock_guard lk(g_mutex);
    Snapshot r;
    r.valid = g_base.valid;
    r.game_rot = g_base.game_rot;
    r.game_loc = g_base.game_loc;
    r.base_level = g_base.base_level;
    r.base_loc = g_base.base_loc;
    r.head = g_base.head;
    r.upm = g_base.upm;
    r.positional = g_base.positional;
    r.align_mode = g_last.mode;
    r.full = g_full.load();
    r.aligned = g_aligned.load();
    r.skipped = g_skipped.load();
    return r;
}

std::string status() {
    std::lock_guard lk(g_mutex);
    const double k = 180.0 / 3.14159265358979323846;
    const Base& b = g_base;
    return std::format("ok marker alignment hook {}; full-pass projections {} aligned {} passed through {}; game camera FOV {:.2f} deg pitch {:.2f} "
                       "deg; last panel {:.2f} to {:.2f} deg wide, {:.2f} to {:.2f} deg high, origin {:.1f} cm from the game camera; "
                       "camera rot {:.4f} {:.4f} {:.4f} loc {:.2f} {:.2f} {:.2f}; eye base rot {:.4f} {:.4f} {:.4f} loc {:.2f} {:.2f} {:.2f}; "
                       "head pos {:.4f} {:.4f} {:.4f} quat {:.4f} {:.4f} {:.4f} {:.4f}; units per metre {:.1f}",
                       g_ready.load() ? "installed" : "off", g_full.load(), g_aligned.load(), g_skipped.load(), b.game_fov, b.game_rot.Pitch,
                       std::atan(g_last.l) * k, std::atan(g_last.r) * k, std::atan(g_last.b) * k, std::atan(g_last.t) * k, g_last.offset_cm,
                       b.game_rot.Pitch, b.game_rot.Yaw, b.game_rot.Roll, b.game_loc.X, b.game_loc.Y, b.game_loc.Z, b.base_level.Pitch,
                       b.base_level.Yaw, b.base_level.Roll, b.base_loc.X, b.base_loc.Y, b.base_loc.Z, b.head.position.x, b.head.position.y,
                       b.head.position.z, b.head.orientation.x, b.head.orientation.y, b.head.orientation.z, b.head.orientation.w, b.upm);
}

}  // namespace ff7vr::engine::ui_align
