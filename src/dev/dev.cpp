#include "ff7vr/dev/dev.h"

#include "cvars.h"
#include "frame_timer.h"

#include "ff7vr/core/log.h"

#include <sstream>
#include <vector>

namespace ff7vr::dev {

bool start(const StartupContext& ctx) {
    const Config& cfg = *ctx.config;
    if (!cfg.get_bool("bench", "frame_timer", false)) return true;
    frame_timer::Options o;
    o.force_sync_interval = static_cast<int>(cfg.get_float("bench", "force_sync_interval", -1));
    return frame_timer::start(o);
}

bool handle_command(const std::string& line, std::string& reply) {
    std::stringstream ss(line);
    std::string word, sub;
    ss >> word;
    if (word != "bench") return false;
    ss >> sub;
    if (sub == "status") {
        reply = frame_timer::status();
    } else if (sub == "start") {
        reply = frame_timer::begin_recording();
    } else if (sub == "stop") {
        std::string path;
        std::getline(ss, path);
        while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(path.begin());
        while (!path.empty() && (path.back() == ' ' || path.back() == '\r')) path.pop_back();
        if (path.empty()) {
            reply = "err usage: bench stop <csv path>";
        } else {
            reply = frame_timer::end_recording(path);
        }
    } else if (sub == "cvars") {
        std::vector<std::string> names;
        std::string n;
        while (ss >> n) names.push_back(n);
        reply = cvars::read(names);
    } else if (sub == "setcvar") {
        std::string name, value;
        ss >> name;
        std::getline(ss, value);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        reply = (name.empty() || value.empty()) ? "err usage: bench setcvar <name> <value>" : cvars::set(name, value);
    } else {
        reply = "err usage: bench status|start|stop <csv path>|cvars <name>...|setcvar <name> <value>";
    }
    return true;
}

}  // namespace ff7vr::dev
