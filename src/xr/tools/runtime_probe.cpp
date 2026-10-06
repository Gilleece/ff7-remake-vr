// ff7vr_runtime_probe: lists the OpenXR runtimes [xr] runtime = auto would
// consider, in its order, and probes them in this one process.
//
//   ff7vr_runtime_probe                 list, then probe like auto does (a runtime
//                                       that would start its server is skipped
//                                       unless running or active)
//   ff7vr_runtime_probe --list          list only
//   ff7vr_runtime_probe SEL [SEL ...]   probe these in this order, in one process
//                                       (SEL: steamvr, virtualdesktop, system or a
//                                       manifest path); shows whether the loader
//                                       switches runtimes inside a process
//
// No device and no session: instance, xrGetSystem, destroy.
#include "ff7vr/xr/xr.h"

#include <cstdio>
#include <string>

using namespace ff7vr::xr;

namespace {

void PrintProbe(const std::string& label, const std::string& manifest) {
    const RuntimeProbe p = ProbeRuntime(manifest);
    std::printf("probe %-28s -> %s, runtime '%s' %s, system '%s', %.0f ms%s%s\n", label.c_str(), ToString(p.result),
                p.runtimeName.c_str(), p.runtimeVersion.c_str(), p.systemName.c_str(), p.ms, p.reason.empty() ? "" : ": ",
                p.reason.c_str());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    const bool listOnly = argc > 1 && std::string(argv[1]) == "--list";
    if (argc > 1 && !listOnly) {
        for (int i = 1; i < argc; ++i) {
            std::string path, err;
            if (!ResolveRuntimeJson(argv[i], &path, &err)) {
                std::printf("%s: %s\n", argv[i], err.c_str());
                continue;
            }
            PrintProbe(argv[i], path);
        }
        return 0;
    }
    const auto cands = EnumerateRuntimeCandidates();
    std::printf("%zu candidates, in the order auto tries them:\n", cands.size());
    for (const auto& c : cands)
        std::printf("  %-26s %s  [%s%s%s%s]\n", c.name.c_str(), c.manifest.c_str(), c.origin.c_str(), c.active ? ", active" : "",
                    c.runningProcess.empty() ? "" : (", running: " + c.runningProcess).c_str(),
                    c.needsRunningBecause.empty() ? "" : ", probed only while running");
    if (listOnly) return 0;
    for (const auto& c : cands) {
        std::string skip;
        if (!ShouldProbeRuntime(c, &skip)) {
            std::printf("skip  %-28s %s\n", c.name.c_str(), skip.c_str());
            continue;
        }
        PrintProbe(c.name, c.manifest);
    }
    return 0;
}
