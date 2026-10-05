#include "ff7vr/core/module.h"

#include <windows.h>
#include <psapi.h>

#include <format>

#pragma comment(lib, "version.lib")

namespace ff7vr::module {
namespace {

std::filesystem::path module_path(HMODULE m) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(m, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

Info info_of(HMODULE m) {
    Info i;
    if (!m) return i;
    i.base = reinterpret_cast<std::uintptr_t>(m);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(i.base + dos->e_lfanew);
    i.size = nt->OptionalHeader.SizeOfImage;
    i.path = module_path(m);
    return i;
}

}  // namespace

Info main_module() { return info_of(GetModuleHandleW(nullptr)); }

Info self() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &m);
    return info_of(m);
}

std::optional<Info> find(std::wstring_view name) {
    HMODULE m = GetModuleHandleW(std::wstring(name).c_str());
    if (!m) return std::nullopt;
    return info_of(m);
}

std::optional<Info> from_address(std::uintptr_t addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &m) ||
        !m)
        return std::nullopt;
    return info_of(m);
}

std::vector<Section> sections(std::uintptr_t base) {
    std::vector<Section> out;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        Section s;
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        s.name = name;
        s.start = base + sec->VirtualAddress;
        s.size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        s.characteristics = sec->Characteristics;
        out.push_back(std::move(s));
    }
    return out;
}

std::string describe(std::uintptr_t addr) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &m) &&
        m) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        char narrow_name[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow_name, MAX_PATH, nullptr, nullptr);
        return std::format("{}+0x{:x}", narrow_name, addr - reinterpret_cast<std::uintptr_t>(m));
    }
    return std::format("0x{:x}(no module)", addr);
}

std::string file_version(const std::filesystem::path& file) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0) return {};
    std::string data(size, '\0');
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return {};
    return std::format("{}.{}.{}.{}", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                       HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
}

std::uint32_t timestamp(std::uintptr_t base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

}  // namespace ff7vr::module
