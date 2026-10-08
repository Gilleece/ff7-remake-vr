#include "player.h"

#include "controls.h"

#include "ue_math.h"
#include "uobj.h"

#include "ff7vr/core/log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace ff7vr::engine::player {
namespace {

using ue::FRotator;
using ue::FVector;

// Engine layouts (docs/re/engine.md sections 2 and 5).
constexpr std::size_t kGameInstance = 0x1070;   // UGameEngine::GameInstance
constexpr std::size_t kLocalPlayers = 0x38;     // UGameInstance::LocalPlayers (TArray)
constexpr std::size_t kPlayerController = 0x30; // ULocalPlayer::PlayerController
// Hysteresis of the follow camera test, in frames (about 0.25 s at 120 fps, 0.33 s at 90).
constexpr int kOrbitFramesIn = 30;
constexpr int kOrbitFramesOut = 30;

enum Fn : std::size_t {
    kGetPawn,
    kGetActorLocation,
    kGetViewTarget,
    kSetVisibility,
    kIsVisible,
    kCount
};
// Optional functions (looked up separately, so a missing one never holds up the others).
enum OptFn : std::size_t {
    kGetNumBones,
    kGetBoneName,
    kGetSocketLocation,
    kOptCount
};

Settings g_settings;
void** g_gengine = nullptr;
uobj::Lookup* g_lookup = nullptr;
uobj::Lookup* g_opt_lookup = nullptr;
uobj::Lookup* g_child_lookup = nullptr;  // SceneComponent.GetChildrenComponents
bool g_lookup_logged = false;
bool g_opt_logged = false;

// Battle signal: a reflected function returning bool, called on a live instance of its class
// each frame ([first_person] battle_signal = Class.Function). See docs/engine-module.md.
std::string g_battle_class, g_battle_function;
bool g_battle_world = false;  // the pawn is passed as the first argument (world context)
int g_battle_offset = 0;      // offset and size of the result in the parameter block
int g_battle_size = 1;
uobj::Lookup* g_battle_lookup = nullptr;

// Work queued by the dev pipe for the game thread (ProcessEvent is game thread only).
std::mutex g_work_mutex;
std::vector<std::function<void()>> g_work;

std::atomic<int> g_toggle_requests{0};
std::atomic<int> g_mode_request{-1};
std::atomic<int> g_combat_override{-1};
std::atomic<std::uint64_t> g_pad_toggles{0};

// Game thread state.
struct State {
    void* pc = nullptr;
    void* pawn = nullptr;
    void* view_target = nullptr;
    bool stereo = false;
    bool first_person = true;   // mode outside the automatic switch
    bool combat = false;
    bool target_ok = false;     // the view target is the pawn or the game's camera actor
    int orbit_frames = 0;       // consecutive frames the camera looked at the pivot (negative: away)
    double aim_miss = 0.0;      // last distance of the pivot from the camera's line of sight (cm)
    bool follow_camera = false; // the game uses its follow camera
    bool fresh = false;         // stereo just started: no follow-camera history yet
    bool snap_first = false;    // start straight in first person at the next frame (no blend)
    float blend = 0.0f;         // 0 third person .. 1 first person
    std::uint64_t frame = 0;
    std::uint64_t loc_frame = ~0ull;
    std::uint64_t aim_frame = ~0ull;
    FVector pawn_loc{};
    bool pawn_loc_ok = false;
    float dt = 1.0f / 90.0f;
    std::uint64_t head_frame = ~0ull;
    bool head_used = false;       // this frame's first-person eye came from the head bone
    FVector head_eye{};
    // skeletal meshes of the pawn (cached per pawn)
    void* meshes_pawn = nullptr;
    std::vector<void*> meshes;
    // head bone
    void* head_pawn = nullptr;
    void* head_mesh = nullptr;
    std::uint64_t head_name = 0;  // FName (ComparisonIndex, Number)
    std::uint64_t head_name2 = 0; // second bone (right eye) when the eyes are used; 0 = one bone
    std::string head_bone;
    bool head_ok = false;         // last frame's head location was valid
    FVector head_loc{};
    math::Vec head_rel{};         // smoothed head location relative to the pawn's location (about 80 ms)
    math::Vec head_slow1{}, head_slow2{};  // the same through two slow stages (steady_seconds each): no step motion
    bool head_rel_valid = false;
    std::uint64_t head_failures = 0;
    // battle signal
    void* battle_obj = nullptr;
    void* battle_cdo = nullptr;
    std::uint64_t battle_value = 0;
    int battle_scan = 0;
    int battle_raw = -1;          // last value read (-1 none)
    std::uint64_t battle_reads = 0, battle_changes = 0;
    bool key_down = false;
    std::uint64_t toggles = 0, auto_switches = 0, calls = 0, call_failures = 0;
    // hidden meshes
    void* hidden_pawn = nullptr;
    std::vector<void*> hidden;
    std::uint64_t hide_scans = 0;
    std::string last_mode = "none";
    std::string logged_mode;
} g;
std::mutex g_status_mutex;
std::string g_status_line;
// Per-frame trace of the first-person eye (fp trace): game thread only.
struct TraceRow {
    double t, dt;
    FVector pawn;
    math::Vec raw, fast, slow;
    int bob;
};
std::vector<TraceRow> g_trace;
std::size_t g_trace_want = 0;
std::string g_trace_path;
std::chrono::steady_clock::time_point g_trace_start;


double dist(const FVector& a, const FVector& b) {
    const double dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

FVector lerp(const FVector& a, const FVector& b, double t) {
    return FVector{static_cast<float>(a.X + (b.X - a.X) * t), static_cast<float>(a.Y + (b.Y - a.Y) * t),
                   static_cast<float>(a.Z + (b.Z - a.Z) * t)};
}

bool call(Fn f, void* obj, void* params) {
    if (!g_lookup || !g_lookup->get(f) || !obj) return false;
    ++g.calls;
    if (uobj::call(obj, g_lookup->get(f), params)) return true;
    ++g.call_failures;
    return false;
}

void* local_player_controller() {
    if (!g_gengine) return nullptr;
    void* engine = nullptr;
    if (!uobj::read(g_gengine, engine) || !engine) return nullptr;
    std::uint8_t* gi = nullptr;
    if (!uobj::read(static_cast<std::uint8_t*>(engine) + kGameInstance, gi) || !gi) return nullptr;
    std::uint8_t** players = nullptr;
    std::int32_t num = 0;
    if (!uobj::read(gi + kLocalPlayers, players) || !uobj::read(gi + kLocalPlayers + 8, num) || !players || num < 1) return nullptr;
    std::uint8_t* lp = nullptr;
    if (!uobj::read(players, lp) || !lp) return nullptr;
    void* pc = nullptr;
    uobj::read(lp + kPlayerController, pc);
    return uobj::alive(pc) ? pc : nullptr;
}

void* get_ptr(Fn f, void* obj) {
    alignas(16) std::uint8_t params[16]{};
    if (!call(f, obj, params)) return nullptr;
    void* r = nullptr;
    std::memcpy(&r, params, sizeof(r));
    return uobj::alive(r) ? r : nullptr;
}

bool pawn_location(void* pawn, FVector& out) {
    alignas(16) std::uint8_t params[16]{};
    if (!call(kGetActorLocation, pawn, params)) return false;
    std::memcpy(&out, params, sizeof(out));
    return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
}

bool is_visible(void* comp) {
    alignas(16) std::uint8_t params[16]{};
    return call(kIsVisible, comp, params) && params[0] != 0;
}

void set_visible(void* comp, bool on) {
    alignas(16) std::uint8_t params[16]{};
    params[0] = on ? 1 : 0;
    params[1] = 0;  // not propagated to attached children
    call(kSetVisibility, comp, params);
}

void restore_meshes() {
    for (void* c : g.hidden)
        if (uobj::alive(c)) set_visible(c, true);
    if (!g.hidden.empty()) log::info("player: {} mesh(es) of the character shown again", g.hidden.size());
    g.hidden.clear();
    g.hidden_pawn = nullptr;
    g.meshes_pawn = nullptr;  // the next first person scans again (meshes added since: equipment)
}

// The pawn's skeletal mesh components (the components' outer is the actor that owns them).
// Scans the whole object array: once per pawn.
const std::vector<void*>& pawn_meshes(void* pawn) {
    if (g.meshes_pawn == pawn) {
        std::erase_if(g.meshes, [](void* m) { return !uobj::alive(m); });
        return g.meshes;
    }
    g.meshes_pawn = pawn;
    g.meshes.clear();
    ++g.hide_scans;
    const int n = uobj::num_objects();
    for (int i = 0; i < n; ++i) {
        void* o = uobj::object_at(i);
        if (!o || uobj::outer_of(o) != pawn) continue;
        if (uobj::object_name(uobj::class_of(o)).find("SkeletalMeshComponent") != std::string::npos) g.meshes.push_back(o);
    }
    return g.meshes;
}

bool opt_call(OptFn f, void* obj, void* params) {
    if (!g_opt_lookup || !g_opt_lookup->get(f) || !obj) return false;
    ++g.calls;
    if (uobj::call(obj, g_opt_lookup->get(f), params)) return true;
    ++g.call_failures;
    return false;
}

std::string fname_text(std::uint64_t n) {
    const auto idx = static_cast<std::uint32_t>(n & 0xffffffffu);
    const auto num = static_cast<std::uint32_t>(n >> 32);
    std::string t = uobj::name_text(idx);
    if (num > 0) t += "_" + std::to_string(num - 1);
    return t;
}

int num_bones(void* mesh) {
    alignas(16) std::uint8_t params[16]{};
    if (!opt_call(kGetNumBones, mesh, params)) return -1;
    std::int32_t n = 0;
    std::memcpy(&n, params, 4);
    return n;
}

// FName of bone i (params: int32 BoneIndex at +0, FName ReturnValue at +4).
bool bone_name(void* mesh, int i, std::uint64_t& out) {
    alignas(16) std::uint8_t params[16]{};
    std::int32_t idx = i;
    std::memcpy(params, &idx, 4);
    if (!opt_call(kGetBoneName, mesh, params)) return false;
    std::memcpy(&out, params + 4, 8);
    return true;
}

// World location of a bone or socket (params: FName at +0, FVector ReturnValue at +8).
bool socket_location(void* comp, std::uint64_t name, FVector& out) {
    alignas(16) std::uint8_t params[32]{};
    std::memcpy(params, &name, 8);
    if (!opt_call(kGetSocketLocation, comp, params)) return false;
    std::memcpy(&out, params + 8, sizeof(out));
    return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
}

std::string lower(std::string t) {
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}

// How much a bone name looks like the head bone: "head" exactly, then names containing
// "head" that are not an end or a helper bone.
int head_score(const std::string& name) {
    const std::string l = lower(name);
    if (l == "head" || l == "c_head" || l == "head_01" || l == "bip01_head") return 3;
    if (l.find("head") == std::string::npos) return 0;
    for (const char* bad : {"end", "nub", "top", "hair", "twist", "jnt_", "_sub", "eye", "dummy", "ik"})
        if (l.find(bad) != std::string::npos) return 1;
    return 2;
}

// Finds the head bone of the pawn: the best-scoring bone over its skeletal meshes, on the
// mesh with the most bones among equal scores. Once per pawn.
void find_head(void* pawn) {
    g.head_pawn = pawn;
    g.head_mesh = nullptr;
    g.head_name = 0;
    g.head_name2 = 0;
    g.head_bone.clear();
    g.head_rel_valid = false;
    if (!pawn || !g_opt_lookup || !g_opt_lookup->ready()) {
        g.head_pawn = nullptr;  // try again once the functions are found
        return;
    }
    int best = 0, best_bones = 0;
    std::string seen;
    for (void* m : pawn_meshes(pawn)) {
        const int n = num_bones(m);
        if (n <= 0 || n > 2048) continue;
        std::uint64_t eye_l = 0, eye_r = 0;
        for (int i = 0; i < n; ++i) {
            std::uint64_t name = 0;
            if (!bone_name(m, i, name)) break;
            const std::string t = fname_text(name);
            const std::string l = lower(t);
            if (l == "l_eye" || l == "lefteye" || l == "eye_l") eye_l = name;
            if (l == "r_eye" || l == "righteye" || l == "eye_r") eye_r = name;
            const int sc = head_score(t);
            if (sc == 0) continue;
            if (seen.size() < 400) seen += " " + uobj::object_name(m) + ":" + t;
            if (sc > best || (sc == best && n > best_bones)) {
                best = sc;
                best_bones = n;
                g.head_mesh = m;
                g.head_name = name;
                g.head_bone = t;
                g.head_name2 = 0;
            }
        }
        // Both eye bones: the point between them is the eye position (best of all).
        if (eye_l && eye_r && best < 4) {
            best = 4;
            best_bones = n;
            g.head_mesh = m;
            g.head_name = eye_l;
            g.head_name2 = eye_r;
            g.head_bone = fname_text(eye_l) + "+" + fname_text(eye_r);
        }
    }
    if (g.head_mesh)
        log::info("player: head bone {} of {} ({} bones); candidates:{}", g.head_bone, uobj::object_name(g.head_mesh), best_bones, seen);
    else
        log::warn("player: no head bone found on {} ({} skeletal meshes); first person uses eye_offset", uobj::object_name(pawn),
                  pawn_meshes(pawn).size());
}

// This frame's head location (after the world tick), smoothed relative to the pawn so the
// animation's small jitters do not shake the view. False: use the fixed offset.
bool head_location(FVector& out) {
    if (g.head_pawn != g.pawn) find_head(g.pawn);
    g.head_ok = false;
    if (!g.head_mesh || !uobj::alive(g.head_mesh)) return false;
    FVector h{}, h2{};
    if (!socket_location(g.head_mesh, g.head_name, h) || dist(h, g.pawn_loc) > 250.0 ||
        (g.head_name2 && (!socket_location(g.head_mesh, g.head_name2, h2) || dist(h, h2) > 30.0))) {
        ++g.head_failures;
        return false;
    }
    if (g.head_name2) h = lerp(h, h2, 0.5);
    const math::Vec rel{h.X - g.pawn_loc.X, h.Y - g.pawn_loc.Y, h.Z - g.pawn_loc.Z};
    const auto approach = [](math::Vec& v, const math::Vec& to, double a) {
        v = math::Vec{v.x + (to.x - v.x) * a, v.y + (to.y - v.y) * a, v.z + (to.z - v.z) * a};
    };
    if (!g.head_rel_valid) {
        g.head_rel = g.head_slow1 = g.head_slow2 = rel;
        g.head_rel_valid = true;
    } else {
        const double dt = static_cast<double>(g.dt);
        approach(g.head_rel, rel, 1.0 - std::exp(-dt / 0.08));  // about 80 ms: animation jitter only
        // Without head bob: two slow first-order stages in a row. The walk and run cycles move
        // the head 2 to 3 times a second; the pair passes slow changes of the head's offset
        // (crouching, climbing, leaning) with a delay of about twice steady_seconds.
        const double a = 1.0 - std::exp(-dt / std::max(0.01, static_cast<double>(g_settings.steady_seconds.load())));
        approach(g.head_slow1, rel, a);
        approach(g.head_slow2, g.head_slow1, a);
    }
    const bool bob = g_settings.head_bob.load();
    const math::Vec& use = bob ? g.head_rel : g.head_slow2;
    if (g_trace_want > 0) {
        g_trace.push_back(TraceRow{std::chrono::duration<double>(std::chrono::steady_clock::now() - g_trace_start).count(),
                                   static_cast<double>(g.dt), g.pawn_loc, rel, g.head_rel, g.head_slow2, bob ? 1 : 0});
        if (g_trace.size() >= g_trace_want) {
            FILE* f = nullptr;
            if (fopen_s(&f, g_trace_path.c_str(), "w") == 0 && f) {
                std::fputs("t,dt,pawn_x,pawn_y,pawn_z,raw_x,raw_y,raw_z,fast_x,fast_y,fast_z,slow_x,slow_y,slow_z,bob\n", f);
                for (const auto& r : g_trace)
                    std::fprintf(f, "%.5f,%.5f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", r.t, r.dt, r.pawn.X, r.pawn.Y,
                                 r.pawn.Z, r.raw.x, r.raw.y, r.raw.z, r.fast.x, r.fast.y, r.fast.z, r.slow.x, r.slow.y, r.slow.z, r.bob);
                std::fclose(f);
                log::info("player: trace of {} frames written to {}", g_trace.size(), g_trace_path);
            } else {
                log::warn("player: trace could not be written to {}", g_trace_path);
            }
            g_trace.clear();
            g_trace_want = 0;
        }
    }
    g.head_loc = h;
    g.head_ok = true;
    out = FVector{static_cast<float>(g.pawn_loc.X + use.x), static_cast<float>(g.pawn_loc.Y + use.y), static_cast<float>(g.pawn_loc.Z + use.z)};
    return true;
}

// Mesh components of other actors attached to the character's meshes (the sword on the
// back): SceneComponent.GetChildrenComponents(true, Children) on each mesh. Params: bool at
// +0, TArray<USceneComponent*> at +8, filled by the engine (it resets the array and adds to
// it). The same array header is passed in every call, so the engine reuses or reallocates
// its one allocation instead of a new one being left behind at every first-person start.
std::uint8_t g_children_array[16]{};  // TArray: data pointer, Num, Max
std::vector<void*> attached_meshes(void* pawn, const std::vector<void*>& meshes) {
    std::vector<void*> out;
    for (void* m : meshes) {
        alignas(16) std::uint8_t params[32]{};
        params[0] = 1;
        std::memcpy(params + 8, g_children_array, sizeof(g_children_array));
        std::memset(params + 16, 0, 4);  // Num = 0, the allocation (Max) is kept
        if (!g_child_lookup || !g_child_lookup->ready()) break;
        ++g.calls;
        if (!uobj::call(m, g_child_lookup->get(0), params)) {
            ++g.call_failures;
            std::memset(g_children_array, 0, sizeof(g_children_array));  // state unknown: start a new array next time
            continue;
        }
        std::memcpy(g_children_array, params + 8, sizeof(g_children_array));
        void** data = nullptr;
        std::int32_t num = 0;
        std::memcpy(&data, params + 8, sizeof(data));
        std::memcpy(&num, params + 16, sizeof(num));
        if (!data || num <= 0 || num > 512 || !uobj::readable(data, sizeof(void*) * static_cast<std::size_t>(num))) continue;
        for (int i = 0; i < num; ++i) {
            void* c = data[i];
            if (!uobj::alive(c) || uobj::outer_of(c) == pawn) continue;
            if (uobj::object_name(uobj::class_of(c)).find("MeshComponent") == std::string::npos) continue;
            if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
        }
    }
    return out;
}

void hide_meshes(void* pawn) {
    if (g.hidden_pawn != pawn) {
        restore_meshes();
        g.hidden_pawn = pawn;
        std::vector<void*> meshes = pawn_meshes(pawn);
        const std::size_t own = meshes.size();
        for (void* a : attached_meshes(pawn, meshes)) meshes.push_back(a);
        std::string names;
        for (void* m : meshes) {
            names += " " + uobj::path_of(m);
            if (is_visible(m)) {
                set_visible(m, false);
                g.hidden.push_back(m);
            }
        }
        log::info("player: first person: hid {} of {} mesh(es) ({} skeletal of {} ({}), {} attached):{}", g.hidden.size(), meshes.size(), own,
                  uobj::object_name(pawn), uobj::object_name(uobj::class_of(pawn)), meshes.size() - own, names);
        return;
    }
    // The game may show a mesh again (equipment, animation events): hide it again.
    for (void* c : g.hidden)
        if (uobj::alive(c) && is_visible(c)) set_visible(c, false);
}

// The game's own camera actor in normal play: an object named EndCameraActor of class
// CameraActor exactly (a cutscene's CineCameraActor or any other camera is not).
bool is_game_camera_actor(void* o) {
    return uobj::class_named(o, "CameraActor") && uobj::object_name(o).rfind("EndCameraActor", 0) == 0;
}

// The battle signal, read once per frame. -1: no signal (function or instance not found).
int read_battle() {
    if (!g_battle_lookup) return -1;
    if (!g_battle_lookup->ready()) {
        // Functions exist from start-up: after three passes over the object array it is not
        // there (a wrong name in the ini); stop scanning.
        if (g_battle_lookup->passes() >= 3) {
            log::warn("player: battle signal {}.{} not found; no automatic third person in battles", g_battle_class, g_battle_function);
            delete g_battle_lookup;
            g_battle_lookup = nullptr;
            return -1;
        }
        g_battle_lookup->step();
        if (!g_battle_lookup->ready()) return -1;
        log::info("player: battle signal function {}.{} found", g_battle_class, g_battle_function);
    }
    if (g.battle_obj && !uobj::alive(g.battle_obj)) g.battle_obj = nullptr;
    if (!g.battle_obj) {
        // A live object of the function's class, a slice of the object array per frame; for a
        // function library (static functions) there is none after a whole pass, and the class
        // default object is used.
        void* cls = uobj::outer_of(g_battle_lookup->get(0));
        const int n = uobj::num_objects();
        const int end = std::min(n, g.battle_scan + 32768);
        for (int i = g.battle_scan; i < end && !g.battle_obj; ++i) {
            void* o = uobj::object_at(i);
            if (!o || uobj::class_of(o) != cls) continue;
            if (uobj::object_name(o).rfind("Default__", 0) == 0) g.battle_cdo = o;
            else g.battle_obj = o;
        }
        g.battle_scan = end >= n ? 0 : end;
        if (!g.battle_obj && g.battle_scan == 0 && g.battle_cdo) g.battle_obj = g.battle_cdo;
        if (!g.battle_obj) return -1;
        log::info("player: battle signal read from {} ({})", uobj::path_of(g.battle_obj), uobj::object_name(uobj::class_of(g.battle_obj)));
    }
    if (g_battle_world && !g.pawn) return -1;
    alignas(16) std::uint8_t params[128]{};
    if (g_battle_world) std::memcpy(params, &g.pawn, sizeof(void*));
    ++g.calls;
    if (!uobj::call(g.battle_obj, g_battle_lookup->get(0), params)) {
        ++g.call_failures;
        g.battle_obj = nullptr;
        return -1;
    }
    ++g.battle_reads;
    std::uint64_t v = 0;
    std::memcpy(&v, params + g_battle_offset, static_cast<std::size_t>(g_battle_size));
    g.battle_value = v;
    return v != 0 ? 1 : 0;
}

bool combat_now() {
    const int v = read_battle();
    if (v != g.battle_raw) {
        ++g.battle_changes;
        log::info("player: battle signal {} -> {}", g.battle_raw, v);
        g.battle_raw = v;
    }
    const int o = g_combat_override.load();
    if (o >= 0) return o != 0;
    return v == 1;
}

void run_work() {
    std::vector<std::function<void()>> work;
    {
        std::lock_guard lock(g_work_mutex);
        work.swap(g_work);
    }
    for (auto& w : work) w();
}

// Runs `fn` on the game thread at the next frame and returns its result (dev commands).
std::string on_game_thread(std::function<std::string()> fn) {
    auto p = std::make_shared<std::promise<std::string>>();
    auto fut = p->get_future();
    {
        std::lock_guard lock(g_work_mutex);
        g_work.push_back([p, fn = std::move(fn)] { p->set_value(fn()); });
    }
    if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) return "err no game frame within 3 s";
    return fut.get();
}

std::string hex_bytes(const std::uint8_t* p, std::size_t n) {
    std::string r;
    for (std::size_t i = 0; i < n; ++i) r += std::format("{:02x}{}", p[i], (i % 4 == 3) ? " " : "");
    return r;
}

}  // namespace

Settings& settings() { return g_settings; }

void set_battle_signal(const std::string& spec) {
    // "Class.Function [world] [result=<offset>:<size>]"
    std::istringstream in(spec);
    std::string fn, w;
    in >> fn;
    const auto dot = fn.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= fn.size()) return;
    g_battle_class = fn.substr(0, dot);
    g_battle_function = fn.substr(dot + 1);
    g_battle_world = false;
    g_battle_offset = 0;
    g_battle_size = 1;
    while (in >> w) {
        if (w == "world") {
            g_battle_world = true;
            if (g_battle_offset == 0) g_battle_offset = 8;
        } else if (w.rfind("result=", 0) == 0) {
            int off = 0, size = 1;
            if (sscanf_s(w.c_str() + 7, "%d:%d", &off, &size) >= 1) {
                g_battle_offset = std::clamp(off, 0, 120);
                g_battle_size = (size == 1 || size == 2 || size == 4 || size == 8) ? size : 1;
            }
        }
    }
}

void init(std::uint8_t* object_array, std::uint8_t* name_pool, void** gengine) {
    uobj::init(object_array, name_pool);
    g_gengine = gengine;
    g.first_person = g_settings.fp_default.load();
    if (!uobj::available() || !gengine) {
        log::warn("player: object array, name pool or GEngine not found; camera modes off");
        return;
    }
    g_lookup = new uobj::Lookup({
        {"K2_GetPawn", "Controller", "Function"},
        {"K2_GetActorLocation", "Actor", "Function"},
        {"GetViewTarget", "Controller", "Function"},
        {"SetVisibility", "SceneComponent", "Function"},
        {"IsVisible", "SceneComponent", "Function"},
    });
    g_opt_lookup = new uobj::Lookup({
        {"GetNumBones", "SkinnedMeshComponent", "Function"},
        {"GetBoneName", "SkinnedMeshComponent", "Function"},
        {"GetSocketLocation", "SceneComponent", "Function"},
    });
    g_child_lookup = new uobj::Lookup({{"GetChildrenComponents", "SceneComponent", "Function"}});
    if (!g_battle_class.empty()) g_battle_lookup = new uobj::Lookup({{g_battle_function, g_battle_class, "Function"}});
}

void tick(bool stereo, float delta_seconds) {
    if (!g_lookup) return;
    ++g.frame;
    g.dt = std::clamp(delta_seconds, 0.0001f, 0.25f);
    if (g_lookup->ready()) run_work();
    if (g_child_lookup && !g_child_lookup->ready() && g_child_lookup->passes() < 3) g_child_lookup->step();
    if (g_opt_lookup && !g_opt_lookup->ready() && g_opt_lookup->passes() < 3) {
        g_opt_lookup->step();
        if (g_opt_lookup->passes() >= 2 && !g_opt_logged) {
            g_opt_logged = true;
            for (const auto& e : g_opt_lookup->entries())
                if (!e.obj) log::warn("player: {} ({}) not found; first person uses eye_offset", e.name, e.outer);
        }
    }
    if (!g_lookup->ready()) {
        g_lookup->step();
        if (g_lookup->passes() >= 2 && !g_lookup_logged) {
            g_lookup_logged = true;
            for (const auto& e : g_lookup->entries())
                if (!e.obj) log::warn("player: {} ({}) not found", e.name, e.outer);
        }
        if (!g_lookup->ready()) return;
        log::info("player: reflected functions found (K2_GetPawn {}, GetViewTarget {})", g_lookup->get(kGetPawn),
                  g_lookup->get(kGetViewTarget));
    }
    const Settings& s = g_settings;
    if (stereo && !g.stereo) {
        // Stereo (re)starts: the follow-camera test has no history, so its first result is
        // taken at once instead of after the hysteresis (no game-camera frames at the start).
        g.follow_camera = false;
        g.orbit_frames = 0;
        g.fresh = true;
        g.snap_first = false;
    }
    g.stereo = stereo;
    g.pc = local_player_controller();
    void* pawn = g.pc ? get_ptr(kGetPawn, g.pc) : nullptr;
    if (pawn != g.pawn) {
        log::info("player: controlled pawn {} ({})", pawn ? uobj::object_name(pawn) : std::string("none"),
                  pawn ? uobj::object_name(uobj::class_of(pawn)) : std::string());
        g.pawn = pawn;
    }
    // AController::GetViewTarget is virtual: the player controller's version returns its
    // camera manager's view target.
    g.view_target = g.pc ? get_ptr(kGetViewTarget, g.pc) : nullptr;

    // Toggle: keyboard (game window in front) and requests from other threads.
    int toggles = g_toggle_requests.exchange(0);
    const int vk = s.toggle_key.load();
    if (vk > 0) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g.key_down) ++toggles;
        g.key_down = down;
    }
    if (!s.fp_available.load()) toggles = 0;  // [first_person] enabled = 0: no toggling either
    if (toggles % 2 == 1) {
        g.first_person = !g.first_person;
        ++g.toggles;
        log::info("player: toggled to {} person", g.first_person ? "first" : "third");
    }
    const int req = g_mode_request.exchange(-1);
    if (req >= 0) g.first_person = req == 1;

    // Battle: third person for its duration, the default mode afterwards.
    const bool combat = s.fp_available.load() && s.auto_combat.load() && combat_now();
    if (combat != g.combat) {
        g.combat = combat;
        ++g.auto_switches;
        g.first_person = combat ? false : s.fp_default.load();
        log::info("player: battle {}: {} person", combat ? "started" : "ended", g.first_person ? "first" : "third");
    }

    // The follow camera: the view target is the pawn or the game's own camera actor (named
    // EndCameraActor, class CameraActor); where it looks is checked in adjust_camera.
    g.target_ok = g.pawn && g.view_target &&
                  (g.view_target == g.pawn || is_game_camera_actor(g.view_target));
    if (!g.target_ok) {
        g.follow_camera = false;
        g.orbit_frames = 0;
    }
    const bool fp_target = stereo && s.fp_available.load() && g.first_person && g.follow_camera;
    const float step = s.blend_seconds.load() > 0.0f ? delta_seconds / s.blend_seconds.load() : 1.0f;
    if (!g.follow_camera || !stereo) g.blend = 0.0f;  // authored camera: cut, no blend
    else if (g.snap_first && fp_target) g.blend = 1.0f;  // stereo starts in first person
    else g.blend = std::clamp(g.blend + (fp_target ? step : -step), 0.0f, 1.0f);
    g.snap_first = false;

    const bool hide = stereo && s.hide.load() == 1 && g.blend > 0.5f && g.pawn;
    if (hide) hide_meshes(g.pawn);
    else if (!g.hidden.empty() || g.hidden_pawn) restore_meshes();

    std::lock_guard lock(g_status_mutex);
    g_status_line = std::format(
        "pc {} pawn {} ({}) view_target {} ({}) target_ok {} aim_miss {:.1f} orbit_frames {} follow {} stereo {} first_person {} combat {} blend {:.2f} hidden {} "
        "bob {} toggles {} (pad {} of {} polls) auto {} calls {} failed {} pawn_loc ({:.1f} {:.1f} {:.1f}) head {} {} ({:.1f} {:.1f} {:.1f}) failures {} "
        "battle_signal {} raw {} value {:#x} reads {} changes {} mode {}",
        g.pc, g.pawn, g.pawn ? uobj::object_name(uobj::class_of(g.pawn)) : "", g.view_target,
        g.view_target ? uobj::object_name(g.view_target) : "", g.target_ok ? 1 : 0, g.aim_miss, g.orbit_frames, g.follow_camera ? 1 : 0, stereo ? 1 : 0, g.first_person ? 1 : 0,
        g.combat ? 1 : 0, g.blend, g.hidden.size(), s.head_bob.load() ? 1 : 0, g.toggles, g_pad_toggles.load(), controls::pad_polls(), g.auto_switches, g.calls, g.call_failures, g.pawn_loc.X,
        g.pawn_loc.Y, g.pawn_loc.Z, g.head_bone.empty() ? "-" : g.head_bone, g.head_ok ? "ok" : "no", g.head_loc.X, g.head_loc.Y,
        g.head_loc.Z, g.head_failures, g_battle_class.empty() ? std::string("none") : g_battle_class + "." + g_battle_function, g.battle_raw,
        g.battle_value, g.battle_reads, g.battle_changes, g.last_mode);
}

// Logs every change of the camera mode with its reason (one line per change).
void note_mode(const char* mode, const std::string& reason) {
    g.last_mode = mode;
    if (g.logged_mode == mode) return;
    log::info("player: camera mode {} -> {} ({})", g.logged_mode.empty() ? "none" : g.logged_mode, mode, reason);
    g.logged_mode = mode;
}

bool adjust_camera(FRotator& rotation, FVector& location, bool decoupled, bool& force_decouple) {
    force_decouple = false;
    if (!g_lookup || !g_lookup->ready() || !g.pawn) {
        note_mode("game camera", "no controlled pawn");
        return false;
    }
    if (g.loc_frame != g.frame) {
        g.loc_frame = g.frame;
        g.pawn_loc_ok = pawn_location(g.pawn, g.pawn_loc);  // after the world tick: this frame's position
    }
    const Settings& s = g_settings;
    if (!g.pawn_loc_ok) {
        note_mode("game camera", "no pawn location");
        return false;
    }
    // The follow camera orbits a pivot above the character and looks at it: the pivot is
    // close to the camera's line of sight, in front of it, and not far away. Checked once per
    // frame (first eye), with hysteresis so a frame or two of a scripted move does not flip.
    if (g.aim_frame != g.frame) {
        g.aim_frame = g.frame;
        const math::Vec pivot{g.pawn_loc.X, g.pawn_loc.Y, g.pawn_loc.Z + s.pivot_height.load()};
        const math::Vec fwd = math::rotate(math::quat_from_rotator(FRotator{rotation.Pitch, rotation.Yaw, 0.0f}), math::Vec{1, 0, 0});
        const math::Vec to{pivot.x - location.X, pivot.y - location.Y, pivot.z - location.Z};
        const double along = to.x * fwd.x + to.y * fwd.y + to.z * fwd.z;
        const double d2 = to.x * to.x + to.y * to.y + to.z * to.z;
        g.aim_miss = std::sqrt(std::max(0.0, d2 - along * along));
        const bool orbit = g.target_ok && along > 0.0 && std::sqrt(d2) < s.follow_distance.load() && g.aim_miss < s.aim_tolerance.load();
        if (orbit) g.orbit_frames = std::max(1, g.orbit_frames + 1);
        else g.orbit_frames = std::min(-1, g.orbit_frames - 1);
        const bool was = g.follow_camera;
        if (g.follow_camera && g.orbit_frames <= -kOrbitFramesOut) g.follow_camera = false;
        else if (!g.follow_camera && (g.orbit_frames >= kOrbitFramesIn || (g.fresh && orbit))) {
            g.follow_camera = true;
            g.snap_first = g.fresh;
        }
        g.fresh = false;
        if (!g.target_ok) g.follow_camera = false;
        if (was != g.follow_camera)
            log::info("player: follow camera {} (view target {} class {}, pivot {:.0f} cm from the line of sight, {:.0f} cm away, {})",
                      g.follow_camera ? "on" : "off", g.view_target ? uobj::object_name(g.view_target) : std::string("none"),
                      g.view_target ? uobj::object_name(uobj::class_of(g.view_target)) : std::string(), g.aim_miss, std::sqrt(d2),
                      along > 0.0 ? "in front" : "behind");
    }
    if (!g.follow_camera) {
        note_mode("game camera", g.target_ok ? "the camera does not look at the character" : "the view target is not the pawn or EndCameraActor");
        return false;
    }
    const math::Quat q_yaw = math::quat_from_rotator(FRotator{0.0f, rotation.Yaw, 0.0f});
    FVector third = location;
    const bool level = decoupled && s.level_boom.load();
    if (level) {
        // Where the camera would be at zero pitch around the pivot.
        const math::Vec pivot{g.pawn_loc.X, g.pawn_loc.Y, g.pawn_loc.Z + s.pivot_height.load()};
        const math::Vec lv = math::level_boom(rotation, location, pivot);
        third = FVector{static_cast<float>(lv.x), static_cast<float>(lv.y), static_cast<float>(lv.z)};
    }
    if (g.blend <= 0.0f) {
        location = third;
        note_mode(level ? "third person (level boom)" : "third person (game boom)",
                  g.combat ? "battle" : (g.first_person && g_settings.fp_available.load() ? "first person blending" : "third person selected"));
        return level;
    }
    // First-person eye: the head bone plus head_offset, or the pawn's location plus eye_offset.
    FVector base = g.pawn_loc;
    math::Vec local{s.eye_forward.load(), s.eye_right.load(), s.eye_up.load()};
    if (g.head_frame != g.frame) {
        g.head_frame = g.frame;
        g.head_used = s.eye_head.load() && head_location(g.head_eye);
    }
    if (g.head_used) {
        base = g.head_eye;
        local = math::Vec{s.head_forward.load(), s.head_right.load(), s.head_up.load()};
    }
    const math::Vec off = math::rotate(q_yaw, local);
    const FVector first{static_cast<float>(base.X + off.x), static_cast<float>(base.Y + off.y), static_cast<float>(base.Z + off.z)};
    const double t = g.blend * g.blend * (3.0 - 2.0 * g.blend);  // smoothstep
    location = lerp(third, first, t);
    force_decouple = true;
    if (g.blend >= 1.0f) note_mode(g.head_used ? "first person (head bone)" : "first person (eye_offset)", "first person selected, follow camera");
    else note_mode("blend", g.first_person ? "to first person" : "to third person");
    return true;
}

void request_toggle() { ++g_toggle_requests; }

void request_pad_toggle() {
    g_pad_toggles.fetch_add(1);
    request_toggle();
}
void request_mode(bool first_person) { g_mode_request = first_person ? 1 : 0; }
void set_combat_override(int v) { g_combat_override = v; }

std::string status() {
    std::lock_guard lock(g_status_mutex);
    return g_status_line.empty() ? std::string("player: not ready (functions not found yet)") : g_status_line;
}

std::string command(const std::string& args) {
    std::istringstream in(args);
    std::vector<std::string> a;
    for (std::string w; in >> w;) a.push_back(w);
    Settings& s = g_settings;
    if (a.empty() || a[0] == "status") return "ok " + status();
    if (a[0] == "toggle") {
        request_toggle();
        return "ok toggle requested";
    }
    if (a[0] == "first" || a[0] == "third") {
        request_mode(a[0] == "first");
        return "ok " + a[0] + " person requested";
    }
    if (a[0] == "available" && a.size() == 2) {
        s.fp_available = a[1] == "1";
        return std::format("ok first person available {}", s.fp_available.load() ? 1 : 0);
    }
    if (a[0] == "pad" && a.size() == 2) {
        // Test of the gamepad filter without a pad: feed one XInput button state through it.
        unsigned short b = static_cast<unsigned short>(std::strtoul(a[1].c_str(), nullptr, 16));
        controls::filter_pad(0, &b);
        return std::format("ok buttons after filter {:#06x}", b);
    }
    if (a[0] == "combat" && a.size() == 2) {
        set_combat_override(a[1] == "auto" ? -1 : std::atoi(a[1].c_str()));
        return "ok combat override " + a[1];
    }
    if (a[0] == "offset" && a.size() == 4) {
        s.eye_forward = static_cast<float>(std::atof(a[1].c_str()));
        s.eye_right = static_cast<float>(std::atof(a[2].c_str()));
        s.eye_up = static_cast<float>(std::atof(a[3].c_str()));
        return std::format("ok eye offset {} {} {}", s.eye_forward.load(), s.eye_right.load(), s.eye_up.load());
    }
    if (a[0] == "hide" && a.size() == 2) {
        s.hide = a[1] == "meshes" ? 1 : 0;
        return std::format("ok hide {}", s.hide.load());
    }
    if (a[0] == "bob" && a.size() == 2) {
        s.head_bob = a[1] == "1";
        return std::format("ok head bob {}", s.head_bob.load() ? 1 : 0);
    }
    if (a[0] == "steady" && a.size() == 2) {
        s.steady_seconds = std::clamp(static_cast<float>(std::atof(a[1].c_str())), 0.01f, 5.0f);
        return std::format("ok steady {} s", s.steady_seconds.load());
    }
    if (a[0] == "trace" && a.size() == 3) {
        // fp trace <frames> <csv path>: the first-person eye's raw, smoothed and steady offsets
        // from the pawn, and the pawn's location, for the next frames in first person.
        const std::size_t frames = std::clamp<std::size_t>(std::strtoul(a[1].c_str(), nullptr, 10), 1, 20000);
        const std::string path = a[2];
        return on_game_thread([frames, path]() -> std::string {
            g_trace.clear();
            g_trace.reserve(frames);
            g_trace_path = path;
            g_trace_start = std::chrono::steady_clock::now();
            g_trace_want = frames;
            return std::format("ok tracing {} frames to {}", frames, path);
        });
    }
    if (a[0] == "boom" && a.size() == 2) {
        s.level_boom = a[1] == "level";
        return std::format("ok boom {}", s.level_boom.load() ? "level" : "game");
    }
    if (a[0] == "pivot" && a.size() == 2) {
        s.pivot_height = static_cast<float>(std::atof(a[1].c_str()));
        return std::format("ok pivot height {}", s.pivot_height.load());
    }
    if (a[0] == "blend" && a.size() == 2) {
        s.blend_seconds = static_cast<float>(std::atof(a[1].c_str()));
        return std::format("ok blend {} s", s.blend_seconds.load());
    }
    if (a[0] == "find" && a.size() >= 2) {
        // Slow (full object array scan) and off the game thread: names only, no calls.
        const auto found = uobj::find_all(a[1], a.size() >= 3 ? a[2] : std::string(), a.size() >= 4 ? a[3] : std::string(), 40);
        std::string r = std::format("ok {} found", found.size());
        for (const auto& f : found) r += std::format(" | [{}] {} class {} path {}", f.index, f.obj, f.cls, f.path);
        return r;
    }
    if (a[0] == "classes" && a.size() >= 2) {
        // Objects whose class name contains the text (first 40), for finding game state objects.
        std::string r;
        int count = 0;
        const int n = uobj::num_objects();
        for (int i = 0; i < n && count < 60; ++i) {
            void* o = uobj::object_at(i);
            if (!o) continue;
            const std::string cls = uobj::object_name(uobj::class_of(o));
            if (cls.find(a[1]) == std::string::npos) continue;
            const std::string path = uobj::path_of(o);
            if (path.find("Default__") != std::string::npos) continue;
            ++count;
            r += std::format(" | [{}] {} class {} path {}", i, o, cls, path);
        }
        return std::format("ok {} object(s){}", count, r);
    }
    if (a[0] == "funcs" && a.size() >= 2) {
        // Reflected functions whose name contains the text (and whose class name contains the
        // second text), for finding game state queries. Names only, no calls.
        const std::string want = lower(a[1]), outer_want = a.size() >= 3 ? lower(a[2]) : std::string();
        std::string r;
        int count = 0;
        const int n = uobj::num_objects();
        for (int i = 0; i < n && count < 200; ++i) {
            void* o = uobj::object_at(i);
            if (!o) continue;
            const std::string cls = uobj::object_name(uobj::class_of(o));
            if (cls != "Function" && cls != "DelegateFunction") continue;
            const std::string name = uobj::object_name(o);
            if (lower(name).find(want) == std::string::npos) continue;
            const std::string outer = uobj::object_name(uobj::outer_of(o));
            if (!outer_want.empty() && lower(outer).find(outer_want) == std::string::npos) continue;
            ++count;
            r += std::format(" | {}.{} {}", outer, name, o);
        }
        return std::format("ok {} function(s){}", count, r);
    }
    if (a[0] == "props" && a.size() >= 2) {
        // Reflected properties whose name contains the text (names only).
        const std::string want = lower(a[1]), outer_want = a.size() >= 3 ? lower(a[2]) : std::string();
        std::string r;
        int count = 0;
        const int n = uobj::num_objects();
        for (int i = 0; i < n && count < 200; ++i) {
            void* o = uobj::object_at(i);
            if (!o) continue;
            const std::string cls = uobj::object_name(uobj::class_of(o));
            if (cls.size() < 8 || cls.compare(cls.size() - 8, 8, "Property") != 0) continue;
            const std::string name = uobj::object_name(o);
            if (want != "*" && lower(name).find(want) == std::string::npos) continue;
            const std::string outer = uobj::object_name(uobj::outer_of(o));
            if (!outer_want.empty() && lower(outer).find(outer_want) == std::string::npos) continue;
            ++count;
            r += std::format(" | {}.{} ({})", outer, name, cls);
        }
        return std::format("ok {} propert(ies){}", count, r);
    }
    if (a[0] == "call" && a.size() >= 3) {
        // fp call <object hex | class name> <Class.Function> [hex bytes of the parameters]:
        // calls the function on the game thread and prints the first 48 bytes of the
        // parameter block afterwards (return values follow the arguments).
        // <Function> alone: the first function of that name, on its own class.
        const auto dot = a[2].find('.');
        const auto fns = dot == std::string::npos ? uobj::find_all(a[2], {}, "Function", 2)
                                                  : uobj::find_all(a[2].substr(dot + 1), a[2].substr(0, dot), "Function", 2);
        if (fns.empty()) return "err function not found";
        void* fn = fns[0].obj;
        if (a[1] == "auto") a[1] = uobj::object_name(uobj::outer_of(fn));
        void* obj = nullptr;
        if (a[1].rfind("0x", 0) == 0) obj = reinterpret_cast<void*>(std::strtoull(a[1].c_str(), nullptr, 16));
        else {
            // The first live object of that class; for a function library (static functions)
            // there is none, and its class default object is used.
            void* cdo = nullptr;
            const int n = uobj::num_objects();
            for (int i = 0; i < n && !obj; ++i) {
                void* o = uobj::object_at(i);
                if (!o || !uobj::class_or_super_named(o, a[1])) continue;
                if (uobj::object_name(o).rfind("Default__", 0) != 0) obj = o;
                else if (!cdo) cdo = o;
            }
            if (!obj) obj = cdo;
        }
        if (!obj || !uobj::alive(obj)) return "err object not found or not alive";
        std::vector<std::uint8_t> bytes;
        if (a.size() >= 4 && a[3] == "pawn") {
            // the pawn as the first argument (a world context object)
            bytes.resize(8);
            std::memcpy(bytes.data(), &g.pawn, 8);
        } else if (a.size() >= 4)
            for (std::size_t i = 0; i + 1 < a[3].size() && bytes.size() < 64; i += 2)
                bytes.push_back(static_cast<std::uint8_t>(std::strtoul(a[3].substr(i, 2).c_str(), nullptr, 16)));
        return on_game_thread([obj, fn, bytes]() -> std::string {
            if (!uobj::alive(obj)) return "err object gone";
            alignas(16) std::uint8_t params[256]{};
            std::memcpy(params, bytes.data(), bytes.size());
            if (!uobj::call(obj, fn, params)) return "err ProcessEvent faulted";
            return std::format("ok {} ({}) -> {}", uobj::path_of(obj), uobj::object_name(uobj::class_of(obj)), hex_bytes(params, 48));
        });
    }
    if (a[0] == "bones") {
        // fp bones [text]: bone names of the pawn's skeletal meshes containing the text
        // (default "head"), with their world location and offset from the pawn's location.
        const std::string want = lower(a.size() >= 2 ? a[1] : std::string("head"));
        return on_game_thread([want]() -> std::string {
            if (!g.pawn) return "err no pawn";
            if (!g_opt_lookup || !g_opt_lookup->ready()) return "err bone functions not found";
            FVector p{};
            pawn_location(g.pawn, p);
            std::string r = std::format("ok pawn ({:.1f} {:.1f} {:.1f})", p.X, p.Y, p.Z);
            for (void* m : pawn_meshes(g.pawn)) {
                const int n = num_bones(m);
                r += std::format(" || {} {} bones", uobj::object_name(m), n);
                for (int i = 0; i < n && i < 2048; ++i) {
                    std::uint64_t name = 0;
                    if (!bone_name(m, i, name)) break;
                    const std::string t = fname_text(name);
                    if (lower(t).find(want) == std::string::npos) continue;
                    FVector b{};
                    socket_location(m, name, b);
                    r += std::format(" | {} ({:.1f} {:.1f} {:.1f}) rel ({:.1f} {:.1f} {:.1f})", t, b.X, b.Y, b.Z, b.X - p.X, b.Y - p.Y, b.Z - p.Z);
                }
            }
            return r;
        });
    }
    if (a[0] == "signal" && a.size() >= 2) {
        // fp signal <Class.Function> [world] [result=<offset>:<size>] | none: battle signal
        std::string spec;
        for (std::size_t i = 1; i < a.size(); ++i) spec += (i > 1 ? " " : "") + a[i];
        return on_game_thread([spec]() -> std::string {
            g_battle_class.clear();
            g_battle_function.clear();
            if (spec != "none") set_battle_signal(spec);
            delete g_battle_lookup;
            g_battle_lookup = g_battle_class.empty() ? nullptr : new uobj::Lookup({{g_battle_function, g_battle_class, "Function"}});
            g.battle_obj = g.battle_cdo = nullptr;
            g.battle_scan = 0;
            g.battle_value = 0;
            return std::format("ok battle signal {}.{} world {} result {}:{}", g_battle_class, g_battle_function, g_battle_world ? 1 : 0,
                               g_battle_offset, g_battle_size);
        });
    }
    if (a[0] == "eye" && a.size() == 2) {
        s.eye_head = a[1] == "head";
        return std::format("ok first-person eye from {}", s.eye_head.load() ? "the head bone" : "eye_offset");
    }
    if (a[0] == "headoffset" && a.size() == 4) {
        s.head_forward = static_cast<float>(std::atof(a[1].c_str()));
        s.head_right = static_cast<float>(std::atof(a[2].c_str()));
        s.head_up = static_cast<float>(std::atof(a[3].c_str()));
        return std::format("ok head offset {} {} {}", s.head_forward.load(), s.head_right.load(), s.head_up.load());
    }
    if (a[0] == "chain" && a.size() == 2) {
        // Class chain of an object given by address (checks UStruct::SuperStruct).
        void* o = a[1] == "pawn" ? g.pawn : a[1] == "view" ? g.view_target : a[1] == "pc" ? g.pc
                                                                            : reinterpret_cast<void*>(std::strtoull(a[1].c_str(), nullptr, 16));
        if (!uobj::alive(o)) return "err not a live object";
        std::string r = "ok";
        void* c = uobj::class_of(o);
        for (int d = 0; c && d < 16; ++d, c = uobj::super_of(c)) r += " " + uobj::object_name(c);
        return r;
    }
    return "err fp status|toggle|first|third|available <0|1>|combat <0|1|auto>|offset <fwd> <right> <up>|hide <none|meshes>|"
           "bob <0|1>|steady <s>|trace <frames> <csv path>|eye <head|offset>|headoffset <fwd> <right> <up>|boom <level|game>|pivot <cm>|blend <s>|find <name> [outer] [class]|"
           "classes <text>|chain <hex address>|funcs <text> [class text]|props <text> [class text]|call <obj|class> <Class.Function> [hex]|"
           "bones [text]";
}

}  // namespace ff7vr::engine::player
