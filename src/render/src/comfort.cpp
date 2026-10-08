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
std::atomic<float> g_sharpen{0.0f};

struct Settings {
    float vignette = 0.0f;  // strength at full stick deflection, 0 = off
    float radius = 0.55f;
    float softness = 0.25f;
    int test = 0;           // 1: as if a stick were pushed all the way
};

std::mutex g_mutex;
Settings g_set;  // under g_mutex

// Presenting thread only.
float g_level = 0.0f;  // 0..1, follows the sticks with the fade times below
int64_t g_lastQpc = 0;
std::atomic<float> g_shownLevel{0.0f}, g_peakLevel{0.0f}, g_lastStick{0.0f};
std::atomic<uint64_t> g_framesOn{0};

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

std::string Text(const Settings& s) {
    return std::format("vignette {:.2f} radius {:.2f} softness {:.2f}{}; level now {:.2f}, peak since last status {:.2f}, last stick {:.2f}, "
                       "frames darkened {}",
                       s.vignette, s.radius, s.softness, s.test ? " (test: forced on)" : "", g_shownLevel.load(), g_peakLevel.exchange(0.0f),
                       g_lastStick.load(), g_framesOn.load());
}

}  // namespace

void SetStickSource(StickSource source) { g_source = source; }

namespace comfort {

void Start(const Config& c) {
    std::lock_guard lk(g_mutex);
    g_set.vignette = static_cast<float>(std::clamp(c.get_float("comfort", "vignette", 0.0), 0.0, 1.0));
    g_set.radius = static_cast<float>(std::clamp(c.get_float("comfort", "vignette_radius", 0.55), 0.1, 2.0));
    g_set.softness = static_cast<float>(std::clamp(c.get_float("comfort", "vignette_softness", 0.25), 0.01, 2.0));
    log::info("comfort: vignette {:.2f} radius {:.2f} softness {:.2f}{}", g_set.vignette, g_set.radius, g_set.softness,
              g_set.vignette > 0.0f ? " (darkens the periphery while the sticks move or turn the view)" : " (off)");
    dev_commands::add("comfort",
                      "comfort status | vignette <0..1> | radius <half-heights> | softness <half-heights> | test 0|1: the comfort vignette "
                      "while the sticks move or turn the view",
                      [](std::string_view args) { return Command(std::string(args)); });
    g_sharpen = static_cast<float>(std::clamp(c.get_float("picture", "sharpen", 0.0), 0.0, 1.0));
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

xr::Vignette Update(const xr::Fov fov[2]) {
    Settings s;
    {
        std::lock_guard lk(g_mutex);
        s = g_set;
    }
    const int64_t now = [] {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        return int64_t(q.QuadPart);
    }();
    const float dt = g_lastQpc ? std::clamp(float(QpcSeconds(now - g_lastQpc)), 0.0f, 0.1f) : 0.0f;
    g_lastQpc = now;

    float target = 0.0f;
    if (s.vignette > 0.0f) {
        float l = 0.0f, r = 0.0f;
        if (const StickSource src = g_source.load()) src(&l, &r);
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
    const char* usage = "err usage: comfort status | vignette <0..1> | radius <r> | softness <s> | test 0|1";
    std::lock_guard lk(g_mutex);
    if (key.empty() || key == "status") return "ok comfort " + Text(g_set);
    char* end = nullptr;
    const float v = std::strtof(value.c_str(), &end);
    if (value.empty() || !end || *end) return usage;
    if (key == "vignette") g_set.vignette = std::clamp(v, 0.0f, 1.0f);
    else if (key == "radius") g_set.radius = std::clamp(v, 0.1f, 2.0f);
    else if (key == "softness") g_set.softness = std::clamp(v, 0.01f, 2.0f);
    else if (key == "test") g_set.test = v != 0.0f ? 1 : 0;
    else return usage;
    const std::string text = Text(g_set);
    log::info("comfort: {}", text);
    return "ok comfort " + text;
}

}  // namespace comfort
}  // namespace ff7vr::render
