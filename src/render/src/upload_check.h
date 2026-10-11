// Upload check ([log] upload_check; docs/benchmarking.md, "Mid-session stall").
//
// Every 10 seconds one small copy from a staging buffer in system memory to a buffer on the
// graphics card (256 KB) is timed with GPU timestamps on the game's immediate context, in
// the Present hook. Normally it takes a few hundredths of a millisecond (3-14 GB/s). In the
// slow state of the card's PCIe transfers (reads of system memory by the card 100 to 500
// times slower, the whole machine, until the next start of the game) it takes 5-15 ms, and the
// game's frames slow down with it. The timing block gets one line per window, and the log one
// warning when two windows in a row measure less than 0.5 GB/s with an explanation for the
// player; one line when it is back.
#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace ff7vr {
class Config;
}

namespace ff7vr::render::upload_check {

// [log] upload_check (1 = on, the default; 0 = off). Registers the `upload` dev command.
void Configure(const Config& cfg);

// Present hook (the thread that owns the game's immediate context): collects a finished
// check and starts the next one when due.
void Frame(ID3D11Device* device, ID3D11DeviceContext* ctx);

// XR thread, once per timing window: one line ("" when off or nothing measured), and the
// warning when the uploads stayed slow.
std::string TakeWindowLine();

// The latest measurement in GB/s (-1: none yet), for the frame budget's classification.
double LatestGBps();

std::string Command(const std::string& args);

}  // namespace ff7vr::render::upload_check
