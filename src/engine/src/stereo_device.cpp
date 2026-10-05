#include "stereo_device.h"

#include "fixed_host.h"
#include "bloom_fix.h"
#include "fixes.h"
#include "gpu_trace.h"
#include "player.h"
#include "rhi_command.h"
#include "ue_math.h"

#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/engine/cvars.h"

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <vector>

namespace ff7vr::engine::device {
namespace {

using namespace ue;

// ------------------------------------------------------------------ shared state
Settings g_settings;
float* g_near_plane = nullptr;

std::atomic<bool> g_active{false};             // this frame renders in stereo; read by both threads
std::atomic<bool> g_wanted{false};             // stereo switched on (ini, dev command, EnableStereo)
std::atomic<std::uint64_t> g_rt_size{0};       // committed eye size: width | height << 32
std::atomic<StereoHost*> g_host{nullptr};
FixedStereoHost* g_fixed = nullptr;            // created in init(), never freed
std::atomic<std::uint64_t> g_latest_frame_id{0};  // newest frame id the host returned

// Frames the device keeps rendering in stereo (with the last views) while the host has no
// frame for it, before it switches the engine to mono. Switching reallocates the eye
// target and suspends the rendering thread, so a single late XR frame must not do that.
constexpr int kHoldFrames = 45;

std::uint64_t pack(std::uint32_t w, std::uint32_t h) { return w | (static_cast<std::uint64_t>(h) << 32); }
std::uint32_t unpack_w(std::uint64_t v) { return static_cast<std::uint32_t>(v); }
std::uint32_t unpack_h(std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); }

// A view from the host that can be rendered: finite values, a unit orientation, a position
// within 100 m of the origin and a field of view that opens in both directions, less than
// 86 degrees from the axis on each side. A runtime that loses tracking may hand out
// undefined poses (OpenXR leaves them undefined without the valid bits); one of those
// would put NaNs into the view matrices.
bool usable(const HostPose& p) {
    const float v[] = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y, p.position.z};
    for (float x : v)
        if (!std::isfinite(x)) return false;
    const float n = p.orientation.x * p.orientation.x + p.orientation.y * p.orientation.y + p.orientation.z * p.orientation.z +
                    p.orientation.w * p.orientation.w;
    return n > 0.8f && n < 1.2f && std::fabs(p.position.x) < 100.0f && std::fabs(p.position.y) < 100.0f && std::fabs(p.position.z) < 100.0f;
}
bool usable(const HostView& v) {
    const HostFov& f = v.fov;
    const float a[] = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
    for (float x : a)
        if (!std::isfinite(x) || std::fabs(x) > 1.5f) return false;
    return usable(v.pose) && f.angleLeft < f.angleRight - 0.01f && f.angleDown < f.angleUp - 0.01f;
}

// Game thread only.
struct GameState {
    std::uint32_t eye_w = 0, eye_h = 0;    // committed: what the eye target has or is about to get
    std::uint32_t want_w = 0, want_h = 0;  // what the host asks for
    GameFrame frame{};
    HostView views[2]{};
    std::uint64_t views_frame_id = 0;
    std::uint64_t ticks = 0;
    std::uint64_t stereo_draws = 0;
    std::uint64_t transitions = 0;
    bool in_tick = false;
    bool views_built = false;  // stereo views were built during this Tick
    bool frame_has_views = false;  // this Tick got fresh views from the host
    int host_gap = 0;          // consecutive stereo frames without a host frame
    std::uint64_t held_frames = 0;
    std::uint64_t unusable_views = 0;  // frames whose views from the host were not usable (kept the last good ones)
    int logged_order = 0;
    // last eye cameras, for diagnostics
    FRotator cam_rot{};
    FVector cam_loc{};
    float world_to_meters = 0;
    FRotator eye_rot[2]{};
    FVector eye_loc[2]{};
    FMatrix proj[2]{};
};
GameState g;
std::mutex g_diag_mutex;  // guards the diagnostic copies read by status()/last_views()

// Which views each engine frame was rendered with, from the game thread to the render
// thread. One entry per engine loop iteration (pushed at the end of UGameEngine::Tick),
// consumed by RenderTexture_RenderThread, which Slate calls once per iteration after the
// iteration's scene. The game thread runs at most one iteration ahead of the render
// thread, so when the render thread consumes, at most two entries are legitimately
// queued; older ones belong to iterations whose window draw never happened (window not
// drawn, viewport drawn outside Tick) and are dropped, which re-synchronises the pairing.
struct FrameEntry {
    std::uint64_t frame_id = 0;  // 0: rendered with views held from an earlier frame
    bool stereo = false;         // the views were built (the frame was drawn in stereo)
    HostView views[2]{};
};
std::mutex g_fifo_mutex;
std::array<FrameEntry, 8> g_fifo{};
std::size_t g_fifo_head = 0, g_fifo_count = 0;
std::atomic<std::uint64_t> g_fifo_dropped{0};

void fifo_push(const FrameEntry& e) {
    std::lock_guard lock(g_fifo_mutex);
    if (g_fifo_count == g_fifo.size()) {
        g_fifo_head = (g_fifo_head + 1) % g_fifo.size();
        --g_fifo_count;
        ++g_fifo_dropped;
    }
    g_fifo[(g_fifo_head + g_fifo_count) % g_fifo.size()] = e;
    ++g_fifo_count;
}
FrameEntry fifo_pop() {
    std::lock_guard lock(g_fifo_mutex);
    while (g_fifo_count > 2) {
        g_fifo_head = (g_fifo_head + 1) % g_fifo.size();
        --g_fifo_count;
        ++g_fifo_dropped;
    }
    if (g_fifo_count == 0) return {};
    const FrameEntry e = g_fifo[g_fifo_head];
    g_fifo_head = (g_fifo_head + 1) % g_fifo.size();
    --g_fifo_count;
    return e;
}

// Counters (any thread).
struct Counters {
    std::atomic<std::uint64_t> view_offset{0}, projection{0}, adjust_rect{0}, render_texture{0}, calc_rt_size{0},
        realloc_yes{0}, frame_end_queued{0}, frame_end_failed{0}, frame_end_run{0}, host_frames{0};
    std::atomic<std::uint32_t> rt_tex_w{0}, rt_tex_h{0};  // size of the eye texture the render thread last saw
    std::atomic<int> rt_tex_format{0};
};
Counters g_count;

// Frame timing on the game thread (start of Tick to start of Tick).
struct FrameTimer {
    std::chrono::steady_clock::time_point last{};
    std::chrono::steady_clock::time_point window_start{};
    std::vector<float> samples;  // ms, current window
    float last_avg = 0, last_p50 = 0, last_p95 = 0, last_max = 0;
    std::size_t last_n = 0;
    bool last_window_stereo = false;
    bool window_stereo_all = true;

    void tick(bool stereo) {
        const auto now = std::chrono::steady_clock::now();
        if (g_settings.frame_window_reset.exchange(false)) {
            samples.clear();
            window_start = now;
            window_stereo_all = true;
        }
        if (last.time_since_epoch().count() != 0) {
            samples.push_back(std::chrono::duration<float, std::milli>(now - last).count());
            window_stereo_all = window_stereo_all && stereo;
        } else {
            window_start = now;
        }
        last = now;
        if (now - window_start >= std::chrono::milliseconds(g_settings.frame_window_ms.load()) && samples.size() >= 10) {
            std::vector<float> s = samples;
            std::sort(s.begin(), s.end());
            double sum = 0;
            for (float v : s) sum += v;
            last_n = s.size();
            last_avg = static_cast<float>(sum / s.size());
            last_p50 = s[s.size() / 2];
            last_p95 = s[std::min(s.size() - 1, s.size() * 95 / 100)];
            last_max = s.back();
            last_window_stereo = window_stereo_all;
            try {
                log::info("frame time: {} frames, avg {:.2f} ms, median {:.2f}, p95 {:.2f}, max {:.2f} ({}, eye {}x{})", last_n,
                          last_avg, last_p50, last_p95, last_max, window_stereo_all ? "stereo" : "not all stereo", g.eye_w,
                          g.eye_h);
            } catch (...) {
            }
            samples.clear();
            window_start = now;
            window_stereo_all = true;
        }
    }
};
FrameTimer g_timer;

// ------------------------------------------------------------------ helpers
int eye_index(EStereoscopicPass pass) { return pass == eSSP_RIGHT_EYE ? 1 : 0; }

ID3D11Texture2D* native_texture(FRHITexture2D* t) {
    if (!t) return nullptr;
    return *reinterpret_cast<ID3D11Texture2D**>(reinterpret_cast<std::uint8_t*>(t) + offsets::FD3D11Texture2D_Resource);
}

// Native pointers are checked once: their vtable must live in a system module (d3d11.dll
// or a layer on top of it), not in the game, before we call COM methods on them.
std::uintptr_t read_vtable_guarded(void* p) {
    __try {
        return *static_cast<std::uintptr_t*>(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool plausible_d3d_object(void* p) {
    static void* last_ok = nullptr;
    if (!p) return false;
    if (p == last_ok) return true;
    const std::uintptr_t vt = read_vtable_guarded(p);
    if (!vt) return false;
    static const module::Info exe = module::main_module();
    if (exe.contains(vt) || !module::from_address(vt)) return false;
    last_ok = p;
    return true;
}

// ------------------------------------------------------------------ IStereoRendering
bool IsStereoEnabled(const void*) { return g_active.load(std::memory_order_acquire); }
bool IsStereoEnabledOnNextFrame(const void*) { return g_active.load(std::memory_order_acquire); }

bool EnableStereo(void*, bool stereo) {
    request_active(stereo);
    return stereo;
}

void AdjustViewRect(const void*, EStereoscopicPass pass, std::int32_t* x, std::int32_t* y, std::uint32_t* sx,
                    std::uint32_t* sy) {
    ++g_count.adjust_rect;
    const bool second_half = (pass == eSSP_RIGHT_EYE) != g_settings.swap_rects.load();
    *x = second_half ? static_cast<std::int32_t>(g.eye_w) : 0;
    *y = 0;
    *sx = g.eye_w;
    *sy = g.eye_h;
}

FVector2D* GetTextSafeRegionBounds(const void*, FVector2D* out) {
    *out = FVector2D{0.75f, 0.75f};
    return out;
}

void CalculateStereoViewOffset(void*, EStereoscopicPass pass, FRotator* rotation, float world_to_meters,
                               FVector* location) {
    if (pass != eSSP_LEFT_EYE && pass != eSSP_RIGHT_EYE) return;
    ++g_count.view_offset;
    const int e = eye_index(pass);
    const Settings& s = g_settings;
    // The game's camera, then the camera mode's eye base (level boom, first person).
    const FRotator game_rot = *rotation;
    const FVector game_loc = *location;
    FRotator base_rot = game_rot;
    FVector base_loc = game_loc;
    bool force_decouple = false;
    player::adjust_camera(base_rot, base_loc, s.decouple_pitch.load(), force_decouple);
    math::EyeCameraInput in;
    in.camera_rotation = base_rot;
    in.camera_location = base_loc;
    in.units_per_metre = static_cast<double>(world_to_meters > 0 ? world_to_meters : 100.0f) * s.world_scale.load();
    in.decouple_pitch = s.decouple_pitch.load() || force_decouple;
    in.positional = s.positional.load();
    in.eye = g.views[e].pose;
    in.head = g.frame.head;
    FRotator out_rot{};
    FVector out_loc{};
    math::compose_eye(in, out_rot, out_loc);

    if (e == 0) {
        ++g.stereo_draws;
        g.views_built = true;
        if (g.logged_order < 3) {
            ++g.logged_order;
            try {
                log::info("stereo: views built {} UGameEngine::Tick (tick {}, frame {})", g.in_tick ? "inside" : "OUTSIDE",
                          g.ticks, g.views_frame_id);
            } catch (...) {
            }
        }
    }
    {
        std::lock_guard lock(g_diag_mutex);
        g.cam_rot = game_rot;
        g.cam_loc = game_loc;
        g.world_to_meters = world_to_meters;
        g.eye_rot[e] = out_rot;
        g.eye_loc[e] = out_loc;
    }
    if (g_settings.log_frames.load() > 0) {
        try {
            log::info("stereo: eye {} cam rot ({:.2f} {:.2f} {:.2f}) loc ({:.1f} {:.1f} {:.1f}) w2m {} -> rot ({:.2f} {:.2f} {:.2f}) "
                      "loc ({:.1f} {:.1f} {:.1f})",
                      e == 0 ? "L" : "R", game_rot.Pitch, game_rot.Yaw, game_rot.Roll, game_loc.X, game_loc.Y, game_loc.Z,
                      world_to_meters, out_rot.Pitch, out_rot.Yaw, out_rot.Roll, out_loc.X, out_loc.Y, out_loc.Z);
        } catch (...) {
        }
        if (e == 1) g_settings.log_frames.fetch_sub(1);
    }
    *rotation = out_rot;
    *location = out_loc;
}

FMatrix* GetStereoProjectionMatrix(const void*, FMatrix* out, EStereoscopicPass pass) {
    ++g_count.projection;
    const int e = eye_index(pass);
    const float near_plane = g_near_plane ? *g_near_plane : 10.0f;
    math::stereo_projection(g.views[e].fov, near_plane, *out);
    {
        std::lock_guard lock(g_diag_mutex);
        g.proj[e] = *out;
    }
    return out;
}

void InitCanvasFromView(void*, FSceneView*, UCanvas*) {}
bool Unknown8(void*) { return false; }

// The end of a stereo frame on the thread that owns the immediate context: hand the eye
// texture to the host and draw the desktop mirror. Appended to the engine's RHI command
// list from RenderTexture_RenderThread, so it runs after the frame's scene and before
// its Slate UI and Present (rhi_command.h). Static ring: the RHI thread is at most one
// frame behind the render thread, four slots leave ample room.
struct FrameEndCommand {
    rhi::Command base;
    EyeTexture eyes{};
    ID3D11Texture2D* back_buffer = nullptr;
    mirror::Mode mirror = mirror::Mode::Off;
    std::atomic<bool> pending{false};
};
FrameEndCommand g_frame_end[4];
unsigned g_frame_end_next = 0;  // render thread only

void frame_end_execute(void*, rhi::Command* self) {
    auto* c = reinterpret_cast<FrameEndCommand*>(self);
    try {
        gpu_trace::frame_boundary(c->eyes.texture);
        bloom_fix::frame(c->eyes.texture);
        if (StereoHost* h = g_host.load()) h->eye_texture_ready(c->eyes);
        if (c->mirror != mirror::Mode::Off)
            mirror::draw(c->eyes.texture, c->back_buffer, c->eyes.eyes[0], c->eyes.eyes[1], c->mirror);
    } catch (...) {
    }
    ++g_count.frame_end_run;
    c->pending.store(false, std::memory_order_release);
}

void RenderTexture_RenderThread(const void*, FRHICommandListImmediate* cmd_list, FRHITexture2D* back_buffer,
                                FRHITexture2D* src, FVector2D /*window_size*/) {
    ++g_count.render_texture;
    if (!src) return;
    ID3D11Texture2D* eye = native_texture(src);
    ID3D11Texture2D* bb = native_texture(back_buffer);
    if (!plausible_d3d_object(eye)) return;
    // Eye rects from the texture actually rendered this frame (the committed size may
    // already have moved on for the next frame).
    const auto* t = reinterpret_cast<const std::uint8_t*>(src);
    const std::uint32_t tw = *reinterpret_cast<const std::uint32_t*>(t + offsets::FRHITexture2D_SizeX);
    const std::uint32_t th = *reinterpret_cast<const std::uint32_t*>(t + offsets::FRHITexture2D_SizeY);
    g_count.rt_tex_w = tw;
    g_count.rt_tex_h = th;

    static ID3D11Texture2D* desc_tex = nullptr;
    static DXGI_FORMAT desc_fmt = DXGI_FORMAT_UNKNOWN;
    if (desc_tex != eye) {
        D3D11_TEXTURE2D_DESC d{};
        eye->GetDesc(&d);
        desc_tex = eye;
        desc_fmt = d.Format;
        g_count.rt_tex_format = static_cast<int>(d.Format);
        try {
            log::info("stereo: eye texture {}x{} (RHI size {}x{}), DXGI format {}", d.Width, d.Height, tw, th,
                      static_cast<int>(d.Format));
        } catch (...) {
        }
    }

    EyeTexture out;
    out.texture = eye;
    out.view_format = mirror::typed_view_format(desc_fmt);
    out.srgb_encoded = true;
    const std::uint32_t ew = tw / 2;
    out.eyes[0] = EyeRect{0, 0, ew, th};
    out.eyes[1] = EyeRect{static_cast<std::int32_t>(ew), 0, ew, th};
    const FrameEntry fe = fifo_pop();
    out.frame_id = fe.stereo ? fe.frame_id : 0;
    out.views_valid = fe.stereo;
    out.views[0] = fe.views[0];
    out.views[1] = fe.views[1];
    out.latest_frame_id = g_latest_frame_id.load();

    FrameEndCommand& c = g_frame_end[g_frame_end_next];
    if (c.pending.load(std::memory_order_acquire)) {
        ++g_count.frame_end_failed;  // the RHI thread is far behind: skip this frame's hand-over
        return;
    }
    c.base.execute = &frame_end_execute;
    c.eyes = out;
    const auto mode = static_cast<mirror::Mode>(g_settings.mirror.load());
    c.mirror = plausible_d3d_object(bb) ? mode : mirror::Mode::Off;
    c.back_buffer = bb;
    c.pending.store(true, std::memory_order_release);
    if (rhi::enqueue(cmd_list, &c.base)) {
        ++g_count.frame_end_queued;
        g_frame_end_next = (g_frame_end_next + 1) % (sizeof(g_frame_end) / sizeof(g_frame_end[0]));
    } else {
        c.pending.store(false, std::memory_order_release);
        ++g_count.frame_end_failed;
    }
}

void GetOrthoProjection(const void*, std::int32_t rt_width, std::int32_t, float, FMatrix* out) {
    // Same as the engine's default: identity for the left half, shifted by half the
    // target width for the right half.
    for (int m = 0; m < 2; ++m)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) out[m].M[r][c] = r == c ? 1.0f : 0.0f;
    out[1].M[3][0] = static_cast<float>(rt_width) * 0.5f;
}

void* Unknown11(void*) { return nullptr; }
void* GetRenderTargetManager(void*);
void* GetStereoLayers(void*) { return nullptr; }
void* DeviceDestructor(void* self, std::uint32_t) { return self; }

const StereoDeviceVtbl kDeviceVtbl{
    &IsStereoEnabled,
    &IsStereoEnabledOnNextFrame,
    &EnableStereo,
    &AdjustViewRect,
    &GetTextSafeRegionBounds,
    &CalculateStereoViewOffset,
    &GetStereoProjectionMatrix,
    &InitCanvasFromView,
    &Unknown8,
    &RenderTexture_RenderThread,
    &GetOrthoProjection,
    &Unknown11,
    &GetRenderTargetManager,
    &GetStereoLayers,
    &DeviceDestructor,
};

// ------------------------------------------------------------------ IStereoRenderTargetManager
bool ShouldUseSeparateRenderTarget(const void*) { return g_active.load(std::memory_order_acquire); }

// Game thread, every stereo frame, right before the views are drawn: the scene buffers of
// the frame about to be enqueued are sized from GSystemResolution (fixes.h).
void UpdateViewport(void*, bool, const FViewport*, void*) {
    if (g_active.load() && g.eye_w && g.eye_h)
        fixes::apply_system_resolution(static_cast<std::int32_t>(2 * g.eye_w), static_cast<std::int32_t>(g.eye_h));
}

void CalculateRenderTargetSize(void*, const FViewport*, std::uint32_t* sx, std::uint32_t* sy) {
    ++g_count.calc_rt_size;
    const std::uint64_t v = g_rt_size.load();
    const std::uint32_t w = unpack_w(v), h = unpack_h(v);
    if (w && h) {
        try {
            log::info("stereo: render target size {}x{} (window {}x{})", 2 * w, h, *sx, *sy);
        } catch (...) {
        }
        *sx = 2 * w;
        *sy = h;
    }
}

bool NeedReAllocateViewportRenderTarget(void*, const FViewport*) {
    if (g.want_w && g.want_h && (g.want_w != g.eye_w || g.want_h != g.eye_h)) {
        g.eye_w = g.want_w;
        g.eye_h = g.want_h;
        g_rt_size = pack(g.eye_w, g.eye_h);
        ++g_count.realloc_yes;
        return true;
    }
    return false;
}

bool NeedReAllocateDepthTexture(void*, const void*) { return false; }
std::uint32_t GetNumberOfBufferedFrames(const void*) { return 1; }
bool AllocateRenderTargetTexture(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t, std::uint32_t,
                                 std::uint32_t, std::uint32_t, FTexture2DRHIRef*, FTexture2DRHIRef*, std::uint32_t) {
    return false;  // the engine allocates it at the size from CalculateRenderTargetSize
}
bool AllocateDepthTexture(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t, std::uint32_t, std::uint32_t,
                          std::uint32_t, FTexture2DRHIRef*, FTexture2DRHIRef*, std::uint32_t) {
    return false;
}

const RenderTargetManagerVtbl kRtmVtbl{
    &ShouldUseSeparateRenderTarget,
    &UpdateViewport,
    &CalculateRenderTargetSize,
    &NeedReAllocateViewportRenderTarget,
    &NeedReAllocateDepthTexture,
    &GetNumberOfBufferedFrames,
    &AllocateRenderTargetTexture,
    &AllocateDepthTexture,
};

struct DeviceObject {
    const StereoDeviceVtbl* vtbl;
};
struct RtmObject {
    const RenderTargetManagerVtbl* vtbl;
};
DeviceObject g_device{&kDeviceVtbl};
RtmObject g_rtm{&kRtmVtbl};

void* GetRenderTargetManager(void*) { return g_active.load(std::memory_order_acquire) ? &g_rtm : nullptr; }

// Reference controller: the device lives for the whole process, so destruction is a no-op.
void ControllerDestroyObject(void*) {}
void* ControllerDestructor(void* self, std::uint32_t) { return self; }
const SharedReferenceControllerVtbl kControllerVtbl{&ControllerDestroyObject, &ControllerDestructor};
SharedReferenceController g_controller{&kControllerVtbl, 1, 1, &g_device};

void update_wanted_size() {
    StereoHost* h = g_host.load();
    std::uint32_t w = 0, hh = 0;
    // Until the host knows its size (an XR session may start later), use the fixed size.
    if ((h && h->eye_render_size(w, hh)) || (!g.want_w && g_fixed && g_fixed->eye_render_size(w, hh))) {
        w = std::clamp<std::uint32_t>(w, 64, 8192);
        hh = std::clamp<std::uint32_t>(hh, 64, 8192);
        g.want_w = w;
        g.want_h = hh;
    }
}

}  // namespace

Settings& settings() { return g_settings; }

void init(float* near_plane, bool start_active, const FixedStereoHost::Options& fixed) {
    g_near_plane = near_plane;
    if (!g_fixed) g_fixed = new FixedStereoHost(fixed);
    if (!g_host.load()) g_host = g_fixed;
    update_wanted_size();
    g.eye_w = g.want_w;
    g.eye_h = g.want_h;
    g_rt_size = pack(g.eye_w, g.eye_h);
    // Views to use until the first frame from the host.
    GameFrame f;
    g_fixed->begin_game_frame(false, f);
    if (f.views_valid) {
        g.views[0] = f.views[0];
        g.views[1] = f.views[1];
    }
    g_wanted = start_active;
    // Stereo starts with the first engine frame the host confirms (tick_begin).
    g_active = false;
}

ue::SharedPtrRaw shared_ptr() { return ue::SharedPtrRaw{&g_device, &g_controller}; }
const void* vtable() { return &kDeviceVtbl; }

void set_host(StereoHost* host) {
    g_host = host ? host : g_fixed;
    log::info("stereo: host {}", host ? "attached" : "reset to fixed values");
}
StereoHost* host() { return g_host.load(); }
FixedStereoHost* fixed_host() { return g_fixed; }

void request_active(bool on) {
    if (g_wanted.exchange(on) != on) log::info("stereo: switched {} (from the next frame)", on ? "on" : "off");
}
bool active() { return g_active.load(); }
bool wanted() { return g_wanted.load(); }

void tick_begin() {
    g.in_tick = true;
    g.views_built = false;
    ++g.ticks;
    const bool wanted = g_wanted.load();
    bool host_stereo = false;
    g.frame_has_views = false;
    update_wanted_size();
    if (StereoHost* h = g_host.load()) {
        GameFrame f;
        h->begin_game_frame(wanted, f);
        ++g_count.host_frames;
        if (f.views_valid && !(usable(f.views[0]) && usable(f.views[1]) && usable(f.head))) {
            // Keep the last good views and head; the image is still handed over with the
            // views it was rendered with, so the runtime re-projects it correctly.
            if (g.unusable_views++ < 5 || (g.unusable_views & 1023) == 0)
                log::warn("stereo: unusable views from the host (frame {}, {} so far): the last good views are kept", f.frame_id,
                          g.unusable_views);
            f.views[0] = g.views[0];
            f.views[1] = g.views[1];
            f.head = g.frame.head;
        }
        g.frame = f;
        if (f.views_valid) {
            g.views[0] = f.views[0];
            g.views[1] = f.views[1];
            g.views_frame_id = f.frame_id;
            g.frame_has_views = true;
            if (f.frame_id) g_latest_frame_id = f.frame_id;
        }
        host_stereo = f.stereo;
    }
    bool frame_stereo = false;
    if (wanted && host_stereo) {
        frame_stereo = true;
        g.host_gap = 0;
    } else if (wanted && g_active.load() && ++g.host_gap <= kHoldFrames) {
        // No frame from the host this time (a late XR frame, a hitch): keep the eye target
        // and render with the last views instead of flipping the engine to mono and back.
        frame_stereo = true;
        ++g.held_frames;
    }
    if (frame_stereo != g_active.load()) {
        if (frame_stereo) {
            // The eye target is (re)allocated at the committed size when the engine
            // switches to the separate render target this frame.
            g.eye_w = g.want_w;
            g.eye_h = g.want_h;
            g_rt_size = pack(g.eye_w, g.eye_h);
            fixes::set_view_rect_patch(true);
            fixes::light_fix_stereo(true);
            fixes::vr_window_enter();
            cvar::stereo_overrides(true);
        } else {
            fixes::restore_system_resolution();
            fixes::set_view_rect_patch(false);
            fixes::light_fix_stereo(false);
            cvar::stereo_overrides(false);
            g.host_gap = 0;
        }
        g_active = frame_stereo;
        ++g.transitions;
        if (g.transitions <= 20 || g.transitions % 100 == 0)
            log::info("stereo: rendering {} from tick {} (eye {}x{}, transition {})", frame_stereo ? "STEREO" : "mono", g.ticks,
                      g.eye_w, g.eye_h, g.transitions);
    }
    if (frame_stereo)
        fixes::apply_system_resolution(static_cast<std::int32_t>(2 * g.eye_w), static_cast<std::int32_t>(g.eye_h));
    g_timer.tick(frame_stereo);
}

void tick_end() {
    g.in_tick = false;
    // Mono frames have no separate target, so the render thread consumes nothing for them.
    if (g_active.load()) {
        FrameEntry e;
        e.frame_id = g.frame_has_views ? g.views_frame_id : 0;
        e.stereo = g.views_built;
        e.views[0] = g.views[0];
        e.views[1] = g.views[1];
        fifo_push(e);
    }
}

std::string status() {
    const std::uint64_t v = g_rt_size.load();
    std::string host_desc = g_host.load() == g_fixed && g_fixed ? g_fixed->describe() : std::string("host: render module");
    return std::format(
        "wanted={} active={} eye={}x{} want={}x{} rt_texture={}x{} fmt={} mirror={} scale={:.2f} decouple_pitch={} positional={} "
        "ticks={} stereo_draws={} transitions={} view_offset_calls={} proj_calls={} rect_calls={} render_texture_calls={} "
        "rt_size_calls={} reallocs={} frame_end_queued={} frame_end_run={} frame_end_failed={} fifo_dropped={} held_frames={} unusable_views={} "
        "sysres={} latest_frame={} frame_ms_avg={:.2f} p50={:.2f} p95={:.2f} max={:.2f} (n={}, {}) | {}",
        g_wanted.load() ? 1 : 0, g_active.load() ? 1 : 0, unpack_w(v), unpack_h(v), g.want_w, g.want_h, g_count.rt_tex_w.load(),
        g_count.rt_tex_h.load(),
        g_count.rt_tex_format.load(), mirror::to_string(static_cast<mirror::Mode>(g_settings.mirror.load())),
        g_settings.world_scale.load(), g_settings.decouple_pitch.load() ? 1 : 0, g_settings.positional.load() ? 1 : 0, g.ticks,
        g.stereo_draws, g.transitions, g_count.view_offset.load(), g_count.projection.load(), g_count.adjust_rect.load(),
        g_count.render_texture.load(), g_count.calc_rt_size.load(), g_count.realloc_yes.load(), g_count.frame_end_queued.load(),
        g_count.frame_end_run.load(), g_count.frame_end_failed.load(), g_fifo_dropped.load(), g.held_frames, g.unusable_views,
        fixes::system_resolution_overridden() ? 1 : 0, g_latest_frame_id.load(), g_timer.last_avg, g_timer.last_p50, g_timer.last_p95,
        g_timer.last_max, g_timer.last_n, g_timer.last_window_stereo ? "stereo" : "mixed/mono", host_desc);
}

std::string last_views() {
    std::lock_guard lock(g_diag_mutex);
    auto m = [](const FMatrix& p) {
        return std::format("[{:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} | {:.3f}]", p.M[0][0], p.M[1][1], p.M[2][0], p.M[2][1],
                           p.M[2][3], p.M[3][2], 0.0);
    };
    return std::format(
        "camera rot ({:.3f} {:.3f} {:.3f}) loc ({:.2f} {:.2f} {:.2f}) w2m {} | L rot ({:.3f} {:.3f} {:.3f}) loc ({:.2f} {:.2f} "
        "{:.2f}) proj {} | R rot ({:.3f} {:.3f} {:.3f}) loc ({:.2f} {:.2f} {:.2f}) proj {}",
        g.cam_rot.Pitch, g.cam_rot.Yaw, g.cam_rot.Roll, g.cam_loc.X, g.cam_loc.Y, g.cam_loc.Z, g.world_to_meters, g.eye_rot[0].Pitch,
        g.eye_rot[0].Yaw, g.eye_rot[0].Roll, g.eye_loc[0].X, g.eye_loc[0].Y, g.eye_loc[0].Z, m(g.proj[0]), g.eye_rot[1].Pitch,
        g.eye_rot[1].Yaw, g.eye_rot[1].Roll, g.eye_loc[1].X, g.eye_loc[1].Y, g.eye_loc[1].Z, m(g.proj[1]));
}

}  // namespace ff7vr::engine::device
