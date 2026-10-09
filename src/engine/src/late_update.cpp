#include "late_update.h"

#include "stereo_device.h"
#include "ue_math.h"

#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace ff7vr::engine::late_update {
namespace {

using ue::FRotator;
using ue::FVector;

// ------------------------------------------------------------------ FViewMatrices (UE 4.18)
// The matrices of FViewMatrices as the engine computes them (indices of ViewSet::m; not
// their order in memory, which is found by value: see "layout" below), then
// PreViewTranslation and ViewOrigin.
enum Mat : int {
    kProjection = 0,
    kProjectionNoAA,
    kInvProjection,
    kView,
    kInvView,
    kViewProjection,
    kInvViewProjection,
    kHmdViewNoRoll,
    kTranslatedView,
    kInvTranslatedView,
    kOverriddenTranslatedView,
    kOverriddenInvTranslatedView,
    kTranslatedViewProjection,
    kInvTranslatedViewProjection,
    kMatCount
};
constexpr int kMaxCopies = 4;

// FConvexVolume: TArray<FPlane, TInlineAllocator<6>> Planes, then
// TArray<FPlane, TInlineAllocator<8>> PermutedPlanes. An inline array is its elements, then
// the heap pointer of the secondary allocator (the pair padded to 16 bytes, FPlane's
// alignment), then ArrayNum and ArrayMax, the whole padded to 16 (LIVE: Planes 0x80 bytes,
// PermutedPlanes 0xA0; docs/re/engine.md, "FViewMatrices in FViewInfo").
constexpr std::size_t kPlanesHeap = 6 * 16, kPlanesNum = 0x70, kPermuted = 0x80;
constexpr std::size_t kPermutedHeap = kPermuted + 8 * 16, kPermutedNum = kPermuted + 0x90, kConvexSize = kPermuted + 0xA0;

struct M4 {
    double m[4][4]{};
};
M4 identity() {
    M4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0;
    return r;
}
M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
M4 transpose(const M4& a) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
    return r;
}
M4 translation(double x, double y, double z) {
    M4 r = identity();
    r.m[3][0] = x;
    r.m[3][1] = y;
    r.m[3][2] = z;
    return r;
}
M4 load(const std::uint8_t* p) {
    M4 r;
    const float* f = reinterpret_cast<const float*>(p);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = f[i * 4 + j];
    return r;
}
void store(const M4& a, std::uint8_t* p) {
    float* f = reinterpret_cast<float*>(p);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) f[i * 4 + j] = static_cast<float>(a.m[i][j]);
}

// ULocalPlayer::GetProjectionData: FInverseRotationMatrix(Rotation) * the view planes
// matrix (X forward, Y right, Z up -> view X right, Y up, Z forward).
M4 view_rotation(const FRotator& r) {
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const double sp = std::sin(r.Pitch * kDeg), cp = std::cos(r.Pitch * kDeg);
    const double sy = std::sin(r.Yaw * kDeg), cy = std::cos(r.Yaw * kDeg);
    const double sr = std::sin(r.Roll * kDeg), cr = std::cos(r.Roll * kDeg);
    // FRotationMatrix rows: forward, right, up.
    const double rm[3][3] = {
        {cp * cy, cp * sy, sp},
        {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
        {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp},
    };
    M4 v = identity();
    for (int i = 0; i < 3; ++i) {
        v.m[i][0] = rm[1][i];
        v.m[i][1] = rm[2][i];
        v.m[i][2] = rm[0][i];
        v.m[i][3] = 0.0;
    }
    return v;
}

// The view-dependent matrices of FViewMatrices for an eye at `loc` with `rot` (as the
// FViewMatrices constructor computes them), given its projection and inverse projection.
struct ViewSet {
    M4 m[kMatCount];
    M4 hmdNoRoll;  // the variant with the roll removed (FViewMatrices::UpdateViewMatrix)
};
ViewSet build(const FVector& loc, const FRotator& rot, const M4& proj, const M4& inv_proj) {
    ViewSet s;
    const M4 vrm = view_rotation(rot);
    const M4 vrm_t = transpose(vrm);
    const double x = loc.X, y = loc.Y, z = loc.Z;
    s.m[kProjection] = proj;
    s.m[kInvProjection] = inv_proj;
    s.m[kView] = mul(translation(-x, -y, -z), vrm);
    s.m[kInvView] = mul(vrm_t, translation(x, y, z));
    s.m[kViewProjection] = mul(s.m[kView], proj);
    s.m[kInvViewProjection] = mul(inv_proj, s.m[kInvView]);
    s.m[kHmdViewNoRoll] = vrm;
    s.hmdNoRoll = view_rotation(FRotator{rot.Pitch, rot.Yaw, 0.0f});
    s.m[kTranslatedView] = vrm;
    s.m[kInvTranslatedView] = vrm_t;
    s.m[kOverriddenTranslatedView] = mul(translation(x, y, z), s.m[kView]);
    s.m[kOverriddenInvTranslatedView] = mul(s.m[kInvView], translation(-x, -y, -z));
    s.m[kTranslatedViewProjection] = mul(vrm, proj);
    s.m[kInvTranslatedViewProjection] = mul(inv_proj, vrm_t);
    return s;
}

double max_error(const M4& a, const M4& b) {
    double e = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            const double d = std::fabs(a.m[i][j] - b.m[i][j]) / (1e-3 + 2e-5 * std::max(std::fabs(a.m[i][j]), std::fabs(b.m[i][j])));
            e = std::max(e, d);
        }
    return e;  // <= 1: equal within float precision
}

// FMatrix::GetFrustum*Plane (MakeFrustumPlane): index 0 near, 1 left, 2 right, 3 top,
// 4 bottom, 5 far (for this reversed-Z projection the "far" formula gives the near plane).
bool frustum_plane(const M4& m, int which, double out[4]) {
    double a, b, c, d;
    auto col = [&](int k, int sign, int k2) {
        a = m.m[0][k] + sign * m.m[0][k2];
        b = m.m[1][k] + sign * m.m[1][k2];
        c = m.m[2][k] + sign * m.m[2][k2];
        d = m.m[3][k] + sign * m.m[3][k2];
    };
    switch (which) {
        case 0: a = m.m[0][2]; b = m.m[1][2]; c = m.m[2][2]; d = m.m[3][2]; break;
        case 1: col(3, +1, 0); break;
        case 2: col(3, -1, 0); break;
        case 3: col(3, -1, 1); break;
        case 4: col(3, +1, 1); break;
        default: col(3, -1, 2); break;
    }
    const double len2 = a * a + b * b + c * c;
    if (len2 <= 1e-16) return false;
    const double inv = 1.0 / std::sqrt(len2);
    out[0] = -a * inv;
    out[1] = -b * inv;
    out[2] = -c * inv;
    out[3] = d * inv;
    return true;
}
bool plane_equal(const float* p, const double q[4]) {
    for (int i = 0; i < 3; ++i)
        if (std::fabs(p[i] - q[i]) > 2e-4) return false;
    return std::fabs(p[3] - q[3]) <= 2e-3 + 2e-5 * std::fabs(q[3]);
}

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<std::uintptr_t>(p) + n <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// ------------------------------------------------------------------ layout (render thread)
// Where the view-dependent matrices of FViewMatrices are is found by value, not assumed:
// around each copy of PreViewTranslation / ViewOrigin (the exact eye position), every
// 16-byte aligned 4x4 block is compared with the matrices rebuilt from the frame's eye
// camera. This build's FViewMatrices does not follow UE 4.18's member order (LIVE: 13 or
// 14 matrices before PreViewTranslation both leave most of them unmatched).
enum Cls : int { cView, cInvView, cViewProj, cInvViewProj, cRot, cRotT, cTVP, cITVP, cClsCount };
constexpr const char* kClsName[cClsCount] = {"View",        "InvView",          "ViewProjection",          "InvViewProjection",
                                              "ViewRotation", "InvViewRotation", "TranslatedViewProjection", "InvTranslatedViewProjection"};
constexpr int kMaxSlots = 6;
constexpr std::size_t kWindow = 0x480;  // bytes before PreViewTranslation that are searched

M4 cls_matrix(const ViewSet& s, int k) {
    switch (k) {
        case cView: return s.m[kView];
        case cInvView: return s.m[kInvView];
        case cViewProj: return s.m[kViewProjection];
        case cInvViewProj: return s.m[kInvViewProjection];
        case cRot: return s.m[kTranslatedView];
        case cRotT: return s.m[kInvTranslatedView];
        case cTVP: return s.m[kTranslatedViewProjection];
        default: return s.m[kInvTranslatedViewProjection];
    }
}

// General 4x4 inverse (Gauss-Jordan with partial pivoting).
M4 inverse(const M4& a) {
    double m[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (p != c)
            for (int j = 0; j < 8; ++j) std::swap(m[p][j], m[c][j]);
        const double d = m[c][c];
        if (std::fabs(d) < 1e-30) return identity();
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = m[r][c];
                for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
            }
    }
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = m[i][j + 4];
    return r;
}

// This engine's reversed-Z infinite projection: [xs 0 0 0; 0 ys 0 0; ox oy 0 1; 0 0 n 0].
bool is_projection(const float* m) {
    return m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[6] == 0 && m[7] == 0 && m[10] == 0 && m[11] == 1.0f && m[12] == 0 &&
           m[13] == 0 && m[15] == 0 && m[0] > 0.05f && m[0] < 20.0f && m[5] > 0.05f && m[5] < 20.0f && m[14] > 0.0f && m[14] < 1000.0f;
}

struct Copy {
    std::size_t origin = 0;  // PreViewTranslation; ViewOrigin follows at +12
    std::size_t proj = 0;    // the projection matrix the view matrices were built with
    int n[cClsCount]{};
    std::size_t off[cClsCount][kMaxSlots]{};
};
struct Layout {
    bool found = false;
    int copies = 0;
    Copy copy[kMaxCopies];
    long long frustum = -1;     // FConvexVolume ViewFrustum
    int planes = 0;
    int plane_kind[6]{};        // frustum_plane index of each stored plane
    int perm_index[8]{};        // which plane each column of PermutedPlanes holds
    long long near_plane = -1;  // FPlane NearClippingPlane (-1: not found)
};
Layout g_layout;
std::uint64_t g_layout_failures = 0, g_layout_next_warn = 1;

std::atomic<bool> g_on{false};
std::atomic<bool> g_dump{false};

// Counters (render thread writes; any thread reads).
struct Stats {
    std::atomic<std::uint64_t> scenes{0}, relocated{0}, no_frame{0}, unpaired{0}, mismatch{0}, not_located{0}, unusable{0}, commit_failed{0};
    std::atomic<double> last_yaw_deg{0}, sum_abs_yaw_deg{0}, max_abs_yaw_deg{0}, sum_shift_cm{0};
    std::atomic<double> sum_ms{0}, max_ms{0};  // render thread time of the whole update, relocated frames
};
Stats g_stats;
// Pose age at the hand-over (RHI thread writes).
std::mutex g_age_mutex;
double g_age_game_sum = 0, g_age_late_sum = 0;
std::uint64_t g_age_game_n = 0, g_age_late_n = 0;

void log_failure(const std::string& why) {
    if (++g_layout_failures >= g_layout_next_warn) {
        g_layout_next_warn *= 2;
        log::warn("lateupdate: {} (search {} failed); no late update until a later stereo frame finds the view layout", why, g_layout_failures);
    }
}

bool same(const float* f, const FVector& v, bool negate) {
    const float s = negate ? -1.0f : 1.0f;
    return f[0] == s * v.X && f[1] == s * v.Y && f[2] == s * v.Z;
}

ViewSet build_for(const std::uint8_t* view, const Copy& cp, const FVector& loc, const FRotator& rot) {
    const M4 proj = load(view + cp.proj);
    return build(loc, rot, proj, inverse(proj));
}

// Largest normalised error (<= 1: equal) of the copy's matched matrices against those
// rebuilt from the eye camera.
double check_copy(const std::uint8_t* view, const Copy& cp, const FVector& loc, const FRotator& rot) {
    const ViewSet s = build_for(view, cp, loc, rot);
    double worst = 0;
    for (int k = 0; k < cClsCount; ++k) {
        const M4 m = cls_matrix(s, k);
        for (int i = 0; i < cp.n[k]; ++i) worst = std::max(worst, max_error(m, load(view + cp.off[k][i])));
    }
    return worst;
}

// Maps the matrices around one copy of PreViewTranslation (at `origin`) in both views.
bool map_copy(std::uint8_t* v[2], std::size_t origin, const device::LateCandidate& c, Copy& cp, std::string& notes) {
    cp = Copy{};
    cp.origin = origin;
    const std::size_t lo = origin > kWindow ? (origin - kWindow) & ~std::size_t{15} : 0;
    bool proj = false;
    for (std::size_t b = lo; b + 64 <= origin && !proj; b += 16)
        if (is_projection(reinterpret_cast<const float*>(v[0] + b)) && is_projection(reinterpret_cast<const float*>(v[1] + b))) {
            cp.proj = b;
            proj = true;
        }
    if (!proj) {
        notes += std::format(" | copy +0x{:x}: no projection matrix in the 0x{:x} bytes before it", origin, kWindow);
        return false;
    }
    const ViewSet s0 = build_for(v[0], cp, c.loc[0], c.rot[0]);
    const ViewSet s1 = build_for(v[1], cp, c.loc[1], c.rot[1]);
    std::string unknown;
    for (std::size_t b = lo; b + 64 <= origin; b += 16) {
        bool any = b == cp.proj;
        for (int k = 0; k < cClsCount; ++k)
            if (max_error(cls_matrix(s0, k), load(v[0] + b)) <= 1.0 && max_error(cls_matrix(s1, k), load(v[1] + b)) <= 1.0) {
                any = true;
                if (cp.n[k] < kMaxSlots) cp.off[k][cp.n[k]++] = b;
            }
        if (!any && b % 64 == cp.proj % 64) {
            const float* f = reinterpret_cast<const float*>(v[0] + b);
            unknown += std::format(" +0x{:x}[{:.3g} {:.3g} {:.3g} {:.3g} / {:.3g} {:.3g} {:.3g} {:.3g}]", b, f[0], f[1], f[2], f[3], f[12], f[13], f[14], f[15]);
        }
    }
    std::string map;
    for (int k = 0; k < cClsCount; ++k) {
        map += std::format(" {}:", kClsName[k]);
        if (!cp.n[k]) map += "-";
        for (int i = 0; i < cp.n[k]; ++i) map += std::format("{}+0x{:x}", i ? "," : "", cp.off[k][i]);
    }
    notes += std::format(" | copy +0x{:x}: projection +0x{:x};{}; blocks matching none (at the projection's 64-byte phase):{}", origin, cp.proj, map,
                         unknown.empty() ? std::string(" none") : unknown);
    return cp.n[cView] && cp.n[cInvView] && cp.n[cViewProj] && cp.n[cRot] && cp.n[cTVP];
}

void find_layout(std::uint8_t* v[2], std::size_t stride, const device::LateCandidate* cands, std::size_t n) {
    g_layout = Layout{};
    std::string notes;
    if (!readable(v[0], stride) || !readable(v[1], stride)) return log_failure("views not readable");
    for (std::size_t ci = 0; ci < n && !g_layout.found; ++ci) {
        const device::LateCandidate& c = cands[ci];
        Layout L;
        // Copies of FViewMatrices: PreViewTranslation (-origin) right before ViewOrigin (origin).
        for (std::size_t o = 0; o + 24 <= stride && L.copies < kMaxCopies; o += 4) {
            const float* a = reinterpret_cast<const float*>(v[0] + o);
            const float* b = reinterpret_cast<const float*>(v[1] + o);
            if (!same(a, c.loc[0], true) || !same(a + 3, c.loc[0], false) || !same(b, c.loc[1], true) || !same(b + 3, c.loc[1], false)) continue;
            Copy cp;
            if (map_copy(v, o, c, cp, notes)) L.copy[L.copies++] = cp;
        }
        if (!L.copies) continue;
        // The view frustum: planes rebuilt from the view-projection match the stored ones.
        const M4 vp = load(v[0] + L.copy[0].off[cViewProj][0]);
        double formula[6][4];
        bool have[6];
        for (int k = 0; k < 6; ++k) have[k] = frustum_plane(vp, k, formula[k]);
        for (std::size_t o = 0; o + kConvexSize <= stride && L.frustum < 0; o += 16) {
            const auto* num = reinterpret_cast<const std::int32_t*>(v[0] + o + kPlanesNum);
            const auto* heap = reinterpret_cast<void* const*>(v[0] + o + kPlanesHeap);
            const auto* pnum = reinterpret_cast<const std::int32_t*>(v[0] + o + kPermutedNum);
            const auto* pheap = reinterpret_cast<void* const*>(v[0] + o + kPermutedHeap);
            // A run of planes of the view-projection starts here?
            int run = 0;
            for (int i = 0; i < 6; ++i) {
                bool m = false;
                for (int k = 0; k < 6 && !m; ++k) m = have[k] && plane_equal(reinterpret_cast<const float*>(v[0] + o + i * 16), formula[k]);
                if (!m) break;
                ++run;
            }
            if (run < 4) continue;
            if (num[0] != run || num[1] < num[0] || num[1] > 64 || *heap || pnum[0] != (num[0] + 3) / 4 * 4 || pnum[1] < pnum[0] || pnum[1] > 64 ||
                *pheap) {
                notes += std::format(" | {} planes at +0x{:x}, but the arrays' headers read num {} max {} heap {} / permuted num {} max {} heap {}", run, o,
                                     num[0], num[1], *heap, pnum[0], pnum[1], *pheap);
                continue;
            }
            bool all = true;
            int kinds[6]{};
            for (int i = 0; i < num[0] && all; ++i) {
                const float* p = reinterpret_cast<const float*>(v[0] + o + i * 16);
                int kind = -1;
                for (int k = 0; k < 6 && kind < 0; ++k)
                    if (have[k] && plane_equal(p, formula[k])) kind = k;
                all = kind >= 0;
                kinds[i] = kind;
            }
            if (!all) continue;
            // PermutedPlanes: which plane each column holds (the last group is padded).
            const float* pl = reinterpret_cast<const float*>(v[0] + o);
            const float* pm = reinterpret_cast<const float*>(v[0] + o + kPermuted);
            int perm[8]{};
            for (int col = 0; col < pnum[0] && all; ++col) {
                const int g = col / 4, j = col % 4;
                int found = -1;
                for (int i = g * 4; i < num[0] && found < 0; ++i) {
                    bool eq = true;
                    for (int comp = 0; comp < 4 && eq; ++comp) eq = pm[(g * 4 + comp) * 4 + j] == pl[i * 4 + comp];
                    if (eq) found = i;
                }
                all = found >= 0;
                perm[col] = found;
            }
            if (!all) {
                notes += std::format(" | planes at +0x{:x} match, permuted planes do not", o);
                continue;
            }
            L.frustum = static_cast<long long>(o);
            L.planes = num[0];
            std::copy(kinds, kinds + 6, L.plane_kind);
            std::copy(perm, perm + 8, L.perm_index);
        }
        if (L.frustum < 0) {
            // The matrices are still updated; culling then uses the frame wait's view (logged).
            notes += " | VIEW FRUSTUM NOT FOUND (culling keeps the frame wait's view); planes of the view-projection seen at:";
            for (int k = 0; k < 6; ++k)
                for (std::size_t o = 0; o + 16 <= stride && have[k]; o += 4)
                    if (plane_equal(reinterpret_cast<const float*>(v[0] + o), formula[k])) notes += std::format(" k{}@+0x{:x}", k, o);
        }
        // The near clipping plane: the "far" formula's plane outside the frustum arrays.
        if (have[5])
            for (std::size_t o = 0; o + 16 <= stride && L.near_plane < 0; o += 16) {
                if (L.frustum >= 0 && o + 16 > static_cast<std::size_t>(L.frustum) && o < static_cast<std::size_t>(L.frustum) + kConvexSize) continue;
                if (plane_equal(reinterpret_cast<const float*>(v[0] + o), formula[5])) L.near_plane = static_cast<long long>(o);
            }
        L.found = true;
        g_layout = L;
    }
    if (!g_layout.found) return log_failure("view matrices not found or not as expected:" + (notes.empty() ? std::string(" no copy of the eye origin") : notes));
    std::string kinds;
    for (int i = 0; i < g_layout.planes; ++i) kinds += std::format("{}{}", i ? "," : "", g_layout.plane_kind[i]);
    log::info("lateupdate: FViewInfo layout found{}: {} copies of the view matrices; view frustum {} ({} planes, kinds {}); near clipping "
              "plane {}{}",
              g_layout_failures ? std::format(" after {} failed search(es)", g_layout_failures) : std::string(), g_layout.copies,
              g_layout.frustum >= 0 ? std::format("at +0x{:x}", g_layout.frustum) : std::string("NOT FOUND"), g_layout.planes, kinds,
              g_layout.near_plane >= 0 ? std::format("at +0x{:x}", g_layout.near_plane) : std::string("not found"), notes);
}

// FConvexVolume::Init: the planes in groups of four, X of four planes, then Y, Z, W; the
// last group padded with repeated planes.
void write_frustum(std::uint8_t* view, const M4& vp) {
    if (g_layout.frustum < 0) {
        if (g_layout.near_plane >= 0) {
            double p[4];
            if (frustum_plane(vp, 5, p)) {
                float* np = reinterpret_cast<float*>(view + g_layout.near_plane);
                for (int k = 0; k < 4; ++k) np[k] = static_cast<float>(p[k]);
            }
        }
        return;
    }
    float* planes = reinterpret_cast<float*>(view + g_layout.frustum);
    float* perm = reinterpret_cast<float*>(view + g_layout.frustum + kPermuted);
    for (int i = 0; i < g_layout.planes; ++i) {
        double p[4];
        if (!frustum_plane(vp, g_layout.plane_kind[i], p)) continue;
        for (int k = 0; k < 4; ++k) planes[i * 4 + k] = static_cast<float>(p[k]);
    }
    const int columns = (g_layout.planes + 3) / 4 * 4;
    for (int col = 0; col < columns; ++col) {
        const int g = col / 4, j = col % 4;
        for (int comp = 0; comp < 4; ++comp) perm[(g * 4 + comp) * 4 + j] = planes[g_layout.perm_index[col] * 4 + comp];
    }
    if (g_layout.near_plane >= 0) {
        double p[4];
        if (frustum_plane(vp, 5, p)) {
            float* np = reinterpret_cast<float*>(view + g_layout.near_plane);
            for (int k = 0; k < 4; ++k) np[k] = static_cast<float>(p[k]);
        }
    }
}

void write_view(std::uint8_t* view, const FVector& loc, const FRotator& rot) {
    M4 vp;
    for (int c = 0; c < g_layout.copies; ++c) {
        const Copy& cp = g_layout.copy[c];
        const ViewSet s = build_for(view, cp, loc, rot);
        for (int k = 0; k < cClsCount; ++k) {
            const M4 m = cls_matrix(s, k);
            for (int i = 0; i < cp.n[k]; ++i) store(m, view + cp.off[k][i]);
        }
        float* pre = reinterpret_cast<float*>(view + cp.origin);
        pre[0] = -loc.X;
        pre[1] = -loc.Y;
        pre[2] = -loc.Z;
        pre[3] = loc.X;
        pre[4] = loc.Y;
        pre[5] = loc.Z;
        if (c == 0) vp = s.m[kViewProjection];
    }
    write_frustum(view, vp);
}

bool usable(const HostView& v) {
    const float a[] = {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w,
                       v.pose.position.x,    v.pose.position.y,    v.pose.position.z};
    for (float x : a)
        if (!std::isfinite(x)) return false;
    const float n = a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3];
    return n > 0.8f && n < 1.2f && std::fabs(a[4]) < 100.0f && std::fabs(a[5]) < 100.0f && std::fabs(a[6]) < 100.0f;
}

void add(std::atomic<double>& a, double v) {
    double cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
    }
}

}  // namespace

void configure(const Config& cfg) {
    g_on = cfg.get_bool("stereo", "late_update", false);
    log::info("lateupdate: {}", g_on.load() ? "on" : "off ([stereo] late_update = 0)");
}

bool enabled() { return g_on.load(std::memory_order_relaxed); }

void before_scene(std::uint8_t* left, std::uint8_t* right, std::size_t stride) {
    const bool dump = g_dump.exchange(false);
    if (!g_on.load(std::memory_order_relaxed) && !dump) return;
    ++g_stats.scenes;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    device::LateCandidate cands[8];
    const std::size_t n = device::late_candidates(cands, 8);
    if (!n) {
        ++g_stats.no_frame;
        return;
    }
    std::uint8_t* v[2] = {left, right};
    if (dump) g_layout.found = false;  // search again and log what is found
    if (!g_layout.found) find_layout(v, stride, cands, n);
    if (!g_layout.found) return;
    // Which frame these views belong to: the eye positions are exact copies of the
    // cameras built for it.
    const float* o0 = reinterpret_cast<const float*>(left + g_layout.copy[0].origin + 12);
    const float* o1 = reinterpret_cast<const float*>(right + g_layout.copy[0].origin + 12);
    const device::LateCandidate* c = nullptr;
    for (std::size_t i = 0; i < n && !c; ++i)
        if (same(o0, cands[i].loc[0], false) && same(o1, cands[i].loc[1], false)) c = &cands[i];
    if (!c) {
        ++g_stats.unpaired;
        return;
    }
    // The views hold what this frame's cameras give (layout and math, every frame).
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < g_layout.copies; ++k) {
            if (check_copy(v[e], g_layout.copy[k], c->loc[e], c->rot[e]) > 1.0) {
                if (g_stats.mismatch++ < 3) log::warn("lateupdate: frame {} eye {}: the view's matrices are not the ones its camera gives; not updated", c->frame_id, e);
                return;
            }
        }
    if (dump) log::info("lateupdate: dump: frame {} paired, matrices checked", c->frame_id);
    if (!g_on.load(std::memory_order_relaxed)) return;
    StereoHost* host = device::host();
    HostView late[2];
    if (!host || !host->relocate_views(c->frame_id, late)) {
        ++g_stats.not_located;
        return;
    }
    if (!usable(late[0]) || !usable(late[1])) {
        ++g_stats.unusable;
        return;
    }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    FVector loc[2];
    FRotator rot[2];
    for (int e = 0; e < 2; ++e) {
        math::EyeCameraInput in = c->in[e];
        in.eye = late[e].pose;
        if (!in.positional) {
            // The head moves with the eyes (only the eye's offset from the head is used).
            const float dx = (late[0].pose.position.x + late[1].pose.position.x - c->views[0].pose.position.x - c->views[1].pose.position.x) * 0.5f;
            const float dy = (late[0].pose.position.y + late[1].pose.position.y - c->views[0].pose.position.y - c->views[1].pose.position.y) * 0.5f;
            const float dz = (late[0].pose.position.z + late[1].pose.position.z - c->views[0].pose.position.z - c->views[1].pose.position.z) * 0.5f;
            in.head.position.x += dx;
            in.head.position.y += dy;
            in.head.position.z += dz;
        }
        math::compose_eye(in, rot[e], loc[e]);
    }
    for (int e = 0; e < 2; ++e) write_view(v[e], loc[e], rot[e]);
    if (!device::late_commit(c->frame_id, c->loc[0], late, q.QuadPart)) ++g_stats.commit_failed;
    ++g_stats.relocated;
    double dyaw = rot[0].Yaw - c->rot[0].Yaw;
    while (dyaw > 180) dyaw -= 360;
    while (dyaw < -180) dyaw += 360;
    g_stats.last_yaw_deg = dyaw;
    add(g_stats.sum_abs_yaw_deg, std::fabs(dyaw));
    if (std::fabs(dyaw) > g_stats.max_abs_yaw_deg.load()) g_stats.max_abs_yaw_deg = std::fabs(dyaw);
    const double sx = loc[0].X - c->loc[0].X, sy = loc[0].Y - c->loc[0].Y, sz = loc[0].Z - c->loc[0].Z;
    add(g_stats.sum_shift_cm, std::sqrt(sx * sx + sy * sy + sz * sz));
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    add(g_stats.sum_ms, ms);
    if (ms > g_stats.max_ms.load()) g_stats.max_ms = ms;
}

void note_handover(double game_age_ms, double late_age_ms) {
    std::lock_guard lk(g_age_mutex);
    g_age_game_sum += game_age_ms;
    ++g_age_game_n;
    if (late_age_ms >= 0) {
        g_age_late_sum += late_age_ms;
        ++g_age_late_n;
    }
}

std::string command(const std::vector<std::string>& a) {
    // a[0] = "stereo", a[1] = "lateupdate"
    const std::string verb = a.size() >= 3 ? a[2] : "status";
    if (verb == "on" || verb == "off") {
        g_on = verb == "on";
        log::info("lateupdate: switched {}", verb);
    } else if (verb == "dump") {
        g_dump = true;
        return "ok the next stereo scene searches the view layout again and logs it (ff7vr.log, lines 'lateupdate:')";
    } else if (verb == "reset") {
        std::lock_guard lk(g_age_mutex);
        g_age_game_sum = g_age_late_sum = 0;
        g_age_game_n = g_age_late_n = 0;
        for (auto* c : {&g_stats.scenes, &g_stats.relocated, &g_stats.no_frame, &g_stats.unpaired, &g_stats.mismatch, &g_stats.not_located, &g_stats.unusable,
                        &g_stats.commit_failed})
            *c = 0;
        g_stats.sum_abs_yaw_deg = 0;
        g_stats.max_abs_yaw_deg = 0;
        g_stats.sum_shift_cm = 0;
        g_stats.sum_ms = 0;
        g_stats.max_ms = 0;
    } else if (verb != "status") {
        return "err usage: stereo lateupdate status | on | off | dump | reset";
    }
    double game = 0, late = 0;
    std::uint64_t gn = 0, ln = 0;
    {
        std::lock_guard lk(g_age_mutex);
        game = g_age_game_n ? g_age_game_sum / g_age_game_n : 0;
        late = g_age_late_n ? g_age_late_sum / g_age_late_n : 0;
        gn = g_age_game_n;
        ln = g_age_late_n;
    }
    const std::uint64_t r = g_stats.relocated.load();
    return std::format("ok late update {}; layout {}; stereo scenes {}, relocated {}, no queued frame {}, not paired {}, matrices not as built {}, "
                       "not located {}, unusable {}, hand-over not found {}; eye yaw change last {:.3f} avg |{:.3f}| max {:.3f} deg, eye shift avg {:.3f} cm; "
                       "pose age at the hand-over: game-thread location {:.2f} ms (n {}), late location {:.2f} ms (n {}); render thread time per "
                       "relocated frame avg {:.3f} max {:.3f} ms",
                       g_on.load() ? "on" : "off",
                       g_layout.found ? std::format("found ({} copies, frustum {})", g_layout.copies, g_layout.frustum >= 0 ? "found" : "NOT found")
                                      : std::string("not found"),
                       g_stats.scenes.load(), r, g_stats.no_frame.load(), g_stats.unpaired.load(), g_stats.mismatch.load(), g_stats.not_located.load(),
                       g_stats.unusable.load(), g_stats.commit_failed.load(), g_stats.last_yaw_deg.load(), r ? g_stats.sum_abs_yaw_deg.load() / r : 0.0,
                       g_stats.max_abs_yaw_deg.load(), r ? g_stats.sum_shift_cm.load() / r : 0.0, game, gn, late, ln,
                       r ? g_stats.sum_ms.load() / r : 0.0, g_stats.max_ms.load());
}

}  // namespace ff7vr::engine::late_update
