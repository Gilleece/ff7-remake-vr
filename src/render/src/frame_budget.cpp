#include "frame_budget.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <mutex>

namespace ff7vr::render {
namespace {
std::atomic<FrameBudgetFactsFn> g_factsFn{nullptr};
}  // namespace

void SetFrameBudgetFacts(FrameBudgetFactsFn fn) { g_factsFn = fn; }
std::string frame_budget_summary() { return frame_budget::Summary(); }

namespace frame_budget {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kOverFraction = 1.05;                 // over budget: median interval above the period by more than 5 %
constexpr int kWindowsToWarn = 2;                      // consecutive windows over budget before the warning
constexpr auto kHoldOff = std::chrono::seconds(30);    // after a load, a stall, a mode change, the start
constexpr auto kWarnEvery = std::chrono::seconds(60);  // at most one warning a minute
constexpr double kGpuBound = 0.85;                     // GPU total at least this share of the interval
constexpr double kSlowUploadGBps = 0.5;               // transfer-bound: the upload check below this
constexpr double kGpuLow = 0.70;                       // game-thread-bound: GPU total below this share ...
constexpr double kWaitLow = 0.30;                      // ... and the frame wait below this share
constexpr double kSceneShare = 0.80;                   // stereo gameplay: the 3D scene rendered in this share of the frames
constexpr double kStallMs = 500.0;                     // a frame this long counts as a load or a stall

std::atomic<bool> g_on{true};
std::atomic<int64_t> g_modeChangeNs{0}, g_captureNs{0};

std::mutex g_mutex;  // everything below
Clock::time_point g_disturbed = Clock::now();  // the newest load, stall or mode change (or the start)
std::string g_disturbedWhy = "start";
Clock::time_point g_lastWarn{};
bool g_everWarned = false;
bool g_warned = false;  // a warning is in effect (the INFO line ends it)
int g_overWindows = 0;
uint64_t g_warnings = 0, g_suppressed = 0, g_evaluated = 0;
FrameBudgetFacts g_lastFacts{};
bool g_haveFacts = false;
std::string g_summary = "not measured yet (no stereo gameplay)";
std::string g_lastWindow = "none";

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
Clock::time_point FromNs(int64_t ns) { return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ns))); }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Verdict {
    std::string kind;    // "GPU-bound", "game-thread-bound", "runtime-bound", "cause unclear"
    std::string detail;  // the parts with their times
    std::string advice;  // one suggestion
    std::string brief;   // for the summary line
};

Verdict Classify(const Window& w, double interval, bool haveGpu, double dlssTwoEyesMs, const FrameBudgetFacts& f, bool haveFacts) {
    Verdict v;
    const double gpu = w.sceneMs + w.afterMs;
    const std::string runtime = w.runtime.empty() ? std::string("the runtime") : w.runtime;
    const std::string lowerRes = std::format("a lower resolution in {}", w.runtime.empty() ? std::string("the headset's runtime") : w.runtime);
    if (w.uploadGBps >= 0 && w.uploadGBps < kSlowUploadGBps) {
        // Checked first: in this state every part of the frame waits on reads of system memory,
        // so the GPU figures look like any other cause.
        v.kind = "transfer-bound";
        v.detail = std::format("the graphics card reads system memory at {:.2f} GB/s (normally 3-14 GB/s){}", w.uploadGBps,
                               haveGpu ? std::format("; GPU {:.1f} ms (scene {:.1f}, after the scene {:.1f})", gpu, w.sceneMs, w.afterMs) : std::string());
        v.advice = "This slow state of the card's PCIe transfers outlasts the game and ends with the next start: quit, wait a minute, start again";
        v.brief = "slow PCIe transfers: restart the game";
        return v;
    }
    if (haveGpu && gpu >= kGpuBound * interval) {
        v.kind = "GPU-bound";
        const bool dlssLargest = dlssTwoEyesMs > 0 && w.afterMs >= w.sceneMs && dlssTwoEyesMs >= 0.5 * w.afterMs;
        if (dlssLargest) {
            const std::string preset = Lower(f.dlssPreset[0] ? std::string(f.dlssPreset) : std::string("default"));
            v.detail = std::format("scene {:.1f} ms, after the scene {:.1f} ms, of which DLSS about {:.1f} ms (preset {} at {}x{})", w.sceneMs, w.afterMs,
                                   dlssTwoEyesMs, preset, f.dlssOutWidth, f.dlssOutHeight);
            v.advice = preset != "default" ? std::format("Try [dlss] preset = default, or {}", lowerRes)
                                           : std::format("Try a lower [dlss] input_scale, or {}", lowerRes);
            v.brief = std::format("GPU-bound: DLSS preset {}", preset);
        } else if (w.sceneMs >= w.afterMs) {
            const std::string scale = haveFacts ? std::format(", render scale {:.2f}", f.renderScale) : std::string();
            const std::string fov = w.foveationPreset.empty() ? std::string("off") : w.foveationPreset;
            v.detail = std::format("scene {:.1f} ms (eye {}x{}{}, foveation {}), after the scene {:.1f} ms", w.sceneMs, w.eyeWidth, w.eyeHeight, scale, fov,
                                   w.afterMs);
            if (fov == "quality" || fov == "balanced")
                v.advice = std::format("Try [foveation] preset = {}, or {}", fov == "quality" ? "balanced" : "performance", lowerRes);
            else
                v.advice = std::format("Try [stereo] render_scale = 0.9, or {}", lowerRes);
            v.brief = "GPU-bound: the scene";
        } else {
            v.detail = std::format("scene {:.1f} ms, after the scene {:.1f} ms (post-processing, the HUD, the copies for the headset){}", w.sceneMs,
                                   w.afterMs, dlssTwoEyesMs > 0 ? std::format(", of which DLSS about {:.1f} ms", dlssTwoEyesMs) : std::string());
            v.advice = std::format("Try {}", lowerRes);
            v.brief = "GPU-bound: after the scene";
        }
        return v;
    }
    const bool waitLargest = w.waitSamples > 0 && (!haveGpu || (w.waitMs >= w.sceneMs && w.waitMs >= w.afterMs));
    if (waitLargest && w.waitMs >= kWaitLow * interval) {
        v.kind = "runtime-bound";
        v.detail = std::format("{} paces the frames: the frame wait takes {:.1f} ms of each frame{}", runtime, w.waitMs,
                               haveGpu ? std::format(", the GPU {:.1f} ms", gpu) : std::string());
        v.advice = std::format("Check the refresh rate and frame rate settings of {} (a halved rate, or its own reprojection)", runtime);
        v.brief = "runtime-bound";
        return v;
    }
    if (haveGpu && gpu < kGpuLow * interval && w.waitMs < kWaitLow * interval) {
        v.kind = "game-thread-bound";
        const std::string copy = w.copyEnginePercent >= 0 ? std::format("; game copy engine {:.0f} %", w.copyEnginePercent) : std::string();
        v.detail = std::format("the GPU takes {:.1f} ms (scene {:.1f}, after the scene {:.1f}) and the frame wait {:.1f} ms; the rest is the game's own "
                               "frame (CPU, or uploads to the card){}",
                               gpu, w.sceneMs, w.afterMs, w.waitMs, copy);
        v.advice = w.copyEnginePercent >= 50
                       ? "The copy engine is busy: the slow state that can follow a quick restart. Quit, wait a minute and a half, and start again"
                       : "See the copy engine figure in the 'video memory:' line; after a quick restart, quit, wait a minute and a half, and start again";
        v.brief = "game-thread-bound";
        return v;
    }
    v.kind = "Cause unclear";
    v.detail = haveGpu ? std::format("GPU {:.1f} ms (scene {:.1f}, after the scene {:.1f}), frame wait {:.1f} ms", gpu, w.sceneMs, w.afterMs, w.waitMs)
                       : std::format("GPU time not measured (foveated rendering is off), frame wait {:.1f} ms", w.waitMs);
    v.advice = std::format("Try {}", lowerRes);
    v.brief = "over budget";
    return v;
}

}  // namespace

void Configure(const Config& cfg) {
    g_on = cfg.get_bool("log", "frame_budget", true);
    {
        std::lock_guard lk(g_mutex);
        g_disturbed = Clock::now();
    }
    log::info("frame budget: warning {}", g_on.load() ? "on ([log] frame_budget = 1)" : "off ([log] frame_budget = 0)");
}

void NoteModeChange() { g_modeChangeNs = NowNs(); }
void NoteCapture() { g_captureNs = NowNs(); }

void Evaluate(const Window& w) {
    const auto now = Clock::now();
    FrameBudgetFacts f{};
    bool haveFacts = false;
    if (const FrameBudgetFactsFn fn = g_factsFn.load()) haveFacts = fn(&f);
    std::lock_guard lk(g_mutex);
    // Differences of the engine's running totals over this window.
    uint64_t scenes = 0;
    double dlssTwoEyes = 0;
    const bool firstFacts = haveFacts && !g_haveFacts;
    if (haveFacts && g_haveFacts) {
        scenes = f.sceneFrames - g_lastFacts.sceneFrames;
        const uint64_t evals = f.dlssEyeEvaluations - g_lastFacts.dlssEyeEvaluations;
        if (f.dlss && evals > 0) dlssTwoEyes = 2.0 * (f.dlssEyeMs - g_lastFacts.dlssEyeMs) / double(evals);
    }
    if (haveFacts) {
        g_lastFacts = f;
        g_haveFacts = true;
    }
    if (!g_on.load()) return;

    const auto windowStart = now - std::chrono::milliseconds(static_cast<int64_t>(w.seconds * 1000.0));
    if (const int64_t ns = g_modeChangeNs.load(); ns && FromNs(ns) > g_disturbed) {
        g_disturbed = FromNs(ns);
        g_disturbedWhy = "mode change";
    }
    // Not stereo gameplay in this window: a loading screen, a movie, the menus or a stall.
    std::string skip;
    if (!w.stereoMode || !(w.refreshHz > 0))
        skip = "not in stereo";
    else if (w.frames < 10 || w.stereoSubmitted * 2 < w.frames)
        skip = "few stereo frames";
    else if (haveFacts && !firstFacts && double(scenes) < kSceneShare * double(w.frames))
        skip = "no 3D scene in most frames (a loading screen or a movie)";
    else if (w.intervalMaxMs >= kStallMs)
        skip = std::format("a stall of {:.0f} ms (a load)", w.intervalMaxMs);
    if (!skip.empty()) {
        g_disturbed = now;
        g_disturbedWhy = skip;
        g_overWindows = 0;
        g_lastWindow = "skipped: " + skip;
        if (!g_warned) g_summary = "not measured (not in stereo gameplay)";
        return;
    }
    if (firstFacts) {
        // The engine's totals are read for the first time: the next window has the differences.
        g_lastWindow = "skipped: first reading of the engine's figures";
        return;
    }
    if (windowStart < g_disturbed + kHoldOff) {
        g_lastWindow = std::format("skipped: within 30 s of the last {}", g_disturbedWhy);
        return;
    }
    if (const int64_t ns = g_captureNs.load(); ns && FromNs(ns) >= windowStart) {
        g_lastWindow = "skipped: a capture in the window";
        return;
    }

    ++g_evaluated;
    const double budget = 1000.0 / w.refreshHz;
    const double interval = w.intervalMs;
    const double fps = w.seconds > 0 ? double(w.frames) / w.seconds : 0.0;
    const bool over = interval > kOverFraction * budget;
    const bool haveGpu = w.sceneSamples > 0 && w.afterSamples > 0;
    g_lastWindow = std::format("{:.1f} fps, frame interval {:.2f} ms (budget {:.2f}), xr frame wait {:.2f} ms, gpu scene {}, after the scene {}{}; {}", fps,
                               interval, budget, w.waitMs, haveGpu ? std::format("{:.2f} ms", w.sceneMs) : std::string("n/a"),
                               haveGpu ? std::format("{:.2f} ms", w.afterMs) : std::string("n/a"),
                               dlssTwoEyes > 0 ? std::format(", DLSS {:.2f} ms (two eyes)", dlssTwoEyes) : std::string(), over ? "over" : "within");
    if (!over) {
        g_overWindows = 0;
        g_summary = "within budget";
        if (g_warned) {
            g_warned = false;
            log::info("frame budget: back within budget ({:.0f} fps against {:.0f} Hz)", fps, w.refreshHz);
        }
        return;
    }
    ++g_overWindows;
    const Verdict v = Classify(w, interval, haveGpu, dlssTwoEyes, f, haveFacts);
    g_summary = std::format("{:.0f} fps / {:.0f} Hz, {}", fps, w.refreshHz, v.brief);
    if (g_overWindows < kWindowsToWarn || g_warned) return;
    if (g_everWarned && now - g_lastWarn < kWarnEvery) {
        ++g_suppressed;
        return;
    }
    log::warn("frame budget: {:.0f} fps against {:.0f} Hz ({:.1f} ms per frame, budget {:.1f}). {}: {}. {}", fps, w.refreshHz, interval, budget, v.kind,
              v.detail, v.advice);
    g_warned = true;
    g_everWarned = true;
    g_lastWarn = now;
    ++g_warnings;
}

std::string Summary() {
    if (!g_on.load()) return "off ([log] frame_budget = 0)";
    std::lock_guard lk(g_mutex);
    return g_summary;
}

std::string Command(const std::string& args) {
    if (args == "on" || args == "off") {
        g_on = args == "on";
        return std::format("ok frame budget warning {}", args);
    }
    if (!args.empty() && args != "status") return "err usage: budget status | on | off";
    const std::string summary = Summary();
    std::lock_guard lk(g_mutex);
    return std::format("ok {}; last window: {}; warning {} ({} written, {} held back by the one-a-minute limit; {} windows judged, {} over in a row); "
                       "last disturbance: {}, {:.0f} s ago",
                       summary, g_lastWindow, g_warned ? "in effect" : "not in effect", g_warnings, g_suppressed, g_evaluated, g_overWindows,
                       g_disturbedWhy, std::chrono::duration<double>(Clock::now() - g_disturbed).count());
}

}  // namespace frame_budget
}  // namespace ff7vr::render
