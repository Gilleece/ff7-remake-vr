#pragma once
// [graphics] profile: a named bundle of ini values (quality | balanced | performance; custom
// applies nothing). Each line of a bundle is an ordinary ini key that can also be set by hand;
// a key the player set explicitly always wins over the profile. See docs/guide.md
// ("Performance") and docs/engine-module.md ("Graphics profiles").

#include "ff7vr/core/config.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::graphics_profile {

struct Line {
    const char* section;  // "foveation", "stereo" or "stereo_cvars"
    const char* key;
    const char* value;
};

// The lines of a profile; empty for "custom" and for unknown names.
std::span<const Line> lines(std::string_view name);
bool known(std::string_view name);  // quality, balanced, performance or custom

// Reads [graphics] profile and fills every key of the bundle the ini does not set. Returns
// readable log lines: which values were applied and which were left to the ini.
std::vector<std::string> apply(Config& cfg);

// The [stereo_cvars] names apply() filled at start-up (the ones the ini did not set).
const std::vector<std::string>& applied_cvars();

}  // namespace ff7vr::graphics_profile
