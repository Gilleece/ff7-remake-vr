#include "ui_align.h"

#include "stereo_device.h"
#include "ue_math.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/pattern.h"

#if FF7VR_ENGINE_WITH_RENDER
#include "ff7vr/render/render.h"
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <intrin.h>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

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
std::atomic<std::uint64_t> g_full{0}, g_aligned{0}, g_skipped{0}, g_excluded{0};

// Callers of the full pass (`uihook callers`): return addresses of the call, as offsets in
// the game's exe. A chain that contains an offset of [ui] align_skip is passed through
// unchanged (the game's own view), for callers that need it.
constexpr int kChainFrames = 6;
using Chain = std::array<std::uint32_t, kChainFrames>;
std::atomic<bool> g_capture{false};
std::atomic<bool> g_have_skip{false};
std::mutex g_chain_mutex;
std::map<Chain, std::uint64_t> g_chains;
std::set<std::uint32_t> g_skip;
std::uintptr_t g_exe_base = 0, g_exe_end = 0;

// The field-target (interaction prompt) selection test of this build, 0x1017ad0: it calls
// GetProjectionData for the full pass directly (`call [rax+0x290]` at 0x1017c5a) and keeps
// only targets inside the view's rectangle (or an ellipse around its centre). With the
// panel's view the test runs from the head through the panel's narrow frustum, so targets
// low and close in front of the character fail it. [ui] align_target_test = 0 leaves this
// one call on the game camera's view (docs/engine-module.md, "Interaction prompts").
constexpr std::uintptr_t kTargetTestReturn = 0x1017c60;
std::uintptr_t g_target_test_ret = 0;      // absolute return address (0: not found)
std::atomic<bool> g_target_test_panel{false};  // [ui] align_target_test
std::atomic<std::uint64_t> g_target_tests{0};

// `uihook targetprobe on`: a probe on the field-target classifier (0x1017ad0; it returns
// 1 = out, 2 = in range, 3 = near; its third argument is the target's location) that logs,
// for each target it tests, where the target lands on the game camera's view and on the
// panel's view (normalised to the view rectangle, as the classifier measures it), whether
// that is inside the classifier's ellipse ((y-0.5)/0.37)^2 + ((x-0.5)/0.28)^2 < 1, and the
// classifier's result. Diagnostic; off by default.
constexpr std::uintptr_t kClassifier = 0x1017ad0;
using ClassifierFn = std::uint64_t (*)(void*, float, const float*, std::uint64_t, std::uint8_t*, std::uint64_t, std::uint64_t,
                                       std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
hook::InlineHook g_probe_hook;
std::atomic<bool> g_probe_on{false};
thread_local const float* t_probe_loc = nullptr;
struct ProbeView {
    bool valid = false;
    float x = 0, y = 0, w = 0;
};
thread_local ProbeView t_probe_game, t_probe_panel;
std::mutex g_probe_mutex;
std::string g_probe_last, g_probe_key;
std::uint64_t g_probe_lines = 0;
double g_probe_next = 0;

// The point's position on the view, normalised to its rectangle (0..1, y down), as
// FSceneView::ProjectWorldToScreen and the classifier compute it.
ProbeView project(const std::uint8_t* data, const float* p) {
    ProbeView r;
    const float* o = reinterpret_cast<const float*>(data + kViewOrigin);
    const float* rm = reinterpret_cast<const float*>(data + kViewRotation);
    const float* pm = reinterpret_cast<const float*>(data + kProjection);
    const double v[4] = {p[0] - o[0], p[1] - o[1], p[2] - o[2], 0.0};
    double vr[4] = {0, 0, 0, 1};
    for (int j = 0; j < 3; ++j) vr[j] = v[0] * rm[0 * 4 + j] + v[1] * rm[1 * 4 + j] + v[2] * rm[2 * 4 + j];
    double c[4];
    for (int j = 0; j < 4; ++j) c[j] = vr[0] * pm[0 * 4 + j] + vr[1] * pm[1 * 4 + j] + vr[2] * pm[2 * 4 + j] + vr[3] * pm[3 * 4 + j];
    r.w = static_cast<float>(c[3]);
    if (!(c[3] > 0.0)) return r;
    r.x = static_cast<float>(c[0] / c[3] * 0.5 + 0.5);
    r.y = static_cast<float>(0.5 - c[1] / c[3] * 0.5);
    r.valid = true;
    return r;
}
bool in_rect(const ProbeView& v) { return v.valid && v.x >= 0 && v.x <= 1 && v.y >= 0 && v.y <= 1; }
const char* verdict(const ProbeView& v, bool ellipse);
bool in_ellipse(const ProbeView& v) {
    const double dx = (v.x - 0.5) / 0.28, dy = (v.y - 0.5) / 0.37;
    return v.valid && dx * dx + dy * dy < 1.0;
}

// "on screen" / "off screen" by the rectangle test, or the ellipse test when the target uses it.
const char* verdict(const ProbeView& v, bool ellipse) {
    if (!v.valid) return "behind";
    return (ellipse ? in_ellipse(v) : in_rect(v)) ? "passes" : "fails";
}

std::uint64_t classifier_detour(void* out, float d, const float* loc, std::uint64_t a4, std::uint8_t* param, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11) {
    t_probe_loc = loc;
    t_probe_game = ProbeView{};
    t_probe_panel = ProbeView{};
    const std::uint64_t r = g_probe_hook.original<ClassifierFn>()(out, d, loc, a4, param, a6, a7, a8, a9, a10, a11);
    t_probe_loc = nullptr;
    if (g_probe_on.load(std::memory_order_relaxed) && loc) {
        float l[3] = {0, 0, 0}, maxd = 0, margins[4] = {0, 0, 0, 0};
        std::uint8_t ellipse = 0, screen = 0;
        std::memcpy(l, loc, sizeof(l));
        if (param) {
            std::memcpy(&maxd, param + 0x10, sizeof(float));
            std::memcpy(margins, param + 0x20, sizeof(margins));
            ellipse = param[0x39];
            screen = param[0x1d];
        }
        const std::string line = std::format(
            "target ({:.0f},{:.0f},{:.0f}) d {:.0f} (max {:.0f}) screen test {} ellipse {} margins {:.2f} {:.2f} {:.2f} {:.2f} | game view ({:.2f},{:.2f}) {} | panel view ({:.2f},{:.2f}) {} | "
            "result {} on the {} view",
            l[0], l[1], l[2], std::sqrt(std::max(0.0f, d)), std::sqrt(std::max(0.0f, maxd)), screen, ellipse, margins[0], margins[1], margins[2], margins[3], t_probe_game.x, t_probe_game.y,
            verdict(t_probe_game, ellipse == 1), t_probe_panel.x, t_probe_panel.y, verdict(t_probe_panel, ellipse == 1), static_cast<int>(r & 0xff),
            g_target_test_panel.load() ? "panel's" : "game camera's");
        const double now = GetTickCount64() / 1000.0;
        // one line per change of the verdicts (inside/outside, result, view), at most every
        // 0.5 s, and every 2 s otherwise
        const std::string key = std::format("{:.0f},{:.0f} {} {} {} {}", l[0], l[1], verdict(t_probe_game, ellipse == 1),
                                            verdict(t_probe_panel, ellipse == 1), static_cast<int>(r & 0xff),
                                            g_target_test_panel.load() ? 1 : 0);
        std::lock_guard lk(g_probe_mutex);
        g_probe_last = line;
        if (now >= g_probe_next || (key != g_probe_key && now >= g_probe_next - 1.5)) {
            g_probe_next = now + 2.0;
            g_probe_key = key;
            ++g_probe_lines;
            log::info("ui: field-target probe: {}", line);
        }
    }
    return r;
}

Chain capture_chain() {
    void* frames[kChainFrames + 1]{};
    const USHORT n = RtlCaptureStackBackTrace(1, kChainFrames, frames, nullptr);
    Chain c{};
    for (USHORT i = 0; i < n && i < kChainFrames; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
        c[i] = a >= g_exe_base && a < g_exe_end ? static_cast<std::uint32_t>(a - g_exe_base) : 0xffffffffu;
    }
    return c;
}
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
        bool excluded = false;
        if (g_target_test_ret && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_target_test_ret) {
            ++g_target_tests;
            if (g_probe_on.load(std::memory_order_relaxed) && t_probe_loc) {
                float loc[3];
                std::memcpy(loc, t_probe_loc, sizeof(loc));
                t_probe_game = project(data, loc);
#if FF7VR_ENGINE_WITH_RENDER
                alignas(16) std::uint8_t copy[0x100];
                std::memcpy(copy, data, sizeof(copy));
                const std::uint64_t aligned = g_aligned.load();
                align(copy);
                if (g_aligned.load() != aligned) {
                    --g_aligned;
                    t_probe_panel = project(copy, loc);
                }
#endif
            }
            if (!g_target_test_panel.load(std::memory_order_relaxed)) excluded = true;
        }
        if (g_capture.load(std::memory_order_relaxed) || g_have_skip.load(std::memory_order_relaxed)) {
            const Chain c = capture_chain();
            std::lock_guard lk(g_chain_mutex);
            if (g_capture.load(std::memory_order_relaxed) && (g_chains.size() < 512 || g_chains.contains(c))) ++g_chains[c];
            for (const std::uint32_t r : c)
                if (r && g_skip.contains(r)) excluded = true;
        }
        if (excluded) ++g_excluded;
        else align(data);
    }
#endif
    return ok;
}

}  // namespace

void set_skip(const std::string& list) {
    std::string t = list;
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream in(t);
    std::set<std::uint32_t> s;
    for (std::string w; in >> w;) {
        const unsigned long v = std::strtoul(w.c_str(), nullptr, 16);
        if (v) s.insert(static_cast<std::uint32_t>(v));
    }
    std::lock_guard lk(g_chain_mutex);
    g_skip = std::move(s);
    g_have_skip = !g_skip.empty();
}

void set_target_test_panel(bool panel) { g_target_test_panel = panel; }

std::string callers_command(const std::string& args) {
    std::istringstream in(args);
    std::string w;
    in >> w;
    if (w == "on" || w == "off") {
        g_capture = w == "on";
        return std::format("ok full-pass caller capture {}", w);
    }
    if (w == "targettest") {
        std::string v;
        in >> v;
        if (v == "0" || v == "1") g_target_test_panel = v == "1";
        return std::format("ok field-target test call {} ({}), {} calls; it sees the {} view", g_target_test_ret ? "found" : "not found",
                           g_target_test_ret ? std::format("+0x{:x}", kTargetTestReturn) : std::string("unknown build"), g_target_tests.load(),
                           g_target_test_panel.load() ? "panel's" : "game camera's");
    }
    if (w == "targetprobe") {
        std::string v;
        in >> v;
        if (v == "on" && !g_probe_hook.installed()) {
            const auto* f = reinterpret_cast<const std::uint8_t*>(g_exe_base + kClassifier);
            static const std::uint8_t kPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10};
            if (!g_target_test_ret || std::memcmp(f, kPrologue, sizeof(kPrologue)) != 0) return "err the field-target classifier is not at its known place";
            if (!g_probe_hook.create(reinterpret_cast<void*>(g_exe_base + kClassifier), &classifier_detour)) return "err probe hook failed";
        }
        if (v == "on" || v == "off") g_probe_on = v == "on";
        std::lock_guard lk(g_probe_mutex);
        return std::format("ok field-target probe {} ({} lines) | last: {}", g_probe_on.load() ? "on" : "off", g_probe_lines, g_probe_last);
    }
    if (w == "reset") {
        std::lock_guard lk(g_chain_mutex);
        g_chains.clear();
        return "ok full-pass callers cleared";
    }
    if (w == "skip") {
        std::string rest, x;
        while (in >> x) rest += x + " ";
        set_skip(rest);
        std::lock_guard lk(g_chain_mutex);
        std::string r;
        for (const auto v : g_skip) r += std::format(" {:x}", v);
        return "ok full-pass callers passed through unchanged:" + (r.empty() ? std::string(" none") : r);
    }
    // list: the chains by count, return addresses as exe offsets (innermost first)
    std::vector<std::pair<std::uint64_t, Chain>> v;
    {
        std::lock_guard lk(g_chain_mutex);
        for (const auto& [c, n] : g_chains) v.push_back({n, c});
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string r = std::format("ok capture {} chains {} full {} aligned {} excluded {} |", g_capture.load() ? "on" : "off", v.size(), g_full.load(),
                                g_aligned.load(), g_excluded.load());
    for (const auto& [n, c] : v) {
        r += std::format(" {}x", n);
        for (const auto a : c) r += a == 0xffffffffu ? " -" : a ? std::format(" {:x}", a) : "";
        r += " |";
    }
    return r;
}

void start(std::uintptr_t base, bool known) {
    g_exe_base = base;
    if (const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base); dos && dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_exe_end = base + nt->OptionalHeader.SizeOfImage;
    }
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
    // The field-target test's call site, checked byte for byte: `xor r8d, r8d` (pass 0)
    // 0x16 bytes before the return address and `call qword ptr [rax+0x290]` right before it.
    if (known && g_exe_end > base + kTargetTestReturn) {
        static const std::uint8_t kCall[] = {0xFF, 0x90, 0x90, 0x02, 0x00, 0x00};
        static const std::uint8_t kPass0[] = {0x45, 0x33, 0xC0};
        const auto* ret = reinterpret_cast<const std::uint8_t*>(base + kTargetTestReturn);
        if (std::memcmp(ret - 6, kCall, sizeof(kCall)) == 0 && std::memcmp(ret - 0x16, kPass0, sizeof(kPass0)) == 0)
            g_target_test_ret = base + kTargetTestReturn;
    }
    if (!g_target_test_ret) log::warn("ui: field-target test call not found; [ui] align_target_test has no effect");
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
    return std::format("ok marker alignment hook {}; full-pass projections {} aligned {} passed through {} (field-target test calls {}, on the {} view); game camera FOV {:.2f} deg pitch {:.2f} "
                       "deg; last panel {:.2f} to {:.2f} deg wide, {:.2f} to {:.2f} deg high, origin {:.1f} cm from the game camera; "
                       "camera rot {:.4f} {:.4f} {:.4f} loc {:.2f} {:.2f} {:.2f}; eye base rot {:.4f} {:.4f} {:.4f} loc {:.2f} {:.2f} {:.2f}; "
                       "head pos {:.4f} {:.4f} {:.4f} quat {:.4f} {:.4f} {:.4f} {:.4f}; units per metre {:.1f}",
                       g_ready.load() ? "installed" : "off", g_full.load(), g_aligned.load(), g_skipped.load(), g_target_tests.load(),
                       g_target_test_panel.load() ? "panel's" : "game camera's", b.game_fov, b.game_rot.Pitch,
                       std::atan(g_last.l) * k, std::atan(g_last.r) * k, std::atan(g_last.b) * k, std::atan(g_last.t) * k, g_last.offset_cm,
                       b.game_rot.Pitch, b.game_rot.Yaw, b.game_rot.Roll, b.game_loc.X, b.game_loc.Y, b.game_loc.Z, b.base_level.Pitch,
                       b.base_level.Yaw, b.base_level.Roll, b.base_loc.X, b.base_loc.Y, b.base_loc.Z, b.head.position.x, b.head.position.y,
                       b.head.position.z, b.head.orientation.x, b.head.orientation.y, b.head.orientation.z, b.head.orientation.w, b.upm);
}

}  // namespace ff7vr::engine::ui_align
