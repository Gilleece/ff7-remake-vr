#include "frame_budget_classify.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace ff7vr::render::frame_budget {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

constexpr const char* kStallAdvice =
    "This is the mid-session stall (GUIDE.md, \"Known problems in full\"): the graphics card's transfers from system memory have slowed down "
    "and stay slow until the game is started again. Quit, wait a minute and start the game again";

}  // namespace

Verdict Classify(const ClassifyInput& in) {
    const Window& w = in.w;
    const double interval = in.intervalMs;
    const double gpu = w.sceneMs + w.afterMs;
    Verdict v;
    const std::string runtime = w.runtime.empty() ? std::string("the runtime") : w.runtime;
    const std::string lowerRes = std::format("a lower resolution in {}", w.runtime.empty() ? std::string("the headset's runtime") : w.runtime);
    const std::string gpuParts = std::format("GPU {:.1f} ms (scene {:.1f}, after the scene {:.1f})", gpu, w.sceneMs, w.afterMs);
    if (w.uploadGBps >= 0 && w.uploadGBps < kSlowUploadGBps) {
        // Checked first: in this state every part of the frame waits on reads of system memory,
        // so the GPU figures look like any other cause.
        v.kind = "transfer-bound";
        v.detail = std::format("the graphics card reads system memory at {:.2f} GB/s (normally 3-14 GB/s){}", w.uploadGBps,
                               in.haveGpu ? "; " + gpuParts : std::string());
        v.advice = kStallAdvice;
        v.brief = "slow PCIe transfers: restart the game";
        v.stall = true;
        return v;
    }
    // The GPU is the limit when its time fills the frame interval, or fills the refresh period
    // on its own: a runtime that cannot get a frame in time waits a whole period more (or
    // halves the rate), so its frame wait is then large as well, but it is waiting for the GPU.
    if (in.haveGpu && (gpu >= kGpuBound * interval || (in.budgetMs > 0 && gpu >= kGpuFillsBudget * in.budgetMs))) {
        v.kind = "GPU-bound";
        // The stall's signature: the GPU time well above that of the last window within budget
        // at the same settings with no load in between (the scene and post-processing did not
        // change, the card got slower), or the upload check below normal.
        const bool risen = in.baselineGpuMs > 0 && gpu >= kStallRise * in.baselineGpuMs && gpu - in.baselineGpuMs >= kStallRiseMs;
        const bool slowUploads = w.uploadGBps >= 0 && w.uploadGBps < kLowUploadGBps;
        if (risen || slowUploads) {
            std::string why;
            if (risen)
                why = std::format("{:.1f} times the {:.1f} ms of the last window within budget at the same settings, with no load or settings change since",
                                  gpu / in.baselineGpuMs, in.baselineGpuMs);
            if (slowUploads)
                why += std::format("{}the upload check measured {:.2f} GB/s (normally 9-14)", why.empty() ? "" : "; ", w.uploadGBps);
            v.detail = std::format("{}: {}", gpuParts, why);
            v.advice = kStallAdvice;
            v.brief = "GPU-bound: stall (restart the game)";
            v.stall = true;
            return v;
        }
        // The card's power and clocks are not read by the mod, so a stall that began before the
        // first measured window looks like any heavy scene: the advice names it as well.
        const std::string ifSudden = " If the frame rate fell suddenly in the middle of a session and stays low, quit and start the game again "
                                     "(the mid-session stall, GUIDE.md)";
        const bool dlssLargest = in.dlssTwoEyesMs > 0 && w.afterMs >= w.sceneMs && in.dlssTwoEyesMs >= 0.5 * w.afterMs;
        if (dlssLargest) {
            const std::string preset = Lower(in.dlssPreset.empty() ? std::string("default") : in.dlssPreset);
            v.detail = std::format("scene {:.1f} ms, after the scene {:.1f} ms, of which DLSS about {:.1f} ms (preset {} at {}x{})", w.sceneMs, w.afterMs,
                                   in.dlssTwoEyesMs, preset, in.dlssOutWidth, in.dlssOutHeight);
            v.advice = preset != "default" ? std::format("Try [dlss] preset = default, or {}.", lowerRes)
                                           : std::format("Try a lower [dlss] input_scale, or {}.", lowerRes);
            v.brief = std::format("GPU-bound: DLSS preset {}", preset);
        } else if (w.sceneMs >= w.afterMs) {
            const std::string scale = in.haveRenderScale ? std::format(", render scale {:.2f}", in.renderScale) : std::string();
            const std::string fov = w.foveationPreset.empty() ? std::string("off") : w.foveationPreset;
            v.detail = std::format("scene {:.1f} ms (eye {}x{}{}, foveation {}), after the scene {:.1f} ms", w.sceneMs, w.eyeWidth, w.eyeHeight, scale, fov,
                                   w.afterMs);
            if (fov == "quality" || fov == "balanced")
                v.advice = std::format("Try [foveation] preset = {}, or {}.", fov == "quality" ? "balanced" : "performance", lowerRes);
            else
                v.advice = std::format("Try [stereo] render_scale = 0.9, or {}.", lowerRes);
            v.brief = "GPU-bound: the scene";
        } else {
            v.detail = std::format("scene {:.1f} ms, after the scene {:.1f} ms (post-processing, the HUD, the copies for the headset){}", w.sceneMs,
                                   w.afterMs, in.dlssTwoEyesMs > 0 ? std::format(", of which DLSS about {:.1f} ms", in.dlssTwoEyesMs) : std::string());
            v.advice = std::format("Try {}.", lowerRes);
            v.brief = "GPU-bound: after the scene";
        }
        if (gpu < kGpuBound * interval)
            v.detail += std::format("; the GPU alone needs more than the {:.1f} ms period, so {} waits for the next one (frame wait {:.1f} ms)", in.budgetMs,
                                    runtime, w.waitMs);
        v.advice += ifSudden;
        return v;
    }
    const bool waitLargest = w.waitSamples > 0 && (!in.haveGpu || (w.waitMs >= w.sceneMs && w.waitMs >= w.afterMs));
    if (waitLargest && w.waitMs >= kWaitLow * interval) {
        v.kind = "runtime-bound";
        v.detail = std::format("{} paces the frames: the frame wait takes {:.1f} ms of each frame{}", runtime, w.waitMs,
                               in.haveGpu ? std::format(", the GPU {:.1f} ms", gpu) : std::string(", the GPU time was not measured"));
        v.advice = std::format("Check the refresh rate and frame rate settings of {} (a halved rate, or its own reprojection)", runtime);
        v.brief = "runtime-bound";
        return v;
    }
    if (in.haveGpu && gpu < kGpuLow * interval && w.waitMs < kWaitLow * interval) {
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
    v.detail = in.haveGpu ? std::format("{}, frame wait {:.1f} ms", gpuParts, w.waitMs)
                          : std::format("GPU time not measured, frame wait {:.1f} ms", w.waitMs);
    v.advice = std::format("Try {}", lowerRes);
    v.brief = "over budget";
    return v;
}

}  // namespace ff7vr::render::frame_budget
