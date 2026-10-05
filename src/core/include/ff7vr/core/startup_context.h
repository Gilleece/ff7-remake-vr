#pragma once
// What the loader hands to each module it starts (see src/loader/startup.cpp,
// the plug-in point). Lives in ff7vr_core so modules can include it without
// depending on the loader.

#include "ff7vr/core/config.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace ff7vr {

struct StartupContext {
    const Config* config = nullptr;        // ff7vr.ini (defaults if missing)
    std::filesystem::path dll_dir;         // End\Binaries\Win64 in the game
    std::uintptr_t game_base = 0;          // ff7remake_.exe image base
    std::size_t game_size = 0;             // SizeOfImage
    bool is_game = false;                  // host exe is ff7remake_.exe
};

}  // namespace ff7vr
