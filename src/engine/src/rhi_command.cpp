#include "rhi_command.h"

#include "ff7vr/core/log.h"
#include "ff7vr/core/pattern.h"

#include <windows.h>

#include <atomic>

namespace ff7vr::engine::rhi {
namespace {
constexpr std::size_t kCommandLink = 0x08;
constexpr std::size_t kExecuting = 0x10;
constexpr std::size_t kNumCommands = 0x14;

std::atomic<bool> g_layout_ok{false};

// The engine appends a recorded command like this (FFakeStereoRendering::RenderTexture_RenderThread
// and every other RHI wrapper; rdi = command list, rcx = new command):
//   mov rax,[rdi+8]; inc dword [rdi+14h]; mov [rax],rcx; lea rax,[ExecuteAndDestruct]; mov [rdi+8],rcx;
//   mov [rcx+8],rax; mov [rcx],r14(=0)
constexpr const char* kAppendSequence = "48 8B 47 08 FF 47 14 48 89 08 48 8D 05 ?? ?? ?? ?? 48 89 4F 08 48 89 41 08 4C 89 31";

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<std::uintptr_t>(p) + n <= end;
}
}  // namespace

bool verify_layout(std::uintptr_t module_base) {
    auto r = pattern::scan_module(module_base, kAppendSequence, pattern::Sections::Executable, 1);
    g_layout_ok = r.found();
    if (!r.found()) log::warn("engine: RHI command list layout not confirmed (append sequence not found); mirror disabled");
    return r.found();
}

bool enqueue(void* cmd_list, Command* cmd) {
    if (!g_layout_ok || !cmd_list || !cmd || !cmd->execute) return false;
    auto* base = static_cast<std::uint8_t*>(cmd_list);
    if (!readable(base, 0x30)) return false;
    if (base[kExecuting] != 0) return false;  // being executed right now: not ours to extend
    auto** link_field = reinterpret_cast<Command***>(base + kCommandLink);
    Command** link = *link_field;
    // CommandLink points at the Root field of an empty list or at the Next field of the last
    // command, which must be null.
    if (!link || !readable(link, sizeof(void*)) || *link != nullptr) return false;
    cmd->next = nullptr;
    *link = cmd;
    *link_field = &cmd->next;
    ++*reinterpret_cast<std::uint32_t*>(base + kNumCommands);
    return true;
}

}  // namespace ff7vr::engine::rhi
