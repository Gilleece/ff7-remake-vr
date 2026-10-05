#pragma once
// Development and measurement tools that run inside the game.
//
// Currently: the frame timer used by the benchmark (tools/bench). It records
// the time of every IDXGISwapChain::Present of the game's swap chain and
// writes the series to a CSV file on request. It is controlled through the
// dev command pipe (\\.\pipe\ff7vr-dev, see src/loader/dev_input.h):
//
//   bench status            -> "ok hooked=1 recording=0 frames=<n> total=<n> backbuffer=WxH sync=<i> flags=<hex>"
//   bench start             start a new recording (clears the previous one)
//   bench stop <csv path>   stop recording and write one line per frame
//   bench cvars <name>...   -> "ok GSystemResolution=WxH r.ScreenPercentage=100 ..." (read only;
//                           values as float, "missing" for an unknown name)
//   bench setcvar <n> <v>   set a console variable at console priority for this
//                           process only (nothing is saved); -> "ok <n>=<now> setby=0x..."
//
// ini keys (all in [bench]):
//   frame_timer = 1             install the Present hook (default 0)
//   force_sync_interval = -1    >= 0: replace the game's sync interval, to remove
//                               a vsync cap while measuring; -1 leaves it alone

#include "ff7vr/core/startup_context.h"

#include <string>

namespace ff7vr::dev {

// Called once by the loader. Installs what the ini enables. Never throws.
bool start(const StartupContext& ctx);

// Dev pipe hook: returns true and sets `reply` when the command line belongs
// to this module (first word "bench").
bool handle_command(const std::string& line, std::string& reply);

}  // namespace ff7vr::dev
