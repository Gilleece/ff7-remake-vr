#include "menu_items.h"

#include "controls.h"
#include "graphics.h"
#include "player.h"
#include "snap_turn.h"
#include "stereo_device.h"

#include "ff7vr/core/live_settings.h"
#include "ff7vr/core/log.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace ff7vr::engine::menu_items {
namespace {

using live_settings::Kind;
using live_settings::Setting;

Setting on_off(const char* id, const char* label, int order, const char* section, const char* key, std::atomic<bool>& value) {
    Setting s;
    s.id = id;
    s.label = label;
    s.order = order;
    s.kind = Kind::Choice;
    s.section = section;
    s.key = key;
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

}  // namespace

void register_all() {
    {
        Setting s;
        s.id = "view";
        s.label = "View";
        s.order = 10;
        s.kind = Kind::Choice;
        s.section = "first_person";
        s.key = "default";
        s.choices = {"Third person", "First person"};
        s.ini_values = {"0", "1"};
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
    {
        Setting s;
        s.id = "snap_turn";
        s.label = "Snap turn";
        s.order = 20;
        s.kind = Kind::Choice;
        s.section = "comfort";
        s.key = "snap_turn";
        s.choices = {"Off", "30 degrees", "45 degrees"};
        s.ini_values = {"0", "30", "45"};
        s.get_choice = [] {
            const float d = snap_turn::degrees();
            return d == 0.0f ? 0 : std::fabs(d - 30.0f) < 0.01f ? 1 : std::fabs(d - 45.0f) < 0.01f ? 2 : -1;
        };
        s.set_choice = [](int i) {
            const char* v[] = {"off", "30", "45"};
            const std::string r = snap_turn::command(std::string("snap ") + v[std::clamp(i, 0, 2)]);
            return r.starts_with("ok") ? std::string() : r;
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s;
        s.id = "render_scale";
        s.label = "Render scale";
        s.order = 60;
        s.kind = Kind::Number;
        s.section = "stereo";
        s.key = "render_scale";
        s.min = 0.5f;
        s.max = 1.0f;
        s.step = 0.05f;
        s.decimals = 2;
        s.part_of_profile = true;
        s.get_number = [] { return device::settings().render_scale.load(); };
        s.set_number = [](float v) {
            device::settings().render_scale = std::clamp(v, 0.3f, 1.0f);  // as `dynres scale`
            log::info("menu: render scale {:.2f}", device::settings().render_scale.load());
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s;
        s.id = "graphics_profile";
        s.label = "Graphics profile";
        s.order = 70;
        s.kind = Kind::Choice;
        s.section = "graphics";
        s.key = "profile";
        s.choices = {"Custom", "Quality", "Balanced", "Performance"};
        s.ini_values = {"custom", "quality", "balanced", "performance"};
        s.get_choice = [] {
            static const char* const names[] = {"custom", "quality", "balanced", "performance"};
            const std::string p = graphics::current_profile();
            for (int i = 0; i < 4; ++i)
                if (p == names[i]) return i;
            return -1;
        };
        s.set_choice = [](int i) {
            const char* names[] = {"custom", "quality", "balanced", "performance"};
            const std::string r = graphics::apply_profile(names[std::clamp(i, 0, 3)]);
            return r.starts_with("ok") ? std::string() : r;
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s;
        s.id = "world_scale";
        s.label = "World scale";
        s.order = 120;
        s.kind = Kind::Number;
        s.section = "stereo";
        s.key = "world_scale";
        s.min = 0.5f;
        s.max = 2.0f;
        s.step = 0.05f;
        s.decimals = 2;
        s.get_number = [] { return device::settings().world_scale.load(); };
        s.set_number = [](float v) {
            device::settings().world_scale = std::clamp(v, 0.1f, 10.0f);  // as `stereo scale`
            log::info("menu: world scale {:.2f}", device::settings().world_scale.load());
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    live_settings::add(on_off("head_bob", "Head bob", 130, "first_person", "head_bob", player::settings().head_bob));
    live_settings::add(on_off("decoupled_pitch", "Level horizon (decoupled pitch)", 140, "stereo", "decoupled_pitch", device::settings().decouple_pitch));
    {
        Setting s;
        s.id = "stereo";
        s.label = "3D";
        s.order = 160;
        s.kind = Kind::Choice;  // live only: [stereo] enabled is the start-up switch, not saved from here
        s.choices = {"Off (virtual screen)", "On"};
        s.get_choice = [] { return device::wanted() ? 1 : 0; };
        s.set_choice = [](int i) {
            // The player's own key path: it also reconnects the headset when the session is gone.
            if ((i == 1) != device::wanted()) controls::command("stereo");
            return std::string();
        };
        live_settings::add(std::move(s));
    }
}

}  // namespace ff7vr::engine::menu_items
