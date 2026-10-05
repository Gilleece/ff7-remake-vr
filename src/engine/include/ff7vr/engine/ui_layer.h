#pragma once
// The game's in-game UI on its own layer in stereo.
//
// This build draws its UMG UI (HUD, command menu, menus, dialogue, markers) inside the
// scene renderer, once per view, into the pooled render target "InGameUIRenderTarget",
// and every view's post-processing composites that texture over the view. In stereo
// that puts the UI into each eye at zero parallax, cropped to the eye's aspect ratio.
//
// With the render module's UI layer active (stereo mode, an XR session, [ui] layer = 1)
// the hooks installed here:
//   * let the UI pass run for the first eye only (one UI render per frame),
//   * clear the view family's in-game UI flag after it, so post-processing binds the
//     engine's empty fallback texture instead of the UI (no UI in the eye images),
//   * hand the UI texture to the render module from the presenting thread, right after
//     the UI pass executed and before the frame's Present, which shows it on a quad
//     layer (render.h, "UI LAYER").
// Mono frames, screen mode and a game without our stereo device are left untouched.
// See docs/engine-module.md ("UI layer") and docs/re/engine.md ("In-game UI").

#include "ff7vr/core/startup_context.h"

namespace ff7vr::engine {

// Called once by the loader, after engine::start. Resolves the UI functions by signature
// and installs two inline hooks when [stereo] enabled = 1. Never throws; returns false
// (and logs why) when the UI stays in the eye images.
bool start_ui_layer(const StartupContext& ctx);

}  // namespace ff7vr::engine
