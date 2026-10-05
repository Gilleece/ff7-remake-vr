#pragma once
// Helpers for loaded modules: base/size, paths, versions, address naming.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ff7vr::module {

struct Info {
    std::uintptr_t base = 0;
    std::size_t size = 0;            // SizeOfImage
    std::filesystem::path path;
    std::uintptr_t end() const { return base + size; }
    bool contains(std::uintptr_t a) const { return a >= base && a < end(); }
};

struct Section {
    std::string name;                // e.g. ".text"
    std::uintptr_t start = 0;
    std::size_t size = 0;            // VirtualSize
    std::uint32_t characteristics = 0;
    bool executable() const { return (characteristics & 0x20000000) != 0; }  // IMAGE_SCN_MEM_EXECUTE
    bool readable() const { return (characteristics & 0x40000000) != 0; }    // IMAGE_SCN_MEM_READ
};

// The process's main executable (ff7remake_.exe in the game).
Info main_module();
// The module containing this code (our DLL).
Info self();
// Any loaded module by name (e.g. L"d3d11.dll"); nullopt if not loaded.
std::optional<Info> find(std::wstring_view name);
// Module containing an address, if any.
std::optional<Info> from_address(std::uintptr_t addr);

std::vector<Section> sections(std::uintptr_t module_base);

// "ff7remake_.exe+0x1a2b3c", or "0x7ff6...(unknown)" if not inside a module.
// Does not allocate from the process heap besides the returned string.
std::string describe(std::uintptr_t addr);

// File version resource ("1.0.0.2") of a file on disk; empty if none.
std::string file_version(const std::filesystem::path& file);
// PE TimeDateStamp of a loaded module (identifies the exact build).
std::uint32_t timestamp(std::uintptr_t module_base);

}  // namespace ff7vr::module
