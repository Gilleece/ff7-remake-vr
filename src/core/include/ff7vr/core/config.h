#pragma once
// Minimal INI reader for ff7vr.ini.
//
//   [section]
//   key = value      ; comment
//   # comment
//
// Section and key names are case-insensitive. Keys before any section header
// belong to the section "". A missing file is not an error: every getter
// returns the supplied default. Values are trimmed; surrounding double quotes
// are removed.
//
// Per-headset sections, [headset:<match>], hold lines of the form
// `section.key = value` (the key may itself contain dots: `stereo_cvars.r.Streaming.PoolSize`).
// They are kept apart from the base settings; set_overlay() makes one of them a layer that
// every getter consults before the base values (see headset_profile.h for when that happens).
//
//   ff7vr::Config cfg;
//   cfg.load(dll_dir / L"ff7vr.ini");
//   int w = cfg.get_int("xr", "width", 1920);

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr {

struct HeadsetEntry {
    std::string section, key, value;
};

struct HeadsetSection {
    std::string match;                   // the text after "headset:", trimmed, as written
    std::string header;                  // the section name as written ("headset: Quest 3"), first occurrence
    std::vector<HeadsetEntry> entries;   // in file order (a key written twice: the last one wins)
    std::vector<std::string> invalid;    // lines with no "section." prefix (ignored)
};

class Config {
public:
    Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // Returns false if the file could not be opened (defaults stay in effect).
    bool load(const std::filesystem::path& path);
    bool load_from_string(std::string_view text);

    bool loaded() const { return loaded_; }
    const std::filesystem::path& source() const { return source_; }

    bool has(std::string_view section, std::string_view key) const;
    std::optional<std::string> get(std::string_view section, std::string_view key) const;

    std::string get_string(std::string_view section, std::string_view key, std::string_view def) const;
    long long get_int(std::string_view section, std::string_view key, long long def) const;
    double get_float(std::string_view section, std::string_view key, double def) const;
    // true/false, 1/0, yes/no, on/off
    bool get_bool(std::string_view section, std::string_view key, bool def) const;

    // Sets the value only if the key is absent (a default from a bundle such as
    // [graphics] profile); returns true if it was set.
    bool set_default(std::string_view section, std::string_view key, std::string_view value);
    // Sets the value whether or not the key exists (a profile that overrides the ini);
    // returns true if the key existed before.
    bool set(std::string_view section, std::string_view key, std::string_view value);

    // All entries as "section.key = value" lines, for logging the effective config. Keys the
    // headset layer overrides show the layer's value and the base value; the headset sections
    // themselves are listed at the end.
    std::vector<std::string> dump() const;

    // ---- per-headset sections ----
    const std::vector<HeadsetSection>& headset_sections() const { return headsets_; }
    // Index of the first section (file order) whose match is contained, ignoring case, in the
    // system name or the runtime name; -1 when none. An empty match never matches.
    int match_headset(std::string_view system_name, std::string_view runtime_name) const;
    // The value a headset section of this file sets for section.key (as written), if any.
    std::optional<std::string> headset_value(std::string_view match, std::string_view section, std::string_view key) const;

    // The override layer. Thread-safe against the getters (the layer can change while the
    // game runs); the base values never change through it. `name` is the section's match.
    void set_overlay(std::string name, const std::vector<HeadsetEntry>& entries);
    void clear_overlay();
    std::string overlay_name() const;   // empty when no layer is active
    // The base value, ignoring the layer.
    std::optional<std::string> base_get(std::string_view section, std::string_view key) const;

private:
    static std::string make_key(std::string_view section, std::string_view key);
    std::optional<std::string> overlay_get(const std::string& k) const;
    std::map<std::string, std::string> values_;
    std::vector<HeadsetSection> headsets_;
    mutable std::mutex overlay_mutex_;
    std::map<std::string, std::string> overlay_;
    std::string overlay_name_;
    std::atomic<bool> has_overlay_{false};
    std::filesystem::path source_;
    bool loaded_ = false;
};

}  // namespace ff7vr
