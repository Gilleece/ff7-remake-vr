// Internal helpers shared by the XR backends.
#pragma once

#include "ff7vr/xr/xr.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <format>
#include <string>

namespace ff7vr::xr {

using Microsoft::WRL::ComPtr;

class Logger {
public:
    void Set(LogCallback cb) { cb_ = std::move(cb); }
    bool Enabled() const { return static_cast<bool>(cb_); }

    void Write(LogLevel level, std::string_view msg) const {
        if (cb_) cb_(level, msg);
    }
    template <class... A>
    void Debug(std::format_string<A...> f, A&&... a) const { Emit(LogLevel::Debug, f, std::forward<A>(a)...); }
    template <class... A>
    void Info(std::format_string<A...> f, A&&... a) const { Emit(LogLevel::Info, f, std::forward<A>(a)...); }
    template <class... A>
    void Warn(std::format_string<A...> f, A&&... a) const { Emit(LogLevel::Warn, f, std::forward<A>(a)...); }
    template <class... A>
    void Error(std::format_string<A...> f, A&&... a) const { Emit(LogLevel::Error, f, std::forward<A>(a)...); }
    template <class... A>
    void Notice(std::format_string<A...> f, A&&... a) const { Emit(LogLevel::Notice, f, std::forward<A>(a)...); }

private:
    template <class... A>
    void Emit(LogLevel level, std::format_string<A...> f, A&&... a) const {
        if (!cb_) return;
        try {
            cb_(level, std::format(f, std::forward<A>(a)...));
        } catch (...) {
        }
    }
    LogCallback cb_;
};

// Format helpers
DXGI_FORMAT TypelessFamily(DXGI_FORMAT f);  // e.g. R8G8B8A8_UNORM_SRGB -> R8G8B8A8_TYPELESS; unknown -> f
bool IsSrgbFormat(DXGI_FORMAT f);
bool IsTypelessFormat(DXGI_FORMAT f);
DXGI_FORMAT SrgbVariant(DXGI_FORMAT f);    // UNORM -> UNORM_SRGB where one exists, else f
DXGI_FORMAT LinearVariant(DXGI_FORMAT f);  // UNORM_SRGB -> UNORM where one exists, else f
// For a typeless format, the default "linear" typed view (R8G8B8A8_TYPELESS -> R8G8B8A8_UNORM).
DXGI_FORMAT DefaultTypedFormat(DXGI_FORMAT f);

std::string HResultString(HRESULT hr);
std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

int64_t QpcNowNs();

// Points the OpenXR loader of this process at a runtime manifest ("" = the
// machine's default): XR_RUNTIME_JSON plus the loader property, which also
// unloads a runtime loaded earlier. Only while no XrInstance exists.
bool PointLoaderAtRuntime(const std::string& manifest, std::string* error);

}  // namespace ff7vr::xr
