#pragma once
// The `graphics` dev command: graphics status | graphics profile <quality|balanced|performance>
// applies a [graphics] profile live (docs/engine-module.md, "Graphics profiles").

namespace ff7vr::engine::graphics {
void register_command();
}
