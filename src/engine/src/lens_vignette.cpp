#include "lens_vignette.h"

#include "stereo_device.h"

#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"

#include <algorithm>
#include <atomic>
#include <format>

namespace ff7vr::engine::lens_vignette {
namespace {

constexpr std::size_t kVignetteIntensity = 0x418;  // float in FPostProcessSettings (build 1.0.0.7)

using CtorFn = void*(__fastcall*)(void*);
hook::InlineHook g_hook;
std::atomic<float> g_scale{1.0f};
std::atomic<std::uint64_t> g_calls{0}, g_scaled{0}, g_unexpected{0};

void* __fastcall ctor_detour(void* self) {
    void* r = g_hook.original<CtorFn>()(self);
    const float s = g_scale.load(std::memory_order_relaxed);
    if (s != 1.0f && self && device::active()) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        float* v = reinterpret_cast<float*>(static_cast<std::uint8_t*>(self) + kVignetteIntensity);
        if (*v == 1.0f) {
            *v = s;
            g_scaled.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_unexpected.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return r;
}

}  // namespace

bool init(std::uintptr_t ctor, float scale) {
    set_scale(scale);
    if (!ctor) {
        log::warn("lens vignette: post-process settings constructor not found; the game's vignette stays in stereo");
        return false;
    }
    if (!g_hook.create(ctor, &ctor_detour)) {
        log::warn("lens vignette: could not hook the post-process settings constructor; the game's vignette stays in stereo");
        return false;
    }
    log::info("lens vignette: hook installed; in stereo the game's vignette is scaled by {:.2f}", g_scale.load());
    return true;
}

void set_scale(float scale) { g_scale.store(std::clamp(scale, 0.0f, 1.0f), std::memory_order_relaxed); }

float scale() { return g_scale.load(std::memory_order_relaxed); }

std::string status() {
    return std::format("lens vignette: {}, scale in stereo {:.2f}; settings built in stereo {}, scaled {}, unexpected default {}",
                       g_hook.installed() ? "hooked" : "not hooked", g_scale.load(), g_calls.load(), g_scaled.load(),
                       g_unexpected.load());
}

}  // namespace ff7vr::engine::lens_vignette
