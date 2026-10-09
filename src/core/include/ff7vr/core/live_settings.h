#pragma once
// Settings that can be changed while the game runs, for the in-headset settings panel
// (src/render/src/menu.cpp). Each module registers the settings it owns with a getter
// and a setter that use its existing live-change path; the panel lists them, changes
// them and writes them back to ff7vr.ini (ini_file.h).
//
//   live_settings::Setting s;
//   s.id = "vignette"; s.label = "Comfort vignette"; s.section = "comfort"; s.key = "vignette";
//   s.kind = live_settings::Kind::Number; s.min = 0; s.max = 1; s.step = 0.1f; s.decimals = 1;
//   s.get_number = [] { return current(); };
//   s.set_number = [](float v) { return apply(v); };
//   live_settings::add(std::move(s));
//
// Getters and setters may be called from any thread (the panel's own thread and the dev
// pipe), never from inside the game's or the renderer's hot paths, and must not block for
// long.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ff7vr::live_settings {

enum class Kind {
    Choice,  // one of `choices`; `ini_values` holds what is written to the ini for each
    Number,  // min..max in steps of `step`, shown with `decimals` decimals
    Action,  // a command (recenter, save, close): no value, `run` on select
};

struct Setting {
    std::string id;     // stable name for the dev command (`menu set <id> <value>`)
    std::string label;  // shown in the panel
    int order = 0;      // position in the panel (ascending)
    Kind kind = Kind::Number;
    // Where the value is saved; empty section: not saved (actions, values with no ini key).
    std::string section, key;
    // Choice
    std::vector<std::string> choices;
    std::vector<std::string> ini_values;
    std::function<int()> get_choice;              // index into choices, -1 = unknown
    std::function<std::string(int)> set_choice;   // empty string = ok, otherwise the error
    // Number
    float min = 0.0f, max = 1.0f, step = 0.1f;
    int decimals = 2;
    std::function<float()> get_number;
    std::function<std::string(float)> set_number;  // empty string = ok, otherwise the error
    // Action: returns a short text for the panel's message line.
    std::function<std::string()> run;
    // Optional: whether the setting can be used now (for example only in stereo); greyed when not.
    std::function<bool()> available;
    // Optional: the value written to the ini when it is not the shown one (first/third person
    // shows the mode now but saves the mode to return to after a battle).
    std::function<std::string()> get_ini;
    // Optional: more keys of the same section written with this one ({key, value}).
    std::function<std::vector<std::pair<std::string, std::string>>()> extra_ini;
    // Part of the [graphics] profile bundle: changing it by hand makes the profile "custom"
    // (the setting with id "graphics_profile"), so that the saved value is not replaced by
    // the profile at the next start.
    bool part_of_profile = false;
};

// Thread-safe. Registers or replaces (same id) a setting.
void add(Setting s);
// Thread-safe. A copy of all settings, sorted by `order`.
std::vector<Setting> all();

// The value of a setting as shown and as written to the ini (Choice: ini_values; Number:
// with `decimals`). Empty for actions and unknown values.
std::string display_value(const Setting& s);
std::string ini_value(const Setting& s);

}  // namespace ff7vr::live_settings
