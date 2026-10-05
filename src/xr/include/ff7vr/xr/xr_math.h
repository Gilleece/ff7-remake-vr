// ff7vr XR layer: math types and conventions.
//
// ======================= CONVENTIONS (read this) ===========================
//
// Tracking space ("XR space") used by every pose this library returns:
//   * OpenXR convention: RIGHT-HANDED, +X right, +Y up, -Z forward (the user
//     looks down -Z when the pose is identity).
//   * Units: METRES.
//   * Origin: the runtime's LOCAL reference space (seated: origin near the
//     head position at session start / last runtime recenter), with this
//     library's own recenter transform applied on top (see IXrBackend::Recenter).
//   * Quat is (x, y, z, w), unit length, active rotation; it rotates vectors
//     from the eye/head's local frame into tracking space:
//         v_tracking = q * v_local * conj(q) + position
//
// Fov: OpenXR XrFovf. Angles in RADIANS measured from the view's -Z axis.
//   angleLeft and angleDown are normally NEGATIVE, angleRight and angleUp
//   POSITIVE. Asymmetric (off-axis) frusta are normal (Quest 3 is asymmetric).
//
// Mat4: 16 floats, ROW-MAJOR storage, m[row][col].
//   * "Column-vector" matrices (Mat4 * column vector) are produced by the
//     *_ColumnVector helpers (OpenGL / OpenXR sample style, p' = M * p).
//   * Unreal uses ROW vectors (p' = p * M); the Unreal helper below returns
//     the matrix laid out exactly as Unreal's FMatrix M[row][col].
//
// Conversion to Unreal (left-handed, Z up, X forward, centimetres):
//   UE.X =  -XR.Z  (forward)
//   UE.Y =   XR.X  (right)
//   UE.Z =   XR.Y  (up)
//   position_cm = position_m * 100 * worldScale
//   quaternion  : UE(x,y,z,w) = (-q.z, q.x, q.y, -q.w)
// These are the same mappings as UE4's own OpenXRHMD plugin (ToFVector/ToFQuat).
// ToUnrealPosition / ToUnrealQuat below implement them; the XR library does not
// depend on Unreal headers, it only produces numbers in that convention.
// ===========================================================================
#pragma once

#include <cmath>
#include <cstdint>

namespace ff7vr::xr {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Pose {
    Quat orientation{};
    Vec3 position{};
};

struct Fov {
    float angleLeft = 0, angleRight = 0, angleUp = 0, angleDown = 0;  // radians
};

struct Mat4 {
    float m[4][4]{};  // row-major storage: m[row][col]
    static Mat4 Identity();
};

// Unreal-convention numbers (no Unreal types). Doubles because UE world
// positions can be large; UE4.18 FVector is float, cast as needed.
struct UeVector {
    double x = 0, y = 0, z = 0;
};
struct UeQuat {
    double x = 0, y = 0, z = 0, w = 1;
};

// ---- basic quaternion / vector helpers ----
Quat QuatMultiply(const Quat& a, const Quat& b);  // a * b (apply b first, then a)
Quat QuatConjugate(const Quat& q);
Quat QuatNormalize(const Quat& q);
Quat QuatFromAxisAngle(const Vec3& axis, float radians);
Vec3 QuatRotate(const Quat& q, const Vec3& v);
float QuatYaw(const Quat& q);  // rotation about +Y (XR up), radians, CCW seen from above (+Y)
Pose PoseMultiply(const Pose& a, const Pose& b);  // a * b: transform b's frame into a's parent
Pose PoseInverse(const Pose& p);

// ---- matrices from XR data (OpenXR right-handed space) ----

// View matrix (world -> eye) for a column-vector convention: p_eye = V * p_world.
// Eye space is right-handed, -Z forward, metres.
Mat4 ViewMatrix_ColumnVector(const Pose& eyePose);

// D3D-style projection for right-handed eye space (-Z forward), column vectors,
// clip.xy in [-1,1], depth in [0,1]. If farZ <= 0 or infinite: infinite far plane.
// reversedZ: near maps to 1, far to 0 (recommended for float depth).
Mat4 Projection_ColumnVector(const Fov& fov, float nearZ, float farZ, bool reversedZ);

// Unreal-style stereo projection, in Unreal's FMatrix layout (row vectors,
// left-handed X-forward view space after UE's own view rotation, reversed-Z,
// infinite far). Identical to UE4 OpenXRHMD::GetStereoProjectionMatrix:
//   [ 2/(r-l)          0               0      0 ]
//   [ 0                2/(t-b)         0      0 ]
//   [ (l+r)/(l-r)      (t+b)/(b-t)     0      1 ]
//   [ 0                0               nearCm 0 ]
// with l=tan(angleLeft), r=tan(angleRight), t=tan(angleUp), b=tan(angleDown).
// nearCm is Unreal's GNearClippingPlane (centimetres; default 10).
Mat4 ProjectionUnreal(const Fov& fov, float nearCm);

// ---- Unreal conversions ----
UeVector ToUnrealPosition(const Vec3& xrMetres, float worldScale /* UE units per metre, normally 100 */);
UeQuat ToUnrealQuat(const Quat& xr);

// Euler angles in Unreal's FRotator order (degrees): Pitch about Y(right), Yaw about Z(up), Roll about X(forward).
struct UeRotator {
    double pitch = 0, yaw = 0, roll = 0;
};
UeRotator ToUnrealRotator(const Quat& xr);

// Frustum tangents (OpenXR order). Handy for engine code that builds its own matrices.
struct FovTangents {
    float left, right, up, down;  // tan(angle); left/down normally negative
};
FovTangents TangentsFromFov(const Fov& fov);

constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kRadToDeg = 57.29577951308232f;

}  // namespace ff7vr::xr
