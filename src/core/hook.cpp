#include "ff7vr/core/hook.h"

#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"

#include <windows.h>

#include <MinHook.h>

#include <mutex>
#include <utility>

namespace ff7vr::hook {
namespace {
std::mutex g_mh_mutex;
bool g_mh_ready = false;
}  // namespace

bool init() {
    std::lock_guard lock(g_mh_mutex);
    if (g_mh_ready) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        log::error("hook: MH_Initialize failed: {}", MH_StatusToString(s));
        return false;
    }
    g_mh_ready = true;
    return true;
}

void shutdown() {
    std::lock_guard lock(g_mh_mutex);
    if (!g_mh_ready) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_mh_ready = false;
}

// ---------------------------------------------------------------- InlineHook

InlineHook::~InlineHook() { remove(); }

InlineHook::InlineHook(InlineHook&& o) noexcept
    : target_(std::exchange(o.target_, nullptr)),
      trampoline_(std::exchange(o.trampoline_, nullptr)),
      enabled_(std::exchange(o.enabled_, false)) {}

InlineHook& InlineHook::operator=(InlineHook&& o) noexcept {
    if (this != &o) {
        remove();
        target_ = std::exchange(o.target_, nullptr);
        trampoline_ = std::exchange(o.trampoline_, nullptr);
        enabled_ = std::exchange(o.enabled_, false);
    }
    return *this;
}

bool InlineHook::create(void* target, void* detour, bool enable_now) {
    if (!init()) return false;
    if (installed()) {
        log::error("hook: InlineHook already installed at {}", module::describe(reinterpret_cast<std::uintptr_t>(target_)));
        return false;
    }
    std::lock_guard lock(g_mh_mutex);
    MH_STATUS s = MH_CreateHook(target, detour, &trampoline_);
    if (s != MH_OK) {
        log::error("hook: MH_CreateHook({}) failed: {}", module::describe(reinterpret_cast<std::uintptr_t>(target)),
                   MH_StatusToString(s));
        trampoline_ = nullptr;
        return false;
    }
    target_ = target;
    log::debug("hook: created inline hook at {}", module::describe(reinterpret_cast<std::uintptr_t>(target)));
    if (enable_now) {
        s = MH_EnableHook(target_);
        if (s != MH_OK) {
            log::error("hook: MH_EnableHook failed: {}", MH_StatusToString(s));
            return false;
        }
        enabled_ = true;
    }
    return true;
}

bool InlineHook::enable() {
    if (!installed()) return false;
    if (enabled_) return true;
    std::lock_guard lock(g_mh_mutex);
    MH_STATUS s = MH_EnableHook(target_);
    enabled_ = s == MH_OK;
    if (!enabled_) log::error("hook: MH_EnableHook failed: {}", MH_StatusToString(s));
    return enabled_;
}

bool InlineHook::disable() {
    if (!installed()) return false;
    if (!enabled_) return true;
    std::lock_guard lock(g_mh_mutex);
    MH_STATUS s = MH_DisableHook(target_);
    if (s == MH_OK) enabled_ = false;
    return s == MH_OK;
}

void InlineHook::remove() {
    if (!installed()) return;
    std::lock_guard lock(g_mh_mutex);
    if (g_mh_ready) MH_RemoveHook(target_);
    target_ = nullptr;
    trampoline_ = nullptr;
    enabled_ = false;
}

// ---------------------------------------------------------------- VTableHook

bool write_memory(void* dst, const void* src, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, size);
    DWORD tmp = 0;
    VirtualProtect(dst, size, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    return true;
}

VTableHook::~VTableHook() { remove(); }

bool VTableHook::create(void** vtable, std::size_t index, void* detour) {
    if (installed()) {
        log::error("hook: VTableHook already installed");
        return false;
    }
    if (!vtable) return false;
    void** slot = vtable + index;
    void* original = *slot;
    // Pointer-sized aligned store is atomic on x64; other threads see either value.
    if (!write_memory(slot, &detour, sizeof(void*))) {
        log::error("hook: VirtualProtect failed on vtable slot {} at {}", index,
                   module::describe(reinterpret_cast<std::uintptr_t>(slot)));
        return false;
    }
    slot_ = slot;
    original_ = original;
    detour_ = detour;
    log::debug("hook: vtable slot {} at {} -> detour (original {})", index,
               module::describe(reinterpret_cast<std::uintptr_t>(slot)),
               module::describe(reinterpret_cast<std::uintptr_t>(original)));
    return true;
}

void VTableHook::remove() {
    if (!installed()) return;
    if (*slot_ == detour_) write_memory(slot_, &original_, sizeof(void*));
    slot_ = nullptr;
    original_ = nullptr;
    detour_ = nullptr;
}

}  // namespace ff7vr::hook
