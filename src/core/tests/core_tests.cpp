// Self-tests for ff7vr_core. Not deployed to the game.
//
//   ff7vr_core_tests.exe                 run unit tests (exit code 0 = pass)
//   ff7vr_core_tests.exe bench <exe>     map <exe> as an image and time 40 scans over it
//   ff7vr_core_tests.exe crash           install the crash handler and crash on purpose;
//                                        check ff7vr_core_tests.log and the .dmp next to the exe
//   ff7vr_core_tests.exe hook            inline + vtable hook round trip

#include "ff7vr/core/config.h"
#include "ff7vr/core/crash.h"
#include "ff7vr/core/graphics_profile.h"
#include "ff7vr/core/hook.h"
#include "ff7vr/core/ini_file.h"
#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"
#include "ff7vr/core/pattern.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ff7vr;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void test_graphics_profile() {
    ff7vr::Config c;
    c.load_from_string("[graphics]\nprofile = Balanced\n[foveation]\npreset = quality\n[stereo_cvars]\nr.shadow.maxcsmresolution = 4096\n");
    auto log = ff7vr::graphics_profile::apply(c);
    CHECK(log.size() == 2);
    CHECK(c.get_string("foveation", "preset", "") == "quality");                      // explicit key wins
    CHECK(c.get_string("stereo_cvars", "r.Shadow.MaxCSMResolution", "") == "4096");    // case-insensitive match
    CHECK(c.get_string("stereo_cvars", "r.Shadow.CSM.MaxCascades", "") == "3");         // filled by the profile
    CHECK(c.get_string("stereo", "render_scale", "") == "1.0");
    ff7vr::Config d;
    d.load_from_string("[graphics]\nprofile = custom\n");
    ff7vr::graphics_profile::apply(d);
    CHECK(!d.has("foveation", "preset"));
    ff7vr::Config e;
    e.load_from_string("[graphics]\nprofile = performance\n");
    ff7vr::graphics_profile::apply(e);
    CHECK(e.get_string("stereo_cvars", "r.VolumetricFog", "") == "0");
    CHECK(e.get_string("stereo", "render_scale", "") == "0.9");
    CHECK(ff7vr::graphics_profile::lines("nonsense").empty());
}

static void test_pattern() {
    auto p = pattern::Pattern::parse("48 8B ?? 05 ? C3");
    CHECK(p.has_value());
    CHECK(p->size() == 6);
    CHECK(p->mask[2] == 0 && p->mask[4] == 0 && p->mask[5] == 1);
    CHECK(!pattern::Pattern::parse("?? ??").has_value());
    CHECK(!pattern::Pattern::parse("4G").has_value());
    CHECK(!pattern::Pattern::parse("4").has_value());

    std::vector<std::uint8_t> buf(1 << 20, 0x90);
    const std::uint8_t sig[] = {0x48, 0x8B, 0x11, 0x05, 0x22, 0xC3};
    std::memcpy(buf.data() + 1000, sig, sizeof(sig));
    std::memcpy(buf.data() + buf.size() - sizeof(sig), sig, sizeof(sig));  // at the very end
    auto r = pattern::scan(buf, *p);
    CHECK(r.matches.size() == 2);
    CHECK(r.first() == reinterpret_cast<std::uintptr_t>(buf.data() + 1000));
    CHECK(r.matches.size() == 2 && r.matches[1] == reinterpret_cast<std::uintptr_t>(buf.data() + buf.size() - 6));

    // Short anchor path (single fixed byte runs).
    auto p2 = pattern::Pattern::parse("48 ? 11 ? 22");
    CHECK(pattern::scan(buf, *p2).matches.size() == 2);

    // RIP resolution: mov rax,[rip+0x10] at offset 0 -> target = insn + 7 + 0x10.
    std::uint8_t insn[7] = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00};
    auto a = reinterpret_cast<std::uintptr_t>(insn);
    CHECK(pattern::rip(a, 3, 7) == a + 7 + 0x10);
    std::uint8_t call[5] = {0xE8, 0xFB, 0xFF, 0xFF, 0xFF};  // call -5 -> itself
    auto c = reinterpret_cast<std::uintptr_t>(call);
    CHECK(pattern::rip(c, 1, 5) == c);

    // Module scan on ourselves: find a string literal.
    auto s = pattern::find_string(module::main_module().base, "ff7vr-unique-test-string-42", false);
    CHECK(s.found());
    std::printf("pattern: ok\n");
}

static const char* volatile g_keep = "ff7vr-unique-test-string-42";

static void test_config() {
    Config cfg;
    cfg.load_from_string("\xEF\xBB\xBFtop=1\n[Log]\nLevel = debug ; comment\n[xr]\nwidth=0x10\nscale=1.5\nname=\"a ; b\"\nenabled=on\nempty =   ; nothing\n");
    CHECK(cfg.get_int("", "top", 0) == 1);
    CHECK(cfg.get_string("log", "level", "") == "debug");
    CHECK(cfg.get_int("XR", "WIDTH", 0) == 16);
    CHECK(cfg.get_float("xr", "scale", 0) == 1.5);
    CHECK(cfg.get_string("xr", "name", "") == "a ; b");
    CHECK(cfg.get_bool("xr", "enabled", false));
    CHECK(cfg.has("xr", "empty") && cfg.get_string("xr", "empty", "x").empty());
    CHECK(cfg.get_int("xr", "missing", 7) == 7);
    Config missing;
    CHECK(!missing.load(L"Z:\\does\\not\\exist.ini"));
    CHECK(missing.get_bool("a", "b", true));
    std::printf("config: ok\n");
}


static void test_ini_file() {
    using ff7vr::ini_file::Update;
    const std::string src =
        "; header comment\r\n"
        "[comfort]\r\n"
        "vignette = 0.0               ; strength (0 = off)\r\n"
        "snap_turn = 0                ; degrees\r\n"
        "\r\n"
        "[Picture]\r\n"
        "contrast=1.2\r\n"
        "sharpen =   ; empty\r\n"
        "\r\n"
        "[ui]\r\n"
        "distance = 3.0\r\n";
    ff7vr::ini_file::Report r;
    const std::string out = ff7vr::ini_file::apply(
        src,
        {{"comfort", "vignette", "0.4"}, {"picture", "CONTRAST", "1.25"}, {"picture", "sharpen", "0.3"}, {"comfort", "snap_turn", "0"},
         {"ui", "size", "1.5"}, {"menu", "key", "46"}, {"comfort", "verylongkeyvalue", "x"}},
        &r);
    const std::string want =
        "; header comment\r\n"
        "[comfort]\r\n"
        "vignette = 0.4               ; strength (0 = off)\r\n"
        "snap_turn = 0                ; degrees\r\n"
        "verylongkeyvalue = x\r\n"
        "\r\n"
        "[Picture]\r\n"
        "contrast=1.25\r\n"
        "sharpen = 0.3 ; empty\r\n"
        "\r\n"
        "[ui]\r\n"
        "distance = 3.0\r\n"
        "size = 1.5\r\n"
        "\r\n"
        "[menu]\r\n"
        "key = 46\r\n";
    CHECK(out == want);
    if (out != want) std::printf("--- got:\n%s--- want:\n%s", out.c_str(), want.c_str());
    CHECK(r.changed.size() == 3 && r.unchanged.size() == 1 && r.added.size() == 3);
    // A value longer than its padding keeps one space before the comment; Config reads it back.
    const std::string longer = ff7vr::ini_file::apply("[a]\nk = 1   ; c\n", {{"a", "k", "123456"}});
    CHECK(longer == "[a]\nk = 123456 ; c\n");
    Config c;
    c.load_from_string(out);
    CHECK(c.get_float("comfort", "vignette", 0) == 0.4);
    CHECK(c.get_float("picture", "sharpen", 0) == 0.3);
    CHECK(c.get_int("menu", "key", 0) == 46);
    // No trailing newline, no sections.
    CHECK(ff7vr::ini_file::apply("[a]\nk = 1", {{"a", "k", "2"}, {"a", "j", "3"}}) == "[a]\nk = 2\nj = 3\n");
    CHECK(ff7vr::ini_file::apply("", {{"a", "k", "2"}}) == "[a]\r\nk = 2\r\n");
    std::printf("ini_file: ok\n");
}

static int (*g_orig_add)(int, int) = nullptr;
__declspec(noinline) int add_fn(int a, int b) { return a + b + (g_keep[0] == 'x'); }
static hook::InlineHook g_add_hook;
static int add_detour(int a, int b) { return g_add_hook.original<int (*)(int, int)>()(a, b) * 10; }

struct IFoo {
    virtual int value() { return 1; }
    virtual ~IFoo() = default;
};
static hook::VTableHook g_vt_hook;
static int __fastcall value_detour(IFoo* self) { return g_vt_hook.original<int(__fastcall*)(IFoo*)>()(self) + 100; }

static void test_hook() {
    int (*volatile fn)(int, int) = &add_fn;
    CHECK(fn(2, 3) == 5);
    CHECK(g_add_hook.create(&add_fn, &add_detour));
    CHECK(fn(2, 3) == 50);
    g_add_hook.disable();
    CHECK(fn(2, 3) == 5);
    g_add_hook.remove();
    (void)g_orig_add;

    IFoo* foo = new IFoo();
    IFoo* volatile vfoo = foo;
    CHECK(vfoo->value() == 1);
    CHECK(g_vt_hook.create(*reinterpret_cast<void***>(foo), 0, &value_detour));
    CHECK(vfoo->value() == 101);
    g_vt_hook.remove();
    CHECK(vfoo->value() == 1);
    delete foo;
    std::printf("hook: ok\n");
}

static int bench(const wchar_t* exe) {
    HMODULE m = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) {
        std::printf("cannot map %ls (%lu)\n", exe, GetLastError());
        return 1;
    }
    // LOAD_LIBRARY_AS_IMAGE_RESOURCE returns the base with low bits tagged.
    auto base = reinterpret_cast<std::uintptr_t>(m) & ~std::uintptr_t(3);
    std::size_t text = 0;
    for (auto& s : module::sections(base))
        if (s.executable()) text += s.size;
    std::printf("image mapped at %p, executable bytes %zu MB\n", reinterpret_cast<void*>(base), text >> 20);
    const char* sigs[] = {
        "48 8B 05 ? ? ? ? 48 85 C0 74 ? 48 8B 80",   // typical global load
        "48 89 5C 24 08 57 48 83 EC 20 48 8B D9",    // common prologue
        "E8 ? ? ? ? 48 8B 4C 24 ? 48 85 C9",
        "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 84 C0",
        "FF 90 ? ? ? ? 48 8B",                         // short anchors, many hits
        "DE AD BE EF ? ? CA FE BA BE",                 // not present
        "48 ? ? ? 90",                                 // worst case: 1-byte anchor
        "4C 8D 05 ? ? ? ? 48 8D 15 ? ? ? ? 48 8D 0D",
    };
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    int scans = 0;
    for (int round = 0; round < 5; ++round) {
        for (const char* s : sigs) {
            auto ts = clock::now();
            auto r = pattern::scan_module(base, s, pattern::Sections::Executable, 4);
            if (round == 0)
                std::printf("  %-50s %zu match(es) %.2f ms\n", s, r.matches.size(),
                            std::chrono::duration<double, std::milli>(clock::now() - ts).count());
            ++scans;
        }
    }
    auto ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    std::printf("bench: %d scans in %.1f ms (%.2f ms/scan)\n", scans, ms, ms / scans);
    FreeLibrary(m);
    return 0;
}

static int crash_test() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logp = exe;
    logp.replace(logp.size() - 4, 4, L".log");
    log::init(logp, log::Level::Debug);
    crash::Options o;
    crash::install(o);
    log::info("about to crash on purpose");
    volatile int* p = nullptr;
    *p = 42;  // ACCESS_VIOLATION write
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "bench") == 0) {
        std::wstring w(argv[2], argv[2] + std::strlen(argv[2]));
        return bench(w.c_str());
    }
    if (argc >= 2 && std::strcmp(argv[1], "crash") == 0) return crash_test();
    test_config();
    test_ini_file();
    test_graphics_profile();
    test_pattern();
    test_hook();
    std::printf(g_failures ? "FAILED (%d)\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
