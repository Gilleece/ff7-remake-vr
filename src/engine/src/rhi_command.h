#pragma once
// Appending our own command to the engine's RHI command list.
//
// In this shipping build the immediate RHI command list never bypasses: every RHI call made
// on the render thread is recorded as a command and executed later, in order, on the RHI
// thread (this game runs D3D11 with one), the frame's last commands inside
// RHIEndDrawingViewport, which presents. A command we append therefore runs on the thread
// that owns the D3D11 immediate context, after all the engine's earlier work for the frame
// has been submitted to it, which makes it the right place for raw D3D11 work that reads
// the frame's render targets.
//
// Layout (UE 4.18, confirmed from the engine's own command recording code):
//   FRHICommandBase      { FRHICommandBase* Next; void (*ExecuteAndDestruct)(FRHICommandListBase&, FRHICommandBase*); }
//   FRHICommandListBase  { FRHICommandBase* Root; FRHICommandBase** CommandLink; bool bExecuting;
//                          uint32 NumCommands (+0x14); ...; IRHICommandContext* Context (+0x20); ... }
// Appending: *CommandLink = cmd; CommandLink = &cmd->Next; ++NumCommands.
//
// The command's memory is ours (the list only links it), so it must stay valid until it
// has executed: callers use a small ring of static commands.

#include <cstdint>

namespace ff7vr::engine::rhi {

struct Command {
    Command* next = nullptr;
    void (*execute)(void* cmd_list, Command* self) = nullptr;
};
static_assert(sizeof(Command) == 16);

// Checks the engine's command recording code against the layout above (signature over
// the engine's own append sequence). Call once at start-up; enqueue() refuses without it.
bool verify_layout(std::uintptr_t module_base);

// Render thread only, with the immediate command list the engine passed us. Returns false
// (and does nothing) if the list does not look like a recording list.
bool enqueue(void* cmd_list, Command* cmd);

}  // namespace ff7vr::engine::rhi
