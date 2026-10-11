#include "ff7vr/core/live_settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <mutex>

namespace ff7vr::live_settings {
namespace {

std::mutex g_mutex;
std::vector<Setting> g_settings;                                // under g_mutex
std::map<std::string, std::function<std::string()>> g_status;  // under g_mutex

// The built-in default of every key: the value tools/package/ff7vr.ini ships (keep the two
// in step). `unset`: the template has the line commented out.
struct Default {
    const char* section;
    const char* key;
    const char* value;
    bool unset = false;
};
constexpr Default kDefaults[] = {
    {"xr", "backend", "openxr"},
    {"xr", "runtime", "auto"},
    {"xr", "resolution_scale", "1.0"},
    {"xr", "retry_interval", "5"},
    {"xr", "pause_on_remove", "0"},
    {"xr", "pause_key", "77"},
    {"xr", "depth_layer", "1"},
    {"xr", "depth_far_m", "100000"},
    {"stereo", "enabled", "1"},
    {"stereo", "start_in_stereo", "1"},
    {"stereo", "world_scale", "1.0"},
    {"stereo", "decoupled_pitch", "1"},
    {"stereo", "positional", "1"},
    {"stereo", "mirror", "crop"},
    {"stereo", "movie_screen", "1"},
    {"stereo", "cutscene_screen", "1"},
    {"stereo", "comfort_cvars", "1"},
    {"stereo", "vr_window", "1280x720"},
    {"stereo", "ao_fix", "1"},
    {"stereo", "light_fix", "1"},
    {"stereo", "ssr_fix", "3"},
    {"stereo", "distortion_fix", "1"},
    {"stereo", "game_vignette", "0"},
    {"stereo", "tonemap_shift", "auto"},
    {"stereo", "render_scale", "1.0"},
    {"stereo", "dynamic_resolution", "0"},
    {"stereo", "dynamic_resolution_min", "0.75"},
    {"stereo", "dynamic_resolution_target", "0.85"},
    {"stereo", "late_update", "0"},
    {"stereo", "hold_target_seconds", "30"},
    {"stereo", "allow_unknown_build", "0"},
    {"stereo", "cutscene_screen_delay_ms", "500"},
    {"stereo", "cutscene_screen_hold_ms", "300"},
    {"camera", "boom", "level"},
    {"camera", "combat", "game"},
    {"camera", "blend_seconds", "0.35"},
    {"camera", "miss_seconds", "1.5"},
    {"camera", "collision", "1"},
    {"camera", "collision_margin", "20"},
    {"camera", "move", "camera"},
    {"first_person", "enabled", "1"},
    {"first_person", "default", "1"},
    {"first_person", "auto_combat", "1"},
    {"first_person", "battle_signal", "EndBattleAPI.GetBattleSceneID result=0:4"},
    {"first_person", "battle_objects", "1"},
    {"first_person", "battle_exit_seconds", "3"},
    {"first_person", "toggle_key", "36"},
    {"first_person", "pad_toggle", "1"},
    {"first_person", "blend_seconds", "0.35"},
    {"first_person", "head_bob", "0"},
    {"first_person", "steady_seconds", "0.3"},
    {"first_person", "hide", "pass"},
    {"first_person", "audio_listener", "1"},
    {"first_person", "battle_log", "0"},
    {"first_person", "prompt_log", "0"},
    {"first_person", "move", "camera"},
    {"controls", "recenter_key", "35"},
    {"controls", "stereo_key", "45"},
    {"controls", "ui_nearer_key", "34"},
    {"controls", "ui_farther_key", "33"},
    {"controls", "ui_step", "0.25"},
    {"controls", "ui_min", "0.75"},
    {"controls", "ui_max", "8"},
    {"controls", "pad", "1"},
    {"controls", "pad_hold_view", "1"},
    {"controls", "fp_toggle_chord", "L3+R3"},
    {"controls", "fp_toggle_chord_ms", "150"},
    {"controls", "pad_source", "auto"},
    {"controls", "pad_log", "0"},
    {"controls", "brightness_up_key", "0"},
    {"controls", "brightness_down_key", "0"},
    {"controls", "brightness_step", "0.05"},
    {"menu", "key", "46"},
    {"menu", "pad", "1"},
    {"menu", "distance", "1.2"},
    {"menu", "size", "0.8"},
    {"ui", "layer", "1"},
    {"ui", "distance", "3.0"},
    {"ui", "size", "2.0"},
    {"ui", "battle_size", "0"},
    {"ui", "follow_head", "0"},
    {"ui", "align", "1"},
    {"ui", "align_target_test", "0"},
    {"screen", "distance", "2.0"},
    {"screen", "width", "1.8"},
    {"picture", "brightness", "-0.25"},
    {"picture", "contrast", "1.5"},
    {"picture", "saturation", "1.2"},
    {"picture", "gamma", "1.0"},
    {"picture", "black_level", "0"},
    {"picture", "sharpen", "1"},
    {"comfort", "vignette", "0"},
    {"comfort", "vignette_radius", "0.55"},
    {"comfort", "vignette_softness", "0.25"},
    {"comfort", "cut_fade_ms", "0"},
    {"comfort", "cut_fade_deg", "25"},
    {"comfort", "cut_fade_m", "1.0"},
    {"comfort", "snap_turn", "0"},
    {"comfort", "snap_turn_deadzone", "0.6"},
    {"comfort", "snap_turn_repeat_ms", "0"},
    {"comfort", "snap_left_key", "0"},
    {"comfort", "snap_right_key", "0"},
    {"graphics", "profile", "custom"},
    {"foveation", "enabled", "1"},
    {"foveation", "preset", "performance"},
    {"foveation", "eye_tracking", "0"},
    {"foveation", "gaze_margin_deg", "5"},
    {"foveation", "gaze_smoothing", "0.5"},
    {"foveation", "subsurface_full_rate", "1"},
    {"foveation", "translucency_full_rate", "0"},
    {"foveation", "dlss_finer", "1"},
    {"dlss", "enabled", "0"},
    {"dlss", "mode", "upscale"},
    {"dlss", "input_scale", "0.60"},
    {"dlss", "output", "runtime"},
    {"dlss", "preset", "m"},
    {"dlss", "firefly", "0"},
    {"dlss", "texture_bias", "auto"},
    {"loader", "force_d3d11", "1"},
    {"log", "level", "debug"},
    {"log", "keep_sessions", "5"},
    {"log", "frame_budget", "1"},
    {"log", "upload_check", "1"},
    {"stereo", "light_volume_pool", "0"},
    {"crash", "enabled", "1"},
    {"crash", "first_chance", "1"},
    {"crash", "full_memory", "0", true},
    {"dev", "pipe", "0"},
    {"dev", "virtual_pad", "0"},
    {"dev", "unsafe_commands", "0"},
    {"stereo_cvars", "r.BloomQuality", "0"},
    {"stereo_cvars", "r.DisableDistortion", "1", true},
    {"stereo_cvars", "r.Streaming.PoolSize", "2500", true},
    {"stereo_cvars", "r.Streaming.Boost", "1", true},
    {"stereo_cvars", "r.StaticMeshLODDistanceScale", "0.5"},
    {"stereo_cvars", "r.SkeletalMeshLODBias", "-1"},
    {"stereo_cvars", "foliage.LODDistanceScale", "2"},
    {"stereo_cvars", "r.ViewDistanceScale", "1.5"},
    {"stereo_cvars", "r.DepthOfFieldQuality", "0", true},
};

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parse_float(const std::string& t, float* out) {
    const char* b = t.data();
    const char* e = t.data() + t.size();
    while (b < e && (*b == ' ' || *b == '+')) ++b;
    while (e > b && e[-1] == ' ') --e;
    if (b == e) return false;
    float v = 0;
    const auto [p, ec] = std::from_chars(b, e, v);
    if (ec != std::errc() || p != e || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

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
        case Kind::Text:
            return s.get_text ? s.get_text() : std::string("?");
    }
    return {};
}

std::string ini_value(const Setting& s) {
    if (s.kind == Kind::Text) return s.get_text ? s.get_text() : std::string();
    if (s.get_ini && s.kind != Kind::Action) return s.get_ini();
    switch (s.kind) {
        case Kind::Choice: {
            const int i = s.get_choice ? s.get_choice() : -1;
            return i >= 0 && i < int(s.ini_values.size()) ? s.ini_values[size_t(i)] : std::string();
        }
        case Kind::Number:
            return s.get_number ? number_text(s.get_number(), s.decimals) : std::string();
        case Kind::Action:
        case Kind::Text:
            return {};
    }
    return {};
}

std::optional<std::string> default_value(std::string_view section, std::string_view key, bool* unset) {
    for (const Default& d : kDefaults)
        if (iequal(d.section, section) && iequal(d.key, key)) {
            if (unset) *unset = d.unset;
            return std::string(d.value);
        }
    return std::nullopt;
}

std::string default_of(const Setting& s) {
    if (!s.default_ini.empty()) return s.default_ini;
    return default_value(s.section, s.key).value_or(std::string());
}

bool same_value(std::string a, std::string b) {
    a = lower(std::move(a));
    b = lower(std::move(b));
    auto boolish = [](std::string& s) {
        if (s == "on" || s == "true" || s == "yes") s = "1";
        if (s == "off" || s == "false" || s == "no") s = "0";
    };
    boolish(a);
    boolish(b);
    float x = 0, y = 0;
    if (parse_float(a, &x) && parse_float(b, &y)) return std::fabs(x - y) < 1e-4f;
    return a == b;
}

std::string set_from_ini(const Setting& s, const std::string& value) {
    switch (s.kind) {
        case Kind::Choice: {
            if (!s.set_choice) return "cannot be changed";
            for (size_t i = 0; i < s.ini_values.size(); ++i)
                if (same_value(s.ini_values[i], value)) return s.set_choice(int(i));
            for (size_t i = 0; i < s.choices.size(); ++i)
                if (iequal(s.choices[i], value)) return s.set_choice(int(i));
            return "not one of the values: " + value;
        }
        case Kind::Number: {
            float v = 0;
            if (!s.set_number) return "cannot be changed";
            if (!parse_float(value, &v)) return "not a number: " + value;
            return s.set_number(v);
        }
        case Kind::Text:
            return s.set_text ? s.set_text(value) : std::string("cannot be changed");
        case Kind::Action:
            return "an action has no value";
    }
    return {};
}

bool is_default(const Setting& s) {
    if (s.kind == Kind::Action || s.section.empty()) return true;
    bool unset = s.default_unset;
    if (s.default_ini.empty()) default_value(s.section, s.key, &unset);
    if (s.optional && s.get_is_set) {
        const bool set = s.get_is_set();
        if (set == unset) return false;  // set where the default is "not set", or the other way round
        if (!set) return true;
    }
    return same_value(ini_value(s), default_of(s));
}

Setting held(Setting s, std::optional<std::string> start_value, std::function<std::string(const std::string&)> live) {
    auto value = std::make_shared<std::string>(start_value.value_or(default_of(s)));
    auto m = std::make_shared<std::mutex>();
    if (!live) s.restart = true;
    // Stores the new ini value once `live` (if any) accepted it.
    auto store = [value, m, live](const std::string& v) {
        if (live) {
            const std::string err = live(v);
            if (!err.empty()) return err;
        }
        std::lock_guard lk(*m);
        *value = v;
        return std::string();
    };
    auto read = [value, m] {
        std::lock_guard lk(*m);
        return *value;
    };
    switch (s.kind) {
        case Kind::Choice: {
            const std::vector<std::string> iv = s.ini_values;
            s.get_choice = [read, iv] {
                const std::string v = read();
                for (size_t i = 0; i < iv.size(); ++i)
                    if (same_value(iv[i], v)) return int(i);
                return -1;
            };
            s.set_choice = [store, iv](int i) {
                if (i < 0 || i >= int(iv.size())) return std::string("out of range");
                return store(iv[size_t(i)]);
            };
            break;
        }
        case Kind::Number: {
            const int dec = s.decimals;
            s.get_number = [read] {
                float v = 0;
                return parse_float(read(), &v) ? v : std::nanf("");
            };
            s.set_number = [store, dec](float v) { return store(number_text(v, dec)); };
            break;
        }
        case Kind::Text:
            s.get_text = read;
            s.set_text = store;
            break;
        case Kind::Action:
            break;
    }
    return s;
}

void set_status(const std::string& name, std::function<std::string()> fn) {
    std::lock_guard lk(g_mutex);
    g_status[name] = std::move(fn);
}

std::string status(const std::string& name) {
    std::function<std::string()> fn;
    {
        std::lock_guard lk(g_mutex);
        const auto it = g_status.find(name);
        if (it == g_status.end() || !it->second) return {};
        fn = it->second;
    }
    return fn();
}

}  // namespace ff7vr::live_settings
