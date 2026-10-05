#pragma once
// Crash handler: on a fatal exception, log the exception code, the faulting
// address as module+RVA, key registers and a short stack (module+RVA per frame),
// then write a minidump next to the log.
//
// The game (UE4) wraps its main loop in __try/__except and handles crashes with
// its own reporter, so an unhandled-exception filter alone would never run.
// Therefore a vectored handler also reports *first-chance* fatal exceptions
// (access violation, illegal instruction, stack overflow, ...). A first-chance
// report that the game later handles is harmless but is marked "first-chance"
// in the log; the number of reports and dumps per process is capped.
//
// All reporting runs on a dedicated pre-created thread, so it also works for
// stack overflows. The faulting thread waits for the report, then the
// exception continues to the game's own handlers unchanged.

#include <filesystem>

namespace ff7vr::crash {

struct Options {
    std::filesystem::path dump_dir;   // where ff7vr-crash-*.dmp go (default: next to the log)
    bool first_chance = true;         // report first-chance fatal exceptions via VEH
    int max_reports = 8;              // stack+registers logged at most this many times
    int max_dumps = 2;                // minidumps written at most this many times
    bool full_memory_dump = false;    // MiniDumpWithFullMemory (huge for this game)
};

bool install(const Options& options);
void uninstall();

// Writes a minidump of the current state on demand (e.g. for a hang). Returns the path or empty.
std::filesystem::path write_dump_now(const char* reason);

}  // namespace ff7vr::crash
