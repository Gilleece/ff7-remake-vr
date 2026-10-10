#include "menu_items.h"

#include "audio_listener.h"
#include "bloom_fix.h"
#include "controls.h"
#include "distortion_fix.h"
#include "fixes.h"
#include "graphics.h"
#include "head_move.h"
#include "late_update.h"
#include "lens_vignette.h"
#include "mirror.h"
#include "movie_watch.h"
#include "player.h"
#include "snap_turn.h"
#include "stereo_device.h"
#include "texture_streaming.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/live_settings.h"
#include "ff7vr/core/log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <memory>

namespace ff7vr::engine::menu_items {
namespace {

using live_settings::Kind;
using live_settings::Setting;

// Category bands of `order`: the panel lists the categories in this order and sorts the
// settings of a category by `order`.
constexpr int kPicture = 100, kComfort = 200, kView = 300, kHud = 400, kPerformance = 500, kControls = 600, kAdvanced = 700;

const char* category_of(int order) {
    return order < kComfort ? "Picture"
           : order < kView ? "Comfort"
           : order < kHud ? "View"
           : order < kPerformance ? "HUD"
           : order < kControls ? "Performance"
           : order < kAdvanced ? "Controls"
                               : "Advanced";
}

Setting base(const char* id, const char* label, int order, const char* section, const char* key, Kind kind) {
    Setting s;
    s.id = id;
    s.label = label;
    s.order = order;
    s.category = category_of(order);
    s.kind = kind;
    s.section = section;
    s.key = key;
    return s;
}

std::string ok_or_error(const std::string& reply) { return reply.starts_with("ok") ? std::string() : reply; }

std::string dispatch(const std::string& line) {
    std::string reply;
    if (!dev_commands::dispatch(line, reply)) return "not available in this build";
    return ok_or_error(reply);
}

Setting on_off(const char* id, const char* label, int order, const char* section, const char* key, std::atomic<bool>& value) {
    Setting s = base(id, label, order, section, key, Kind::Choice);
    s.choices = {"Off", "On"};
    s.ini_values = {"0", "1"};
    s.get_choice = [&value] { return value.load() ? 1 : 0; };
    s.set_choice = [&value, id](int i) {
        value = i == 1;
        log::info("menu: {} {}", id, i == 1 ? "on" : "off");
        return std::string();
    };
    return s;
}

// Two named values of a boolean ([camera] boom = level | game): [0] = false, [1] = true.
Setting named_bool(const char* id, const char* label, int order, const char* section, const char* key, std::atomic<bool>& value,
                   std::vector<std::string> choices, std::vector<std::string> ini) {
    Setting s = base(id, label, order, section, key, Kind::Choice);
    s.choices = std::move(choices);
    s.ini_values = std::move(ini);
    s.get_choice = [&value] { return value.load() ? 1 : 0; };
    s.set_choice = [&value, id](int i) {
        value = i == 1;
        log::info("menu: {} {}", id, i);
        return std::string();
    };
    return s;
}

Setting number(const char* id, const char* label, int order, const char* section, const char* key, float mn, float mx, float step, int dec,
               std::atomic<float>& value) {
    Setting s = base(id, label, order, section, key, Kind::Number);
    s.min = mn;
    s.max = mx;
    s.step = step;
    s.decimals = dec;
    s.get_number = [&value] { return value.load(); };
    s.set_number = [&value, id, mn, mx, dec](float v) {
        value = std::clamp(v, mn, mx);
        log::info("menu: {} {:.{}f}", id, value.load(), dec);
        return std::string();
    };
    return s;
}

Setting integer(const char* id, const char* label, int order, const char* section, const char* key, float mn, float mx, float step,
                std::atomic<int>& value, bool key_code = false) {
    Setting s = base(id, label, order, section, key, Kind::Number);
    s.min = mn;
    s.max = mx;
    s.step = step;
    s.decimals = 0;
    s.key_code = key_code;
    s.get_number = [&value] { return float(value.load()); };
    s.set_number = [&value, id, mn, mx](float v) {
        value = int(std::lround(std::clamp(v, mn, mx)));
        log::info("menu: {} {}", id, value.load());
        return std::string();
    };
    return s;
}

// A value whose module keeps no getter: kept as written in the ini, `live` applies a change.
void add_held(Setting s, const Config& cfg, std::function<std::string(const std::string&)> live) {
    auto start = cfg.get(s.section, s.key);
    live_settings::add(live_settings::held(std::move(s), std::move(start), std::move(live)));
}

Setting choice(const char* id, const char* label, int order, const char* section, const char* key, std::vector<std::string> choices,
               std::vector<std::string> ini) {
    Setting s = base(id, label, order, section, key, Kind::Choice);
    s.choices = std::move(choices);
    s.ini_values = std::move(ini);
    return s;
}

Setting num_spec(const char* id, const char* label, int order, const char* section, const char* key, float mn, float mx, float step, int dec) {
    Setting s = base(id, label, order, section, key, Kind::Number);
    s.min = mn;
    s.max = mx;
    s.step = step;
    s.decimals = dec;
    return s;
}

std::atomic<int> g_cut_delay{500}, g_cut_hold{300};

}  // namespace

void register_all(const Config& cfg) {
    player::Settings& p = player::settings();
    device::Settings& d = device::settings();
    controls::Settings& c = controls::settings();

    // ---- Picture ----
    {
        Setting s = num_spec("stereo.game_vignette", "Game's own vignette", kPicture + 70, "stereo", "game_vignette", 0.0f, 1.0f, 0.1f, 1);
        s.get_number = [] { return lens_vignette::scale(); };
        s.set_number = [](float v) {
            lens_vignette::set_scale(std::clamp(v, 0.0f, 1.0f));
            return std::string();
        };
        live_settings::add(std::move(s));
    }

    // ---- Comfort ----
    {
        Setting s = choice("snap_turn", "Snap turn", kComfort + 10, "comfort", "snap_turn", {"Off", "30 degrees", "45 degrees"}, {"0", "30", "45"});
        s.get_choice = [] {
            const float deg = snap_turn::degrees();
            return deg == 0.0f ? 0 : std::fabs(deg - 30.0f) < 0.01f ? 1 : std::fabs(deg - 45.0f) < 0.01f ? 2 : -1;
        };
        s.set_choice = [](int i) {
            const char* v[] = {"off", "30", "45"};
            return ok_or_error(snap_turn::command(std::string("snap ") + v[std::clamp(i, 0, 2)]));
        };
        live_settings::add(std::move(s));
    }
    add_held(num_spec("comfort.snap_turn_deadzone", "Snap turn: stick threshold", kComfort + 11, "comfort", "snap_turn_deadzone", 0.1f, 0.95f, 0.05f, 2),
             cfg, [](const std::string& v) { return ok_or_error(snap_turn::command("deadzone " + v)); });
    add_held(num_spec("comfort.snap_turn_repeat_ms", "Snap turn: repeat while held (ms)", kComfort + 12, "comfort", "snap_turn_repeat_ms", 0, 2000, 50, 0),
             cfg, [](const std::string& v) { return ok_or_error(snap_turn::command("repeat " + v)); });
    {
        Setting s = num_spec("world_scale", "World scale", kComfort + 40, "stereo", "world_scale", 0.5f, 2.0f, 0.05f, 2);
        s.get_number = [&d] { return d.world_scale.load(); };
        s.set_number = [&d](float v) {
            d.world_scale = std::clamp(v, 0.1f, 10.0f);  // as `stereo scale`
            log::info("menu: world scale {:.2f}", d.world_scale.load());
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    live_settings::add(on_off("decoupled_pitch", "Level horizon (decoupled pitch)", kComfort + 41, "stereo", "decoupled_pitch", d.decouple_pitch));
    live_settings::add(on_off("stereo.positional", "Head position moves the view", kComfort + 42, "stereo", "positional", d.positional));
    live_settings::add(on_off("head_bob", "Head bob (first person)", kComfort + 50, "first_person", "head_bob", p.head_bob));
    live_settings::add(number("first_person.steady_seconds", "Head steadiness (s)", kComfort + 51, "first_person", "steady_seconds", 0.05f, 2.0f, 0.05f, 2,
                              p.steady_seconds));
    add_held(choice("first_person.move", "Stick moves (first person)", kComfort + 60, "first_person", "move", {"Camera direction", "Head direction"},
                    {"camera", "head"}),
             cfg, [](const std::string& v) { return ok_or_error(head_move::command(v + " first")); });
    add_held(choice("camera.move", "Stick moves (third person)", kComfort + 61, "camera", "move", {"Camera direction", "Head direction"}, {"camera", "head"}),
             cfg, [](const std::string& v) { return ok_or_error(head_move::command(v + " third")); });

    // ---- View ----
    {
        Setting s = choice("view", "View", kView + 10, "first_person", "default", {"Third person", "First person"}, {"0", "1"});
        s.get_choice = [] { return player::first_person_selected() ? 1 : 0; };
        s.set_choice = [](int i) {
            // The mode now, and the one to return to after a battle ([first_person] default).
            player::settings().fp_default = i == 1;
            player::request_mode(i == 1);
            log::info("menu: {} person", i == 1 ? "first" : "third");
            return std::string();
        };
        s.get_ini = [] { return std::string(player::settings().fp_default.load() ? "1" : "0"); };
        s.available = [] { return player::settings().fp_available.load(); };
        live_settings::add(std::move(s));
    }
    live_settings::add(on_off("first_person.auto_combat", "Third person in battles", kView + 11, "first_person", "auto_combat", p.auto_combat));
    live_settings::add(number("first_person.blend_seconds", "First/third person blend (s)", kView + 12, "first_person", "blend_seconds", 0.0f, 2.0f, 0.05f, 2,
                              p.blend_seconds));
    {
        Setting s = choice("first_person.hide", "Character in first person", kView + 13, "first_person", "hide",
                           {"Left out (shadow kept)", "Meshes hidden", "Shown"}, {"pass", "meshes", "none"});
        s.get_choice = [&p] {
            const int h = p.hide.load();
            return h == 4 ? 0 : h == 1 ? 1 : h == 0 ? 2 : -1;
        };
        s.set_choice = [&p](int i) {
            p.hide = i == 0 ? 4 : i == 1 ? 1 : 0;
            log::info("menu: first person hide {}", i == 0 ? "pass" : i == 1 ? "meshes" : "none");
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    add_held(choice("first_person.audio_listener", "Sound heard from your head", kView + 14, "first_person", "audio_listener", {"Off", "On"}, {"0", "1"}),
             cfg, [](const std::string& v) {
                 audio_listener::set_enabled(v == "1");
                 return std::string();
             });
    add_held(base("first_person.battle_signal", "Battle signal", kView + 15, "first_person", "battle_signal", Kind::Text), cfg, [](const std::string& v) {
        player::set_battle_signal(v);
        return std::string();
    });
    live_settings::add(named_bool("camera.boom", "Third person height", kView + 20, "camera", "boom", p.level_boom, {"Game camera", "Shoulder level"},
                                  {"game", "level"}));
    live_settings::add(named_bool("camera.combat", "Battle camera", kView + 21, "camera", "combat", p.combat_level, {"Game's shots", "Behind the character"},
                                  {"game", "level"}));
    live_settings::add(number("camera.blend_seconds", "Camera change blend (s)", kView + 22, "camera", "blend_seconds", 0.0f, 2.0f, 0.05f, 2,
                              p.camera_blend_seconds));
    live_settings::add(number("camera.miss_seconds", "Camera may look away for (s)", kView + 23, "camera", "miss_seconds", 0.0f, 10.0f, 0.25f, 2,
                              p.miss_seconds));
    live_settings::add(on_off("camera.collision", "Camera collision", kView + 24, "camera", "collision", p.collision));
    live_settings::add(number("camera.collision_margin", "Camera collision margin (cm)", kView + 25, "camera", "collision_margin", 0.0f, 100.0f, 5.0f, 0,
                              p.collision_margin));
    add_held(choice("stereo.movie_screen", "Movies on the virtual screen", kView + 30, "stereo", "movie_screen", {"Off", "On"}, {"0", "1"}), cfg,
             [](const std::string& v) {
                 movie::set_enabled(v == "1");
                 return std::string();
             });
    add_held(choice("stereo.cutscene_screen", "Cutscenes on the virtual screen", kView + 31, "stereo", "cutscene_screen", {"Off", "On"}, {"0", "1"}), cfg,
             [](const std::string& v) {
                 movie::set_cutscene(v == "1");
                 return std::string();
             });
    g_cut_delay = static_cast<int>(cfg.get_int("stereo", "cutscene_screen_delay_ms", 500));
    g_cut_hold = static_cast<int>(cfg.get_int("stereo", "cutscene_screen_hold_ms", 300));
    {
        Setting s = integer("stereo.cutscene_screen_delay_ms", "Cutscene screen: delay (ms)", kView + 32, "stereo", "cutscene_screen_delay_ms", 0, 5000, 100,
                            g_cut_delay);
        s.set_number = [](float v) {
            g_cut_delay = int(std::lround(std::clamp(v, 0.0f, 5000.0f)));
            movie::set_cutscene_times(g_cut_delay.load(), g_cut_hold.load());
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s = integer("stereo.cutscene_screen_hold_ms", "Cutscene screen: hold (ms)", kView + 33, "stereo", "cutscene_screen_hold_ms", 0, 5000, 100,
                            g_cut_hold);
        s.set_number = [](float v) {
            g_cut_hold = int(std::lround(std::clamp(v, 0.0f, 5000.0f)));
            movie::set_cutscene_times(g_cut_delay.load(), g_cut_hold.load());
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    {
        // Live only: [stereo] enabled is the start-up switch (a row of its own).
        Setting s = choice("stereo", "3D now (Insert)", kView + 40, "", "", {"Off (virtual screen)", "On"}, {});
        s.get_choice = [] { return device::wanted() ? 1 : 0; };
        s.set_choice = [](int i) {
            // The player's own key path: it also reconnects the headset when the session is gone.
            if ((i == 1) != device::wanted()) controls::command("stereo");
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    {
        static const char* const kMirror[] = {"crop", "left", "right", "both", "off"};
        Setting s = choice("stereo.mirror", "Monitor shows", kView + 50, "stereo", "mirror", {"Left eye, cropped", "Left eye", "Right eye", "Both eyes", "Nothing"},
                           {"crop", "left", "right", "both", "off"});
        s.get_choice = [&d] {
            for (int i = 0; i < 5; ++i) {
                mirror::Mode m{};
                if (mirror::parse_mode(kMirror[i], m) && static_cast<int>(m) == d.mirror.load()) return i;
            }
            return -1;
        };
        s.set_choice = [&d](int i) {
            mirror::Mode m{};
            if (i < 0 || i > 4 || !mirror::parse_mode(kMirror[i], m)) return std::string("unknown mode");
            d.mirror = static_cast<int>(m);
            return std::string();
        };
        live_settings::add(std::move(s));
    }

    // ---- Performance ----
    {
        Setting s = choice("graphics_profile", "Graphics profile", kPerformance + 10, "graphics", "profile", {"Custom", "Quality", "Balanced", "Performance"},
                           {"custom", "quality", "balanced", "performance"});
        s.get_choice = [] {
            static const char* const names[] = {"custom", "quality", "balanced", "performance"};
            const std::string prof = graphics::current_profile();
            for (int i = 0; i < 4; ++i)
                if (prof == names[i]) return i;
            return -1;
        };
        s.set_choice = [](int i) {
            const char* names[] = {"custom", "quality", "balanced", "performance"};
            return ok_or_error(graphics::apply_profile(names[std::clamp(i, 0, 3)]));
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s = num_spec("render_scale", "Render scale", kPerformance + 20, "stereo", "render_scale", 0.3f, 1.0f, 0.05f, 2);
        s.part_of_profile = true;
        s.get_number = [&d] { return d.render_scale.load(); };
        s.set_number = [&d](float v) {
            d.render_scale = std::clamp(v, 0.3f, 1.0f);  // as `dynres scale`
            log::info("menu: render scale {:.2f}", d.render_scale.load());
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    live_settings::add(on_off("stereo.dynamic_resolution", "Dynamic resolution", kPerformance + 21, "stereo", "dynamic_resolution", d.dynres));
    live_settings::add(number("stereo.dynamic_resolution_min", "Dynamic resolution: lowest scale", kPerformance + 22, "stereo", "dynamic_resolution_min", 0.3f,
                              1.0f, 0.05f, 2, d.dynres_min));
    live_settings::add(number("stereo.dynamic_resolution_target", "Dynamic resolution: GPU target", kPerformance + 23, "stereo", "dynamic_resolution_target",
                              0.5f, 1.0f, 0.05f, 2, d.dynres_target));
    add_held(choice("stereo.late_update", "Late head update", kPerformance + 60, "stereo", "late_update", {"Off", "On"}, {"0", "1"}), cfg,
             [](const std::string& v) {
                 late_update::command({"stereo", "lateupdate", v == "1" ? "on" : "off"});
                 return std::string();
             });
    add_held(choice("dlss.enabled", "DLSS", kPerformance + 70, "dlss", "enabled", {"Off", "On"}, {"0", "1"}), cfg,
             [](const std::string& v) { return dispatch(std::string("dlss ") + (v == "1" ? "on" : "off")); });
    add_held(choice("dlss.preset", "DLSS model", kPerformance + 74, "dlss", "preset", {"Default", "K", "L", "M"}, {"default", "k", "l", "m"}), cfg,
             [](const std::string& v) { return dispatch("dlss preset " + v); });

    // ---- Controls ----
    live_settings::add(integer("first_person.toggle_key", "First/third person key", kControls + 10, "first_person", "toggle_key", 0, 254, 1, p.toggle_key, true));
    live_settings::add(on_off("first_person.pad_toggle", "View/Back + right stick switches person", kControls + 11, "first_person", "pad_toggle", p.pad_toggle));
    live_settings::add(integer("controls.recenter_key", "Recenter key", kControls + 20, "controls", "recenter_key", 0, 254, 1, c.recenter_key, true));
    live_settings::add(integer("controls.stereo_key", "3D on/off key", kControls + 21, "controls", "stereo_key", 0, 254, 1, c.stereo_key, true));
    live_settings::add(integer("controls.ui_nearer_key", "HUD nearer key", kControls + 22, "controls", "ui_nearer_key", 0, 254, 1, c.ui_nearer_key, true));
    live_settings::add(integer("controls.ui_farther_key", "HUD farther key", kControls + 23, "controls", "ui_farther_key", 0, 254, 1, c.ui_farther_key, true));
    live_settings::add(number("controls.ui_step", "HUD distance step (m)", kControls + 24, "controls", "ui_step", 0.05f, 2.0f, 0.05f, 2, c.ui_step));
    live_settings::add(number("controls.ui_min", "HUD nearest (m)", kControls + 25, "controls", "ui_min", 0.25f, 5.0f, 0.25f, 2, c.ui_min));
    live_settings::add(number("controls.ui_max", "HUD farthest (m)", kControls + 26, "controls", "ui_max", 1.0f, 20.0f, 0.5f, 2, c.ui_max));
    live_settings::add(on_off("controls.pad", "Gamepad View/Back combinations", kControls + 30, "controls", "pad", c.pad));
    live_settings::add(on_off("controls.pad_hold_view", "Hold View/Back back from the game", kControls + 31, "controls", "pad_hold_view", c.pad_hold_view));
    add_held(base("controls.fp_toggle_chord", "First/third person chord", kControls + 32, "controls", "fp_toggle_chord", Kind::Text), cfg,
             [](const std::string& v) { return ok_or_error(controls::command("chord " + (v.empty() ? std::string("off") : v))); });
    live_settings::add(integer("controls.fp_toggle_chord_ms", "Chord window (ms)", kControls + 33, "controls", "fp_toggle_chord_ms", 20, 1000, 10, c.fp_chord_ms));

    // ---- Advanced ----
    {
        Setting s = choice("stereo.ao_fix", "Ambient occlusion fix", kAdvanced + 10, "stereo", "ao_fix", {"Off", "On"}, {"0", "1"});
        s.get_choice = [] { return bloom_fix::ao_enabled() ? 1 : 0; };
        s.set_choice = [](int i) {
            bloom_fix::set_ao_enabled(i == 1);
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s = choice("stereo.light_fix", "Indoor light fix", kAdvanced + 11, "stereo", "light_fix", {"Off", "On"}, {"0", "1"});
        s.get_choice = [] { return fixes::light_fix_wanted() ? 1 : 0; };
        s.set_choice = [](int i) {
            fixes::set_light_fix(i == 1);
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s = choice("stereo.ssr_fix", "Reflections", kAdvanced + 12, "stereo", "ssr_fix",
                           {"Left eye only (0)", "Right moved (1)", "Off in both eyes (2)", "Both eyes (3)"}, {"0", "1", "2", "3"});
        s.get_choice = [] { return std::clamp(fixes::ssr_fix_mode(), -1, 3); };
        s.set_choice = [](int i) {
            fixes::set_ssr_fix(std::clamp(i, 0, 3));
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s = choice("stereo.distortion_fix", "Heat haze per eye", kAdvanced + 13, "stereo", "distortion_fix", {"Off", "On"}, {"0", "1"});
        s.get_choice = [] { return distortion_fix::enabled() ? 1 : 0; };
        s.set_choice = [](int i) {
            distortion_fix::set_enabled(i == 1);
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    add_held(choice("stereo.tonemap_shift", "Luma tonemap correction", kAdvanced + 14, "stereo", "tonemap_shift", {"Auto", "Always", "Off"},
                    {"auto", "1", "0"}),
             cfg, [](const std::string& v) {
                 bloom_fix::set_tonemap_shift(v == "0" ? 0 : v == "1" ? 1 : 2);
                 return std::string();
             });
    add_held(num_spec("controls.pad_log", "Log gamepad changes (count)", kAdvanced + 60, "controls", "pad_log", 0, 1000, 50, 0), cfg,
             [](const std::string& v) { return ok_or_error(controls::command("padlog " + v)); });

    // ---- a status line for the panel's Performance header ----
    live_settings::set_status("texstream", [] {
        if (!texstream::available()) return std::string();
        const std::string r = texstream::command("status");
        const size_t at = r.find("over budget");
        if (at == std::string::npos) return std::string();
        const size_t end = r.find(" MB", at);
        return end == std::string::npos ? std::string() : "textures " + r.substr(at, end + 3 - at);
    });
}

}  // namespace ff7vr::engine::menu_items
