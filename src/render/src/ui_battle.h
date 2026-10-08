// HUD panel size in battles: while a battle is in progress the UI layer's height blends to
// [ui] battle_size, where the markers over enemies line up with the world, and back
// afterwards (docs/render.md, "Size in battles").
#pragma once

#include <string>

namespace ff7vr {
class Config;
}

namespace ff7vr::render::ui_battle {

// [ui] battle_size (metres, 0 = the panel keeps [ui] size in battles).
void Start(const Config& cfg);

// Presenting thread, once per frame that shows the UI layer: the panel height for `base`
// ([ui] size) with the battle blend applied.
float Height(float base);

// `ui battle_size <m>`; false when the value is not a number.
bool SetSize(const std::string& value);
// One status fragment: battle size, battle flag, blend.
std::string Text();

}  // namespace ff7vr::render::ui_battle
