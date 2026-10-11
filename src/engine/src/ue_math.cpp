#include "ue_math.h"

#include <algorithm>
#include <cmath>

namespace ff7vr::engine::math {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

// FRotator::NormalizeAxis: (-180, 180]
double normalize_axis(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0.0) a += 360.0;
    if (a > 180.0) a -= 360.0;
    return a;
}
}  // namespace

Quat quat_from_rotator(const ue::FRotator& r) {
    // FRotator::Quaternion (UE 4.18)
    const double half = kDegToRad / 2.0;
    const double sp = std::sin(r.Pitch * half), cp = std::cos(r.Pitch * half);
    const double sy = std::sin(r.Yaw * half), cy = std::cos(r.Yaw * half);
    const double sr = std::sin(r.Roll * half), cr = std::cos(r.Roll * half);
    Quat q;
    q.x = cr * sp * sy - sr * cp * cy;
    q.y = -cr * sp * cy - sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    q.w = cr * cp * cy + sr * sp * sy;
    return q;
}

ue::FRotator rotator_from_quat(const Quat& q) {
    // FQuat::Rotator (UE 4.18)
    const double singularity = q.z * q.x - q.w * q.y;
    const double yaw_y = 2.0 * (q.w * q.z + q.x * q.y);
    const double yaw_x = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
    constexpr double kThreshold = 0.4999995;
    ue::FRotator r{};
    if (singularity < -kThreshold) {
        r.Pitch = -90.0f;
        r.Yaw = static_cast<float>(std::atan2(yaw_y, yaw_x) * kRadToDeg);
        r.Roll = static_cast<float>(normalize_axis(-r.Yaw - 2.0 * std::atan2(q.x, q.w) * kRadToDeg));
    } else if (singularity > kThreshold) {
        r.Pitch = 90.0f;
        r.Yaw = static_cast<float>(std::atan2(yaw_y, yaw_x) * kRadToDeg);
        r.Roll = static_cast<float>(normalize_axis(r.Yaw - 2.0 * std::atan2(q.x, q.w) * kRadToDeg));
    } else {
        double s = 2.0 * singularity;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        r.Pitch = static_cast<float>(std::asin(s) * kRadToDeg);
        r.Yaw = static_cast<float>(std::atan2(yaw_y, yaw_x) * kRadToDeg);
        r.Roll = static_cast<float>(
            std::atan2(-2.0 * (q.w * q.x + q.y * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)) * kRadToDeg);
    }
    return r;
}

Quat mul(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat conjugate(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

Quat normalize(const Quat& q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-12) return Quat{};
    return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
}

Vec rotate(const Quat& q, const Vec& v) {
    // v' = q v q*, written as v + 2w(u x v) + 2u x (u x v)
    const Vec u{q.x, q.y, q.z};
    const Vec t{2.0 * (u.y * v.z - u.z * v.y), 2.0 * (u.z * v.x - u.x * v.z), 2.0 * (u.x * v.y - u.y * v.x)};
    return Vec{
        v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
        v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
        v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
    };
}

Vec add(const Vec& a, const Vec& b) { return Vec{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec scale(const Vec& v, double s) { return Vec{v.x * s, v.y * s, v.z * s}; }

Vec xr_to_ue_position(const HostVec3& p) { return Vec{-p.z, p.x, p.y}; }

Quat xr_to_ue_quat(const HostQuat& q) { return normalize(Quat{-q.z, q.x, q.y, -q.w}); }

HostFov centred_fov(double horizontal_fov_deg, double width, double height) {
    const double tx = std::tan(horizontal_fov_deg * 0.5 * 3.14159265358979323846 / 180.0);
    HostFov f;
    f.angleRight = static_cast<float>(std::atan(tx));
    f.angleLeft = -f.angleRight;
    f.angleUp = static_cast<float>(std::atan(tx * (width > 0 && height > 0 ? height / width : 1.0)));
    f.angleDown = -f.angleUp;
    return f;
}

void stereo_projection(const HostFov& fov, float near_plane, ue::FMatrix& out) {
    const double l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const double t = std::tan(fov.angleUp), b = std::tan(fov.angleDown);
    for (auto& row : out.M)
        for (float& v : row) v = 0.0f;
    out.M[0][0] = static_cast<float>(2.0 / (r - l));
    out.M[1][1] = static_cast<float>(2.0 / (t - b));
    out.M[2][0] = static_cast<float>((l + r) / (l - r));
    out.M[2][1] = static_cast<float>((t + b) / (b - t));
    out.M[2][3] = 1.0f;
    out.M[3][2] = near_plane;
}

void compose_eye(const EyeCameraInput& in, ue::FRotator& out_rotation, ue::FVector& out_location) {
    ue::FRotator base = in.camera_rotation;
    if (in.decouple_pitch) {
        base.Pitch = 0.0f;
        base.Roll = 0.0f;
    }
    const Quat q_cam = quat_from_rotator(base);
    const Quat q_eye = xr_to_ue_quat(in.eye.orientation);
    out_rotation = rotator_from_quat(normalize(mul(q_cam, q_eye)));

    Vec p_eye = xr_to_ue_position(in.eye.position);  // metres, tracking space
    if (!in.positional) {
        // Keep only the eye's offset from the head (the IPD), rotated with the head.
        p_eye = add(p_eye, scale(xr_to_ue_position(in.head.position), -1.0));
    }
    const Vec offset = rotate(q_cam, scale(p_eye, in.units_per_metre));
    out_location.X = static_cast<float>(in.camera_location.X + offset.x);
    out_location.Y = static_cast<float>(in.camera_location.Y + offset.y);
    out_location.Z = static_cast<float>(in.camera_location.Z + offset.z);
}

Vec level_boom(const ue::FRotator& camera_rotation, const ue::FVector& camera_location, const Vec& pivot) {
    const Quat q_cam = quat_from_rotator(ue::FRotator{camera_rotation.Pitch, camera_rotation.Yaw, 0.0f});
    const Quat q_yaw = quat_from_rotator(ue::FRotator{0.0f, camera_rotation.Yaw, 0.0f});
    const Vec v{camera_location.X - pivot.x, camera_location.Y - pivot.y, camera_location.Z - pivot.z};
    return add(pivot, rotate(q_yaw, rotate(conjugate(q_cam), v)));
}

bool clamp_to_view(double cx, double cy, double w, double x0, double y0, double x1, double y1, double inset, double& out_x,
                   double& out_y) {
    const double hw = (x1 - x0) * 0.5, hh = (y1 - y0) * 0.5;
    const double mx = x0 + hw, my = y0 + hh;
    // The point's offset from the centre in pixels, times w (the same direction, w > 0).
    const double dx = cx * hw, dy = -cy * hh;
    const double lx = hw - inset, ly = hh - inset;
    // How far out the point is, in units of w: inside the shrunk rectangle when s <= w.
    const double s = lx > 0.0 && ly > 0.0 ? std::max(std::fabs(dx) / lx, std::fabs(dy) / ly) : 0.0;
    if (!(w > 0.0) || !(s > w)) {
        out_x = w > 0.0 ? mx + dx / w : mx;
        out_y = w > 0.0 ? my + dy / w : my;
        return false;
    }
    out_x = mx + dx / s;
    out_y = my + dy / s;
    return true;
}

}  // namespace ff7vr::engine::math
