#include "diag.h"

#include "battle_objects.h"
#include "ue_math.h"
#include "ui_align.h"
#include "uobj.h"

#include "ff7vr/core/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ff7vr::engine::diag {
namespace {

using ue::FRotator;
using ue::FVector;

constexpr double kPollSeconds = 0.1;
constexpr double kHeartbeatSeconds = 10.0;
constexpr int kScanSlice = 16384;   // objects per poll (a full pass of the object array in a few seconds)
constexpr int kNoisyChanges = 20;   // changes per heartbeat window before a value is left out of the change test
constexpr std::size_t kMaxWidgets = 12;
constexpr int kPerClass = 2;  // instances per widget class
constexpr std::size_t kMaxStateObjects = 4;

std::atomic<bool> g_battle_on{false}, g_prompt_on{false};
const auto g_start = std::chrono::steady_clock::now();

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }
long long now_ms() { return static_cast<long long>(now_s() * 1000.0); }

std::string lower(std::string t) {
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t;
}
bool has_any(const std::string& l, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (l.find(w) != std::string::npos) return true;
    return false;
}

// Reflection walk (UE 4.18): UStruct::Children +0x38, UField::Next +0x28,
// UProperty::Offset_Internal +0x44, UBoolProperty ByteOffset +0x71 and FieldMask +0x73.
struct Prop {
    std::string name;
    std::string type;
    std::int32_t offset = -1;
    std::uint8_t bool_byte = 0, bool_mask = 0;
};
template <class F>
void for_props(void* ustruct, F&& fn) {
    void* f = nullptr;
    if (!ustruct || !uobj::read(static_cast<std::uint8_t*>(ustruct) + 0x38, f)) return;
    for (int guard = 0; f && guard < 256; ++guard) {
        Prop p;
        p.name = uobj::object_name(f);
        p.type = uobj::object_name(uobj::class_of(f));
        uobj::read(static_cast<std::uint8_t*>(f) + 0x44, p.offset);
        if (p.type == "BoolProperty") {
            uobj::read(static_cast<std::uint8_t*>(f) + 0x71, p.bool_byte);
            uobj::read(static_cast<std::uint8_t*>(f) + 0x73, p.bool_mask);
        }
        fn(p);
        if (!uobj::read(static_cast<std::uint8_t*>(f) + 0x28, f)) return;
    }
}
// The class itself or the super class named `name` (nullptr: none).
void* class_in_chain(void* cls, std::string_view name) {
    for (int i = 0; cls && i < 32; ++i, cls = uobj::super_of(cls))
        if (uobj::object_name(cls) == name) return cls;
    return nullptr;
}
bool find_prop(void* cls, std::string_view name, Prop& out) {
    bool found = false;
    for_props(cls, [&](const Prop& p) {
        if (!found && p.name == name) {
            out = p;
            found = true;
        }
    });
    return found;
}
bool readable_type(const std::string& t) {
    return t == "IntProperty" || t == "UInt32Property" || t == "ByteProperty" || t == "EnumProperty" || t == "BoolProperty" ||
           t == "NameProperty" || t == "ObjectProperty" || t == "WeakObjectProperty";
}
std::string obj_text(void* o) {
    if (!o) return "none";
    if (!uobj::alive(o)) return "stale";
    return uobj::object_name(o);
}
std::string read_prop(void* obj, const Prop& p) {
    if (!obj || p.offset < 0) return "?";
    const auto* a = static_cast<const std::uint8_t*>(obj) + p.offset;
    if (p.type == "IntProperty" || p.type == "UInt32Property") {
        std::int32_t v = 0;
        return uobj::read(a, v) ? std::to_string(v) : "?";
    }
    if (p.type == "ByteProperty" || p.type == "EnumProperty") {
        std::uint8_t v = 0;
        return uobj::read(a, v) ? std::to_string(v) : "?";
    }
    if (p.type == "BoolProperty") {
        std::uint8_t v = 0;
        if (!uobj::read(a + p.bool_byte, v)) return "?";
        return p.bool_mask ? ((v & p.bool_mask) ? "1" : "0") : std::format("raw{:#x}", v);
    }
    if (p.type == "NameProperty") {
        std::uint32_t id = 0;
        return uobj::read(a, id) ? uobj::name_text(id) : "?";
    }
    if (p.type == "ObjectProperty") {
        void* o = nullptr;
        return uobj::read(a, o) ? obj_text(o) : "?";
    }
    if (p.type == "WeakObjectProperty") {
        std::int32_t index = 0, serial = 0;
        if (!uobj::read(a, index) || !uobj::read(a + 4, serial)) return "?";
        if (index <= 0 || serial == 0) return "none";
        return obj_text(uobj::object_at(index));
    }
    return "?";
}

// Up to `max` readable properties of the class chain (stopping at the engine's base
// classes), those whose names suggest state first.
struct Picked {
    std::string label;
    Prop prop;
};
std::vector<Picked> pick_props(void* cls, std::size_t max, std::initializer_list<const char*> strong,
                               std::initializer_list<const char*> weak) {
    struct Scored {
        int score;
        Prop prop;
    };
    std::vector<Scored> all;
    for (int depth = 0; cls && depth < 8; ++depth, cls = uobj::super_of(cls)) {
        const std::string cn = uobj::object_name(cls);
        if (cn == "Object" || cn == "Actor" || cn == "ActorComponent" || cn == "Pawn" || cn == "Character" || cn == "Controller" ||
            cn == "Info" || cn == "Widget" || cn == "UserWidget" || cn == "PlayerCameraManager" || cn == "PlayerController")
            break;
        for_props(cls, [&](const Prop& p) {
            if (!readable_type(p.type)) return;
            const std::string l = lower(p.name);
            const bool bflag = p.name.size() > 1 && p.name[0] == 'b' && std::isupper(static_cast<unsigned char>(p.name[1]));
            const int score = has_any(l, strong) ? 3 : has_any(l, weak) ? 2 : bflag ? 1 : 0;
            all.push_back({score, p});
        });
    }
    std::stable_sort(all.begin(), all.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    std::vector<Picked> r;
    for (const auto& s : all) {
        if (r.size() >= max) break;
        r.push_back({s.prop.name, s.prop});
    }
    return r;
}

// Class kinds, cached per class pointer.
struct ClassInfo {
    std::string name;
    bool widget = false;       // derives from Widget
    bool user_widget = false;  // derives from UserWidget
    bool wbp = false;          // a widget blueprint class
    bool meta = false;         // reflection data (functions, classes, properties)
    bool battle_kw = false, prompt_kw = false, cam_mgr = false, battle_cls = false, state_cls = false;
    int battle_rank = 0, prompt_rank = 0;  // listed widget classes first (higher = earlier)
};
std::unordered_map<void*, ClassInfo> g_classes;
const ClassInfo& class_info(void* cls) {
    auto it = g_classes.find(cls);
    if (it != g_classes.end()) return it->second;
    ClassInfo ci;
    ci.name = uobj::object_name(cls);
    const std::string meta_cls = uobj::object_name(uobj::class_of(cls));
    const std::string l = lower(ci.name);
    ci.meta = ci.name == "Function" || ci.name == "DelegateFunction" || ci.name == "Class" || ci.name == "BlueprintGeneratedClass" ||
              ci.name == "WidgetBlueprintGeneratedClass" || ci.name == "ScriptStruct" || ci.name == "Enum" || ci.name == "UserDefinedEnum" ||
              ci.name == "UserDefinedStruct" || ci.name == "Package" || ci.name == "WidgetBlueprint" || ci.name == "Blueprint" ||
              (ci.name.size() > 8 && ci.name.compare(ci.name.size() - 8, 8, "Property") == 0);
    if (!ci.meta) {
        ci.wbp = meta_cls == "WidgetBlueprintGeneratedClass";
        ci.widget = class_in_chain(cls, "Widget") != nullptr;
        ci.user_widget = ci.widget && class_in_chain(cls, "UserWidget") != nullptr;
        ci.battle_kw = has_any(l, {"battle", "command", "atb", "gauge"});
        ci.prompt_kw = has_any(l, {"interact", "prompt", "talk", "action", "pick", "btnguide", "fieldtarget"});
        // the widget blueprint classes of this build most likely to be the battle HUD and the
        // interaction prompt (from the class list this log prints)
        static const char* const battle_list[] = {"ATBGauge_C",    "Command_C",      "Command_BattleInEffect_C", "BattleTarget_New_C",
                                                  "EnemyStatus_C", "LockOnMarker_C", "Command_Default_C",        "Command_Base_C"};
        static const char* const prompt_list[] = {"FieldAction_Command_C", "Field_BtnGuide_C", "FieldTarget_01_C",  "FieldTargetSearch_02_C",
                                                  "FieldTarget_Block_C",   "BasicGuide_C",     "TalkMenu_Center_C", "Icon_Btn_00_C"};
        for (int i = 0; i < 8; ++i) {
            if (ci.name == battle_list[i]) ci.battle_rank = 100 - i;
            if (ci.name == prompt_list[i]) ci.prompt_rank = 100 - i;
        }
        ci.cam_mgr = l.find("playercameramanager") != std::string::npos;
        // classes with Battle in the name, without the animation notifies (dozens, loaded with animations)
        ci.battle_cls = l.find("battle") != std::string::npos && l.find("animnotify") == std::string::npos;
        ci.state_cls = ci.battle_cls && !has_any(l, {"dataobject", "camerashake", "btdecorator", "bttask", "talk", "areabox", "notify"});
    }
    return g_classes.emplace(cls, std::move(ci)).first->second;
}

struct WidgetRef {
    void* obj;
    std::string name;   // object name
    std::string label;  // name(class)
    bool user = false;  // a UserWidget (the blueprint widgets the game adds as a whole)
    void* cls = nullptr;
    int rank = 0;
};
struct Scan {
    int pos = 0;
    std::vector<WidgetRef> battle_widgets, prompt_widgets;
    std::vector<void*> state_objects;
    std::vector<WidgetRef> hud;  // every instance of the listed battle HUD classes
    void* cam_mgr = nullptr;
    std::map<std::string, int> wbp_classes;     // widget blueprint class -> live instances
    std::map<std::string, int> battle_classes;  // non-widget classes with Battle in the name -> instances
};
Scan g_next, g_cur;
bool g_scan_done = false;
int g_passes = 0;

// Keeps the best kMaxWidgets: listed classes first, then user widgets, at most kPerClass
// instances of a class.
int widget_key(const WidgetRef& w) { return w.rank * 2 + (w.user ? 1 : 0); }
void add_widget(std::vector<WidgetRef>& v, WidgetRef w) {
    if (std::count_if(v.begin(), v.end(), [&](const WidgetRef& x) { return x.cls == w.cls; }) >= kPerClass) return;
    if (v.size() < kMaxWidgets) {
        v.push_back(std::move(w));
    } else {
        auto worst = std::min_element(v.begin(), v.end(), [](const WidgetRef& a, const WidgetRef& b) { return widget_key(a) < widget_key(b); });
        if (widget_key(*worst) >= widget_key(w)) return;
        *worst = std::move(w);
    }
    std::stable_sort(v.begin(), v.end(), [](const WidgetRef& a, const WidgetRef& b) { return a.rank > b.rank; });
}

void scan_object(void* o) {
    void* cls = uobj::class_of(o);
    if (!cls) return;
    const ClassInfo& ci = class_info(cls);
    if (ci.meta) return;
    if (!ci.widget && !ci.battle_cls && !ci.cam_mgr) return;
    const std::string name = uobj::object_name(o);
    // class default objects and the templates inside widget blueprint classes are not shown
    // (a widget blueprint class keeps a whole widget tree of its own for its design)
    if (name.rfind("Default__", 0) == 0 || name == "WidgetArchetype" || uobj::is_template(o)) return;
    if (ci.cam_mgr && !g_next.cam_mgr) g_next.cam_mgr = o;
    if (ci.widget) {
        if (ci.wbp) ++g_next.wbp_classes[ci.name];
        const std::string ln = lower(name);
        const bool battle = ci.battle_rank || ci.battle_kw || has_any(ln, {"battle", "command", "atb", "gauge"});
        const bool prompt = ci.prompt_rank || ci.prompt_kw || has_any(ln, {"interact", "prompt", "talk", "pick"});
        if (battle) add_widget(g_next.battle_widgets, WidgetRef{o, name, std::format("{}({})", name, ci.name), ci.user_widget, cls, ci.battle_rank});
        if (ci.battle_rank && g_next.hud.size() < 256) g_next.hud.push_back(WidgetRef{o, name, ci.name, ci.user_widget, cls, ci.battle_rank});
        if (prompt) add_widget(g_next.prompt_widgets, WidgetRef{o, name, std::format("{}({})", name, ci.name), ci.user_widget, cls, ci.prompt_rank});
        return;
    }
    if (ci.battle_cls) {
        ++g_next.battle_classes[ci.name];
        if (ci.state_cls && g_next.state_objects.size() < kMaxStateObjects &&
            std::none_of(g_next.state_objects.begin(), g_next.state_objects.end(), [&](void* s) { return uobj::class_of(s) == cls; }))
            g_next.state_objects.push_back(o);
    }
}

std::string list_text(const std::vector<WidgetRef>& v) {
    std::string r;
    for (const auto& w : v) r += (r.empty() ? "" : ", ") + w.label;
    return r.empty() ? "none" : r;
}
std::string map_delta(const std::map<std::string, int>& before, const std::map<std::string, int>& after) {
    std::string r;
    for (const auto& [k, n] : after)
        if (!before.contains(k)) r += " +" + k + (n > 1 ? std::format("x{}", n) : "");
    for (const auto& [k, n] : before)
        if (!after.contains(k)) r += " -" + k;
    return r;
}
std::string map_text(const std::map<std::string, int>& m) {
    std::string r;
    for (const auto& [k, n] : m) r += (r.empty() ? "" : " ") + k + (n > 1 ? std::format("x{}", n) : "");
    return r.empty() ? "none" : r;
}

// Reflected functions called on the game thread.
enum Fn : std::size_t { kIsVisible, kIsInViewport, kCamLoc, kCamRot, kActorLoc };
uobj::Lookup* g_fn = nullptr;
bool g_fn_logged = false;
bool call_fn(Fn f, void* obj, std::uint8_t* params) {
    if (!g_fn || !g_fn->get(f) || !obj || !uobj::alive(obj)) return false;
    return uobj::call(obj, g_fn->get(f), params);
}
bool call_bool(Fn f, void* obj, bool& out) {
    alignas(16) std::uint8_t params[16]{};
    if (!call_fn(f, obj, params)) return false;
    out = params[0] != 0;
    return true;
}
bool call_vec(Fn f, void* obj, FVector& out) {
    alignas(16) std::uint8_t params[32]{};
    if (!call_fn(f, obj, params)) return false;
    std::memcpy(&out, params, sizeof(out));
    return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
}
bool call_rot(Fn f, void* obj, FRotator& out) {
    alignas(16) std::uint8_t params[32]{};
    if (!call_fn(f, obj, params)) return false;
    std::memcpy(&out, params, sizeof(out));
    return true;
}

// Widget layout: UWidget::Visibility, UWidget::RenderTransform (Translation first),
// UWidget::Slot; CanvasPanelSlot::LayoutData (FAnchorData: Offsets Left, Top first).
Prop g_vis_prop, g_rt_prop, g_slot_prop, g_layout_prop;
bool g_widget_props = false, g_layout_ok = false, g_layout_tried = false;
void widget_props(void* widget_obj) {
    if (g_widget_props) return;
    void* wcls = class_in_chain(uobj::class_of(widget_obj), "Widget");
    if (!wcls) return;
    g_widget_props = true;
    find_prop(wcls, "Visibility", g_vis_prop);
    find_prop(wcls, "RenderTransform", g_rt_prop);
    find_prop(wcls, "Slot", g_slot_prop);
    log::info("diag: Widget.Visibility @{} RenderTransform @{} Slot @{}", g_vis_prop.offset, g_rt_prop.offset, g_slot_prop.offset);
}
struct WidgetState {
    int visible = -1;      // IsVisible() (-1: call failed)
    int in_viewport = -1;  // UserWidget.IsInViewport()
    int vis_enum = -1;     // UWidget::Visibility (ESlateVisibility)
    float tx = 0, ty = 0;  // render translation
    bool slot = false;
    float sx = 0, sy = 0;  // canvas slot offsets (left, top)
};
WidgetState widget_state(const WidgetRef& w) {
    WidgetState s;
    if (!uobj::alive(w.obj)) return s;
    widget_props(w.obj);
    bool b = false;
    if (call_bool(kIsVisible, w.obj, b)) s.visible = b ? 1 : 0;
    if (w.user && call_bool(kIsInViewport, w.obj, b)) s.in_viewport = b ? 1 : 0;
    auto* base = static_cast<std::uint8_t*>(w.obj);
    std::uint8_t v = 0;
    if (g_vis_prop.offset > 0 && uobj::read(base + g_vis_prop.offset, v)) s.vis_enum = v;
    if (g_rt_prop.offset > 0) {
        uobj::read(base + g_rt_prop.offset, s.tx);
        uobj::read(base + g_rt_prop.offset + 4, s.ty);
    }
    void* slot = nullptr;
    if (g_slot_prop.offset > 0 && uobj::read(base + g_slot_prop.offset, slot) && slot && uobj::alive(slot) &&
        uobj::class_named(slot, "CanvasPanelSlot")) {
        if (!g_layout_tried) {
            g_layout_tried = true;
            g_layout_ok = find_prop(uobj::class_of(slot), "LayoutData", g_layout_prop);
            log::info("diag: CanvasPanelSlot.LayoutData @{}", g_layout_ok ? g_layout_prop.offset : -1);
        }
        if (g_layout_ok) {
            auto* sb = static_cast<std::uint8_t*>(slot) + g_layout_prop.offset;
            s.slot = uobj::read(sb, s.sx) && uobj::read(sb + 4, s.sy);
        }
    }
    return s;
}
std::string widget_text(const WidgetState& s) {
    std::string r = std::format("vis={} vp={} enum={} rt=({:.0f},{:.0f})", s.visible, s.in_viewport, s.vis_enum, s.tx, s.ty);
    if (s.slot) r += std::format(" slot=({:.0f},{:.0f})", s.sx, s.sy);
    return r;
}

// Battle candidates by name, with the change test leaving out values that change too often.
struct Value {
    std::string text;
    int changes = 0;  // in the current heartbeat window
    bool noisy = false;
};
std::map<std::string, Value> g_values;
double g_next_poll = 0, g_next_heartbeat = 0;
void* g_props_pc = nullptr;
void* g_props_pawn = nullptr;
void* g_props_cam = nullptr;
std::vector<Picked> g_pc_props, g_pawn_props, g_cam_props;
std::map<void*, std::vector<Picked>> g_state_props;
Prop g_cam_style;
bool g_cam_style_ok = false;

// Properties of the object's class chain naming a target, a lock-on or an enemy.
std::vector<Picked> target_props(void* obj) {
    std::vector<Picked> r;
    void* cls = uobj::class_of(obj);
    for (int depth = 0; cls && depth < 10 && r.size() < 6; ++depth, cls = uobj::super_of(cls))
        for_props(cls, [&](const Prop& p) {
            if (r.size() >= 6) return;
            if (p.type != "ObjectProperty" && p.type != "WeakObjectProperty" && p.type != "BoolProperty") return;
            const std::string l = lower(p.name);
            if (has_any(l, {"viewtarget", "cameratarget"})) return;
            if (has_any(l, {"target", "lockon", "lock_on", "enemy"})) r.push_back({p.name, p});
        });
    return r;
}

// The game camera and the mod's head, for the prompt lines.
struct Camera {
    bool cam_ok = false;
    FVector cam{};
    FRotator rot{};
    bool pawn_ok = false;
    FVector pawn{};
    ui_align::Snapshot snap{};
    bool head_ok = false;
    math::Vec head{};
};
double dist(const FVector& a, const math::Vec& b) {
    const double dx = a.X - b.x, dy = a.Y - b.y, dz = a.Z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
Camera camera_facts(const Frame& f) {
    Camera c;
    c.cam_ok = call_vec(kCamLoc, g_cur.cam_mgr, c.cam) && call_rot(kCamRot, g_cur.cam_mgr, c.rot);
    c.pawn_ok = call_vec(kActorLoc, f.pawn, c.pawn);
    c.snap = ui_align::snapshot();
    if (c.snap.valid) {
        // the head in the world: the eye base plus the tracked head position turned by the eye base rotation
        math::Vec hp = c.snap.positional ? math::scale(math::xr_to_ue_position(c.snap.head.position), c.snap.upm) : math::Vec{};
        hp = math::rotate(math::quat_from_rotator(c.snap.base_level), hp);
        c.head = math::add(math::Vec{c.snap.base_loc.X, c.snap.base_loc.Y, c.snap.base_loc.Z}, hp);
        c.head_ok = true;
    }
    return c;
}
std::string camera_text(const Frame& f, const Camera& c) {
    std::string r = std::format("mode={} fp_applies={} follow={} stereo={} battle={}", f.first_person ? "first" : "third", f.fp_applies ? 1 : 0,
                                f.follow_camera ? 1 : 0, f.stereo ? 1 : 0, f.in_battle ? 1 : 0);
    if (c.cam_ok)
        r += std::format(" cam=({:.0f},{:.0f},{:.0f}) rot=({:.1f},{:.1f},{:.1f})", c.cam.X, c.cam.Y, c.cam.Z, c.rot.Pitch, c.rot.Yaw, c.rot.Roll);
    else
        r += " cam=?";
    if (c.snap.valid)
        r += std::format(" stereo_cam=({:.0f},{:.0f},{:.0f}) yaw={:.1f} base=({:.0f},{:.0f},{:.0f}) head_xr=({:.3f},{:.3f},{:.3f}) "
                         "head_q=({:.3f},{:.3f},{:.3f},{:.3f}) head=({:.0f},{:.0f},{:.0f})",
                         c.snap.game_loc.X, c.snap.game_loc.Y, c.snap.game_loc.Z, c.snap.game_rot.Yaw, c.snap.base_loc.X, c.snap.base_loc.Y,
                         c.snap.base_loc.Z, c.snap.head.position.x, c.snap.head.position.y, c.snap.head.position.z, c.snap.head.orientation.x,
                         c.snap.head.orientation.y, c.snap.head.orientation.z, c.snap.head.orientation.w, c.head.x, c.head.y, c.head.z);
    r += std::format(" align={} aligned={} full={}", c.snap.align_mode, c.snap.aligned, c.snap.full);
    if (c.pawn_ok) {
        r += std::format(" pawn=({:.0f},{:.0f},{:.0f})", c.pawn.X, c.pawn.Y, c.pawn.Z);
        if (c.cam_ok) r += std::format(" d_cam={:.0f}", dist(c.pawn, math::Vec{c.cam.X, c.cam.Y, c.cam.Z}));
        if (c.head_ok) r += std::format(" d_head={:.0f}", dist(c.pawn, c.head));
    }
    return r;
}

std::map<void*, WidgetState> g_prompt_last;
std::mutex g_status_mutex;
std::string g_prompt_status = "no poll yet";
std::uint64_t g_prompt_lines = 0, g_battle_lines = 0;
int g_prompt_budget = 0;  // lines in the current second (at most 10)
double g_prompt_second = 0;

void poll_battle(const Frame& f, double t) {
    std::vector<std::pair<std::string, std::string>> vals;
    vals.emplace_back("verdict", f.in_battle ? "battle" : "none");
    vals.emplace_back("mode", f.first_person ? "first" : "third");
    vals.emplace_back("signal", std::to_string(f.battle_raw));
    {
        // the fight detection's battle objects (battle_objects.h) and, per listed battle HUD
        // class, how many of its live instances are visible: "visible/instances"
        const battle::ObjectsState o = battle::last();
        vals.emplace_back("objects", std::format("{}:{}/{}", o.shown, o.visible, o.instances));
        std::map<std::string, std::pair<int, int>> per;
        for (const auto& w : g_cur.hud) {
            if (uobj::pending_kill(w.obj)) continue;
            bool b = false;
            auto& c = per[w.label];
            ++c.second;
            if (call_bool(kIsVisible, w.obj, b) && b) ++c.first;
        }
        for (const auto& [k, c] : per) vals.emplace_back("n:" + k, std::format("{}/{}", c.first, c.second));
    }
    std::map<std::string, int> seen;
    for (const auto& w : g_cur.battle_widgets) {
        const WidgetState s = widget_state(w);
        std::string key = "w:" + w.label;
        if (++seen[key] > 1) key += std::format("#{}", seen[key]);
        vals.emplace_back(key, std::format("{}/{}/{}", s.visible, s.in_viewport, s.vis_enum));
    }
    auto merge = [](std::vector<Picked>& into, std::vector<Picked> more) {
        for (auto& p : more)
            if (std::none_of(into.begin(), into.end(), [&](const Picked& x) { return x.label == p.label; })) into.push_back(std::move(p));
    };
    if (f.pc != g_props_pc) {
        g_props_pc = f.pc;
        g_pc_props = f.pc ? target_props(f.pc) : std::vector<Picked>{};
        if (f.pc) merge(g_pc_props, pick_props(uobj::class_of(f.pc), 3, {"battle", "lock", "mode", "state"}, {"target", "enemy", "camera"}));
    }
    if (f.pawn != g_props_pawn) {
        g_props_pawn = f.pawn;
        g_pawn_props = f.pawn ? target_props(f.pawn) : std::vector<Picked>{};
        if (f.pawn) {
            merge(g_pawn_props, pick_props(uobj::class_of(f.pawn), 4, {"battle", "lock", "mode", "state"}, {"target", "enemy", "combat"}));
            std::string names;
            for (const auto& p : g_pawn_props) names += " " + p.label + "(" + p.prop.type + ")";
            log::info("battle diag: pawn {} properties:{}", uobj::object_name(uobj::class_of(f.pawn)), names.empty() ? " none" : names);
        }
    }
    for (const auto& p : g_pc_props) vals.emplace_back("pc." + p.label, read_prop(f.pc, p.prop));
    for (const auto& p : g_pawn_props) vals.emplace_back("pawn." + p.label, read_prop(f.pawn, p.prop));
    for (void* o : g_cur.state_objects) {
        if (!uobj::alive(o)) continue;
        auto it = g_state_props.find(o);
        if (it == g_state_props.end())
            it = g_state_props
                     .emplace(o, pick_props(uobj::class_of(o), 3, {"battle", "state", "phase", "mode", "active"},
                                            {"enable", "flag", "type", "id", "count", "enemy"}))
                     .first;
        const std::string cn = uobj::object_name(uobj::class_of(o));
        if (it->second.empty()) vals.emplace_back(cn, "live");
        for (const auto& p : it->second) vals.emplace_back(cn + "." + p.label, read_prop(o, p.prop));
    }
    void* cm = g_cur.cam_mgr && uobj::alive(g_cur.cam_mgr) ? g_cur.cam_mgr : nullptr;
    if (cm) {
        if (cm != g_props_cam) {
            g_props_cam = cm;
            g_cam_props = pick_props(uobj::class_of(cm), 3, {"mode", "state", "battle", "camera"}, {"type", "active", "target", "index"});
            void* pcm = class_in_chain(uobj::class_of(cm), "PlayerCameraManager");
            g_cam_style_ok = pcm && find_prop(pcm, "CameraStyle", g_cam_style);
        }
        vals.emplace_back("cam.class", uobj::object_name(uobj::class_of(cm)));
        if (g_cam_style_ok) vals.emplace_back("cam.style", read_prop(cm, g_cam_style));
        for (const auto& p : g_cam_props) vals.emplace_back("cam." + p.label, read_prop(cm, p.prop));
    }
    vals.emplace_back("view", f.view_target ? std::format("{}({})", uobj::object_name(f.view_target), uobj::object_name(uobj::class_of(f.view_target)))
                                            : std::string("none"));
    bool changed = false;
    for (const auto& [k, v] : vals) {
        auto it = g_values.find(k);
        if (it == g_values.end()) {
            g_values.emplace(k, Value{v});
            changed = true;
            continue;
        }
        if (it->second.text == v) continue;
        it->second.text = v;
        if (it->second.noisy) continue;
        if (++it->second.changes > kNoisyChanges) {
            it->second.noisy = true;
            log::info("battle diag: {} changes too often; left out of the change test", k);
            continue;
        }
        changed = true;
    }
    const bool heartbeat = t >= g_next_heartbeat;
    if (heartbeat) {
        g_next_heartbeat = t + kHeartbeatSeconds;
        for (auto& [k, v] : g_values) v.changes = 0;
    }
    if (!changed && !heartbeat) return;
    std::string line = std::format("battle diag: t={}{} fp_applies={}", now_ms(), changed ? "" : " (heartbeat)", f.fp_applies ? 1 : 0);
    for (const auto& [k, v] : vals) line += " " + k + "=" + v;
    log::info("{}", line);
    ++g_battle_lines;
}

// "name(class) < outer(class) < ...": where a widget lives (a widget tree, a widget
// component of an actor, the game instance).
std::string outer_chain(void* o) {
    std::string r;
    for (int depth = 0; o && depth < 10; ++depth, o = uobj::outer_of(o))
        r += std::format("{}{}({})", r.empty() ? "" : " < ", uobj::object_name(o), uobj::object_name(uobj::class_of(o)));
    return r;
}

// The widget component (if any) that holds `widget`: its owner, EWidgetSpace (0 world, 1
// screen) and its location. One pass over the object array (a few tens of ms; once per new
// prompt instance).
std::string holder_of(void* widget) {
    std::unordered_map<void*, void*> wc_class;  // class -> its WidgetComponent super (or nullptr)
    Prop widget_prop, space_prop;
    bool props = false;
    int components = 0;
    const int n = uobj::num_objects();
    for (int i = 0; i < n; ++i) {
        void* o = uobj::object_at(i);
        if (!o) continue;
        void* cls = uobj::class_of(o);
        auto it = wc_class.find(cls);
        if (it == wc_class.end()) it = wc_class.emplace(cls, class_in_chain(cls, "WidgetComponent")).first;
        if (!it->second) continue;
        if (uobj::object_name(o).rfind("Default__", 0) == 0) continue;
        ++components;
        if (!props) {
            props = true;
            find_prop(it->second, "Widget", widget_prop);
            find_prop(it->second, "Space", space_prop);
        }
        void* w = nullptr;
        if (widget_prop.offset <= 0 || !uobj::read(static_cast<std::uint8_t*>(o) + widget_prop.offset, w) || w != widget) continue;
        std::uint8_t space = 0xff;
        if (space_prop.offset > 0) uobj::read(static_cast<std::uint8_t*>(o) + space_prop.offset, space);
        return std::format("held by widget component {} space {} (of {} components)", outer_chain(o), space, components);
    }
    return std::format("not held by a widget component ({} components, Widget property @{})", components, widget_prop.offset);
}

void poll_prompt(const Frame& f, double t) {
    if (t >= g_prompt_second + 1.0) {
        g_prompt_second = t;
        g_prompt_budget = 0;
    }
    std::string changes, all;
    for (const auto& w : g_cur.prompt_widgets) {
        const WidgetState s = widget_state(w);
        auto it = g_prompt_last.find(w.obj);
        const bool fresh = it == g_prompt_last.end();
        if (fresh && uobj::alive(w.obj))
            log::info("prompt diag: instance {} at index {}: {}; {}", w.label, uobj::index_of(w.obj), outer_chain(w.obj), w.rank > 0 ? holder_of(w.obj) : std::string("-"));
        const bool vis_change = fresh || it->second.visible != s.visible || it->second.in_viewport != s.in_viewport || it->second.vis_enum != s.vis_enum;
        const bool moved = !fresh && (std::abs(it->second.tx - s.tx) > 2.0f || std::abs(it->second.ty - s.ty) > 2.0f || it->second.slot != s.slot ||
                                      std::abs(it->second.sx - s.sx) > 2.0f || std::abs(it->second.sy - s.sy) > 2.0f);
        const std::string text = w.label + " " + widget_text(s);
        all += " | " + text;
        if (vis_change || moved) changes += " | " + text;
        g_prompt_last[w.obj] = s;
    }
    const Camera c = camera_facts(f);
    const std::string facts = camera_text(f, c);
    {
        std::lock_guard lk(g_status_mutex);
        g_prompt_status = std::format("t={} {}{}", now_ms(), facts, all.empty() ? std::string(" | no prompt widget found") : all);
    }
    if (changes.empty() || ++g_prompt_budget > 10) return;
    log::info("prompt diag: t={} {}{}", now_ms(), facts, changes);
    ++g_prompt_lines;
}

}  // namespace

void set_battle_log(bool on) {
    if (g_battle_on.exchange(on) != on) log::info("diag: battle log {}", on ? "on" : "off");
}
void set_prompt_log(bool on) {
    if (g_prompt_on.exchange(on) != on) log::info("diag: prompt log {}", on ? "on" : "off");
}

void player_frame(const Frame& f) {
    const bool battle = g_battle_on.load(std::memory_order_relaxed), prompt = g_prompt_on.load(std::memory_order_relaxed);
    if (!battle && !prompt) return;
    const double t = now_s();
    if (t < g_next_poll) return;
    g_next_poll = t + kPollSeconds;
    if (!uobj::available()) return;
    if (!g_fn)
        g_fn = new uobj::Lookup({{"IsVisible", "Widget", "Function"},
                                 {"IsInViewport", "UserWidget", "Function"},
                                 {"GetCameraLocation", "PlayerCameraManager", "Function"},
                                 {"GetCameraRotation", "PlayerCameraManager", "Function"},
                                 {"K2_GetActorLocation", "Actor", "Function"}});
    if (!g_fn->ready() && g_fn->passes() < 3) g_fn->step(65536);
    if (!g_fn_logged && (g_fn->ready() || g_fn->passes() >= 3)) {
        g_fn_logged = true;
        std::string r;
        for (const auto& e : g_fn->entries()) r += std::format(" {}.{}={}", e.outer, e.name, e.obj ? "found" : "missing");
        log::info("diag: functions{}", r);
    }
    // a slice of the object array per poll; the candidates are swapped in at the end of each pass
    const int n = uobj::num_objects();
    const int end = std::min(n, g_next.pos + kScanSlice);
    for (int i = g_next.pos; i < end; ++i)
        if (void* o = uobj::object_at(i)) scan_object(o);
    g_next.pos = end;
    if (end >= n) {
        ++g_passes;
        const std::string wbp_delta = map_delta(g_cur.wbp_classes, g_next.wbp_classes);
        const std::string battle_delta = map_delta(g_cur.battle_classes, g_next.battle_classes);
        const bool first = !g_scan_done;
        const bool cand_changed = list_text(g_cur.battle_widgets) != list_text(g_next.battle_widgets) ||
                                  list_text(g_cur.prompt_widgets) != list_text(g_next.prompt_widgets) || g_cur.cam_mgr != g_next.cam_mgr;
        g_cur = std::move(g_next);
        g_next = Scan{};
        g_scan_done = true;
        if (first) {
            log::info("battle diag: candidates: battle widgets [{}] camera manager {} battle classes [{}] signal raw {} ({} objects, {} classes)",
                      list_text(g_cur.battle_widgets), g_cur.cam_mgr ? uobj::path_of(g_cur.cam_mgr) : std::string("none"),
                      map_text(g_cur.battle_classes), f.battle_raw, n, g_classes.size());
            log::info("battle diag: widget blueprint classes: {}", map_text(g_cur.wbp_classes));
            log::info("prompt diag: candidates: [{}]", list_text(g_cur.prompt_widgets));
        } else {
            if (!wbp_delta.empty()) log::info("battle diag: t={} widget blueprint classes{}", now_ms(), wbp_delta);
            if (!battle_delta.empty()) log::info("battle diag: t={} battle classes{}", now_ms(), battle_delta);
            if (cand_changed)
                log::info("battle diag: t={} candidates now: battle widgets [{}] prompt widgets [{}]", now_ms(), list_text(g_cur.battle_widgets),
                          list_text(g_cur.prompt_widgets));
        }
        for (auto it = g_state_props.begin(); it != g_state_props.end();)
            it = uobj::alive(it->first) ? std::next(it) : g_state_props.erase(it);
        for (auto it = g_prompt_last.begin(); it != g_prompt_last.end();)
            it = uobj::alive(it->first) ? std::next(it) : g_prompt_last.erase(it);
    }
    if (!g_scan_done || (!g_fn->ready() && g_fn->passes() < 3)) return;
    if (battle) poll_battle(f, t);
    if (prompt) poll_prompt(f, t);
}

std::string command(const std::string& args) {
    auto flag = [&](std::size_t from) {
        const auto v = args.find_first_of("01", from);
        return v == std::string::npos ? -1 : args[v] == '1' ? 1 : 0;
    };
    if (args.rfind("battlelog", 0) == 0) {
        if (const int v = flag(9); v >= 0) set_battle_log(v == 1);
        return std::format("ok battle log {} ({} lines, {} scan passes)", g_battle_on.load() ? 1 : 0, g_battle_lines, g_passes);
    }
    if (args.rfind("promptlog", 0) == 0) {
        if (const int v = flag(9); v >= 0) set_prompt_log(v == 1);
        return std::format("ok prompt log {} ({} lines, {} scan passes)", g_prompt_on.load() ? 1 : 0, g_prompt_lines, g_passes);
    }
    if (args.rfind("prompt", 0) == 0) {
        std::lock_guard lk(g_status_mutex);
        return std::format("ok prompt log {} passes {} | {}", g_prompt_on.load() ? 1 : 0, g_passes,
                           g_prompt_on.load() ? g_prompt_status : std::string("off (fp promptlog 1)"));
    }
    return "err fp battlelog 0|1 | promptlog 0|1 | prompt status";
}

}  // namespace ff7vr::engine::diag
