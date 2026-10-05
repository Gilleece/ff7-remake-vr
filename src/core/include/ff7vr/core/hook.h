#pragma once
// Hooking: inline hooks (MinHook) and vtable slot hooks.
//
// Inline hook (patches the function's first instructions, affects every caller):
//
//   static ff7vr::hook::InlineHook g_tick;
//   void __fastcall tick_detour(void* self, float dt) {
//       g_tick.original<void(__fastcall*)(void*, float)>()(self, dt);
//   }
//   g_tick.create(target_address, &tick_detour);   // installs and enables
//
// VTable hook (replaces one slot of one vtable; affects every object that
// shares that vtable, nothing else):
//
//   static ff7vr::hook::VTableHook g_present;
//   g_present.create(*(void***)swapchain, 8, &present_detour);
//   auto orig = g_present.original<HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT)>();
//
// Both are RAII: destruction removes the hook. Hooks are created/removed while
// other threads may run; MinHook suspends other threads while patching. Do not
// destroy a hook while a detour may still be executing on another thread
// (prefer disable() and leaving it alive until process exit).

#include <cstddef>
#include <cstdint>
#include <string>

namespace ff7vr::hook {

// Initialise MinHook. Called by the loader once; safe to call repeatedly.
bool init();
// Disable and remove every MinHook hook (process shutdown only).
void shutdown();

class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();
    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;
    InlineHook(InlineHook&& other) noexcept;
    InlineHook& operator=(InlineHook&& other) noexcept;

    // Creates and (by default) enables the hook. Returns false and logs on failure.
    bool create(void* target, void* detour, bool enable_now = true);
    template <class T, class D>
    bool create(T target, D detour, bool enable_now = true) {
        return create(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour), enable_now);
    }
    bool enable();
    bool disable();
    void remove();

    bool installed() const { return target_ != nullptr; }
    bool enabled() const { return enabled_; }
    void* target() const { return target_; }

    // Trampoline to the original function.
    template <class Fn> Fn original() const { return reinterpret_cast<Fn>(trampoline_); }

private:
    void* target_ = nullptr;
    void* trampoline_ = nullptr;
    bool enabled_ = false;
};

class VTableHook {
public:
    VTableHook() = default;
    ~VTableHook();
    VTableHook(const VTableHook&) = delete;
    VTableHook& operator=(const VTableHook&) = delete;

    // vtable: pointer to the first slot (i.e. *(void***)object).
    bool create(void** vtable, std::size_t index, void* detour);
    template <class D>
    bool create(void** vtable, std::size_t index, D detour) {
        return create(vtable, index, reinterpret_cast<void*>(detour));
    }
    // Restores the original slot value (only if the slot still holds our detour).
    void remove();

    bool installed() const { return slot_ != nullptr; }
    template <class Fn> Fn original() const { return reinterpret_cast<Fn>(original_); }

private:
    void** slot_ = nullptr;
    void* original_ = nullptr;
    void* detour_ = nullptr;
};

// Write bytes into protected memory (code or rdata), restoring protection.
bool write_memory(void* dst, const void* src, std::size_t size);

}  // namespace ff7vr::hook
