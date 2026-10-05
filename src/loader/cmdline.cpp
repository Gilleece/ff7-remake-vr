#include "cmdline.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <format>
#include <vector>

namespace ff7vr::loader::cmdline {
namespace {

// Splits a command line into arguments the way the engine sees them: separated
// by spaces or tabs, double quotes group. The first token (the exe) is skipped.
std::vector<std::wstring> arguments(std::wstring_view s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false, have = false;
    for (wchar_t c : s) {
        if (c == L'"') { quoted = !quoted; have = true; continue; }
        if (!quoted && (c == L' ' || c == L'\t')) {
            if (have) out.push_back(cur);
            cur.clear();
            have = false;
            continue;
        }
        cur.push_back(c);
        have = true;
    }
    if (have) out.push_back(cur);
    if (!out.empty()) out.erase(out.begin());
    return out;
}

bool iequals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

// GetCommandLineW and GetCommandLineA in kernelbase.dll are
//     mov rax, qword ptr [rip + disp32]   ; 48 8B 05 disp32
//     ret                                 ; C3
// reading the Buffer field of a UNICODE_STRING / ANSI_STRING that kernelbase
// fills at process start. Returns the address of that pointer, or nullptr if
// the code has another form.
void** cached_pointer(const unsigned char* fn) {
    if (!fn) return nullptr;
    if (fn[0] != 0x48 || fn[1] != 0x8B || fn[2] != 0x05 || fn[7] != 0xC3) return nullptr;
    int32_t disp;
    std::memcpy(&disp, fn + 3, sizeof disp);
    return reinterpret_cast<void**>(const_cast<unsigned char*>(fn) + 7 + disp);
}

bool writable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

// The Length/MaximumLength pair in front of the Buffer field of a
// UNICODE_STRING or ANSI_STRING (x64 layout: two USHORTs, padding, pointer).
struct Lengths {
    USHORT* length;
    USHORT* maximum;
};

Lengths string_lengths(void** buffer_field, size_t expected_bytes) {
    auto* base = reinterpret_cast<unsigned char*>(buffer_field) - 8;
    auto* len = reinterpret_cast<USHORT*>(base);
    auto* max = reinterpret_cast<USHORT*>(base + 2);
    if (!writable(base)) return {};
    if (*len != expected_bytes || *max < *len) return {};
    return {len, max};
}

}  // namespace

D3DChoice find_d3d_choice(std::wstring_view command_line, std::wstring* option) {
    static constexpr std::wstring_view kD3D11[] = {L"d3d11", L"dx11"};
    static constexpr std::wstring_view kOther[] = {L"d3d12", L"dx12", L"vulkan", L"opengl", L"d3d10", L"dx10"};
    for (const auto& a : arguments(command_line)) {
        if (a.size() < 2 || (a[0] != L'-' && a[0] != L'/')) continue;
        std::wstring_view name(a);
        name.remove_prefix(1);
        for (auto n : kD3D11)
            if (iequals(name, n)) {
                if (option) *option = a;
                return D3DChoice::D3D11;
            }
        for (auto n : kOther)
            if (iequals(name, n)) {
                if (option) *option = a;
                return D3DChoice::Other;
            }
    }
    return D3DChoice::None;
}

bool append_argument(std::wstring_view argument, std::string* detail) {
    auto say = [&](std::string s) {
        if (detail) *detail = std::move(s);
    };

    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) { say("kernelbase.dll is not loaded"); return false; }
    auto* fnW = reinterpret_cast<const unsigned char*>(GetProcAddress(kb, "GetCommandLineW"));
    auto* fnA = reinterpret_cast<const unsigned char*>(GetProcAddress(kb, "GetCommandLineA"));
    void** slotW = cached_pointer(fnW);
    void** slotA = cached_pointer(fnA);
    const wchar_t* oldW = GetCommandLineW();
    const char* oldA = GetCommandLineA();
    if (!slotW || !slotA) { say("GetCommandLineW/A in kernelbase.dll do not have the expected code"); return false; }
    if (*slotW != oldW || *slotA != oldA) { say("GetCommandLineW/A in kernelbase.dll do not read the expected strings"); return false; }
    if (!writable(slotW) || !writable(slotA)) { say("the command-line strings of kernelbase.dll are not writable"); return false; }

    std::wstring w(oldW);
    if (std::count(w.begin(), w.end(), L'"') % 2 != 0) { say("the command line ends inside quotes"); return false; }
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\t')) w.pop_back();
    w.push_back(L' ');
    w.append(argument);
    if (w.size() * sizeof(wchar_t) >= 0xFFFF) { say("the command line would be too long"); return false; }

    int na = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (na <= 0) { say("the command line cannot be converted to the ANSI code page"); return false; }

    // Never freed: the process keeps using these strings until it exits.
    HANDLE heap = GetProcessHeap();
    auto* newW = static_cast<wchar_t*>(HeapAlloc(heap, 0, (w.size() + 1) * sizeof(wchar_t)));
    auto* newA = static_cast<char*>(HeapAlloc(heap, 0, static_cast<size_t>(na)));
    if (!newW || !newA) { say("out of memory"); return false; }
    std::memcpy(newW, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, newA, na, nullptr, nullptr);

    const USHORT bytesW = static_cast<USHORT>(w.size() * sizeof(wchar_t));
    const USHORT bytesA = static_cast<USHORT>(na - 1);
    Lengths lenW = string_lengths(slotW, std::wcslen(oldW) * sizeof(wchar_t));
    Lengths lenA = string_lengths(slotA, std::strlen(oldA));

    // The pointer writes are single aligned 8-byte stores; readers see the old or the new string.
    if (lenW.length) { *lenW.maximum = static_cast<USHORT>(bytesW + sizeof(wchar_t)); *lenW.length = bytesW; }
    *slotW = newW;
    if (lenA.length) { *lenA.maximum = static_cast<USHORT>(bytesA + 1); *lenA.length = bytesA; }
    *slotA = newA;

    // The process parameters in the PEB, for code that reads the command line from there.
    bool peb = false;
    auto* teb = NtCurrentTeb();
    auto* pp = teb && teb->ProcessEnvironmentBlock ? teb->ProcessEnvironmentBlock->ProcessParameters : nullptr;
    if (pp && writable(&pp->CommandLine)) {
        pp->CommandLine.Buffer = newW;
        pp->CommandLine.Length = bytesW;
        pp->CommandLine.MaximumLength = static_cast<USHORT>(bytesW + sizeof(wchar_t));
        peb = true;
    }

    const bool ok = GetCommandLineW() == newW && GetCommandLineA() == newA;
    say(std::format("{} (lengths {}/{}, process parameters {})", ok ? "done" : "the new command line did not take",
                    lenW.length ? "W" : "-", lenA.length ? "A" : "-", peb ? "updated" : "not updated"));
    return ok;
}

}  // namespace ff7vr::loader::cmdline
