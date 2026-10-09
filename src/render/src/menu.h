// The in-headset settings panel ([menu] in ff7vr.ini; docs/render.md, "Settings panel").
//
// A list of the settings the modules registered (ff7vr/core/live_settings.h), drawn by the
// mod into its own texture and shown on a quad layer of its own in front of the HUD panel.
// Opened with [menu] key (Delete) or View/Back + Y on the gamepad. While it is open the
// game receives a neutral gamepad (MenuFilterPad in render.h) and the panel reads the real
// one: D-pad (or left stick) up/down choose, left/right change, A selects, X saves to
// ff7vr.ini, B closes. Keyboard: arrows, Enter, S, Escape (the game sees those keys too).
//
// Threads: a thread of its own polls the keyboard and the gamepad state recorded by the
// pad filter, applies changes through the settings' setters and draws the panel image on
// the CPU when something changed; the presenting thread only uploads a new image (Frame)
// and submits the layer. Nothing is drawn, uploaded or submitted while the panel is closed.
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ff7vr {
class Config;
}

namespace ff7vr::render::menu {

// [menu] keys; registers the render module's settings and the `menu` dev command, starts
// the panel's thread. `iniPath`: the ff7vr.ini that Save writes (next to the DLL).
void Start(const Config& cfg, const std::filesystem::path& iniPath);

// Any thread.
bool IsOpen();

struct PanelFrame {
    ID3D11Texture2D* texture = nullptr;  // new content to copy into the layer; null = unchanged
    uint32_t width = 0, height = 0;      // image size in pixels
    float widthM = 0.8f, heightM = 0.6f;  // size in metres
    float distance = 1.2f;               // metres in front of the head
    uint64_t placement = 0;              // changes when the panel must be placed again (opened, recentered)
};
// Presenting thread, while a frame is submitted: false while the panel is closed. Uploads
// the newest image into the panel texture when it changed (or `forceContent`: a new layer).
bool Frame(ID3D11Device* device, ID3D11DeviceContext* ctx, bool forceContent, PanelFrame* out);

// `menu ...` dev command (any thread).
std::string Command(const std::string& args);

}  // namespace ff7vr::render::menu
