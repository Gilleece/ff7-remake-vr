// NVIDIA DLSS in place of the game's temporal anti-aliasing, per eye. See dlss.h and
// docs/dlss.md. Built only with -DFF7VR_DLSS=ON (needs the NVIDIA DLSS SDK).

#include "dlss.h"

#include "stereo_device.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ff7vr::engine::dlss {
namespace {

using Microsoft::WRL::ComPtr;

// Project id for NGX with the custom engine type (GUID-like, as NGX requires).
constexpr const char* kProjectId = "24ba703a-e546-49d9-ae9f-9a14a4d14340";
constexpr const char* kEngineVersion = "4.18";

// The view's uniform buffer (FViewUniformShaderParameters), as float4 rows: bound as the
// temporal anti-aliasing pixel shader's constant buffer 1. docs/re/engine.md section 12.
constexpr int kRowFirst = 0;    // rows kept from each capture
constexpr int kRowCount = 146;  // 0..145
constexpr int kRowViewForward = 52;             // xyz: the view's forward direction
constexpr int kRowWorldCameraOrigin = 59;       // xyz: the view's position (world units, cm)
constexpr int kRowPrevWorldCameraOrigin = 103;  // xyz: the position the engine's motion is relative to
constexpr int kRowClipToPrevClip = 114;  // 114..117
constexpr int kRowJitter = 118;          // xy this frame, zw previous frame (NDC units)
constexpr int kRowCameraCut = 140;       // .y: camera cut (candidate; logged, used only with `dlss cutflag 1`)

// ------------------------------------------------------------------ settings (any thread)
enum class Preset : unsigned { Default = 0, J = 10, K = 11, L = 12, M = 13 };

struct Settings {
    std::atomic<bool> enabled{false};
    std::atomic<int> mode{0};  // 0 dlaa, 1 upscale
    std::atomic<bool> init_requested{false};
    std::atomic<unsigned> preset{static_cast<unsigned>(Preset::Default)};
    std::atomic<bool> auto_exposure{true};
    std::atomic<int> mv_jitter{2};  // 0: remove the jitter difference, 1: MVs flagged jittered, 2: as is, not flagged
    std::atomic<bool> cut_reset{true};     // reset the history on a camera cut (see detect_cut)
    std::atomic<bool> cut_flag{false};     // also on row 140.y of the view constants (candidate flag)
    std::atomic<float> cut_distance{100.0f};  // camera moved farther than this in one frame (world units, cm)
    std::atomic<float> cut_angle{30.0f};      // camera turned more than this in one frame (degrees)
    // Upscale mode tests (picture quality at low input sizes)
    std::atomic<bool> up_hdr{false};       // flag the display-referred input as HDR
    std::atomic<float> sharpness{0.0f};    // InSharpness (ignored by current models)
    std::atomic<float> pre_exposure{0.0f}; // InPreExposure (0: not set)
    std::atomic<bool> no_grain{false};     // last pass at the reduced size without its noise texture (t0)
    std::atomic<bool> max_full{false};     // test: create the features for the input at render scale 1, whatever [stereo] render_scale is
    std::atomic<float> jitter_scale_x{0.5f};
    std::atomic<float> jitter_scale_y{-0.5f};
    std::atomic<int> reset_requests{0};
    std::atomic<int> recreate_requests{0};
    std::atomic<int> dump_requests{0};  // views whose constants go to the log (`dlss dump [n]`)
    std::atomic<bool> log_ngx{true};
    bool log_verbose = false;  // [dlss] log_verbose: NGX's most detailed log level (read at NGX start)
    // Fault isolation tests (dev commands; the test_ keys of [dlss] set them from the start)
    std::atomic<bool> skip_eval{false};  // upscale mode: everything but the evaluation; the game's own pass upscales
    std::atomic<bool> no_ngx{false};     // test_no_ngx (with test_skip): NGX is not initialised
    std::atomic<int> skip_level{0};      // test_skip: 1 no evaluation, 2 also no graded re-run, 3 also no motion vectors or state swap, 4 the game's own pass
    std::atomic<int> stall_requests{0};  // one full GPU wait at the next frame end, like a blocking read-back
    std::atomic<int> stall_ms{40};       // then the thread sleeps this long (the GPU stays idle)
    std::atomic<int> test_eyes{0};       // 1: left eye only, 2: right eye only (the other eye runs the game's passes)
    std::atomic<bool> zero_mv{false};    // motion vectors all zero
    std::atomic<bool> mv_sanitize{false};  // motion vectors: non-finite values to zero, clamped to the input size
    std::atomic<int> flush_after{0};     // 1: Flush the immediate context after each evaluation
    std::atomic<bool> own_params{false};  // each feature its own NGX parameter map (else the capability map for everything)
    std::atomic<bool> own_state{false};   // [dlss] context_state = own: draws and NGX inside our own device context state object (else the game's state)
    std::atomic<bool> copy_inputs{false};  // each eye's inputs copied into textures of the input size, output into its own texture
    std::atomic<int> copy_mask{7};         // with copy_inputs: which inputs DLSS reads from the copies (1 colour, 2 depth, 4 motion vectors)
    std::atomic<bool> mv_per_eye{true};    // [dlss] mv_textures = eye: each eye's motion vectors at the origin of a texture of its own (see MvTarget)
    std::atomic<bool> eval_at_end{false};  // upscale mode: evaluations at the end of the frame instead of at each eye's last pass
    // Checks and diagnostics
    std::atomic<bool> validate{true};     // every evaluation's inputs checked first; the game's pass runs if one fails
    std::atomic<bool> input_stats{false};  // GPU statistics of the evaluation inputs (non-finite or huge values), read back late
    std::atomic<int> events_requests{0};  // `dlss events`: dump the event ring to the log
    int log_lines = 0;                    // [dlss] log_lines: NGX messages kept in the log (0: 400, or 20000 with log_verbose)
    // Synthetic cost measurement: one extra DLSS evaluation per frame on blank textures of a
    // chosen input and output size (the image is discarded).
    std::atomic<bool> bench{false};
    std::atomic<unsigned> bench_in_w{0}, bench_in_h{0}, bench_out_w{0}, bench_out_h{0};
    std::atomic<int> bench_repeat{1};  // evaluations of the bench feature per frame
    std::atomic<bool> bench_jitter{false};  // bench: jitter offsets of an 8-phase Halton(2,3) sequence (else 0)
    std::atomic<bool> bench_write{false};   // bench: its inputs cleared to new values every frame (render targets)
    std::atomic<bool> bench_copyout{false};  // bench: its output copied to another texture after the evaluations
    std::atomic<bool> no_copyout{false};     // upscale: the output is not copied into the eye (test; the game's image stays with eval_at = frame_end)
    // [dlss] output = runtime (upscale mode): the engine's eye target at input_scale of the
    // runtime's eye size, DLSS's output at the runtime's size in a texture of its own, handed
    // to the runtime instead of the engine's target. output = engine: the engine's target at
    // the runtime's size, the views at input_scale of it ([stereo] render_scale), DLSS's output
    // copied back into the target.
    std::atomic<bool> output_runtime{true};
    std::atomic<float> engine_scale{0.5f};  // input_scale with output = runtime
};
Settings g_set;
std::atomic<std::uint64_t> g_runtime_eye{0};  // the runtime's eye size (w << 32 | h), from engine_eye_size
std::atomic<bool> g_ngx_failed{false};       // NGX could not start: the engine renders at the runtime's size again

bool runtime_output() {
    return g_set.enabled.load(std::memory_order_relaxed) && g_set.mode.load(std::memory_order_relaxed) == 1 &&
           g_set.output_runtime.load(std::memory_order_relaxed) && !g_ngx_failed.load(std::memory_order_relaxed);
}
std::filesystem::path g_dll_dir;
std::wstring g_extra_dll_dir;  // [dlss] dll_dir

const char* preset_name(unsigned p) {
    switch (p) {
        case 0: return "default";
        case 10: return "J";
        case 11: return "K";
        case 12: return "L";
        case 13: return "M";
        default: return "?";
    }
}
bool parse_preset(std::string s, unsigned& out) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (s == "default" || s == "0") out = 0;
    else if (s == "j") out = 10;
    else if (s == "k") out = 11;
    else if (s == "l") out = 12;
    else if (s == "m") out = 13;
    else return false;
    return true;
}

// ------------------------------------------------------------------ view uniform buffer capture
struct ViewRows {
    float v[kRowCount][4]{};
    std::uint64_t serial = 0;
};
std::atomic<UINT> g_ub_size{0};  // learned from the first recognised pass (constant buffer 1)
std::mutex g_ub_mutex;
std::unordered_map<ID3D11Buffer*, ViewRows> g_ub;  // pointers compared only, no reference held
std::atomic<std::uint64_t> g_ub_serial{0};
std::atomic<std::uint64_t> g_ub_captures_map{0}, g_ub_captures_create{0}, g_ub_captures_update{0};

bool copy_rows(float (*dst)[4], const void* data) {
    __try {
        std::memcpy(dst, static_cast<const std::uint8_t*>(data) + kRowFirst * 16, sizeof(float) * 4 * kRowCount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void store_rows(ID3D11Buffer* b, const void* data) {
    ViewRows r;
    if (!copy_rows(r.v, data)) return;
    r.serial = ++g_ub_serial;
    std::lock_guard lock(g_ub_mutex);
    if (g_ub.size() > 1024) {
        // Every 4096-byte constant buffer the engine writes is kept, so the map grows; drop the
        // entries not written recently (the views' buffers are written every frame). Clearing
        // the whole map instead made the next pass of both eyes miss its rows (one frame of
        // the game's anti-aliasing every few minutes).
        const std::uint64_t keep_from = r.serial > 512 ? r.serial - 512 : 0;
        std::erase_if(g_ub, [&](const auto& e) { return e.second.serial < keep_from; });
    }
    g_ub[b] = r;
}

bool is_view_ub(ID3D11Resource* res, UINT size) {
    D3D11_RESOURCE_DIMENSION dim{};
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
    return d.ByteWidth == size && (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);

struct UbHooks {
    hook::InlineHook map, unmap, update, create_buffer;
};
UbHooks* g_ub_hooks = new UbHooks();  // never destroyed
std::atomic<int> g_ub_hooks_state{0};  // 0 not yet, 1 installed, -1 failed

thread_local ID3D11Resource* t_mapped = nullptr;
thread_local void* t_mapped_data = nullptr;

// The immediate context has no multithread protection in this game (device creation flags 0):
// every thread other than the RHI thread that calls Map, Unmap or UpdateSubresource on it is
// recorded (first call logged per thread), and separately if the call came while an NGX call
// was running on the RHI thread.
std::atomic<DWORD> g_rhi_tid{0};      // the thread of the frame ends (dlss::frame)
std::atomic<ID3D11Device*> g_rhi_dev{nullptr};  // the game's device (compared only)
std::atomic<bool> g_in_ngx{false};    // an NGX create or evaluate is running on the RHI thread
std::atomic<std::uint64_t> g_foreign_calls{0}, g_foreign_during_ngx{0};
std::mutex g_tid_mutex;
std::unordered_map<DWORD, std::uint64_t> g_foreign_tids;

void note_context_thread(ID3D11DeviceContext* c, const char* fn) {
    const DWORD rhi = g_rhi_tid.load(std::memory_order_relaxed);
    const DWORD tid = GetCurrentThreadId();
    if (!rhi || tid == rhi || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    {
        // Only the game's device counts: an XR runtime in the process may drive a D3D11
        // device of its own from its own threads.
        static std::atomic<ID3D11Device*> game_dev{nullptr};
        ID3D11Device* d = nullptr;
        c->GetDevice(&d);
        if (d) d->Release();  // compared only
        ID3D11Device* gd = game_dev.load(std::memory_order_relaxed);
        if (!gd) {
            ID3D11Device* rd = g_rhi_dev.load(std::memory_order_relaxed);
            if (rd) game_dev.store(gd = rd, std::memory_order_relaxed);
        }
        if (!gd || d != gd) return;
    }
    ++g_foreign_calls;
    const bool during = g_in_ngx.load(std::memory_order_relaxed);
    if (during) ++g_foreign_during_ngx;
    bool first = false;
    {
        std::lock_guard lock(g_tid_mutex);
        first = g_foreign_tids[tid]++ == 0;
    }
    static std::atomic<int> during_logs{0};
    if (first || (during && during_logs.fetch_add(1) < 20)) {
        wchar_t* name = nullptr;
        std::string tname;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name) {
            tname = log::narrow(name);
            LocalFree(name);
        }
        log::info("dlss: immediate context {} from thread {} '{}' (the RHI thread is {}){}", fn, tid, tname, rhi,
                  during ? ": WHILE AN NGX CALL RAN ON THE RHI THREAD" : "");
    }
}

HRESULT STDMETHODCALLTYPE map_detour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                     D3D11_MAPPED_SUBRESOURCE* out) {
    note_context_thread(c, "Map");
    const HRESULT hr = g_ub_hooks->map.original<MapFn>()(c, r, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && sub == 0 && type == D3D11_MAP_WRITE_DISCARD && g_ub_size.load(std::memory_order_relaxed)) {
        t_mapped = r;
        t_mapped_data = out->pData;
    }
    return hr;
}

void STDMETHODCALLTYPE unmap_detour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    note_context_thread(c, "Unmap");
    if (r && r == t_mapped && t_mapped_data) {
        const UINT size = g_ub_size.load(std::memory_order_relaxed);
        if (size && is_view_ub(r, size)) {
            store_rows(static_cast<ID3D11Buffer*>(r), t_mapped_data);
            ++g_ub_captures_map;
        }
        t_mapped = nullptr;
        t_mapped_data = nullptr;
    }
    g_ub_hooks->unmap.original<UnmapFn>()(c, r, sub);
}

void STDMETHODCALLTYPE update_detour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp,
                                     UINT dp) {
    note_context_thread(c, "UpdateSubresource");
    const UINT size = g_ub_size.load(std::memory_order_relaxed);
    if (size && r && data && !box && sub == 0 && is_view_ub(r, size)) {
        store_rows(static_cast<ID3D11Buffer*>(r), data);
        ++g_ub_captures_update;
    }
    g_ub_hooks->update.original<UpdateFn>()(c, r, sub, box, data, rp, dp);
}

HRESULT STDMETHODCALLTYPE create_buffer_detour(ID3D11Device* d, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init,
                                               ID3D11Buffer** out) {
    const HRESULT hr = g_ub_hooks->create_buffer.original<CreateBufferFn>()(d, desc, init, out);
    const UINT size = g_ub_size.load(std::memory_order_relaxed);
    if (SUCCEEDED(hr) && size && desc && init && init->pSysMem && out && *out && desc->ByteWidth == size &&
        (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        store_rows(*out, init->pSysMem);
        ++g_ub_captures_create;
    }
    return hr;
}

// Context slots (d3d11.h order): Map 14, Unmap 15, UpdateSubresource 48. Device: CreateBuffer 3.
bool install_ub_hooks(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    int st = g_ub_hooks_state.load();
    if (st != 0) return st > 0;
    void** cvt = *reinterpret_cast<void***>(ctx);
    void** dvt = *reinterpret_cast<void***>(dev);
    UbHooks& h = *g_ub_hooks;
    const bool ok = h.map.create(cvt[14], &map_detour) && h.unmap.create(cvt[15], &unmap_detour) &&
                    h.update.create(cvt[48], &update_detour) && h.create_buffer.create(dvt[3], &create_buffer_detour);
    if (!ok) {
        h.map.remove();
        h.unmap.remove();
        h.update.remove();
        h.create_buffer.remove();
    }
    g_ub_hooks_state = ok ? 1 : -1;
    log::info("dlss: view uniform buffer capture hooks {}", ok ? "installed (Map/Unmap, UpdateSubresource, CreateBuffer)" : "FAILED");
    return ok;
}

bool lookup_rows(ID3D11Buffer* b, ViewRows& out) {
    std::lock_guard lock(g_ub_mutex);
    auto it = g_ub.find(b);
    if (it == g_ub.end()) return false;
    out = it->second;
    return true;
}

const float* row(const ViewRows& r, int i) { return r.v[i - kRowFirst]; }

// ------------------------------------------------------------------ camera cuts (RHI thread)
// The history of an eye is reset when its camera jumps: what DLSS accumulated no longer
// belongs to the view, and the motion vectors may not describe the change. Per eye and per
// frame, from the view constants of the anti-aliasing pass:
//  - engine: the camera the engine's motion is relative to (PrevWorldCameraOrigin) is not
//    the camera of the eye's previous frame. The engine resets its previous-frame matrices
//    on its own camera cuts (cuts in cutscenes, teleports, a new view state), and then the
//    motion vectors claim no motion although the picture changed.
//  - distance / angle: the camera moved farther than cut_distance or turned more than
//    cut_angle since the eye's previous frame (a switch between the game's camera and the
//    mod's first or third person camera, recentering). The blend between first and third
//    person stays below these.
//  - flag: row 140.y of the view constants (a candidate camera-cut flag, only with cutflag).
enum CutReason : unsigned { kCutEngine = 1, kCutFlag = 2, kCutDistance = 4, kCutAngle = 8 };

struct EyeTrack {
    bool valid = false;
    std::uint64_t frame = 0;
    float origin[3]{}, fwd[3]{};
};
EyeTrack g_track[2];
std::atomic<std::uint64_t> g_cut_count[4]{};  // per reason bit
std::atomic<std::uint64_t> g_cut_resets{0}, g_flag_frames{0};
std::atomic<int> g_cut_logs{0};

float dist3(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string cut_text(unsigned why) {
    std::string s;
    if (why & kCutEngine) s += "engine ";
    if (why & kCutFlag) s += "flag ";
    if (why & kCutDistance) s += "distance ";
    if (why & kCutAngle) s += "angle ";
    if (!s.empty()) s.pop_back();
    return s;
}

unsigned detect_cut(int eye, const ViewRows& rows, std::uint64_t frame, std::string* text) {
    const float* o = row(rows, kRowWorldCameraOrigin);
    const float* po = row(rows, kRowPrevWorldCameraOrigin);
    const float* f = row(rows, kRowViewForward);
    EyeTrack& t = g_track[eye];
    unsigned why = 0;
    float moved = 0, gap = 0, angle = 0;
    if (t.valid && t.frame + 1 == frame) {
        moved = dist3(o, t.origin);
        gap = dist3(po, t.origin);
        const float lf = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        const float lt = std::sqrt(t.fwd[0] * t.fwd[0] + t.fwd[1] * t.fwd[1] + t.fwd[2] * t.fwd[2]);
        if (lf > 0.5f && lt > 0.5f) {
            const float c = std::clamp((f[0] * t.fwd[0] + f[1] * t.fwd[1] + f[2] * t.fwd[2]) / (lf * lt), -1.0f, 1.0f);
            angle = std::acos(c) * 57.29578f;
        }
        if (gap > 1.0f) why |= kCutEngine;
        if (moved > g_set.cut_distance.load()) why |= kCutDistance;
        if (angle > g_set.cut_angle.load()) why |= kCutAngle;
    }
    if (row(rows, kRowCameraCut)[1] != 0.0f) {
        ++g_flag_frames;
        if (g_set.cut_flag.load()) why |= kCutFlag;
    }
    t.valid = true;
    t.frame = frame;
    std::memcpy(t.origin, o, sizeof(t.origin));
    std::memcpy(t.fwd, f, sizeof(t.fwd));
    if (why) {
        for (int b = 0; b < 4; ++b)
            if (why & (1u << b)) ++g_cut_count[b];
        const std::string s = std::format("eye {} frame {}: {} (moved {:.1f} cm, turned {:.1f} deg, engine's previous camera {:.1f} cm from the eye's last, "
                                          "row 140 {:.3g})",
                                          eye, frame, cut_text(why), moved, angle, gap, row(rows, kRowCameraCut)[1]);
        if (g_cut_logs.fetch_add(1) < 40) log::info("dlss: camera cut {}", s);
        if (text) *text = s;
    }
    return why;
}

// ------------------------------------------------------------------ history resets per eye (RHI thread)
// Every evaluation that resets an eye's history is counted by its causes, per eye: a feature
// created for it, the eye not evaluated in the previous frame, a camera cut (detect_cut), a
// request (`dlss reset`, switching on). An eye that resets often has no temporal history to
// speak of and looks unstable; more than kResetBurst resets in kResetWindow frames is logged.
enum ResetCause : int { kResetCreated, kResetGap, kResetCut, kResetRequest, kResetCauses };
constexpr const char* kResetNames[kResetCauses] = {"new feature", "not evaluated in the previous frame", "camera cut", "requested"};
constexpr std::uint64_t kResetWindow = 100;
constexpr unsigned kResetBurst = 10;
struct ResetTrack {
    std::atomic<std::uint64_t> by[kResetCauses]{};
    std::atomic<std::uint64_t> resets{0}, evals{0}, bursts{0};
    std::uint64_t window_start = 0;  // RHI thread
    unsigned window_resets = 0;
};
ResetTrack g_resets[2];

void note_reset(int eye, std::uint64_t frame, bool created, bool gap, bool cut, bool request) {
    ResetTrack& t = g_resets[eye];
    ++t.evals;
    const bool reset = created || gap || cut || request;
    if (created) ++t.by[kResetCreated];
    if (gap) ++t.by[kResetGap];
    if (cut) ++t.by[kResetCut];
    if (request) ++t.by[kResetRequest];
    if (reset) {
        ++t.resets;
        ++t.window_resets;
    }
    if (frame >= t.window_start + kResetWindow) {
        if (t.window_resets > kResetBurst) {
            const std::uint64_t n = ++t.bursts;
            if (n <= 3 || (n & (n - 1)) == 0)
                log::warn("dlss: eye {}: history reset {} times in the last {} frames (time {}): new feature {}, not evaluated in the previous frame {}, "
                          "camera cut {}, requested {} (since the start)",
                          eye, t.window_resets, frame - t.window_start, n, t.by[kResetCreated].load(), t.by[kResetGap].load(), t.by[kResetCut].load(),
                          t.by[kResetRequest].load());
        }
        t.window_start = frame;
        t.window_resets = 0;
    }
}

std::string resets_text() {
    std::string s;
    for (int e = 0; e < 2; ++e) {
        const ResetTrack& t = g_resets[e];
        s += std::format("{}{} {} of {} evaluations (", e ? "; " : "", e ? "R" : "L", t.resets.load(), t.evals.load());
        for (int c = 0; c < kResetCauses; ++c) s += std::format("{}{} {}", c ? ", " : "", kResetNames[c], t.by[c].load());
        s += std::format("; windows with more than {} in {} frames: {})", kResetBurst, kResetWindow, t.bursts.load());
    }
    return s;
}

// ------------------------------------------------------------------ NGX (RHI thread)
std::string vram_text(ID3D11Device* dev);

struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    // The parameter map of its creation and evaluations: its own (NVSDK_NGX_D3D11_AllocateParameters,
    // destroyed after the feature is released) or, with [dlss] params = shared, the capability map.
    NVSDK_NGX_Parameter* params = nullptr;
    bool own_params = false;
    bool subrects = true;  // created with output sub-rectangles (off with [dlss] test_copy_inputs)
    int quality = -1;      // the NGX mode (NVSDK_NGX_PerfQuality_Value) it was created for
    UINT w = 0, h = 0, ow = 0, oh = 0;
    unsigned preset = 0;
    int flags = 0;
    std::uint64_t last_eval_frame = 0;
    // Input rectangles an evaluation may use: min..creation size when NGX allows dynamic
    // scaling for this feature, otherwise exactly the creation size. An evaluation outside
    // it fails in NGX ("Dynamic scaling disabled ... RenderSubrect must match Creation time
    // res") and was followed by a GPU fault and a driver reset on the test machine.
    bool dynamic = false;
    UINT min_w = 0, min_h = 0;
};

struct Timing {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* t0 = nullptr;
    ID3D11Query* t1 = nullptr;  // after the motion vectors
    ID3D11Query* t2 = nullptr;  // after DLSS and the copy
    int eye = 0;
    bool pending = false;
};

struct Rhi {
    ID3D11Device* dev = nullptr;
    int ngx_state = 0;  // 0 not tried, 1 ready, -1 failed
    NVSDK_NGX_Parameter* params = nullptr;
    Feature feat[2];
    std::uint64_t frame = 0;
    // Motion vector pass
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3DDeviceContextState> state;
    bool shaders_failed = false;
    // Double-wide textures like the pass's colour input
    ComPtr<ID3D11Texture2D> mv;
    ComPtr<ID3D11RenderTargetView> mv_rtv;
    ComPtr<ID3D11ShaderResourceView> mv_srv;
    ComPtr<ID3D11Texture2D> out;
    D3D11_TEXTURE2D_DESC mv_desc{}, out_desc{};
    // Each eye's own motion vector texture ([dlss] mv_textures = eye, see MvTarget)
    ComPtr<ID3D11Texture2D> mv_eye[2];
    ComPtr<ID3D11RenderTargetView> mv_eye_rtv[2];
    ComPtr<ID3D11ShaderResourceView> mv_eye_srv[2];
    D3D11_TEXTURE2D_DESC mv_eye_desc[2]{};
    // Recognised pass
    ID3D11PixelShader* taa_ps = nullptr;  // compared only
    // Timing ring
    std::array<Timing, 8> timing{};
    unsigned timing_next = 0;
    std::chrono::steady_clock::time_point last_frame_time{};
    float frame_dt_ms = 11.1f;
    // Synthetic bench
    Feature bench;
    UINT bench_out_w = 0, bench_out_h = 0;
    ComPtr<ID3D11Texture2D> bench_color, bench_depth, bench_mv, bench_out, bench_copy;
};
Rhi* g_rhi = new Rhi();  // never freed (D3D objects must not be released at process exit)

// Status shown by "dlss status" (written on the RHI thread, read on the pipe thread).
struct Info {
    std::string ngx = "not initialised";
    std::string capability;
    std::string dll;
    std::string optimal;
    std::string pass;  // the recognised pass's inputs
    std::string features[2];
    std::string last_error;
    std::uint64_t taa_seen = 0, replaced = 0, fallback = 0, no_rows = 0, creates = 0, eval_fail = 0;
    unsigned seen_this_frame = 0, seen_last_frame = 0;
    double ms_sum[3]{}, mv_ms_sum[3]{};  // [2]: the synthetic bench
    std::uint64_t ms_n[3]{};
    std::string bench;
    float jitter_px[2][2]{};
    std::string dump;
    std::string last_cut;
    std::string cut_test;
    std::string up_sizes;  // upscale mode: feature size, input and output rectangles
    std::string vram;      // the latest video memory reading (every 2 s while NGX runs)
};
std::mutex g_info_mutex;
Info g_info;

template <class F>
void with_info(F&& f) {
    std::lock_guard lock(g_info_mutex);
    f(g_info);
}

// ------------------------------------------------------------------ event ring (RHI thread writes, any thread dumps)
// The last kEventCount DLSS events: the passes seen with their textures and rectangles, every
// evaluation with its parameters, feature creation and release, failed checks and the input
// statistics read back from the GPU. Written to the log when the device is lost and by
// `dlss events`; the GPU runs a few frames behind the RHI thread, so the evaluation that
// faulted is among the last ones recorded.
constexpr std::size_t kEventCount = 400;
struct EventRing {
    std::mutex m;
    std::array<std::string, kEventCount> lines;
    std::size_t next = 0;
    std::uint64_t total = 0;
};
EventRing g_events;

void event(std::string s) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::string line = std::format("{:02}:{:02}:{:02}.{:03} f{} {}", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_rhi->frame, s);
    std::lock_guard lock(g_events.m);
    g_events.lines[g_events.next] = std::move(line);
    g_events.next = (g_events.next + 1) % kEventCount;
    ++g_events.total;
}

void dump_events(const char* why) {
    std::lock_guard lock(g_events.m);
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(g_events.total, kEventCount));
    log::info("dlss: the last {} events ({}; {} recorded in all):", n, why, g_events.total);
    for (std::size_t i = 0; i < n; ++i) log::info("dlss: event {}", g_events.lines[(g_events.next + kEventCount - n + i) % kEventCount]);
}

std::string res_text(ID3D11Resource* r) {
    D3D11_TEXTURE2D_DESC d{};
    D3D11_RESOURCE_DIMENSION dim{};
    if (!r) return "null";
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return std::format("{} (not a 2D texture)", static_cast<void*>(r));
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    return std::format("{} {}x{} f{}", static_cast<void*>(r), d.Width, d.Height, static_cast<int>(d.Format));
}

// ------------------------------------------------------------------ checks before every evaluation (RHI thread)
// Each evaluation's inputs are checked against what the feature and NGX expect; if one check
// fails, the evaluation is not made and the game's own pass runs for that eye this frame.
// Each reason is logged the first time it occurs and counted (`dlss status`).
enum Check : int {
    kChkFeature,     // no feature, or the input rectangle is outside the sizes it accepts
    kChkColor,       // colour input missing, not a 2D texture, rectangle outside it, or an unexpected format
    kChkDepth,       // depth missing, not a 2D texture, rectangle outside it, or an unexpected format
    kChkMv,          // motion vectors missing, rectangle outside them, or not R16G16_FLOAT
    kChkOutput,      // output missing, the output rectangle outside it, no unordered access, or an unexpected format
    kChkJitter,      // jitter not finite or more than a pixel
    kChkEyes,        // upscale mode: the eye's anti-aliasing pass not seen this frame before its last pass
    kChkCount
};
constexpr const char* kCheckNames[kChkCount] = {"feature", "colour", "depth", "motion vectors", "output", "jitter", "pass order"};
std::atomic<std::uint64_t> g_check_fail[kChkCount]{};
std::atomic<std::uint64_t> g_checked{0}, g_depth_changes{0};
ID3D11Resource* g_last_depth[2]{};  // compared only
std::atomic<int> g_check_logs{0};

struct EvalInputs {
    int eye = 0;
    bool upscale = false;
    NVSDK_NGX_Handle* handle = nullptr;
    UINT fw = 0, fh = 0, fminw = 0, fminh = 0, fow = 0, foh = 0;  // the feature
    ID3D11Resource* color = nullptr;
    ID3D11Resource* depth = nullptr;
    ID3D11Resource* mv = nullptr;
    ID3D11Resource* out = nullptr;
    UINT x = 0, y = 0, w = 0, h = 0;  // input rectangle (colour)
    UINT dx = 0, dy = 0, mx = 0, my = 0;  // its origin in the depth and the motion vectors
    UINT ox = 0, oy = 0;              // output rectangle origin (the size is the feature's output size)
    float jx = 0, jy = 0;
    bool reset = false;
};

bool finite(float v) { return std::isfinite(v); }

bool rect_in(ID3D11Resource* r, UINT x, UINT y, UINT w, UINT h, D3D11_TEXTURE2D_DESC& d) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    return w > 0 && h > 0 && x + w <= d.Width && y + h <= d.Height && d.SampleDesc.Count == 1;
}

// -1: every check passed; otherwise the failed check (and a description in `detail`).
int check_eval(const EvalInputs& e, std::string& detail) {
    ++g_checked;
    D3D11_TEXTURE2D_DESC d{};
    if (!e.handle || e.w > e.fw || e.h > e.fh || e.w < e.fminw || e.h < e.fminh) {
        detail = std::format("input {}x{}, feature {}x{} (smallest {}x{})", e.w, e.h, e.fw, e.fh, e.fminw, e.fminh);
        return kChkFeature;
    }
    if (!rect_in(e.color, e.x, e.y, e.w, e.h, d)) {
        detail = std::format("colour {} for the rectangle {},{} {}x{}", res_text(e.color), e.x, e.y, e.w, e.h);
        return kChkColor;
    }
    const DXGI_FORMAT cf = d.Format;
    const bool color_ok = e.upscale ? (cf == DXGI_FORMAT_R10G10B10A2_UNORM || cf == DXGI_FORMAT_R10G10B10A2_TYPELESS)
                                    : (cf == DXGI_FORMAT_R16G16B16A16_FLOAT || cf == DXGI_FORMAT_R16G16B16A16_TYPELESS || cf == DXGI_FORMAT_R11G11B10_FLOAT ||
                                       cf == DXGI_FORMAT_R32G32B32A32_FLOAT);
    if (!color_ok) {
        detail = std::format("colour format {}", static_cast<int>(cf));
        return kChkColor;
    }
    if (!rect_in(e.depth, e.dx, e.dy, e.w, e.h, d) ||
        (d.Format != DXGI_FORMAT_R32G8X24_TYPELESS && d.Format != DXGI_FORMAT_R24G8_TYPELESS && d.Format != DXGI_FORMAT_R32_TYPELESS &&
         d.Format != DXGI_FORMAT_R32_FLOAT && d.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT && d.Format != DXGI_FORMAT_D24_UNORM_S8_UINT &&
         d.Format != DXGI_FORMAT_D32_FLOAT)) {
        detail = std::format("depth {} for the rectangle {},{} {}x{}", res_text(e.depth), e.dx, e.dy, e.w, e.h);
        return kChkDepth;
    }
    if (!rect_in(e.mv, e.mx, e.my, e.w, e.h, d) || d.Format != DXGI_FORMAT_R16G16_FLOAT) {
        detail = std::format("motion vectors {} for the rectangle {},{} {}x{}", res_text(e.mv), e.mx, e.my, e.w, e.h);
        return kChkMv;
    }
    if (!rect_in(e.out, e.ox, e.oy, e.fow, e.foh, d) || !(d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ||
        (d.Format != DXGI_FORMAT_R10G10B10A2_UNORM && d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format != DXGI_FORMAT_R32G32B32A32_FLOAT &&
         d.Format != DXGI_FORMAT_R11G11B10_FLOAT)) {
        detail = std::format("output {} for the rectangle {},{} {}x{}", res_text(e.out), e.ox, e.oy, e.fow, e.foh);
        return kChkOutput;
    }
    if (!finite(e.jx) || !finite(e.jy) || std::abs(e.jx) > 1.0f || std::abs(e.jy) > 1.0f) {
        detail = std::format("jitter {} {}", e.jx, e.jy);
        return kChkJitter;
    }
    return -1;
}

// Records the evaluation's inputs and checks them: false if the evaluation must not be made.
bool before_eval(const EvalInputs& e) {
    if (g_last_depth[e.eye] && g_last_depth[e.eye] != e.depth) {
        ++g_depth_changes;
        event(std::format("eye {}: depth texture changed from {} to {}", e.eye, static_cast<void*>(g_last_depth[e.eye]), res_text(e.depth)));
    }
    g_last_depth[e.eye] = e.depth;
    event(std::format("eval eye {} handle {} feature {}x{} (min {}x{}) -> {}x{} | in {},{} {}x{} | colour {} | depth {} | mv {} | out {} at {},{} | "
                      "jitter {:.4f} {:.4f} | reset {}",
                      e.eye, static_cast<void*>(e.handle), e.fw, e.fh, e.fminw, e.fminh, e.fow, e.foh, e.x, e.y, e.w, e.h, res_text(e.color),
                      res_text(e.depth), res_text(e.mv), res_text(e.out), e.ox, e.oy, e.jx, e.jy, e.reset ? 1 : 0));
    if (!g_set.validate.load(std::memory_order_relaxed)) return true;
    std::string detail;
    const int bad = check_eval(e, detail);
    if (bad < 0) return true;
    const std::uint64_t n = ++g_check_fail[bad];
    event(std::format("CHECK FAILED eye {}: {}: {}", e.eye, kCheckNames[bad], detail));
    if (n == 1 || (n & (n - 1)) == 0) log::warn("dlss: eye {}: check '{}' failed ({} times): {}; the game's pass runs instead", e.eye, kCheckNames[bad], n, detail);
    return false;
}

std::atomic<int> g_ngx_log_lines{0};
void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!g_set.log_ngx.load(std::memory_order_relaxed) || !message) return;
    const int cap = g_set.log_lines > 0 ? g_set.log_lines : (g_set.log_verbose ? 20000 : 400);
    if (g_ngx_log_lines.fetch_add(1) > cap) return;
    std::string m(message);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    log::info("ngx: {}", m);
    // Where the DLSS model really came from (the driver can override the application's DLL).
    const std::size_t at = m.find("Loaded from path");
    if (at != std::string::npos) {
        std::lock_guard lock(g_info_mutex);
        g_info.dll = m.substr(at);
    }
}

std::string result_text(NVSDK_NGX_Result r) {
    return std::format("0x{:08x}{}", static_cast<unsigned>(r), NVSDK_NGX_SUCCEED(r) ? " (ok)" : "");
}

std::string loaded_dll() {
    HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
    if (!m) return "nvngx_dlss.dll not loaded";
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(m, path, MAX_PATH);
    return std::format("{} version {}", log::narrow(path), module::file_version(path));
}

void query_optimal(NVSDK_NGX_Parameter* p, std::string& out) {
    struct Mode {
        const char* name;
        NVSDK_NGX_PerfQuality_Value v;
    };
    const Mode modes[] = {{"DLAA", NVSDK_NGX_PerfQuality_Value_DLAA},
                          {"Quality", NVSDK_NGX_PerfQuality_Value_MaxQuality},
                          {"Balanced", NVSDK_NGX_PerfQuality_Value_Balanced},
                          {"Performance", NVSDK_NGX_PerfQuality_Value_MaxPerf},
                          {"UltraPerformance", NVSDK_NGX_PerfQuality_Value_UltraPerformance}};
    const UINT sizes[][2] = {{3072, 3264}, {3600, 3600}};
    for (const auto& s : sizes) {
        out += std::format(" | output {}x{}:", s[0], s[1]);
        for (const Mode& m : modes) {
            unsigned ow = 0, oh = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
            float sharp = 0;
            const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(p, s[0], s[1], m.v, &ow, &oh, &maxw, &maxh, &minw, &minh, &sharp);
            if (NVSDK_NGX_SUCCEED(r)) out += std::format(" {} {}x{} (min {}x{});", m.name, ow, oh, minw, minh);
            else out += std::format(" {} {};", m.name, result_text(r));
        }
    }
}

bool init_ngx(ID3D11Device* dev) {
    Rhi& R = *g_rhi;
    if (R.ngx_state != 0) return R.ngx_state > 0;
    R.ngx_state = -1;
    R.dev = dev;
    std::vector<std::wstring> paths;
    if (!g_extra_dll_dir.empty()) paths.push_back(g_extra_dll_dir);
    if (!g_dll_dir.empty()) paths.push_back(g_dll_dir.wstring());
    std::vector<const wchar_t*> path_ptrs;
    for (const auto& s : paths) path_ptrs.push_back(s.c_str());
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = path_ptrs.empty() ? nullptr : path_ptrs.data();
    info.PathListInfo.Length = static_cast<unsigned>(path_ptrs.size());
    info.LoggingInfo.LoggingCallback = &ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = g_set.log_verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;
    // NGX keeps its own data (logs, cache) here; a per-user temporary folder, so nothing is
    // written next to the game.
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::filesystem::path data = std::filesystem::path(tmp) / L"ff7vr-ngx";
    std::error_code ec;
    std::filesystem::create_directories(data, ec);
    const auto t0 = std::chrono::steady_clock::now();
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                                   data.wstring().c_str(), dev, &info, NVSDK_NGX_Version_API);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string ngx = std::format("NVSDK_NGX_D3D11_Init_with_ProjectID {} in {:.1f} ms", result_text(r), ms);
    log::info("dlss: {}", ngx);
    if (NVSDK_NGX_FAILED(r)) {
        with_info([&](Info& i) { i.ngx = ngx; });
        return false;
    }
    NVSDK_NGX_Parameter* p = nullptr;
    const NVSDK_NGX_Result rp = NVSDK_NGX_D3D11_GetCapabilityParameters(&p);
    std::string cap;
    if (NVSDK_NGX_FAILED(rp) || !p) {
        cap = std::format("GetCapabilityParameters {}", result_text(rp));
        log::warn("dlss: {}", cap);
        with_info([&](Info& i) {
            i.ngx = ngx;
            i.capability = cap;
        });
        return false;
    }
    int available = 0, needs_driver = 0;
    unsigned min_major = 0, min_minor = 0;
    int init_result = 0;
    NVSDK_NGX_Parameter_GetI(p, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(p, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_major);
    NVSDK_NGX_Parameter_GetUI(p, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_minor);
    NVSDK_NGX_Parameter_GetI(p, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
    cap = std::format("SuperSampling.Available {}, NeedsUpdatedDriver {}, MinDriverVersion {}.{}, FeatureInitResult 0x{:08x}", available,
                      needs_driver, min_major, min_minor, static_cast<unsigned>(init_result));
    std::string optimal;
    if (available) query_optimal(p, optimal);
    const std::string dll = loaded_dll();
    log::info("dlss: capability: {}", cap);
    log::info("dlss: optimal render sizes:{}", optimal);
    log::info("dlss: feature library: {}", dll);
    with_info([&](Info& i) {
        i.ngx = ngx;
        i.capability = cap;
        i.optimal = optimal;
        if (i.dll.empty() || dll.find("not loaded") == std::string::npos) i.dll = dll;
    });
    if (!available) return false;
    R.params = p;
    R.ngx_state = 1;
    return true;
}

// ------------------------------------------------------------------ deferred release (RHI thread)
// A feature, and every texture an evaluation reads or writes, is released only after the GPU
// has finished the commands that use it. The SDK guide (section 5.5, ReleaseFeature): "A
// feature handle should only be released once the command lists that were used in Evaluate
// calls are no longer in flight", because the commands can still reference the feature's
// internal state and resources; NGX also keeps a released feature's memory for the next
// creation, so releasing while an evaluation runs hands memory still in use to the new
// feature. Releasing at once is what happened at every size change, preset change and switch
// off until now (a GPU fault came 20 ms after one such release, docs/dlss.md "GPU faults").
// Things retired during a frame wait for an event query issued at that frame's end, after
// every command that used them, and for at least kRetireFrames more frames.
constexpr std::uint64_t kRetireFrames = 3;
constexpr std::uint64_t kRetireForceFrames = 900;  // released anyway if the query never answers
struct RetireBatch {
    std::vector<NVSDK_NGX_Handle*> features;
    std::vector<NVSDK_NGX_Parameter*> params;  // the features' own parameter maps, destroyed after them
    std::vector<ComPtr<IUnknown>> objects;
    ComPtr<ID3D11Query> fence;
    std::uint64_t frame = 0;
};
RetireBatch g_retire_open;               // collected during the current frame
std::deque<RetireBatch> g_retire_queue;  // waiting for their fence
std::atomic<std::uint64_t> g_retired_features{0}, g_released_features{0}, g_retire_forced{0}, g_retire_waiting{0};
std::atomic<std::uint64_t> g_stalls{0};

void retire_feature(Feature& f) {
    if (f.handle) {
        g_retire_open.features.push_back(f.handle);
        ++g_retired_features;
        event(std::format("feature {} retired ({}x{} -> {}x{})", static_cast<void*>(f.handle), f.w, f.h, f.ow, f.oh));
    }
    if (f.own_params && f.params) g_retire_open.params.push_back(f.params);
    f = Feature{};
}

// Keeps a reference until the GPU is past this frame (textures replaced by new ones, and the
// engine's textures an evaluation of this frame reads).
void retire_object(IUnknown* p) {
    if (p) g_retire_open.objects.emplace_back(p);
}
template <class T>
void retire(ComPtr<T>& p) {
    retire_object(p.Get());
    p.Reset();
}

// Frame end: fence what was retired this frame, release what the GPU is done with.
void retire_frame_end(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::uint64_t frame) {
    if (!g_retire_open.features.empty() || !g_retire_open.objects.empty() || !g_retire_open.params.empty()) {
        RetireBatch b = std::move(g_retire_open);
        g_retire_open = RetireBatch{};
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(dev->CreateQuery(&qd, &b.fence))) ctx->End(b.fence.Get());
        b.frame = frame;
        g_retire_queue.push_back(std::move(b));
    }
    while (!g_retire_queue.empty()) {
        RetireBatch& b = g_retire_queue.front();
        BOOL done = FALSE;
        const bool signalled =
            b.fence && ctx->GetData(b.fence.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done;
        const bool forced = frame >= b.frame + kRetireForceFrames;
        if (!(signalled && frame >= b.frame + kRetireFrames) && !forced) break;
        if (!signalled) {
            ++g_retire_forced;
            log::warn("dlss: {} features and {} objects retired at frame {} released without their GPU fence", b.features.size(), b.objects.size(),
                      b.frame);
        }
        for (NVSDK_NGX_Handle* h : b.features) {
            NVSDK_NGX_D3D11_ReleaseFeature(h);
            ++g_released_features;
            event(std::format("feature {} released", static_cast<void*>(h)));
        }
        for (NVSDK_NGX_Parameter* p : b.params) NVSDK_NGX_D3D11_DestroyParameters(p);
        g_retire_queue.pop_front();
    }
    std::uint64_t waiting = g_retire_open.features.size();
    for (const RetireBatch& b : g_retire_queue) waiting += b.features.size();
    g_retire_waiting = waiting;
}

int wanted_flags(bool hdr = true) {
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (hdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (g_set.auto_exposure.load()) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (g_set.mv_jitter.load() == 1) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    return flags;
}

NVSDK_NGX_PerfQuality_Value quality_for(UINT in_w, UINT out_w) {
    const double ratio = static_cast<double>(in_w) / static_cast<double>(out_w);
    return in_w >= out_w     ? NVSDK_NGX_PerfQuality_Value_DLAA
           : ratio >= 0.66   ? NVSDK_NGX_PerfQuality_Value_MaxQuality
           : ratio >= 0.57   ? NVSDK_NGX_PerfQuality_Value_Balanced
           : ratio >= 0.49   ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                             : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

// The input sizes NGX accepts for an output size and mode (NGX_DLSS_GET_OPTIMAL_SETTINGS),
// remembered per output size and mode.
struct DynRange {
    bool ok = false;
    UINT min_w = 0, min_h = 0, max_w = 0, max_h = 0;
};
DynRange dynamic_range(UINT ow, UINT oh, NVSDK_NGX_PerfQuality_Value q) {
    static std::unordered_map<std::uint64_t, DynRange> cache;  // RHI thread
    const std::uint64_t key = (static_cast<std::uint64_t>(ow) << 40) | (static_cast<std::uint64_t>(oh) << 16) | static_cast<std::uint64_t>(q);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    DynRange d;
    unsigned optw = 0, opth = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(g_rhi->params, ow, oh, q, &optw, &opth, &maxw, &maxh, &minw, &minh, &sharp);
    if (NVSDK_NGX_SUCCEED(r)) d = DynRange{true, minw, minh, maxw, maxh};
    log::info("dlss: input range for output {}x{} mode {}: {} (optimal {}x{}, min {}x{}, max {}x{})", ow, oh, static_cast<int>(q), result_text(r), optw,
              opth, minw, minh, maxw, maxh);
    cache[key] = d;
    return d;
}

// One feature per eye: input w x h, output ow x oh (equal for DLAA), HDR or display-referred input.
bool ensure_feature(ID3D11DeviceContext* ctx, int eye, UINT w, UINT h, bool& created, UINT ow = 0, UINT oh = 0, bool hdr = true,
                    bool dynamic = false, UINT min_w = 0, UINT min_h = 0, int quality = -1) {
    Rhi& R = *g_rhi;
    Feature& f = R.feat[eye];
    if (!ow) ow = w;
    if (!oh) oh = h;
    const unsigned preset = g_set.preset.load();
    const int flags = wanted_flags(hdr);
    created = false;
    const bool own = g_set.own_params.load();
    const bool subrects = !(g_set.copy_inputs.load() && g_set.mode.load() == 1);  // the copies exist in upscale mode only
    const NVSDK_NGX_PerfQuality_Value q = quality >= 0 ? static_cast<NVSDK_NGX_PerfQuality_Value>(quality) : quality_for(w, ow);
    if (f.handle && f.w == w && f.h == h && f.ow == ow && f.oh == oh && f.preset == preset && f.flags == flags && f.dynamic == dynamic &&
        f.own_params == own && f.subrects == subrects && f.quality == static_cast<int>(q))
        return true;
    retire_feature(f);
    NVSDK_NGX_Parameter* params = R.params;
    if (own) {
        params = nullptr;
        const NVSDK_NGX_Result ra = NVSDK_NGX_D3D11_AllocateParameters(&params);
        if (NVSDK_NGX_FAILED(ra) || !params) {
            log::warn("dlss: NVSDK_NGX_D3D11_AllocateParameters {}", result_text(ra));
            return false;
        }
    }
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, preset);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = w;
    cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = ow;
    cp.Feature.InTargetHeight = oh;
    cp.Feature.InPerfQualityValue = q;
    cp.InFeatureCreateFlags = flags;
    cp.InEnableOutputSubrects = subrects;
    const auto t0 = std::chrono::steady_clock::now();
    g_in_ngx = true;
    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &f.handle, params, &cp);
    g_in_ngx = false;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string text = std::format("eye {} {}x{} -> {}x{} quality value {} preset {} flags 0x{:x} input {} parameters {}: create {} in {:.1f} ms | {}", eye,
                                   w, h, ow, oh, static_cast<int>(cp.Feature.InPerfQualityValue), preset_name(preset), flags,
                                   dynamic ? std::format("{}x{} to {}x{} (dynamic)", min_w, min_h, w, h) : std::string("this size only"),
                                   own ? "own" : "shared", result_text(r), ms, vram_text(R.dev));
    log::info("dlss: feature {}", text);
    event(std::format("feature {} created: {}", static_cast<void*>(f.handle), text));
    with_info([&](Info& i) {
        i.features[eye] = text;
        ++i.creates;
        if (i.dll.empty() || i.dll.find("not loaded") != std::string::npos) i.dll = loaded_dll();
    });
    if (NVSDK_NGX_FAILED(r) || !f.handle) {
        if (own) NVSDK_NGX_D3D11_DestroyParameters(params);
        f = Feature{};
        return false;
    }
    f.params = params;
    f.own_params = own;
    f.subrects = subrects;
    f.quality = static_cast<int>(q);
    f.w = w;
    f.h = h;
    f.ow = ow;
    f.oh = oh;
    f.preset = preset;
    f.flags = flags;
    f.dynamic = dynamic;
    f.min_w = dynamic ? min_w : w;
    f.min_h = dynamic ? min_h : h;
    created = true;
    static bool dll_logged = false;
    if (!dll_logged) {
        dll_logged = true;
        log::info("dlss: feature library after the first feature: {}", loaded_dll());
    }
    return true;
}

// ------------------------------------------------------------------ motion vector pass
constexpr char kShader[] = R"(
cbuffer EyePass : register(b0) { float4 Rect; float4 Opt; float4 Opt2; float4 Opt3; };   // eye rect x y w h; 1/w 1/h, remove-jitter, zero (test); sanitize, limit; Opt3.xy: offset of the drawn pixels from the engine's (drawn at the origin of the eye's own texture)
cbuffer View : register(b1) { float4 V[146]; };
Texture2D<float> Depth : register(t0);
Texture2D<float2> Velocity : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 ps(float4 pos : SV_Position) : SV_Target {
    if (Opt.w > 0.5) return float2(0, 0);
    float2 q = pos.xy + Opt3.xy;   // the pixel in the engine's buffers
    int3 p = int3(q, 0);
    float2 uv = (q - Rect.xy) * Opt.xy;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float d = Depth.Load(p);
    float4 prev = ndc.x * V[114] + ndc.y * V[115] + d * V[116] + V[117];
    float2 delta = ndc - prev.xy / prev.w;
    float2 vel = Velocity.Load(p);
    if (vel.x > 0) delta = (vel - 32767.0 / 65535.0) / (0.499 * 0.5);
    if (Opt.z > 0.5) delta -= V[118].xy - V[118].zw;
    float2 mv = -delta * float2(0.5, -0.5) * Rect.zw;
    if (Opt2.x > 0.5) {
        if ((asuint(mv.x) & 0x7f800000u) == 0x7f800000u || (asuint(mv.y) & 0x7f800000u) == 0x7f800000u) mv = float2(0, 0);
        mv = clamp(mv, -Opt2.yy, Opt2.yy);
    }
    return mv;
}
// The eye's rectangle of the depth (Rect.xy its origin) into a texture of its own at the origin.
float psdepth(float4 pos : SV_Position) : SV_Target {
    if (Opt.y > 0) return Opt.y;   // test: constant depth
    return Depth.Load(int3(pos.xy + Rect.xy, 0));
}
// The same for the colour, each channel raised to at least Opt2.z, plus per-pixel noise of
// amplitude Opt2.w that changes every frame (Opt.x) (tests: no black or no constant input).
Texture2D<float4> Src : register(t2);
float4 pscolor(float4 pos : SV_Position) : SV_Target {
    float4 c = max(Src.Load(int3(pos.xy + Rect.xy, 0)), Opt2.zzzz);
    uint h = (uint(pos.x) * 73856093u) ^ (uint(pos.y) * 19349663u) ^ (uint(Opt.x) * 83492791u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return c + Opt2.w * float(h & 1023u) / 1023.0;
}
)";

struct PassConsts {
    float rect[4];
    float opt[4];
    float opt2[4];
    float opt3[4];
};

// Statistics of an evaluation's inputs, computed on the GPU over the input rectangle and read
// back a few frames later: motion vectors that are not finite or larger than the limit, the
// largest motion vector, depth that is not finite or outside 0..1, far-plane depth, and (with
// HDR colour input) colour that is not finite and the brightest channel.
constexpr char kStatsShader[] = R"(
cbuffer S : register(b0) { uint4 R; uint4 O; };   // R: rectangle x y w h; O.x: colour bound, O.y: motion vector limit (float bits)
Texture2D<float2> Mv : register(t0);
Texture2D<float> Dp : register(t1);
Texture2D<float4> Co : register(t2);
RWByteAddressBuffer Out : register(u0);
groupshared uint gs[10];
bool nf(float v) { return (asuint(v) & 0x7f800000u) == 0x7f800000u; }
[numthreads(16, 16, 1)]
void cs(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 10) gs[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint k;
    if (id.x < R.z && id.y < R.w) {
        int3 p = int3(R.x + id.x, R.y + id.y, 0);
        float2 mv = Mv.Load(p);
        if (nf(mv.x) || nf(mv.y)) InterlockedAdd(gs[0], 1u, k);
        else {
            float m = max(abs(mv.x), abs(mv.y));
            if (m > asfloat(O.y)) InterlockedAdd(gs[1], 1u, k);
            InterlockedMax(gs[2], asuint(m) & 0x7fffffffu, k);
        }
        float d = Dp.Load(p);
        if (nf(d)) InterlockedAdd(gs[3], 1u, k);
        else if (d < 0 || d > 1) InterlockedAdd(gs[4], 1u, k);
        else if (d == 0) InterlockedAdd(gs[7], 1u, k);
        if (!nf(d) && d >= 0.999) InterlockedAdd(gs[8], 1u, k);
        if (!nf(d)) InterlockedMax(gs[9], asuint(max(d, 0)) & 0x7fffffffu, k);
        if (O.x == 1) {
            float4 c = Co.Load(p);
            if (nf(c.r) || nf(c.g) || nf(c.b) || nf(c.a)) InterlockedAdd(gs[5], 1u, k);
            else InterlockedMax(gs[6], asuint(max(max(c.r, c.g), max(c.b, 0))) & 0x7fffffffu, k);
        } else if (O.x == 2) {   // display-referred colour: count black pixels
            float4 c = Co.Load(p);
            float m = max(max(c.r, c.g), max(c.b, 0));
            if (m < 1.0 / 1023.0) InterlockedAdd(gs[5], 1u, k);
            InterlockedMax(gs[6], asuint(m) & 0x7fffffffu, k);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        Out.InterlockedAdd(0, gs[0], k);
        Out.InterlockedAdd(4, gs[1], k);
        Out.InterlockedMax(8, gs[2], k);
        Out.InterlockedAdd(12, gs[3], k);
        Out.InterlockedAdd(16, gs[4], k);
        Out.InterlockedAdd(20, gs[5], k);
        Out.InterlockedMax(24, gs[6], k);
        Out.InterlockedAdd(28, gs[7], k);
        Out.InterlockedAdd(32, gs[8], k);
        Out.InterlockedMax(36, gs[9], k);
    }
}
)";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                      ID3DBlob**);

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out, const char* src = kShader, std::size_t len = sizeof(kShader) - 1) {
    static D3DCompileFn fn = [] {
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<D3DCompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    if (!fn) return false;
    ComPtr<ID3DBlob> err;
    const HRESULT hr =
        fn(src, len, "dlss_mv", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &out, &err);
    if (FAILED(hr)) {
        log::error("dlss: shader {} failed: {}", entry, err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "?");
        return false;
    }
    return true;
}

bool ensure_shaders(ID3D11Device* dev) {
    Rhi& R = *g_rhi;
    if (R.ps) return true;
    if (R.shaders_failed) return false;
    R.shaders_failed = true;
    ComPtr<ID3DBlob> vsb, psb;
    if (!compile("vs", "vs_5_0", vsb) || !compile("ps", "ps_5_0", psb)) return false;
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &R.vs))) return false;
    if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &R.ps))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(PassConsts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &R.cb))) return false;
    ComPtr<ID3D11Device1> dev1;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev1)))) return false;
    const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    D3D_FEATURE_LEVEL chosen{};
    if (FAILED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &R.state))) return false;
    R.shaders_failed = false;
    log::info("dlss: motion vector shaders and context state ready");
    return true;
}

// The texture an eye's motion vectors are drawn into and DLSS reads them from. By default
// ([dlss] mv_textures = eye) each eye has a texture of its own, the input's size, with the
// motion vectors at its origin. DLSS does not read motion vectors at a sub-rectangle base
// other than 0 correctly: with the right eye's vectors at the right half of a shared
// double-wide texture (base x = eye width) the right eye's output lost its temporal
// stability whenever the head moved (docs/dlss.md, "The right eye's shimmer"); the left
// eye, at base 0, was fine. mv_textures = shared keeps that layout for comparison.
struct MvTarget {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT bx = 0, by = 0;     // where the eye's rectangle is in it (the sub-rectangle base for DLSS)
    bool at_origin = false;  // drawn at the origin (the eye's own texture)
};

bool ensure_eye_mv(ID3D11Device* dev, int eye, UINT w, UINT h) {
    Rhi& R = *g_rhi;
    if (R.mv_eye[eye] && R.mv_eye_desc[eye].Width >= w && R.mv_eye_desc[eye].Height >= h) return true;
    retire(R.mv_eye[eye]);
    retire(R.mv_eye_rtv[eye]);
    retire(R.mv_eye_srv[eye]);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &R.mv_eye[eye])) || FAILED(dev->CreateRenderTargetView(R.mv_eye[eye].Get(), nullptr, &R.mv_eye_rtv[eye])) ||
        FAILED(dev->CreateShaderResourceView(R.mv_eye[eye].Get(), nullptr, &R.mv_eye_srv[eye]))) {
        R.mv_eye[eye].Reset();
        return false;
    }
    R.mv_eye_desc[eye] = d;
    log::info("dlss: eye {} motion vector texture {}x{} R16G16_FLOAT (vectors at its origin)", eye, w, h);
    return true;
}

// The motion vector target of an eye whose rectangle is x,y w x h in the engine's buffers.
bool mv_target(int eye, UINT x, UINT y, UINT w, UINT h, MvTarget& t) {
    Rhi& R = *g_rhi;
    if (g_set.mv_per_eye.load(std::memory_order_relaxed)) {
        if (!ensure_eye_mv(R.dev, eye, w, h)) return false;
        t = MvTarget{R.mv_eye[eye].Get(), R.mv_eye_rtv[eye].Get(), R.mv_eye_srv[eye].Get(), 0, 0, true};
        return true;
    }
    if (!R.mv) return false;
    t = MvTarget{R.mv.Get(), R.mv_rtv.Get(), R.mv_srv.Get(), x, y, false};
    return true;
}

bool ensure_textures(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& color, DXGI_FORMAT out_format, bool need_out = true) {
    Rhi& R = *g_rhi;
    if (g_set.mv_per_eye.load(std::memory_order_relaxed)) {
        // The eyes' own motion vector textures are made by mv_target.
    } else if (!R.mv || R.mv_desc.Width != color.Width || R.mv_desc.Height != color.Height) {
        retire(R.mv);
        retire(R.mv_rtv);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = color.Width;
        d.Height = color.Height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        retire(R.mv_srv);
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &R.mv)) || FAILED(dev->CreateRenderTargetView(R.mv.Get(), nullptr, &R.mv_rtv)) ||
            FAILED(dev->CreateShaderResourceView(R.mv.Get(), nullptr, &R.mv_srv))) {
            R.mv.Reset();
            return false;
        }
        R.mv_desc = d;
        log::info("dlss: motion vector texture {}x{} R16G16_FLOAT", d.Width, d.Height);
    }
    if (need_out && (!R.out || R.out_desc.Width != color.Width || R.out_desc.Height != color.Height || R.out_desc.Format != out_format)) {
        retire(R.out);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = color.Width;
        d.Height = color.Height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = out_format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &R.out))) return false;
        R.out_desc = d;
        log::info("dlss: output texture {}x{} format {}", d.Width, d.Height, static_cast<int>(d.Format));
    }
    return true;
}

// ------------------------------------------------------------------ input statistics (RHI thread)
struct StatsSlot {
    ComPtr<ID3D11Buffer> staging;
    std::uint64_t frame = 0;
    int eye = 0;
    bool pending = false;
};
struct Stats {
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer> cb, buf;
    ComPtr<ID3D11UnorderedAccessView> uav;
    std::array<StatsSlot, 8> slots;
    unsigned next = 0;
    bool failed = false;
    // Totals over all evaluations read back (pipe thread reads them under g_info_mutex via status)
    std::uint64_t read = 0, mv_nonfinite = 0, mv_huge = 0, depth_nonfinite = 0, depth_out = 0, color_nonfinite = 0, frames_bad = 0;
    float mv_max = 0, color_max = 0;
    float last_far_share[2]{};
};
Stats g_stats;
std::mutex g_stats_mutex;  // the totals

bool ensure_stats(ID3D11Device* dev) {
    Stats& S = g_stats;
    if (S.cs) return true;
    if (S.failed) return false;
    S.failed = true;
    ComPtr<ID3DBlob> b;
    if (!compile("cs", "cs_5_0", b, kStatsShader, sizeof(kStatsShader) - 1)) return false;
    if (FAILED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &S.cs))) return false;
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 32;
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cd, nullptr, &S.cb))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &S.buf))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 12;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(dev->CreateUnorderedAccessView(S.buf.Get(), &ud, &S.uav))) return false;
    for (StatsSlot& s : S.slots) {
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = 48;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &s.staging))) return false;
    }
    S.failed = false;
    log::info("dlss: input statistics shader ready");
    return true;
}

// Inside our own pipeline state, before the evaluation: statistics of the inputs over the
// input rectangle. color: an HDR colour view (DLAA mode) or null.
void run_stats(ID3D11DeviceContext* ctx, int eye, ID3D11ShaderResourceView* mv, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* color, bool ldr,
               UINT x, UINT y, UINT w, UINT h) {
    if (!g_set.input_stats.load(std::memory_order_relaxed) || !mv || !depth || !ensure_stats(g_rhi->dev)) return;
    Stats& S = g_stats;
    StatsSlot& slot = S.slots[S.next];
    if (slot.pending) return;  // not read back yet: skip this one
    S.next = (S.next + 1) % S.slots.size();
    const float limit = static_cast<float>(std::max(w, h));
    std::uint32_t c[8] = {x, y, w, h, color ? (ldr ? 2u : 1u) : 0u, std::bit_cast<std::uint32_t>(limit), 0, 0};
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(S.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, c, sizeof(c));
    ctx->Unmap(S.cb.Get(), 0);
    const UINT zero[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(S.uav.Get(), zero);
    ID3D11ShaderResourceView* srvs[3] = {mv, depth, color};
    ID3D11UnorderedAccessView* uav = S.uav.Get();
    ID3D11Buffer* cb = S.cb.Get();
    ctx->CSSetShader(S.cs.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 3, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    ID3D11ShaderResourceView* none_srv[3]{};
    ID3D11UnorderedAccessView* none_uav = nullptr;
    ctx->CSSetShaderResources(0, 3, none_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &none_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->CopyResource(slot.staging.Get(), S.buf.Get());
    slot.frame = g_rhi->frame;
    slot.eye = eye;
    slot.pending = true;
}

// Frame end: statistics the GPU has finished.
void collect_stats(ID3D11DeviceContext* ctx) {
    Stats& S = g_stats;
    for (StatsSlot& s : S.slots) {
        if (!s.pending || s.frame + 2 > g_rhi->frame) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        std::uint32_t v[10];
        std::memcpy(v, m.pData, sizeof(v));
        ctx->Unmap(s.staging.Get(), 0);
        s.pending = false;
        const float mv_max = std::bit_cast<float>(v[2]), col_max = std::bit_cast<float>(v[6]);
        const bool bad = v[0] || v[1] || v[3] || v[4];
        {
            std::lock_guard lock(g_stats_mutex);
            ++S.read;
            S.mv_nonfinite += v[0];
            S.mv_huge += v[1];
            S.depth_nonfinite += v[3];
            S.depth_out += v[4];
            S.color_nonfinite += v[5];
            S.mv_max = std::max(S.mv_max, mv_max);
            S.color_max = std::max(S.color_max, col_max);
            if (bad) ++S.frames_bad;
        }
        event(std::format("stats eye {} of frame {}: mv non-finite {} huge {} max {:.1f} px | depth non-finite {} outside 0..1 {} far {} near (>= 0.999) {} max {:.6g} | colour non-finite (HDR) or black (display-referred) {} max {:.3g}",
                          s.eye, s.frame, v[0], v[1], mv_max, v[3], v[4], v[7], v[8], std::bit_cast<float>(v[9]), v[5], col_max));
        static int logged = 0;
        if (bad && logged++ < 20)
            log::info("dlss: input statistics eye {} frame {}: motion vectors non-finite {} larger than the input {} (largest {:.1f} px), depth non-finite {} "
                      "outside 0..1 {}, colour non-finite {}",
                      s.eye, s.frame, v[0], v[1], mv_max, v[3], v[4], v[5]);
    }
}

// Video memory of the process's adapter (local segment): budget and current usage, in MB.
std::string vram_text(ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dd;
    ComPtr<IDXGIAdapter> a;
    ComPtr<IDXGIAdapter3> a3;
    if (!dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&dd))) || FAILED(dd->GetAdapter(&a)) || FAILED(a.As(&a3))) return "video memory: ?";
    DXGI_QUERY_VIDEO_MEMORY_INFO l{}, n{};
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l);
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n);
    unsigned long long dlss_bytes = 0;
    if (g_rhi->params) NGX_DLSS_GET_STATS(g_rhi->params, &dlss_bytes);
    return std::format("video memory: local used {} of budget {} MB (reserved {}), non-local used {} MB, DLSS {} MB", l.CurrentUsage >> 20, l.Budget >> 20,
                       l.CurrentReservation >> 20, n.CurrentUsage >> 20, dlss_bytes >> 20);
}

// ------------------------------------------------------------------ camera cut test (dev command)
// `dlss cuttest <path prefix> <reset 0|1> <zero_mv 0|1>`: at the next camera cut, with or
// without the history reset and with the motion vectors of that frame set to zero or not
// (zero is what the engine's own cuts give), the left eye's upscaled image (a centred crop of
// at most 2048x2048) of the frames 0, 1, 2, 3, 5, 10 and 60 after the cut is written to
// <prefix>_f<n>_<w>x<h>.r10g10b10a2 (raw R10G10B10A2 rows, read back without a stall).
constexpr int kCutTestOffsets[7] = {0, 1, 2, 3, 5, 10, 60};
struct CutTest {
    std::mutex m;
    std::string prefix;            // under m
    std::atomic<bool> armed{false};
    std::atomic<bool> want_reset{true}, want_zero{false};
    // RHI thread
    std::uint64_t cut_frame = 0;
    bool reset = true, zero_mv = false;
    struct Slot {
        ComPtr<ID3D11Texture2D> staging;
        std::uint64_t frame = 0;
        UINT w = 0, h = 0;
        bool used = false, pending = false;
    } slots[7];
    int written = 0;
};
CutTest g_cuttest;

void cuttest_on_pass(unsigned why, std::uint64_t frame, bool& zero_mv, bool& allow_reset) {
    CutTest& t = g_cuttest;
    zero_mv = false;
    allow_reset = true;
    if (!t.armed.load()) return;
    if (t.cut_frame == 0 && why) {
        t.cut_frame = frame;
        t.reset = t.want_reset.load();
        t.zero_mv = t.want_zero.load();
        for (auto& s : t.slots) s = CutTest::Slot{};
        t.written = 0;
        log::info("dlss: cut test: cut at frame {} ({}), reset {}, motion vectors {}", frame, cut_text(why), t.reset ? "on" : "off",
                  t.zero_mv ? "zero" : "as computed");
    }
    if (t.cut_frame == frame) {
        zero_mv = t.zero_mv;
        allow_reset = t.reset;
    }
}

void cuttest_on_output(ID3D11DeviceContext* ctx, ID3D11Texture2D* out, UINT ox, UINT oy, UINT ow, UINT oh, std::uint64_t frame) {
    CutTest& t = g_cuttest;
    if (!t.armed.load() || t.cut_frame == 0 || frame < t.cut_frame) return;
    const std::uint64_t off = frame - t.cut_frame;
    for (int i = 0; i < 7; ++i) {
        if (static_cast<std::uint64_t>(kCutTestOffsets[i]) != off || t.slots[i].used) continue;
        CutTest::Slot& s = t.slots[i];
        const UINT cw = std::min(ow, 2048u), ch = std::min(oh, 2048u);
        if (!s.staging || s.w != cw || s.h != ch) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = cw;
            d.Height = ch;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            s.staging.Reset();
            if (FAILED(g_rhi->dev->CreateTexture2D(&d, nullptr, &s.staging))) return;
            s.w = cw;
            s.h = ch;
        }
        const UINT cx = ox + (ow - cw) / 2, cy = oy + (oh - ch) / 2;
        const D3D11_BOX box{cx, cy, 0, cx + cw, cy + ch, 1};
        ctx->CopySubresourceRegion(s.staging.Get(), 0, 0, 0, 0, out, 0, &box);
        s.used = true;
        s.pending = true;
        s.frame = frame;
    }
}

// RHI thread, at the frame end: write the copies that the GPU has finished.
void cuttest_collect(ID3D11DeviceContext* ctx, std::uint64_t frame) {
    CutTest& t = g_cuttest;
    if (!t.armed.load() || t.cut_frame == 0) return;
    std::string prefix;
    {
        std::lock_guard lock(t.m);
        prefix = t.prefix;
    }
    for (int i = 0; i < 7; ++i) {
        CutTest::Slot& s = t.slots[i];
        if (!s.pending || s.frame + 2 > frame) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        const std::string path = std::format("{}_f{}_{}x{}.r10g10b10a2", prefix, kCutTestOffsets[i], s.w, s.h);
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
            for (UINT y = 0; y < s.h; ++y) fwrite(static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, 4, s.w, f);
            fclose(f);
        }
        ctx->Unmap(s.staging.Get(), 0);
        s.pending = false;
        ++t.written;
        log::info("dlss: cut test: frame {} after the cut -> {}", kCutTestOffsets[i], path);
    }
    if (t.written >= 7 || frame > t.cut_frame + 400) {
        log::info("dlss: cut test finished ({} of 7 images)", t.written);
        with_info([&](Info& i) { i.cut_test = std::format("finished: cut at frame {}, {} of 7 images, {}", t.cut_frame, t.written, prefix); });
        t.armed = false;
        t.cut_frame = 0;
        for (auto& s : t.slots) s = CutTest::Slot{};
    }
}

// ------------------------------------------------------------------ the pass
struct PassInputs {
    ComPtr<ID3D11ShaderResourceView> srv[5];
    ComPtr<ID3D11RenderTargetView> rtv[2];
    ComPtr<ID3D11Buffer> cb1;
    D3D11_VIEWPORT vp{};
    ComPtr<ID3D11Resource> depth, color, velocity, target;
    D3D11_TEXTURE2D_DESC color_desc{}, target_desc{}, depth_desc{}, velocity_desc{};
    D3D11_SHADER_RESOURCE_VIEW_DESC depth_view{}, velocity_view{};
    D3D11_RENDER_TARGET_VIEW_DESC target_view{};
};

bool tex_desc(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& d) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    return true;
}

bool is_depth_view(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || f == DXGI_FORMAT_R32_FLOAT ||
           f == DXGI_FORMAT_R16_UNORM;
}

// The temporal anti-aliasing pass of a view: t1 scene depth, t2 scene colour (float), t3 the
// history (same size and format as t2), t4 the velocity buffer (two 16-bit channels), one or
// two render targets of t2's size, constant buffer 1 (the view). Shape checks only; the
// first match fixes the pixel shader, later draws must use the same one.
bool read_pass(ID3D11DeviceContext* ctx, PassInputs& in, std::string* why) {
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps) ps->Release();  // compared only
    Rhi& R = *g_rhi;
    if (R.taa_ps && ps != R.taa_ps) return false;
    ID3D11ShaderResourceView* srv[5]{};
    ctx->PSGetShaderResources(0, 5, srv);
    for (int i = 0; i < 5; ++i) in.srv[i].Attach(srv[i]);
    if (!in.srv[1] || !in.srv[2] || !in.srv[3] || !in.srv[4]) return false;
    in.srv[1]->GetDesc(&in.depth_view);
    in.srv[4]->GetDesc(&in.velocity_view);
    if (!is_depth_view(in.depth_view.Format)) return false;
    if (in.velocity_view.Format != DXGI_FORMAT_R16G16_UNORM && in.velocity_view.Format != DXGI_FORMAT_R16G16_FLOAT) return false;
    in.srv[1]->GetResource(&in.depth);
    in.srv[2]->GetResource(&in.color);
    in.srv[4]->GetResource(&in.velocity);
    ComPtr<ID3D11Resource> hist;
    in.srv[3]->GetResource(&hist);
    D3D11_TEXTURE2D_DESC hd{};
    if (!tex_desc(in.color.Get(), in.color_desc) || !tex_desc(hist.Get(), hd) || !tex_desc(in.depth.Get(), in.depth_desc) ||
        !tex_desc(in.velocity.Get(), in.velocity_desc))
        return false;
    const DXGI_FORMAT cf = in.color_desc.Format;
    if (cf != DXGI_FORMAT_R16G16B16A16_FLOAT && cf != DXGI_FORMAT_R16G16B16A16_TYPELESS && cf != DXGI_FORMAT_R11G11B10_FLOAT &&
        cf != DXGI_FORMAT_R32G32B32A32_FLOAT)
        return false;
    ID3D11RenderTargetView* rtv[2]{};
    ctx->OMGetRenderTargets(2, rtv, nullptr);
    in.rtv[0].Attach(rtv[0]);
    in.rtv[1].Attach(rtv[1]);
    if (!in.rtv[0]) return false;
    in.rtv[0]->GetResource(&in.target);
    in.rtv[0]->GetDesc(&in.target_view);
    if (!tex_desc(in.target.Get(), in.target_desc)) return false;
    if (in.target_desc.Width != in.color_desc.Width || in.target_desc.Height != in.color_desc.Height) return false;
    if (in.depth_desc.Width != in.color_desc.Width || in.velocity_desc.Width != in.color_desc.Width) return false;
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &in.vp);
    if (nvp == 0 || in.vp.Width < 64 || in.vp.Height < 64) return false;
    ID3D11Buffer* cb[2]{};
    ctx->PSGetConstantBuffers(0, 2, cb);
    if (cb[0]) cb[0]->Release();
    in.cb1.Attach(cb[1]);
    if (!in.cb1) return false;
    if (!R.taa_ps) {
        R.taa_ps = ps;
        D3D11_BUFFER_DESC bd{};
        in.cb1->GetDesc(&bd);
        g_ub_size = bd.ByteWidth;
        const std::string text = std::format(
            "pixel shader {} | t1 depth {}x{} res fmt {} view fmt {} | t2 colour {}x{} fmt {} | t3 history fmt {} | t4 velocity {}x{} view fmt {} | rt0 "
            "{}x{} fmt {} view fmt {} | rt1 {} | vp {} {} {} {} | cb1 {} bytes",
            static_cast<void*>(ps), in.depth_desc.Width, in.depth_desc.Height, static_cast<int>(in.depth_desc.Format),
            static_cast<int>(in.depth_view.Format), in.color_desc.Width, in.color_desc.Height, static_cast<int>(cf), static_cast<int>(hd.Format),
            in.velocity_desc.Width, in.velocity_desc.Height, static_cast<int>(in.velocity_view.Format), in.target_desc.Width, in.target_desc.Height,
            static_cast<int>(in.target_desc.Format), static_cast<int>(in.target_view.Format), in.rtv[1] ? "bound" : "-", in.vp.TopLeftX,
            in.vp.TopLeftY, in.vp.Width, in.vp.Height, bd.ByteWidth);
        log::info("dlss: temporal anti-aliasing pass recognised: {}", text);
        with_info([&](Info& i) { i.pass = text; });
    }
    if (why) *why = "";
    return true;
}

DXGI_FORMAT typed_output(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
    }
}

Timing* begin_timing(ID3D11DeviceContext* ctx, int eye) {
    Rhi& R = *g_rhi;
    Timing& t = R.timing[R.timing_next];
    if (t.pending) return nullptr;  // results not read yet: skip timing this time
    if (!t.disjoint) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(R.dev->CreateQuery(&qd, &t.disjoint)) || FAILED(R.dev->CreateQuery(&qt, &t.t0)) || FAILED(R.dev->CreateQuery(&qt, &t.t1)) ||
            FAILED(R.dev->CreateQuery(&qt, &t.t2)))
            return nullptr;
    }
    R.timing_next = (R.timing_next + 1) % R.timing.size();
    t.eye = eye;
    ctx->Begin(t.disjoint);
    ctx->End(t.t0);
    return &t;
}

void collect_timing(ID3D11DeviceContext* ctx) {
    Rhi& R = *g_rhi;
    for (Timing& t : R.timing) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0, c = 0;
        if (ctx->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(t.t0, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(t.t1, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(t.t2, &c, sizeof(c), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        if (dj.Disjoint || dj.Frequency == 0) continue;
        const double total = static_cast<double>(c - a) * 1000.0 / static_cast<double>(dj.Frequency);
        const double mv = static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
        with_info([&](Info& i) {
            i.ms_sum[t.eye] += total;
            i.mv_ms_sum[t.eye] += mv;
            ++i.ms_n[t.eye];
        });
    }
}

void dump_rows(const ViewRows& rows, const PassInputs& in, int eye) {
    std::string s = std::format("eye {} vp {} {} {} {} | ", eye, in.vp.TopLeftX, in.vp.TopLeftY, in.vp.Width, in.vp.Height);
    for (int r = 0; r < kRowCount; ++r)
        s += std::format("[{}] {:.6g} {:.6g} {:.6g} {:.6g}  ", kRowFirst + r, rows.v[r][0], rows.v[r][1], rows.v[r][2], rows.v[r][3]);
    log::info("dlss: view rows {}", s);
    with_info([&](Info& i) { i.dump = s; });
}

struct MvTarget;
void draw_motion_vectors(ID3D11DeviceContext* ctx, const PassInputs& in, const MvTarget& t, UINT x, UINT y, UINT w, UINT h, bool zero = false);
void draw_motion_vectors_in_game_state(ID3D11DeviceContext* ctx, const PassInputs& in, const MvTarget& t, UINT x, UINT y, UINT w, UINT h, bool zero);

// Replaces one view's temporal anti-aliasing draw. False: the game's draw runs instead.
bool run_dlss(ID3D11DeviceContext* ctx, PassInputs& in, int eye, std::string& why) {
    Rhi& R = *g_rhi;
    ViewRows rows;
    if (!lookup_rows(in.cb1.Get(), rows)) {
        why = "no captured view constants for constant buffer 1";
        with_info([](Info& i) { ++i.no_rows; });
        return false;
    }
    if (g_set.dump_requests.load() > 0) {
        g_set.dump_requests.fetch_sub(1);
        dump_rows(rows, in, eye);
    }
    if (!ensure_shaders(R.dev)) {
        why = "motion vector shaders unavailable";
        return false;
    }
    const DXGI_FORMAT out_format = typed_output(in.target_desc.Format);
    if (!ensure_textures(R.dev, in.color_desc, out_format)) {
        why = "could not create the motion vector or output texture";
        return false;
    }
    const UINT x = static_cast<UINT>(in.vp.TopLeftX), y = static_cast<UINT>(in.vp.TopLeftY);
    const UINT w = static_cast<UINT>(in.vp.Width), h = static_cast<UINT>(in.vp.Height);
    if (x + w > in.color_desc.Width || y + h > in.color_desc.Height) {
        why = "viewport outside the colour texture";
        return false;
    }
    MvTarget mt;
    if (!mv_target(eye, x, y, w, h, mt)) {
        why = "could not create the motion vector texture";
        return false;
    }
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
        why = "no ID3D11DeviceContext1";
        return false;
    }
    std::string cut;
    const unsigned cut_why = detect_cut(eye, rows, R.frame, &cut);
    bool zero_mv = false, allow_reset = true;
    cuttest_on_pass(cut_why, R.frame, zero_mv, allow_reset);
    const bool cut_reset = cut_why && g_set.cut_reset.load() && allow_reset;
    if (cut_why)
        with_info([&](Info& i) { i.last_cut = cut + (cut_reset ? ", history reset" : ", no reset"); });
    if (cut_reset) ++g_cut_resets;
    // [dlss] context_state = game (the default): the motion vectors are drawn in the game's own
    // pipeline state (what the draw changes is saved and put back) and NGX creates and evaluates
    // in it (NGX keeps the state of the immediate D3D11 context, programming guide 5.2.5). A draw
    // inside a device context state of our own, swapped in with SwapDeviceContextState in the
    // middle of the engine's frame, hung the GPU within seconds at the title screen
    // (docs/dlss.md, "GPU faults"). context_state = own: the earlier way, all of it inside our
    // own state object (kept to compare).
    const bool own = g_set.own_state.load(std::memory_order_relaxed);
    bool ok = false;
    bool created = false;
    Feature& f = R.feat[eye];
    ID3DDeviceContextState* game_state = nullptr;
    if (own) ctx1->SwapDeviceContextState(R.state.Get(), &game_state);
    if (ensure_feature(ctx, eye, w, h, created)) {
        Timing* t = begin_timing(ctx, eye);
        // Motion vectors for the eye's rectangle.
        if (own) draw_motion_vectors(ctx, in, mt, x, y, w, h, zero_mv);
        else draw_motion_vectors_in_game_state(ctx, in, mt, x, y, w, h, zero_mv);
        if (t) ctx->End(t->t1);
        // DLSS for the eye's rectangle.
        const float jx = rows.v[kRowJitter - kRowFirst][0] * static_cast<float>(w) * g_set.jitter_scale_x.load();
        const float jy = rows.v[kRowJitter - kRowFirst][1] * static_cast<float>(h) * g_set.jitter_scale_y.load();
        const bool gap = !created && f.last_eval_frame + 1 < R.frame;
        const bool requested = g_set.reset_requests.load() > 0;
        const bool reset = created || gap || cut_reset || requested;
        EvalInputs e;
        e.eye = eye;
        e.handle = f.handle;
        e.fw = f.w;
        e.fh = f.h;
        e.fminw = f.min_w;
        e.fminh = f.min_h;
        e.fow = f.ow;
        e.foh = f.oh;
        e.color = in.color.Get();
        e.depth = in.depth.Get();
        e.mv = mt.tex;
        e.out = R.out.Get();
        e.x = e.dx = x;
        e.y = e.dy = y;
        e.mx = mt.bx;
        e.my = mt.by;
        e.w = w;
        e.h = h;
        e.ox = x;
        e.oy = y;
        e.jx = jx;
        e.jy = jy;
        e.reset = reset;
        NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
        if (before_eval(e)) {
            note_reset(eye, R.frame, created, gap, cut_reset, requested);
            if (own && !mt.at_origin) run_stats(ctx, eye, mt.srv, in.srv[1].Get(), in.srv[2].Get(), false, x, y, w, h);
            NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
            ep.Feature.pInColor = in.color.Get();
            ep.Feature.pInOutput = R.out.Get();
            ep.pInDepth = in.depth.Get();
            ep.pInMotionVectors = mt.tex;
            ep.InJitterOffsetX = jx;
            ep.InJitterOffsetY = jy;
            ep.InRenderSubrectDimensions = {w, h};
            ep.InReset = reset ? 1 : 0;
            ep.InMVScaleX = 1.0f;
            ep.InMVScaleY = 1.0f;
            ep.InColorSubrectBase = {x, y};
            ep.InDepthSubrectBase = {x, y};
            ep.InMVSubrectBase = {mt.bx, mt.by};
            ep.InOutputSubrectBase = {x, y};
            ep.InFrameTimeDeltaInMsec = R.frame_dt_ms;
            g_in_ngx = true;
            r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f.handle, f.params, &ep);
            g_in_ngx = false;
            if (own) ctx->ClearState();
            if (g_set.flush_after.load(std::memory_order_relaxed)) ctx->Flush();
            event(std::format("eval eye {} result {}", eye, result_text(r)));
        } else {
            why = "a check before the evaluation failed (see the log)";
        }
        retire_object(in.color.Get());  // the engine's textures, read by the evaluation
        retire_object(in.depth.Get());
        if (NVSDK_NGX_SUCCEED(r)) {
            const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
            ctx->CopySubresourceRegion(in.target.Get(), in.target_view.Texture2D.MipSlice, x, y, 0, R.out.Get(), 0, &box);
            f.last_eval_frame = R.frame;
            ok = true;
            with_info([&](Info& i) {
                i.jitter_px[eye][0] = jx;
                i.jitter_px[eye][1] = jy;
            });
        } else if (why.empty()) {
            why = std::format("EvaluateFeature {}", result_text(r));
            with_info([](Info& i) { ++i.eval_fail; });
        }
        if (t) {
            ctx->End(t->t2);
            ctx->End(t->disjoint);
            t->pending = true;
        }
    } else {
        why = "feature creation failed";
    }
    if (own) {
        ctx1->SwapDeviceContextState(game_state, nullptr);
        if (game_state) game_state->Release();
    }
    return ok;
}

std::atomic<std::uint64_t> g_fallback_log{0};

ComPtr<ID3D11Texture2D> make_tex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind);

// ------------------------------------------------------------------ upscale mode
// With r.ScreenPercentage below 100 the engine renders, anti-aliases, blooms and tonemaps each
// eye at the reduced size, and its last pass (colour grading, film grain) scales the result
// up into the eye's rectangle of the eye texture (docs/re/engine.md section 12). In this mode
// the anti-aliasing pass only passes the jittered image through (and the motion vectors are
// made then); the last pass is run once more into a texture at the reduced size (same pass,
// smaller viewport, so it grades without scaling), and DLSS scales that up into the eye's
// rectangle of the eye texture, with display-referred input.
struct Stash {
    std::uint64_t frame = 0;
    UINT x = 0, y = 0, w = 0, h = 0;
    float jx = 0, jy = 0;
    bool reset = false;  // camera cut: reset the eye's history
    ComPtr<ID3D11Resource> depth;
    ComPtr<ID3D11ShaderResourceView> depth_srv;  // the engine's view of it (input statistics)
    ComPtr<ID3D11Texture2D> mv;                   // the eye's motion vectors (see MvTarget)
    ComPtr<ID3D11ShaderResourceView> mv_srv;
    UINT mbx = 0, mby = 0;                        // where the eye's rectangle is in it
    ViewRows rows;  // the view constants of the anti-aliasing pass (frame dump)
};
Stash g_stash[2];  // RHI thread

// ------------------------------------------------------------------ frame dump (dev command)
// `dlss frames <prefix> [n] [crop]`: for n consecutive frames (default 4) the centre crop
// (default 1024 pixels square) of each eye's DLSS input (the graded colour at the reduced
// size) and of its motion vectors, copied at the frame end and read back without a stall,
// as raw files <prefix>_f<k>_e<eye>_color_<w>x<h>.r10g10b10a2 and ..._mv_<w>x<h>.rg16f, with
// each eye's rectangle, jitter, reset and view constants in <prefix>_meta.txt. Upscale mode.
// For checking the motion vectors and the jitter of each eye against the images.
struct DumpEye {
    std::uint64_t frame = 0;  // the frame of the evaluation
    UINT x = 0, y = 0, w = 0, h = 0;
    UINT ox = 0, oy = 0, ow = 0, oh = 0;  // the output rectangle
    ComPtr<ID3D11Texture2D> out;          // the texture DLSS wrote (the shared output, or the eye's own with test_copy_inputs)
    UINT out_x = 0, out_y = 0;            // where in it
    ComPtr<ID3D11Texture2D> mv;           // the eye's motion vectors, and where its rectangle is in them
    UINT mbx = 0, mby = 0;
    float jx = 0, jy = 0;
    bool reset = false;
    ViewRows rows;
};
DumpEye g_dump_eye[2];  // RHI thread
struct FrameDump {
    std::mutex m;
    std::string prefix;  // under m
    std::atomic<int> remaining{0};
    std::atomic<int> crop{1024};
    struct Slot {
        ComPtr<ID3D11Texture2D> color, mv, out;
        std::uint64_t frame = 0;
        int index = 0, eye = 0;
        UINT w = 0, h = 0, out_w = 0, out_h = 0;
        bool pending = false;
    };
    std::vector<Slot> slots;  // RHI thread
    int index = 0;
    std::atomic<int> written{0};
};
FrameDump g_framedump;
// The eyes whose anti-aliasing pass ran in the frame g_taa_frame (bit 0 left, bit 1 right).
std::uint64_t g_taa_frame = 0;
unsigned g_taa_mask = 0;
// The eye rectangle DLSS wrote in the eye texture (the whole half), per eye and frame: the
// image handed to the runtime is then the whole half, also when the views rendered less of
// it ([stereo] render_scale, dynamic resolution).
struct FullRect {
    std::uint64_t frame = 0;
    EyeRect rect;
};
FullRect g_full[2];  // RHI thread
ComPtr<ID3D11Texture2D> g_graded;  // the last pass at the reduced size
ComPtr<ID3D11RenderTargetView> g_graded_rtv;
D3D11_TEXTURE2D_DESC g_graded_desc{};
ComPtr<ID3D11Texture2D> g_up_out;  // DLSS output: like the eye texture, or two eyes of the runtime's size with output = runtime
// [dlss] output = runtime: the frame whose evaluations wrote g_up_out (two eyes of ow x oh
// side by side), and how often it went to the runtime.
struct RuntimeOut {
    std::uint64_t frame = 0;
    UINT ow = 0, oh = 0;
    std::atomic<std::uint64_t> handed{0}, fallback_eyes{0}, frames{0};
};
RuntimeOut g_rt_out;  // RHI thread (counters read by status)
D3D11_TEXTURE2D_DESC g_up_out_desc{};
ID3D11PixelShader* g_final_ps = nullptr;  // compared only
std::atomic<std::uint64_t> g_up_count{0}, g_up_fail{0}, g_up_skipped{0};
// The last pass's pixel shader constants (cb0, 1024 bytes): rows 30/31 the input rectangle
// and size, rows 34/35 the output rectangle (min x, min y, max x, max y) and size (w, h,
// 1/w, 1/h); the pixel shader maps SV_Position through rows 34/35 (docs/re/engine.md
// section 12). For the run at the reduced size a copy of cb0 gets rows 34/35 replaced.
constexpr UINT kFinalOutRectRow = 34;
ComPtr<ID3D11Buffer> g_cb_copy, g_cb_patch;
int g_cb_checked = 0;  // 0 not yet, 1 rows 34/35 matched the viewport, -1 they did not

// One-time check (stalls once): rows 34/35 of the bound cb0 describe the current viewport.
bool check_final_rows(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, const D3D11_VIEWPORT& vp) {
    Rhi& R = *g_rhi;
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = 16 * (kFinalOutRectRow + 2);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> st;
    if (FAILED(R.dev->CreateBuffer(&sd, nullptr, &st))) return false;
    const D3D11_BOX box{0, 0, 0, sd.ByteWidth, 1, 1};
    ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, cb, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const float* f = static_cast<const float*>(m.pData) + 4 * kFinalOutRectRow;
    const bool ok = f[0] == vp.TopLeftX && f[1] == vp.TopLeftY && f[2] == vp.TopLeftX + vp.Width && f[3] == vp.TopLeftY + vp.Height &&
                    f[4] == vp.Width && f[5] == vp.Height;
    log::info("dlss: last pass cb0 rows {}/{}: {} {} {} {} | {} {} {} {} ({} the viewport {} {} {} {})", kFinalOutRectRow, kFinalOutRectRow + 1, f[0],
              f[1], f[2], f[3], f[4], f[5], f[6], f[7], ok ? "match" : "DO NOT match", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    ctx->Unmap(st.Get(), 0);
    return ok;
}

void draw_motion_vectors(ID3D11DeviceContext* ctx, const PassInputs& in, const MvTarget& t, UINT x, UINT y, UINT w, UINT h, bool zero) {
    Rhi& R = *g_rhi;
    PassConsts pc{};
    pc.rect[0] = static_cast<float>(x);
    pc.rect[1] = static_cast<float>(y);
    pc.rect[2] = static_cast<float>(w);
    pc.rect[3] = static_cast<float>(h);
    pc.opt[0] = 1.0f / static_cast<float>(w);
    pc.opt[1] = 1.0f / static_cast<float>(h);
    pc.opt[2] = g_set.mv_jitter.load() == 0 ? 1.0f : 0.0f;
    pc.opt[3] = (zero || g_set.zero_mv.load(std::memory_order_relaxed)) ? 1.0f : 0.0f;
    pc.opt2[0] = g_set.mv_sanitize.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    pc.opt2[1] = static_cast<float>(std::max(w, h));
    D3D11_VIEWPORT vp = in.vp;
    if (t.at_origin) {
        pc.opt3[0] = in.vp.TopLeftX;
        pc.opt3[1] = in.vp.TopLeftY;
        vp.TopLeftX = vp.TopLeftY = 0.0f;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(R.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &pc, sizeof(pc));
        ctx->Unmap(R.cb.Get(), 0);
    }
    ID3D11Buffer* cbs[2] = {R.cb.Get(), in.cb1.Get()};
    ID3D11ShaderResourceView* srvs[2] = {in.srv[1].Get(), in.srv[4].Get()};
    ID3D11RenderTargetView* rt = t.rtv;
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(R.vs.Get(), nullptr, 0);
    ctx->PSSetShader(R.ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    ctx->ClearState();
}

// The motion vector draw in the game's own pipeline state: the state it changes is saved
// first and put back afterwards (no context-state swap).
void draw_motion_vectors_in_game_state(ID3D11DeviceContext* ctx, const PassInputs& in, const MvTarget& t, UINT x, UINT y, UINT w, UINT h, bool zero) {
    Rhi& R = *g_rhi;
    // Save
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ctx->IAGetPrimitiveTopology(&topo);
    ComPtr<ID3D11InputLayout> layout;
    ctx->IAGetInputLayout(&layout);
    ComPtr<ID3D11VertexShader> vs;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ComPtr<ID3D11GeometryShader> gs;
    ctx->GSGetShader(&gs, nullptr, nullptr);
    ComPtr<ID3D11PixelShader> ps;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ID3D11Buffer* cbs_raw[2]{};
    ctx->PSGetConstantBuffers(0, 2, cbs_raw);
    ID3D11ShaderResourceView* srvs_raw[2]{};
    ctx->PSGetShaderResources(0, 2, srvs_raw);
    ID3D11RenderTargetView* rtvs_raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv_raw = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_raw, &dsv_raw);
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nvp, vps);
    ComPtr<ID3D11RasterizerState> rs;
    ctx->RSGetState(&rs);
    ComPtr<ID3D11BlendState> blend;
    float blend_factor[4]{};
    UINT sample_mask = 0;
    ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
    ComPtr<ID3D11DepthStencilState> dss;
    UINT stencil_ref = 0;
    ctx->OMGetDepthStencilState(&dss, &stencil_ref);
    // Ours
    PassConsts pc{};
    pc.rect[0] = static_cast<float>(x);
    pc.rect[1] = static_cast<float>(y);
    pc.rect[2] = static_cast<float>(w);
    pc.rect[3] = static_cast<float>(h);
    pc.opt[0] = 1.0f / static_cast<float>(w);
    pc.opt[1] = 1.0f / static_cast<float>(h);
    pc.opt[2] = g_set.mv_jitter.load() == 0 ? 1.0f : 0.0f;
    pc.opt[3] = (zero || g_set.zero_mv.load(std::memory_order_relaxed)) ? 1.0f : 0.0f;
    pc.opt2[0] = g_set.mv_sanitize.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    pc.opt2[1] = static_cast<float>(std::max(w, h));
    D3D11_VIEWPORT vp = in.vp;
    if (t.at_origin) {
        // Drawn at the origin of the eye's own texture; the shader loads at the engine's pixels.
        pc.opt3[0] = in.vp.TopLeftX;
        pc.opt3[1] = in.vp.TopLeftY;
        vp.TopLeftX = vp.TopLeftY = 0.0f;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(R.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &pc, sizeof(pc));
        ctx->Unmap(R.cb.Get(), 0);
    }
    ID3D11RenderTargetView* rt = t.rtv;
    ctx->OMSetRenderTargets(1, &rt, nullptr);  // first, so the depth texture is no longer bound for writing
    ID3D11Buffer* cbs[2] = {R.cb.Get(), in.cb1.Get()};
    ID3D11ShaderResourceView* srvs[2] = {in.srv[1].Get(), in.srv[4].Get()};
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(R.vs.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(R.ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->Draw(3, 0);
    // Restore
    ctx->PSSetShaderResources(0, 2, srvs_raw);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_raw, dsv_raw);
    ctx->PSSetConstantBuffers(0, 2, cbs_raw);
    ctx->IASetInputLayout(layout.Get());
    ctx->IASetPrimitiveTopology(topo);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->GSSetShader(gs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->RSSetState(rs.Get());
    ctx->RSSetViewports(nvp, vps);
    ctx->OMSetBlendState(blend.Get(), blend_factor, sample_mask);
    ctx->OMSetDepthStencilState(dss.Get(), stencil_ref);
    for (auto* p : cbs_raw)
        if (p) p->Release();
    for (auto* p : srvs_raw)
        if (p) p->Release();
    for (auto* p : rtvs_raw)
        if (p) p->Release();
    if (dsv_raw) dsv_raw->Release();
}

// Anti-aliasing pass in upscale mode: motion vectors, jittered colour passed through.
bool run_passthrough(ID3D11DeviceContext* ctx, PassInputs& in, int eye, std::string& why) {
    Rhi& R = *g_rhi;
    ViewRows rows;
    if (!lookup_rows(in.cb1.Get(), rows)) {
        why = "no captured view constants for constant buffer 1";
        with_info([](Info& i) { ++i.no_rows; });
        return false;
    }
    if (!ensure_shaders(R.dev) || !ensure_textures(R.dev, in.color_desc, typed_output(in.target_desc.Format), false)) {
        why = "motion vector resources unavailable";
        return false;
    }
    // At screen percentages below 100 the right view's rectangle can reach a pixel or two past
    // the scaled buffer (4116 wide for two 2059-pixel views at 67 %); the rest is clipped.
    const UINT x = static_cast<UINT>(in.vp.TopLeftX), y = static_cast<UINT>(in.vp.TopLeftY);
    const UINT w = std::min(static_cast<UINT>(in.vp.Width), in.color_desc.Width > x ? in.color_desc.Width - x : 0u);
    const UINT h = std::min(static_cast<UINT>(in.vp.Height), in.color_desc.Height > y ? in.color_desc.Height - y : 0u);
    if (w < 64 || h < 64 || in.target_desc.Format != in.color_desc.Format) {
        why = "viewport outside the colour texture or formats differ";
        return false;
    }
    MvTarget mt;
    if (!mv_target(eye, x, y, static_cast<UINT>(in.vp.Width), static_cast<UINT>(in.vp.Height), mt)) {
        why = "could not create the motion vector texture";
        return false;
    }
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return false;
    std::string cut;
    const unsigned cut_why = detect_cut(eye, rows, R.frame, &cut);
    bool zero_mv = false, allow_reset = true;
    cuttest_on_pass(cut_why, R.frame, zero_mv, allow_reset);
    const bool cut_reset = cut_why && g_set.cut_reset.load() && allow_reset;
    if (cut_why)
        with_info([&](Info& i) { i.last_cut = cut + (cut_reset ? ", history reset" : ", no reset"); });
    if (cut_reset) ++g_cut_resets;
    const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    const int skip_level = g_set.skip_level.load(std::memory_order_relaxed);
    if (skip_level == 5) {
        // Test: our state swapped in and out around the copy, no draw.
        ID3DDeviceContextState* game_state = nullptr;
        ctx1->SwapDeviceContextState(R.state.Get(), &game_state);
        ctx->ClearState();
        ctx->CopySubresourceRegion(in.target.Get(), in.target_view.Texture2D.MipSlice, x, y, 0, in.color.Get(), 0, &box);
        ctx1->SwapDeviceContextState(game_state, nullptr);
        if (game_state) game_state->Release();
    } else if (skip_level == 6) {
        // Test: the motion vectors drawn in the game's own state (saved and restored here), no swap.
        draw_motion_vectors_in_game_state(ctx, in, mt, x, y, static_cast<UINT>(in.vp.Width), static_cast<UINT>(in.vp.Height), zero_mv);
        ctx->CopySubresourceRegion(in.target.Get(), in.target_view.Texture2D.MipSlice, x, y, 0, in.color.Get(), 0, &box);
    } else if (skip_level >= 3) {
        // Test: the jittered colour passed through in the game's state, no motion vectors.
        ctx->CopySubresourceRegion(in.target.Get(), in.target_view.Texture2D.MipSlice, x, y, 0, in.color.Get(), 0, &box);
    } else if (!g_set.own_state.load(std::memory_order_relaxed)) {
        // The motion vectors drawn in the game's own state (see run_dlss), then the copy.
        draw_motion_vectors_in_game_state(ctx, in, mt, x, y, static_cast<UINT>(in.vp.Width), static_cast<UINT>(in.vp.Height), zero_mv);
        ctx->CopySubresourceRegion(in.target.Get(), in.target_view.Texture2D.MipSlice, x, y, 0, in.color.Get(), 0, &box);
    } else {
        ID3DDeviceContextState* game_state = nullptr;
        ctx1->SwapDeviceContextState(R.state.Get(), &game_state);
        draw_motion_vectors(ctx, in, mt, x, y, static_cast<UINT>(in.vp.Width), static_cast<UINT>(in.vp.Height), zero_mv);
        ctx->CopySubresourceRegion(in.target.Get(), in.target_view.Texture2D.MipSlice, x, y, 0, in.color.Get(), 0, &box);
        ctx1->SwapDeviceContextState(game_state, nullptr);
        if (game_state) game_state->Release();
    }
    Stash& s = g_stash[eye];
    s.frame = R.frame;
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    s.reset = cut_reset;
    s.jx = rows.v[kRowJitter - kRowFirst][0] * in.vp.Width * g_set.jitter_scale_x.load();
    s.jy = rows.v[kRowJitter - kRowFirst][1] * in.vp.Height * g_set.jitter_scale_y.load();
    s.depth = in.depth;
    s.depth_srv = in.srv[1];
    s.mv = mt.tex;
    s.mv_srv = mt.srv;
    s.mbx = mt.bx;
    s.mby = mt.by;
    s.rows = rows;
    if (g_taa_frame != R.frame) {
        g_taa_frame = R.frame;
        g_taa_mask = 0;
    }
    g_taa_mask |= 1u << eye;
    event(std::format("taa eye {} vp {} {} {} {} | colour {} | depth {} (view f{}) | velocity {} | target {} | cb1 {} | jitter {:.4f} {:.4f}{}", eye,
                      in.vp.TopLeftX, in.vp.TopLeftY, in.vp.Width, in.vp.Height, res_text(in.color.Get()), res_text(in.depth.Get()),
                      static_cast<int>(in.depth_view.Format), res_text(in.velocity.Get()), res_text(in.target.Get()), static_cast<void*>(in.cb1.Get()),
                      s.jx, s.jy, cut_why ? " | cut " + cut_text(cut_why) : std::string()));
    return true;
}

// The parts of the game's pipeline state that a draw of ours changes, saved and put back
// (draws in the game's own state, no context-state swap: see run_dlss).
struct SavedState {
    ID3D11DeviceContext* ctx;
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ID3D11Buffer* cbs[2]{};
    ID3D11ShaderResourceView* srvs[3]{};
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11BlendState> blend;
    float blend_factor[4]{};
    UINT sample_mask = 0;
    ComPtr<ID3D11DepthStencilState> dss;
    UINT stencil_ref = 0;
    explicit SavedState(ID3D11DeviceContext* c) : ctx(c) {
        ctx->IAGetPrimitiveTopology(&topo);
        ctx->IAGetInputLayout(&layout);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->PSGetConstantBuffers(0, 2, cbs);
        ctx->PSGetShaderResources(0, 3, srvs);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        ctx->RSGetViewports(&nvp, vps);
        ctx->RSGetState(&rs);
        ctx->OMGetBlendState(&blend, blend_factor, &sample_mask);
        ctx->OMGetDepthStencilState(&dss, &stencil_ref);
        // Our draws: no input layout, no geometry shader, default rasterizer, blend and depth states.
        ctx->IASetInputLayout(nullptr);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        ctx->OMSetDepthStencilState(nullptr, 0);
    }
    ~SavedState() {
        ID3D11ShaderResourceView* none[3]{};
        ctx->PSSetShaderResources(0, 3, none);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(topo);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->GSSetShader(gs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->RSSetState(rs.Get());
        ctx->RSSetViewports(nvp, vps);
        ctx->OMSetBlendState(blend.Get(), blend_factor, sample_mask);
        ctx->OMSetDepthStencilState(dss.Get(), stencil_ref);
        for (auto* p : cbs)
            if (p) p->Release();
        for (auto* p : srvs)
            if (p) p->Release();
        for (auto* p : rtvs)
            if (p) p->Release();
        if (dsv) dsv->Release();
    }
    SavedState(const SavedState&) = delete;
    SavedState& operator=(const SavedState&) = delete;
};

// Test (`[dlss] test_copy_inputs`): each eye's inputs in textures of their own, at the origin
// and of the feature's input size, and its output in a texture of the output size, so that
// no two features read or write the same textures and no sub-rectangle bases are used.
struct EyeCopies {
    ComPtr<ID3D11Texture2D> color, depth, mv, out;
    ComPtr<ID3D11RenderTargetView> depth_rtv, color_rtv;
    ComPtr<ID3D11ShaderResourceView> depth_srv, mv_srv;
    D3D11_TEXTURE2D_DESC cd{}, od{};
};
EyeCopies g_copies[2];
ComPtr<ID3D11PixelShader> g_ps_depth, g_ps_color;
ComPtr<ID3D11ShaderResourceView> g_graded_srv;  // the graded texture (colour floor test)
std::atomic<float> g_color_floor{0.0f};          // [dlss] test_color_floor: with test_copy_inputs, colour channels raised to at least this
std::atomic<float> g_color_noise{0.0f};          // [dlss] test_color_noise: with test_copy_inputs, per-pixel noise of this amplitude added
std::atomic<float> g_depth_const{0.0f};          // [dlss] test_depth_const: with test_copy_inputs, the depth handed to DLSS is this constant

// Inside our own pipeline state: the eye's rectangle x,y w x h of the colour and the motion
// vectors copied, and of the depth drawn (the engine's depth is a typeless depth-stencil
// texture, which cannot be copied in part), to the origin of the eye's own textures.
bool prepare_copies(ID3D11DeviceContext* ctx, int eye, ID3D11Resource* color, ID3D11ShaderResourceView* color_srv, ID3D11ShaderResourceView* depth_srv,
                    ID3D11Resource* mv, UINT mvx, UINT mvy, UINT x, UINT y, UINT w, UINT h, UINT fw, UINT fh, UINT ow, UINT oh,
                    DXGI_FORMAT out_fmt, bool game_state) {
    Rhi& R = *g_rhi;
    EyeCopies& c = g_copies[eye];
    D3D11_TEXTURE2D_DESC sd{};
    if (!tex_desc(color, sd) || !depth_srv || !mv) return false;
    if (!c.color || c.cd.Width != fw || c.cd.Height != fh || c.cd.Format != sd.Format) {
        retire(c.color);
        retire(c.depth);
        retire(c.mv);
        retire(c.depth_rtv);
        retire(c.depth_srv);
        retire(c.mv_srv);
        retire(c.color_rtv);
        c.color = make_tex(R.dev, fw, fh, sd.Format, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        c.mv = make_tex(R.dev, fw, fh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        c.depth = make_tex(R.dev, fw, fh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        if (!c.color || !c.mv || !c.depth || FAILED(R.dev->CreateRenderTargetView(c.depth.Get(), nullptr, &c.depth_rtv)) ||
            FAILED(R.dev->CreateRenderTargetView(c.color.Get(), nullptr, &c.color_rtv)) ||
            FAILED(R.dev->CreateShaderResourceView(c.depth.Get(), nullptr, &c.depth_srv)) ||
            FAILED(R.dev->CreateShaderResourceView(c.mv.Get(), nullptr, &c.mv_srv))) {
            c.color.Reset();
            return false;
        }
        c.color->GetDesc(&c.cd);
        log::info("dlss: eye {} input copies {}x{} (colour format {})", eye, fw, fh, static_cast<int>(sd.Format));
    }
    if (!c.out || c.od.Width != ow || c.od.Height != oh || c.od.Format != out_fmt) {
        retire(c.out);
        c.out = make_tex(R.dev, ow, oh, out_fmt, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        if (!c.out) return false;
        c.out->GetDesc(&c.od);
        log::info("dlss: eye {} output texture {}x{} format {}", eye, ow, oh, static_cast<int>(out_fmt));
    }
    if (!g_ps_depth) {
        ComPtr<ID3DBlob> b, bc;
        if (!compile("psdepth", "ps_5_0", b) || FAILED(R.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ps_depth)) ||
            !compile("pscolor", "ps_5_0", bc) || FAILED(R.dev->CreatePixelShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, &g_ps_color)))
            return false;
    }
    const float floor_v = g_color_floor.load(std::memory_order_relaxed);
    const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    const bool draw_color = (floor_v > 0.0f || g_color_noise.load(std::memory_order_relaxed) > 0.0f) && color_srv;
    if (!draw_color) ctx->CopySubresourceRegion(c.color.Get(), 0, 0, 0, 0, color, 0, &box);
    const D3D11_BOX mbox{mvx, mvy, 0, mvx + w, mvy + h, 1};
    ctx->CopySubresourceRegion(c.mv.Get(), 0, 0, 0, 0, mv, 0, &mbox);
    PassConsts pc{};
    pc.rect[0] = static_cast<float>(x);
    pc.rect[1] = static_cast<float>(y);
    pc.rect[2] = static_cast<float>(w);
    pc.rect[3] = static_cast<float>(h);
    pc.opt2[2] = floor_v;
    pc.opt2[3] = g_color_noise.load(std::memory_order_relaxed);
    pc.opt[0] = static_cast<float>(R.frame & 0xffff);
    pc.opt[1] = g_depth_const.load(std::memory_order_relaxed);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(R.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    std::memcpy(m.pData, &pc, sizeof(pc));
    ctx->Unmap(R.cb.Get(), 0);
    ID3D11Buffer* cb = R.cb.Get();
    ID3D11RenderTargetView* rt = c.depth_rtv.Get();
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(R.vs.Get(), nullptr, 0);
    ctx->PSSetShader(g_ps_depth.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &depth_srv);
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    if (draw_color) {
        ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, color_srv};
        ID3D11RenderTargetView* crt = c.color_rtv.Get();
        ctx->OMSetRenderTargets(1, &crt, nullptr);
        ctx->PSSetShader(g_ps_color.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->Draw(3, 0);
    }
    if (!game_state) ctx->ClearState();  // in the game's state the caller's SavedState puts it back
    return true;
}

// [dlss] eval_at = frame_end: each eye's evaluation queued at its last pass and made at the end
// of the frame (dlss::frame, before the eye texture goes to the runtime), outside the engine's
// post-processing; the game's own last pass draws the eye first.
struct Pending {
    std::uint64_t frame = 0;
    Stash s;
    UINT half = 0, td_w = 0, td_h = 0, id_w = 0, id_h = 0, out_w = 0, out_h = 0;
    float vp_w = 0, vp_h = 0;
    bool copy_back = true;
    ComPtr<ID3D11Resource> target;
};
Pending g_pending[2];  // RHI thread

bool evaluate_upscale(ID3D11DeviceContext* ctx, int eye, Stash& s, UINT half, UINT td_w, UINT td_h, UINT id_w, UINT id_h, ID3D11Resource* target_res,
                      float vp_w, float vp_h, UINT out_w, UINT out_h, bool copy_back);

// Frame end: the queued evaluations of this frame (frame_end placement).
void run_pending(ID3D11DeviceContext* ctx) {
    Rhi& R = *g_rhi;
    for (int eye = 0; eye < 2; ++eye) {
        Pending& p = g_pending[eye];
        if (!p.target) continue;
        if (p.frame == R.frame && R.ngx_state == 1 && g_set.enabled.load()) {
            if (!evaluate_upscale(ctx, eye, p.s, p.half, p.td_w, p.td_h, p.id_w, p.id_h, p.target.Get(), p.vp_w, p.vp_h, p.out_w, p.out_h, p.copy_back))
                event(std::format("frame-end evaluation of eye {} not made; the game's image stays", eye));
        }
        retire(p.s.depth);
        retire(p.s.depth_srv);
        p.target.Reset();
        p.frame = 0;
    }
}

// The last pass of a view in upscale mode: true if it was replaced.
bool try_final(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    Rhi& R = *g_rhi;
    if (g_set.skip_level.load(std::memory_order_relaxed) >= 2) return false;  // test: the game's last pass as it is
    if (g_stash[0].frame != R.frame && g_stash[1].frame != R.frame) return false;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps) ps->Release();
    if (g_final_ps && ps != g_final_ps) return false;
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    if (nvp == 0) return false;
    const int eye = vp.TopLeftX >= 1.0f ? 1 : 0;
    Stash& s = g_stash[eye];
    if (s.frame != R.frame || !s.depth) return false;
    ID3D11RenderTargetView* rtv[8]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(8, rtv, &dsv);
    ComPtr<ID3D11RenderTargetView> saved_rtv[8];
    for (int i = 0; i < 8; ++i) saved_rtv[i].Attach(rtv[i]);
    ComPtr<ID3D11DepthStencilView> saved_dsv;
    saved_dsv.Attach(dsv);
    if (!saved_rtv[0] || saved_rtv[1]) return false;
    ComPtr<ID3D11Resource> target;
    saved_rtv[0]->GetResource(&target);
    D3D11_TEXTURE2D_DESC td{};
    if (!tex_desc(target.Get(), td)) return false;
    // The eye texture: two halves side by side. The pass writes the view's rectangle at the
    // corner of the eye's half: the whole half, or less of it with [stereo] render_scale.
    const UINT half = td.Width / 2;
    if ((td.Format != DXGI_FORMAT_R10G10B10A2_UNORM && td.Format != DXGI_FORMAT_R10G10B10A2_TYPELESS) || half < 64 || vp.Width < 64.0f ||
        vp.TopLeftX != static_cast<float>(eye * half) || vp.TopLeftY != 0.0f || vp.Width > static_cast<float>(half) ||
        vp.Height > static_cast<float>(td.Height) || vp.Width < static_cast<float>(s.w) || vp.Height < static_cast<float>(s.h))
        return false;
    ID3D11ShaderResourceView* srv1 = nullptr;
    ctx->PSGetShaderResources(1, 1, &srv1);
    ComPtr<ID3D11ShaderResourceView> input;
    input.Attach(srv1);
    if (!input) return false;
    ComPtr<ID3D11Resource> input_res;
    input->GetResource(&input_res);
    D3D11_TEXTURE2D_DESC id{};
    if (!tex_desc(input_res.Get(), id) || s.x + s.w > id.Width || s.y + s.h > id.Height) return false;
    if (!g_final_ps) {
        g_final_ps = ps;
        log::info("dlss: last pass recognised: pixel shader {} | target {}x{} format {} | vp {} {} {} {} | input {}x{} format {} | eye rect at the reduced size {} {} {} {}",
                  static_cast<void*>(ps), td.Width, td.Height, static_cast<int>(td.Format), vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, id.Width,
                  id.Height, static_cast<int>(id.Format), s.x, s.y, s.w, s.h);
    }
    event(std::format("final eye {} vp {} {} {} {} | target {} | input {} | stash {},{} {}x{} of frame {}", eye, vp.TopLeftX, vp.TopLeftY, vp.Width,
                      vp.Height, res_text(target.Get()), res_text(input_res.Get()), s.x, s.y, s.w, s.h, s.frame));
    // The size DLSS writes per eye: the half of the eye texture, or with [dlss] output =
    // runtime the runtime's eye size, into a texture of DLSS's own that goes to the runtime
    // instead of the eye texture (output_texture).
    UINT out_w = half, out_h = td.Height;
    const bool rt_mode = runtime_output();
    if (rt_mode) {
        const std::uint64_t rs = g_runtime_eye.load(std::memory_order_relaxed);
        const UINT rw = static_cast<UINT>(rs >> 32), rh = static_cast<UINT>(rs & 0xffffffffu);
        if (rw >= vp.Width && rh >= vp.Height && rw <= 8192 && rh <= 8192) {
            out_w = rw;
            out_h = rh;
        }
    }
    // 1. The eye's image at the reduced size, graded like the game's last pass grades it: with
    //    output = runtime and a last pass that does not scale (the view rendered its whole
    //    rectangle), the game's own draw into the eye texture, copied; otherwise the last pass
    //    once more into a texture at the reduced size (same shaders and inputs, a viewport the
    //    size of the eye's reduced rectangle).
    const bool one_to_one = rt_mode && vp.Width == static_cast<float>(s.w) && vp.Height == static_cast<float>(s.h);
    if (!g_graded || g_graded_desc.Width != id.Width || g_graded_desc.Height != id.Height || g_graded_desc.Format != td.Format) {
        retire(g_graded);
        retire(g_graded_rtv);
        retire(g_graded_srv);
        g_graded = make_tex(R.dev, id.Width, id.Height, td.Format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (!g_graded || FAILED(R.dev->CreateRenderTargetView(g_graded.Get(), nullptr, &g_graded_rtv)) ||
            FAILED(R.dev->CreateShaderResourceView(g_graded.Get(), nullptr, &g_graded_srv))) {
            g_graded.Reset();
            return false;
        }
        g_graded->GetDesc(&g_graded_desc);
        log::info("dlss: graded texture {}x{} format {}", id.Width, id.Height, static_cast<int>(td.Format));
    }
    if (!g_up_out || g_up_out_desc.Width != 2 * out_w || g_up_out_desc.Height != out_h) {
        retire(g_up_out);
        UINT support = 0;
        R.dev->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &support);
        if (!(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
            ++g_up_fail;
            return false;
        }
        g_up_out = make_tex(R.dev, 2 * out_w, out_h, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        if (!g_up_out) return false;
        g_up_out->GetDesc(&g_up_out_desc);
        log::info("dlss: upscale output texture {}x{} R10G10B10A2_UNORM ({}; the engine's eye texture {}x{}) | {}", 2 * out_w, out_h,
                  rt_mode ? "output = runtime: it goes to the runtime" : "copied into the eye texture", td.Width, td.Height, vram_text(R.dev));
    }
    if (rt_mode) {
        g_rt_out.frame = R.frame;
        g_rt_out.ow = out_w;
        g_rt_out.oh = out_h;
    }
    if (one_to_one) {
        original(ctx, count, start, base);
        const D3D11_BOX box{static_cast<UINT>(vp.TopLeftX), static_cast<UINT>(vp.TopLeftY), 0, static_cast<UINT>(vp.TopLeftX) + s.w,
                            static_cast<UINT>(vp.TopLeftY) + s.h, 1};
        ctx->CopySubresourceRegion(g_graded.Get(), 0, s.x, s.y, 0, target.Get(), 0, &box);
        if (g_set.skip_eval.load(std::memory_order_relaxed)) {
            s.frame = 0;
            retire(s.depth);
            retire(s.depth_srv);
            ++g_up_skipped;
            return true;
        }
        evaluate_upscale(ctx, eye, s, half, td.Width, td.Height, id.Width, id.Height, target.Get(), vp.Width, vp.Height, out_w, out_h, false);
        s.frame = 0;
        retire(s.depth);
        retire(s.depth_srv);
        return true;  // the eye texture holds the game's image if DLSS did not write the eye
    }
    ID3D11Buffer* cb0_raw = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &cb0_raw);
    ComPtr<ID3D11Buffer> game_cb0;
    game_cb0.Attach(cb0_raw);
    D3D11_BUFFER_DESC cbd{};
    if (game_cb0) game_cb0->GetDesc(&cbd);
    if (!game_cb0 || cbd.ByteWidth < 16 * (kFinalOutRectRow + 2)) return false;
    if (g_cb_checked == 0) g_cb_checked = check_final_rows(ctx, game_cb0.Get(), vp) ? 1 : -1;
    if (g_cb_checked < 0) {
        ++g_up_fail;
        return false;
    }
    if (!g_cb_copy || [&] {
            D3D11_BUFFER_DESC d{};
            g_cb_copy->GetDesc(&d);
            return d.ByteWidth != cbd.ByteWidth;
        }()) {
        retire(g_cb_copy);
        retire(g_cb_patch);
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = cbd.ByteWidth;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_BUFFER_DESC p = d;
        p.ByteWidth = 32;
        if (FAILED(R.dev->CreateBuffer(&d, nullptr, &g_cb_copy)) || FAILED(R.dev->CreateBuffer(&p, nullptr, &g_cb_patch))) {
            g_cb_copy.Reset();
            return false;
        }
    }
    const float patch[8] = {static_cast<float>(s.x), static_cast<float>(s.y), static_cast<float>(s.x + s.w), static_cast<float>(s.y + s.h),
                            static_cast<float>(s.w), static_cast<float>(s.h), 1.0f / static_cast<float>(s.w), 1.0f / static_cast<float>(s.h)};
    ctx->UpdateSubresource(g_cb_patch.Get(), 0, nullptr, patch, 0, 0);
    ctx->CopyResource(g_cb_copy.Get(), game_cb0.Get());
    const D3D11_BOX pbox{0, 0, 0, 32, 1, 1};
    ctx->CopySubresourceRegion(g_cb_copy.Get(), 0, 16 * kFinalOutRectRow, 0, 0, g_cb_patch.Get(), 0, &pbox);
    D3D11_VIEWPORT reduced_vp{static_cast<float>(s.x), static_cast<float>(s.y), static_cast<float>(s.w), static_cast<float>(s.h), vp.MinDepth, vp.MaxDepth};
    ID3D11RenderTargetView* graded = g_graded_rtv.Get();
    ID3D11Buffer* patched = g_cb_copy.Get();
    // The engine's scissor rectangle is the eye's full rectangle; for the right eye it would
    // clip the reduced viewport away (seen: a black right eye at 50 %).
    D3D11_RECT saved_scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&nsc, saved_scissor);
    const D3D11_RECT reduced_sc{static_cast<LONG>(s.x), static_cast<LONG>(s.y), static_cast<LONG>(s.x + s.w), static_cast<LONG>(s.y + s.h)};
    ctx->OMSetRenderTargets(1, &graded, saved_dsv.Get());
    ctx->RSSetViewports(1, &reduced_vp);
    ctx->RSSetScissorRects(1, &reduced_sc);
    ctx->PSSetConstantBuffers(0, 1, &patched);
    // Test (`dlss nograin 1`): the pass's noise texture array (t0, 64 slices of 64x64) unbound.
    ComPtr<ID3D11ShaderResourceView> saved_t0;
    const bool no_grain = g_set.no_grain.load();
    if (no_grain) {
        ID3D11ShaderResourceView* t0 = nullptr;
        ctx->PSGetShaderResources(0, 1, &t0);
        saved_t0.Attach(t0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }
    original(ctx, count, start, base);
    if (no_grain) {
        ID3D11ShaderResourceView* t0 = saved_t0.Get();
        ctx->PSSetShaderResources(0, 1, &t0);
    }
    ID3D11Buffer* game_cb0_raw = game_cb0.Get();
    ctx->PSSetConstantBuffers(0, 1, &game_cb0_raw);
    ID3D11RenderTargetView* restore[8];
    for (int i = 0; i < 8; ++i) restore[i] = saved_rtv[i].Get();
    ctx->OMSetRenderTargets(8, restore, saved_dsv.Get());
    ctx->RSSetViewports(nvp, &vp);
    ctx->RSSetScissorRects(nsc, nsc ? saved_scissor : nullptr);
    if (g_set.skip_eval.load(std::memory_order_relaxed)) {
        // Test (`dlss skip 1`): no NGX call; the game's own pass scales the image up.
        s.frame = 0;
        retire(s.depth);
        retire(s.depth_srv);
        ++g_up_skipped;
        original(ctx, count, start, base);
        return true;
    }
    // 2. DLSS from the reduced rectangle into the eye's rectangle: here, or ([dlss] eval_at =
    //    frame_end) at the end of the frame, after the game's own pass has drawn the eye.
    if (g_set.eval_at_end.load(std::memory_order_relaxed)) {
        original(ctx, count, start, base);  // the eye's image if the evaluation does not happen
        Pending& p = g_pending[eye];
        p.frame = R.frame;
        p.s = s;
        p.half = half;
        p.td_w = td.Width;
        p.td_h = td.Height;
        p.id_w = id.Width;
        p.id_h = id.Height;
        p.vp_w = vp.Width;
        p.vp_h = vp.Height;
        p.out_w = out_w;
        p.out_h = out_h;
        p.copy_back = !rt_mode;
        p.target = target;
        event(std::format("queued eye {} for the frame end", eye));
        s.frame = 0;
        s.depth.Reset();
        s.depth_srv.Reset();
        return true;
    }
    const bool ok =
        evaluate_upscale(ctx, eye, s, half, td.Width, td.Height, id.Width, id.Height, target.Get(), vp.Width, vp.Height, out_w, out_h, !rt_mode);
    s.frame = 0;  // one use per frame
    retire(s.depth);  // the engine's depth texture, read by this frame's evaluation
    retire(s.depth_srv);
    // If DLSS failed, the eye's rectangle still holds the previous frame; draw the game's pass.
    if (!ok) original(ctx, count, start, base);
    return true;
}

// The evaluation of one eye in upscale mode: DLSS from the eye's reduced rectangle of the
// graded texture into the eye's whole half of the eye texture (target). True if it was written.
bool evaluate_upscale(ID3D11DeviceContext* ctx, int eye, Stash& s, UINT half, UINT td_w, UINT td_h, UINT id_w, UINT id_h, ID3D11Resource* target_res,
                      float vp_w, float vp_h, UINT out_w, UINT out_h, bool copy_back) {
    Rhi& R = *g_rhi;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return false;
    const bool in_game_state = !g_set.own_state.load(std::memory_order_relaxed);
    if (in_game_state) {
        // NGX reads the engine's depth texture; in the game's state it must not be bound for
        // writing (the runtime would unbind NGX's view of it). Logged once.
        static bool logged = false;
        if (!logged) {
            logged = true;
            ID3D11DepthStencilView* dsv = nullptr;
            ctx->OMGetRenderTargets(0, nullptr, &dsv);
            D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
            if (dsv) dsv->GetDesc(&dd);
            log::info("dlss: at the evaluation the game's state has {}", dsv ? std::format("a depth-stencil view bound (flags 0x{:x})", dd.Flags) : std::string("no depth-stencil view bound"));
            if (dsv) dsv->Release();
        }
    }
    ID3DDeviceContextState* game_state = nullptr;
    if (!in_game_state) ctx1->SwapDeviceContextState(R.state.Get(), &game_state);
    bool ok = false, created = false;
    // DLSS writes the eye's whole half of its output texture (out_w x out_h per eye: the eye
    // texture's half, or the runtime's eye size), whatever part of it the view rendered.
    const UINT ox = static_cast<UINT>(eye) * out_w, oy = 0;
    const UINT ow = out_w, oh = out_h;
    // The feature is created once for the largest input the settings allow, so that dynamic
    // resolution only changes the input rectangle per evaluation (no recreation): the eye's
    // rectangle at [stereo] render_scale (the upper bound of dynamic resolution, rounded like
    // the device rounds it), times the screen percentage (the input buffer's share of the eye
    // texture), rounded up, plus 2 pixels for the engine's rounding.
    const float rs = g_set.max_full.load() ? 1.0f : std::clamp(device::settings().render_scale.load(), 0.3f, 1.0f);
    auto scaled = [rs](UINT full) {
        return rs >= 0.999f ? full : std::clamp<UINT>(static_cast<UINT>(std::lround(full * rs / 8.0) * 8), 64u, full);
    };
    const UINT max_w = std::min(ow, std::max(s.w, static_cast<UINT>(std::ceil(static_cast<double>(scaled(half)) * id_w / td_w)) + 2));
    const UINT max_h = std::min(oh, std::max(s.h, static_cast<UINT>(std::ceil(static_cast<double>(scaled(td_h)) * id_h / td_h)) + 2));
    // NGX allows a smaller input than the creation size only within the range its optimal
    // settings give for the mode (dynamic resolution, guide 3.2.2): where the range of the
    // chosen mode holds both the largest and the current input, the feature is created for the
    // largest and evaluated at the current one; otherwise it is created at the current input
    // size (a change recreates it).
    // The feature is created only for a mode whose reported range contains its creation size
    // (the guide: the optimal settings are to be used as given): the mode of the input's share
    // of the output first, then the others. If no mode accepts it, there is no evaluation and
    // the game's pass runs (logged). With the NVIDIA App's override forcing DLAA every mode
    // reports about 99-100 % of the output, so an input below that is refused.
    const NVSDK_NGX_PerfQuality_Value modes[] = {quality_for(max_w, ow), NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
                                                 NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance,
                                                 NVSDK_NGX_PerfQuality_Value_DLAA};
    DynRange dr;
    bool dyn = false, found = false, any_range = false;
    NVSDK_NGX_PerfQuality_Value mode = modes[0];
    UINT cw = s.w, ch = s.h;
    for (const NVSDK_NGX_PerfQuality_Value m : modes) {
        const DynRange d = dynamic_range(ow, oh, m);
        if (!d.ok) continue;
        any_range = true;
        auto inside = [&](UINT iw, UINT ih) { return iw >= d.min_w && iw <= d.max_w && ih >= d.min_h && ih <= d.max_h; };
        if (d.min_w < d.max_w && d.min_h < d.max_h && inside(max_w, max_h) && inside(s.w, s.h)) {
            dr = d, dyn = true, found = true, mode = m, cw = max_w, ch = max_h;
            break;
        }
        if (inside(s.w, s.h)) {
            dr = d, dyn = false, found = true, mode = m, cw = s.w, ch = s.h;
            break;
        }
    }
    if (!any_range) found = true;  // NGX gave no range at all: create for the input as it is
    if (!found) {
        static std::uint64_t refused = 0;
        ++refused;
        event(std::format("eye {}: input {}x{} outside every input range NGX reports for the output {}x{}; not evaluated", eye, s.w, s.h, ow, oh));
        if ((refused & (refused - 1)) == 0)
            log::warn("dlss: eye {}: the input {}x{} is outside every input range NGX reports for the output {}x{} ({} times); the game's pass runs. "
                      "If the NVIDIA App's DLSS override for this game forces DLAA, set its Super Resolution override back to the application's choice",
                      eye, s.w, s.h, ow, oh, refused);
    }
    Feature& f = R.feat[eye];
    if (found && ensure_feature(ctx, eye, cw, ch, created, ow, oh, g_set.up_hdr.load(), dyn, dr.min_w, dr.min_h, static_cast<int>(mode)) &&
        s.w <= f.w && s.h <= f.h && s.w >= f.min_w && s.h >= f.min_h) {
        Timing* t = begin_timing(ctx, eye);
        if (t) ctx->End(t->t1);
        const bool gap = !created && f.last_eval_frame + 1 < R.frame;
        const bool requested = g_set.reset_requests.load() > 0;
        const bool reset = created || gap || requested || s.reset;
        // Inputs and output as NGX gets them: the shared double-wide textures at the eye's
        // rectangle, or (test_copy_inputs) the eye's own copies at the origin.
        bool copies = !f.subrects;
        if (copies) {
            if (in_game_state) {
                // In the game's own state, what the copies change put back (no state swap).
                SavedState saved(ctx);
                copies = prepare_copies(ctx, eye, g_graded.Get(), g_graded_srv.Get(), s.depth_srv.Get(), s.mv.Get(), s.mbx, s.mby, s.x, s.y, s.w, s.h, f.w, f.h, ow, oh,
                                        DXGI_FORMAT_R10G10B10A2_UNORM, true);
            } else {
                copies = prepare_copies(ctx, eye, g_graded.Get(), g_graded_srv.Get(), s.depth_srv.Get(), s.mv.Get(), s.mbx, s.mby, s.x, s.y, s.w, s.h, f.w, f.h, ow, oh,
                                        DXGI_FORMAT_R10G10B10A2_UNORM, false);
            }
        }
        EyeCopies& cp = g_copies[eye];
        // test_copy_inputs: which inputs come from the copies (bit 0 colour, bit 1 depth, bit 2
        // motion vectors); the output always goes to the eye's own texture then.
        const int mask = copies ? g_set.copy_mask.load(std::memory_order_relaxed) : 0;
        const bool c_col = mask & 1, c_dep = mask & 2, c_mv = mask & 4;
        ID3D11Resource* in_color = c_col ? cp.color.Get() : static_cast<ID3D11Resource*>(g_graded.Get());
        ID3D11Resource* in_depth = c_dep ? cp.depth.Get() : s.depth.Get();
        ID3D11Resource* in_mv = c_mv ? cp.mv.Get() : static_cast<ID3D11Resource*>(s.mv.Get());
        ID3D11Resource* in_out = copies ? cp.out.Get() : g_up_out.Get();
        const UINT bx = c_col ? 0 : s.x, by = c_col ? 0 : s.y, obx = copies ? 0 : ox, oby = copies ? 0 : oy;
        const UINT dbx = c_dep ? 0 : s.x, dby = c_dep ? 0 : s.y, mbx = c_mv ? 0 : s.mbx, mby = c_mv ? 0 : s.mby;
        EvalInputs e;
        e.eye = eye;
        e.upscale = true;
        e.handle = f.handle;
        e.fw = f.w;
        e.fh = f.h;
        e.fminw = f.min_w;
        e.fminh = f.min_h;
        e.fow = f.ow;
        e.foh = f.oh;
        e.color = in_color;
        e.depth = in_depth;
        e.mv = in_mv;
        e.out = in_out;
        e.x = bx;
        e.y = by;
        e.dx = dbx;
        e.dy = dby;
        e.mx = mbx;
        e.my = mby;
        e.w = s.w;
        e.h = s.h;
        e.ox = obx;
        e.oy = oby;
        e.jx = s.jx;
        e.jy = s.jy;
        e.reset = reset;
        NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
        bool checked = (copies || f.subrects) && before_eval(e);
        // The engine runs post-processing view by view (left: anti-aliasing ... last pass, then
        // right), so at an eye's last pass that eye's anti-aliasing pass must have run this frame.
        if (checked && g_set.validate.load(std::memory_order_relaxed) && !(g_taa_frame == R.frame && (g_taa_mask & (1u << eye)))) {
            const std::uint64_t n = ++g_check_fail[kChkEyes];
            event(std::format("CHECK FAILED eye {}: its anti-aliasing pass not seen this frame (passes 0x{:x} of frame {})", eye, g_taa_mask, g_taa_frame));
            if (n == 1 || (n & (n - 1)) == 0)
                log::warn("dlss: eye {}: check 'pass order' failed ({} times): the eye's anti-aliasing pass was not seen this frame; the game's pass runs instead",
                          eye, n);
            checked = false;
        }
        if (checked) {
            note_reset(eye, R.frame, created, gap, s.reset, requested);
            if (!in_game_state) {
                if (copies) run_stats(ctx, eye, cp.mv_srv.Get(), cp.depth_srv.Get(), nullptr, true, 0, 0, s.w, s.h);
                else if (s.mbx == s.x && s.mby == s.y) run_stats(ctx, eye, s.mv_srv.Get(), s.depth_srv.Get(), g_graded_srv.Get(), true, s.x, s.y, s.w, s.h);
            }
            NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
            ep.Feature.InSharpness = g_set.sharpness.load();
            ep.InPreExposure = g_set.pre_exposure.load();
            ep.Feature.pInColor = in_color;
            ep.Feature.pInOutput = in_out;
            ep.pInDepth = in_depth;
            ep.pInMotionVectors = in_mv;
            ep.InJitterOffsetX = s.jx;
            ep.InJitterOffsetY = s.jy;
            ep.InRenderSubrectDimensions = {s.w, s.h};
            ep.InReset = reset ? 1 : 0;
            ep.InMVScaleX = 1.0f;
            ep.InMVScaleY = 1.0f;
            ep.InColorSubrectBase = {bx, by};
            ep.InDepthSubrectBase = {dbx, dby};
            ep.InMVSubrectBase = {mbx, mby};
            ep.InOutputSubrectBase = {obx, oby};
            ep.InFrameTimeDeltaInMsec = R.frame_dt_ms;
            g_in_ngx = true;
            r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f.handle, f.params, &ep);
            g_in_ngx = false;
            if (!in_game_state) ctx->ClearState();
            if (g_set.flush_after.load(std::memory_order_relaxed)) ctx->Flush();
            event(std::format("eval eye {} result {}", eye, result_text(r)));
        }
        if (NVSDK_NGX_SUCCEED(r)) {
            if (g_set.no_copyout.load(std::memory_order_relaxed)) {
                // Test: the output is not used (the eye keeps the game's own image).
            } else if (!copy_back) {
                // output = runtime: the output texture itself goes to the runtime.
                if (copies) {
                    const D3D11_BOX box{0, 0, 0, ow, oh, 1};
                    ctx->CopySubresourceRegion(g_up_out.Get(), 0, ox, oy, 0, in_out, 0, &box);
                }
            } else if (copies) {
                const D3D11_BOX box{0, 0, 0, ow, oh, 1};
                ctx->CopySubresourceRegion(target_res, 0, ox, oy, 0, in_out, 0, &box);
            } else {
                const D3D11_BOX box{ox, oy, 0, ox + ow, oy + oh, 1};
                ctx->CopySubresourceRegion(target_res, 0, ox, oy, 0, g_up_out.Get(), 0, &box);
            }
            if (eye == 0 && !copies) cuttest_on_output(ctx, g_up_out.Get(), ox, oy, ow, oh, R.frame);
            if (g_framedump.remaining.load(std::memory_order_relaxed) > 0) {
                DumpEye& de = g_dump_eye[eye];
                de.frame = R.frame;
                de.x = s.x;
                de.y = s.y;
                de.w = s.w;
                de.h = s.h;
                de.ox = ox;
                de.oy = oy;
                de.ow = ow;
                de.oh = oh;
                de.out = copies ? cp.out : g_up_out;
                de.out_x = copies ? 0 : ox;
                de.out_y = copies ? 0 : oy;
                de.mv = s.mv;
                de.mbx = s.mbx;
                de.mby = s.mby;
                de.jx = s.jx;
                de.jy = s.jy;
                de.reset = reset;
                de.rows = s.rows;
            }
            f.last_eval_frame = R.frame;
            g_full[eye].frame = R.frame;
            g_full[eye].rect = EyeRect{static_cast<std::int32_t>(ox), static_cast<std::int32_t>(oy), ow, oh};
            ok = true;
            ++g_up_count;
            with_info([&](Info& i) {
                i.jitter_px[eye][0] = s.jx;
                i.jitter_px[eye][1] = s.jy;
                if (eye == 0)
                    i.up_sizes = std::format("feature {}x{} (largest input) | input {}x{} at {},{} | output {}x{} at {},{} (view rect {}x{})", f.w, f.h, s.w,
                                             s.h, s.x, s.y, ow, oh, ox, oy, vp_w, vp_h);
            });
        } else if (checked) {
            ++g_up_fail;
            with_info([&](Info& i) {
                ++i.eval_fail;
                i.last_error = std::format("upscale EvaluateFeature {}", result_text(r));
            });
        } else {
            with_info([&](Info& i) { i.last_error = "a check before the evaluation failed (see the log)"; });
        }
        if (t) {
            ctx->End(t->t2);
            ctx->End(t->disjoint);
            t->pending = true;
        }
    } else {
        ++g_up_fail;
    }
    if (!in_game_state) {
        ctx1->SwapDeviceContextState(game_state, nullptr);
        if (game_state) game_state->Release();
    }
    return ok;
}

ComPtr<ID3D11Texture2D> make_staging(UINT w, UINT h, DXGI_FORMAT f) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g_rhi->dev->CreateTexture2D(&d, nullptr, &t))) t.Reset();
    return t;
}

// RHI thread, frame end (`ended` is the frame that has just finished): the copies of
// `dlss frames`, then the files of the copies the GPU has finished.
void framedump_frame(ID3D11DeviceContext* ctx, std::uint64_t ended) {
    FrameDump& d = g_framedump;
    std::string prefix;
    {
        std::lock_guard lock(d.m);
        prefix = d.prefix;
    }
    if (d.remaining.load() > 0 && g_graded && g_rhi->dev) {
        std::string meta;
        for (int e = 0; e < 2; ++e) {
            const DumpEye& de = g_dump_eye[e];
            if (de.frame != ended || de.w == 0 || de.h == 0 || !de.mv) continue;
            const UINT c = static_cast<UINT>(std::max(64, d.crop.load()));
            const UINT cw = std::min(de.w, c), ch = std::min(de.h, c);
            const UINT cx = de.x + (de.w - cw) / 2, cy = de.y + (de.h - ch) / 2;
            FrameDump::Slot s;
            s.color = make_staging(cw, ch, g_graded_desc.Format);
            s.mv = make_staging(cw, ch, DXGI_FORMAT_R16G16_FLOAT);
            if (!s.color || !s.mv) continue;
            const D3D11_BOX box{cx, cy, 0, cx + cw, cy + ch, 1};
            ctx->CopySubresourceRegion(s.color.Get(), 0, 0, 0, 0, g_graded.Get(), 0, &box);
            const D3D11_BOX mbox{de.mbx + (cx - de.x), de.mby + (cy - de.y), 0, de.mbx + (cx - de.x) + cw, de.mby + (cy - de.y) + ch, 1};
            ctx->CopySubresourceRegion(s.mv.Get(), 0, 0, 0, 0, de.mv.Get(), 0, &mbox);
            // The same part of the eye in DLSS's output (the output rectangle scaled like the input)
            UINT ocx = 0, ocy = 0, ocw = 0, och = 0;
            if (de.out && de.ow && de.oh) {
                const double sx = static_cast<double>(de.ow) / de.w, sy = static_cast<double>(de.oh) / de.h;
                ocw = std::min(de.ow, static_cast<UINT>(std::lround(cw * sx)));
                och = std::min(de.oh, static_cast<UINT>(std::lround(ch * sy)));
                ocx = de.ox + (de.ow - ocw) / 2;
                ocy = de.oy + (de.oh - och) / 2;
                D3D11_TEXTURE2D_DESC od{};
                de.out->GetDesc(&od);
                s.out = make_staging(ocw, och, od.Format);
                if (s.out) {
                    const UINT sx0 = de.out_x + (ocx - de.ox), sy0 = de.out_y + (ocy - de.oy);
                    const D3D11_BOX obox{sx0, sy0, 0, sx0 + ocw, sy0 + och, 1};
                    ctx->CopySubresourceRegion(s.out.Get(), 0, 0, 0, 0, de.out.Get(), 0, &obox);
                    s.out_w = ocw;
                    s.out_h = och;
                }
            }
            s.frame = ended;
            s.index = d.index;
            s.eye = e;
            s.w = cw;
            s.h = ch;
            s.pending = true;
            d.slots.push_back(std::move(s));
            meta += std::format("dump {} frame {} eye {} rect {} {} {} {} crop {} {} {} {} jitter_px {:.6f} {:.6f} reset {} out {} {} {} {} outcrop {} {} {} {} rows",
                                d.index, ended, e, de.x, de.y, de.w, de.h, cx, cy, cw, ch, de.jx, de.jy, de.reset ? 1 : 0, de.ox, de.oy, de.ow, de.oh, ocx,
                                ocy, ocw, och);
            for (int r = 0; r < kRowCount; ++r)
                meta += std::format(" {:.9g} {:.9g} {:.9g} {:.9g}", de.rows.v[r][0], de.rows.v[r][1], de.rows.v[r][2], de.rows.v[r][3]);
            meta += "\n";
        }
        if (!meta.empty()) {
            ++d.index;
            --d.remaining;
            FILE* f = nullptr;
            if (fopen_s(&f, (prefix + "_meta.txt").c_str(), "ab") == 0 && f) {
                fwrite(meta.data(), 1, meta.size(), f);
                fclose(f);
            }
        }
    }
    for (auto it = d.slots.begin(); it != d.slots.end();) {
        FrameDump::Slot& s = *it;
        if (s.frame + 2 > ended) {
            ++it;
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mc{}, mm{};
        if (ctx->Map(s.color.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mc) != S_OK) {
            ++it;
            continue;
        }
        if (ctx->Map(s.mv.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mm) != S_OK) {
            ctx->Unmap(s.color.Get(), 0);
            ++it;
            continue;
        }
        const std::string base = std::format("{}_f{}_e{}", prefix, s.index, s.eye);
        FILE* f = nullptr;
        if (fopen_s(&f, std::format("{}_color_{}x{}.r10g10b10a2", base, s.w, s.h).c_str(), "wb") == 0 && f) {
            for (UINT y = 0; y < s.h; ++y) fwrite(static_cast<const std::uint8_t*>(mc.pData) + static_cast<std::size_t>(y) * mc.RowPitch, 4, s.w, f);
            fclose(f);
        }
        if (fopen_s(&f, std::format("{}_mv_{}x{}.rg16f", base, s.w, s.h).c_str(), "wb") == 0 && f) {
            for (UINT y = 0; y < s.h; ++y) fwrite(static_cast<const std::uint8_t*>(mm.pData) + static_cast<std::size_t>(y) * mm.RowPitch, 4, s.w, f);
            fclose(f);
        }
        ctx->Unmap(s.color.Get(), 0);
        ctx->Unmap(s.mv.Get(), 0);
        D3D11_MAPPED_SUBRESOURCE mo{};
        if (s.out && ctx->Map(s.out.Get(), 0, D3D11_MAP_READ, 0, &mo) == S_OK) {  // copied with the others: finished too
            if (fopen_s(&f, std::format("{}_out_{}x{}.r10g10b10a2", base, s.out_w, s.out_h).c_str(), "wb") == 0 && f) {
                for (UINT y = 0; y < s.out_h; ++y)
                    fwrite(static_cast<const std::uint8_t*>(mo.pData) + static_cast<std::size_t>(y) * mo.RowPitch, 4, s.out_w, f);
                fclose(f);
            }
            ctx->Unmap(s.out.Get(), 0);
        }
        ++d.written;
        it = d.slots.erase(it);
    }
}

ComPtr<ID3D11Texture2D> make_tex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) t.Reset();
    return t;
}

// RHI thread, once per frame while "dlss bench" is set: one DLSS evaluation of the chosen
// sizes on blank textures, timed like the eyes (slot 2). Measures the cost of a mode
// without changing the engine's render size.
void run_bench(ID3D11DeviceContext* ctx) {
    Rhi& R = *g_rhi;
    const UINT iw = g_set.bench_in_w.load(), ih = g_set.bench_in_h.load(), ow = g_set.bench_out_w.load(), oh = g_set.bench_out_h.load();
    if (!iw || !ih || !ow || !oh || iw > ow || ih > oh || !ensure_shaders(R.dev)) return;
    if (!R.bench_out || R.bench_out_w != ow || R.bench_out_h != oh || R.bench.w != iw || R.bench.h != ih) {
        retire_feature(R.bench);
        R.bench_color = make_tex(R.dev, iw, ih, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        R.bench_depth = make_tex(R.dev, iw, ih, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        R.bench_mv = make_tex(R.dev, iw, ih, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        R.bench_out = make_tex(R.dev, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        R.bench_out_w = ow;
        R.bench_out_h = oh;
        if (!R.bench_color || !R.bench_depth || !R.bench_mv || !R.bench_out) {
            g_set.bench = false;
            return;
        }
    }
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return;
    ID3DDeviceContextState* game_state = nullptr;
    ctx1->SwapDeviceContextState(R.state.Get(), &game_state);
    if (!R.bench.handle) {
        const double ratio = static_cast<double>(iw) / static_cast<double>(ow);
        const NVSDK_NGX_PerfQuality_Value q = iw == ow   ? NVSDK_NGX_PerfQuality_Value_DLAA
                                              : ratio >= 0.66 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                              : ratio >= 0.57 ? NVSDK_NGX_PerfQuality_Value_Balanced
                                              : ratio >= 0.49 ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                              : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        const unsigned preset = g_set.preset.load();
        NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
        NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
        NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
        NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
        NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = iw;
        cp.Feature.InHeight = ih;
        cp.Feature.InTargetWidth = ow;
        cp.Feature.InTargetHeight = oh;
        cp.Feature.InPerfQualityValue = q;
        cp.InFeatureCreateFlags = wanted_flags();
        const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &R.bench.handle, R.params, &cp);
        const std::string text = std::format("bench {}x{} -> {}x{} quality value {} preset {}: create {}", iw, ih, ow, oh, static_cast<int>(q),
                                             preset_name(preset), result_text(r));
        log::info("dlss: {}", text);
        with_info([&](Info& i) {
            i.bench = text;
            i.ms_sum[2] = i.mv_ms_sum[2] = 0;
            i.ms_n[2] = 0;
        });
        if (NVSDK_NGX_FAILED(r)) {
            R.bench = Feature{};
            g_set.bench = false;
        } else {
            R.bench.w = iw;
            R.bench.h = ih;
        }
    }
    if (R.bench.handle) {
        Timing* t = begin_timing(ctx, 2);
        if (t) ctx->End(t->t1);
        NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
        ep.Feature.pInColor = R.bench_color.Get();
        ep.Feature.pInOutput = R.bench_out.Get();
        ep.pInDepth = R.bench_depth.Get();
        ep.pInMotionVectors = R.bench_mv.Get();
        ep.InRenderSubrectDimensions = {iw, ih};
        ep.InMVScaleX = 1.0f;
        ep.InMVScaleY = 1.0f;
        ep.InFrameTimeDeltaInMsec = R.frame_dt_ms;
        if (g_set.bench_jitter.load()) {
            auto halton = [](unsigned i, unsigned b) {
                float f = 1.0f, r = 0.0f;
                while (i > 0) {
                    f /= static_cast<float>(b);
                    r += f * static_cast<float>(i % b);
                    i /= b;
                }
                return r;
            };
            const unsigned k = static_cast<unsigned>(R.frame % 8) + 1;
            ep.InJitterOffsetX = halton(k, 2) - 0.5f;
            ep.InJitterOffsetY = halton(k, 3) - 0.5f;
        }
        if (g_set.bench_write.load()) {
            // New input values every frame, written as render targets like the real inputs.
            ID3D11Resource* targets[3] = {R.bench_color.Get(), R.bench_depth.Get(), R.bench_mv.Get()};
            const float v = 0.02f + 0.002f * static_cast<float>(R.frame % 8);
            const float values[3][4] = {{v, v, v, 1.0f}, {0.5f + v, 0, 0, 0}, {0, 0, 0, 0}};
            for (int k = 0; k < 3; ++k) {
                ComPtr<ID3D11RenderTargetView> rtv;
                if (SUCCEEDED(R.dev->CreateRenderTargetView(targets[k], nullptr, &rtv))) ctx->ClearRenderTargetView(rtv.Get(), values[k]);
            }
        }
        const int repeat = std::clamp(g_set.bench_repeat.load(), 1, 8);
        for (int k = 0; k < repeat; ++k) {
            g_in_ngx = true;
            const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, R.bench.handle, R.params, &ep);
            g_in_ngx = false;
            static std::uint64_t bench_events = 0;
            if ((bench_events++ & 63) == 0) event(std::format("bench evaluation {} x {}: {}", repeat, bench_events, result_text(r)));
        }
        ctx->ClearState();
        if (g_set.bench_copyout.load()) {
            if (!R.bench_copy) R.bench_copy = make_tex(R.dev, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
            if (R.bench_copy) ctx->CopyResource(R.bench_copy.Get(), R.bench_out.Get());
        }
        if (t) {
            ctx->End(t->t2);
            ctx->End(t->disjoint);
            t->pending = true;
        }
    }
    ctx1->SwapDeviceContextState(game_state, nullptr);
    if (game_state) game_state->Release();
}

}  // namespace

void start(const Config& cfg, const std::filesystem::path& dll_dir) {
    g_dll_dir = dll_dir;
    g_set.enabled = cfg.get_bool("dlss", "enabled", false);
    g_set.init_requested = cfg.get_bool("dlss", "init", false);
    unsigned p = 0;
    if (parse_preset(cfg.get_string("dlss", "preset", "default"), p)) g_set.preset = p;
    g_set.auto_exposure = cfg.get_bool("dlss", "auto_exposure", true);
    g_set.mv_jitter = static_cast<int>(cfg.get_int("dlss", "mv_jitter", 2));
    g_set.cut_reset = cfg.get_bool("dlss", "camera_cut_reset", true);
    g_set.cut_distance = static_cast<float>(cfg.get_float("dlss", "cut_distance", 100.0));
    g_set.cut_angle = static_cast<float>(cfg.get_float("dlss", "cut_angle", 30.0));
    g_set.log_ngx = cfg.get_bool("dlss", "log_ngx", true);
    g_set.log_verbose = cfg.get_bool("dlss", "log_verbose", false);
    g_set.log_lines = static_cast<int>(cfg.get_int("dlss", "log_lines", 0));
    g_set.validate = cfg.get_bool("dlss", "validate", true);
    g_set.input_stats = cfg.get_bool("dlss", "input_stats", false);
    g_set.own_params = cfg.get_string("dlss", "params", "shared") == "feature";
    // Fault isolation tests, from the first frame (docs/dlss.md, "Dev commands")
    g_set.test_eyes = static_cast<int>(cfg.get_int("dlss", "test_eyes", 0));
    g_set.zero_mv = cfg.get_bool("dlss", "test_zero_mv", false);
    g_set.mv_sanitize = cfg.get_bool("dlss", "test_mv_sanitize", false);
    g_set.flush_after = static_cast<int>(cfg.get_int("dlss", "test_flush", 0));
    g_set.own_state = cfg.get_string("dlss", "context_state", "game") == "own";
    g_set.skip_level = static_cast<int>(cfg.get_int("dlss", "test_skip", 0));
    g_set.skip_eval = g_set.skip_level.load() >= 1;
    g_set.no_ngx = cfg.get_bool("dlss", "test_no_ngx", false);
    g_set.copy_inputs = cfg.get_bool("dlss", "test_copy_inputs", false);
    g_set.mv_per_eye = cfg.get_string("dlss", "mv_textures", "eye") != "shared";
    g_set.eval_at_end = cfg.get_string("dlss", "eval_at", "pass") == "frame_end";
    g_set.no_copyout = cfg.get_bool("dlss", "test_no_copyout", false);
    {
        // test_jitter = <sx> <sy>: scale of the jitter handed to DLSS (default 0.5 -0.5; 0 0 passes no jitter)
        const std::string j = cfg.get_string("dlss", "test_jitter", "");
        float sx = 0, sy = 0;
        if (!j.empty() && sscanf_s(j.c_str(), "%f %f", &sx, &sy) == 2) {
            g_set.jitter_scale_x = sx;
            g_set.jitter_scale_y = sy;
            log::info("dlss: jitter scale {} {} (test_jitter)", sx, sy);
        }
    }
    g_color_floor = static_cast<float>(cfg.get_float("dlss", "test_color_floor", 0.0));
    g_color_noise = static_cast<float>(cfg.get_float("dlss", "test_color_noise", 0.0));
    g_depth_const = static_cast<float>(cfg.get_float("dlss", "test_depth_const", 0.0));
    {
        // test_bench = <out w> <out h> <in w> <in h> [evaluations per frame]: the synthetic bench
        // from the first stereo frame (NGX starts even with enabled = 0)
        const std::string b = cfg.get_string("dlss", "test_bench", "");
        unsigned v[5] = {0, 0, 0, 0, 1};
        std::size_t i = 0, n = 0;
        while (i < b.size() && n < 5) {
            while (i < b.size() && (b[i] == ' ' || b[i] == 'x' || b[i] == ',')) ++i;
            std::size_t j = i;
            while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
            if (j == i) break;
            v[n++] = static_cast<unsigned>(std::stoul(b.substr(i, j - i)));
            i = j;
        }
        if (n >= 4 && v[0] && v[1] && v[2] && v[3]) {
            g_set.bench_out_w = v[0];
            g_set.bench_out_h = v[1];
            g_set.bench_in_w = v[2];
            g_set.bench_in_h = v[3];
            g_set.bench_repeat = static_cast<int>(std::clamp(v[4], 1u, 8u));
            g_set.bench_jitter = cfg.get_bool("dlss", "test_bench_jitter", false);
            g_set.bench_write = cfg.get_bool("dlss", "test_bench_write", false);
            g_set.bench_copyout = cfg.get_bool("dlss", "test_bench_copyout", false);
            g_set.bench = true;
            g_set.init_requested = true;
            log::info("dlss: bench test from the start: {}x{} from {}x{}, {} evaluation(s) per frame", v[0], v[1], v[2], v[3], v[4]);
        }
    }
    const std::string extra = cfg.get_string("dlss", "dll_dir", "");
    if (!extra.empty()) g_extra_dll_dir = log::widen(extra);
    const std::string mode = cfg.get_string("dlss", "mode", "upscale");
    g_set.mode = mode == "dlaa" ? 0 : 1;
    if (mode != "dlaa" && mode != "upscale") log::warn("dlss: mode '{}' unknown; upscale used", mode);
    // [dlss] input_scale: the share of each eye's width and height rendered before DLSS scales
    // it up. It sets [stereo] render_scale (the view rects; with dynamic resolution its upper
    // bound) while DLSS upscales; r.ScreenPercentage, if also set, multiplies with it.
    const double input = cfg.get_float("dlss", "input_scale", 0.5);
    {
        const std::string out = cfg.get_string("dlss", "output", "runtime");
        g_set.output_runtime = out != "engine";
        if (out != "runtime" && out != "engine") log::warn("dlss: output '{}' unknown; runtime used", out);
    }
    g_set.engine_scale = input > 0.0 ? static_cast<float>(std::clamp(input, 0.3, 1.0)) : 1.0f;
    if (g_set.enabled.load() && g_set.mode.load() == 1 && g_set.output_runtime.load()) {
        log::info("dlss: output = runtime: the engine renders each eye at input_scale {:.2f} of the runtime's eye size, DLSS writes the runtime's size",
                  g_set.engine_scale.load());
    } else if (g_set.enabled.load() && g_set.mode.load() == 1 && input > 0.0) {
        const float s = static_cast<float>(std::clamp(input, 0.3, 1.0));
        device::settings().render_scale = s;
        log::info("dlss: input_scale {:.2f}: [stereo] render_scale set to it (output = engine)", s);
    }
    log::info("dlss: built in; {} (mode {}, preset {}, auto exposure {}, motion vector jitter mode {}{})",
              g_set.enabled.load() ? "ON" : "off ([dlss] enabled = 0)", g_set.mode.load() ? "upscale" : "dlaa", preset_name(g_set.preset.load()),
              g_set.auto_exposure.load() ? "on" : "off", g_set.mv_jitter.load(), extra.empty() ? "" : ", extra library folder " + extra);
    log::info("dlss: evaluations at {}; motion vectors and NGX in {}", g_set.eval_at_end.load() ? "the frame end ([dlss] eval_at = frame_end)" : "each eye's last pass",
              g_set.own_state.load() ? "our own device context state ([dlss] context_state = own)" : "the game's pipeline state");
    log::info("dlss: checks {}, input statistics {}, parameter maps {}; tests: eyes {} zero mv {} mv sanitize {} flush {} own context state {} skip {} copy inputs {}",
              g_set.validate.load() ? "on" : "off", g_set.input_stats.load() ? "on" : "off", g_set.own_params.load() ? "one per feature" : "shared",
              g_set.test_eyes.load(), g_set.zero_mv.load() ? 1 : 0, g_set.mv_sanitize.load() ? 1 : 0, g_set.flush_after.load(),
              g_set.own_state.load() ? 1 : 0, g_set.skip_eval.load() ? 1 : 0, g_set.copy_inputs.load() ? 1 : 0);
}

bool wants_hooks() { return g_set.enabled.load(std::memory_order_relaxed) || g_set.init_requested.load(std::memory_order_relaxed); }

void output_rects(EyeRect rects[2]) {
    const std::uint64_t ended = g_rhi->frame - 1;  // frame() has already counted the frame that just ended
    for (int e = 0; e < 2; ++e)
        if (g_full[e].frame != 0 && g_full[e].frame == ended) rects[e] = g_full[e].rect;
}

void engine_eye_size(std::uint32_t& w, std::uint32_t& h) {
    g_runtime_eye.store((static_cast<std::uint64_t>(w) << 32) | h, std::memory_order_relaxed);
    if (!runtime_output()) return;
    const float s = g_set.engine_scale.load(std::memory_order_relaxed);
    if (s >= 0.999f) return;
    // Multiples of 4, like the engine's scene buffer sizes.
    auto scaled = [s](std::uint32_t full) {
        const auto v = static_cast<std::uint32_t>(std::lround(full * s / 4.0) * 4);
        return std::clamp<std::uint32_t>(v, 64, full);
    };
    w = scaled(w);
    h = scaled(h);
}

void output_texture(EyeTexture& eyes) {
    const std::uint64_t ended = g_rhi->frame - 1;
    if (g_rt_out.frame != ended || !g_up_out || !eyes.texture) {
        output_rects(eyes.eyes);
        return;
    }
    const UINT ow = g_rt_out.ow, oh = g_rt_out.oh;
    ComPtr<ID3D11DeviceContext> ctx;
    {
        ComPtr<ID3D11Device> dev;
        eyes.texture->GetDevice(&dev);
        if (dev) dev->GetImmediateContext(&ctx);
    }
    if (!ctx) {
        output_rects(eyes.eyes);
        return;
    }
    for (int e = 0; e < 2; ++e) {
        if (g_full[e].frame == ended) {
            eyes.eyes[e] = g_full[e].rect;
            continue;
        }
        // DLSS did not write this eye: its image at the engine's size goes into the output
        // texture's half at the corner, and the runtime scales it.
        const EyeRect r = eyes.eyes[e];
        const UINT w = std::min(r.width, ow), h = std::min(r.height, oh);
        const D3D11_BOX box{static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0, static_cast<UINT>(r.x) + w, static_cast<UINT>(r.y) + h, 1};
        ctx->CopySubresourceRegion(g_up_out.Get(), 0, static_cast<UINT>(e) * ow, 0, 0, eyes.texture, 0, &box);
        eyes.eyes[e] = EyeRect{static_cast<std::int32_t>(static_cast<UINT>(e) * ow), 0, w, h};
        ++g_rt_out.fallback_eyes;
    }
    eyes.texture = g_up_out.Get();
    ++g_rt_out.handed;
}

void frame(ID3D11Texture2D* any_texture) {
    Rhi& R = *g_rhi;
    if ((g_pending[0].target || g_pending[1].target) && any_texture) {
        ID3D11Device* d = nullptr;
        any_texture->GetDevice(&d);
        if (d) {
            ID3D11DeviceContext* c = nullptr;
            d->GetImmediateContext(&c);
            if (c) {
                run_pending(c);
                c->Release();
            }
            d->Release();
        }
    }
    if (R.ngx_state == 1) event(std::format("frame end (anti-aliasing passes 0x{:x})", g_taa_frame == R.frame ? g_taa_mask : 0u));
    ++R.frame;
    g_rhi_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (R.last_frame_time.time_since_epoch().count() != 0) {
        const float dt = std::chrono::duration<float, std::milli>(now - R.last_frame_time).count();
        if (dt > 0.0f && dt < 200.0f) R.frame_dt_ms = dt;
    }
    R.last_frame_time = now;
    with_info([](Info& i) {
        i.seen_last_frame = i.seen_this_frame;
        i.seen_this_frame = 0;
    });
    if (g_set.reset_requests.load() > 0) g_set.reset_requests.fetch_sub(1);
    // Once NGX runs, every frame end is needed to release what was retired, also while off.
    if ((!wants_hooks() && R.ngx_state != 1) || !any_texture) return;
    ID3D11Device* dev = nullptr;
    any_texture->GetDevice(&dev);
    if (!dev) return;
    g_rhi_dev.store(dev, std::memory_order_relaxed);
    static bool removed_logged = false;
    if (!removed_logged) {
        const HRESULT rr = dev->GetDeviceRemovedReason();
        if (rr != S_OK) {
            removed_logged = true;
            log::error("dlss: the D3D11 device was removed, reason 0x{:08x} ({}); DLSS {}, {} features waiting for release", static_cast<unsigned>(rr),
                       rr == DXGI_ERROR_DEVICE_HUNG     ? "hung: this process's commands"
                       : rr == DXGI_ERROR_DEVICE_RESET  ? "reset: another process's commands"
                       : rr == DXGI_ERROR_DEVICE_REMOVED ? "removed"
                       : rr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "driver internal error"
                                                                : "other",
                       g_set.enabled.load() ? "on" : "off", g_retire_waiting.load());
            dump_events("the device was removed");
        }
    }
    if (g_set.events_requests.exchange(0) > 0) dump_events("dlss events");
    {
        static auto last_vram = std::chrono::steady_clock::time_point{};
        if (R.ngx_state == 1 && now - last_vram > std::chrono::seconds(2)) {
            last_vram = now;
            std::string v = vram_text(dev);
            event(v);
            with_info([&](Info& i) { i.vram = std::move(v); });
        }
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        install_ub_hooks(dev, ctx);
        if (R.ngx_state == 0 && wants_hooks()) {
            if (g_set.no_ngx.load() && g_set.skip_level.load() >= 1) {
                // Test: the DLSS path without NGX (no evaluation is made with test_skip).
                R.dev = dev;
                R.ngx_state = 1;
                log::info("dlss: NGX not initialised (test_no_ngx with test_skip)");
            } else {
                init_ngx(dev);
                if (R.ngx_state < 0 && !g_ngx_failed.exchange(true) && g_set.output_runtime.load())
                    log::warn("dlss: NGX is not available; the engine renders at the runtime's eye size again ([dlss] output = runtime)");
            }
        }
        // Features hold several hundred MB of video memory each: release them while DLSS is off.
        if (!g_set.enabled.load() && (R.feat[0].handle || R.feat[1].handle)) {
            retire_feature(R.feat[0]);
            retire_feature(R.feat[1]);
            with_info([](Info& i) { i.features[0] = i.features[1] = "released (off)"; });
            log::info("dlss: features released (off)");
        }
        if (g_set.recreate_requests.exchange(0) > 0) {
            retire_feature(R.feat[0]);
            retire_feature(R.feat[1]);
            retire_feature(R.bench);
            with_info([](Info& i) { i.features[0] = i.features[1] = "released"; });
        }
        collect_timing(ctx);
        collect_stats(ctx);
        cuttest_collect(ctx, R.frame);
        if (g_framedump.remaining.load(std::memory_order_relaxed) > 0 || !g_framedump.slots.empty()) framedump_frame(ctx, R.frame - 1);
        if (g_set.bench.load() && R.ngx_state == 1) run_bench(ctx);
        else if (R.bench.handle) retire_feature(R.bench);
        retire_frame_end(dev, ctx, R.frame);
        if (g_set.stall_requests.load() > 0) {
            // Test (`dlss stall`): what a blocking read-back does to the frame, without one: the
            // thread waits until the GPU has finished everything, then stays away a while.
            g_set.stall_requests.fetch_sub(1);
            const auto t0 = std::chrono::steady_clock::now();
            ComPtr<ID3D11Query> q;
            D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            bool done_ok = false;
            if (SUCCEEDED(dev->CreateQuery(&qd, &q))) {
                ctx->End(q.Get());
                BOOL done = FALSE;
                for (;;) {
                    const HRESULT hr = ctx->GetData(q.Get(), &done, sizeof(done), 0);
                    if (hr == S_OK) {
                        done_ok = true;
                        break;
                    }
                    if (hr != S_FALSE || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) break;
                    std::this_thread::yield();
                }
            }
            const double wait = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const int idle = g_set.stall_ms.load();
            if (idle > 0) Sleep(static_cast<DWORD>(idle));
            const std::uint64_t n = ++g_stalls;
            if (n <= 5 || (n & (n - 1)) == 0)
                log::info("dlss: stall test {}: GPU idle after {:.2f} ms{}, then {} ms away", n, wait, done_ok ? "" : " (no answer)", idle);
        }
        ctx->Release();
    }
    dev->Release();
}

bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, gpu_trace::DrawIndexedFn original) {
    if (count != 3 || !wants_hooks() || !device::active()) return false;
    const bool upscale = g_set.mode.load(std::memory_order_relaxed) == 1;
    if (upscale && g_set.enabled.load(std::memory_order_relaxed) && g_rhi->ngx_state == 1 && try_final(ctx, count, start, base, original)) return true;
    PassInputs in;
    if (!read_pass(ctx, in, nullptr)) return false;
    const int eye = in.vp.TopLeftX >= 1.0f ? 1 : 0;
    with_info([](Info& i) {
        ++i.taa_seen;
        ++i.seen_this_frame;
    });
    // Test (`[dlss] test_eyes`): one eye only; the other eye runs the game's own passes.
    const int only = g_set.test_eyes.load(std::memory_order_relaxed);
    if ((only == 1 && eye != 0) || (only == 2 && eye != 1)) return false;
    if (g_set.skip_level.load(std::memory_order_relaxed) >= 4) return false;  // test: recognised only, the game's pass runs
    if (!g_set.enabled.load(std::memory_order_relaxed) || g_rhi->ngx_state != 1) {
        if (g_set.dump_requests.load() > 0) {
            ViewRows rows;
            if (lookup_rows(in.cb1.Get(), rows)) {
                g_set.dump_requests.fetch_sub(1);
                dump_rows(rows, in, eye);
            }
        }
        return false;
    }
    std::string why;
    const bool ok = upscale ? run_passthrough(ctx, in, eye, why) : run_dlss(ctx, in, eye, why);
    with_info([&](Info& i) {
        ++(ok ? i.replaced : i.fallback);
        if (!ok) i.last_error = why;
    });
    if (!ok && g_fallback_log.fetch_add(1) < 10) {
        // The view constants are captured from the first recognised pass on, so that pass
        // itself always falls back (expected at start-up, not a warning).
        const bool startup = g_ub_captures_map.load() + g_ub_captures_update.load() + g_ub_captures_create.load() == 0;
        if (startup) log::info("dlss: eye {}: the game's anti-aliasing runs this frame: {} (start-up: capture of the view constants begins with this pass)", eye, why);
        else log::warn("dlss: eye {}: the game's anti-aliasing runs instead: {}", eye, why);
    }
    return ok;
}

std::string command(const std::string& args) {
    std::vector<std::string> a;
    {
        std::size_t i = 0;
        while (i < args.size()) {
            while (i < args.size() && args[i] == ' ') ++i;
            const std::size_t j = args.find(' ', i);
            if (i < args.size()) a.push_back(args.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (j == std::string::npos) break;
            i = j;
        }
    }
    const std::string sub = a.empty() ? "status" : a[0];
    if (sub == "on" || sub == "off") {
        g_set.enabled = sub == "on";
        g_set.reset_requests = 2;
        log::info("dlss: switched {} (dev command)", sub);
        return std::string("ok dlss ") + sub;
    }
    if (sub == "mode" && a.size() == 2 && (a[1] == "dlaa" || a[1] == "upscale")) {
        g_set.mode = a[1] == "upscale" ? 1 : 0;
        g_set.reset_requests = 2;
        log::info("dlss: mode {} (dev command)", a[1]);
        return std::format("ok mode {}{}", a[1], a[1] == "upscale" ? " (set r.ScreenPercentage below 100 for fewer rendered pixels)" : "");
    }
    if (sub == "init") {
        g_set.init_requested = true;
        return "ok NGX initialises at the next stereo frame";
    }
    if (sub == "output" && a.size() >= 2 && (a[1] == "runtime" || a[1] == "engine")) {
        // runtime: the engine's eye target shrinks to input_scale of the runtime's size (a
        // reallocation), the views use all of it; engine: the target at the runtime's size,
        // the views at input_scale of it (render_scale).
        if (a.size() >= 3) g_set.engine_scale = static_cast<float>(std::clamp(std::atof(a[2].c_str()), 0.3, 1.0));
        g_set.output_runtime = a[1] == "runtime";
        device::settings().render_scale = a[1] == "runtime" ? 1.0f : g_set.engine_scale.load();
        g_set.reset_requests = 2;
        log::info("dlss: output {} at input_scale {:.2f} (dev command)", a[1], g_set.engine_scale.load());
        return std::format("ok output {} at input_scale {:.2f}", a[1], g_set.engine_scale.load());
    }
    if (sub == "preset" && a.size() == 2) {
        unsigned p = 0;
        if (!parse_preset(a[1], p)) return "err preset default|j|k|l|m";
        g_set.preset = p;
        log::info("dlss: preset {} (dev command)", preset_name(p));
        return std::format("ok preset {} (features are recreated)", preset_name(p));
    }
    if (sub == "autoexp" && a.size() == 2) {
        g_set.auto_exposure = a[1] == "1";
        return std::format("ok auto exposure {}", g_set.auto_exposure.load() ? "on" : "off");
    }
    if (sub == "mvjitter" && a.size() == 2) {
        g_set.mv_jitter = std::atoi(a[1].c_str());
        return std::format("ok motion vector jitter mode {}", g_set.mv_jitter.load());
    }
    if (sub == "jitter" && a.size() == 3) {
        g_set.jitter_scale_x = static_cast<float>(std::atof(a[1].c_str()));
        g_set.jitter_scale_y = static_cast<float>(std::atof(a[2].c_str()));
        g_set.reset_requests = 2;
        return std::format("ok jitter scale {} {}", g_set.jitter_scale_x.load(), g_set.jitter_scale_y.load());
    }
    if (sub == "cutreset" && a.size() == 2) {
        g_set.cut_reset = a[1] == "1";
        return std::format("ok camera cut reset {}", g_set.cut_reset.load() ? "on" : "off");
    }
    if (sub == "cutflag" && a.size() == 2) {
        g_set.cut_flag = a[1] == "1";
        return std::format("ok row 140 camera cut flag {}", g_set.cut_flag.load() ? "used" : "only counted");
    }
    if (sub == "cutlimits" && a.size() == 3) {
        g_set.cut_distance = static_cast<float>(std::atof(a[1].c_str()));
        g_set.cut_angle = static_cast<float>(std::atof(a[2].c_str()));
        return std::format("ok camera cut at more than {} cm or {} degrees in one frame", g_set.cut_distance.load(), g_set.cut_angle.load());
    }
    if (sub == "cuttest" && a.size() == 4) {
        {
            std::lock_guard lock(g_cuttest.m);
            g_cuttest.prefix = a[1];
        }
        g_cuttest.want_reset = a[2] == "1";
        g_cuttest.want_zero = a[3] == "1";
        g_cuttest.armed = true;
        with_info([&](Info& i) { i.cut_test = "armed: " + args; });
        return std::format("ok cut test armed (reset {}, motion vectors {}): the next camera cut writes {}_f<n>_<w>x<h>.r10g10b10a2",
                           a[2] == "1" ? "on" : "off", a[3] == "1" ? "zero" : "as computed", a[1]);
    }
    if (sub == "hdr" && a.size() == 2) {
        g_set.up_hdr = a[1] == "1";
        return std::format("ok upscale input flagged {} (features are recreated)", g_set.up_hdr.load() ? "HDR" : "display-referred (LDR)");
    }
    if (sub == "sharpness" && a.size() == 2) {
        g_set.sharpness = static_cast<float>(std::atof(a[1].c_str()));
        return std::format("ok sharpness {}", g_set.sharpness.load());
    }
    if (sub == "preexp" && a.size() == 2) {
        g_set.pre_exposure = static_cast<float>(std::atof(a[1].c_str()));
        return std::format("ok pre-exposure {}", g_set.pre_exposure.load());
    }
    if (sub == "maxinput" && a.size() == 2 && (a[1] == "full" || a[1] == "setting")) {
        g_set.max_full = a[1] == "full";
        return std::format("ok features sized for the input at {}", g_set.max_full.load() ? "render scale 1" : "[stereo] render_scale");
    }
    if (sub == "nograin" && a.size() == 2) {
        g_set.no_grain = a[1] == "1";
        return std::format("ok last pass noise texture {}", g_set.no_grain.load() ? "unbound at the reduced size" : "as the game binds it");
    }
    if (sub == "reset") {
        g_set.reset_requests = 2;
        return "ok history reset at the next frame";
    }
    if (sub == "skip" && a.size() == 2) {
        g_set.skip_eval = a[1] == "1";
        log::info("dlss: evaluation {} (dev command)", g_set.skip_eval.load() ? "skipped, the game's pass upscales" : "on");
        return std::format("ok upscale evaluation {}", g_set.skip_eval.load() ? "skipped (motion vectors and the graded copy still run)" : "on");
    }
    if (sub == "stall") {
        if (a.size() == 2) g_set.stall_ms = std::clamp(std::atoi(a[1].c_str()), 0, 500);
        g_set.stall_requests.fetch_add(1);
        return std::format("ok one full GPU wait at the next frame end, then {} ms away", g_set.stall_ms.load());
    }
    if (sub == "recreate") {
        g_set.recreate_requests = 1;
        return "ok features released, recreated at the next frame";
    }
    if (sub == "events") {
        dump_events("dlss events");  // from this thread: also works while the RHI thread is blocked
        return "ok the event ring is in the log";
    }
    if (sub == "mvtextures" && a.size() == 2 && (a[1] == "eye" || a[1] == "shared")) {
        g_set.mv_per_eye = a[1] == "eye";
        log::info("dlss: motion vector textures {} (dev command)", a[1]);
        return std::format("ok motion vectors in {}", a[1] == "eye" ? "each eye's own texture at its origin" : "one shared double-wide texture (sub-rectangle bases)");
    }
    if (sub == "copymask" && a.size() == 2) {
        g_set.copy_mask = std::clamp(std::atoi(a[1].c_str()), 0, 7);
        return std::format("ok with copyinputs 1 DLSS reads from the copies: colour {} depth {} motion vectors {} (output always its own)",
                           g_set.copy_mask.load() & 1, (g_set.copy_mask.load() >> 1) & 1, (g_set.copy_mask.load() >> 2) & 1);
    }
    if (sub == "eyes" && a.size() == 2) {
        g_set.test_eyes = std::clamp(std::atoi(a[1].c_str()), 0, 2);
        return std::format("ok eyes {}", g_set.test_eyes.load() == 0 ? "both" : g_set.test_eyes.load() == 1 ? "left only" : "right only");
    }
    if ((sub == "zeromv" || sub == "mvsanitize" || sub == "flush" || sub == "ownstate" || sub == "validate" || sub == "stats" ||
         sub == "params" || sub == "copyinputs" || sub == "evalend") &&
        a.size() == 2) {
        const bool on = a[1] == "1" || a[1] == "feature";
        if (sub == "zeromv") g_set.zero_mv = on;
        else if (sub == "mvsanitize") g_set.mv_sanitize = on;
        else if (sub == "flush") g_set.flush_after = on ? 1 : 0;
        else if (sub == "ownstate") g_set.own_state = on;
        else if (sub == "validate") g_set.validate = on;
        else if (sub == "stats") g_set.input_stats = on;
        else if (sub == "params") g_set.own_params = on;
        else if (sub == "evalend") g_set.eval_at_end = on;
        else g_set.copy_inputs = on;
        log::info("dlss: {} {} (dev command)", sub, on ? 1 : 0);
        return std::format("ok {} {}", sub, on ? 1 : 0);
    }
    if (sub == "dump") {
        const int n = a.size() >= 2 ? std::clamp(std::atoi(a[1].c_str()), 1, 64) : 2;
        g_set.dump_requests = n;
        return std::format("ok the next {} views' constants go to the log", n);
    }
    if (sub == "frames" && a.size() >= 2) {
        {
            std::lock_guard lock(g_framedump.m);
            g_framedump.prefix = a[1];
        }
        g_framedump.crop = a.size() >= 4 ? std::clamp(std::atoi(a[3].c_str()), 64, 4096) : 1024;
        g_framedump.remaining = a.size() >= 3 ? std::clamp(std::atoi(a[2].c_str()), 1, 16) : 4;
        return std::format("ok the next {} frames of both eyes (centre crops of {} pixels) go to {}_f<k>_e<eye>_*", g_framedump.remaining.load(),
                           g_framedump.crop.load(), a[1]);
    }
    if (sub == "bench") {
        if (a.size() == 2 && a[1] == "off") {
            g_set.bench = false;
            return "ok bench off";
        }
        if (a.size() != 5) return "err dlss bench <out w> <out h> <in w> <in h> | dlss bench off";
        g_set.bench_out_w = static_cast<unsigned>(std::atoi(a[1].c_str()));
        g_set.bench_out_h = static_cast<unsigned>(std::atoi(a[2].c_str()));
        g_set.bench_in_w = static_cast<unsigned>(std::atoi(a[3].c_str()));
        g_set.bench_in_h = static_cast<unsigned>(std::atoi(a[4].c_str()));
        g_set.recreate_requests = 1;
        g_set.init_requested = true;
        g_set.bench = true;
        return std::format("ok bench {}x{} from {}x{} (one extra evaluation per frame, timed as slot 2)", a[1], a[2], a[3], a[4]);
    }
    if (sub == "timing") {
        with_info([](Info& i) {
            for (int e = 0; e < 3; ++e) i.ms_sum[e] = i.mv_ms_sum[e] = 0, i.ms_n[e] = 0;
        });
        return "ok GPU timing restarted";
    }
    if (sub != "status")
        return "err dlss status|on|off|init|mode <dlaa|upscale>|preset <default|j|k|l|m>|autoexp <0|1>|mvjitter <0|1|2>|jitter <sx> <sy>|cutreset <0|1>|"
               "cutflag <0|1>|cutlimits <cm> <deg>|cuttest <prefix> <reset 0|1> <zero_mv 0|1>|hdr <0|1>|sharpness <v>|preexp <v>|nograin <0|1>|reset|"
               "recreate|skip <0|1>|stall [ms]|dump|timing|bench ...";
    std::string s;
    with_info([&](Info& i) {
        auto avg = [&](int e, const double* sum) { return i.ms_n[e] ? sum[e] / static_cast<double>(i.ms_n[e]) : 0.0; };
        s = std::format(
            "ok dlss {} | preset {} | auto exposure {} | mv jitter mode {} | NGX: {} | {} | {} | pass: {} | seen {} (last frame {}) replaced {} "
            "fallback {} (no constants {}, eval failures {}) creates {} | last error: {} | feature L: {} | feature R: {} | GPU ms per eye (motion "
            "vectors + DLSS + copy) L {:.3f} R {:.3f} (motion vectors L {:.3f} R {:.3f}, samples {} {}) | jitter px L {:.3f} {:.3f} R {:.3f} {:.3f} "
            "| view constants: size {} captures map {} update {} create {}{}",
            g_set.enabled.load() ? "on" : "off", preset_name(g_set.preset.load()), g_set.auto_exposure.load() ? "on" : "off", g_set.mv_jitter.load(),
            i.ngx, i.capability, i.dll, i.pass.empty() ? "not seen" : i.pass, i.taa_seen, i.seen_last_frame, i.replaced, i.fallback, i.no_rows,
            i.eval_fail, i.creates, i.last_error.empty() ? "-" : i.last_error, i.features[0].empty() ? "-" : i.features[0],
            i.features[1].empty() ? "-" : i.features[1], avg(0, i.ms_sum), avg(1, i.ms_sum), avg(0, i.mv_ms_sum), avg(1, i.mv_ms_sum), i.ms_n[0], i.ms_n[1],
            i.jitter_px[0][0], i.jitter_px[0][1], i.jitter_px[1][0], i.jitter_px[1][1], g_ub_size.load(), g_ub_captures_map.load(),
            g_ub_captures_update.load(), g_ub_captures_create.load(), i.optimal.empty() ? "" : " | optimal:" + i.optimal);
        s += std::format(" | mode {} | upscaled {} upscale failures {} skipped {} | {}", g_set.mode.load() ? "upscale" : "dlaa", g_up_count.load(),
                         g_up_fail.load(), g_up_skipped.load(), i.up_sizes.empty() ? "-" : i.up_sizes);
        {
            const std::uint64_t rs = g_runtime_eye.load();
            std::uint32_t ew = static_cast<std::uint32_t>(rs >> 32), eh = static_cast<std::uint32_t>(rs & 0xffffffffu);
            const std::uint32_t rw = ew, rh = eh;
            engine_eye_size(ew, eh);
            s += std::format(" | output {}{}: runtime eye {}x{}, engine eye {}x{} (input_scale {:.2f}); frames handed to the runtime from DLSS's texture {}, "
                             "eyes in it at the engine's size {} | {}",
                             g_set.output_runtime.load() ? "runtime" : "engine", runtime_output() ? " (active)" : " (inactive)", rw, rh, ew, eh,
                             g_set.engine_scale.load(), g_rt_out.handed.load(), g_rt_out.fallback_eyes.load(), i.vram.empty() ? "video memory: -" : i.vram);
        }
        s += std::format(" | deferred release: features retired {} released {} waiting {} released without fence {} | stall tests {}",
                         g_retired_features.load(), g_released_features.load(), g_retire_waiting.load(), g_retire_forced.load(), g_stalls.load());
        s += std::format(" | camera cuts: reset {} ({} resets) engine {} distance {} angle {} flag {} (row 140 set in {} views; limits {} cm {} deg) last: {} | "
                         "cut test: {} | tests: hdr input {} sharpness {} pre-exposure {} no grain {}",
                         g_set.cut_reset.load() ? "on" : "off", g_cut_resets.load(), g_cut_count[0].load(), g_cut_count[2].load(), g_cut_count[3].load(),
                         g_cut_count[1].load(), g_flag_frames.load(), g_set.cut_distance.load(), g_set.cut_angle.load(),
                         i.last_cut.empty() ? "-" : i.last_cut, i.cut_test.empty() ? "-" : i.cut_test, g_set.up_hdr.load() ? 1 : 0,
                         g_set.sharpness.load(), g_set.pre_exposure.load(), g_set.no_grain.load() ? 1 : 0);
        if (!i.bench.empty())
            s += std::format(" | {}{}: GPU {:.3f} ms (samples {})", i.bench, g_set.bench.load() ? "" : " (off)", avg(2, i.ms_sum), i.ms_n[2]);
    });
    s += std::format(" | checks {}: {} evaluations checked, failed:", g_set.validate.load() ? "on" : "off", g_checked.load());
    for (int c = 0; c < kChkCount; ++c) s += std::format(" {} {}{}", kCheckNames[c], g_check_fail[c].load(), c + 1 < kChkCount ? "," : "");
    s += std::format(" | depth texture changes {} | immediate context calls from other threads {} ({} during NGX calls)", g_depth_changes.load(),
                     g_foreign_calls.load(), g_foreign_during_ngx.load());
    s += std::format(" | motion vector textures: {}", g_set.mv_per_eye.load() ? "each eye's own" : "shared (sub-rectangle bases)");
    s += " | history resets: " + resets_text();
    s += std::format(" | frame dump: {} to go, {} images written", g_framedump.remaining.load(), g_framedump.written.load());
    {
        std::lock_guard lock(g_stats_mutex);
        const Stats& S = g_stats;
        s += std::format(" | input statistics {}: {} read, frames with bad values {}, motion vectors non-finite {} over the input size {} largest {:.1f} px, "
                         "depth non-finite {} outside 0..1 {}, colour non-finite {} brightest {:.3g}",
                         g_set.input_stats.load() ? "on" : "off", S.read, S.frames_bad, S.mv_nonfinite, S.mv_huge, S.mv_max, S.depth_nonfinite, S.depth_out,
                         S.color_nonfinite, S.color_max);
    }
    s += std::format(" | evaluations at {}", g_set.eval_at_end.load() ? "the frame end" : "each eye's last pass");
    s += std::format(" | parameter maps {} | tests: eyes {} zero mv {} mv sanitize {} flush {} own context state {} skip {} copy inputs {} | events {}",
                     g_set.own_params.load() ? "one per feature" : "shared", g_set.test_eyes.load(), g_set.zero_mv.load() ? 1 : 0,
                     g_set.mv_sanitize.load() ? 1 : 0, g_set.flush_after.load(), g_set.own_state.load() ? 1 : 0, g_set.skip_eval.load() ? 1 : 0,
                     g_set.copy_inputs.load() ? 1 : 0, [] {
                         std::lock_guard lock(g_events.m);
                         return g_events.total;
                     }());
    return s;
}

}  // namespace ff7vr::engine::dlss
