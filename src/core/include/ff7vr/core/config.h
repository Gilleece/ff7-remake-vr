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
//   ff7vr::Config cfg;
//   cfg.load(dll_dir / L"ff7vr.ini");
//   int w = cfg.get_int("xr", "width", 1920);

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr {

class Config {
public:
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

    // All entries as "section.key = value" lines, for logging the effective config.
    std::vector<std::string> dump() const;

private:
    static std::string make_key(std::string_view section, std::string_view key);
    std::map<std::string, std::string> values_;
    std::filesystem::path source_;
    bool loaded_ = false;
};

}  // namespace ff7vr
