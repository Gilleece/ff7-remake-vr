#include "ff7vr/core/dev_commands.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <map>
#include <memory>
#include <mutex>

namespace ff7vr::dev_commands {
namespace {

struct Entry {
    std::string help;
    std::shared_ptr<Handler> handler;
};

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, Entry, std::less<>>& registry() {
    static std::map<std::string, Entry, std::less<>> r;
    return r;
}

std::atomic<bool> g_unsafe{false};

}  // namespace

void set_unsafe_allowed(bool allowed) { g_unsafe = allowed; }
bool unsafe_allowed() { return g_unsafe.load(); }

void add(std::string_view word, std::string_view help, Handler handler) {
    std::lock_guard lk(registry_mutex());
    registry()[std::string(word)] = Entry{std::string(help), std::make_shared<Handler>(std::move(handler))};
}

bool dispatch(std::string_view line, std::string& reply) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos) return false;
    line.remove_prefix(start);
    const size_t end = line.find_first_of(" \t");
    const std::string_view word = line.substr(0, end);
    std::string_view args = end == std::string_view::npos ? std::string_view{} : line.substr(end + 1);
    const size_t a = args.find_first_not_of(" \t");
    args = a == std::string_view::npos ? std::string_view{} : args.substr(a);

    std::shared_ptr<Handler> h;
    {
        std::lock_guard lk(registry_mutex());
        const auto it = registry().find(word);
        if (it == registry().end()) return false;
        h = it->second.handler;
    }
    try {
        reply = (*h)(args);
    } catch (const std::exception& e) {
        reply = std::string("err ") + e.what();
    } catch (...) {
        reply = "err exception";
    }
    return true;
}

std::vector<std::string> help() {
    std::lock_guard lk(registry_mutex());
    std::vector<std::string> out;
    for (const auto& [word, e] : registry()) out.push_back(word + ": " + e.help);
    return out;
}

}  // namespace ff7vr::dev_commands
