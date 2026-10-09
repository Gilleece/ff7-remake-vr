#pragma once
// The engine's settings in the in-headset settings panel (ff7vr/core/live_settings.h):
// first/third person, snap turn, render scale, graphics profile, world scale, head bob,
// decoupled pitch and 3D on/off. Each one changes the same state as the matching dev
// command.

namespace ff7vr::engine::menu_items {

void register_all();

}  // namespace ff7vr::engine::menu_items
