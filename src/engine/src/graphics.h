#pragma once

#include <string>
// The `graphics` dev command: graphics status | graphics profile <quality|balanced|performance>
// applies a [graphics] profile live (docs/engine-module.md, "Graphics profiles").

namespace ff7vr::engine::graphics {
// `profile` is the [graphics] profile read at start-up (custom, quality, balanced, performance).
void register_command(const std::string& startup_profile = "custom");
// Any thread: the profile in effect (the start-up one, the last one applied live, or custom
// after a setting of a profile was changed by hand).
std::string current_profile();
void set_current_profile(const std::string& name);
// `graphics profile <name>`: applies a profile live; "ok ..." or "err ...".
std::string apply_profile(const std::string& name);
}
