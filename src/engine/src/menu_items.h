#pragma once

namespace ff7vr {
class Config;
}

namespace ff7vr::engine::menu_items {

// Registers the engine's settings for the in-headset panel (live_settings.h). `cfg`: the
// values read at start, for settings whose module keeps no getter.
void register_all(const Config& cfg);

}  // namespace ff7vr::engine::menu_items
