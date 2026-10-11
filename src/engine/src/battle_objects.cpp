#include "battle_objects.h"

#include "uobj.h"

#include "ff7vr/core/log.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ff7vr::engine::battle {
namespace {

constexpr double kPollSeconds = 0.1;
constexpr int kScanSlice = 16384;  // objects per poll: a pass of about 200,000 objects in about 1.3 s

std::mutex g_mutex;
std::vector<std::string> g_names{"BattleTarget_New_C"};
std::uint64_t g_names_version = 1;

// Game thread state.
struct Scan {
    std::uint64_t version = 0;
    std::unordered_map<void*, int> kinds;  // class -> 0 not wanted, 1 widget, 2 other
    std::vector<void*> cur, next;          // instances of the last pass and of the pass in progress
    int pos = 0;
    int passes = 0;
    double next_poll = 0.0;
    ObjectsState state{};
    std::uint64_t polls = 0, shown_polls = 0, calls_failed = 0;
} g;
uobj::Lookup* g_fn = nullptr;  // Widget.IsVisible

int kind_of(void* cls, const std::vector<std::string>& names) {
    auto it = g.kinds.find(cls);
    if (it != g.kinds.end()) return it->second;
    int k = 0;
    const std::string n = uobj::object_name(cls);
    if (std::find(names.begin(), names.end(), n) != names.end()) {
        k = 2;
        for (void* c = cls; c; c = uobj::super_of(c))
            if (uobj::object_name(c) == "Widget") {
                k = 1;
                break;
            }
    }
    g.kinds.emplace(cls, k);
    return k;
}

bool counted(void* o) {
    // class default objects and the templates inside widget blueprint classes are never shown
    return o && !uobj::pending_kill(o) && !uobj::is_template(o);
}

}  // namespace

void set_classes(const std::string& list) {
    std::string t = list;
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream in(t);
    std::vector<std::string> names;
    for (std::string w; in >> w;) names.push_back(w);
    std::lock_guard lk(g_mutex);
    g_names = std::move(names);
    ++g_names_version;
}

std::string classes() {
    std::lock_guard lk(g_mutex);
    std::string r;
    for (const auto& n : g_names) r += (r.empty() ? "" : ",") + n;
    return r;
}

ObjectsState poll(double now, bool& polled) {
    polled = false;
    if (now < g.next_poll || !uobj::available()) return g.state;
    g.next_poll = now + kPollSeconds;
    polled = true;
    std::vector<std::string> names;
    {
        std::lock_guard lk(g_mutex);
        if (g.version != g_names_version) {
            // a new class list: start over
            g.version = g_names_version;
            g.kinds.clear();
            g.cur.clear();
            g.next.clear();
            g.pos = 0;
            g.passes = 0;
            g.state = ObjectsState{};
        }
        names = g_names;
    }
    if (!g_fn) g_fn = new uobj::Lookup({{"IsVisible", "Widget", "Function"}});
    if (!g_fn->ready() && g_fn->passes() < 3) g_fn->step(65536);
    if (names.empty()) {
        g.state = ObjectsState{0, 0, 0};
        return g.state;
    }
    const int n = uobj::num_objects();
    const int end = std::min(n, g.pos + kScanSlice);
    for (int i = g.pos; i < end; ++i) {
        void* o = uobj::object_at(i);
        if (!o) continue;
        void* cls = uobj::class_of(o);
        if (!cls || kind_of(cls, names) == 0 || !counted(o)) continue;
        g.next.push_back(o);
        // a new instance counts from this poll, not from the end of the pass
        if (std::find(g.cur.begin(), g.cur.end(), o) == g.cur.end()) g.cur.push_back(o);
    }
    g.pos = end;
    if (end >= n) {
        g.cur.swap(g.next);
        g.next.clear();
        g.pos = 0;
        g.kinds.clear();  // a class unloaded with its level may leave its address to another
        if (++g.passes == 1)
            log::info("battle: first pass over {} objects done; {} live instance(s) of {}", n, g.cur.size(), classes());
    }
    ObjectsState s;
    for (void* o : g.cur) {
        if (!counted(o)) continue;
        ++s.instances;
        const int k = kind_of(uobj::class_of(o), names);
        bool shown = k == 2;
        if (k == 1) {
            alignas(16) std::uint8_t params[16]{};
            if (g_fn->ready() && uobj::call(o, g_fn->get(0), params)) shown = params[0] != 0;
            else ++g.calls_failed;
        }
        if (shown) ++s.visible;
    }
    s.shown = g.passes == 0 ? (s.visible > 0 ? 1 : -1) : (s.visible > 0 ? 1 : 0);
    g.state = s;
    ++g.polls;
    if (s.shown == 1) ++g.shown_polls;
    return g.state;
}

ObjectsState last() { return g.state; }

std::string status() {
    return std::format("battle objects [{}]: shown {} ({} of {} live instance(s)); passes {} polls {} shown {} failed calls {}", classes(),
                       g.state.shown, g.state.visible, g.state.instances, g.passes, g.polls, g.shown_polls, g.calls_failed);
}

}  // namespace ff7vr::engine::battle
