#include "menu.h"

#include "comfort.h"
#include "foveation.h"
#include "menu_canvas.h"
#include "xr_controller.h"

#include "ff7vr/core/config.h"
#include "ff7vr/core/dev_commands.h"
#include "ff7vr/core/ini_file.h"
#include "ff7vr/core/live_settings.h"
#include "ff7vr/core/log.h"
#include "ff7vr/render/render.h"
#include "ff7vr_buildinfo.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace ff7vr::render {
bool MenuFilterPad(unsigned long user, PadState* s);
namespace menu {
namespace {

using Microsoft::WRL::ComPtr;
using live_settings::Kind;
using live_settings::Setting;

// Panel image and layout (pixels). 1024 x 720 shown 0.8 m wide at 1.2 m is about 37 x 26
// degrees: about 27 image pixels per degree, close to a Quest 3's display, so the 30 px
// body text is about 1.1 degrees high.
constexpr uint32_t kW = 1024, kH = 720;
constexpr int kTitleH = 78, kRowH = 46, kRows = 12, kFooterH = 60, kListTop = kTitleH + 8;
constexpr int kTitlePx = 36, kBodyPx = 30, kSmallPx = 23;

// XInput buttons.
constexpr unsigned short kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kView = 0x0020, kA = 0x1000, kB = 0x2000,
                         kX = 0x4000, kY = 0x8000;

struct Settings {
    int key = VK_DELETE;   // [menu] key
    bool pad = true;       // [menu] pad: View/Back + Y
    float distance = 1.2f; // [menu] distance (m)
    float width = 0.8f;    // [menu] size: width (m)
};
Settings g_cfg;
std::filesystem::path g_iniPath;
std::atomic<bool> g_started{false}, g_open{false};
std::atomic<uint64_t> g_placement{1};
std::atomic<int64_t> g_replaceAtMs{0};  // re-place the panel at this time (after a recenter)

int64_t NowMs() { return static_cast<int64_t>(GetTickCount64()); }

std::atomic<bool> g_anyFocus{false};  // `menu focus any` (tests): input also without the window focus

bool Focus() {
    if (g_anyFocus.load(std::memory_order_relaxed)) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// ---- gamepad, recorded by the pad filter (the game's XInput poll) ----
struct PadUser {
    std::atomic<uint32_t> buttons{0};
    std::atomic<int> lx{0}, ly{0};
    std::atomic<int64_t> seenMs{0};
    unsigned short prev = 0;  // under g_padMutex
    bool withhold = false;    // under g_padMutex: the game gets a neutral pad
};
PadUser g_pad[4];
std::mutex g_padMutex;
std::atomic<uint64_t> g_padPolls{0}, g_padWithheld{0}, g_padOpens{0};
std::atomic<uint32_t> g_padLastRaw{0};

// The real pad for navigation: the buttons of every pad polled in the last half second,
// and the left stick of the one pushed farthest.
void PadNow(unsigned short* buttons, int* lx, int* ly) {
    const int64_t now = NowMs();
    unsigned short b = 0;
    int bx = 0, by = 0;
    for (PadUser& u : g_pad) {
        if (now - u.seenMs.load(std::memory_order_relaxed) > 500) continue;
        b |= static_cast<unsigned short>(u.buttons.load(std::memory_order_relaxed));
        const int x = u.lx.load(std::memory_order_relaxed), y = u.ly.load(std::memory_order_relaxed);
        if (std::abs(x) + std::abs(y) > std::abs(bx) + std::abs(by)) bx = x, by = y;
    }
    *buttons = b;
    *lx = bx;
    *ly = by;
}

// ---- model: under g_mutex (the panel's thread and the dev pipe) ----
std::recursive_mutex g_mutex;
std::vector<Setting> g_items;
int g_sel = 0, g_top = 0;
std::string g_msg;
int64_t g_msgUntil = 0;
std::map<std::string, std::string> g_startValues;  // id (or id.key) -> ini value when the panel first opened
bool g_snapshot = false;
Canvas g_canvas;
bool g_fontTried = false;
std::string g_lastSignature;
int64_t g_lastDrawMs = 0;

// ---- image: drawn by the panel's thread, uploaded by the presenting thread ----
std::mutex g_pixMutex;
std::vector<uint8_t> g_pixels;
uint64_t g_pixVersion = 0;

// Presenting thread.
ComPtr<ID3D11Texture2D> g_tex;
ID3D11Device* g_texDevice = nullptr;
uint64_t g_uploaded = 0;
std::atomic<uint64_t> g_frames{0}, g_uploads{0};

// `menu dump`.
std::mutex g_dumpMutex;
std::condition_variable g_dumpCv;
bool g_dumpRequested = false;
std::string g_dumpPath, g_dumpResult;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Available(const Setting& s) { return !s.available || s.available(); }

void Message(std::string text) {
    std::lock_guard lk(g_mutex);
    g_msg = std::move(text);
    g_msgUntil = NowMs() + 4000;
}

void Refresh() {
    std::lock_guard lk(g_mutex);
    const std::string selId = g_sel < int(g_items.size()) ? g_items[size_t(g_sel)].id : std::string();
    g_items = live_settings::all();
    g_sel = 0;
    for (size_t i = 0; i < g_items.size(); ++i)
        if (g_items[i].id == selId) g_sel = int(i);
}

void KeepSelectionVisible() {
    const int n = int(g_items.size());
    g_sel = n ? std::clamp(g_sel, 0, n - 1) : 0;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + kRows) g_top = g_sel - kRows + 1;
    g_top = std::clamp(g_top, 0, std::max(0, n - kRows));
}

std::string ValueText(const Setting& s, bool selected) {
    if (s.kind == Kind::Action) return selected ? "press A" : "";
    std::string v = live_settings::display_value(s);
    if (v.empty() || v == "?") v = s.kind == Kind::Choice ? "custom" : "?";
    return selected && Available(s) ? "<   " + v + "   >" : v;
}

// Draws the panel into the canvas and publishes the image when it changed.
void Draw(bool force) {
    std::lock_guard lk(g_mutex);
    if (!g_fontTried) {
        g_fontTried = true;
        std::string which;
        if (g_canvas.LoadFont(&which))
            log::info("menu: font {}", which);
        else
            log::error("menu: no font found in the Windows fonts folder (segoeui.ttf, arial.ttf); the panel shows no text");
        g_canvas.Resize(kW, kH);
    }
    KeepSelectionVisible();
    const bool showMsg = !g_msg.empty() && NowMs() < g_msgUntil;
    // Everything shown, to skip drawing when nothing changed.
    std::string sig = std::format("{}|{}|{}|", g_sel, g_top, showMsg ? g_msg : std::string());
    std::vector<std::string> values(g_items.size());
    std::vector<bool> avail(g_items.size());
    for (size_t i = 0; i < g_items.size(); ++i) {
        avail[i] = Available(g_items[i]);
        values[i] = ValueText(g_items[i], int(i) == g_sel);
        sig += g_items[i].label + "=" + values[i] + (avail[i] ? "|" : "-|");
    }
    if (!force && sig == g_lastSignature) return;
    g_lastSignature = sig;

    Canvas& c = g_canvas;
    c.Clear();
    c.FillRect(0, 0, kW, kH, {14, 16, 22, 0.90f});
    c.FillRect(0, 0, kW, kTitleH, {32, 38, 52, 0.97f});
    const Rgba border{112, 136, 178, 1.0f};
    c.FillRect(0, 0, kW, 3, border);
    c.FillRect(0, kH - 3, kW, 3, border);
    c.FillRect(0, 0, 3, kH, border);
    c.FillRect(kW - 3, 0, 3, kH, border);
    const int titleBase = (kTitleH + c.Ascent(kTitlePx)) / 2 - 2;
    c.Text(28, titleBase, "FF7 Remake VR settings", kTitlePx, {240, 242, 248, 1.0f});
    const std::string build = std::string(FF7VR_VERSION);
    c.Text(int(kW) - 28 - c.TextWidth(build, kSmallPx), titleBase, build, kSmallPx, {150, 160, 180, 1.0f});

    const int n = int(g_items.size());
    for (int i = g_top; i < std::min(n, g_top + kRows); ++i) {
        const int y = kListTop + (i - g_top) * kRowH;
        const bool sel = i == g_sel;
        if (sel) c.FillRect(10, y + 2, int(kW) - 20 - 18, kRowH - 4, {46, 96, 172, 0.96f});
        const Rgba text = avail[size_t(i)] ? Rgba{236, 238, 244, 1.0f} : Rgba{120, 124, 134, 1.0f};
        const int base = y + (kRowH + c.Ascent(kBodyPx)) / 2 - 3;
        const Setting& s = g_items[size_t(i)];
        c.Text(32, base, s.label, kBodyPx, s.kind == Kind::Action && !sel ? Rgba{170, 200, 240, 1.0f} : text);
        const std::string& v = values[size_t(i)];
        if (!v.empty()) {
            const Rgba vc = sel ? Rgba{255, 255, 255, 1.0f} : (avail[size_t(i)] ? Rgba{196, 210, 232, 1.0f} : text);
            c.Text(int(kW) - 48 - c.TextWidth(v, kBodyPx), base, v, kBodyPx, vc);
        }
    }
    if (n > kRows) {
        // Scroll bar: where the visible rows are in the list.
        const int trackY = kListTop + 2, trackH = kRows * kRowH - 4, x = int(kW) - 24;
        c.FillRect(x, trackY, 8, trackH, {58, 64, 80, 1.0f});
        const int thumbH = std::max(24, trackH * kRows / n);
        const int thumbY = trackY + (trackH - thumbH) * g_top / std::max(1, n - kRows);
        c.FillRect(x, thumbY, 8, thumbH, {150, 170, 205, 1.0f});
    }
    const int footY = int(kH) - kFooterH;
    c.FillRect(10, footY, int(kW) - 20, 2, {70, 80, 100, 1.0f});
    const std::string foot = showMsg ? g_msg : "D-pad: choose    Left/Right: change    A: select    X: save    B: close";
    const int fw = c.TextWidth(foot, kSmallPx);
    c.Text((int(kW) - fw) / 2, footY + (kFooterH + c.Ascent(kSmallPx)) / 2 - 2, foot, kSmallPx,
           showMsg ? Rgba{255, 214, 120, 1.0f} : Rgba{176, 186, 204, 1.0f});
    {
        std::lock_guard pl(g_pixMutex);
        g_pixels = c.Pixels();
        ++g_pixVersion;
    }
    g_lastDrawMs = NowMs();
}

// The value a setting would write, for the save's comparison with the ini.
bool SameValue(std::string a, std::string b) {
    a = Lower(std::string(a));
    b = Lower(std::string(b));
    auto boolish = [](std::string& s) {
        if (s == "on" || s == "true" || s == "yes") s = "1";
        if (s == "off" || s == "false" || s == "no") s = "0";
    };
    boolish(a);
    boolish(b);
    double x = 0, y = 0;
    const auto [pa, ea] = std::from_chars(a.data(), a.data() + a.size(), x);
    const auto [pb, eb] = std::from_chars(b.data(), b.data() + b.size(), y);
    if (ea == std::errc() && eb == std::errc() && pa == a.data() + a.size() && pb == b.data() + b.size()) return std::fabs(x - y) < 1e-4;
    return a == b;
}

void TakeSnapshot() {
    if (g_snapshot) return;
    g_snapshot = true;
    for (const Setting& s : g_items) {
        if (s.section.empty() || s.kind == Kind::Action) continue;
        g_startValues[s.id] = live_settings::ini_value(s);
        if (s.extra_ini)
            for (const auto& [k, v] : s.extra_ini()) g_startValues[s.id + "." + k] = v;
    }
}

std::string Save() {
    std::lock_guard lk(g_mutex);
    TakeSnapshot();
    Config ini;
    ini.load(g_iniPath);
    std::vector<ini_file::Update> updates;
    auto consider = [&](const std::string& snapId, const std::string& section, const std::string& key, const std::string& value) {
        if (value.empty()) return;
        const auto cur = ini.get(section, key);
        // A key in the file: written when the value differs. A key the file does not have: only
        // when the value is not the one in effect when the panel first opened (the default).
        const bool differs = cur ? !SameValue(*cur, value) : !SameValue(g_startValues[snapId], value);
        if (differs) updates.push_back({section, key, value});
    };
    for (const Setting& s : g_items) {
        if (s.section.empty() || s.kind == Kind::Action) continue;
        consider(s.id, s.section, s.key, live_settings::ini_value(s));
        if (s.extra_ini)
            for (const auto& [k, v] : s.extra_ini()) consider(s.id + "." + k, s.section, k, v);
    }
    const std::string file = log::narrow(g_iniPath.wstring());
    if (updates.empty()) {
        log::info("menu: save: {} already holds every value shown; nothing written", file);
        return "Nothing to save: ff7vr.ini already has these values";
    }
    ini_file::Report report;
    std::string err;
    if (!ini_file::update_file(g_iniPath, updates, &report, &err)) {
        log::error("menu: save to {} failed: {}", file, err);
        return "Save failed: " + err;
    }
    for (const auto& l : report.changed) log::info("menu: saved {}", l);
    for (const auto& l : report.added) log::info("menu: saved {} (added)", l);
    log::info("menu: save: {} values written to {} (previous file kept as ff7vr.ini.bak)", report.changed.size() + report.added.size(), file);
    for (const Setting& s : g_items) {
        if (s.section.empty() || s.kind == Kind::Action) continue;
        g_startValues[s.id] = live_settings::ini_value(s);
    }
    return std::format("Saved {} setting{} to ff7vr.ini", updates.size(), updates.size() == 1 ? "" : "s");
}

void OpenPanel(const char* how) {
    std::lock_guard lk(g_mutex);
    if (g_open.load()) return;
    Refresh();
    TakeSnapshot();
    g_msg.clear();
    g_placement.fetch_add(1);
    Draw(true);
    g_open = true;
    log::info("menu: opened ({})", how);
}

void ClosePanel(const char* how) {
    if (!g_open.exchange(false)) return;
    log::info("menu: closed ({})", how);
}

// After a change to a setting of the graphics profile, the profile becomes custom so that
// the saved value is the one used at the next start.
void ProfileToCustom() {
    for (Setting& p : g_items)
        if (p.id == "graphics_profile" && p.get_choice && p.get_choice() != 0 && p.set_choice) {
            p.set_choice(0);
            log::info("menu: graphics profile is custom now (a setting of the profile was changed)");
        }
}

void Move(int d) {
    std::lock_guard lk(g_mutex);
    const int n = int(g_items.size());
    if (!n) return;
    g_sel = ((g_sel + d) % n + n) % n;
    KeepSelectionVisible();
}

void Change(int d) {
    std::lock_guard lk(g_mutex);
    if (g_sel >= int(g_items.size())) return;
    Setting& s = g_items[size_t(g_sel)];
    if (!Available(s)) {
        Message(s.label + " is not available now");
        return;
    }
    std::string err;
    if (s.kind == Kind::Choice && s.set_choice && !s.choices.empty()) {
        const int n = int(s.choices.size());
        const int cur = s.get_choice ? s.get_choice() : -1;
        const int next = cur < 0 ? (d > 0 ? 0 : n - 1) : ((cur + d) % n + n) % n;
        err = s.set_choice(next);
        log::info("menu: {} -> {}{}", s.label, s.choices[size_t(next)], err.empty() ? "" : " (" + err + ")");
    } else if (s.kind == Kind::Number && s.set_number && s.get_number) {
        float v = s.get_number() + float(d) * s.step;
        v = s.min + std::round((v - s.min) / s.step) * s.step;
        v = std::clamp(v, s.min, s.max);
        err = s.set_number(v);
        log::info("menu: {} -> {:.{}f}{}", s.label, v, s.decimals, err.empty() ? "" : " (" + err + ")");
    } else {
        return;
    }
    if (!err.empty()) Message(s.label + ": " + err);
    else if (s.part_of_profile) ProfileToCustom();
}

void Select() {
    std::lock_guard lk(g_mutex);
    if (g_sel >= int(g_items.size())) return;
    Setting& s = g_items[size_t(g_sel)];
    if (s.kind == Kind::Action) {
        if (s.run) {
            const std::string r = s.run();
            log::info("menu: {}: {}", s.label, r.empty() ? "done" : r);
            if (!r.empty()) Message(r);
        }
    } else if (s.kind == Kind::Choice) {
        Change(+1);
    }
}

// ---- the panel's thread: keyboard and gamepad ----
struct Repeat {
    bool down = false;
    int64_t next = 0;
    // True on the press and then, while held, after 400 ms every 110 ms.
    bool Fire(bool now, int64_t t) {
        if (!now) {
            down = false;
            return false;
        }
        if (!down) {
            down = true;
            next = t + 400;
            return true;
        }
        if (t >= next) {
            next = t + 110;
            return true;
        }
        return false;
    }
};

void ThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"ff7vr menu");
    bool keyDown[256] = {};
    auto edge = [&](int vk, bool focus) {
        const bool down = focus && vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool pressed = down && !keyDown[vk];
        if (vk > 0 && vk < 256) keyDown[vk] = down;
        return pressed;
    };
    auto held = [&](int vk, bool focus) { return focus && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    Repeat rep[4];
    unsigned short padPrev = 0;
    bool wasOpen = false;
    for (;;) {
        Sleep(15);
        const int64_t t = NowMs();
        const bool focus = Focus();
        if (const int64_t at = g_replaceAtMs.load(); at && t >= at) {
            g_replaceAtMs = 0;
            g_placement.fetch_add(1);
        }
        if (edge(g_cfg.key, focus)) {
            if (g_open.load()) ClosePanel("key");
            else OpenPanel("key");
        }
        const bool open = g_open.load();
        unsigned short pad = 0;
        int lx = 0, ly = 0;
        PadNow(&pad, &lx, &ly);
        if (!focus) pad = 0, lx = 0, ly = 0;
        if (open && !wasOpen) padPrev = pad;  // buttons held while it opened (View + Y) are not presses
        wasOpen = open;
        // Keys used while open: tracked always, so a key held when the panel opens is no press.
        const bool kEnter = edge(VK_RETURN, focus), kEsc = edge(VK_ESCAPE, focus), kS = edge('S', focus);
        if (!open) {
            padPrev = pad;
            continue;
        }
        const unsigned short pressed = static_cast<unsigned short>(pad & ~padPrev);
        padPrev = pad;
        constexpr int kStick = 20000;
        const bool dir[4] = {held(VK_UP, focus) || (pad & kUp) || ly > kStick, held(VK_DOWN, focus) || (pad & kDown) || ly < -kStick,
                             held(VK_LEFT, focus) || (pad & kLeft) || lx < -kStick, held(VK_RIGHT, focus) || (pad & kRight) || lx > kStick};
        if (rep[0].Fire(dir[0], t)) Move(-1);
        if (rep[1].Fire(dir[1], t)) Move(+1);
        if (rep[2].Fire(dir[2], t)) Change(-1);
        if (rep[3].Fire(dir[3], t)) Change(+1);
        if ((pressed & kA) || kEnter) Select();
        if ((pressed & kX) || kS) Message(Save());
        if ((pressed & kB) || kEsc || ((pressed & kY) && (pad & kView))) ClosePanel(kEsc ? "Escape" : "gamepad");
        if (g_open.load()) Draw(false);  // only publishes a new image when something shown changed
    }
}

// ---- the render module's settings ----
Setting Number(const char* id, const char* label, int order, const char* section, const char* key, float mn, float mx, float step, int dec,
               std::function<float()> get, std::function<std::string(float)> set) {
    Setting s;
    s.id = id;
    s.label = label;
    s.order = order;
    s.kind = Kind::Number;
    s.section = section;
    s.key = key;
    s.min = mn;
    s.max = mx;
    s.step = step;
    s.decimals = dec;
    s.get_number = std::move(get);
    s.set_number = std::move(set);
    return s;
}

std::string OkOrError(const std::string& reply) { return reply.starts_with("ok") ? std::string() : reply; }

std::string Dispatch(const std::string& line) {
    std::string reply;
    if (!dev_commands::dispatch(line, reply)) return "err no handler for " + line;
    return reply;
}

void RegisterSettings() {
    XrController& xc = XrController::Get();
    live_settings::add(Number("vignette", "Comfort vignette", 30, "comfort", "vignette", 0.0f, 1.0f, 0.1f, 1, [] { return comfort::VignetteStrength(); },
                              [](float v) { return OkOrError(comfort::Command(std::format("vignette {:.2f}", v))); }));
    live_settings::add(Number(
        "hud_distance", "HUD distance (m)", 40, "ui", "distance", 0.75f, 8.0f, 0.25f, 2,
        [&xc] {
            float d = 0;
            xc.UiPlacement(&d, nullptr, nullptr);
            return d;
        },
        [&xc](float v) { return OkOrError(xc.UiCommand(std::format("distance {:.2f}", v))); }));
    live_settings::add(Number(
        "hud_size", "HUD size (m)", 50, "ui", "size", 0.5f, 4.0f, 0.1f, 2,
        [&xc] {
            float s = 0;
            xc.UiPlacement(nullptr, &s, nullptr);
            return s;
        },
        [&xc](float v) { return OkOrError(xc.UiCommand(std::format("size {:.2f}", v))); }));
    {
        Setting s;
        s.id = "foveation";
        s.label = "Foveated rendering";
        s.order = 80;
        s.kind = Kind::Choice;
        s.section = "foveation";
        s.key = "preset";
        s.choices = {"Off", "Quality", "Balanced", "Performance"};
        s.ini_values = {"off", "quality", "balanced", "performance"};
        s.part_of_profile = true;
        s.get_choice = [] {
            const std::string p = foveation::PresetName();
            for (int i = 0; i < 4; ++i)
                if (p == (i == 0 ? "off" : i == 1 ? "quality" : i == 2 ? "balanced" : "performance")) return i;
            return -1;  // custom radii or rates
        };
        s.set_choice = [](int i) {
            const char* names[] = {"off", "quality", "balanced", "performance"};
            return OkOrError(foveation::Command(std::string("preset ") + names[std::clamp(i, 0, 3)]));
        };
        s.extra_ini = [] {
            return std::vector<std::pair<std::string, std::string>>{{"enabled", foveation::PresetName() == "off" ? "0" : "1"}};
        };
        live_settings::add(std::move(s));
    }
    auto pic = [&xc](const char* id, const char* label, int order, const char* key, float mn, float mx, float xr::PictureAdjust::*field) {
        live_settings::add(Number(
            id, label, order, "picture", key, mn, mx, 0.05f, 2, [&xc, field] { return xc.Picture().*field; },
            [&xc, key](float v) { return OkOrError(xc.PictureCommand(std::format("{} {:.3f}", key, v))); }));
    };
    pic("brightness", "Brightness", 90, "brightness", -1.0f, 1.0f, &xr::PictureAdjust::brightness);
    pic("contrast", "Contrast", 91, "contrast", 0.5f, 2.0f, &xr::PictureAdjust::contrast);
    pic("saturation", "Saturation", 92, "saturation", 0.0f, 2.0f, &xr::PictureAdjust::saturation);
    pic("gamma", "Gamma", 93, "gamma", 0.5f, 2.0f, &xr::PictureAdjust::gamma);
    live_settings::add(Number("sharpen", "Sharpen", 100, "picture", "sharpen", 0.0f, 1.0f, 0.1f, 1, [] { return comfort::Sharpen(); },
                              [](float v) { return OkOrError(Dispatch(std::format("sharpen {:.2f}", v))); }));
    {
        Setting s;
        s.id = "recenter";
        s.label = "Recenter";
        s.order = 150;
        s.kind = Kind::Action;
        s.run = [&xc] {
            const std::string r = xc.Recenter();
            if (!r.starts_with("ok")) return std::string("Recenter: ") + r;
            g_replaceAtMs = NowMs() + 200;  // then the panel moves in front again, in the new origin
            return std::string("Recentered");
        };
        live_settings::add(std::move(s));
    }
    {
        Setting s;
        s.id = "save";
        s.label = "Save settings to ff7vr.ini";
        s.order = 170;
        s.kind = Kind::Action;
        s.run = [] { return Save(); };
        live_settings::add(std::move(s));
    }
    {
        Setting s;
        s.id = "close";
        s.label = "Close";
        s.order = 180;
        s.kind = Kind::Action;
        s.run = [] {
            ClosePanel("Close");
            return std::string();
        };
        live_settings::add(std::move(s));
    }
}

std::string StatusText() {
    std::lock_guard lk(g_mutex);
    std::string items;
    for (size_t i = 0; i < g_items.size(); ++i) {
        const Setting& s = g_items[i];
        if (s.kind == Kind::Action) continue;
        items += std::format("{}{}{}={}", items.empty() ? "" : ", ", int(i) == g_sel ? "*" : "", s.id, live_settings::display_value(s));
    }
    return std::format("ok menu {}; selected {} '{}'; frames shown {} images uploaded {}; pad: polls {} withheld from the game {} opened {} "
                       "last raw {:#06x}; key {} pad {}; {:.2f} m wide at {:.2f} m; ini {}; {}",
                       g_open.load() ? "open" : "closed", g_sel, g_sel < int(g_items.size()) ? g_items[size_t(g_sel)].label : std::string(),
                       g_frames.load(), g_uploads.load(), g_padPolls.load(), g_padWithheld.load(), g_padOpens.load(), g_padLastRaw.load(), g_cfg.key,
                       g_cfg.pad ? 1 : 0, g_cfg.width, g_cfg.distance, log::narrow(g_iniPath.wstring()), items);
}

}  // namespace

void Start(const Config& c, const std::filesystem::path& iniPath) {
    if (g_started.exchange(true)) return;
    g_cfg.key = static_cast<int>(std::clamp<long long>(c.get_int("menu", "key", g_cfg.key), 0, 255));
    g_cfg.pad = c.get_bool("menu", "pad", g_cfg.pad);
    g_cfg.distance = static_cast<float>(std::clamp(c.get_float("menu", "distance", g_cfg.distance), 0.3, 10.0));
    g_cfg.width = static_cast<float>(std::clamp(c.get_float("menu", "size", g_cfg.width), 0.1, 5.0));
    g_iniPath = iniPath;
    RegisterSettings();
    dev_commands::add("menu",
                      "menu status | open | close | toggle | up | down | left | right | select | back | save | dump <png path>: the in-headset "
                      "settings panel",
                      [](std::string_view args) { return Command(std::string(args)); });
    std::thread(ThreadMain).detach();
    log::info("menu: settings panel on key {} and {}; {:.2f} m wide at {:.2f} m; saves to {}", g_cfg.key,
              g_cfg.pad ? "View/Back + Y" : "no gamepad combination", g_cfg.width, g_cfg.distance, log::narrow(g_iniPath.wstring()));
}

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

bool Frame(ID3D11Device* device, ID3D11DeviceContext* ctx, bool forceContent, PanelFrame* out) {
    if (!g_open.load(std::memory_order_acquire) || !device || !ctx) return false;
    if (!g_tex || g_texDevice != device) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kW;
        td.Height = kH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_tex.Reset();
        if (FAILED(device->CreateTexture2D(&td, nullptr, &g_tex))) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) log::error("menu: the panel texture could not be created");
            return false;
        }
        g_texDevice = device;
        g_uploaded = 0;
    }
    bool uploaded = false;
    {
        std::lock_guard pl(g_pixMutex);
        if (g_pixels.size() == size_t(kW) * kH * 4 && (forceContent || g_uploaded != g_pixVersion)) {
            ctx->UpdateSubresource(g_tex.Get(), 0, nullptr, g_pixels.data(), kW * 4, 0);
            g_uploaded = g_pixVersion;
            uploaded = true;
        }
    }
    if (g_uploaded == 0) return false;  // nothing drawn yet
    if (uploaded) ++g_uploads;
    ++g_frames;
    out->texture = uploaded ? g_tex.Get() : nullptr;
    out->width = kW;
    out->height = kH;
    out->widthM = g_cfg.width;
    out->heightM = g_cfg.width * float(kH) / float(kW);
    out->distance = g_cfg.distance;
    out->placement = g_placement.load();
    {
        std::unique_lock lk(g_dumpMutex);
        if (g_dumpRequested) {
            std::string err;
            const bool ok = xr::WriteTexturePng(ctx, g_tex.Get(), g_dumpPath, &err);
            g_dumpResult = ok ? std::format("ok {} ({}x{} panel texture)", g_dumpPath, kW, kH) : "err " + err;
            g_dumpRequested = false;
            g_dumpCv.notify_all();
        }
    }
    return true;
}

std::string Command(const std::string& argsIn) {
    std::string args = argsIn;
    while (!args.empty() && args.back() == ' ') args.pop_back();
    const size_t sp = args.find(' ');
    const std::string verb = Lower(args.substr(0, sp));
    std::string rest = sp == std::string::npos ? std::string() : args.substr(sp + 1);
    if (verb.empty() || verb == "status") return StatusText();
    if (verb == "open") {
        OpenPanel("dev command");
        return StatusText();
    }
    if (verb == "close" || verb == "back") {
        ClosePanel("dev command");
        return StatusText();
    }
    if (verb == "toggle") {
        if (g_open.load()) ClosePanel("dev command");
        else OpenPanel("dev command");
        return StatusText();
    }
    if (verb == "focus") {
        if (rest != "any" && rest != "window") return "err usage: menu focus any|window";
        g_anyFocus = rest == "any";
        return std::string("ok menu input ") + (g_anyFocus.load() ? "also without the window focus (test)" : "only while the game window has the focus");
    }
    if (verb == "pad") {
        // Test without a pad: one XInput state through the panel's filter, as the game's poll
        // would pass it (buttons in hex, optional user index); replies with what the game gets.
        std::istringstream in(rest);
        std::string hex;
        unsigned long user = 0;
        in >> hex >> user;
        if (hex.empty() || user > 3) return "err usage: menu pad <hex buttons> [user 0..3]";
        PadState s{};
        s.buttons = static_cast<unsigned short>(std::strtoul(hex.c_str(), nullptr, 16));
        s.ly = 1234;
        const bool withheld = MenuFilterPad(user, &s);
        return std::format("ok pad {} {:#06x}: the game gets buttons {:#06x} left stick y {} ({})", user, std::strtoul(hex.c_str(), nullptr, 16),
                           s.buttons, s.ly, withheld ? "withheld by the panel" : "passed through");
    }
    if (verb == "dump") {
        if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
        if (rest.empty()) return "err usage: menu dump <png path>";
        if (!g_open.load()) return "err the panel is closed (menu open first)";
        std::unique_lock lk(g_dumpMutex);
        g_dumpPath = rest;
        g_dumpResult.clear();
        g_dumpRequested = true;
        if (!g_dumpCv.wait_for(lk, std::chrono::seconds(5), [] { return !g_dumpResult.empty(); })) {
            g_dumpRequested = false;
            return "err the panel was not drawn within 5 s (no XR frame submitted?)";
        }
        return g_dumpResult;
    }
    if (!g_open.load()) return "err the panel is closed (menu open first)";
    if (verb == "up") Move(-1);
    else if (verb == "down") Move(+1);
    else if (verb == "left") Change(-1);
    else if (verb == "right") Change(+1);
    else if (verb == "select") Select();
    else if (verb == "save") Message(Save());
    else return "err usage: menu status | open | close | toggle | up | down | left | right | select | back | save | dump <png path>";
    Draw(false);
    return StatusText();
}

}  // namespace menu

// The game's XInput poll (any thread, every user index): records the real pad for the
// panel, opens it on View/Back + Y, and withholds the pad from the game while it is open
// and until every button that was down is released after it closed.
bool MenuFilterPad(unsigned long user, PadState* s) {
    using namespace menu;
    if (!g_started.load(std::memory_order_relaxed) || user >= 4 || !s) return false;
    PadUser& u = g_pad[user];
    u.buttons.store(s->buttons, std::memory_order_relaxed);
    u.lx.store(s->lx, std::memory_order_relaxed);
    u.ly.store(s->ly, std::memory_order_relaxed);
    u.seenMs.store(NowMs(), std::memory_order_relaxed);
    g_padPolls.fetch_add(1, std::memory_order_relaxed);
    g_padLastRaw.store(s->buttons, std::memory_order_relaxed);
    bool withhold = false;
    bool openNow = false;
    {
        std::lock_guard lk(g_padMutex);
        const unsigned short pressed = static_cast<unsigned short>(s->buttons & ~u.prev);
        u.prev = s->buttons;
        if (!g_open.load() && g_cfg.pad && (s->buttons & kView) && (pressed & kY) && Focus()) openNow = true;
        if (g_open.load() || openNow) {
            u.withhold = true;
        } else if (u.withhold && s->buttons == 0 && s->leftTrigger < 30 && s->rightTrigger < 30) {
            u.withhold = false;  // closed and everything released: the game gets the pad again
        }
        withhold = u.withhold;
    }
    if (openNow) {
        g_padOpens.fetch_add(1);
        std::thread([] { OpenPanel("gamepad View/Back + Y"); }).detach();  // never block the game's input poll
    }
    if (withhold) {
        g_padWithheld.fetch_add(1, std::memory_order_relaxed);
        *s = PadState{};
    }
    return withhold;
}

}  // namespace ff7vr::render
