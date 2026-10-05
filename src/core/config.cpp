#include "ff7vr/core/config.h"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ff7vr {
namespace {

std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

}  // namespace

std::string Config::make_key(std::string_view section, std::string_view key) {
    return lower(trim(section)) + '\x1f' + lower(trim(key));
}

bool Config::load(const std::filesystem::path& path) {
    source_ = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return load_from_string(ss.str());
}

bool Config::load_from_string(std::string_view text) {
    // Skip a UTF-8 BOM (Windows PowerShell 5.1 writes one by default).
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);

    std::string section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            size_t close = line.find(']');
            section = std::string(trim(line.substr(1, close == std::string_view::npos ? line.size() - 1 : close - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = trim(line.substr(0, eq));
        std::string_view value = trim(line.substr(eq + 1));
        if (!value.empty() && value.front() == '"') {
            size_t close = value.find('"', 1);
            value = value.substr(1, close == std::string_view::npos ? value.size() - 1 : close - 1);
        } else if (!value.empty() && (value.front() == ';' || value.front() == '#')) {
            value = {};  // "key =   ; comment": an empty value followed by a comment
        } else {
            // Inline comment: " ;" or " #" after the value.
            for (size_t i = 1; i < value.size(); ++i) {
                if ((value[i] == ';' || value[i] == '#') && (value[i - 1] == ' ' || value[i - 1] == '\t')) {
                    value = trim(value.substr(0, i));
                    break;
                }
            }
        }
        if (!key.empty()) values_[make_key(section, key)] = std::string(value);
    }
    loaded_ = true;
    return true;
}

bool Config::has(std::string_view section, std::string_view key) const {
    return values_.contains(make_key(section, key));
}

std::optional<std::string> Config::get(std::string_view section, std::string_view key) const {
    auto it = values_.find(make_key(section, key));
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::string Config::get_string(std::string_view section, std::string_view key, std::string_view def) const {
    auto v = get(section, key);
    return v ? *v : std::string(def);
}

long long Config::get_int(std::string_view section, std::string_view key, long long def) const {
    auto v = get(section, key);
    if (!v || v->empty()) return def;
    std::string_view s = *v;
    int base = 10;
    bool neg = false;
    if (s.front() == '-') { neg = true; s.remove_prefix(1); }
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s.remove_prefix(2); }
    long long out = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out, base);
    if (ec != std::errc() || p != s.data() + s.size()) return def;
    return neg ? -out : out;
}

double Config::get_float(std::string_view section, std::string_view key, double def) const {
    auto v = get(section, key);
    if (!v || v->empty()) return def;
    double out = 0;
    auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
    if (ec != std::errc() || p != v->data() + v->size()) return def;
    return out;
}

bool Config::get_bool(std::string_view section, std::string_view key, bool def) const {
    auto v = get(section, key);
    if (!v) return def;
    std::string s = lower(*v);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

std::vector<std::string> Config::dump() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : values_) {
        std::string key = k;
        size_t sep = key.find('\x1f');
        if (sep != std::string::npos) key[sep] = '.';
        out.push_back(key + " = " + v);
    }
    return out;
}

}  // namespace ff7vr
