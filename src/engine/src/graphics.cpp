#include "graphics.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/graphics_profile.h"
#include "ff7vr/core/log.h"
#include "ff7vr/engine/cvars.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace ff7vr::engine::graphics {
namespace {

std::mutex g_mutex;
std::string g_live = "(none applied live)";
std::vector<std::string> g_held;  // console variables a profile holds (start-up or live), not set in the ini

std::string run(std::string_view line) {
    std::string reply;
    if (!dev_commands::dispatch(line, reply)) return "err no handler for " + std::string(line);
    return reply;
}

std::string command(std::string_view args) {
    std::string a(args);
    while (!a.empty() && a.back() == ' ') a.pop_back();
    if (a.empty() || a == "status") {
        std::lock_guard lock(g_mutex);
        return "ok graphics: last profile applied live: " + g_live + "; the start-up profile is in the log (graphics: ...)";
    }
    constexpr std::string_view kProfile = "profile ";
    if (a.rfind(kProfile, 0) != 0) return "err usage: graphics status | graphics profile quality|balanced|performance";
    const std::string name = a.substr(kProfile.size());
    const auto lines = graphics_profile::lines(name);
    if (lines.empty()) return "err unknown profile '" + name + "' (quality, balanced, performance)";
    std::string out = "ok graphics profile " + name + ":";
    // A variable the previous profile held and this one does not list goes back to the game's
    // value; the player's own [stereo_cvars] lines are never released.
    std::vector<std::string> held;
    {
        std::lock_guard lock(g_mutex);
        if (g_live == "(none applied live)") g_held = graphics_profile::applied_cvars();
        held = g_held;
    }
    std::vector<std::string> now;
    for (const auto& l : lines)
        if (std::string_view(l.section) == "stereo_cvars") now.emplace_back(l.key);
    for (const auto& h : held) {
        const bool listed = std::any_of(now.begin(), now.end(), [&](const std::string& n) { return _stricmp(n.c_str(), h.c_str()) == 0; });
        if (!listed) {
            cvar::release_in_stereo(log::widen(h));
            out += " released " + h;
        }
    }
    for (const auto& l : lines) {
        const std::string section = l.section;
        std::string r;
        if (section == "stereo_cvars") {
            cvar::hold_in_stereo(log::widen(l.key), log::widen(l.value));
            r = "held in stereo";
        } else if (section == "foveation") {
            r = run(std::format("fov preset {}", l.value));
        } else if (section == "stereo" && std::string_view(l.key) == "render_scale") {
            r = run(std::format("dynres scale {}", l.value));
        }
        const bool ok = r.rfind("err", 0) != 0;
        out += std::format(" {}={}{}", l.key, l.value, ok ? "" : " (" + r + ")");
    }
    log::info("graphics: profile {} applied live", name);
    {
        std::lock_guard lock(g_mutex);
        g_live = name;
        g_held = std::move(now);
    }
    return out;
}

}  // namespace

void register_command() {
    dev_commands::add("graphics", "graphics status | graphics profile quality|balanced|performance: apply a [graphics] profile live",
                      [](std::string_view args) { return command(args); });
}

}  // namespace ff7vr::engine::graphics
