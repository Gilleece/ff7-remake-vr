#pragma once
// Present-based frame timer (see include/ff7vr/dev/dev.h for the commands).

#include <string>

namespace ff7vr::dev::frame_timer {

struct Options {
    int force_sync_interval = -1;
};

// Starts a thread that waits for the game's window and then hooks
// IDXGISwapChain::Present. Returns false if the thread could not be started.
bool start(const Options& options);

std::string status();
std::string begin_recording();
std::string end_recording(const std::string& csv_path);

}  // namespace ff7vr::dev::frame_timer
