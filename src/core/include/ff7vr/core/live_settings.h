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
//
// A key with no live path is still listed: `held()` makes a setting that keeps the value read
// at start, shows and changes it, and writes it on save; it takes effect at the next start
// (`restart`). The built-in default of every ini key (the value tools/package/ff7vr.ini
// ships) is in one table here: default_value().

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ff7vr::live_settings {

enum class Kind {
    Choice,  // one of `choices`; `ini_values` holds what is written to the ini for each
    Number,  // min..max in steps of `step`, shown with `decimals` decimals
    Action,  // a command (recenter, save, close): no value, `run` on select
    Text,    // a free string (shown; only "reset to default" changes it): get_text / set_text
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

    // The panel's category (Picture, Comfort, View, HUD, Performance, Controls, Advanced).
    std::string category;
    // Number: a Windows virtual-key code (shown with the key's name; 0 = no key).
    bool key_code = false;
    // No live path: the value is kept and saved, and takes effect at the next start.
    bool restart = false;
    // Shown, never changed from the panel (developer keys).
    bool read_only = false;
    // Text
    std::function<std::string()> get_text;
    std::function<std::string(const std::string&)> set_text;  // empty string = ok, otherwise the error
    // An optional line (a [stereo_cvars] entry the template ships commented out): "not set"
    // writes the line commented out (ini_file::Update::comment_out).
    bool optional = false;
    std::function<bool()> get_is_set;
    std::function<void(bool)> set_is_set;
    // The built-in default as written to the ini; empty: default_value(section, key).
    std::string default_ini;
    // An optional line not set by default.
    bool default_unset = false;
};

// Thread-safe. Registers or replaces (same id) a setting.
void add(Setting s);
// Thread-safe. A copy of all settings, sorted by `order`.
std::vector<Setting> all();

// The value of a setting as shown and as written to the ini (Choice: ini_values; Number:
// with `decimals`). Empty for actions and unknown values.
std::string display_value(const Setting& s);
std::string ini_value(const Setting& s);

// The built-in default of an ini key as written to the ini (the template's value); nullopt
// for a key the table does not know. `unset`: the template ships the line commented out.
std::optional<std::string> default_value(std::string_view section, std::string_view key, bool* unset = nullptr);
// The default of a setting (default_ini, else the table).
std::string default_of(const Setting& s);
// Sets a setting from a value as written in the ini (reset to default, `menu set`): empty
// string = ok, otherwise the error.
std::string set_from_ini(const Setting& s, const std::string& value);
// Whether the setting shows its default value now.
bool is_default(const Setting& s);

// A setting with no getter or setter of its own: keeps the value as written in the ini
// (`start_value`, or the default when the ini has no such key) and changes it. `live`, when
// set, is called with the new ini value after each change (a dev command, for example) and
// may refuse it; without it the setting is marked `restart`.
Setting held(Setting s, std::optional<std::string> start_value, std::function<std::string(const std::string&)> live = {});

// Short status lines for the panel's Performance header, provided by other modules (the
// texture streamer, the frame budget). Thread-safe; an unregistered name returns "".
void set_status(const std::string& name, std::function<std::string()> fn);
std::string status(const std::string& name);

// Compares two ini values the way Config reads them (numbers numerically, on/off/true/false
// as 1/0, otherwise without regard to case).
bool same_value(std::string a, std::string b);

}  // namespace ff7vr::live_settings
