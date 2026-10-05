#include "ff7vr/xr/xr_math.h"

#include <algorithm>

namespace ff7vr::xr {

Mat4 Mat4::Identity() {
    Mat4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
}

Quat QuatMultiply(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat QuatConjugate(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

Quat QuatNormalize(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 1e-12f) return Quat{};
    return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
}

Quat QuatFromAxisAngle(const Vec3& axis, float radians) {
    const float n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (n <= 1e-12f) return Quat{};
    const float s = std::sin(radians * 0.5f) / n;
    return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f)};
}

Vec3 QuatRotate(const Quat& q, const Vec3& v) {
    // v' = v + 2w(u x v) + 2 u x (u x v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t{2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z), 2.0f * (u.x * v.y - u.y * v.x)};
    return Vec3{
        v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
        v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
        v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
    };
}

float QuatYaw(const Quat& q) {
    const Vec3 f = QuatRotate(q, Vec3{0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

Pose PoseMultiply(const Pose& a, const Pose& b) {
    Pose r;
    r.orientation = QuatNormalize(QuatMultiply(a.orientation, b.orientation));
    const Vec3 p = QuatRotate(a.orientation, b.position);
    r.position = Vec3{p.x + a.position.x, p.y + a.position.y, p.z + a.position.z};
    return r;
}

Pose PoseInverse(const Pose& p) {
    Pose r;
    r.orientation = QuatConjugate(p.orientation);
    const Vec3 t = QuatRotate(r.orientation, p.position);
    r.position = Vec3{-t.x, -t.y, -t.z};
    return r;
}

static void RotationRows(const Quat& q, float r[3][3]) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r[0][0] = 1 - 2 * (yy + zz); r[0][1] = 2 * (xy - wz);     r[0][2] = 2 * (xz + wy);
    r[1][0] = 2 * (xy + wz);     r[1][1] = 1 - 2 * (xx + zz); r[1][2] = 2 * (yz - wx);
    r[2][0] = 2 * (xz - wy);     r[2][1] = 2 * (yz + wx);     r[2][2] = 1 - 2 * (xx + yy);
}

Mat4 ViewMatrix_ColumnVector(const Pose& eyePose) {
    float r[3][3];
    RotationRows(eyePose.orientation, r);
    const Vec3& p = eyePose.position;
    Mat4 v{};
    // V = [R^T, -R^T p; 0 0 0 1]
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) v.m[i][j] = r[j][i];
        v.m[i][3] = -(r[0][i] * p.x + r[1][i] * p.y + r[2][i] * p.z);
    }
    v.m[3][3] = 1.0f;
    return v;
}

FovTangents TangentsFromFov(const Fov& fov) {
    return FovTangents{std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleUp), std::tan(fov.angleDown)};
}

Mat4 Projection_ColumnVector(const Fov& fov, float nearZ, float farZ, bool reversedZ) {
    const FovTangents t = TangentsFromFov(fov);
    const float w = t.right - t.left;
    const float h = t.up - t.down;
    Mat4 p{};
    p.m[0][0] = 2.0f / w;
    p.m[0][2] = (t.right + t.left) / w;
    p.m[1][1] = 2.0f / h;
    p.m[1][2] = (t.up + t.down) / h;
    const bool infinite = !(farZ > nearZ) || std::isinf(farZ);
    float a, b;  // clip.z = a * z + b, clip.w = -z
    if (infinite) {
        a = -1.0f;
        b = -nearZ;
    } else {
        a = farZ / (nearZ - farZ);
        b = nearZ * farZ / (nearZ - farZ);
    }
    if (reversedZ) {
        a = -1.0f - a;
        b = -b;
    }
    p.m[2][2] = a;
    p.m[2][3] = b;
    p.m[3][2] = -1.0f;
    return p;
}

Mat4 ProjectionUnreal(const Fov& fov, float nearCm) {
    const FovTangents t = TangentsFromFov(fov);
    const float invRL = 1.0f / (t.right - t.left);
    const float invTB = 1.0f / (t.up - t.down);
    Mat4 p{};
    p.m[0][0] = 2.0f * invRL;
    p.m[1][1] = 2.0f * invTB;
    p.m[2][0] = -(t.right + t.left) * invRL;
    p.m[2][1] = -(t.up + t.down) * invTB;
    p.m[2][2] = 0.0f;
    p.m[2][3] = 1.0f;
    p.m[3][2] = nearCm;
    return p;
}

UeVector ToUnrealPosition(const Vec3& v, float worldScale) {
    return UeVector{-double(v.z) * worldScale, double(v.x) * worldScale, double(v.y) * worldScale};
}

UeQuat ToUnrealQuat(const Quat& q) { return UeQuat{-double(q.z), double(q.x), double(q.y), -double(q.w)}; }

UeRotator ToUnrealRotator(const Quat& xr) {
    // Same as UE4 FQuat::Rotator().
    const UeQuat q = ToUnrealQuat(xr);
    const double X = q.x, Y = q.y, Z = q.z, W = q.w;
    const double singularityTest = Z * X - W * Y;
    const double yawY = 2.0 * (W * Z + X * Y);
    const double yawX = 1.0 - 2.0 * (Y * Y + Z * Z);
    constexpr double kSingularity = 0.4999995;
    constexpr double kRad2Deg = 57.295779513082320876;
    UeRotator r;
    if (singularityTest < -kSingularity) {
        r.pitch = -90.0;
        r.yaw = std::atan2(yawY, yawX) * kRad2Deg;
        r.roll = -r.yaw - 2.0 * std::atan2(X, W) * kRad2Deg;
    } else if (singularityTest > kSingularity) {
        r.pitch = 90.0;
        r.yaw = std::atan2(yawY, yawX) * kRad2Deg;
        r.roll = r.yaw - 2.0 * std::atan2(X, W) * kRad2Deg;
    } else {
        r.pitch = std::asin(2.0 * singularityTest) * kRad2Deg;
        r.yaw = std::atan2(yawY, yawX) * kRad2Deg;
        r.roll = std::atan2(-2.0 * (W * X + Y * Z), 1.0 - 2.0 * (X * X + Y * Y)) * kRad2Deg;
    }
    // Normalise roll into (-180, 180] like FRotator::NormalizeAxis.
    auto norm = [](double a) {
        a = std::fmod(a, 360.0);
        if (a > 180.0) a -= 360.0;
        if (a <= -180.0) a += 360.0;
        return a;
    };
    r.roll = norm(r.roll);
    return r;
}

}  // namespace ff7vr::xr
