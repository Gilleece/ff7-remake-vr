#include "stereo_device.h"

#include "fixed_host.h"
#include "bloom_fix.h"
#if FF7VR_ENGINE_WITH_DLSS
#include "dlss.h"
#endif
#include "fixes.h"
#include "gpu_trace.h"
#include "player.h"
#include "rhi_command.h"
#include "ue_math.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/engine/cvars.h"

#include <d3d11.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <mutex>
#include <new>
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
    std::uint32_t rect_w = 0, rect_h = 0;  // view rect of each eye this frame (render scale applied); 0 = whole half
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
    std::uint32_t rect_w = 0, rect_h = 0;  // eye view rect size the frame was rendered with (0 = whole half)
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

// Frame log (development, `stereo framelog start|stop <csv>`): timestamps and the calling
// thread's CPU time at fixed points of each frame on the game thread, the render thread and
// the thread that runs the frame-end command, so a slow frame can be attributed to a thread
// and to waiting or working. Off by default; when off each point costs one relaxed load.
namespace framelog {
enum Point : std::uint32_t {
    kTickBegin = 0,  // start of UGameEngine::Tick (game thread)
    kHostDone,       // the host's begin_game_frame returned (XR frame wait and poses)
    kTickBeginDone,  // the device's per-frame work at the start of Tick is done
    kTickEnd,        // end of UGameEngine::Tick
    kRenderEnd,      // RenderTexture_RenderThread: the render thread finished the frame's scene
    kRhiEnd,         // frame-end command starts on the thread that owns the immediate context
    kRhiEndDone,     // frame-end command done (hand-over to the host, desktop mirror)
};
struct Event {
    std::int64_t qpc;
    float cpu_ms;  // CPU time of the calling thread since it started (its cycle count, in ms)
    float gpu_ms;  // latest GPU frame time from the host (kTickBegin only)
    std::uint32_t point;
    std::uint32_t tid;
    std::uint64_t gpu_samples;
};
constexpr std::size_t kCapacity = std::size_t{1} << 18;
std::atomic<bool> g_on{false};
std::atomic<std::uint64_t> g_next{0};
Event* g_events = nullptr;  // allocated on the first start, never freed

// GetThreadTimes only advances in scheduler ticks (15.6 ms); the thread's cycle count is exact.
// Cycles are converted with the rate of the time stamp counter measured between start and stop.
double g_cycles_per_ms = 0;
std::int64_t g_start_qpc = 0;
std::uint64_t g_start_tsc = 0;
float thread_cpu_ms() {
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) return -1.0f;
    const double rate = g_cycles_per_ms > 0 ? g_cycles_per_ms : 3.4e6;
    return static_cast<float>(static_cast<double>(cycles) / rate);
}

void record(Point p, float gpu_ms = 0.0f, std::uint64_t gpu_samples = 0) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const std::uint64_t i = g_next.fetch_add(1, std::memory_order_relaxed);
    if (i >= kCapacity) return;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_events[i] = Event{q.QuadPart, thread_cpu_ms(), gpu_ms, p, GetCurrentThreadId(), gpu_samples};
}

std::string command(const std::vector<std::string>& a) {
    if (a.size() >= 3 && a[2] == "start") {
        if (!g_events) g_events = new (std::nothrow) Event[kCapacity];
        if (!g_events) return "err out of memory";
        g_on = false;
        g_next = 0;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_start_qpc = q.QuadPart;
        g_start_tsc = __rdtsc();
        g_on = true;
        return std::format("ok frame log recording (up to {} events)", kCapacity);
    }
    if (a.size() >= 4 && a[2] == "stop") {
        g_on = false;
        Sleep(50);  // let a point being recorded finish
        const std::uint64_t n = std::min<std::uint64_t>(g_next.load(), kCapacity);
        std::string path = a[3];
        for (std::size_t i = 4; i < a.size(); ++i) path += " " + a[i];
        FILE* f = nullptr;
        if (_wfopen_s(&f, log::widen(path).c_str(), L"w") != 0 || !f) return "err cannot write " + path;
        LARGE_INTEGER fq, q;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&q);
        const double tsc_per_ms = static_cast<double>(__rdtsc() - g_start_tsc) /
                                  (static_cast<double>(q.QuadPart - g_start_qpc) * 1000.0 / static_cast<double>(fq.QuadPart));
        // The CPU times were recorded with the previous rate (or a guess): rescale them.
        const double used = g_cycles_per_ms > 0 ? g_cycles_per_ms : 3.4e6;
        g_cycles_per_ms = tsc_per_ms;
        for (std::uint64_t i = 0; i < n; ++i) g_events[i].cpu_ms = static_cast<float>(g_events[i].cpu_ms * used / tsc_per_ms);
        std::fprintf(f, "qpc_freq,%lld\npoint,tid,qpc,cpu_ms,gpu_ms,gpu_samples\n", fq.QuadPart);
        for (std::uint64_t i = 0; i < n; ++i) {
            const Event& e = g_events[i];
            std::fprintf(f, "%u,%u,%lld,%.4f,%.4f,%llu\n", e.point, e.tid, e.qpc, e.cpu_ms, e.gpu_ms,
                         static_cast<unsigned long long>(e.gpu_samples));
        }
        std::fclose(f);
        return std::format("ok {} events written to {}", n, path);
    }
    return "err usage: stereo framelog start | stop <csv>";
}
}  // namespace framelog

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
    // Each eye starts at the corner of its half; with a render scale below 1 it covers only
    // part of it (the eye target and the scene buffers keep their size).
    *sx = g.rect_w && g.rect_w <= g.eye_w ? g.rect_w : g.eye_w;
    *sy = g.rect_h && g.rect_h <= g.eye_h ? g.rect_h : g.eye_h;
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
    framelog::record(framelog::kRhiEnd);
    try {
        gpu_trace::frame_boundary(c->eyes.texture);
        bloom_fix::frame(c->eyes.texture);
#if FF7VR_ENGINE_WITH_DLSS
        dlss::output_rects(c->eyes.eyes);  // DLSS upscaling wrote the whole half: the runtime and the mirror get it
#endif
        if (StereoHost* h = g_host.load()) h->eye_texture_ready(c->eyes);
        if (c->mirror != mirror::Mode::Off)
            mirror::draw(c->eyes.texture, c->back_buffer, c->eyes.eyes[0], c->eyes.eyes[1], c->mirror);
    } catch (...) {
    }
    ++g_count.frame_end_run;
    framelog::record(framelog::kRhiEndDone);
    c->pending.store(false, std::memory_order_release);
}

void RenderTexture_RenderThread(const void*, FRHICommandListImmediate* cmd_list, FRHITexture2D* back_buffer,
                                FRHITexture2D* src, FVector2D /*window_size*/) {
    framelog::record(framelog::kRenderEnd);
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
    const FrameEntry fe = fifo_pop();
    // The part of each half the frame's views rendered (render scale), else the whole half.
    const std::uint32_t rw = fe.rect_w && fe.rect_w <= ew ? fe.rect_w : ew;
    const std::uint32_t rh = fe.rect_h && fe.rect_h <= th ? fe.rect_h : th;
    out.eyes[0] = EyeRect{0, 0, rw, rh};
    out.eyes[1] = EyeRect{static_cast<std::int32_t>(ew), 0, rw, rh};
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

// ------------------------------------------------------------------ render scale
// The share of each eye's half of the target that the views render (per axis). Fixed at
// [stereo] render_scale, or adjusted every few frames from the GPU time of the frames
// (dynamic resolution): down as soon as their average exceeds the budget (a share of the
// display's frame period), up slowly while it stays below 90 % of it. The eye target and
// the scene buffers keep their size, so a change reallocates nothing; the runtime gets
// the smaller image as the layer's sub-image and scales it to the display.
struct DynState {
    float scale = 1.0f;
    std::uint64_t last_samples = 0;
    int skip = 0;       // GPU samples to ignore after a change (they arrive a few frames late)
    float window[16]{};
    int n = 0;
    std::uint64_t last_change_tick = 0;
};
DynState g_dyn;  // game thread
std::atomic<float> g_dyn_scale{1.0f}, g_dyn_avg{0.0f}, g_dyn_budget{0.0f};
std::atomic<std::uint64_t> g_dyn_changes{0};

constexpr int kDynWindow = 10;         // frames per decision (their median: single slow frames do not move the scale)
constexpr int kDynSkipAfterChange = 20;  // frames ignored after a change: the engine reallocates view-sized buffers, a few slow frames
constexpr float kDynStep = 0.02f;      // scale granularity
constexpr float kDynUpHeadroom = 0.90f;
constexpr std::uint64_t kDynUpHold = 60;  // frames after a change before the scale may rise

void finish_render_scale(float s);

void update_render_scale(bool stereo) {
    const float max_s = std::clamp(g_settings.render_scale.load(), 0.3f, 1.0f);
    const float min_s = std::clamp(g_settings.dynres_min.load(), 0.3f, max_s);
    float s = g_dyn.scale;
    // The GPU time is averaged also while the scale is fixed, for `dynres` status (diagnostics in a headset session).
    const bool dynamic = g_settings.dynres.load();
    if (!dynamic) s = max_s;
    if (stereo) {
        float ms = 0, hz = 0;
        std::uint64_t samples = 0;
        StereoHost* h = g_host.load();
        if (h && h->gpu_frame_time(ms, samples, hz) && samples != g_dyn.last_samples && hz > 0) {
            g_dyn.last_samples = samples;
            if (g_dyn.skip > 0) {
                --g_dyn.skip;
            } else if (g_dyn.n < kDynWindow) {
                g_dyn.window[g_dyn.n++] = ms;
            }
            if (g_dyn.n >= kDynWindow) {
                const float budget = std::clamp(g_settings.dynres_target.load(), 0.3f, 1.2f) * 1000.0f / hz;
                std::sort(g_dyn.window, g_dyn.window + g_dyn.n);
                const float avg = g_dyn.window[g_dyn.n / 2];  // median
                g_dyn.n = 0;
                g_dyn_avg = avg;
                g_dyn_budget = budget;
                if (!dynamic) return finish_render_scale(s);
                // GPU time grows roughly with the pixel count, so with the square of the scale.
                const float fit = s * std::sqrt(budget / std::max(avg, 0.1f));
                float next = s;
                if (avg > budget)
                    next = std::max(fit, s - 0.10f);
                else if (avg < budget * kDynUpHeadroom && g.ticks - g_dyn.last_change_tick >= kDynUpHold)
                    next = std::min(fit, s + 0.04f);
                next = std::floor(next / kDynStep + 0.001f) * kDynStep;
                next = std::clamp(next, min_s, max_s);
                if (std::fabs(next - s) >= kDynStep * 0.5f) {
                    try {
                        log::info("dynres: scale {:.2f} -> {:.2f} (GPU median {:.2f} ms of {} frames, budget {:.2f} ms)", s, next, avg,
                                  kDynWindow, budget);
                    } catch (...) {
                    }
                    s = next;
                    g_dyn.last_change_tick = g.ticks;
                    g_dyn.skip = kDynSkipAfterChange;
                    ++g_dyn_changes;
                }
            }
        }
    }
    finish_render_scale(std::clamp(s, std::min(min_s, max_s), max_s));
}

void finish_render_scale(float s) {
    g_dyn.scale = s;
    g_dyn_scale = s;
    if (s >= 0.999f || !g.eye_w || !g.eye_h) {
        g.rect_w = g.rect_h = 0;
    } else {
        auto scaled = [s](std::uint32_t full) {
            const auto v = static_cast<std::uint32_t>(std::lround(full * s / 8.0) * 8);
            return std::clamp<std::uint32_t>(v, 64, full);
        };
        g.rect_w = scaled(g.eye_w);
        g.rect_h = scaled(g.eye_h);
    }
}

std::string dynres_status() {
    return std::format("ok dynres {} scale {:.2f} (render_scale {:.2f}, min {:.2f}, target {:.2f} of the frame period) rect {}x{} of {}x{}; "
                       "GPU median {:.2f} ms (last 10 measured frames) budget {:.2f} ms; changes {}",
                       g_settings.dynres.load() ? "on" : "off", g_dyn_scale.load(), g_settings.render_scale.load(),
                       g_settings.dynres_min.load(), g_settings.dynres_target.load(), g.rect_w ? g.rect_w : g.eye_w,
                       g.rect_h ? g.rect_h : g.eye_h, g.eye_w, g.eye_h, g_dyn_avg.load(), g_dyn_budget.load(), g_dyn_changes.load());
}

std::string dynres_command(std::string_view args) {
    std::vector<std::string> a;
    for (std::size_t i = 0; i < args.size();) {
        while (i < args.size() && args[i] == ' ') ++i;
        std::size_t j = i;
        while (j < args.size() && args[j] != ' ') ++j;
        if (j > i) a.emplace_back(args.substr(i, j - i));
        i = j;
    }
    auto num = [](const std::string& s, float& v) {
        try {
            v = std::stof(s);
            return std::isfinite(v);
        } catch (...) {
            return false;
        }
    };
    float v = 0;
    if (a.empty() || a[0] == "status") return dynres_status();
    if (a[0] == "on" || a[0] == "off") {
        g_settings.dynres = a[0] == "on";
        log::info("dynres: switched {}", a[0]);
        return dynres_status();
    }
    if (a.size() == 2 && num(a[1], v)) {
        if (a[0] == "scale" && v >= 0.3f && v <= 1.0f) g_settings.render_scale = v;
        else if (a[0] == "min" && v >= 0.3f && v <= 1.0f) g_settings.dynres_min = v;
        else if (a[0] == "target" && v >= 0.3f && v <= 1.2f) g_settings.dynres_target = v;
        else return "err value out of range";
        log::info("dynres: {} = {:.2f}", a[0], v);
        return dynres_status();
    }
    return "err usage: dynres [status] | on | off | scale <0.3-1> | min <0.3-1> | target <share of the frame period>";
}

}  // namespace

void configure_render_scale(const Config& cfg) {
    g_settings.render_scale = static_cast<float>(std::clamp(cfg.get_float("stereo", "render_scale", 1.0), 0.3, 1.0));
    g_settings.dynres = cfg.get_bool("stereo", "dynamic_resolution", false);
    g_settings.dynres_min = static_cast<float>(std::clamp(cfg.get_float("stereo", "dynamic_resolution_min", 0.75), 0.3, 1.0));
    g_settings.dynres_target = static_cast<float>(std::clamp(cfg.get_float("stereo", "dynamic_resolution_target", 0.85), 0.3, 1.2));
    log::info("dynres: render scale {:.2f}, dynamic resolution {} (min {:.2f}, target {:.2f} of the frame period)",
              g_settings.render_scale.load(), g_settings.dynres.load() ? "on" : "off", g_settings.dynres_min.load(),
              g_settings.dynres_target.load());
    dev_commands::add("dynres", "dynres [status] | on | off | scale <0.3-1> | min <0.3-1> | target <share>: render scale and dynamic resolution in stereo",
                      [](std::string_view args) { return dynres_command(args); });
    // Reflections per eye (fixes.h); its draw hook belongs to the post-process fixes.
    fixes::set_ssr_per_eye(cfg.get_bool("stereo", "ssr_per_eye", true));
    fixes::set_ssr_fix(cfg.get_bool("stereo", "ssr_fix", true));
    dev_commands::add("ssr", "ssr [status] | on | off | fix 0|1 | poison 0-3: screen-space reflections per eye half, right-eye reflections fix",
                      [](std::string_view args) {
                          if (args == "on" || args == "off") fixes::set_ssr_per_eye(args == "on");
                          else if (args == "fix 0" || args == "fix 1") fixes::set_ssr_fix(args == "fix 1");
                          else if (args.size() == 8 && args.substr(0, 7) == "poison " && args[7] >= '0' && args[7] <= '3') fixes::set_ssr_poison(args[7] - '0');
                          else if (!args.empty() && args != "status") return std::string("err usage: ssr [status] | on | off | fix 0|1 | poison 0-3");
                          return "ok " + fixes::ssr_status();
                      });
    fixes::set_hzb_skip(static_cast<int>(cfg.get_int("stereo", "hzb_skip", 0)));
    dev_commands::add("hzb", "hzb [status] | 0-4: the unread hierarchical depth chain (1 = not built; 2, 3, 4 = tests)",
                      [](std::string_view args) {
                          if (args.size() == 1 && args[0] >= '0' && args[0] <= '4') fixes::set_hzb_skip(args[0] - '0');
                          else if (!args.empty() && args != "status") return std::string("err usage: hzb [status] | 0-4");
                          return "ok " + fixes::hzb_status();
                      });
    {
        const std::string m = cfg.get_string("stereo", "tonemap_shift", "auto");
        bloom_fix::set_tonemap_shift(m == "0" || m == "off" ? 0 : m == "1" || m == "on" ? 1 : 2);
    }
    dev_commands::add("tonemapshift", "tonemapshift [0|1|2]: right view's tonemapping input shifted to the origin for Luma (2 = auto: while Luma is loaded)",
                      [](std::string_view args) {
                          if (args == "0" || args == "1" || args == "2") bloom_fix::set_tonemap_shift(args[0] - '0');
                          else if (!args.empty()) return std::string("err usage: tonemapshift [0|1|2]");
                          return "ok " + bloom_fix::tonemap_shift_status();
                      });
}

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
    if (framelog::g_on.load(std::memory_order_relaxed)) {
        float gpu_ms = 0, hz = 0;
        std::uint64_t samples = 0;
        if (StereoHost* h = g_host.load()) h->gpu_frame_time(gpu_ms, samples, hz);
        framelog::record(framelog::kTickBegin, gpu_ms, samples);
    }
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
        framelog::record(framelog::kHostDone);
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
    update_render_scale(frame_stereo);
    if (frame_stereo)
        fixes::apply_system_resolution(static_cast<std::int32_t>(2 * g.eye_w), static_cast<std::int32_t>(g.eye_h));
    g_timer.tick(frame_stereo);
    fixes::hzb_tick();
    framelog::record(framelog::kTickBeginDone);
}

void tick_end() {
    framelog::record(framelog::kTickEnd);
    g.in_tick = false;
    // Mono frames have no separate target, so the render thread consumes nothing for them.
    if (g_active.load()) {
        FrameEntry e;
        e.frame_id = g.frame_has_views ? g.views_frame_id : 0;
        e.stereo = g.views_built;
        e.views[0] = g.views[0];
        e.views[1] = g.views[1];
        e.rect_w = g.rect_w;
        e.rect_h = g.rect_h;
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

std::string framelog_command(const std::vector<std::string>& a) { return framelog::command(a); }

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
