#pragma once
// Built-in StereoHost used when no headset host is attached: fixed per-eye size, FOV and
// IPD from ff7vr.ini, and optionally the same scripted head motion as the XR layer's Null
// backend (yaw sweep +-30 deg over 8 s; position sway x +-3 cm / 4 s, y +-2 cm / 3 s,
// z +-1 cm / 5 s). Nobody consumes the eye texture.

#include "ff7vr/engine/stereo_host.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace ff7vr {
class Config;
}

namespace ff7vr::engine {

enum class HeadMotion { Static, YawSweep, Sway, YawAndSway };
const char* to_string(HeadMotion m);
bool parse_head_motion(const std::string& s, HeadMotion& out);

class FixedStereoHost final : public StereoHost {
public:
    struct Options {
        std::uint32_t eye_width = 1280;
        std::uint32_t eye_height = 1440;
        HostFov fov_left{};      // right eye mirrors left/right
        float ipd_metres = 0.064f;
        HeadMotion motion = HeadMotion::Static;
    };

    static Options from_config(const Config& cfg);

    explicit FixedStereoHost(const Options& o);

    bool eye_render_size(std::uint32_t& width, std::uint32_t& height) override;
    void begin_game_frame(bool stereo_active, GameFrame& out) override;
    void eye_texture_ready(const EyeTexture&) override {}

    // Runtime changes (dev commands); any thread.
    Options options() const;
    void set_options(const Options& o);
    std::string describe() const;

private:
    mutable std::mutex mutex_;
    Options opt_;
    std::uint64_t frame_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace ff7vr::engine
