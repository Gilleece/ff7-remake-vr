#include "ff7vr/core/headset_profile.h"

#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/live_settings.h"
#include "ff7vr/core/log.h"

#include <windows.h>

#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace ff7vr::headset_profile {
namespace {

struct State {
    Config* cfg = nullptr;
    std::filesystem::path dir;
    // Start.
    std::string start_source, start_system, start_runtime, start_match;
    bool start_matched = false;
    size_t start_keys = 0;
    // Last session.
    bool session_seen = false;
    std::string session_system, session_runtime, session_match;
    bool session_matched = false;
    std::vector<std::string> live, restart, failed;  // of the last change of layer while running
};

std::mutex g_mutex;
State g_state;  // under g_mutex

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool iequal(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

std::string read_small_file(const std::filesystem::path& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 4096) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(f);
    return text;
}

bool write_small_file(const std::filesystem::path& p, const std::string& text) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
    CloseHandle(f);
    return ok;
}

std::string first_lines(const std::string& text, std::string* second) {
    auto clean = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        return s;
    };
    const size_t nl = text.find('\n');
    if (nl == std::string::npos) {
        second->clear();
        return clean(text);
    }
    const size_t nl2 = text.find('\n', nl + 1);
    *second = clean(text.substr(nl + 1, nl2 == std::string::npos ? std::string::npos : nl2 - nl - 1));
    return clean(text.substr(0, nl));
}

const HeadsetSection* find_section(const Config& cfg, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& h : cfg.headset_sections())
        if (iequal(h.match, name)) return &h;
    return nullptr;
}

std::optional<live_settings::Setting> find_setting(const std::vector<live_settings::Setting>& all, const std::string& section,
                                                   const std::string& key) {
    for (const auto& s : all)
        if (!s.section.empty() && iequal(s.section, section) && iequal(s.key, key) && s.kind != live_settings::Kind::Action) return s;
    return std::nullopt;
}

struct Change {
    std::string section, key, value;  // value now in effect (layer, base, or the built-in default)
};

// Runs the live setters of the changed keys; the rest wait for the next start.
void apply_changes(std::vector<Change> changes, std::string label) {
    std::vector<std::string> live, restart, failed;
    const auto all = live_settings::all();
    for (const Change& c : changes) {
        const std::string name = c.section + "." + c.key + " = " + c.value;
        const auto s = find_setting(all, c.section, c.key);
        if (!s || s->restart || s->read_only) {
            restart.push_back(name);
            continue;
        }
        if (s->optional && s->set_is_set) s->set_is_set(true);
        const std::string err = live_settings::set_from_ini(*s, c.value);
        if (err.empty())
            live.push_back(name);
        else
            failed.push_back(name + " (" + err + ")");
    }
    for (const auto& l : live) log::info("headset: {}: applied live: {}", label, l);
    for (const auto& l : restart) log::info("headset: {}: takes effect at the next start: {}", label, l);
    for (const auto& l : failed) log::warn("headset: {}: live change refused: {}", label, l);
    log::info("headset: {}: {} keys applied live, {} at the next start, {} refused", label, live.size(), restart.size(), failed.size());
    std::lock_guard lk(g_mutex);
    g_state.live = std::move(live);
    g_state.restart = std::move(restart);
    g_state.failed = std::move(failed);
}

}  // namespace

std::vector<std::string> apply_at_start(Config& cfg, const std::filesystem::path& dll_dir) {
    std::vector<std::string> out;
    std::lock_guard lk(g_mutex);
    g_state.cfg = &cfg;
    g_state.dir = dll_dir;
    const auto& sections = cfg.headset_sections();
    if (sections.empty()) return out;
    std::string sys, rt;
    if (lower(cfg.get_string("xr", "backend", "openxr")) == "null") {
        sys = kNullSystemName;
        rt = kNullRuntimeName;
        g_state.start_source = "the Null backend";
    } else {
        const std::string text = read_small_file(dll_dir / kStateFile);
        sys = first_lines(text, &rt);
        g_state.start_source = text.empty() ? "nothing (no earlier session recorded in ff7vr-headset.txt)"
                                            : "the previous session (ff7vr-headset.txt)";
    }
    g_state.start_system = sys;
    g_state.start_runtime = rt;
    const int idx = sys.empty() && rt.empty() ? -1 : cfg.match_headset(sys, rt);
    std::string names;
    for (const auto& h : sections) names += (names.empty() ? "" : ", ") + ("[headset:" + h.match + "]");
    if (idx < 0) {
        out.push_back(std::format("headset: {} sections ({}); at start: system '{}', runtime '{}' from {}: no section matches", sections.size(),
                                  names, sys, rt, g_state.start_source));
        return out;
    }
    const HeadsetSection& h = sections[size_t(idx)];
    cfg.set_overlay(h.match, h.entries);
    g_state.start_match = h.match;
    g_state.start_matched = true;
    g_state.start_keys = h.entries.size();
    out.push_back(std::format("headset: {} sections ({}); at start: system '{}', runtime '{}' from {}: applied [headset:{}] ({} keys)",
                              sections.size(), names, sys, rt, g_state.start_source, h.match, h.entries.size()));
    for (const auto& e : h.entries) out.push_back(std::format("headset:   {}.{} = {}", e.section, e.key, e.value));
    for (const auto& bad : h.invalid) out.push_back(std::format("headset:   ignored '{}' (write section.key = value)", bad));
    return out;
}

namespace {
void session_impl(std::string_view system_name, std::string_view runtime_name, bool remember) {
    std::vector<Change> changes;
    std::string label;
    {
        std::lock_guard lk(g_mutex);
        Config* cfg = g_state.cfg;
        if (!cfg) return;
        g_state.session_seen = true;
        g_state.session_system = system_name;
        g_state.session_runtime = runtime_name;
        const auto& sections = cfg->headset_sections();
        if (sections.empty()) return;
        // Remembered for the next start (only for players who use the sections; not for the
        // Null backend, whose names are known at start anyway).
        remember = remember && runtime_name != kNullRuntimeName;
        const std::string text = std::string(system_name) + "\r\n" + std::string(runtime_name) + "\r\n";
        if (remember && read_small_file(g_state.dir / kStateFile) != text && !write_small_file(g_state.dir / kStateFile, text))
            log::warn("headset: could not write {}", log::narrow((g_state.dir / kStateFile).wstring()));

        const int idx = cfg->match_headset(system_name, runtime_name);
        const std::string now = idx >= 0 ? sections[size_t(idx)].match : std::string();
        const std::string before = cfg->overlay_name();
        g_state.session_match = now;
        g_state.session_matched = idx >= 0;
        if (iequal(now, before)) {
            log::info("headset: session: system '{}', runtime '{}': {}", system_name, runtime_name,
                      now.empty() ? std::string("no [headset:...] section matches") : "[headset:" + now + "] (in effect since start)");
            return;
        }
        // The keys of the old and the new layer, with their values before the change.
        std::map<std::string, std::pair<HeadsetEntry, std::optional<std::string>>> keys;
        auto collect = [&](const HeadsetSection* h) {
            if (!h) return;
            for (const auto& e : h->entries) keys.try_emplace(lower(e.section) + "." + lower(e.key), e, cfg->get(e.section, e.key));
        };
        collect(find_section(*cfg, before));
        collect(find_section(*cfg, now));
        if (idx >= 0)
            cfg->set_overlay(now, sections[size_t(idx)].entries);
        else
            cfg->clear_overlay();
        for (auto& [k, kv] : keys) {
            const HeadsetEntry& e = kv.first;
            std::optional<std::string> after = cfg->get(e.section, e.key);
            if (!after) after = live_settings::default_value(e.section, e.key);
            if (!after) continue;  // no value to go back to: the module's own default stays
            if (kv.second && live_settings::same_value(*kv.second, *after)) continue;
            changes.push_back({e.section, e.key, *after});
        }
        label = std::format("session: system '{}', runtime '{}': {} -> {}", system_name, runtime_name,
                            before.empty() ? std::string("base settings") : "[headset:" + before + "]",
                            now.empty() ? std::string("base settings") : "[headset:" + now + "]");
        log::info("headset: {} ({} keys change)", label, changes.size());
        g_state.live.clear();
        g_state.restart.clear();
        g_state.failed.clear();
    }
    if (changes.empty()) return;
    // Setters may take locks of their modules: not on the caller's (render) thread.
    std::thread([changes = std::move(changes)]() mutable { apply_changes(std::move(changes), "change"); }).detach();
}
}  // namespace

void on_session(std::string_view system_name, std::string_view runtime_name) { session_impl(system_name, runtime_name, true); }

std::string active_section() {
    Config* cfg = nullptr;
    {
        std::lock_guard lk(g_mutex);
        cfg = g_state.cfg;
    }
    return cfg ? cfg->overlay_name() : std::string();
}

std::vector<std::string> status_lines() {
    std::lock_guard lk(g_mutex);
    std::vector<std::string> out;
    const Config* cfg = g_state.cfg;
    if (!cfg) return {"headset: not initialised"};
    const auto& sections = cfg->headset_sections();
    std::string names;
    for (const auto& h : sections) names += (names.empty() ? "" : ", ") + ("[headset:" + h.match + "]");
    out.push_back(std::format("sections: {}", sections.empty() ? std::string("none") : names));
    out.push_back(std::format("start: system '{}', runtime '{}' from {}: {}", g_state.start_system, g_state.start_runtime,
                              g_state.start_source.empty() ? std::string("-") : g_state.start_source,
                              g_state.start_matched ? std::format("[headset:{}] ({} keys)", g_state.start_match, g_state.start_keys)
                                                    : std::string("no match")));
    if (g_state.session_seen)
        out.push_back(std::format("session: system '{}', runtime '{}': {}", g_state.session_system, g_state.session_runtime,
                                  g_state.session_matched ? "[headset:" + g_state.session_match + "]" : std::string("no match")));
    else
        out.push_back("session: none yet");
    const std::string active = cfg->overlay_name();
    out.push_back(std::format("active layer: {}", active.empty() ? std::string("none (base settings)") : "[headset:" + active + "]"));
    if (const HeadsetSection* h = find_section(*cfg, active))
        for (const auto& e : h->entries) out.push_back(std::format("  {}.{} = {}", e.section, e.key, e.value));
    if (!g_state.live.empty() || !g_state.restart.empty() || !g_state.failed.empty())
        out.push_back(std::format("last change while running: {} live, {} at the next start, {} refused", g_state.live.size(),
                                  g_state.restart.size(), g_state.failed.size()));
    for (const auto& l : g_state.live) out.push_back("  live: " + l);
    for (const auto& l : g_state.restart) out.push_back("  next start: " + l);
    for (const auto& l : g_state.failed) out.push_back("  refused: " + l);
    return out;
}

void register_dev_command() {
    dev_commands::add("headset",
                      "headset status | headset session <system name>[|<runtime name>]: the [headset:...] section in effect; "
                      "`session` acts as if an XR session reported that headset",
                      [](std::string_view args) -> std::string {
                          while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
                          if (args.empty() || args == "status") {
                              std::string r = "ok";
                              for (const auto& l : status_lines()) r += " | " + l;
                              return r;
                          }
                          if (args.starts_with("session ")) {
                              std::string_view rest = args.substr(8);
                              const size_t bar = rest.find('|');
                              const std::string sys(rest.substr(0, bar));
                              const std::string rt(bar == std::string_view::npos ? std::string_view() : rest.substr(bar + 1));
                              session_impl(sys, rt, false);
                              return "ok session '" + sys + "' runtime '" + rt + "' (see the log for the keys applied)";
                          }
                          return "err usage: headset status | headset session <system name>[|<runtime name>]";
                      });
}

}  // namespace ff7vr::headset_profile
