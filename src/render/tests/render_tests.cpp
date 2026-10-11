// Unit tests for the render module's pure parts (the frame budget's classifier). Exit code 0 = pass.

#include "frame_budget_classify.h"

#include <cstdio>
#include <string>

using namespace ff7vr::render::frame_budget;

namespace {
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// A window at 72 Hz with the given medians; GPU measured when scene or after is above 0.
ClassifyInput window72(double interval, double wait, double scene, double after) {
    ClassifyInput in;
    in.w.runtime = "VirtualDesktopXR";
    in.w.refreshHz = 72.0f;
    in.w.intervalMs = interval;
    in.w.waitMs = wait;
    in.w.waitSamples = 700;
    in.w.sceneMs = scene;
    in.w.afterMs = after;
    in.w.sceneSamples = in.w.afterSamples = (scene > 0 || after > 0) ? 700 : 0;
    in.w.eyeWidth = 3476;
    in.w.eyeHeight = 3364;
    in.intervalMs = interval;
    in.budgetMs = 1000.0 / 72.0;
    in.haveGpu = scene > 0 || after > 0;
    return in;
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

int main() {
    {
        // The runtime halves the rate because the GPU cannot make 13.9 ms: 26.4 ms interval, 24.3 ms
        // of it in the frame wait, GPU 15 ms. GPU-bound, not runtime-bound.
        const Verdict v = Classify(window72(26.4, 24.3, 9.0, 6.0));
        check(v.kind == "GPU-bound", "slow GPU behind a halved rate is GPU-bound");
        check(!has(v.advice, "refresh rate and frame rate settings"), "... and the advice does not point at the runtime's settings");
        check(has(v.advice, "quit and start the game again"), "... and it names quit-and-restart for a sudden drop");
    }
    {
        // The same window without GPU figures (foveation off before the timers moved): runtime-bound.
        const Verdict v = Classify(window72(26.4, 24.3, 0, 0));
        check(v.kind == "runtime-bound", "without GPU figures the large wait reads as runtime-bound");
    }
    {
        // A light GPU and a large wait: the runtime really paces.
        const Verdict v = Classify(window72(27.8, 24.0, 4.0, 2.0));
        check(v.kind == "runtime-bound", "light GPU and a large frame wait is runtime-bound");
    }
    {
        // GPU time doubled at the same settings with no load since: the stall's signature.
        ClassifyInput in = window72(26.4, 20.0, 12.0, 4.0);
        in.baselineGpuMs = 7.5;
        const Verdict v = Classify(in);
        check(v.kind == "GPU-bound" && v.stall, "GPU time 2.1x the baseline is the stall");
        check(has(v.advice, "mid-session stall") && has(v.advice, "Quit"), "... its advice is the stall note and a restart");
        check(has(v.detail, "times the 7.5 ms"), "... and the detail names the baseline");
    }
    {
        // A modest rise (a heavier view): GPU-bound with the normal advice.
        ClassifyInput in = window72(16.0, 3.0, 10.0, 4.0);
        in.baselineGpuMs = 10.0;
        const Verdict v = Classify(in);
        check(v.kind == "GPU-bound" && !v.stall, "1.4x the baseline is not the stall");
        check(has(v.advice, "render_scale"), "... the advice is about the scene's cost");
    }
    {
        // Upload check below normal (milder state) while GPU-bound: the stall.
        ClassifyInput in = window72(20.0, 2.0, 15.0, 4.0);
        in.w.uploadGBps = 1.2;
        const Verdict v = Classify(in);
        check(v.kind == "GPU-bound" && v.stall && has(v.detail, "1.20 GB/s"), "GPU-bound with a 1.2 GB/s upload check is the stall");
    }
    {
        // Deep state: transfer-bound first, whatever the GPU says.
        ClassifyInput in = window72(45.0, 1.0, 30.0, 5.0);
        in.w.uploadGBps = 0.05;
        const Verdict v = Classify(in);
        check(v.kind == "transfer-bound" && v.stall, "upload check at 0.05 GB/s is transfer-bound");
    }
    {
        // t.MaxFPS 40 with a light GPU and no wait: the game's own frame.
        ClassifyInput in = window72(25.0, 0.0, 6.4, 0.7);
        in.w.refreshHz = 90.0f;
        in.budgetMs = 1000.0 / 90.0;
        in.w.copyEnginePercent = 0;
        const Verdict v = Classify(in);
        check(v.kind == "game-thread-bound", "40 fps cap with a light GPU is game-thread-bound");
    }
    {
        // DLSS the largest part after the scene.
        ClassifyInput in = window72(18.5, 1.0, 7.8, 9.6);
        in.dlssTwoEyesMs = 8.0;
        in.dlssPreset = "L";
        in.dlssOutWidth = 3708;
        in.dlssOutHeight = 3396;
        const Verdict v = Classify(in);
        check(v.kind == "GPU-bound" && has(v.brief, "DLSS preset l"), "DLSS the largest part names the preset");
    }
    std::printf("%s (%d failures)\n", g_failures ? "RESULT: FAIL" : "RESULT: PASS", g_failures);
    return g_failures ? 1 : 0;
}
