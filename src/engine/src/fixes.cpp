#include "fixes.h"

#include "engine_internal.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

namespace ff7vr::engine::fixes {

std::optional<bool> light_patch() {
    const std::uint8_t* p = addresses().LightSortKeyImm;
    if (!p) return std::nullopt;
    if (*p == 0x40) return false;
    if (*p == 0x60) return true;
    return std::nullopt;
}

bool set_light_patch(bool on) {
    std::uint8_t* p = addresses().LightSortKeyImm;
    auto cur = light_patch();
    if (!p || !cur) {
        log::warn("fixes: light sort-key patch site not available");
        return false;
    }
    if (*cur == on) return true;
    const std::uint8_t v = on ? 0x60 : 0x40;
    if (!hook::write_memory(p, &v, 1)) {
        log::warn("fixes: writing the light sort-key patch failed");
        return false;
    }
    log::info("fixes: light sort-key patch {}", on ? "on (0x60)" : "off (0x40, game default)");
    return true;
}

}  // namespace ff7vr::engine::fixes
