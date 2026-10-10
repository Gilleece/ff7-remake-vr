#pragma once
// Texture streaming status: what the engine's texture streamer reports about its pool.
//
// UE 4.18's FStreamingManagerTexture (reached through the static of IStreamingManager::Get(),
// an FStreamingManagerCollection, member TextureStreamingManager) keeps the figures the
// engine's own "TEXTURE STREAMING POOL OVER x MB" message uses: the pool size
// (GTexturePoolSize, from r.Streaming.PoolSize), the memory over budget and the most memory
// it has ever needed. They are read here without calling into the engine; every address and
// member offset comes from docs/re/engine.md ("Texture streaming") and is only used on the
// known build. `texstream` (dev command) prints them with the r.Streaming.* variables, the
// timing block logs them every 10 s, and a warning is logged when the streamer stays over
// budget for a few seconds.

#include <cstddef>
#include <cstdint>
#include <string>

namespace ff7vr::engine::texstream {

struct Addresses {
    void** collection = nullptr;             // FStreamingManagerCollection* static of IStreamingManager::Get
    std::uintptr_t vtable = 0;               // FStreamingManagerTexture::vftable (checked before every read)
    std::size_t off_texture_manager = 0;     // FStreamingManagerCollection::TextureStreamingManager
    std::size_t off_over_budget = 0;         // FStreamingManagerTexture::MemoryOverBudget (int64)
    std::size_t off_max_required = 0;        // FStreamingManagerTexture::MaxEverRequired (int64)
    std::size_t off_num_wanting = 0;         // IStreamingManager::NumWantingResources (int32)
    const std::int64_t* pool_size = nullptr; // GTexturePoolSize (bytes)
};

// Null members leave the feature off ("texstream" then answers "err").
void init(const Addresses& a);
bool available();
// Game thread, once per engine frame: the over-budget warning.
void tick();
// texstream [status] | timing | raw | entry <i>
std::string command(const std::string& args);

}  // namespace ff7vr::engine::texstream
