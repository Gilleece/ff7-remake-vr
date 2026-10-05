#include "xr_common.h"

namespace ff7vr::xr {

const char* ToString(Result r) {
    switch (r) {
        case Result::Ok: return "Ok";
        case Result::NotInitialized: return "NotInitialized";
        case Result::InvalidArgument: return "InvalidArgument";
        case Result::CallOrder: return "CallOrder";
        case Result::RuntimeUnavailable: return "RuntimeUnavailable";
        case Result::SystemUnavailable: return "SystemUnavailable";
        case Result::GraphicsMismatch: return "GraphicsMismatch";
        case Result::SessionLost: return "SessionLost";
        case Result::Error: return "Error";
    }
    return "?";
}

const char* ToString(SessionState s) {
    switch (s) {
        case SessionState::Uninitialized: return "Uninitialized";
        case SessionState::Idle: return "Idle";
        case SessionState::Running: return "Running";
        case SessionState::Visible: return "Visible";
        case SessionState::Focused: return "Focused";
        case SessionState::Stopping: return "Stopping";
        case SessionState::Lost: return "Lost";
        case SessionState::ExitRequested: return "ExitRequested";
    }
    return "?";
}

const char* DxgiFormatName(DXGI_FORMAT f) {
    switch (f) {
#define F(x) \
    case DXGI_FORMAT_##x: return #x;
        F(UNKNOWN)
        F(R32G32B32A32_TYPELESS) F(R32G32B32A32_FLOAT)
        F(R16G16B16A16_TYPELESS) F(R16G16B16A16_FLOAT) F(R16G16B16A16_UNORM)
        F(R10G10B10A2_TYPELESS) F(R10G10B10A2_UNORM) F(R11G11B10_FLOAT)
        F(R8G8B8A8_TYPELESS) F(R8G8B8A8_UNORM) F(R8G8B8A8_UNORM_SRGB)
        F(B8G8R8A8_TYPELESS) F(B8G8R8A8_UNORM) F(B8G8R8A8_UNORM_SRGB)
        F(B8G8R8X8_TYPELESS) F(B8G8R8X8_UNORM) F(B8G8R8X8_UNORM_SRGB)
        F(D32_FLOAT) F(D24_UNORM_S8_UINT) F(D16_UNORM) F(D32_FLOAT_S8X24_UINT)
        F(R24G8_TYPELESS) F(R32_TYPELESS) F(R16_TYPELESS) F(R32G8X24_TYPELESS)
#undef F
        default: return "DXGI_FORMAT_other";
    }
}

DXGI_FORMAT TypelessFamily(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        default: return f;
    }
}

bool IsSrgbFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
           f == DXGI_FORMAT_BC1_UNORM_SRGB || f == DXGI_FORMAT_BC2_UNORM_SRGB || f == DXGI_FORMAT_BC3_UNORM_SRGB ||
           f == DXGI_FORMAT_BC7_UNORM_SRGB;
}

bool IsTypelessFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS: return true;
        default: return false;
    }
}

DXGI_FORMAT SrgbVariant(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        default: return f;
    }
}

DXGI_FORMAT LinearVariant(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
        default: return f;
    }
}

DXGI_FORMAT DefaultTypedFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
    }
}

std::string HResultString(HRESULT hr) { return std::format("0x{:08X}", static_cast<uint32_t>(hr)); }

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}

int64_t QpcNowNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t sec = c.QuadPart / freq;
    const int64_t rem = c.QuadPart % freq;
    return sec * 1000000000LL + rem * 1000000000LL / freq;
}

}  // namespace ff7vr::xr
