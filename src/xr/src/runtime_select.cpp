// Per-process OpenXR runtime selection. Reads the registry and files only;
// never writes machine-wide state. The system default runtime is untouched.
#include "xr_common.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace ff7vr::xr {
namespace {

std::wstring FindInAvailableRuntimes(const wchar_t* needleLower);

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

int DisableImplicitApiLayers(std::vector<std::string>* disabled) {
    int n = 0;
    for (const ImplicitLayer& l : EnumerateImplicitApiLayers()) {
        if (!l.enabledInRegistry || l.disableEnvironment.empty()) continue;
        const std::wstring var = Utf8ToWide(l.disableEnvironment);
        SetEnvironmentVariableW(var.c_str(), L"1");
        _wputenv_s(var.c_str(), L"1");
        if (disabled) disabled->push_back(l.manifest);
        ++n;
    }
    return n;
}

}  // namespace ff7vr::xr
