// Unit tests for the engine module's camera and projection math. Exit code 0 = pass.

#include "ue_math.h"

#include <cmath>
#include <cstdio>

using namespace ff7vr::engine;
using namespace ff7vr::engine::math;

namespace {
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }
bool near_rot(const ue::FRotator& a, const ue::FRotator& b, double eps = 1e-2) {
    auto ang = [](double x, double y) {
        double d = std::fmod(x - y, 360.0);
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        return std::fabs(d);
    };
    return ang(a.Pitch, b.Pitch) <= eps && ang(a.Yaw, b.Yaw) <= eps && ang(a.Roll, b.Roll) <= eps;
}
constexpr double kDeg = 3.14159265358979323846 / 180.0;
HostQuat xr_yaw(double deg) {  // rotation about +Y (OpenXR up): positive turns the view to the left
    return HostQuat{0.0f, static_cast<float>(std::sin(deg * kDeg / 2)), 0.0f, static_cast<float>(std::cos(deg * kDeg / 2))};
}
HostQuat xr_pitch(double deg) {  // rotation about +X (OpenXR right): positive looks up
    return HostQuat{static_cast<float>(std::sin(deg * kDeg / 2)), 0.0f, 0.0f, static_cast<float>(std::cos(deg * kDeg / 2))};
}
}  // namespace

int main() {
    // FRotator <-> FQuat round trips (UE 4.18 formulas).
    const ue::FRotator rots[] = {{0, 0, 0}, {10, 20, 30}, {-45, 170, -10}, {80, -90, 5}, {0, 180, 0}, {-30, 45, 0}};
    bool rt_ok = true;
    for (const auto& r : rots) rt_ok = rt_ok && near_rot(rotator_from_quat(quat_from_rotator(r)), r);
    check(rt_ok, "rotator -> quat -> rotator round trip");

    // Yaw +90 turns +X (forward) into +Y (right), as in UE.
    Vec v = rotate(quat_from_rotator({0, 90, 0}), Vec{1, 0, 0});
    check(near(v.x, 0) && near(v.y, 1) && near(v.z, 0), "yaw +90 maps forward to right");
    // Pitch +90 turns forward into up.
    v = rotate(quat_from_rotator({90, 0, 0}), Vec{1, 0, 0});
    check(near(v.x, 0) && near(v.y, 0) && near(v.z, 1), "pitch +90 maps forward to up");

    // OpenXR -> UE: head turned left 30 deg is UE yaw -30; looking up 20 deg is UE pitch +20.
    check(near_rot(rotator_from_quat(xr_to_ue_quat(xr_yaw(30))), {0, -30, 0}), "XR yaw left 30 -> UE yaw -30");
    check(near_rot(rotator_from_quat(xr_to_ue_quat(xr_pitch(20))), {20, 0, 0}), "XR pitch up 20 -> UE pitch +20");
    Vec p = xr_to_ue_position(HostVec3{0.1f, 0.2f, -0.3f});
    check(near(p.x, 0.3) && near(p.y, 0.1) && near(p.z, 0.2), "XR position (right, up, back) -> UE (forward, right, up)");

    // Eye composition: camera yawed 90 (looking along +Y), left eye 32 mm to the left.
    EyeCameraInput in;
    in.camera_rotation = {0, 90, 0};
    in.camera_location = {100, 200, 300};
    in.units_per_metre = 100;
    in.eye.position = HostVec3{-0.032f, 0, 0};
    ue::FRotator r{};
    ue::FVector l{};
    compose_eye(in, r, l);
    // The camera's right is -X, so its left is +X.
    check(near(l.X, 103.2) && near(l.Y, 200) && near(l.Z, 300), "left eye sits 3.2 cm to the camera's left");
    check(near_rot(r, {0, 90, 0}), "identity head keeps the camera rotation");

    // Decoupled pitch: game camera pitch and roll are dropped, head pitch is kept.
    in.camera_rotation = {-35, 90, 10};
    in.eye = HostPose{xr_pitch(15), HostVec3{}};
    compose_eye(in, r, l);
    check(near_rot(r, {15, 90, 0}), "decoupled pitch: camera pitch/roll removed, head pitch kept");
    in.decouple_pitch = false;
    in.camera_rotation = {-35, 90, 0};
    in.eye = HostPose{};
    compose_eye(in, r, l);
    check(near_rot(r, {-35, 90, 0}), "without decoupling the camera pitch is kept");

    // Head yaw adds to camera yaw.
    in.decouple_pitch = true;
    in.camera_rotation = {0, 40, 0};
    in.eye = HostPose{xr_yaw(30), HostVec3{}};
    compose_eye(in, r, l);
    check(near_rot(r, {0, 10, 0}), "head turned left 30 from camera yaw 40 -> yaw 10");

    // Orientation-only mode keeps the IPD but drops head translation.
    in.camera_rotation = {0, 0, 0};
    in.camera_location = {0, 0, 0};
    in.positional = false;
    in.head = HostPose{HostQuat{}, HostVec3{0.5f, 0.5f, 0.5f}};
    in.eye = HostPose{HostQuat{}, HostVec3{0.5f + 0.032f, 0.5f, 0.5f}};
    compose_eye(in, r, l);
    check(near(l.X, 0) && near(l.Y, 3.2) && near(l.Z, 0), "positional off: only the eye's offset from the head remains");

    // Projection: symmetric 90 x 90.
    ue::FMatrix m{};
    stereo_projection(HostFov{static_cast<float>(-45 * kDeg), static_cast<float>(45 * kDeg), static_cast<float>(45 * kDeg),
                              static_cast<float>(-45 * kDeg)},
                      10.0f, m);
    check(near(m.M[0][0], 1) && near(m.M[1][1], 1) && near(m.M[2][0], 0) && near(m.M[2][1], 0) && near(m.M[2][3], 1) &&
              near(m.M[3][2], 10) && near(m.M[2][2], 0) && near(m.M[3][3], 0),
          "symmetric 90 deg projection");
    // A point on the right frustum edge lands at NDC x = +1, the top edge at y = +1 (UE view
    // space after the view rotation: x right, y up, z forward; row vector times matrix).
    const HostFov q3{static_cast<float>(-52 * kDeg), static_cast<float>(42 * kDeg), static_cast<float>(46 * kDeg),
                     static_cast<float>(-50 * kDeg)};
    stereo_projection(q3, 10.0f, m);
    auto ndc = [&](double x, double y, double z) {
        const double cx = x * m.M[0][0] + y * m.M[1][0] + z * m.M[2][0] + m.M[3][0];
        const double cy = x * m.M[0][1] + y * m.M[1][1] + z * m.M[2][1] + m.M[3][1];
        const double cw = x * m.M[0][3] + y * m.M[1][3] + z * m.M[2][3] + m.M[3][3];
        return Vec{cx / cw, cy / cw, 0};
    };
    const Vec right_edge = ndc(std::tan(42 * kDeg) * 100, 0, 100);
    const Vec left_edge = ndc(std::tan(-52 * kDeg) * 100, 0, 100);
    const Vec top_edge = ndc(0, std::tan(46 * kDeg) * 100, 100);
    const Vec bottom_edge = ndc(0, std::tan(-50 * kDeg) * 100, 100);
    check(near(right_edge.x, 1) && near(left_edge.x, -1) && near(top_edge.y, 1) && near(bottom_edge.y, -1),
          "asymmetric projection maps the FOV edges to NDC +-1");
    // Reversed Z, infinite far: depth = near / z.
    const double depth = (100 * m.M[2][2] + m.M[3][2]) / (100 * m.M[2][3]);
    check(near(depth, 0.1), "reversed-Z depth = near / z");

    // Level boom: a camera 400 cm behind the pivot on a boom pitched down 34 degrees (looking
    // down from above) comes back to the pivot's height, 400 cm behind it along the yaw.
    {
        const double pitch = -34 * kDeg, yaw = 90 * kDeg;
        const Vec pivot{1000, 2000, 160};
        const Vec fwd{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
        const ue::FVector cam{static_cast<float>(pivot.x - 400 * fwd.x), static_cast<float>(pivot.y - 400 * fwd.y),
                              static_cast<float>(pivot.z - 400 * fwd.z)};
        const Vec b = level_boom(ue::FRotator{-34, 90, 0}, cam, pivot);
        check(near(b.x, 1000, 0.05) && near(b.y, 1600, 0.05) && near(b.z, 160, 0.05),
              "level boom: pitched camera back to the pivot's height");
        // A sideways offset of the camera (over the shoulder) is kept.
        const ue::FVector cam2{cam.X + 50, cam.Y, cam.Z};  // yaw 90: +X is the camera's left
        const Vec b2 = level_boom(ue::FRotator{-34, 90, 0}, cam2, pivot);
        check(near(b2.x, 1050, 0.05) && near(b2.y, 1600, 0.05) && near(b2.z, 160, 0.05), "level boom keeps the sideways offset");
        const Vec b3 = level_boom(ue::FRotator{0, 90, 0}, ue::FVector{1000, 1600, 160}, pivot);
        check(near(b3.x, 1000, 0.05) && near(b3.y, 1600, 0.05) && near(b3.z, 160, 0.05), "level boom: zero pitch unchanged");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "RESULT: FAIL" : "RESULT: PASS", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
