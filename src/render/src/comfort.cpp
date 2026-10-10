#include "comfort.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <sstream>

namespace ff7vr::render {

namespace {

std::atomic<StickSource> g_source{nullptr};
// The PlayStation pad's sticks (NotePlayStationSticks) and the QPC of the reading.
std::atomic<float> g_psLeft{0.0f}, g_psRight{0.0f};
std::atomic<int64_t> g_psQpc{0};
constexpr double kPsStaleS = 0.25;  // an older reading counts as sticks at rest
std::atomic<float> g_sharpen{1.0f};

struct Settings {
    float vignette = 0.0f;  // strength at full stick deflection, 0 = off
    float radius = 0.55f;
    float softness = 0.25f;
    int test = 0;           // 1: as if a stick were pushed all the way
    // Fade to black on the game's camera cuts.
    int cut_fade_ms = 0;    // whole fade (out and back in), 0 = off
    float cut_fade_deg = 25.0f;
    float cut_fade_m = 1.0f;
};

std::mutex g_mutex;
Settings g_set;  // under g_mutex

// Presenting thread only.
float g_level = 0.0f;  // 0..1, follows the sticks with the fade times below
int64_t g_lastQpc = 0;
std::atomic<float> g_shownLevel{0.0f}, g_peakLevel{0.0f}, g_lastStick{0.0f};
std::atomic<uint64_t> g_framesOn{0};

// Camera cuts (NoteGameCamera on the game thread, the fade on the presenting thread).
struct CutCamera {
    bool valid = false;
    uint64_t frame = 0;
    bool scripted = false;
    float pitch = 0, yaw = 0, x = 0, y = 0, z = 0;
};
CutCamera g_cutPrev;                                // game thread
std::atomic<int64_t> g_cutStart{0};                 // QPC of the current fade's start (0 = none)
std::atomic<int> g_cutDurMs{0};                     // its length
std::atomic<uint64_t> g_cutFades{0};                // fades started by a cut (not by `comfort cut`)
std::atomic<uint64_t> g_cutTests{0};                // fades started by `comfort cut`
std::atomic<uint64_t> g_cutIgnored{0};              // jumps above the thresholds outside a scripted camera
std::atomic<uint64_t> g_cutFramesBlack{0};          // presented frames with some darkening from a fade
std::atomic<uint64_t> g_cutFramesSeen{0};           // camera frames compared
std::atomic<uint64_t> g_cutScriptedFrames{0};       // of those, frames where a scripted camera showed in both
std::atomic<float> g_cutTestYaw{0.0f};               // `comfort jump`: added to the game camera's yaw from the next frame
std::atomic<float> g_cutLastDeg{0.0f}, g_cutLastM{0.0f}, g_cutMaxDeg{0.0f}, g_cutMaxM{0.0f}, g_cutBlackShown{0.0f};

constexpr float kFadeInS = 0.10f;   // rest to full
constexpr float kFadeOutS = 0.30f;  // full to rest
constexpr float kDeadZone = 0.25f;  // of the stick's range (XInput's own dead zones are 0.24 and 0.27)

double QpcSeconds(int64_t d) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(d) / freq;
}

// Stick deflection beyond the dead zone, 0..1.
float Deflection(float mag) { return std::clamp((mag - kDeadZone) / (1.0f - kDeadZone), 0.0f, 1.0f); }

int64_t QpcNow() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return int64_t(q.QuadPart);
}

// The PlayStation pad's stick deflections if the reading is recent, else 0.
void PlayStationSticks(float* left, float* right) {
    const int64_t at = g_psQpc.load(std::memory_order_relaxed);
    const bool fresh = at != 0 && QpcSeconds(QpcNow() - at) < kPsStaleS;
    *left = fresh ? g_psLeft.load(std::memory_order_relaxed) : 0.0f;
    *right = fresh ? g_psRight.load(std::memory_order_relaxed) : 0.0f;
}

float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

int64_t MsToQpc(double ms) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return int64_t(ms / 1000.0 * freq);
}

// Starts a fade now. A cut while the image is still darkening is covered by the running
// fade; one while it brightens again turns back to black from the level shown.
void StartCutFade(int ms) {
    if (ms <= 0) return;
    const int64_t now = QpcNow();
    const int64_t start = g_cutStart.load();
    const int dur = g_cutDurMs.load();
    if (start && dur > 0) {
        const double t = QpcSeconds(now - start) * 1000.0 / dur;
        if (t < 0.5) return;
        if (t < 1.0) {
            // The same darkness at the mirrored point of a fade of the new length.
            g_cutDurMs = ms;
            g_cutStart = now - MsToQpc((1.0 - t) * ms);
            return;
        }
    }
    g_cutDurMs = ms;
    g_cutStart = now;
}

// 0..1 for the frame presented now.
float CutBlack() {
    int64_t start = g_cutStart.load();
    const int dur = g_cutDurMs.load();
    if (!start || dur <= 0) return 0.0f;
    const float t = float(QpcSeconds(QpcNow() - start) * 1000.0 / dur);
    if (t >= 1.0f) {
        g_cutStart.compare_exchange_strong(start, 0);
        return 0.0f;
    }
    return t < 0.5f ? Smooth(t * 2.0f) : Smooth((1.0f - t) * 2.0f);
}

std::string CutText(const Settings& s) {
    return std::format("cut fade {} ms (jump above {:.1f} deg or {:.2f} m while a scripted camera shows): fades {} (tests {}), jumps "
                       "outside a scripted camera {}, frames compared {} (scripted {}), frames darkened {}, last jump {:.1f} deg "
                       "{:.2f} m, largest {:.1f} deg {:.2f} m, black now {:.2f}",
                       s.cut_fade_ms, s.cut_fade_deg, s.cut_fade_m, g_cutFades.load(), g_cutTests.load(), g_cutIgnored.load(),
                       g_cutFramesSeen.load(), g_cutScriptedFrames.load(), g_cutFramesBlack.load(), g_cutLastDeg.load(), g_cutLastM.load(),
                       g_cutMaxDeg.load(), g_cutMaxM.load(), g_cutBlackShown.load());
}

std::string Text(const Settings& s) {
    const int64_t at = g_psQpc.load();
    const std::string ps = at ? std::format("; PlayStation pad sticks {:.2f} {:.2f} ({:.0f} ms ago)", g_psLeft.load(), g_psRight.load(),
                                            QpcSeconds(QpcNow() - at) * 1000.0)
                              : std::string("; no PlayStation pad reading");
    return std::format("vignette {:.2f} radius {:.2f} softness {:.2f}{}; level now {:.2f}, peak since last status {:.2f}, last stick {:.2f}, "
                       "frames darkened {}{}",
                       s.vignette, s.radius, s.softness, s.test ? " (test: forced on)" : "", g_shownLevel.load(), g_peakLevel.exchange(0.0f),
                       g_lastStick.load(), g_framesOn.load(), ps) +
           "; " + CutText(s);
}

}  // namespace

void SetStickSource(StickSource source) { g_source = source; }

void NoteGameCamera(uint64_t frame, const float rotation[2], const float location_m[3], bool scripted) {
    CutCamera c;
    c.valid = true;
    c.frame = frame;
    c.scripted = scripted;
    c.pitch = rotation[0];
    c.yaw = rotation[1] + g_cutTestYaw.load(std::memory_order_relaxed);
    c.x = location_m[0];
    c.y = location_m[1];
    c.z = location_m[2];
    const CutCamera p = g_cutPrev;
    g_cutPrev = c;
    if (!p.valid || p.frame + 1 != frame) return;  // not consecutive frames (stereo was off, a loading screen)
    // The angle between the two view directions (yaw and pitch; roll is ignored).
    auto dir = [](float pitch, float yaw, double out[3]) {
        const double kRad = 3.14159265358979323846 / 180.0;
        const double cp = std::cos(pitch * kRad);
        out[0] = cp * std::cos(yaw * kRad);
        out[1] = cp * std::sin(yaw * kRad);
        out[2] = std::sin(pitch * kRad);
    };
    double a[3], b[3];
    dir(p.pitch, p.yaw, a);
    dir(c.pitch, c.yaw, b);
    const double dot = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0);
    const float deg = float(std::acos(dot) * 180.0 / 3.14159265358979323846);
    const float dx = c.x - p.x, dy = c.y - p.y, dz = c.z - p.z;
    const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
    g_cutFramesSeen.fetch_add(1, std::memory_order_relaxed);
    const bool both = scripted && p.scripted;
    if (both) g_cutScriptedFrames.fetch_add(1, std::memory_order_relaxed);
    Settings s;
    {
        std::lock_guard lk(g_mutex);
        s = g_set;
    }
    if (deg <= s.cut_fade_deg && m <= s.cut_fade_m) return;
    g_cutLastDeg = deg;
    g_cutLastM = m;
    if (deg > g_cutMaxDeg.load()) g_cutMaxDeg = deg;
    if (m > g_cutMaxM.load()) g_cutMaxM = m;
    if (!both) {
        ++g_cutIgnored;
        log::debug("comfort: camera jump {:.1f} deg {:.2f} m outside a scripted camera: no fade", deg, m);
        return;
    }
    if (s.cut_fade_ms <= 0) return;
    ++g_cutFades;
    StartCutFade(s.cut_fade_ms);
    log::info("comfort: camera cut {:.1f} deg {:.2f} m (frame {}): fade to black over {} ms (fade {})", deg, m, frame, s.cut_fade_ms,
              g_cutFades.load());
}

void NotePlayStationSticks(float left, float right) {
    g_psLeft.store(std::clamp(left, 0.0f, 1.0f), std::memory_order_relaxed);
    g_psRight.store(std::clamp(right, 0.0f, 1.0f), std::memory_order_relaxed);
    g_psQpc.store(QpcNow(), std::memory_order_relaxed);
}

namespace comfort {

void Start(const Config& c) {
    std::lock_guard lk(g_mutex);
    g_set.vignette = static_cast<float>(std::clamp(c.get_float("comfort", "vignette", 0.0), 0.0, 1.0));
    g_set.radius = static_cast<float>(std::clamp(c.get_float("comfort", "vignette_radius", 0.55), 0.1, 2.0));
    g_set.softness = static_cast<float>(std::clamp(c.get_float("comfort", "vignette_softness", 0.25), 0.01, 2.0));
    log::info("comfort: vignette {:.2f} radius {:.2f} softness {:.2f}{}", g_set.vignette, g_set.radius, g_set.softness,
              g_set.vignette > 0.0f ? " (darkens the periphery while the sticks move or turn the view)" : " (off)");
    g_set.cut_fade_ms = static_cast<int>(std::clamp(c.get_int("comfort", "cut_fade_ms", 0), 0LL, 2000LL));
    g_set.cut_fade_deg = static_cast<float>(std::clamp(c.get_float("comfort", "cut_fade_deg", 25.0), 0.1, 180.0));
    g_set.cut_fade_m = static_cast<float>(std::clamp(c.get_float("comfort", "cut_fade_m", 1.0), 0.01, 100.0));
    log::info("comfort: cut fade {} ms, on a jump of the game camera above {:.0f} deg or {:.2f} m while a scripted camera shows{}",
              g_set.cut_fade_ms, g_set.cut_fade_deg, g_set.cut_fade_m, g_set.cut_fade_ms > 0 ? "" : " (off)");
    dev_commands::add("comfort",
                      "comfort status | vignette <0..1> | radius <half-heights> | softness <half-heights> | test 0|1: the comfort vignette "
                      "while the sticks move or turn the view; cutfade <ms> | cutdeg <deg> | cutm <m>: the fade to black on the game's "
                      "camera cuts; cut [ms]: one fade now (test); jump <deg>: the camera as the cut detection sees it turns by "
                      "<deg> (test)",
                      [](std::string_view args) { return Command(std::string(args)); });
    g_sharpen = static_cast<float>(std::clamp(c.get_float("picture", "sharpen", 1.0), 0.0, 1.0));
    log::info("render: picture sharpen {:.2f}", g_sharpen.load());
    dev_commands::add("sharpen", "sharpen [0..1]: the unsharp mask of the eye images ([picture] sharpen); no value = status",
                      [](std::string_view args) {
                          const std::string a(args);
                          if (!a.empty()) {
                              char* end = nullptr;
                              const float v = std::strtof(a.c_str(), &end);
                              if (!end || *end) return std::string("err usage: sharpen [0..1]");
                              g_sharpen = std::clamp(v, 0.0f, 1.0f);
                              log::info("render: picture sharpen {:.2f}", g_sharpen.load());
                          }
                          return std::format("ok sharpen {:.2f}", g_sharpen.load());
                      });
}

float Sharpen() { return g_sharpen.load(std::memory_order_relaxed); }

int CutFadeMs() {
    std::lock_guard lk(g_mutex);
    return g_set.cut_fade_ms;
}

float VignetteStrength() {
    std::lock_guard lk(g_mutex);
    return g_set.vignette;
}

xr::Vignette Update(const xr::Fov fov[2]) {
    Settings s;
    {
        std::lock_guard lk(g_mutex);
        s = g_set;
    }
    const int64_t now = QpcNow();
    const float dt = g_lastQpc ? std::clamp(float(QpcSeconds(now - g_lastQpc)), 0.0f, 0.1f) : 0.0f;
    g_lastQpc = now;

    float target = 0.0f;
    if (s.vignette > 0.0f) {
        float l = 0.0f, r = 0.0f, psl = 0.0f, psr = 0.0f;
        if (const StickSource src = g_source.load()) src(&l, &r);
        PlayStationSticks(&psl, &psr);  // either pad moves the view
        l = std::max(l, psl);
        r = std::max(r, psr);
        g_lastStick = std::max(l, r);
        target = s.test ? 1.0f : std::max(Deflection(l), Deflection(r));
    }
    if (target > g_level)
        g_level = std::min(target, g_level + dt / kFadeInS);
    else
        g_level = std::max(target, g_level - dt / kFadeOutS);
    g_shownLevel = g_level;
    if (g_level > g_peakLevel.load()) g_peakLevel = g_level;

    xr::Vignette v;
    v.black = CutBlack();
    g_cutBlackShown = v.black;
    if (v.black > 0.0f) ++g_cutFramesBlack;
    if (g_level <= 0.0f) return v;
    ++g_framesOn;
    // Ease the level so the fade starts and ends softly.
    const float eased = g_level * g_level * (3.0f - 2.0f * g_level);
    v.strength = s.vignette * eased;
    v.radius = s.radius;
    v.softness = s.softness;
    for (int e = 0; e < 2; ++e) {
        // Where the view axis meets the image: asymmetric headset FOVs put it off-centre.
        const float l = std::tan(fov[e].angleLeft), r = std::tan(fov[e].angleRight);
        const float u = std::tan(fov[e].angleUp), d = std::tan(fov[e].angleDown);
        v.centre[e][0] = r - l > 1e-4f ? -l / (r - l) : 0.5f;
        v.centre[e][1] = u - d > 1e-4f ? u / (u - d) : 0.5f;
    }
    return v;
}

std::string Command(const std::string& args) {
    std::istringstream in(args);
    std::string key, value;
    in >> key >> value;
    const char* usage =
        "err usage: comfort status | vignette <0..1> | radius <r> | softness <s> | test 0|1 | cutfade <ms> | cutdeg <deg> | cutm <m> | "
        "cut [ms] | jump <deg>";
    std::lock_guard lk(g_mutex);
    if (key.empty() || key == "status") return "ok comfort " + Text(g_set);
    if (key == "cut") {
        // A test fade: the given length, else cut_fade_ms, else 150 ms.
        int ms = g_set.cut_fade_ms > 0 ? g_set.cut_fade_ms : 150;
        if (!value.empty()) {
            char* e = nullptr;
            const long v = std::strtol(value.c_str(), &e, 10);
            if (!e || *e || v <= 0 || v > 10000) return usage;
            ms = int(v);
        }
        ++g_cutTests;
        StartCutFade(ms);
        log::info("comfort: test fade to black over {} ms", ms);
        return std::format("ok comfort test fade {} ms", ms);
    }
    if (key == "jump") {
        // Test of the detection: the game camera as seen by it turns by <deg> from the next
        // frame on (an emulated jump; the fade follows only while a scripted camera shows).
        char* e = nullptr;
        const float deg = std::strtof(value.c_str(), &e);
        if (value.empty() || !e || *e) return usage;
        const float total = g_cutTestYaw.load() + deg;
        g_cutTestYaw = std::fabs(total) < 1e-3f ? 0.0f : total;
        log::info("comfort: test camera jump {:.1f} deg (offset now {:.1f} deg)", deg, g_cutTestYaw.load());
        return std::format("ok comfort test jump {:.1f} deg, offset {:.1f} deg", deg, g_cutTestYaw.load());
    }
    char* end = nullptr;
    const float v = std::strtof(value.c_str(), &end);
    if (value.empty() || !end || *end) return usage;
    if (key == "vignette") g_set.vignette = std::clamp(v, 0.0f, 1.0f);
    else if (key == "radius") g_set.radius = std::clamp(v, 0.1f, 2.0f);
    else if (key == "softness") g_set.softness = std::clamp(v, 0.01f, 2.0f);
    else if (key == "test") g_set.test = v != 0.0f ? 1 : 0;
    else if (key == "cutfade") g_set.cut_fade_ms = int(std::clamp(v, 0.0f, 2000.0f));
    else if (key == "cutdeg") g_set.cut_fade_deg = std::clamp(v, 0.1f, 180.0f);
    else if (key == "cutm") g_set.cut_fade_m = std::clamp(v, 0.01f, 100.0f);
    else return usage;
    const std::string text = Text(g_set);
    log::info("comfort: {}", text);
    return "ok comfort " + text;
}

}  // namespace comfort
}  // namespace ff7vr::render
