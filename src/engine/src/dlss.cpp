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
#include <windows.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
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
constexpr int kRowFirst = 110;  // rows kept from each capture
constexpr int kRowCount = 36;   // 110..145
constexpr int kRowClipToPrevClip = 114;  // 114..117
constexpr int kRowJitter = 118;          // xy this frame, zw previous frame (NDC units)
constexpr int kRowCameraCut = 140;       // .y: camera cut (not verified)

// ------------------------------------------------------------------ settings (any thread)
enum class Preset : unsigned { Default = 0, J = 10, K = 11, L = 12, M = 13 };

struct Settings {
    std::atomic<bool> enabled{false};
    std::atomic<bool> init_requested{false};
    std::atomic<unsigned> preset{static_cast<unsigned>(Preset::Default)};
    std::atomic<bool> auto_exposure{true};
    std::atomic<int> mv_jitter{2};  // 0: remove the jitter difference, 1: MVs flagged jittered, 2: as is, not flagged
    std::atomic<bool> cut_reset{false};
    std::atomic<float> jitter_scale_x{0.5f};
    std::atomic<float> jitter_scale_y{-0.5f};
    std::atomic<int> reset_requests{0};
    std::atomic<int> recreate_requests{0};
    std::atomic<int> dump_requests{0};
    std::atomic<bool> log_ngx{true};
};
Settings g_set;
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
    if (g_ub.size() > 512) g_ub.clear();
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

HRESULT STDMETHODCALLTYPE map_detour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                     D3D11_MAPPED_SUBRESOURCE* out) {
    const HRESULT hr = g_ub_hooks->map.original<MapFn>()(c, r, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && sub == 0 && type == D3D11_MAP_WRITE_DISCARD && g_ub_size.load(std::memory_order_relaxed)) {
        t_mapped = r;
        t_mapped_data = out->pData;
    }
    return hr;
}

void STDMETHODCALLTYPE unmap_detour(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
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

// ------------------------------------------------------------------ NGX (RHI thread)
struct Feature {
    NVSDK_NGX_Handle* handle = nullptr;
    UINT w = 0, h = 0;
    unsigned preset = 0;
    int flags = 0;
    std::uint64_t last_eval_frame = 0;
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
    ComPtr<ID3D11Texture2D> out;
    D3D11_TEXTURE2D_DESC mv_desc{}, out_desc{};
    // Recognised pass
    ID3D11PixelShader* taa_ps = nullptr;  // compared only
    // Timing ring
    std::array<Timing, 8> timing{};
    unsigned timing_next = 0;
    std::chrono::steady_clock::time_point last_frame_time{};
    float frame_dt_ms = 11.1f;
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
    double ms_sum[2]{}, mv_ms_sum[2]{};
    std::uint64_t ms_n[2]{};
    float jitter_px[2][2]{};
    std::string dump;
};
std::mutex g_info_mutex;
Info g_info;

template <class F>
void with_info(F&& f) {
    std::lock_guard lock(g_info_mutex);
    f(g_info);
}

std::atomic<int> g_ngx_log_lines{0};
void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!g_set.log_ngx.load(std::memory_order_relaxed) || !message) return;
    if (g_ngx_log_lines.fetch_add(1) > 400) return;
    std::string m(message);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    log::info("ngx: {}", m);
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
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
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
        i.dll = dll;
    });
    if (!available) return false;
    R.params = p;
    R.ngx_state = 1;
    return true;
}

void release_feature(Feature& f) {
    if (f.handle) NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
    f = Feature{};
}

int wanted_flags() {
    int flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (g_set.auto_exposure.load()) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (g_set.mv_jitter.load() == 1) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    return flags;
}

bool ensure_feature(ID3D11DeviceContext* ctx, int eye, UINT w, UINT h, bool& created) {
    Rhi& R = *g_rhi;
    Feature& f = R.feat[eye];
    const unsigned preset = g_set.preset.load();
    const int flags = wanted_flags();
    created = false;
    if (f.handle && f.w == w && f.h == h && f.preset == preset && f.flags == flags) return true;
    release_feature(f);
    NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
    NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
    NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
    NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
    NVSDK_NGX_Parameter_SetUI(R.params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = w;
    cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = w;
    cp.Feature.InTargetHeight = h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags = flags;
    cp.InEnableOutputSubrects = true;
    const auto t0 = std::chrono::steady_clock::now();
    const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &f.handle, R.params, &cp);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string text = std::format("eye {} {}x{} DLAA preset {} flags 0x{:x}: create {} in {:.1f} ms", eye, w, h, preset_name(preset), flags,
                                   result_text(r), ms);
    log::info("dlss: feature {}", text);
    with_info([&](Info& i) {
        i.features[eye] = text;
        ++i.creates;
        if (i.dll.empty() || i.dll.find("not loaded") != std::string::npos) i.dll = loaded_dll();
    });
    if (NVSDK_NGX_FAILED(r) || !f.handle) {
        f = Feature{};
        return false;
    }
    f.w = w;
    f.h = h;
    f.preset = preset;
    f.flags = flags;
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
cbuffer Pass : register(b0) { float4 Rect; float4 Opt; };   // eye rect x y w h; 1/w 1/h, remove-jitter, 0
cbuffer View : register(b1) { float4 V[146]; };
Texture2D<float> Depth : register(t0);
Texture2D<float2> Velocity : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 ps(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    float2 uv = (pos.xy - Rect.xy) * Opt.xy;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float d = Depth.Load(p);
    float4 prev = ndc.x * V[114] + ndc.y * V[115] + d * V[116] + V[117];
    float2 delta = ndc - prev.xy / prev.w;
    float2 vel = Velocity.Load(p);
    if (vel.x > 0) delta = (vel - 32767.0 / 65535.0) / (0.499 * 0.5);
    if (Opt.z > 0.5) delta -= V[118].xy - V[118].zw;
    return -delta * float2(0.5, -0.5) * Rect.zw;
}
)";

struct PassConsts {
    float rect[4];
    float opt[4];
};

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                      ID3DBlob**);

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    static D3DCompileFn fn = [] {
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<D3DCompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    if (!fn) return false;
    ComPtr<ID3DBlob> err;
    const HRESULT hr = fn(kShader, sizeof(kShader) - 1, "dlss_mv", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
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

bool ensure_textures(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& color, DXGI_FORMAT out_format) {
    Rhi& R = *g_rhi;
    if (!R.mv || R.mv_desc.Width != color.Width || R.mv_desc.Height != color.Height) {
        R.mv.Reset();
        R.mv_rtv.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = color.Width;
        d.Height = color.Height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &R.mv)) || FAILED(dev->CreateRenderTargetView(R.mv.Get(), nullptr, &R.mv_rtv))) {
            R.mv.Reset();
            return false;
        }
        R.mv_desc = d;
        log::info("dlss: motion vector texture {}x{} R16G16_FLOAT", d.Width, d.Height);
    }
    if (!R.out || R.out_desc.Width != color.Width || R.out_desc.Height != color.Height || R.out_desc.Format != out_format) {
        R.out.Reset();
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
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
        why = "no ID3D11DeviceContext1";
        return false;
    }
    // Everything below runs in our own pipeline state; the engine's state comes back as it was.
    ID3DDeviceContextState* game_state = nullptr;
    ctx1->SwapDeviceContextState(R.state.Get(), &game_state);
    bool ok = false;
    bool created = false;
    Feature& f = R.feat[eye];
    if (ensure_feature(ctx, eye, w, h, created)) {
        Timing* t = begin_timing(ctx, eye);
        // Motion vectors for the eye's rectangle.
        PassConsts pc{};
        pc.rect[0] = static_cast<float>(x);
        pc.rect[1] = static_cast<float>(y);
        pc.rect[2] = static_cast<float>(w);
        pc.rect[3] = static_cast<float>(h);
        pc.opt[0] = 1.0f / static_cast<float>(w);
        pc.opt[1] = 1.0f / static_cast<float>(h);
        pc.opt[2] = g_set.mv_jitter.load() == 0 ? 1.0f : 0.0f;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(R.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &pc, sizeof(pc));
            ctx->Unmap(R.cb.Get(), 0);
        }
        ID3D11Buffer* cbs[2] = {R.cb.Get(), in.cb1.Get()};
        ID3D11ShaderResourceView* srvs[2] = {in.srv[1].Get(), in.srv[4].Get()};
        ID3D11RenderTargetView* rt = R.mv_rtv.Get();
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(R.vs.Get(), nullptr, 0);
        ctx->PSSetShader(R.ps.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->RSSetViewports(1, &in.vp);
        ctx->Draw(3, 0);
        ctx->ClearState();
        if (t) ctx->End(t->t1);
        // DLSS for the eye's rectangle.
        const float jx = rows.v[kRowJitter - kRowFirst][0] * static_cast<float>(w) * g_set.jitter_scale_x.load();
        const float jy = rows.v[kRowJitter - kRowFirst][1] * static_cast<float>(h) * g_set.jitter_scale_y.load();
        bool reset = created || f.last_eval_frame + 1 < R.frame;
        if (g_set.reset_requests.load() > 0) reset = true;
        if (g_set.cut_reset.load() && rows.v[kRowCameraCut - kRowFirst][1] != 0.0f) reset = true;
        NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
        ep.Feature.pInColor = in.color.Get();
        ep.Feature.pInOutput = R.out.Get();
        ep.pInDepth = in.depth.Get();
        ep.pInMotionVectors = R.mv.Get();
        ep.InJitterOffsetX = jx;
        ep.InJitterOffsetY = jy;
        ep.InRenderSubrectDimensions = {w, h};
        ep.InReset = reset ? 1 : 0;
        ep.InMVScaleX = 1.0f;
        ep.InMVScaleY = 1.0f;
        ep.InColorSubrectBase = {x, y};
        ep.InDepthSubrectBase = {x, y};
        ep.InMVSubrectBase = {x, y};
        ep.InOutputSubrectBase = {x, y};
        ep.InFrameTimeDeltaInMsec = R.frame_dt_ms;
        const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f.handle, R.params, &ep);
        ctx->ClearState();
        if (NVSDK_NGX_SUCCEED(r)) {
            const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
            ctx->CopySubresourceRegion(in.target.Get(), in.target_view.Texture2D.MipSlice, x, y, 0, R.out.Get(), 0, &box);
            f.last_eval_frame = R.frame;
            ok = true;
            with_info([&](Info& i) {
                i.jitter_px[eye][0] = jx;
                i.jitter_px[eye][1] = jy;
            });
        } else {
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
    ctx1->SwapDeviceContextState(game_state, nullptr);
    if (game_state) game_state->Release();
    return ok;
}

std::atomic<std::uint64_t> g_fallback_log{0};

}  // namespace

void start(const Config& cfg, const std::filesystem::path& dll_dir) {
    g_dll_dir = dll_dir;
    g_set.enabled = cfg.get_bool("dlss", "enabled", false);
    g_set.init_requested = cfg.get_bool("dlss", "init", false);
    unsigned p = 0;
    if (parse_preset(cfg.get_string("dlss", "preset", "default"), p)) g_set.preset = p;
    g_set.auto_exposure = cfg.get_bool("dlss", "auto_exposure", true);
    g_set.mv_jitter = static_cast<int>(cfg.get_int("dlss", "mv_jitter", 2));
    g_set.cut_reset = cfg.get_bool("dlss", "camera_cut_reset", false);
    g_set.log_ngx = cfg.get_bool("dlss", "log_ngx", true);
    const std::string extra = cfg.get_string("dlss", "dll_dir", "");
    if (!extra.empty()) g_extra_dll_dir = log::widen(extra);
    const std::string mode = cfg.get_string("dlss", "mode", "dlaa");
    if (mode != "dlaa") log::warn("dlss: mode '{}' is not available (only dlaa: the anti-aliasing at the eye size)", mode);
    log::info("dlss: built in; {} (preset {}, auto exposure {}, motion vector jitter mode {}{})", g_set.enabled.load() ? "ON" : "off ([dlss] enabled = 0)",
              preset_name(g_set.preset.load()), g_set.auto_exposure.load() ? "on" : "off", g_set.mv_jitter.load(),
              extra.empty() ? "" : ", extra library folder " + extra);
}

bool wants_hooks() { return g_set.enabled.load(std::memory_order_relaxed) || g_set.init_requested.load(std::memory_order_relaxed); }

void frame(ID3D11Texture2D* any_texture) {
    Rhi& R = *g_rhi;
    ++R.frame;
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
    if (!wants_hooks() || !any_texture) return;
    ID3D11Device* dev = nullptr;
    any_texture->GetDevice(&dev);
    if (!dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) {
        install_ub_hooks(dev, ctx);
        if (R.ngx_state == 0) init_ngx(dev);
        if (g_set.recreate_requests.exchange(0) > 0) {
            release_feature(R.feat[0]);
            release_feature(R.feat[1]);
            with_info([](Info& i) { i.features[0] = i.features[1] = "released"; });
        }
        collect_timing(ctx);
        ctx->Release();
    }
    dev->Release();
}

bool on_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT, INT, gpu_trace::DrawIndexedFn) {
    if (count != 3 || !wants_hooks() || !device::active()) return false;
    PassInputs in;
    if (!read_pass(ctx, in, nullptr)) return false;
    const int eye = in.vp.TopLeftX >= 1.0f ? 1 : 0;
    with_info([](Info& i) {
        ++i.taa_seen;
        ++i.seen_this_frame;
    });
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
    const bool ok = run_dlss(ctx, in, eye, why);
    with_info([&](Info& i) {
        ++(ok ? i.replaced : i.fallback);
        if (!ok) i.last_error = why;
    });
    if (!ok && g_fallback_log.fetch_add(1) < 10) log::warn("dlss: eye {}: the game's anti-aliasing runs instead: {}", eye, why);
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
    if (sub == "init") {
        g_set.init_requested = true;
        return "ok NGX initialises at the next stereo frame";
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
    if (sub == "reset") {
        g_set.reset_requests = 2;
        return "ok history reset at the next frame";
    }
    if (sub == "recreate") {
        g_set.recreate_requests = 1;
        return "ok features released, recreated at the next frame";
    }
    if (sub == "dump") {
        g_set.dump_requests = 2;
        return "ok the next two views' constants go to the log";
    }
    if (sub == "timing") {
        with_info([](Info& i) {
            for (int e = 0; e < 2; ++e) i.ms_sum[e] = i.mv_ms_sum[e] = 0, i.ms_n[e] = 0;
        });
        return "ok GPU timing restarted";
    }
    if (sub != "status") return "err dlss status|on|off|init|preset <default|j|k|l|m>|autoexp <0|1>|mvjitter <0|1|2>|jitter <sx> <sy>|cutreset <0|1>|reset|recreate|dump|timing";
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
    });
    return s;
}

}  // namespace ff7vr::engine::dlss
