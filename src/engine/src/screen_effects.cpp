#include "screen_effects.h"

#include "stereo_device.h"
#include "uobj.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

namespace ff7vr::engine::screen_effects {
namespace {

// FSceneView::FinalPostProcessSettings (build 1.0.0.7): EndFinalPostprocessSettings stores the
// final SceneFringeIntensity at view+0xE68 and BloomIntensity at view+0xE70, which are +0x228 and
// +0x230 of FPostProcessSettings (reflection).
constexpr std::size_t kFinalSettings = 0xC40;

// Fields of FPostProcessSettings (build 1.0.0.7; offsets from the reflection data, checked
// again at run time by check_layout()).
struct Field {
    const char* name;
    std::size_t offset;
    float neutral;  // the constructor's default
    bool removed;   // zeroed in stereo
};
constexpr std::array<Field, 9> kFields{{
    {"SceneFringeIntensity", 0x228, 0.0f, true},
    {"ChromaticAberrationIntensity", 0x42c, 0.0f, true},
    {"ChromaticSaturation", 0x430, 1.0f, false},
    {"DirectionalBlurIntensity", 0x54c, 1.0f, false},
    {"DirectionalBlurDistortionIntensity", 0x550, 0.0f, true},
    {"DirectionalBlurDistortionGradient", 0x554, 0.0f, false},
    {"DirectionalBlurDistortionField", 0x558, 1.0f, false},
    {"DirectionalBlurDistortionFrequency", 0x55c, 0.0f, false},
    {"DirectionalBlurDistortionPosition", 0x560, 0.0f, false},
}};
constexpr std::size_t kFringe = 0, kAberration = 1, kBlur = 4;

using EndFinalFn = void(__fastcall*)(void* view, void* init_options);
hook::InlineHook g_hook;
std::atomic<bool> g_remove{true};
std::atomic<bool> g_layout_ok{false};    // offsets usable (known build, or confirmed by reflection)
std::atomic<int> g_layout_checked{0};    // 0 = not yet, 1 = confirmed, -1 = differs, -2 = struct not found
bool g_known_build = false;

std::atomic<std::uint64_t> g_calls{0}, g_stereo_calls{0}, g_views_removed{0};

// A run of frames in which the game applied one of the effects (for the log).
struct Episode {
    bool active = false;
    bool stereo = false;
    std::chrono::steady_clock::time_point start{}, last{};
    float max_blur = 0.0f, max_aberration = 0.0f, max_fringe = 0.0f;
    std::uint64_t views = 0, removed_views = 0;
};
std::mutex g_mutex;  // g_episode, g_force, g_last
Episode g_episode;
std::array<float, kFields.size()> g_last{};  // the last view's values before removal
std::uint64_t g_episodes = 0;

struct Forced {
    std::size_t offset = 0;
    float value = 0.0f;
};
std::array<Forced, 4> g_force{};
std::atomic<int> g_force_count{0};

float read_f(const std::uint8_t* s, std::size_t off) {
    float v = 0.0f;
    std::memcpy(&v, s + off, sizeof v);
    return v;
}
void write_f(std::uint8_t* s, std::size_t off, float v) { std::memcpy(s + off, &v, sizeof v); }

std::string values_text(const std::array<float, kFields.size()>& v) {
    std::string r;
    for (std::size_t i = 0; i < kFields.size(); ++i)
        if (v[i] != kFields[i].neutral) r += std::format(" {} {:.3f}", kFields[i].name, v[i]);
    return r.empty() ? " (all at their defaults)" : r;
}

// The first runs are logged in full (start and end); after that every 25th end line only, with
// the count, so a long battle does not fill the log.
constexpr std::uint64_t kLoggedRuns = 10, kLogEvery = 25;
bool log_run(std::uint64_t n) { return n <= kLoggedRuns || n % kLogEvery == 0; }

void end_episode_locked() {
    if (log_run(g_episodes)) {
        const double secs = std::chrono::duration<double>(g_episode.last - g_episode.start).count();
        log::info("screen effects: the game's screen effect ended after {:.1f} s ({} views; largest screen blur {:.3f}, colour separation {:.3f}, "
                  "fringe {:.3f}; {}; {} run(s) so far)",
                  secs, g_episode.views, g_episode.max_blur, g_episode.max_aberration, g_episode.max_fringe,
                  g_episode.removed_views == g_episode.views ? std::string("removed in 3D")
                  : g_episode.removed_views > 0 ? std::format("removed in {} of the views", g_episode.removed_views)
                  : g_episode.stereo            ? std::string("kept: [comfort] screen_blur = 1")
                                                : std::string("flat view, kept"),
                  g_episodes);
    }
    g_episode = Episode{};
}

// Called for every view after the game's own EndFinalPostprocessSettings. Game thread.
void process(std::uint8_t* settings) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const bool stereo = device::presenting();
    if (stereo) g_stereo_calls.fetch_add(1, std::memory_order_relaxed);

    // The view's memory is only touched outside the lock (the caller guards against faults, and
    // a fault must not leave the mutex held).
    if (g_force_count.load(std::memory_order_acquire) > 0) {
        std::array<Forced, 4> forced{};
        int n = 0;
        {
            std::lock_guard lk(g_mutex);
            n = g_force_count.load();
            forced = g_force;
        }
        for (int i = 0; i < n; ++i) write_f(settings, forced[i].offset, forced[i].value);
    }

    std::array<float, kFields.size()> v{};
    for (std::size_t i = 0; i < kFields.size(); ++i) v[i] = read_f(settings, kFields[i].offset);
    const bool applied = v[kBlur] > 0.0f || v[kAberration] > 0.0f || v[kFringe] > 0.0f;
    const bool remove = stereo && g_remove.load(std::memory_order_relaxed);
    if (applied && remove) {
        for (const Field& f : kFields)
            if (f.removed) write_f(settings, f.offset, 0.0f);
        g_views_removed.fetch_add(1, std::memory_order_relaxed);
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk(g_mutex);
    g_last = v;
    if (applied) {
        if (!g_episode.active) {
            g_episode.active = true;
            g_episode.start = now;
            g_episode.stereo = stereo;
            ++g_episodes;
            if (g_episodes <= kLoggedRuns)
                log::info("screen effects: the game applies a screen effect{}; {}", values_text(v),
                          remove ? "removed in 3D" : (stereo ? "kept ([comfort] screen_blur = 1)" : "flat view, kept"));
        }
        g_episode.stereo = g_episode.stereo || stereo;
        g_episode.last = now;
        g_episode.max_blur = std::max(g_episode.max_blur, v[kBlur]);
        g_episode.max_aberration = std::max(g_episode.max_aberration, v[kAberration]);
        g_episode.max_fringe = std::max(g_episode.max_fringe, v[kFringe]);
        ++g_episode.views;
        if (remove) ++g_episode.removed_views;
    } else if (g_episode.active && now - g_episode.last > std::chrono::seconds(1)) {
        end_episode_locked();
    }
}

void __fastcall end_final_detour(void* view, void* init_options) {
    g_hook.original<EndFinalFn>()(view, init_options);
    if (!view || !g_layout_ok.load(std::memory_order_relaxed)) return;
    __try {
        process(static_cast<std::uint8_t*>(view) + kFinalSettings);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Reflection walk (UE 4.18): UStruct::Children +0x38, UField::Next +0x28,
// UProperty::Offset_Internal +0x44.
std::string check_layout_text(int& result) {
    result = -2;
    if (!uobj::available()) return "object array not available";
    const auto found = uobj::find_all("PostProcessSettings", {}, "ScriptStruct", 4);
    if (found.empty()) return "ScriptStruct PostProcessSettings not found";
    void* ustruct = found[0].obj;
    std::array<std::int32_t, kFields.size()> seen;
    seen.fill(-1);
    int props = 0;
    void* f = nullptr;
    if (!uobj::read(static_cast<std::uint8_t*>(ustruct) + 0x38, f)) return "cannot read the struct's properties";
    for (int guard = 0; f && guard < 2048; ++guard, ++props) {
        const std::string name = uobj::object_name(f);
        std::int32_t off = -1;
        uobj::read(static_cast<std::uint8_t*>(f) + 0x44, off);
        for (std::size_t i = 0; i < kFields.size(); ++i)
            if (name == kFields[i].name) seen[i] = off;
        if (!uobj::read(static_cast<std::uint8_t*>(f) + 0x28, f)) break;
    }
    std::string r = std::format("{} ({} properties):", uobj::path_of(ustruct), props);
    bool ok = true;
    for (std::size_t i = 0; i < kFields.size(); ++i) {
        const bool same = seen[i] == static_cast<std::int32_t>(kFields[i].offset);
        ok = ok && same;
        r += std::format(" {} {:#x}{}", kFields[i].name, seen[i], same ? "" : std::format(" (expected {:#x})", kFields[i].offset));
    }
    result = ok ? 1 : -1;
    return r;
}

}  // namespace

bool init(std::uintptr_t end_final, bool known_build, bool remove) {
    set_remove(remove);
    g_known_build = known_build;
    g_layout_ok = known_build;
    if (!end_final) {
        log::warn("screen effects: EndFinalPostprocessSettings not found; the game's screen blur and colour fringe stay in stereo");
        return false;
    }
    if (!known_build) {
        // The field offsets were read on build 1.0.0.7; another build needs them confirmed by
        // the reflection data (`stereo screenfx layout`) before anything is changed.
        log::warn("screen effects: not the known build; run 'stereo screenfx layout' to confirm the post-process field offsets");
    }
    if (!g_hook.create(end_final, &end_final_detour)) {
        log::warn("screen effects: could not hook EndFinalPostprocessSettings; the game's screen blur and colour fringe stay in stereo");
        return false;
    }
    log::info("screen effects: hook installed; in stereo the game's screen blur and colour fringe are {}",
              remove ? "removed ([comfort] screen_blur = 0)" : "kept ([comfort] screen_blur = 1)");
    return true;
}

void set_remove(bool remove) { g_remove.store(remove, std::memory_order_relaxed); }

bool remove() { return g_remove.load(std::memory_order_relaxed); }

std::string status() {
    std::lock_guard lk(g_mutex);
    std::string r = std::format(
        "screen effects: {}, {} in stereo; layout {}; views {}, in stereo {}, with an effect removed {}; runs with an effect {}{}; last view:{}",
        g_hook.installed() ? "hooked" : "not hooked", g_remove.load() ? "removed" : "kept",
        g_layout_checked.load() == 1    ? "confirmed by reflection"
        : g_layout_checked.load() == -1 ? "DIFFERS from reflection (off)"
        : g_layout_ok.load()            ? "known build"
                                        : "unconfirmed (off)",
        g_calls.load(), g_stereo_calls.load(), g_views_removed.load(), g_episodes, g_episode.active ? " (one now)" : "", values_text(g_last));
    if (const int n = g_force_count.load(); n > 0) {
        r += "; forced:";
        for (int i = 0; i < n; ++i) r += std::format(" {:#x}={:.3f}", g_force[i].offset, g_force[i].value);
    }
    return r;
}

std::string layout() {
    int result = 0;
    const std::string text = check_layout_text(result);
    g_layout_checked = result;
    if (result == 1) g_layout_ok = true;
    else if (result == -1) g_layout_ok = false;
    log::info("screen effects: layout {}: {}", result == 1 ? "confirmed" : result == -1 ? "DIFFERS" : "not checked", text);
    return std::format("{} {}", result == 1 ? "confirmed" : result == -1 ? "differs" : "not checked", text);
}

namespace {

// Float and bool properties of an object (its class chain, up to UObject), and the float
// properties of objects its struct properties point to (for example a distribution's constant).
std::string object_values(void* obj, int depth) {
    std::string r;
    for (void* c = uobj::class_of(obj); c && depth < 3; c = uobj::super_of(c)) {
        const std::string cname = uobj::object_name(c);
        if (cname == "Object" || cname == "ParticleModule") break;
        void* f = nullptr;
        if (!uobj::read(static_cast<std::uint8_t*>(c) + 0x38, f)) break;
        for (int guard = 0; f && guard < 256; ++guard) {
            const std::string name = uobj::object_name(f);
            const std::string type = uobj::object_name(uobj::class_of(f));
            std::int32_t off = -1, size = 0;
            uobj::read(static_cast<std::uint8_t*>(f) + 0x44, off);
            uobj::read(static_cast<std::uint8_t*>(f) + 0x34, size);
            const auto* at = static_cast<const std::uint8_t*>(obj) + off;
            if (off >= 0 && type == "FloatProperty") {
                float v = 0.0f;
                if (uobj::read(at, v)) r += std::format(" {}={:.3f}", name, v);
            } else if (off >= 0 && type == "BoolProperty") {
                std::uint8_t byte_off = 0, mask = 0, v = 0;
                uobj::read(static_cast<std::uint8_t*>(f) + 0x71, byte_off);
                uobj::read(static_cast<std::uint8_t*>(f) + 0x73, mask);
                if (uobj::read(at + byte_off, v) && (v & mask)) r += std::format(" {}=1", name);
            } else if (off >= 0 && type == "StructProperty" && size > 0 && size <= 0x100) {
                // a distribution: its first words as floats (when any is set), and the objects it
                // points to, with their own float values
                std::string words;
                bool any = false;
                for (std::int32_t q = 0; q + 4 <= size && q < 0x40; q += 4) {
                    std::uint32_t u = 0;
                    if (!uobj::read(at + q, u)) break;
                    float fv = 0.0f;
                    std::memcpy(&fv, &u, sizeof fv);
                    any = any || u != 0;
                    // a float of plausible size (or 0) as a number, anything else in hex
                    const std::uint32_t e = u >> 24;
                    const bool plausible = u == 0 || (e >= 0x38 && e <= 0x47) || (e >= 0xb8 && e <= 0xc7);
                    words += plausible
                                 ? std::format("{}{:g}", words.empty() ? "" : ",", fv)
                                 : std::format("{}#{:x}", words.empty() ? "" : ",", u);
                }
                if (any) r += std::format(" {}{{{}}}", name, words);
                for (std::int32_t q = 0; q + 8 <= size; q += 8) {
                    void* p = nullptr;
                    if (!uobj::read(at + q, p) || !p || !uobj::alive(p)) continue;
                    r += std::format(" {}=[{}{}]", name, uobj::object_name(uobj::class_of(p)), object_values(p, depth + 1));
                }
            }
            if (!uobj::read(static_cast<std::uint8_t*>(f) + 0x28, f)) break;
        }
    }
    return r;
}

}  // namespace

std::string vfx(const std::string& name) {
    if (!uobj::available()) return "object array not available";
    std::string r;
    int n = 0;
    const int count = uobj::num_objects();
    for (int i = 0; i < count && n < 24; ++i) {
        void* o = uobj::object_at(i);
        if (!o) continue;
        const std::string cls = uobj::object_name(uobj::class_of(o));
        if (cls.find("PostProcess") == std::string::npos) continue;
        const std::string path = uobj::path_of(o);
        if (path.find(name) == std::string::npos || path.find("Default__") != std::string::npos) continue;
        ++n;
        r += std::format(" | {} {}:{}", cls, path, object_values(o, 0));
    }
    log::info("screen effects: post-process objects under {}: {}{}", name, n, r);
    return std::format("{} post-process object(s) under {}{}", n, name, r);
}

std::string force(const std::string& offset, const std::string& value) {
    std::lock_guard lk(g_mutex);
    if (offset == "clear") {
        g_force_count = 0;
        return "forced values cleared";
    }
    const std::size_t off = std::strtoul(offset.c_str(), nullptr, 16);
    if (off < 0x10 || off > 0x5f0 || (off & 3)) return "err offset: a float of FPostProcessSettings (hex, 4-byte aligned)";
    const float v = static_cast<float>(std::atof(value.c_str()));
    int n = g_force_count.load();
    int i = 0;
    while (i < n && g_force[i].offset != off) ++i;
    if (i == n) {
        if (n == static_cast<int>(g_force.size())) return "err four forced values already";
        ++n;
    }
    g_force[i] = {off, v};
    g_force_count.store(n, std::memory_order_release);
    return std::format("forced {:#x} = {:.3f} in every view's final settings", off, v);
}

}  // namespace ff7vr::engine::screen_effects
