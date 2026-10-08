#include "ui_battle.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>

namespace ff7vr::render {

namespace {

constexpr float kBlendSeconds = 0.5f;

std::atomic<float> g_battleSize{1.57f};
std::atomic<bool> g_battle{false};
std::atomic<float> g_blend{0.0f};       // 0 = [ui] size, 1 = battle_size (before easing)
std::atomic<float> g_lastHeight{0.0f};  // the height last handed out
int64_t g_lastQpc = 0;                  // presenting thread

double QpcSeconds(int64_t d) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(d) / freq;
}

}  // namespace

void SetBattleActive(bool active) {
    if (g_battle.exchange(active) != active)
        log::info("render: battle {}: HUD panel {}", active ? "started" : "ended",
                  g_battleSize.load() <= 0.0f ? std::string("unchanged")
                  : active                    ? std::format("blends to {:.2f} m", g_battleSize.load())
                                              : std::string("blends back to [ui] size"));
}

namespace ui_battle {

void Start(const Config& c) {
    g_battleSize = static_cast<float>(std::clamp(c.get_float("ui", "battle_size", 1.57), 0.0, 50.0));
    log::info("render: UI layer in battles {}", g_battleSize.load() > 0.0f ? std::format("{:.2f} m high", g_battleSize.load()) : "unchanged");
}

float Height(float base) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    const float dt = g_lastQpc ? std::clamp(float(QpcSeconds(q.QuadPart - g_lastQpc)), 0.0f, 0.1f) : 0.0f;
    g_lastQpc = q.QuadPart;
    const float target = g_battle.load() ? 1.0f : 0.0f;
    float b = g_blend.load();
    b = target > b ? std::min(target, b + dt / kBlendSeconds) : std::max(target, b - dt / kBlendSeconds);
    g_blend = b;
    const float size = g_battleSize.load();
    float h = base;
    if (size > 0.0f) {
        const float eased = b * b * (3.0f - 2.0f * b);
        h = base + (size - base) * eased;
    }
    g_lastHeight = h;
    return h;
}

bool SetSize(const std::string& value) {
    char* end = nullptr;
    const float v = std::strtof(value.c_str(), &end);
    if (value.empty() || !end || *end) return false;
    g_battleSize = std::clamp(v, 0.0f, 50.0f);
    log::info("render: UI layer in battles {:.2f} m high (0 = unchanged)", g_battleSize.load());
    return true;
}

std::string Text() {
    return std::format("battle_size {:.2f} m, battle {}, blend {:.2f}, height now {:.2f} m", g_battleSize.load(), g_battle.load() ? 1 : 0,
                       g_blend.load(), g_lastHeight.load());
}

}  // namespace ui_battle
}  // namespace ff7vr::render
