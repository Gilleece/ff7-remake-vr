#pragma once
// Per-headset settings: [headset:<match>] sections of ff7vr.ini (see config.h). A section
// applies when <match> is contained, ignoring case, in the OpenXR system name
// (XrSystemProperties::systemName, e.g. "Meta Quest 3") or in the runtime name
// (e.g. "VirtualDesktopXR"). The first matching section in file order wins; its keys form a
// layer over the base settings.
//
// When it is applied. The system name is only known once an OpenXR instance exists, long
// after the modules have read their settings. So:
//   1. At start (DllMain, before any module reads the config) the headset is taken from what
//      is known without an instance: the Null backend's fixed name, otherwise the headset of
//      the previous session, remembered in ff7vr-headset.txt beside the DLL. Every key of the
//      matched section then takes effect as if it were in the base ini.
//   2. When the XR session is created the real names are known. If they match a different
//      section (another headset than last time, or the first session), the layer is
//      replaced: keys with a live setter in the settings registry (live_settings.h) change at
//      once; the others are logged as taking effect at the next start, which will pick the
//      new headset up from ff7vr-headset.txt.
// Probing the runtime before the modules start was not used: creating an instance there
// delays the hooks that must be in place before the engine creates its device, can start a
// runtime's server (SteamVR) and runs before the implicit API layer handling of the XR
// backend.

#include "ff7vr/core/config.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::headset_profile {

// The names the Null backend reports (src/xr/src/null_backend.cpp).
inline constexpr const char* kNullSystemName = "Null HMD (Quest 3 class)";
inline constexpr const char* kNullRuntimeName = "Null";
// Beside the DLL: the last session's system and runtime names, one per line.
inline constexpr const wchar_t* kStateFile = L"ff7vr-headset.txt";

// Step 1. Plain Win32 file calls only (runs under the loader lock). Keeps `cfg` for the
// later steps. Returns lines for the log (written once the log is open).
std::vector<std::string> apply_at_start(Config& cfg, const std::filesystem::path& dll_dir);

// Step 2. Call after an XR session is created, from any thread; the live setters run on a
// thread of its own. Does nothing before apply_at_start.
void on_session(std::string_view system_name, std::string_view runtime_name);

// The match of the section in effect now; empty when none (the settings panel saves the keys
// that section sets into it rather than into the base sections).
std::string active_section();

// The dev command `headset status` (registered by register_dev_command).
std::vector<std::string> status_lines();
void register_dev_command();

}  // namespace ff7vr::headset_profile
