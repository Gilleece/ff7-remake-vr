#include "texture_streaming.h"

#include "ff7vr/core/log.h"
#include "ff7vr/engine/cvars.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace ff7vr::engine::texstream {
namespace {

Addresses g_a;
bool g_on = false;

// FStreamingManagerTexture beyond the getters, this build (docs/re/engine.md, "Texture
// streaming"): StreamingTextures, a TArray<FStreamingTexture> of 0xD8-byte entries at +0x10.
constexpr std::size_t kStreamingTextures = 0x10;
constexpr std::size_t kEntrySize = 0xd8;
// FStreamingTexture members (int32 unless noted).
constexpr std::size_t kMipCount = 0x10;
constexpr std::size_t kTextureSizes = 0x18;  // int32[14]: bytes with 1..14 mips resident
constexpr int kMaxMips = 14;
constexpr std::size_t kResidentMips = 0x5c;      // RequestedMips +0x60 (inferred order), MinAllowedMips +0x64, MaxAllowedMips +0x68
constexpr std::size_t kVisibleWantedMips = 0x8c;
constexpr std::size_t kHiddenWantedMips = 0x90;
constexpr std::size_t kBudgetedMips = 0x98;
constexpr std::size_t kWantedMips = 0xa0;
// Raw dump (`texstream raw`): the members around the over-budget pair and both
// FTextureStreamingStats (0xB8 bytes each at +0x328 and +0x3E0; zero in this build).
constexpr std::size_t kRawFrom = 0x2f0, kRawTo = 0x328;
constexpr std::size_t kStats = 0x328, kStatsSize = 0xb8;

struct Snapshot {
    bool ok = false;
    ULONGLONG at = 0;
    std::int64_t pool = 0;          // GTexturePoolSize
    std::int32_t textures = 0;      // streaming textures
    std::int32_t engine_wanting = 0;  // IStreamingManager::NumWantingResources
    std::int64_t resident = 0;      // bytes with the mips each texture has now
    std::int64_t required = 0;      // bytes with the mips the views ask for (max of visible and hidden wanted)
    std::int64_t budgeted = 0;      // bytes after the pool budget
    std::int32_t cut = 0;           // textures the budget gives fewer mips than the views ask for
    std::int64_t cut_bytes = 0;     // required - budgeted over those textures
    std::int32_t loading = 0;       // textures with fewer mips resident than wanted
    std::int64_t loading_bytes = 0;
};

std::mutex g_mutex;
Snapshot g_snap;  // g_mutex

int size_of(const std::uint8_t* e, int mips) {
    if (mips <= 0) return 0;
    if (mips > kMaxMips) mips = kMaxMips;
    return *reinterpret_cast<const std::int32_t*>(e + kTextureSizes + 4 * (mips - 1));
}

std::int32_t i32(const std::uint8_t* p, std::size_t off) { return *reinterpret_cast<const std::int32_t*>(p + off); }

// SEH-guarded reads (no C++ objects with destructors in these frames).
std::uint8_t* manager_guarded() {
    __try {
        std::uint8_t* coll = static_cast<std::uint8_t*>(*g_a.collection);
        if (!coll) return nullptr;
        std::uint8_t* m = *reinterpret_cast<std::uint8_t**>(coll + g_a.off_texture_manager);
        if (!m || *reinterpret_cast<std::uintptr_t*>(m) != g_a.vtable) return nullptr;
        return m;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Game thread (the thread that adds and removes streaming textures).
bool gather_guarded(Snapshot* s) {
    std::uint8_t* m = manager_guarded();
    if (!m) return false;
    __try {
        s->pool = *g_a.pool_size;
        s->engine_wanting = i32(m, g_a.off_num_wanting);
        const std::uint8_t* data = *reinterpret_cast<std::uint8_t* const*>(m + kStreamingTextures);
        const std::int32_t num = i32(m, kStreamingTextures + 8);
        if (num < 0 || num > 1000000 || (num > 0 && !data)) return false;
        s->textures = num;
        for (std::int32_t i = 0; i < num; ++i) {
            const std::uint8_t* e = data + static_cast<std::size_t>(i) * kEntrySize;
            if (!*reinterpret_cast<void* const*>(e)) continue;  // removed entry
            const int mip_count = i32(e, kMipCount);
            if (mip_count <= 0 || mip_count > kMaxMips) continue;
            const int resident = i32(e, kResidentMips);
            const int perfect = std::min(mip_count, std::max(i32(e, kVisibleWantedMips), i32(e, kHiddenWantedMips)));
            const int budgeted = std::min(mip_count, i32(e, kBudgetedMips));
            const int wanted = std::min(mip_count, i32(e, kWantedMips));
            s->resident += size_of(e, resident);
            s->required += size_of(e, perfect);
            s->budgeted += size_of(e, budgeted);
            if (budgeted < perfect) {
                ++s->cut;
                s->cut_bytes += size_of(e, perfect) - size_of(e, budgeted);
            }
            if (resident < wanted) {
                ++s->loading;
                s->loading_bytes += size_of(e, wanted) - size_of(e, resident);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool copy_guarded(const std::uint8_t* from, void* to, std::size_t n) {
    __try {
        std::memcpy(to, from, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

long long mb(std::int64_t bytes) { return static_cast<long long>(bytes / (1024 * 1024)); }

std::string cvar_text(const wchar_t* name) {
    const auto v = cvar::get(name);
    if (!v) return "?";
    if (v->f == static_cast<float>(v->i)) return std::format("{}", v->i);
    return std::format("{:.2f}", v->f);
}

std::string cvars_line() {
    return std::format("r.Streaming.PoolSize={} Boost={} MipBias={}", cvar_text(L"r.Streaming.PoolSize"),
                       cvar_text(L"r.Streaming.Boost"), cvar_text(L"r.Streaming.MipBias"));
}

// Over-budget warning (game thread).
constexpr ULONGLONG kPollMs = 500;
constexpr ULONGLONG kWarnAfterMs = 5000;
constexpr ULONGLONG kClearAfterMs = 3000;
constexpr std::int64_t kOverThreshold = 1024 * 1024;  // 1 MB
ULONGLONG g_next_poll = 0;
ULONGLONG g_over_since = 0;  // 0 = within the pool
ULONGLONG g_under_since = 0;
bool g_warned = false;
long long g_peak_over_mb = 0;
std::atomic<ULONGLONG> g_over_since_pub{0};
std::atomic<bool> g_warned_pub{false};

std::string figures(const Snapshot& s) {
    return std::format(
        "pool {} MB, required {} MB, budgeted {} MB, resident {} MB, over budget {} MB ({} textures with fewer mips than wanted), "
        "streaming in {} MB ({} textures), {} streaming textures",
        mb(s.pool), mb(s.required), mb(s.budgeted), mb(s.resident), mb(s.cut_bytes), s.cut, mb(s.loading_bytes), s.loading, s.textures);
}

}  // namespace

void init(const Addresses& a) {
    g_a = a;
    g_on = a.collection && a.vtable && a.off_texture_manager && a.off_over_budget && a.off_max_required && a.off_num_wanting &&
           a.pool_size;
    log::info("texture streaming status: {}", g_on ? "available (dev command texstream)" : "not available on this build");
}

bool available() { return g_on; }

void tick() {
    if (!g_on) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_poll) return;
    g_next_poll = now + kPollMs;
    Snapshot s;
    if (!gather_guarded(&s)) return;
    s.ok = true;
    s.at = now;
    {
        std::lock_guard lock(g_mutex);
        g_snap = s;
    }
    if (s.cut_bytes >= kOverThreshold) {
        g_under_since = 0;
        if (!g_over_since) g_over_since = now;
        g_peak_over_mb = std::max(g_peak_over_mb, mb(s.cut_bytes));
        if (!g_warned && now - g_over_since >= kWarnAfterMs) {
            g_warned = true;
            log::warn("texture streaming: {} MB over the pool ({} textures); textures show lower detail; raise r.Streaming.PoolSize or "
                      "lower r.Streaming.Boost (pool {} MB, required {} MB; {})",
                      mb(s.cut_bytes), s.cut, mb(s.pool), mb(s.required), cvars_line());
        }
    } else {
        g_over_since = 0;
        if (g_warned) {
            if (!g_under_since) g_under_since = now;
            if (now - g_under_since >= kClearAfterMs) {
                g_warned = false;
                log::info("texture streaming: back within the pool ({} MB; at most {} MB over before)", mb(s.pool), g_peak_over_mb);
                g_peak_over_mb = 0;
            }
        }
    }
    g_over_since_pub.store(g_over_since);
    g_warned_pub.store(g_warned);
}

std::string command(const std::string& args) {
    if (!g_on) return "err texture streaming status not available on this build";
    if (args.empty() || args == "status" || args == "timing") {
        Snapshot s;
        {
            std::lock_guard lock(g_mutex);
            s = g_snap;
        }
        if (!s.ok) return "err no reading yet (the texture streaming manager does not exist yet, or the engine is not ticking)";
        std::string state = "within the pool";
        if (const ULONGLONG since = g_over_since_pub.load())
            state = std::format("over budget for {:.1f} s{}", (GetTickCount64() - since) / 1000.0, g_warned_pub.load() ? " (warned)" : "");
        return std::format("ok {}; {}; {}; reading {} ms old", figures(s), state, cvars_line(), GetTickCount64() - s.at);
    }
    if (args == "raw") {
        std::uint8_t* m = manager_guarded();
        if (!m) return "err no manager";
        std::int64_t pre[(kRawTo - kRawFrom) / 8]{};
        std::int64_t stats[2][kStatsSize / 8]{};
        if (!copy_guarded(m + kRawFrom, pre, sizeof(pre)) || !copy_guarded(m + kStats, stats, sizeof(stats))) return "err read failed";
        std::int32_t wanting = 0;
        copy_guarded(m + g_a.off_num_wanting, &wanting, 4);
        std::string s = std::format("ok manager {} NumWantingResources {} +{:#x}:", static_cast<void*>(m), wanting, kRawFrom);
        for (std::int64_t v : pre) s += std::format(" {}", v);
        for (int k = 0; k < 2; ++k) {
            s += std::format(" | stats{} +{:#x}:", k, kStats + k * kStatsSize);
            for (std::int64_t v : stats[k]) s += std::format(" {}", v);
        }
        return s;
    }
    if (args.starts_with("entry ")) {
        std::uint8_t* m = manager_guarded();
        if (!m) return "err no manager";
        const int i = std::atoi(args.c_str() + 6);
        std::uint8_t* data = nullptr;
        std::int32_t num = 0;
        if (!copy_guarded(m + kStreamingTextures, &data, 8) || !copy_guarded(m + kStreamingTextures + 8, &num, 4)) return "err read failed";
        if (i < 0 || i >= num || !data) return std::format("err index out of range ({} entries)", num);
        std::int32_t e[kEntrySize / 4]{};
        if (!copy_guarded(data + static_cast<std::size_t>(i) * kEntrySize, e, sizeof(e))) return "err read failed";
        std::string s = std::format("ok entry {} of {}:", i, num);
        for (std::size_t k = 0; k < kEntrySize / 4; ++k) s += std::format(" {:x}:{}", k * 4, e[k]);
        return s;
    }
    return "err usage: texstream [status] | timing | raw | entry <i>";
}

}  // namespace ff7vr::engine::texstream
