#pragma once
// Shared between the engine module's source files.

#include "addresses.h"

namespace ff7vr::engine {

const Addresses& addresses();
bool on_game_thread();

}  // namespace ff7vr::engine
