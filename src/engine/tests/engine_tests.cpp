// Unit tests for the engine module's camera and projection math. Exit code 0 = pass.

#include "battle_state.h"
#include "ue_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

// Fight detection replayed on a timeline of (time, battle signal, target marker shown) as
// the battle diagnostic logged it (the value holds until the next entry), polled every
// 100 ms as in the game. Returns the fights as [start, end) pairs.
struct Sample {
    double t;
    int signal;
    int marker;  // -1 no marker instance yet, 0 hidden, 1 shown
};
std::vector<std::pair<double, double>> replay(const std::vector<Sample>& tl, const battle::Config& cfg, std::vector<std::string>* reasons = nullptr) {
    std::vector<std::pair<double, double>> fights;
    if (tl.empty()) return fights;
    battle::Decider d;
    std::size_t i = 0;
    const double end = tl.back().t + 10.0;
    for (int k = 0;; ++k) {
        const double t = tl.front().t + k * 0.1;
        if (t > end) break;
        while (i + 1 < tl.size() && tl[i + 1].t <= t) ++i;
        const battle::Change c = d.update(cfg, battle::Input{t, tl[i].signal, tl[i].marker < 0 ? 0 : tl[i].marker});
        if (!c.changed) continue;
        if (reasons) reasons->push_back(std::to_string(t) + (c.fight ? " on: " : " off: ") + c.reason);
        if (c.fight) fights.push_back({t, -1.0});
        else if (!fights.empty()) fights.back().second = t;
    }
    return fights;
}

// The owner's session of 2026-10-10 (three fights in one battle area; the battle signal
// stayed on from the first fight to the end of the session). He switched back to first
// person by hand at 632.6 s, after the first fight.
const std::vector<Sample> kRecorded = {
    {10.705, 0, -1},  {567.414, 1, -1}, {569.857, 1, 1},  {575.634, 1, 0},  {576.747, 1, 1},  {598.976, 1, 0},
    {599.198, 1, 1},  {599.641, 1, 0},  {601.627, 1, 1},  {607.949, 1, 0},  {610.236, 1, 1},  {617.767, 1, 0},
    {619.213, 1, 1},  {626.990, 1, 0},  {884.992, 1, 1},  {885.326, 1, 0},  {885.993, 1, 1},  {931.078, 1, 0},
    {931.289, 1, 1},  {960.854, 1, 0},  {1004.122, 1, 1}, {1028.610, 1, 0}, {1028.944, 1, 1}, {1029.721, 1, 0},
    {1500.0, 1, 0},
};

// Reads the battle diagnostic lines of a log (`battle diag: t=<ms> ... signal=<v> ...
// w:BattleTarget_New_C(BattleTarget_New_C)=<visible>/...`).
std::vector<Sample> read_log(const char* path) {
    std::vector<Sample> tl;
    std::ifstream in(path);
    const std::string key = "w:BattleTarget_New_C(BattleTarget_New_C)=";
    for (std::string line; std::getline(in, line);) {
        const auto p = line.find("battle diag: t=");
        if (p == std::string::npos) continue;
        const auto sp = line.find(" signal=", p);
        if (sp == std::string::npos) continue;
        Sample s{std::atof(line.c_str() + p + 15) / 1000.0, std::atoi(line.c_str() + sp + 8), -1};
        const auto mp = line.find(key, p);
        if (mp != std::string::npos) s.marker = std::atoi(line.c_str() + mp + key.size());
        tl.push_back(s);
    }
    return tl;
}
HostQuat xr_yaw(double deg) {  // rotation about +Y (OpenXR up): positive turns the view to the left
    return HostQuat{0.0f, static_cast<float>(std::sin(deg * kDeg / 2)), 0.0f, static_cast<float>(std::cos(deg * kDeg / 2))};
}
HostQuat xr_pitch(double deg) {  // rotation about +X (OpenXR right): positive looks up
    return HostQuat{static_cast<float>(std::sin(deg * kDeg / 2)), 0.0f, 0.0f, static_cast<float>(std::cos(deg * kDeg / 2))};
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--replay") == 0) {
        // ff7vr_engine_tests --replay <ff7vr.log>: the fights the detection finds in a log
        // recorded with [first_person] battle_log = 1.
        const std::vector<Sample> tl = read_log(argv[2]);
        std::vector<std::string> reasons;
        const auto fights = replay(tl, battle::Config{}, &reasons);
        std::printf("%zu battle diag samples, %zu fight(s)\n", tl.size(), fights.size());
        for (const auto& r : reasons) std::printf("  %s\n", r.c_str());
        return 0;
    }
    // FRotator <-> FQuat round trips (UE 4.18 formulas).
    const ue::FRotator rots[] = {{0, 0, 0}, {10, 20, 30}, {-45, 170, -10}, {80, -90, 5}, {0, 180, 0}, {-30, 45, 0}};
    bool rt_ok = true;
    for (const auto& r : rots) rt_ok = rt_ok && near_rot(rotator_from_quat(quat_from_rotator(r)), r);
    check(rt_ok, "rotator -> quat -> rotator round trip");

    // Looking straight up or down (the poles): the rotator from a quaternion may split yaw
    // and roll differently, but it must describe the same orientation.
    bool pole_ok = true;
    const float pitches[] = {90.0f, -90.0f, 89.99f, -89.99f};
    const Vec axes[] = {Vec{1, 0, 0}, Vec{0, 1, 0}, Vec{0, 0, 1}};
    for (float pitch : pitches) {
        const Quat q = mul(quat_from_rotator({0, 30, 0}), quat_from_rotator({pitch, 0, 0}));
        const Quat back = quat_from_rotator(rotator_from_quat(q));
        for (const Vec& axis : axes) {
            const Vec a = rotate(q, axis), b = rotate(back, axis);
            pole_ok = pole_ok && near(a.x, b.x, 1e-3) && near(a.y, b.y, 1e-3) && near(a.z, b.z, 1e-3);
        }
    }
    check(pole_ok, "orientation looking straight up or down survives the rotator conversion");

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

    // Screen phase: a 60 degree flat view in a 2084x2020 eye rect, shown as the centre crop at
    // 16:9. The edges of the flat game's 16:9 frustum must land on the crop's edges: x = +-1
    // and y = +-(the crop's share of the rect's height).
    {
        const double w = 2084, h = 2020, aspect = 16.0 / 9.0, tx = std::tan(30 * kDeg);
        stereo_projection(centred_fov(60.0, w, h), 10.0f, m);
        const Vec re = ndc(tx * 100, 0, 100);
        const Vec t = ndc(0, tx / aspect * 100, 100);
        const Vec b = ndc(0, -tx / aspect * 100, 100);
        const double crop_share = (w / aspect) / h;
        check(near(re.x, 1) && near(re.y, 0) && near(t.y, crop_share) && near(b.y, -crop_share) && near(t.x, 0),
              "screen phase: the flat game's 16:9 frustum fills the centre crop of the eye rect");
    }

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

    // Fight detection on the recorded session.
    {
        std::vector<std::string> reasons;
        const auto f = replay(kRecorded, battle::Config{}, &reasons);
        for (const auto& line : reasons) std::printf("      %s\n", line.c_str());
        check(f.size() == 3, "recorded session: three fights");
        if (f.size() == 3) {
            check(near(f[0].first, 567.4, 0.11), "fight 1 starts with the battle signal (567.4 s)");
            check(f[0].second > 629.0 && f[0].second < 631.0, "fight 1 ends about 3 s after the marker went (627.0 s), before the manual switch at 632.6 s");
            check(f[1].first > 884.9 && f[1].first < 885.3, "fight 2 starts with the marker (885.0 s)");
            check(f[1].second > 963.0 && f[1].second < 965.0, "fight 2 ends about 3 s after the marker went (960.9 s)");
            check(f[2].first > 1004.1 && f[2].first < 1004.4 && f[2].second > 1032.0 && f[2].second < 1034.0, "fight 3: 1004.1 to about 1032.7 s");
        }
        // 1.5 s and 2 s are too short: the marker hides for 1.4, 2.0 and 2.3 s mid-fight while the target changes.
        battle::Config short_exit;
        short_exit.exit_seconds = 1.5;
        check(replay(kRecorded, short_exit).size() == 6, "exit after 1.5 s would split fight 1 into four");
        short_exit.exit_seconds = 2.0;
        check(replay(kRecorded, short_exit).size() == 5, "exit after 2 s would split fight 1 into three");
        // The signal alone (battle_objects = 0): one fight from 567 s to the end.
        battle::Config signal_only;
        signal_only.objects = false;
        const auto so = replay(kRecorded, signal_only);
        check(so.size() == 1 && so[0].second < 0, "signal alone: one fight that never ends (the old behaviour)");
        // A start by the signal without any marker ends after the grace period.
        const auto g = replay({{0.0, 0, -1}, {10.0, 1, -1}, {40.0, 1, -1}}, battle::Config{});
        check(g.size() == 1 && near(g[0].first, 10.0, 0.11) && near(g[0].second, 18.0, 0.11), "signal on without a marker: fight for the 8 s grace");
        // The signal off ends a fight at once; a signal that is on from the start (a save loaded in
        // a battle area) does not start one by itself.
        const auto o = replay({{0.0, 0, 0}, {5.0, 1, 1}, {9.0, 0, 1}, {20.0, 0, 0}}, battle::Config{});
        check(o.size() == 1 && near(o[0].second, 9.0, 0.11), "signal off ends the fight at once");
        const auto loaded = replay({{0.0, 1, 0}, {30.0, 1, 0}}, battle::Config{});
        check(loaded.empty(), "signal on from the start without a marker: no fight");
        const auto loaded2 = replay({{0.0, -1, -1}, {1.0, 1, 0}, {5.0, 1, 1}, {9.0, 1, 0}, {20.0, 1, 0}}, battle::Config{});
        check(loaded2.size() == 1 && near(loaded2[0].first, 5.1, 0.11) && near(loaded2[0].second, 11.9, 0.11), "signal on from the start: the marker starts and ends the fight");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "RESULT: FAIL" : "RESULT: PASS", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
