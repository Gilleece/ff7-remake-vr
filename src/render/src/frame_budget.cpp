#include "frame_budget.h"
#include "frame_budget_classify.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <algorithm>
#include <atomic>
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
// The stall's signature: the GPU time of the newest window within budget, at the settings in
// g_baselineKey, with no load, mode change or settings change since (0: none).
double g_baselineGpuMs = 0;
std::string g_baselineKey;

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
Clock::time_point FromNs(int64_t ns) { return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ns))); }

// What makes the GPU time comparable between two windows: eye size, render scale, DLSS and
// foveation as they were in effect.
std::string SettingsKey(const Window& w, const FrameBudgetFacts& f, bool haveFacts) {
    return std::format("{}x{} {} {} {:.3f} {} {} {}x{}->{}x{}", w.eyeWidth, w.eyeHeight, w.foveationPreset, haveFacts ? 1 : 0, haveFacts ? f.renderScale : 1.0f,
                       haveFacts && f.dlss ? 1 : 0, haveFacts ? f.dlssPreset : "", f.dlssInWidth, f.dlssInHeight, f.dlssOutWidth, f.dlssOutHeight);
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
        g_baselineGpuMs = 0;
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
    const std::string key = SettingsKey(w, f, haveFacts);
    if (key != g_baselineKey) {
        g_baselineKey = key;
        g_baselineGpuMs = 0;
    }
    g_lastWindow = std::format("{:.1f} fps, frame interval {:.2f} ms (budget {:.2f}), xr frame wait {:.2f} ms, gpu scene {}, after the scene {}{}; {}", fps,
                               interval, budget, w.waitMs, haveGpu ? std::format("{:.2f} ms", w.sceneMs) : std::string("n/a"),
                               haveGpu ? std::format("{:.2f} ms", w.afterMs) : std::string("n/a"),
                               dlssTwoEyes > 0 ? std::format(", DLSS {:.2f} ms (two eyes)", dlssTwoEyes) : std::string(), over ? "over" : "within");
    if (!over) {
        g_overWindows = 0;
        g_summary = "within budget";
        if (haveGpu) g_baselineGpuMs = w.sceneMs + w.afterMs;
        if (g_warned) {
            g_warned = false;
            log::info("frame budget: back within budget ({:.0f} fps against {:.0f} Hz)", fps, w.refreshHz);
        }
        return;
    }
    ++g_overWindows;
    ClassifyInput in;
    in.w = w;
    in.intervalMs = interval;
    in.budgetMs = budget;
    in.haveGpu = haveGpu;
    in.dlssTwoEyesMs = dlssTwoEyes;
    in.dlssPreset = f.dlssPreset;
    in.dlssOutWidth = f.dlssOutWidth;
    in.dlssOutHeight = f.dlssOutHeight;
    in.haveRenderScale = haveFacts;
    in.renderScale = f.renderScale;
    in.baselineGpuMs = g_baselineGpuMs;
    const Verdict v = Classify(in);
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
