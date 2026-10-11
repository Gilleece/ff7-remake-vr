#include "menu.h"

#include "comfort.h"
#include "foveation.h"
#include "menu_canvas.h"
#include "timing.h"
#include "video_memory.h"
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
constexpr int kTitleH = 56, kTabTop = 56, kTabH = 50, kInfoTop = kTabTop + kTabH, kInfoH = 34, kRowH = 46, kFooterH = 74;
constexpr int kListTop = kInfoTop + kInfoH + 4;
constexpr int kRows = (int(kH) - kFooterH - kListTop - 4) / kRowH;  // 10
constexpr int kTitlePx = 32, kBodyPx = 30, kTabPx = 25, kSmallPx = 23;

// XInput buttons.
constexpr unsigned short kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kView = 0x0020, kLB = 0x0100, kRB = 0x0200, kA = 0x1000,
                         kB = 0x2000, kX = 0x4000, kY = 0x8000;

const char* const kCategories[] = {"Picture", "Comfort", "View", "HUD", "Performance", "Controls", "Advanced"};
constexpr int kNumCategories = int(std::size(kCategories));

// [menu] keys; changed live from the panel itself.
struct Settings {
    std::atomic<int> key{VK_DELETE};    // [menu] key
    std::atomic<bool> pad{true};        // [menu] pad: View/Back + Y
    std::atomic<float> distance{1.2f};  // [menu] distance (m)
    std::atomic<float> width{0.8f};     // [menu] size: width (m)
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

// ---- gamepad, recorded by the pad filter: XInput users 0..3, PlayStation pads 4..7 ----
constexpr unsigned long kPadSlots = 8;
struct PadUser {
    std::atomic<uint32_t> buttons{0};
    std::atomic<int> lx{0}, ly{0};
    std::atomic<int64_t> seenMs{0};
    unsigned short prev = 0;  // under g_padMutex
    bool withhold = false;    // under g_padMutex: the game gets a neutral pad
};
PadUser g_pad[kPadSlots];
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

// ---- keyboard: the keys the panel uses never reach the game while it is open ----
// A WH_GETMESSAGE hook on the game window's thread turns their WM_KEYDOWN / WM_SYSKEYDOWN /
// WM_CHAR into WM_NULL before the game's message loop translates or dispatches them. Key-up
// messages pass (a key held when the panel opened is released normally in the game).
HHOOK g_kbHook = nullptr;  // the panel's thread (installed) and Stop
std::atomic<DWORD> g_kbThread{0};
std::atomic<uint64_t> g_kbSwallowed{0};
std::atomic<int64_t> g_kbNextTryMs{0};

bool PanelKey(WPARAM vk) {
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN || vk == 'S' || vk == VK_BACK ||
           (g_cfg.key.load(std::memory_order_relaxed) > 0 && vk == WPARAM(g_cfg.key.load(std::memory_order_relaxed)));
}

LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam && g_open.load(std::memory_order_relaxed)) {
        MSG* m = reinterpret_cast<MSG*>(lParam);
        bool drop = false;
        if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) drop = PanelKey(m->wParam);
        else if (m->message == WM_CHAR) drop = m->wParam == '\r' || m->wParam == 's' || m->wParam == 'S' || m->wParam == '\b';
        if (drop) {
            if (wParam == PM_REMOVE) g_kbSwallowed.fetch_add(1, std::memory_order_relaxed);
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

struct FindWindowData {
    DWORD pid;
    HWND hwnd;
};

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<FindWindowData*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != d->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (std::wstring_view(cls) != L"UnrealWindow") return TRUE;
    d->hwnd = hwnd;
    return FALSE;
}

// The panel's thread: installs the hook once the game's window exists (tried every second).
void EnsureKeyboardHook() {
    if (g_kbHook || NowMs() < g_kbNextTryMs.load()) return;
    g_kbNextTryMs = NowMs() + 1000;
    FindWindowData d{GetCurrentProcessId(), nullptr};
    EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&d));
    if (!d.hwnd) return;
    const DWORD tid = GetWindowThreadProcessId(d.hwnd, nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GetMsgProc), &self);
    g_kbHook = SetWindowsHookExW(WH_GETMESSAGE, &GetMsgProc, self, tid);
    if (!g_kbHook) {
        log::warn("menu: the keyboard hook on the game window's thread {} failed (error {}); the game also sees the panel's keys", tid, GetLastError());
        g_kbNextTryMs = NowMs() + 60000;
        return;
    }
    g_kbThread = tid;
    log::info("menu: keyboard hook on the game window's thread {}: the panel's keys do not reach the game while it is open", tid);
}

// ---- model: under g_mutex (the panel's thread and the dev pipe) ----
std::recursive_mutex g_mutex;
std::vector<Setting> g_items;  // every setting, sorted by order
int g_cat = 0;                 // current category
int g_sel = -1;                // row in the current category; -1 = the category row
int g_top = 0;
std::string g_msg;
int64_t g_msgUntil = 0;
std::string g_armed;  // an action that needs a second select ("reset all")
int64_t g_armedUntil = 0;
std::map<std::string, std::string> g_startValues;  // id (or id.key) -> ini value when the panel first opened
bool g_snapshot = false;
Canvas g_canvas;
bool g_fontTried = false;
std::string g_lastSignature;

// Performance figures (the panel's thread).
std::string g_perfLine;
int64_t g_perfNextMs = 0;
double g_fpsAvg = 0;
std::mutex g_devMutex;
ComPtr<ID3D11Device> g_vramDevice;  // under g_devMutex: set by Frame, queried by the panel's thread

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

// ---- key names ----
std::string KeyName(int vk) {
    if (vk == 0) return "no key";
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk) {
        case VK_BACK: return "Backspace";
        case VK_TAB: return "Tab";
        case VK_RETURN: return "Enter";
        case VK_PAUSE: return "Pause";
        case VK_CAPITAL: return "Caps Lock";
        case VK_ESCAPE: return "Escape";
        case VK_SPACE: return "Space";
        case VK_PRIOR: return "Page Up";
        case VK_NEXT: return "Page Down";
        case VK_END: return "End";
        case VK_HOME: return "Home";
        case VK_LEFT: return "Left";
        case VK_UP: return "Up";
        case VK_RIGHT: return "Right";
        case VK_DOWN: return "Down";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_MULTIPLY: return "Numpad *";
        case VK_ADD: return "Numpad +";
        case VK_SUBTRACT: return "Numpad -";
        case VK_DECIMAL: return "Numpad .";
        case VK_DIVIDE: return "Numpad /";
        case VK_NUMLOCK: return "Num Lock";
        case VK_SCROLL: return "Scroll Lock";
        case VK_OEM_1: return ";";
        case VK_OEM_PLUS: return "=";
        case VK_OEM_COMMA: return ",";
        case VK_OEM_MINUS: return "-";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_2: return "/";
        case VK_OEM_3: return "`";
        case VK_OEM_4: return "[";
        case VK_OEM_5: return "\\";
        case VK_OEM_6: return "]";
        case VK_OEM_7: return "'";
        default: return "key";
    }
}

// The keys Left/Right step through on a key-code row (any other code can be set in the ini
// or with `menu set`).
const std::vector<int>& CommonKeys() {
    static const std::vector<int> keys = [] {
        std::vector<int> k = {0, VK_BACK, VK_TAB, VK_RETURN, VK_PAUSE, VK_SPACE, VK_PRIOR, VK_NEXT, VK_END, VK_HOME, VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN,
                              VK_INSERT, VK_DELETE};
        for (int c = '0'; c <= '9'; ++c) k.push_back(c);
        for (int c = 'A'; c <= 'Z'; ++c) k.push_back(c);
        for (int c = VK_NUMPAD0; c <= VK_DIVIDE; ++c)
            if (c != VK_SEPARATOR) k.push_back(c);
        for (int c = VK_F1; c <= VK_F12; ++c) k.push_back(c);
        for (int c : {VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7})
            k.push_back(c);
        std::sort(k.begin(), k.end());
        return k;
    }();
    return keys;
}

// ---- rows of the current category ----
// The category's settings, then its actions (reset this category, reset all, save, close).
std::vector<int> g_rows;  // indexes into g_items; actions are in g_items too (category "*")

void BuildRows() {
    g_rows.clear();
    const std::string cat = kCategories[g_cat];
    for (size_t i = 0; i < g_items.size(); ++i)
        if (g_items[i].category == cat) g_rows.push_back(int(i));
    for (size_t i = 0; i < g_items.size(); ++i)
        if (g_items[i].category == "*") g_rows.push_back(int(i));
}

Setting* SelectedSetting() {
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return nullptr;
    return &g_items[size_t(g_rows[size_t(g_sel)])];
}

void Refresh() {
    std::lock_guard lk(g_mutex);
    const Setting* cur = SelectedSetting();
    const std::string selId = cur ? cur->id : std::string();
    g_items = live_settings::all();
    for (Setting& s : g_items)
        if (s.category.empty()) s.category = s.kind == Kind::Action ? "*" : "Advanced";
    BuildRows();
    if (!selId.empty())
        for (size_t r = 0; r < g_rows.size(); ++r)
            if (g_items[size_t(g_rows[r])].id == selId) g_sel = int(r);
}

void KeepSelectionVisible() {
    const int n = int(g_rows.size());
    if (g_sel >= n) g_sel = n - 1;
    if (g_sel < -1) g_sel = -1;
    if (g_sel >= 0 && g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + kRows) g_top = g_sel - kRows + 1;
    if (g_sel < 0) g_top = 0;
    g_top = std::clamp(g_top, 0, std::max(0, n - kRows));
}

std::string ValueText(const Setting& s, bool selected) {
    if (s.kind == Kind::Action) return selected ? (g_armed == s.id ? "press A again" : "press A") : "";
    std::string v = live_settings::display_value(s);
    if (s.kind == Kind::Number && s.key_code && s.get_number) {
        const float f = s.get_number();
        if (std::isfinite(f)) v = std::format("{} ({})", KeyName(int(std::lround(f))), int(std::lround(f)));
    }
    if (s.kind == Kind::Text && v.empty()) v = "(empty)";
    if (v.empty() || v == "?") v = s.kind == Kind::Choice ? "custom" : "?";
    if (s.optional && s.get_is_set && !s.get_is_set()) v = "not set (" + v + ")";
    const bool changeable = Available(s) && !s.read_only && s.kind != Kind::Text;
    return selected && changeable ? "<  " + v + "  >" : v;
}

std::string HelpText(const Setting& s) {
    if (s.kind == Kind::Action) return {};
    std::string h = s.section.empty() ? std::string("not saved (changes the game now only)") : "[" + s.section + "] " + s.key;
    if (s.read_only) h += "  -  read only";
    else if (s.restart) h += "  -  takes effect at the next start";
    const std::string def = live_settings::default_of(s);
    if (!s.section.empty() && !s.read_only) {
        bool unset = false;
        live_settings::default_value(s.section, s.key, &unset);
        h += "  -  default " + (s.optional && unset ? std::string("not set") : def.empty() ? std::string("(empty)") : def);
    }
    if (s.optional) h += "  -  A: set / not set";
    else if (s.kind == Kind::Text) h += "  -  Y: reset to default";
    return h;
}

// Live figures for the Performance category (the panel's thread, at most twice a second).
void UpdatePerformance() {
    const int64_t now = NowMs();
    uint64_t n = 0;
    const double interval = GetTiming().frameInterval.Last(&n);
    if (n && interval > 0.1) {
        const double fps = 1000.0 / interval;
        g_fpsAvg = g_fpsAvg <= 0 ? fps : g_fpsAvg + (fps - g_fpsAvg) * 0.1;
    }
    if (now < g_perfNextMs) return;
    g_perfNextMs = now + 500;
    std::string line;
    EyeSetup eye{};
    const bool haveEye = GetEyeSetup(&eye) && eye.refreshHz > 0;
    line = g_fpsAvg > 0 ? std::format("{:.0f} fps", g_fpsAvg) : std::string("- fps");
    if (haveEye) line += std::format(" of {:.0f} Hz", eye.refreshHz);
    double scene = 0, after = 0;
    if (foveation::LastGpuTimes(&scene, &after)) line += std::format("  |  GPU scene {:.1f} + after {:.1f} ms", scene, after);
    ComPtr<ID3D11Device> dev;
    {
        std::lock_guard lk(g_devMutex);
        dev = g_vramDevice;
    }
    if (dev) {
        const video_memory::Info vi = video_memory::Query(dev.Get());
        if (vi.valid && vi.localBudgetMb > 0)
            line += std::format("  |  VRAM {:.1f} of {:.1f} GB", vi.localUsageMb / 1024.0, vi.localBudgetMb / 1024.0);
    }
    const std::string tex = live_settings::status("texstream");
    if (!tex.empty()) line += "  |  " + tex;
    const std::string budget = live_settings::status("frame_budget");
    if (!budget.empty()) line += "\n" + budget;
    std::lock_guard lk(g_mutex);
    g_perfLine = line;
}

// Draws text clipped to a width (with "..." when it does not fit).
void FitText(Canvas& c, int x, int base, std::string t, int px, int maxW, Rgba col) {
    if (c.TextWidth(t, px) > maxW) {
        while (!t.empty() && c.TextWidth(t + "...", px) > maxW) t.pop_back();
        t += "...";
    }
    c.Text(x, base, t, px, col);
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
    if (g_armed.size() && NowMs() >= g_armedUntil) g_armed.clear();
    KeepSelectionVisible();
    const bool showMsg = !g_msg.empty() && NowMs() < g_msgUntil;
    const bool perf = std::string(kCategories[g_cat]) == "Performance";
    // Everything shown, to skip drawing when nothing changed.
    std::string sig = std::format("{}|{}|{}|{}|{}|{}|", g_cat, g_sel, g_top, showMsg ? g_msg : std::string(), g_armed, perf ? g_perfLine : std::string());
    const int n = int(g_rows.size());
    std::vector<std::string> values(size_t(std::max(0, n)));
    std::vector<bool> avail(values.size()), restart(values.size()), changed(values.size());
    for (int r = g_top; r < std::min(n, g_top + kRows); ++r) {
        const Setting& s = g_items[size_t(g_rows[size_t(r)])];
        avail[size_t(r)] = Available(s) && !s.read_only;
        values[size_t(r)] = ValueText(s, r == g_sel);
        restart[size_t(r)] = s.restart;
        changed[size_t(r)] = !live_settings::is_default(s);
        sig += s.label + "=" + values[size_t(r)] + (avail[size_t(r)] ? "|" : "-|") + (changed[size_t(r)] ? "c" : "");
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
    const int titleBase = (kTitleH + c.Ascent(kTitlePx)) / 2 - 1;
    c.Text(24, titleBase, "FF7 Remake VR settings", kTitlePx, {240, 242, 248, 1.0f});
    const std::string build = std::string(FF7VR_VERSION);
    c.Text(int(kW) - 24 - c.TextWidth(build, kSmallPx), titleBase, build, kSmallPx, {150, 160, 180, 1.0f});

    // Category row.
    {
        int total = 0;
        std::vector<int> w(kNumCategories);
        for (int i = 0; i < kNumCategories; ++i) total += (w[size_t(i)] = c.TextWidth(kCategories[i], kTabPx) + 22);
        const int gap = std::max(2, (int(kW) - 20 - total) / (kNumCategories - 1 > 0 ? kNumCategories - 1 : 1));
        int x = 10;
        const int base = kTabTop + (kTabH + c.Ascent(kTabPx)) / 2 - 2;
        for (int i = 0; i < kNumCategories; ++i) {
            const bool cur = i == g_cat;
            if (cur) c.FillRect(x, kTabTop + 4, w[size_t(i)], kTabH - 8, g_sel < 0 ? Rgba{46, 96, 172, 0.96f} : Rgba{52, 60, 80, 1.0f});
            c.Text(x + 11, base, kCategories[i], kTabPx, cur ? Rgba{255, 255, 255, 1.0f} : Rgba{160, 170, 190, 1.0f});
            x += w[size_t(i)] + gap;
        }
        c.FillRect(10, kTabTop + kTabH - 2, int(kW) - 20, 2, {70, 80, 100, 1.0f});
    }
    // Info line: category name and position, or the live figures in Performance.
    {
        const int base = kInfoTop + (kInfoH + c.Ascent(kSmallPx)) / 2 - 2;
        const std::string pos = g_sel < 0 ? std::format("{} settings", n) : std::format("{} of {}", g_sel + 1, n);
        const int pw = c.TextWidth(pos, kSmallPx);
        c.Text(int(kW) - 24 - pw, base, pos, kSmallPx, {150, 160, 180, 1.0f});
        std::string info = kCategories[g_cat];
        if (perf && !g_perfLine.empty()) {
            const size_t nl = g_perfLine.find('\n');
            info = g_perfLine.substr(0, nl);
        }
        FitText(c, 24, base, info, kSmallPx, int(kW) - 24 - pw - 40, perf ? Rgba{140, 220, 160, 1.0f} : Rgba{200, 210, 230, 1.0f});
    }

    for (int r = g_top; r < std::min(n, g_top + kRows); ++r) {
        const int y = kListTop + (r - g_top) * kRowH;
        const bool sel = r == g_sel;
        const Setting& s = g_items[size_t(g_rows[size_t(r)])];
        if (sel) c.FillRect(10, y + 2, int(kW) - 20 - 18, kRowH - 4, {46, 96, 172, 0.96f});
        const Rgba text = avail[size_t(r)] ? Rgba{236, 238, 244, 1.0f} : Rgba{120, 124, 134, 1.0f};
        const int base = y + (kRowH + c.Ascent(kBodyPx)) / 2 - 3;
        const std::string& v = values[size_t(r)];
        int right = int(kW) - 48;
        if (!v.empty()) {
            const Rgba vc = sel ? Rgba{255, 255, 255, 1.0f} : (avail[size_t(r)] ? Rgba{196, 210, 232, 1.0f} : text);
            const int vw = std::min(c.TextWidth(v, kBodyPx), 460);
            FitText(c, right - vw, base, v, kBodyPx, 460, vc);
            right -= vw + 14;
        }
        if (restart[size_t(r)]) {
            const std::string tag = s.read_only ? "read only" : "next start";
            const int tw = c.TextWidth(tag, kSmallPx);
            c.Text(right - tw, base, tag, kSmallPx, sel ? Rgba{255, 226, 150, 1.0f} : Rgba{214, 178, 98, 1.0f});
            right -= tw + 14;
        }
        if (changed[size_t(r)] && s.kind != Kind::Action) c.FillRect(18, y + 12, 5, kRowH - 24, {214, 178, 98, 1.0f});  // not the default
        FitText(c, 32, base, s.label, kBodyPx, std::max(80, right - 32 - 8), s.kind == Kind::Action && !sel ? Rgba{170, 200, 240, 1.0f} : text);
    }
    if (n > kRows) {
        // Scroll bar: where the visible rows are in the category.
        const int trackY = kListTop + 2, trackH = kRows * kRowH - 4, x = int(kW) - 24;
        c.FillRect(x, trackY, 8, trackH, {58, 64, 80, 1.0f});
        const int thumbH = std::max(24, trackH * kRows / n);
        const int thumbY = trackY + (trackH - thumbH) * g_top / std::max(1, n - kRows);
        c.FillRect(x, thumbY, 8, thumbH, {150, 170, 205, 1.0f});
    }
    const int footY = int(kH) - kFooterH;
    c.FillRect(10, footY, int(kW) - 20, 2, {70, 80, 100, 1.0f});
    std::string help;
    if (showMsg) help = g_msg;
    else if (const Setting* s = SelectedSetting()) help = HelpText(*s);
    else if (g_sel < 0) help = "Left/Right or LB/RB: category    Down: the settings";
    if (perf && !showMsg && g_perfLine.find('\n') != std::string::npos && help.empty()) help = g_perfLine.substr(g_perfLine.find('\n') + 1);
    const int line1 = footY + 8 + c.Ascent(kSmallPx);
    const int hw = c.TextWidth(help, kSmallPx);
    FitText(c, std::max(16, (int(kW) - hw) / 2), line1, help, kSmallPx, int(kW) - 32,
            showMsg ? Rgba{255, 214, 120, 1.0f} : Rgba{214, 200, 160, 1.0f});
    const std::string keys = "D-pad: choose / change    LB/RB: category    A: select    Y: default    X: save    B: close";
    const int kw = c.TextWidth(keys, kSmallPx);
    c.Text((int(kW) - kw) / 2, line1 + 30, keys, kSmallPx, {176, 186, 204, 1.0f});
    {
        std::lock_guard pl(g_pixMutex);
        g_pixels = c.Pixels();
        ++g_pixVersion;
    }
}

// The value a setting would write, for the save's comparison with the ini.
bool SameValue(const std::string& a, const std::string& b) { return live_settings::same_value(a, b); }

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
    auto consider = [&](const std::string& snapId, const std::string& section, const std::string& key, const std::string& value, bool text) {
        if (value.empty() && !text) return;
        const auto cur = ini.get(section, key);
        // A key in the file: written when the value differs. A key the file does not have: only
        // when the value is not the one in effect when the panel first opened (the default).
        const bool differs = cur ? !SameValue(*cur, value) : !SameValue(g_startValues[snapId], value);
        if (differs) updates.push_back({section, key, value});
    };
    for (const Setting& s : g_items) {
        if (s.section.empty() || s.kind == Kind::Action || s.read_only) continue;
        if (s.optional && s.get_is_set) {
            const auto cur = ini.get(s.section, s.key);
            const std::string v = live_settings::ini_value(s);
            if (!s.get_is_set()) {
                if (cur) {
                    ini_file::Update u{s.section, s.key, *cur};  // the line keeps its text, behind a ';'
                    u.comment_out = true;
                    updates.push_back(u);
                }
            } else if (!cur || !SameValue(*cur, v)) {
                updates.push_back({s.section, s.key, v});
            }
            continue;
        }
        consider(s.id, s.section, s.key, live_settings::ini_value(s), s.kind == Kind::Text);
        if (s.extra_ini)
            for (const auto& [k, v] : s.extra_ini()) consider(s.id + "." + k, s.section, k, v, false);
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
    bool restartNeeded = false;
    for (const ini_file::Update& u : updates)
        for (const Setting& s : g_items)
            if (s.restart && Lower(s.section) == Lower(u.section) && Lower(s.key) == Lower(u.key)) restartNeeded = true;
    return std::format("Saved {} setting{} to ff7vr.ini{}", updates.size(), updates.size() == 1 ? "" : "s",
                       restartNeeded ? " (some take effect at the next start)" : "");
}

void OpenPanel(const char* how) {
    std::lock_guard lk(g_mutex);
    if (g_open.load()) return;
    Refresh();
    TakeSnapshot();
    g_msg.clear();
    g_armed.clear();
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

void SetCategory(int c) {
    std::lock_guard lk(g_mutex);
    g_cat = ((c % kNumCategories) + kNumCategories) % kNumCategories;
    BuildRows();
    if (g_sel >= 0) g_sel = 0;
    g_top = 0;
    g_armed.clear();
}

void Move(int d) {
    std::lock_guard lk(g_mutex);
    const int n = int(g_rows.size());
    // -1 is the category row: up from the first setting, down from the last.
    g_sel += d;
    if (g_sel >= n) g_sel = -1;
    if (g_sel < -1) g_sel = n - 1;
    KeepSelectionVisible();
}

// Resets a setting to its built-in default; empty = ok.
std::string ResetSetting(Setting& s) {
    if (s.kind == Kind::Action || s.section.empty() || s.read_only) return {};
    bool unset = s.default_unset;
    const std::string def = live_settings::default_of(s);
    if (s.default_ini.empty()) live_settings::default_value(s.section, s.key, &unset);
    std::string err;
    if (!live_settings::same_value(live_settings::ini_value(s), def) || s.kind == Kind::Text) err = live_settings::set_from_ini(s, def);
    if (s.optional && s.set_is_set) s.set_is_set(!unset);
    return err;
}

void ResetSelected() {
    std::lock_guard lk(g_mutex);
    Setting* s = SelectedSetting();
    if (!s || s->kind == Kind::Action) return;
    if (s->read_only) {
        Message(s->label + " is read only here");
        return;
    }
    const std::string err = ResetSetting(*s);
    log::info("menu: {} reset to its default ({}){}", s->label, live_settings::default_of(*s), err.empty() ? "" : ": " + err);
    Message(err.empty() ? s->label + ": default" : s->label + ": " + err);
    if (err.empty() && s->part_of_profile) ProfileToCustom();
}

std::string ResetMany(const std::string& category) {
    std::lock_guard lk(g_mutex);
    int done = 0, failed = 0;
    // The profile first: resetting it must not undo the settings reset after it.
    for (int pass = 0; pass < 2; ++pass)
        for (Setting& s : g_items) {
            if ((s.id == "graphics_profile") != (pass == 0)) continue;
            if (!category.empty() && s.category != category) continue;
            if (s.kind == Kind::Action || s.section.empty() || s.read_only || live_settings::is_default(s)) continue;
            const std::string err = ResetSetting(s);
            if (err.empty()) ++done;
            else {
                ++failed;
                log::warn("menu: reset {}: {}", s.id, err);
            }
        }
    log::info("menu: reset {} to the defaults: {} changed, {} refused", category.empty() ? std::string("every setting") : category, done, failed);
    return std::format("{} reset to defaults: {} changed{}", category.empty() ? std::string("All settings") : category, done,
                       failed ? std::format(", {} refused", failed) : std::string());
}

void Change(int d) {
    std::lock_guard lk(g_mutex);
    if (g_sel < 0) {
        SetCategory(g_cat + d);
        return;
    }
    Setting* sp = SelectedSetting();
    if (!sp) return;
    Setting& s = *sp;
    if (!Available(s)) {
        Message(s.label + " is not available now");
        return;
    }
    if (s.read_only) {
        Message(s.label + " is read only here");
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
        float v = s.get_number();
        if (!std::isfinite(v)) v = s.min;
        if (s.key_code) {
            const auto& keys = CommonKeys();
            const int cur = int(std::lround(v));
            int next = cur;
            if (d > 0) {
                const auto it = std::upper_bound(keys.begin(), keys.end(), cur);
                next = it == keys.end() ? keys.front() : *it;
            } else {
                auto it = std::lower_bound(keys.begin(), keys.end(), cur);
                next = it == keys.begin() ? keys.back() : *(it - 1);
            }
            v = float(next);
        } else {
            v += float(d) * s.step;
            v = s.min + std::round((v - s.min) / s.step) * s.step;
            v = std::clamp(v, s.min, s.max);
        }
        err = s.set_number(v);
        log::info("menu: {} -> {:.{}f}{}", s.label, v, s.decimals, err.empty() ? "" : " (" + err + ")");
    } else if (s.kind == Kind::Text) {
        Message(s.label + ": edit it in ff7vr.ini (Y resets it to the default)");
        return;
    } else {
        return;
    }
    if (s.optional && s.set_is_set && err.empty()) s.set_is_set(true);
    if (!err.empty()) Message(s.label + ": " + err);
    else if (s.part_of_profile) ProfileToCustom();
    else if (s.restart) Message(s.label + ": takes effect at the next start (save with X)");
}

void Select() {
    std::lock_guard lk(g_mutex);
    if (g_sel < 0) {
        g_sel = 0;
        return;
    }
    Setting* sp = SelectedSetting();
    if (!sp) return;
    Setting& s = *sp;
    if (s.kind == Kind::Action) {
        if (s.id == "reset_category" || s.id == "reset_all") {
            // A confirm step: the first select arms it for 4 s.
            if (g_armed != s.id) {
                g_armed = s.id;
                g_armedUntil = NowMs() + 4000;
                Message(s.id == "reset_all" ? "Select again to reset EVERY setting to its default"
                                            : std::string("Select again to reset ") + kCategories[g_cat] + " to the defaults");
                return;
            }
            g_armed.clear();
            Message(ResetMany(s.id == "reset_all" ? std::string() : std::string(kCategories[g_cat])));
            return;
        }
        if (s.run) {
            const std::string r = s.run();
            log::info("menu: {}: {}", s.label, r.empty() ? "done" : r);
            if (!r.empty()) Message(r);
        }
    } else if (s.optional && s.set_is_set && s.get_is_set && !s.read_only) {
        const bool now = !s.get_is_set();
        s.set_is_set(now);
        log::info("menu: {} {}", s.label, now ? "set" : "not set");
        Message(s.label + (now ? ": set" : ": not set") + " (next start; save with X)");
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
        EnsureKeyboardHook();
        const int64_t t = NowMs();
        const bool focus = Focus();
        if (const int64_t at = g_replaceAtMs.load(); at && t >= at) {
            g_replaceAtMs = 0;
            g_placement.fetch_add(1);
        }
        if (edge(g_cfg.key.load(), focus)) {
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
        const bool kEnter = edge(VK_RETURN, focus), kS = edge('S', focus), kBack = edge(VK_BACK, focus);
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
        if (pressed & kLB) SetCategory(g_cat - 1);
        if (pressed & kRB) SetCategory(g_cat + 1);
        if ((pressed & kA) || kEnter) Select();
        if (((pressed & kY) && !(pad & kView)) || kBack) ResetSelected();
        if ((pressed & kX) || kS) Message(Save());
        if ((pressed & kB) || ((pressed & kY) && (pad & kView))) ClosePanel("gamepad");
        if (g_open.load()) {
            if (std::string(kCategories[g_cat]) == "Performance") UpdatePerformance();
            Draw(false);  // only publishes a new image when something shown changed
        }
    }
}

// ---- the render module's settings, and every other ini key ----
constexpr int kPicture = 100, kComfort = 200, kViewCat = 300, kHud = 400, kPerformance = 500, kControls = 600, kAdvanced = 700;

const char* CategoryOf(int order) {
    return order < kComfort ? "Picture"
           : order < kViewCat ? "Comfort"
           : order < kHud ? "View"
           : order < kPerformance ? "HUD"
           : order < kControls ? "Performance"
           : order < kAdvanced ? "Controls"
                               : "Advanced";
}

Setting Base(const char* id, const char* label, int order, const char* section, const char* key, Kind kind) {
    Setting s;
    s.id = id;
    s.label = label;
    s.order = order;
    s.category = order > 0 ? CategoryOf(order) : "*";
    s.kind = kind;
    s.section = section;
    s.key = key;
    return s;
}

Setting Number(const char* id, const char* label, int order, const char* section, const char* key, float mn, float mx, float step, int dec,
               std::function<float()> get = {}, std::function<std::string(float)> set = {}) {
    Setting s = Base(id, label, order, section, key, Kind::Number);
    s.min = mn;
    s.max = mx;
    s.step = step;
    s.decimals = dec;
    s.get_number = std::move(get);
    s.set_number = std::move(set);
    return s;
}

Setting Choice(const char* id, const char* label, int order, const char* section, const char* key, std::vector<std::string> choices,
               std::vector<std::string> ini) {
    Setting s = Base(id, label, order, section, key, Kind::Choice);
    s.choices = std::move(choices);
    s.ini_values = std::move(ini);
    return s;
}

Setting OnOff(const char* id, const char* label, int order, const char* section, const char* key) {
    return Choice(id, label, order, section, key, {"Off", "On"}, {"0", "1"});
}

Setting Text(const char* id, const char* label, int order, const char* section, const char* key) {
    return Base(id, label, order, section, key, Kind::Text);
}

std::string OkOrError(const std::string& reply) { return reply.starts_with("ok") ? std::string() : reply; }

std::string Dispatch(const std::string& line) {
    std::string reply;
    if (!dev_commands::dispatch(line, reply)) return "err no handler for " + line;
    return reply;
}

// A setting that keeps its value as read at start; `live` applies a change now, without
// it the value takes effect at the next start.
const Config* g_startCfg = nullptr;
void Held(Setting s, std::function<std::string(const std::string&)> live = {}) {
    auto start = g_startCfg ? g_startCfg->get(s.section, s.key) : std::nullopt;
    live_settings::add(live_settings::held(std::move(s), std::move(start), std::move(live)));
}

// A line of [stereo_cvars] (or another key) that may be absent: "not set" writes it
// commented out. Applied at the next start.
void Optional(Setting s) {
    bool unset = false;
    live_settings::default_value(s.section, s.key, &unset);
    const auto start = g_startCfg ? g_startCfg->get(s.section, s.key) : std::nullopt;
    auto isSet = std::make_shared<std::atomic<bool>>(start.has_value());
    s.optional = true;
    s.default_unset = unset;
    s.get_is_set = [isSet] { return isSet->load(); };
    s.set_is_set = [isSet](bool v) { *isSet = v; };
    live_settings::add(live_settings::held(std::move(s), start, {}));
}

void ReadOnly(Setting s) {
    s.read_only = true;
    Held(std::move(s));
}

// Whether a module already registered a setting for this key.
bool Registered(const char* section, const char* key) {
    for (const Setting& s : live_settings::all())
        if (Lower(s.section) == Lower(section) && Lower(s.key) == Lower(key)) return true;
    return false;
}

void RegisterSettings(const Config& cfg) {
    g_startCfg = &cfg;
    XrController& xc = XrController::Get();

    // ---- Picture ----
    auto pic = [&xc](const char* id, const char* label, int order, const char* key, float mn, float mx, float xr::PictureAdjust::*field) {
        live_settings::add(Number(
            id, label, order, "picture", key, mn, mx, 0.05f, 2, [&xc, field] { return xc.Picture().*field; },
            [&xc, key](float v) { return OkOrError(xc.PictureCommand(std::format("{} {:.3f}", key, v))); }));
    };
    pic("brightness", "Brightness", kPicture + 10, "brightness", -1.0f, 1.0f, &xr::PictureAdjust::brightness);
    pic("contrast", "Contrast", kPicture + 11, "contrast", 0.5f, 2.0f, &xr::PictureAdjust::contrast);
    pic("saturation", "Saturation", kPicture + 12, "saturation", 0.0f, 2.0f, &xr::PictureAdjust::saturation);
    pic("gamma", "Gamma", kPicture + 13, "gamma", 0.5f, 2.0f, &xr::PictureAdjust::gamma);
    live_settings::add(Number(
        "picture.black_level", "Black level", kPicture + 14, "picture", "black_level", -0.1f, 0.1f, 0.01f, 2,
        [&xc] { return xc.Picture().blackLevel; }, [&xc](float v) { return OkOrError(xc.PictureCommand(std::format("black_level {:.3f}", v))); }));
    live_settings::add(Number("sharpen", "Sharpen", kPicture + 20, "picture", "sharpen", 0.0f, 1.0f, 0.1f, 1, [] { return comfort::Sharpen(); },
                              [](float v) { return OkOrError(Dispatch(std::format("sharpen {:.2f}", v))); }));
    Optional(Number("cvar.depth_of_field", "Depth of field (r.DepthOfFieldQuality)", kPicture + 80, "stereo_cvars", "r.DepthOfFieldQuality", 0, 4, 1, 0));

    // ---- Comfort ----
    live_settings::add(Number("vignette", "Comfort vignette", kComfort + 20, "comfort", "vignette", 0.0f, 1.0f, 0.1f, 1,
                              [] { return comfort::VignetteStrength(); },
                              [](float v) { return OkOrError(comfort::Command(std::format("vignette {:.2f}", v))); }));
    Held(Number("comfort.vignette_radius", "Vignette clear centre", kComfort + 21, "comfort", "vignette_radius", 0.1f, 1.5f, 0.05f, 2),
         [](const std::string& v) { return OkOrError(comfort::Command("radius " + v)); });
    Held(Number("comfort.vignette_softness", "Vignette fade width", kComfort + 22, "comfort", "vignette_softness", 0.05f, 1.0f, 0.05f, 2),
         [](const std::string& v) { return OkOrError(comfort::Command("softness " + v)); });
    if (!Registered("comfort", "snap_left_key")) {
        Setting l = Number("comfort.snap_left_key", "Snap left key", kComfort + 13, "comfort", "snap_left_key", 0, 254, 1, 0);
        l.key_code = true;
        Held(std::move(l));
        Setting r = Number("comfort.snap_right_key", "Snap right key", kComfort + 14, "comfort", "snap_right_key", 0, 254, 1, 0);
        r.key_code = true;
        Held(std::move(r));
    }
    Held(OnOff("xr.pause_on_remove", "Pause when the headset comes off", kComfort + 70, "xr", "pause_on_remove"),
         [](const std::string& v) { return OkOrError(Dispatch(std::string("xr-pause ") + (v == "1" ? "on" : "off"))); });
    Held(OnOff("stereo.comfort_cvars", "No motion blur / chromatic aberration", kComfort + 71, "stereo", "comfort_cvars"));

    // ---- View ----
    {
        Setting s = Base("recenter", "Recenter", kViewCat + 1, "", "", Kind::Action);
        s.run = [&xc] {
            const std::string r = xc.Recenter();
            if (!r.starts_with("ok")) return std::string("Recenter: ") + r;
            g_replaceAtMs = NowMs() + 200;  // then the panel moves in front again, in the new origin
            return std::string("Recentered");
        };
        live_settings::add(std::move(s));
    }
    Held(OnOff("first_person.enabled", "First person can be used", kViewCat + 9, "first_person", "enabled"));
    Held(OnOff("stereo.enabled", "3D at start", kViewCat + 41, "stereo", "enabled"));
    Held(OnOff("stereo.start_in_stereo", "3D from the start", kViewCat + 42, "stereo", "start_in_stereo"));

    // ---- HUD ----
    live_settings::add(Number(
        "hud_distance", "HUD distance (m)", kHud + 10, "ui", "distance", 0.75f, 8.0f, 0.25f, 2,
        [&xc] {
            float d = 0;
            xc.UiPlacement(&d, nullptr, nullptr);
            return d;
        },
        [&xc](float v) { return OkOrError(xc.UiCommand(std::format("distance {:.2f}", v))); }));
    live_settings::add(Number(
        "hud_size", "HUD size (m)", kHud + 11, "ui", "size", 0.5f, 4.0f, 0.1f, 2,
        [&xc] {
            float s = 0;
            xc.UiPlacement(nullptr, &s, nullptr);
            return s;
        },
        [&xc](float v) { return OkOrError(xc.UiCommand(std::format("size {:.2f}", v))); }));
    Held(Number("ui.battle_size", "HUD size in battles (m)", kHud + 12, "ui", "battle_size", 0.0f, 4.0f, 0.01f, 2),
         [](const std::string& v) { return OkOrError(Dispatch("ui battle_size " + v)); });
    Held(OnOff("ui.follow_head", "HUD follows your head", kHud + 13, "ui", "follow_head"),
         [&xc](const std::string& v) { return OkOrError(xc.UiCommand("follow " + v)); });
    Held(OnOff("ui.align", "Markers aligned with the world", kHud + 15, "ui", "align"),
         [&xc](const std::string& v) { return OkOrError(xc.UiCommand("align " + v)); });
    Held(OnOff("ui.layer", "HUD on its own panel", kHud + 14, "ui", "layer"));
    Held(Number("screen.distance", "Virtual screen distance (m)", kHud + 20, "screen", "distance", 0.5f, 10.0f, 0.25f, 2));
    Held(Number("screen.width", "Virtual screen width (m)", kHud + 21, "screen", "width", 0.5f, 10.0f, 0.1f, 2));
    live_settings::add(Number("menu.distance", "Settings panel distance (m)", kHud + 30, "menu", "distance", 0.5f, 3.0f, 0.1f, 2,
                              [] { return g_cfg.distance.load(); },
                              [](float v) {
                                  g_cfg.distance = std::clamp(v, 0.3f, 10.0f);
                                  g_placement.fetch_add(1);
                                  return std::string();
                              }));
    live_settings::add(Number("menu.size", "Settings panel width (m)", kHud + 31, "menu", "size", 0.4f, 2.0f, 0.05f, 2, [] { return g_cfg.width.load(); },
                              [](float v) {
                                  g_cfg.width = std::clamp(v, 0.1f, 5.0f);
                                  return std::string();
                              }));

    // ---- Performance ----
    {
        Setting s = Choice("foveation", "Foveated rendering", kPerformance + 30, "foveation", "preset", {"Off", "Quality", "Balanced", "Performance"},
                           {"off", "quality", "balanced", "performance"});
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
        // [foveation] enabled is written with it.
        s.extra_ini = [] {
            return std::vector<std::pair<std::string, std::string>>{{"enabled", foveation::PresetName() == "off" ? "0" : "1"}};
        };
        live_settings::add(std::move(s));
    }
    Held(Choice("foveation.eye_tracking", "Eye-tracked foveation", kPerformance + 35, "foveation", "eye_tracking", {"Off", "On", "Auto"}, {"0", "1", "auto"}));
    Held(Number("foveation.gaze_margin_deg", "Eye tracking margin (degrees)", kPerformance + 36, "foveation", "gaze_margin_deg", 0, 20, 1, 0),
         [](const std::string& v) { return OkOrError(foveation::Command("gaze margin " + v)); });
    Held(Number("foveation.gaze_smoothing", "Eye tracking smoothing", kPerformance + 37, "foveation", "gaze_smoothing", 0.0f, 0.95f, 0.05f, 2),
         [](const std::string& v) { return OkOrError(foveation::Command("gaze smoothing " + v)); });
    Held(OnOff("foveation.subsurface_full_rate", "Skin edges at full detail", kPerformance + 38, "foveation", "subsurface_full_rate"),
         [](const std::string& v) { return OkOrError(foveation::Command("subsurface " + v)); });
    Held(OnOff("foveation.translucency_full_rate", "Smoke and glass at full detail", kPerformance + 39, "foveation", "translucency_full_rate"),
         [](const std::string& v) { return OkOrError(foveation::Command("translucency " + v)); });
    Held(OnOff("foveation.dlss_finer", "Foveation one step finer with DLSS", kPerformance + 40, "foveation", "dlss_finer"),
         [](const std::string& v) { return OkOrError(foveation::Command("dlss_finer " + v)); });
    Held(Number("xr.resolution_scale", "Headset resolution scale", kPerformance + 50, "xr", "resolution_scale", 0.5f, 1.5f, 0.05f, 2));
    Held(OnOff("xr.depth_layer", "Depth for the headset's reprojection", kPerformance + 55, "xr", "depth_layer"),
         [](const std::string& v) { return OkOrError(Dispatch(std::string("xr-depth ") + (v == "1" ? "on" : "off"))); });
    if (!Registered("log", "frame_budget"))  // a module with a live path registers its own
        Held(OnOff("log.frame_budget", "Frame budget warning in the log", kPerformance + 90, "log", "frame_budget"));
    Held(OnOff("log.upload_check", "Slow-transfer check in the log", kPerformance + 91, "log", "upload_check"));
    Held(OnOff("stereo.light_volume_pool", "Reuse the light volume buffers", kPerformance + 92, "stereo", "light_volume_pool"));
    Held(Choice("dlss.mode", "DLSS mode", kPerformance + 71, "dlss", "mode", {"Upscale", "DLAA"}, {"upscale", "dlaa"}));
    Held(Number("dlss.input_scale", "DLSS input scale", kPerformance + 72, "dlss", "input_scale", 0.33f, 1.0f, 0.01f, 2));
    Held(Choice("dlss.output", "DLSS output", kPerformance + 73, "dlss", "output", {"Runtime size", "Engine size"}, {"runtime", "engine"}));
    Held(Choice("dlss.texture_bias", "DLSS texture sharpening", kPerformance + 75, "dlss", "texture_bias", {"Off", "Auto", "Auto - 1"}, {"off", "auto", "auto-1"}));
    Held(OnOff("dlss.firefly", "DLSS firefly swatter", kPerformance + 76, "dlss", "firefly"));
    Optional(Number("cvar.pool_size", "Texture pool (MB)", kPerformance + 80, "stereo_cvars", "r.Streaming.PoolSize", 1000, 8000, 250, 0));
    Optional(Number("cvar.streaming_boost", "Texture streaming boost", kPerformance + 81, "stereo_cvars", "r.Streaming.Boost", 0.5f, 4.0f, 0.5f, 1));
    Optional(Number("cvar.static_lod", "Static mesh detail distance", kPerformance + 82, "stereo_cvars", "r.StaticMeshLODDistanceScale", 0.25f, 2.0f, 0.05f, 2));
    Optional(Number("cvar.skeletal_lod", "Character detail bias", kPerformance + 83, "stereo_cvars", "r.SkeletalMeshLODBias", -2, 2, 1, 0));
    Optional(Number("cvar.foliage_lod", "Foliage detail distance", kPerformance + 84, "stereo_cvars", "foliage.LODDistanceScale", 0.5f, 4.0f, 0.25f, 2));
    Optional(Number("cvar.view_distance", "Small object view distance", kPerformance + 85, "stereo_cvars", "r.ViewDistanceScale", 0.5f, 3.0f, 0.25f, 2));
    Optional(Number("cvar.bloom", "Bloom quality (r.BloomQuality)", kPerformance + 86, "stereo_cvars", "r.BloomQuality", 0, 5, 1, 0));

    // ---- Controls ----
    {
        Setting s = Number("menu.key", "Settings panel key", kControls + 40, "menu", "key", 0, 254, 1, 0, [] { return float(g_cfg.key.load()); },
                           [](float v) {
                               g_cfg.key = int(std::lround(std::clamp(v, 0.0f, 254.0f)));
                               return std::string();
                           });
        s.key_code = true;
        live_settings::add(std::move(s));
    }
    {
        Setting s = OnOff("menu.pad", "View/Back + Y opens this panel", kControls + 41, "menu", "pad");
        s.get_choice = [] { return g_cfg.pad.load() ? 1 : 0; };
        s.set_choice = [](int i) {
            g_cfg.pad = i == 1;
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    Held(Choice("controls.pad_source", "Gamepads read", kControls + 35, "controls", "pad_source", {"Both kinds", "XInput only", "PlayStation only"},
                {"auto", "xinput", "playstation"}));
    {
        Setting s = Number("xr.pause_key", "Key sent to pause", kControls + 50, "xr", "pause_key", 0, 254, 1, 0);
        s.key_code = true;
        Held(std::move(s), [](const std::string& v) { return OkOrError(Dispatch("xr-pause key " + v)); });
    }
    {
        Setting up = Number("controls.brightness_up_key", "Brightness up key", kControls + 60, "controls", "brightness_up_key", 0, 254, 1, 0);
        up.key_code = true;
        Held(std::move(up));
        Setting down = Number("controls.brightness_down_key", "Brightness down key", kControls + 61, "controls", "brightness_down_key", 0, 254, 1, 0);
        down.key_code = true;
        Held(std::move(down));
        Held(Number("controls.brightness_step", "Brightness step per press", kControls + 62, "controls", "brightness_step", 0.01f, 0.5f, 0.01f, 2));
    }

    // ---- Advanced ----
    Held(Choice("xr.backend", "XR backend", kAdvanced + 20, "xr", "backend", {"OpenXR (headset)", "Null (testing)"}, {"openxr", "null"}));
    Held(Text("xr.runtime", "OpenXR runtime", kAdvanced + 21, "xr", "runtime"));
    Held(Number("xr.retry_interval", "Headset retry interval (s)", kAdvanced + 22, "xr", "retry_interval", 1, 60, 1, 0));
    Held(Number("xr.depth_far_m", "Depth far distance (m, 0 = infinite)", kAdvanced + 23, "xr", "depth_far_m", 0, 1000000, 10000, 0));
    Held(Text("stereo.vr_window", "Window size in 3D", kAdvanced + 30, "stereo", "vr_window"));
    Held(OnOff("stereo.allow_unknown_build", "Allow an unknown game build", kAdvanced + 31, "stereo", "allow_unknown_build"));
    Optional(Number("cvar.disable_distortion", "No heat haze at all (r.DisableDistortion)", kAdvanced + 35, "stereo_cvars", "r.DisableDistortion", 0, 1, 1, 0));
    Held(OnOff("loader.force_d3d11", "Add -d3d11 to the command line", kAdvanced + 40, "loader", "force_d3d11"));
    Held(Choice("log.level", "Log level", kAdvanced + 41, "log", "level", {"Error", "Warn", "Info", "Debug", "Trace"}, {"error", "warn", "info", "debug", "trace"}),
         [](const std::string& v) {
             log::set_level(log::parse_level(v, log::Level::Info));
             return std::string();
         });
    Held(Number("log.keep_sessions", "Earlier sessions' logs kept", kAdvanced + 42, "log", "keep_sessions", 0, 50, 1, 0));
    Held(OnOff("crash.enabled", "Crash reports", kAdvanced + 43, "crash", "enabled"));
    Held(OnOff("crash.first_chance", "Crash reports: first chance", kAdvanced + 44, "crash", "first_chance"));
    Optional(OnOff("crash.full_memory", "Crash dump with the whole memory", kAdvanced + 45, "crash", "full_memory"));
    ReadOnly(OnOff("dev.pipe", "Developer command pipe", kAdvanced + 50, "dev", "pipe"));
    ReadOnly(OnOff("dev.virtual_pad", "Developer test input", kAdvanced + 51, "dev", "virtual_pad"));
    ReadOnly(OnOff("dev.unsafe_commands", "Developer unsafe commands", kAdvanced + 52, "dev", "unsafe_commands"));

    // ---- actions at the end of every category ----
    {
        Setting s = Base("reset_category", "Reset this category to defaults", 0, "", "", Kind::Action);
        s.order = 900;
        live_settings::add(std::move(s));
    }
    {
        Setting s = Base("reset_all", "Reset all settings to defaults", 0, "", "", Kind::Action);
        s.order = 901;
        live_settings::add(std::move(s));
    }
    {
        Setting s = Base("save", "Save settings to ff7vr.ini", 0, "", "", Kind::Action);
        s.order = 902;
        s.run = [] { return Save(); };
        live_settings::add(std::move(s));
    }
    {
        Setting s = Base("close", "Close", 0, "", "", Kind::Action);
        s.order = 903;
        s.run = [] {
            ClosePanel("Close");
            return std::string();
        };
        live_settings::add(std::move(s));
    }
    g_startCfg = nullptr;
}

std::string StatusText() {
    std::lock_guard lk(g_mutex);
    std::string items;
    for (int r = 0; r < int(g_rows.size()); ++r) {
        const Setting& s = g_items[size_t(g_rows[size_t(r)])];
        if (s.kind == Kind::Action) continue;
        items += std::format("{}{}{}={}{}", items.empty() ? "" : ", ", r == g_sel ? "*" : "", s.id, ValueText(s, false), s.restart ? " (next start)" : "");
    }
    const Setting* sel = SelectedSetting();
    return std::format("ok menu {}; category {} ({}); selected {} '{}'; frames shown {} images uploaded {}; pad: polls {} withheld from the game {} "
                       "opened {} last raw {:#06x}; keyboard: hook {} on thread {}, keys kept from the game {}; key {} pad {}; {:.2f} m wide at {:.2f} m; "
                       "ini {}; {}",
                       g_open.load() ? "open" : "closed", kCategories[g_cat], g_rows.size(), g_sel, sel ? sel->label : std::string("(category row)"),
                       g_frames.load(), g_uploads.load(), g_padPolls.load(), g_padWithheld.load(), g_padOpens.load(), g_padLastRaw.load(),
                       g_kbHook ? "on" : "off", g_kbThread.load(), g_kbSwallowed.load(), g_cfg.key.load(), g_cfg.pad.load() ? 1 : 0, g_cfg.width.load(),
                       g_cfg.distance.load(), log::narrow(g_iniPath.wstring()), items);
}

// `menu list`: every setting with its category, value, default and whether it is live (one
// line each in ff7vr.log, joined with " || " in the reply).
std::string ListText() {
    std::lock_guard lk(g_mutex);
    std::string out = std::format("ok {} settings", g_items.size());
    for (const Setting& s : g_items) {
        if (s.kind == Kind::Action) continue;
        const std::string line = std::format("{} | {} | [{}] {} | {} | default {} | {}", s.category, s.id, s.section, s.key, ValueText(s, false),
                                             live_settings::default_of(s), s.read_only ? "read only" : s.restart ? "next start" : "live");
        log::info("menu list: {}", line);
        out += " || " + line;
    }
    return out;
}

Setting* FindSetting(const std::string& idOrKey) {
    const std::string want = Lower(idOrKey);
    for (Setting& s : g_items)
        if (Lower(s.id) == want || (!s.section.empty() && Lower(s.section + "." + s.key) == want)) return &s;
    return nullptr;
}

}  // namespace

void Start(const Config& c, const std::filesystem::path& iniPath) {
    if (g_started.exchange(true)) return;
    g_cfg.key = static_cast<int>(std::clamp<long long>(c.get_int("menu", "key", g_cfg.key.load()), 0, 255));
    g_cfg.pad = c.get_bool("menu", "pad", g_cfg.pad.load());
    g_cfg.distance = static_cast<float>(std::clamp(c.get_float("menu", "distance", g_cfg.distance.load()), 0.3, 10.0));
    g_cfg.width = static_cast<float>(std::clamp(c.get_float("menu", "size", g_cfg.width.load()), 0.1, 5.0));
    g_iniPath = iniPath;
    RegisterSettings(c);
    {
        std::lock_guard lk(g_mutex);
        Refresh();
    }
    dev_commands::add("menu",
                      "menu status | open | close | toggle | up | down | left | right | select | reset | back | save | cat <name|next|prev> | list | "
                      "set <id> <value> | default <id> | dump <png path>: the in-headset settings panel",
                      [](std::string_view args) { return Command(std::string(args)); });
    std::thread(ThreadMain).detach();
    log::info("menu: settings panel on key {} and {}; {:.2f} m wide at {:.2f} m; {} settings in {} categories; saves to {}", g_cfg.key.load(),
              g_cfg.pad.load() ? "View/Back + Y" : "no gamepad combination", g_cfg.width.load(), g_cfg.distance.load(), g_items.size(), kNumCategories,
              log::narrow(g_iniPath.wstring()));
}

void Stop() {
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
}

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

bool Frame(ID3D11Device* device, ID3D11DeviceContext* ctx, bool forceContent, PanelFrame* out) {
    if (!g_open.load(std::memory_order_acquire) || !device || !ctx) return false;
    {
        std::lock_guard lk(g_devMutex);
        if (g_vramDevice.Get() != device) g_vramDevice = device;
    }
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
    out->widthM = g_cfg.width.load();
    out->heightM = out->widthM * float(kH) / float(kW);
    out->distance = g_cfg.distance.load();
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
    if (verb == "list") {
        {
            std::lock_guard lk(g_mutex);
            if (g_items.empty()) Refresh();
        }
        return ListText();
    }
    if (verb == "focus") {
        if (rest != "any" && rest != "window") return "err usage: menu focus any|window";
        g_anyFocus = rest == "any";
        return std::string("ok menu input ") + (g_anyFocus.load() ? "also without the window focus (test)" : "only while the game window has the focus");
    }
    if (verb == "pad") {
        // Test without a pad: one XInput state through the panel's filter, as the game's poll
        // would pass it (buttons in hex, optional slot 0..7, 4..7 = PlayStation pads); replies
        // with what the game gets.
        std::istringstream in(rest);
        std::string hex;
        unsigned long user = 0;
        in >> hex >> user;
        if (hex.empty() || user >= kPadSlots) return "err usage: menu pad <hex buttons> [slot 0..7]";
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
        if (std::string(kCategories[g_cat]) == "Performance") {
            g_perfNextMs = 0;
            UpdatePerformance();
        }
        Draw(false);
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
    if (verb == "set" || verb == "default") {
        // menu set <id or section.key> <ini value> | menu default <id or section.key>: as the
        // panel's own change and reset (works with the panel closed too).
        std::lock_guard lk(g_mutex);
        if (g_items.empty()) Refresh();
        const size_t sp2 = rest.find(' ');
        const std::string id = rest.substr(0, sp2);
        Setting* s = FindSetting(id);
        if (!s) return "err no setting " + id;
        std::string err;
        if (verb == "default") {
            err = ResetSetting(*s);
        } else {
            if (sp2 == std::string::npos) return "err usage: menu set <id> <value> (unset: not set)";
            const std::string value = rest.substr(sp2 + 1);
            if (s->read_only) return "err " + s->id + " is read only";
            if (s->optional && s->set_is_set && Lower(value) == "unset") {
                s->set_is_set(false);
            } else {
                err = live_settings::set_from_ini(*s, value);
                if (err.empty() && s->optional && s->set_is_set) s->set_is_set(true);
            }
        }
        Draw(false);
        if (!err.empty()) return "err " + err;
        return std::format("ok {} = {}{}", s->id, ValueText(*s, false), s->restart ? " (takes effect at the next start)" : "");
    }
    if (verb == "cat") {
        std::lock_guard lk(g_mutex);
        const std::string want = Lower(rest);
        int c = want == "next" ? g_cat + 1 : want == "prev" ? g_cat - 1 : -100;
        for (int i = 0; i < kNumCategories && c == -100; ++i)
            if (Lower(kCategories[i]) == want) c = i;
        if (c == -100) return "err usage: menu cat <Picture|Comfort|View|HUD|Performance|Controls|Advanced|next|prev>";
        SetCategory(c);
        Draw(false);
        return StatusText();
    }
    if (!g_open.load()) return "err the panel is closed (menu open first)";
    if (verb == "up") Move(-1);
    else if (verb == "down") Move(+1);
    else if (verb == "left") Change(-1);
    else if (verb == "right") Change(+1);
    else if (verb == "select") Select();
    else if (verb == "reset") ResetSelected();
    else if (verb == "save") Message(Save());
    else return "err usage: menu status | open | close | toggle | up | down | left | right | select | reset | save | cat <name> | list | set | default | dump";
    Draw(false);
    return StatusText();
}

}  // namespace menu

// The game's pad polls (any thread): XInput users 0..3 from the XInput proxy, PlayStation
// pads 4..7 from the libScePad hook. Records the real pad for the panel, opens it on
// View/Back + Y, and withholds the pad from the game while it is open and until every button
// that was down is released after it closed.
bool MenuFilterPad(unsigned long user, PadState* s) {
    using namespace menu;
    if (!g_started.load(std::memory_order_relaxed) || user >= kPadSlots || !s) return false;
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
        if (!g_open.load() && g_cfg.pad.load() && (s->buttons & kView) && (pressed & kY) && Focus()) openNow = true;
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
