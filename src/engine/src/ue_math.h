#pragma once
// Camera and projection math in Unreal Engine conventions, plus the conversion from the
// headset's (OpenXR) conventions.
//
// Unreal: left-handed, +X forward, +Y right, +Z up, centimetres. FRotator is {Pitch, Yaw,
// Roll} in degrees; FQuat <-> FRotator follow UE 4.18's FRotator::Quaternion and
// FQuat::Rotator exactly. Quaternion product a * b applies b first, then a.
//
// OpenXR: right-handed, +X right, +Y up, -Z forward, metres. Conversion (same as UE's own
// OpenXR plugin): UE.X = -XR.Z, UE.Y = XR.X, UE.Z = XR.Y; quaternion (x, y, z, w)_UE =
// (-q.z, q.x, q.y, -q.w).

#include "ff7vr/engine/stereo_abi.h"
#include "ff7vr/engine/stereo_host.h"

namespace ff7vr::engine::math {

struct Vec {
    double x = 0, y = 0, z = 0;
};
struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

Quat quat_from_rotator(const ue::FRotator& r);
ue::FRotator rotator_from_quat(const Quat& q);
Quat mul(const Quat& a, const Quat& b);
Quat conjugate(const Quat& q);
Quat normalize(const Quat& q);
Vec rotate(const Quat& q, const Vec& v);
Vec add(const Vec& a, const Vec& b);
Vec scale(const Vec& v, double s);

// Headset (OpenXR) to Unreal. Position: metres in, metres out (axes swapped only).
Vec xr_to_ue_position(const HostVec3& p);
Quat xr_to_ue_quat(const HostQuat& q);

// Stereo projection in Unreal's FMatrix layout (row vectors, reversed Z, infinite far):
//   [ 2/(r-l)       0             0     0 ]
//   [ 0             2/(t-b)       0     0 ]
//   [ (l+r)/(l-r)   (t+b)/(b-t)   0     1 ]
//   [ 0             0             near  0 ]
// l, r, t, b are the tangents of the FOV angles (l and b negative).
void stereo_projection(const HostFov& fov, float near_plane, ue::FMatrix& out);

// What a seated player sees from a third-person game camera:
//   orientation = camera * head-tracked eye orientation
//   location    = camera location + camera.rotate(eye position * units per metre)
// With decouple_pitch the camera's pitch and roll are dropped first, so only the game
// camera's yaw turns the player and the horizon stays level whatever the game camera does.
struct EyeCameraInput {
    ue::FRotator camera_rotation{};
    ue::FVector camera_location{};
    double units_per_metre = 100.0;  // WorldToMeters * world scale
    bool decouple_pitch = true;
    bool positional = true;          // apply the head/eye position (false: orientation only, IPD kept)
    HostPose eye{};                  // eye pose in tracking space (OpenXR conventions)
    HostPose head{};                 // head pose (only used when positional is false)
};
void compose_eye(const EyeCameraInput& in, ue::FRotator& out_rotation, ue::FVector& out_location);

// Where a follow camera at `camera_location` with `camera_rotation` would be at zero pitch
// (and no roll) around `pivot`: the camera's offset from the pivot, taken in the camera's
// frame, put back with the camera's yaw only. A camera boom that swings over the pivot when
// the camera pitches is brought back to the pivot's height.
Vec level_boom(const ue::FRotator& camera_rotation, const ue::FVector& camera_location, const Vec& pivot);

}  // namespace ff7vr::engine::math
