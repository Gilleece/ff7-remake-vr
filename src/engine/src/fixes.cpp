#include "fixes.h"

#include "engine_internal.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

namespace ff7vr::engine::fixes {
namespace {

// Byte patches with a known original and patched value.
std::optional<bool> read_patch(const std::uint8_t* p, std::uint8_t off, std::uint8_t on) {
    if (!p) return std::nullopt;
    if (*p == off) return false;
    if (*p == on) return true;
    return std::nullopt;
}

bool write_patch(std::uint8_t* p, std::uint8_t off, std::uint8_t on, bool enable, const char* what) {
    const auto cur = read_patch(p, off, on);
    if (!cur) {
        log::warn("fixes: {} patch site not available", what);
        return false;
    }
    if (*cur == enable) return true;
    const std::uint8_t v = enable ? on : off;
    if (!hook::write_memory(p, &v, 1)) {
        log::warn("fixes: writing the {} patch failed", what);
        return false;
    }
    log::info("fixes: {} patch {}", what, enable ? "on" : "off (game default)");
    return true;
}

bool g_sysres_active = false;
std::int32_t g_sysres_game[2]{};     // the game's value before the first override
std::int32_t g_sysres_written[2]{};  // what we wrote last

}  // namespace

std::optional<bool> light_patch() { return read_patch(addresses().LightSortKeyImm, 0x40, 0x60); }
bool set_light_patch(bool on) { return write_patch(addresses().LightSortKeyImm, 0x40, 0x60, on, "light sort-key"); }

// 0x75 = jne rel8, 0xEB = jmp rel8 (same displacement).
std::optional<bool> view_rect_patch() { return read_patch(addresses().ViewRectOverrideJump, 0x75, 0xEB); }
bool set_view_rect_patch(bool on) {
    return write_patch(addresses().ViewRectOverrideJump, 0x75, 0xEB, on, "windowed-fullscreen view rect");
}

void apply_system_resolution(std::int32_t width, std::int32_t height) {
    std::int32_t* r = addresses().GSystemResolution;
    if (!r || width <= 0 || height <= 0) return;
    if (!g_sysres_active) {
        g_sysres_game[0] = r[0];
        g_sysres_game[1] = r[1];
        g_sysres_active = true;
        log::info("fixes: GSystemResolution {}x{} -> {}x{} while stereo renders (scene buffers cover the eye target)",
                  r[0], r[1], width, height);
    } else if (r[0] != g_sysres_written[0] || r[1] != g_sysres_written[1]) {
        // The game changed it (window resize): that is its value now.
        g_sysres_game[0] = r[0];
        g_sysres_game[1] = r[1];
        if (r[0] == width && r[1] == height) return;
    }
    r[0] = width;
    r[1] = height;
    g_sysres_written[0] = width;
    g_sysres_written[1] = height;
}

void restore_system_resolution() {
    std::int32_t* r = addresses().GSystemResolution;
    if (!r || !g_sysres_active) return;
    g_sysres_active = false;
    if (r[0] == g_sysres_written[0] && r[1] == g_sysres_written[1]) {
        r[0] = g_sysres_game[0];
        r[1] = g_sysres_game[1];
    }
    log::info("fixes: GSystemResolution back to {}x{}", r[0], r[1]);
}

bool system_resolution_overridden() { return g_sysres_active; }

}  // namespace ff7vr::engine::fixes
