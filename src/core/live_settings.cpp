#include "ff7vr/core/live_settings.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace ff7vr::live_settings {
namespace {

std::mutex g_mutex;
std::vector<Setting> g_settings;  // under g_mutex

std::string number_text(float v, int decimals) {
    if (!std::isfinite(v)) return {};
    if (std::fabs(v) < 0.5f * std::pow(10.0f, -float(decimals))) v = 0.0f;  // no "-0.00"
    return std::format("{:.{}f}", v, std::clamp(decimals, 0, 6));
}

}  // namespace

void add(Setting s) {
    std::lock_guard lk(g_mutex);
    const auto it = std::find_if(g_settings.begin(), g_settings.end(), [&](const Setting& e) { return e.id == s.id; });
    if (it != g_settings.end())
        *it = std::move(s);
    else
        g_settings.push_back(std::move(s));
}

std::vector<Setting> all() {
    std::vector<Setting> out;
    {
        std::lock_guard lk(g_mutex);
        out = g_settings;
    }
    std::stable_sort(out.begin(), out.end(), [](const Setting& a, const Setting& b) { return a.order < b.order; });
    return out;
}

std::string display_value(const Setting& s) {
    switch (s.kind) {
        case Kind::Choice: {
            const int i = s.get_choice ? s.get_choice() : -1;
            return i >= 0 && i < int(s.choices.size()) ? s.choices[size_t(i)] : std::string("?");
        }
        case Kind::Number:
            return s.get_number ? number_text(s.get_number(), s.decimals) : std::string("?");
        case Kind::Action:
            return {};
    }
    return {};
}

std::string ini_value(const Setting& s) {
    if (s.get_ini && s.kind != Kind::Action) return s.get_ini();
    switch (s.kind) {
        case Kind::Choice: {
            const int i = s.get_choice ? s.get_choice() : -1;
            return i >= 0 && i < int(s.ini_values.size()) ? s.ini_values[size_t(i)] : std::string();
        }
        case Kind::Number:
            return s.get_number ? number_text(s.get_number(), s.decimals) : std::string();
        case Kind::Action:
            return {};
    }
    return {};
}

}  // namespace ff7vr::live_settings
