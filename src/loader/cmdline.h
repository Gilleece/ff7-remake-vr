#pragma once

// Adding an argument to the process command line before the game reads it.
//
// The game imports xinput1_3.dll statically, so this DLL's DllMain runs while
// the exe's imports are resolved: before the exe's own C runtime start-up,
// static constructors and WinMain, and therefore before the engine copies the
// command line and chooses its graphics API. Code that runs from DllMain can
// still change what GetCommandLineW/GetCommandLineA return to the engine.

#include <string>
#include <string_view>

namespace ff7vr::loader::cmdline {

enum class D3DChoice {
    None,      // no graphics API option on the command line
    D3D11,     // -d3d11 or -dx11 is already there
    Other,     // the player chose another API (-d3d12, -dx12, -vulkan, -opengl, ...)
};

// What the command line already asks for. `other` receives the option found
// for D3DChoice::Other / D3DChoice::D3D11.
D3DChoice find_d3d_choice(std::wstring_view command_line, std::wstring* option = nullptr);

// Appends " <argument>" to the command line seen by GetCommandLineW,
// GetCommandLineA and the process parameters in the PEB. Safe to call from
// DllMain (no library loads, no locks other than the heap's). Changes nothing
// unless the code of GetCommandLineW/A in kernelbase.dll has the expected form
// and points at the strings they currently return. Returns true when the new
// command line is in place; `detail` receives a one-line description either way.
bool append_argument(std::wstring_view argument, std::string* detail);

}  // namespace ff7vr::loader::cmdline
