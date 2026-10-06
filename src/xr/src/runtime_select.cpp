// Per-process OpenXR runtime selection. Reads the registry and files only;
// never writes machine-wide state. The system default runtime is untouched.
#include "xr_common.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace ff7vr::xr {
namespace {

std::wstring FindInAvailableRuntimes(const wchar_t* needleLower);
std::string JsonStringValue(const std::string& json, const char* key);

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring GetEnvW(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    const DWORD m = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(m);
    return v;
}

std::wstring RegString(HKEY root, const wchar_t* sub, const wchar_t* value) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, sub, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// First string of the "runtime" array in %LOCALAPPDATA%\openvr\openvrpaths.vrpath.
std::wstring SteamVrFromOpenVrPaths() {
    const std::wstring local = GetEnvW(L"LOCALAPPDATA");
    if (local.empty()) return {};
    std::ifstream f(local + L"\\openvr\\openvrpaths.vrpath", std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    size_t k = s.find("\"runtime\"");
    if (k == std::string::npos) return {};
    k = s.find('[', k);
    if (k == std::string::npos) return {};
    k = s.find('"', k);
    if (k == std::string::npos) return {};
    std::string out;
    for (size_t i = k + 1; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
        }
        out.push_back(s[i]);
    }
    if (out.empty()) return {};
    return Utf8ToWide(out) + L"\\steamxr_win64.json";
}

std::wstring FindSteamVr() {
    std::wstring p = SteamVrFromOpenVrPaths();
    if (!p.empty() && FileExists(p)) return p;
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        std::replace(steam.begin(), steam.end(), L'/', L'\\');
        p = steam + L"\\steamapps\\common\\SteamVR\\steamxr_win64.json";
        if (FileExists(p)) return p;
    }
    return FindInAvailableRuntimes(L"steamxr_win64.json");
}

std::wstring FindInAvailableRuntimes(const wchar_t* needleLower) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS)
        return {};
    std::wstring found;
    for (DWORD i = 0;; ++i) {
        wchar_t name[2048];
        DWORD nameLen = 2048;
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring n(name, nameLen);
        if (Lower(n).find(needleLower) != std::wstring::npos && FileExists(n)) {
            found = n;
            break;
        }
    }
    RegCloseKey(key);
    return found;
}

std::wstring FindVirtualDesktop() {
    std::wstring p = FindInAvailableRuntimes(L"virtualdesktop-openxr.json");
    if (!p.empty()) return p;
    // Not registered (unusual): try the streamer's default install folder under %ProgramFiles%.
    const std::wstring pf = GetEnvW(L"ProgramW6432").empty() ? GetEnvW(L"ProgramFiles") : GetEnvW(L"ProgramW6432");
    if (!pf.empty()) {
        p = pf + L"\\Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr.json";
        if (FileExists(p)) return p;
    }
    return {};
}

}  // namespace

bool ResolveRuntimeJson(std::string_view selection, std::string* outPath, std::string* error) {
    std::wstring sel = Lower(Utf8ToWide(selection));
    std::wstring path;
    if (sel.empty() || sel == L"system" || sel == L"inherit" || sel == L"default") {
        if (outPath) outPath->clear();
        return true;
    }
    if (IsAutoRuntimeSelection(selection)) {
        if (error) *error = "'auto' is a choice among several runtimes, not one runtime";
        return false;
    }
    if (sel == L"steamvr" || sel == L"steam") {
        path = FindSteamVr();
        if (path.empty()) {
            if (error) *error = "SteamVR OpenXR runtime (steamxr_win64.json) not found";
            return false;
        }
    } else if (sel == L"virtualdesktop" || sel == L"vdxr" || sel == L"vd") {
        path = FindVirtualDesktop();
        if (path.empty()) {
            if (error) *error = "Virtual Desktop OpenXR runtime (virtualdesktop-openxr.json) not found";
            return false;
        }
    } else {
        std::wstring raw = Utf8ToWide(selection);
        wchar_t full[MAX_PATH * 4];
        const DWORD n = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
        path = (n > 0 && n < std::size(full)) ? std::wstring(full, n) : raw;
        if (!FileExists(path)) {
            if (error) *error = "runtime JSON not found: " + WideToUtf8(path);
            return false;
        }
    }
    if (outPath) *outPath = WideToUtf8(path);
    return true;
}

bool ApplyRuntimeSelection(std::string_view selection, std::string* outPath, std::string* error) {
    std::string path;
    if (!ResolveRuntimeJson(selection, &path, error)) return false;
    const std::wstring sel = Lower(Utf8ToWide(selection));
    if (sel.empty() || sel == L"inherit") {
        // Leave the environment untouched; report what is set.
        if (outPath) *outPath = WideToUtf8(GetEnvW(L"XR_RUNTIME_JSON"));
        return true;
    }
    if (path.empty()) {
        // "system": remove any override so the loader uses the registry default runtime.
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr);
        _wputenv_s(L"XR_RUNTIME_JSON", L"");
    } else {
        const std::wstring w = Utf8ToWide(path);
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", w.c_str());
        _wputenv_s(L"XR_RUNTIME_JSON", w.c_str());  // keep the CRT copy in sync too
    }
    if (outPath) *outPath = path;
    return true;
}

bool IsAutoRuntimeSelection(std::string_view selection) {
    const std::wstring sel = Lower(Utf8ToWide(selection));
    return sel == L"auto" || sel == L"any";
}

namespace {

// ---- automatic choice: candidates ----

std::wstring ProgramFiles64() {
    std::wstring pf = GetEnvW(L"ProgramW6432");
    return pf.empty() ? GetEnvW(L"ProgramFiles") : pf;
}

std::wstring FileNameLower(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return Lower(s == std::wstring::npos ? path : path.substr(s + 1));
}

std::wstring Canonical(const std::wstring& path) {
    wchar_t full[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    return Lower((n > 0 && n < std::size(full)) ? std::wstring(full, n) : path);
}

std::wstring FindOculus() {
    // The Oculus/Meta PC app records its install folder; default C:\Program Files\Oculus.
    for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Oculus VR, LLC\\Oculus", 0, KEY_READ | view, &key) != ERROR_SUCCESS) continue;
        wchar_t buf[2048];
        DWORD size = sizeof(buf);
        const bool ok = RegGetValueW(key, nullptr, L"Base", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
        RegCloseKey(key);
        if (ok) {
            std::wstring base = buf;
            if (!base.empty() && base.back() != L'\\') base += L'\\';
            const std::wstring p = base + L"Support\\oculus-runtime\\oculus_openxr_64.json";
            if (FileExists(p)) return p;
        }
    }
    const std::wstring p = ProgramFiles64() + L"\\Oculus\\Support\\oculus-runtime\\oculus_openxr_64.json";
    return FileExists(p) ? p : std::wstring();
}

std::wstring FindPico() {
    const std::wstring p = ProgramFiles64() + L"\\PICO Streaming Service\\openxr_runtime_pc\\PicoStreamingXRRuntime\\picostreaming-openxr.json";
    return FileExists(p) ? p : std::wstring();
}

std::wstring FindWmr() {
    const std::wstring sys = GetEnvW(L"SystemRoot");
    if (sys.empty()) return {};
    const std::wstring p = sys + L"\\System32\\MixedRealityRuntime.json";
    return FileExists(p) ? p : std::wstring();
}

std::wstring FindPimax() {
    const std::wstring p = ProgramFiles64() + L"\\Pimax\\Runtime\\PiOpenXR_64.json";
    return FileExists(p) ? p : std::wstring();
}

std::wstring FindVirtualDesktopInstall() {
    const std::wstring p = ProgramFiles64() + L"\\Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr.json";
    return FileExists(p) ? p : std::wstring();
}

// Runtimes the mod knows by name. Matched by the manifest's file name; the
// install path is a fallback for a runtime that is installed but not registered.
struct KnownRuntime {
    const char* name;
    const wchar_t* manifestFile;  // lower case
    std::vector<const wchar_t*> processes;
    // Non-null: probe only while one of `processes` runs (or, with probeWhenActive,
    // while it is the machine's active runtime); the text says why.
    const char* needsRunningBecause;
    bool probeWhenActive;
    std::wstring (*find)();
};

constexpr const char* kStartsServer = "loading it would start it";
// Seen on a PC with PICO's streaming service and no PICO headset: xrGetSystem
// returns a system ('pico'), so a probe cannot tell whether a headset is there.
constexpr const char* kReportsSystemAlways = "its runtime reports a headset even when none is connected";

const std::vector<KnownRuntime>& KnownRuntimes() {
    static const std::vector<KnownRuntime> k = {
        {"Virtual Desktop", L"virtualdesktop-openxr.json", {L"VirtualDesktop.Streamer.exe"}, nullptr, false, &FindVirtualDesktopInstall},
        {"SteamVR", L"steamxr_win64.json", {L"vrserver.exe"}, kStartsServer, true, &FindSteamVr},
        {"Meta Quest Link (Oculus)", L"oculus_openxr_64.json", {L"OVRServer_x64.exe"}, kStartsServer, true, &FindOculus},
        {"PICO", L"picostreaming-openxr.json", {L"PICO Connect.exe", L"PICO Connect TMP.exe"}, kReportsSystemAlways, false, &FindPico},
        {"Windows Mixed Reality", L"mixedrealityruntime.json", {L"MixedRealityPortal.exe"}, kStartsServer, true, &FindWmr},
        {"Pimax", L"piopenxr_64.json", {}, nullptr, false, &FindPimax},
        {"PimaxXR", L"pimax-openxr.json", {}, nullptr, false, nullptr},
        {"Varjo", L"varjoopenxr.json", {}, nullptr, false, nullptr},
        {"Monado", L"openxr_monado.json", {}, nullptr, false, nullptr},
    };
    return k;
}

std::vector<std::wstring> RunningProcessesLower() {
    std::vector<std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) out.push_back(Lower(pe.szExeFile));
    CloseHandle(snap);
    return out;
}

std::string ManifestRuntimeName(const std::wstring& manifest) {
    std::ifstream f(manifest, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    const size_t r = s.find("\"runtime\"");
    if (r == std::string::npos) return {};
    std::string name = JsonStringValue(s.substr(r), "name");
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    return name;
}

}  // namespace

std::vector<RuntimeCandidate> EnumerateRuntimeCandidates() {
    struct Entry {
        std::wstring path;
        std::string origin;
    };
    std::vector<Entry> found;
    auto add = [&](const std::wstring& p, const char* origin) {
        if (p.empty() || !FileExists(p)) return;
        const std::wstring c = Canonical(p);
        for (const Entry& e : found)
            if (Canonical(e.path) == c) return;
        found.push_back({p, origin});
    };
    const std::wstring active = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    add(active, "active runtime");
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ | KEY_WOW64_64KEY, &key) ==
            ERROR_SUCCESS) {
            for (DWORD i = 0;; ++i) {
                wchar_t name[2048];
                DWORD nameLen = 2048;
                DWORD type = 0, data = 0, dataSize = sizeof(data);
                if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize) != ERROR_SUCCESS)
                    break;
                // Value 1 marks a runtime its installer registered as disabled.
                if (type == REG_DWORD && data != 0) continue;
                add(std::wstring(name, nameLen), "registered");
            }
            RegCloseKey(key);
        }
    }
    for (const KnownRuntime& k : KnownRuntimes())
        if (k.find) add(k.find(), "install folder");

    const std::vector<std::wstring> procs = RunningProcessesLower();
    const std::wstring activeCanon = active.empty() ? std::wstring() : Canonical(active);
    std::vector<RuntimeCandidate> out;
    for (const Entry& e : found) {
        RuntimeCandidate c;
        c.manifest = WideToUtf8(e.path);
        c.origin = e.origin;
        c.active = !activeCanon.empty() && Canonical(e.path) == activeCanon;
        const std::wstring file = FileNameLower(e.path);
        const KnownRuntime* known = nullptr;
        for (const KnownRuntime& k : KnownRuntimes())
            if (file == k.manifestFile) known = &k;
        if (known) {
            c.name = known->name;
            if (known->needsRunningBecause) c.needsRunningBecause = known->needsRunningBecause;
            c.probeWhenActive = known->probeWhenActive;
            for (const wchar_t* p : known->processes) {
                if (!c.processesLookedFor.empty()) c.processesLookedFor += ", ";
                c.processesLookedFor += WideToUtf8(p);
                if (c.runningProcess.empty() && std::find(procs.begin(), procs.end(), Lower(p)) != procs.end())
                    c.runningProcess = WideToUtf8(p);
            }
        } else {
            c.name = ManifestRuntimeName(e.path);
            if (c.name.empty()) c.name = WideToUtf8(FileNameLower(e.path));
        }
        out.push_back(std::move(c));
    }
    // Running first, then the active runtime, then the rest (stable: registry order).
    std::stable_sort(out.begin(), out.end(), [](const RuntimeCandidate& a, const RuntimeCandidate& b) {
        auto rank = [](const RuntimeCandidate& c) { return !c.runningProcess.empty() ? 0 : c.active ? 1 : 2; };
        return rank(a) < rank(b);
    });
    return out;
}

bool ShouldProbeRuntime(const RuntimeCandidate& c, std::string* skipReason) {
    if (c.needsRunningBecause.empty() || !c.runningProcess.empty() || (c.active && c.probeWhenActive)) return true;
    if (skipReason)
        *skipReason = "not running (looked for " + (c.processesLookedFor.empty() ? std::string("its process") : c.processesLookedFor) +
                      "); " + c.needsRunningBecause;
    return false;
}

namespace {

// Value of a top-level-ish string key in a small JSON manifest. Enough for the
// "disable_environment" key of an API layer manifest.
std::string JsonStringValue(const std::string& json, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return {};
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    const size_t e = json.find('"', p + 1);
    if (e == std::string::npos) return {};
    return json.substr(p + 1, e - p - 1);
}

void CollectImplicitLayers(HKEY root, std::vector<ImplicitLayer>& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[2048];
        DWORD nameLen = 2048;
        DWORD type = 0, data = 1, dataSize = sizeof(data);
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize) != ERROR_SUCCESS)
            break;
        ImplicitLayer l;
        l.manifest = WideToUtf8(std::wstring(name, nameLen));
        l.enabledInRegistry = (type == REG_DWORD && data == 0);  // OpenXR registry convention: 0 = enabled
        std::ifstream f(std::wstring(name, nameLen), std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            l.disableEnvironment = JsonStringValue(ss.str(), "disable_environment");
            l.name = JsonStringValue(ss.str(), "name");  // the only "name" key of a layer manifest is api_layer.name
        }
        out.push_back(std::move(l));
    }
    RegCloseKey(key);
}

}  // namespace

std::vector<ImplicitLayer> EnumerateImplicitApiLayers() {
    std::vector<ImplicitLayer> out;
    CollectImplicitLayers(HKEY_LOCAL_MACHINE, out);
    CollectImplicitLayers(HKEY_CURRENT_USER, out);
    return out;
}

namespace {

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool LayerMatches(const ImplicitLayer& l, const std::vector<std::string>* patterns) {
    if (!patterns) return true;
    const std::string name = LowerAscii(l.name), path = LowerAscii(l.manifest);
    for (const std::string& p : *patterns) {
        const std::string lp = LowerAscii(p);
        if (!lp.empty() && (name.find(lp) != std::string::npos || path.find(lp) != std::string::npos)) return true;
    }
    return false;
}

int DisableLayers(const std::vector<std::string>* patterns, std::vector<std::string>* disabled) {
    int n = 0;
    for (const ImplicitLayer& l : EnumerateImplicitApiLayers()) {
        if (!l.enabledInRegistry || l.disableEnvironment.empty() || !LayerMatches(l, patterns)) continue;
        const std::wstring var = Utf8ToWide(l.disableEnvironment);
        SetEnvironmentVariableW(var.c_str(), L"1");
        _wputenv_s(var.c_str(), L"1");
        if (disabled) disabled->push_back(l.manifest);
        ++n;
    }
    return n;
}

}  // namespace

int DisableImplicitApiLayers(std::vector<std::string>* disabled) { return DisableLayers(nullptr, disabled); }

int DisableImplicitApiLayersMatching(const std::vector<std::string>& patterns, std::vector<std::string>* disabled) {
    return DisableLayers(&patterns, disabled);
}

}  // namespace ff7vr::xr
