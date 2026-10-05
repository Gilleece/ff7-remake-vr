// D3D11 / DXGI hooks: finds the game's device and swap chain from inside the
// process, tracks them and records how the game uses D3D11.
//
// Hooks (installed at start-up, before the engine creates its device):
//   * IDXGISwapChain::Present, IDXGISwapChain1::Present1, ResizeBuffers,
//     IDXGISwapChain3::ResizeBuffers1: vtable slots of DXGI's swap chain class.
//     The vtable is taken from a throwaway swap chain; every swap chain the
//     game creates later is checked and its vtable hooked too if it differs.
//     A vtable slot hook chains with inline hooks other software places on
//     the same functions (Steam overlay, ReShade, a frame timer): whoever
//     calls through the vtable reaches us first, and we call the function
//     that was in the slot.
//   * IDXGIFactory::CreateSwapChain, IDXGIFactory2::CreateSwapChainForHwnd,
//     D3D11CreateDevice, D3D11CreateDeviceAndSwapChain,
//     ID3D11Device::CreateDeferredContext: inline hooks, for facts only
//     (which thread creates what, with which flags) and to catch new swap
//     chain classes.
//   * A vectored exception handler that records thread names the engine
//     announces with the 0x406D1388 debugger exception.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <string>

namespace ff7vr::render {

// Everything the Present callback gets. Pointers are valid for the duration of the call only.
struct PresentInfo {
    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;  // immediate context; the presenting thread owns it during the call
    ID3D11Texture2D* backBuffer = nullptr;   // the image about to be presented (buffer 0)
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    HWND window = nullptr;
    UINT syncInterval = 0, flags = 0;
    int64_t entryQpc = 0;  // QueryPerformanceCounter at entry to our Present detour
};

struct HookCallbacks {
    // Main swap chain only, on the presenting thread, before the real Present.
    void (*onPresent)(const PresentInfo& info) = nullptr;
    // Any swap chain, before the real ResizeBuffers(1). Release every reference to its buffers here.
    void (*onResize)(IDXGISwapChain* swapchain) = nullptr;
};

// Installs everything. Call once, early. Returns false if the Present hook could not be installed.
bool InstallD3D11Hooks(const HookCallbacks& callbacks);

// One-line facts about the device, swap chain and threads (for the status command).
std::string D3D11Summary();
// The process's threads by CPU time, marking the one that presents (for the status command).
std::string ThreadReport();

// Name of a thread: GetThreadDescription, else the name announced through the debugger exception, else "".
std::string ThreadName(DWORD tid);

int64_t QpcNow();
double QpcToMs(int64_t ticks);

}  // namespace ff7vr::render
