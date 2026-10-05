#include "fixed_host.h"

#include "ff7vr/core/config.h"

#include <cmath>
#include <format>

namespace ff7vr::engine {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

HostQuat quat_about_y(double radians) {
    return HostQuat{0.0f, static_cast<float>(std::sin(radians / 2)), 0.0f, static_cast<float>(std::cos(radians / 2))};
}

// q * v * conj(q) for unit quaternions (OpenXR conventions, plain vector math).
HostVec3 rotate(const HostQuat& q, const HostVec3& v) {
    const double ux = q.x, uy = q.y, uz = q.z, w = q.w;
    const double tx = 2 * (uy * v.z - uz * v.y), ty = 2 * (uz * v.x - ux * v.z), tz = 2 * (ux * v.y - uy * v.x);
    return HostVec3{static_cast<float>(v.x + w * tx + (uy * tz - uz * ty)),
                    static_cast<float>(v.y + w * ty + (uz * tx - ux * tz)),
                    static_cast<float>(v.z + w * tz + (ux * ty - uy * tx))};
}
}  // namespace

const char* to_string(HeadMotion m) {
    switch (m) {
        case HeadMotion::Static: return "static";
        case HeadMotion::YawSweep: return "yaw";
        case HeadMotion::Sway: return "sway";
        case HeadMotion::YawAndSway: return "yawsway";
    }
    return "?";
}

bool parse_head_motion(const std::string& s, HeadMotion& out) {
    if (s == "static" || s == "none") out = HeadMotion::Static;
    else if (s == "yaw") out = HeadMotion::YawSweep;
    else if (s == "sway") out = HeadMotion::Sway;
    else if (s == "yawsway") out = HeadMotion::YawAndSway;
    else return false;
    return true;
}

FixedStereoHost::Options FixedStereoHost::from_config(const Config& cfg) {
    Options o;
    o.eye_width = static_cast<std::uint32_t>(cfg.get_int("stereo", "eye_width", 1280));
    o.eye_height = static_cast<std::uint32_t>(cfg.get_int("stereo", "eye_height", 1440));
    // Left eye FOV in degrees (left and down negative). Default: symmetric 90 x 90.
    o.fov_left.angleLeft = static_cast<float>(cfg.get_float("stereo", "fov_left_deg", -45.0) * kDegToRad);
    o.fov_left.angleRight = static_cast<float>(cfg.get_float("stereo", "fov_right_deg", 45.0) * kDegToRad);
    o.fov_left.angleUp = static_cast<float>(cfg.get_float("stereo", "fov_up_deg", 45.0) * kDegToRad);
    o.fov_left.angleDown = static_cast<float>(cfg.get_float("stereo", "fov_down_deg", -45.0) * kDegToRad);
    o.ipd_metres = static_cast<float>(cfg.get_float("stereo", "ipd_mm", 64.0) / 1000.0);
    HeadMotion m{};
    if (parse_head_motion(cfg.get_string("stereo", "head_motion", "static"), m)) o.motion = m;
    return o;
}

FixedStereoHost::FixedStereoHost(const Options& o) : opt_(o) {}

FixedStereoHost::Options FixedStereoHost::options() const {
    std::lock_guard lock(mutex_);
    return opt_;
}

void FixedStereoHost::set_options(const Options& o) {
    std::lock_guard lock(mutex_);
    opt_ = o;
}

std::string FixedStereoHost::describe() const {
    std::lock_guard lock(mutex_);
    const double r2d = 180.0 / kPi;
    return std::format("fixed host: {}x{} per eye, left eye FOV L{:.1f} R{:.1f} U{:.1f} D{:.1f} deg, IPD {:.1f} mm, motion {}",
                       opt_.eye_width, opt_.eye_height, opt_.fov_left.angleLeft * r2d, opt_.fov_left.angleRight * r2d,
                       opt_.fov_left.angleUp * r2d, opt_.fov_left.angleDown * r2d, opt_.ipd_metres * 1000.0,
                       to_string(opt_.motion));
}

bool FixedStereoHost::eye_render_size(std::uint32_t& width, std::uint32_t& height) {
    std::lock_guard lock(mutex_);
    width = opt_.eye_width;
    height = opt_.eye_height;
    return true;
}

void FixedStereoHost::begin_game_frame(bool stereo_wanted, GameFrame& out) {
    std::lock_guard lock(mutex_);
    out.stereo = stereo_wanted;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    HostPose head;
    const bool yaw = opt_.motion == HeadMotion::YawSweep || opt_.motion == HeadMotion::YawAndSway;
    const bool sway = opt_.motion == HeadMotion::Sway || opt_.motion == HeadMotion::YawAndSway;
    if (yaw) head.orientation = quat_about_y(30.0 * kDegToRad * std::sin(2 * kPi * t / 8.0));
    if (sway) {
        head.position.x = static_cast<float>(0.03 * std::sin(2 * kPi * t / 4.0));
        head.position.y = static_cast<float>(0.02 * std::sin(2 * kPi * t / 3.0));
        head.position.z = static_cast<float>(0.01 * std::sin(2 * kPi * t / 5.0));
    }
    const HostFov& l = opt_.fov_left;
    const HostFov fovs[2] = {l, HostFov{-l.angleRight, -l.angleLeft, l.angleUp, l.angleDown}};
    for (int e = 0; e < 2; ++e) {
        const HostVec3 local{(e == 0 ? -0.5f : 0.5f) * opt_.ipd_metres, 0.0f, 0.0f};
        const HostVec3 off = rotate(head.orientation, local);
        out.views[e].pose.orientation = head.orientation;
        out.views[e].pose.position = HostVec3{head.position.x + off.x, head.position.y + off.y, head.position.z + off.z};
        out.views[e].fov = fovs[e];
    }
    out.head = head;
    out.frame_id = ++frame_;
    out.views_valid = true;
}

}  // namespace ff7vr::engine
