// OpenXR backend for D3D11 (XR_KHR_D3D11_enable). The OpenXR loader is linked
// statically. One swapchain per eye and one per quad layer; per frame an
// optional projection layer followed by quad layers; LOCAL reference space
// (VIEW space for head-locked quads). See xr.h for the threading contract.
#include "backend_base.h"

#include <dxgi.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace ff7vr::xr {
namespace {

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

Pose ToPose(const XrPosef& p) {
    return Pose{Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, Vec3{p.position.x, p.position.y, p.position.z}};
}
XrPosef ToXr(const Pose& p) {
    XrPosef r;
    r.orientation = XrQuaternionf{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    r.position = XrVector3f{p.position.x, p.position.y, p.position.z};
    return r;
}
Fov ToFov(const XrFovf& f) { return Fov{f.angleLeft, f.angleRight, f.angleUp, f.angleDown}; }
XrFovf ToXr(const Fov& f) { return XrFovf{f.angleLeft, f.angleRight, f.angleUp, f.angleDown}; }

std::string VersionString(XrVersion v) {
    return std::format("{}.{}.{}", XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), XR_VERSION_PATCH(v));
}

const char* XrStateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

// Order of preference for the eye swapchains. sRGB 8-bit first: the runtime
// gets gamma-correct data with the best precision per bit and the copy path
// applies to sRGB-encoded 8-bit sources.
constexpr DXGI_FORMAT kPreferredFormats[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM,
};

class OpenXrBackend final : public BackendBase {
public:
    ~OpenXrBackend() override { Shutdown(); }

    BackendType Type() const override { return BackendType::OpenXR; }

    Result Init(const InitDesc& desc) override;
    void Shutdown() override;
    Result WaitFrame(FrameInfo& info) override;
    Result BeginFrame(uint64_t frameId) override;
    Result RelocateViews(uint64_t frameId, View outViews[2]) override;
    Result SubmitFrame(uint64_t frameId, const SubmitDesc& desc) override;
    Result SkipFrame(uint64_t frameId) override;
    Result CreateQuadLayer(const QuadLayerCreateDesc& desc, LayerHandle* out) override;
    void DestroyQuadLayer(LayerHandle layer) override;

private:
    enum class ImageWait { Ready, NotReady, Failed };

    std::string Name(XrResult r) const;
    bool Check(XrResult r, const char* what) const;
    Result MapError(XrResult r) const;
    Result InitImpl(const InitDesc& desc);
    void DestroyAll();
    void PollEvents();
    void HandleSessionState(XrSessionState s);
    bool LocateViews(int64_t time, View raw[2], bool* orientationValid, bool* positionValid);
    Result EndFrameLocked(int64_t displayTime, const XrCompositionLayerBaseHeader* const* layers, uint32_t layerCount);
    Result StaleOrUnknown(uint64_t frameId);
    Result BeginLocked(FrameRecord& r);
    Result CreateSwapchain(uint32_t w, uint32_t h, DXGI_FORMAT fmt, SwapImages* out);
    void DestroySwapchain(SwapImages& sc);
    // Acquires an image and waits for it (retrying a wait that timed out on an
    // earlier frame instead of acquiring another one).
    ImageWait AcquireAndWait(SwapImages& sc, uint32_t* index);
    // Acquire, copy with `transfer(target, &w, &h)`, release. Updates sc.last*.
    template <class F>
    ImageWait UpdateImage(SwapImages& sc, F&& transfer);
    static XrSwapchain Handle(const SwapImages& sc) { return reinterpret_cast<XrSwapchain>(sc.xr); }

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrDebugUtilsMessengerEXT messenger_ = XR_NULL_HANDLE;
    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    SwapImages eyes_[2];

    // Guards xrBeginSession/xrEndSession against xrBeginFrame/xrEndFrame on the other thread.
    std::mutex sessionMutex_;
    std::atomic<bool> sessionRunning_{false};
    XrSessionState xrState_ = XR_SESSION_STATE_UNKNOWN;  // GT
    bool exitRequested_ = false;                         // GT

    bool hasRefreshRate_ = false, hasDepth_ = false, hasDebugUtils_ = false;
    PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetD3D11Req_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB pfnGetRefresh_ = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumRefresh_ = nullptr;
    PFN_xrCreateDebugUtilsMessengerEXT pfnCreateMessenger_ = nullptr;
    PFN_xrDestroyDebugUtilsMessengerEXT pfnDestroyMessenger_ = nullptr;

    std::string runtimeJson_;
};

std::string OpenXrBackend::Name(XrResult r) const {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance_ != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance_, r, buf))) return buf;
    // Without an instance: the results that occur before one exists.
    switch (r) {
        case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_API_LAYER_NOT_PRESENT: return "XR_ERROR_API_LAYER_NOT_PRESENT";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        default: return std::format("XrResult({})", static_cast<int>(r));
    }
}

bool OpenXrBackend::Check(XrResult r, const char* what) const {
    if (XR_SUCCEEDED(r)) return true;
    log_.Error("{} failed: {}", what, Name(r));
    return false;
}

Result OpenXrBackend::MapError(XrResult r) const {
    switch (r) {
        case XR_SUCCESS: return Result::Ok;
        case XR_ERROR_SESSION_LOST:
        case XR_ERROR_INSTANCE_LOST:
        case XR_SESSION_LOSS_PENDING: return Result::SessionLost;
        case XR_ERROR_CALL_ORDER_INVALID:
        case XR_ERROR_SESSION_NOT_RUNNING: return Result::CallOrder;
        case XR_ERROR_VALIDATION_FAILURE: return Result::InvalidArgument;
        default: return Result::Error;
    }
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
Result OpenXrBackend::Init(const InitDesc& desc) {
    if (initialized_) Shutdown();
    const Result r = InitImpl(desc);
    if (r != Result::Ok) {
        DestroyAll();
        ShutdownCommon();
        return r;
    }
    initialized_ = true;
    return Result::Ok;
}

Result OpenXrBackend::InitImpl(const InitDesc& desc) {
    if (const Result r = InitCommon(desc); r != Result::Ok) return r;
    RuntimeInfo info;
    info.backend = BackendType::OpenXR;

    // ---- runtime selection (this process only) ----
    std::string err;
    if (!ApplyRuntimeSelection(desc.runtime, &runtimeJson_, &err)) {
        log_.Error("runtime selection '{}': {}", desc.runtime, err);
        return Result::RuntimeUnavailable;
    }
    info.runtimeJson = runtimeJson_;
    log_.Info("OpenXR runtime: {}", runtimeJson_.empty() ? std::string("system default") : runtimeJson_);
    {
        // Same choice as a loader property: honoured even where the loader ignores
        // the environment (elevated process). An empty list clears earlier overrides.
        PFN_xrInitializeLoaderKHR initLoader = nullptr;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                               reinterpret_cast<PFN_xrVoidFunction*>(&initLoader))) &&
            initLoader) {
            XrLoaderInitPropertyValueEXT prop{"XR_RUNTIME_JSON", runtimeJson_.c_str()};
            XrLoaderInitInfoPropertiesEXT props{XR_TYPE_LOADER_INIT_INFO_PROPERTIES_EXT};
            props.propertyValueCount = runtimeJson_.empty() ? 0u : 1u;
            props.propertyValues = &prop;
            const XrResult xr = initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&props));
            if (XR_FAILED(xr)) log_.Warn("xrInitializeLoaderKHR: {} (continuing with the environment only)", Name(xr));
        }
    }

    // ---- implicit API layers ----
    std::vector<std::string> disabledLayers;
    if (desc.disableImplicitApiLayers)
        DisableImplicitApiLayers(&disabledLayers);
    else if (!desc.disableImplicitApiLayersMatching.empty())
        DisableImplicitApiLayersMatching(desc.disableImplicitApiLayersMatching, &disabledLayers);
    for (const ImplicitLayer& l : EnumerateImplicitApiLayers()) {
        const bool disabledHere = std::find(disabledLayers.begin(), disabledLayers.end(), l.manifest) != disabledLayers.end();
        info.implicitLayers.push_back(l.manifest + (disabledHere ? " (disabled for this process)"
                                                                 : (l.enabledInRegistry ? " (enabled)" : " (disabled)")));
    }
    {
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrEnumerateApiLayerProperties(0, &n, nullptr)) && n) {
            std::vector<XrApiLayerProperties> layers(n, XrApiLayerProperties{XR_TYPE_API_LAYER_PROPERTIES});
            if (XR_SUCCEEDED(xrEnumerateApiLayerProperties(n, &n, layers.data())))
                for (uint32_t i = 0; i < n; ++i) info.apiLayers.push_back(layers[i].layerName);
        }
    }

    // ---- extensions ----
    uint32_t extCount = 0;
    XrResult xr = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(xr)) {
        log_.Error("xrEnumerateInstanceExtensionProperties failed: {} (runtime missing or failed to load)", Name(xr));
        return Result::RuntimeUnavailable;
    }
    std::vector<XrExtensionProperties> exts(extCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()), "xrEnumerateInstanceExtensionProperties"))
        return Result::RuntimeUnavailable;
    auto has = [&](const char* name) {
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    std::vector<const char*> enable;
    if (!has(XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
        log_.Error("runtime does not offer {}", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return Result::RuntimeUnavailable;
    }
    enable.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    if (desc.requestDepthExtension && has(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) {
        enable.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
        hasDepth_ = true;
    }
    if (has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        hasRefreshRate_ = true;
    }
    if (desc.enableDebugUtils && has(XR_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        enable.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
        hasDebugUtils_ = true;
    }
    for (const char* e : enable) info.enabledExtensions.emplace_back(e);

    // ---- instance ----
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ici.applicationInfo.applicationName, desc.appName.c_str(), _TRUNCATE);
    ici.applicationInfo.applicationVersion = desc.appVersion;
    strncpy_s(ici.applicationInfo.engineName, "ff7vr", _TRUNCATE);
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;  // widest runtime compatibility; nothing from 1.1 is needed
    ici.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ici.enabledExtensionNames = enable.data();
    xr = xrCreateInstance(&ici, &instance_);
    if (XR_FAILED(xr)) {
        log_.Error("xrCreateInstance failed: {}", Name(xr));
        return Result::RuntimeUnavailable;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (Check(xrGetInstanceProperties(instance_, &ip), "xrGetInstanceProperties")) {
        info.runtimeName = ip.runtimeName;
        info.runtimeVersion = VersionString(ip.runtimeVersion);
    }
    log_.Info("OpenXR instance: runtime '{}' {}", info.runtimeName, info.runtimeVersion);

    auto getProc = [&](const char* name, auto* pfn) {
        return XR_SUCCEEDED(xrGetInstanceProcAddr(instance_, name, reinterpret_cast<PFN_xrVoidFunction*>(pfn))) && *pfn;
    };
    if (!getProc("xrGetD3D11GraphicsRequirementsKHR", &pfnGetD3D11Req_)) {
        log_.Error("xrGetD3D11GraphicsRequirementsKHR not available");
        return Result::RuntimeUnavailable;
    }
    if (hasRefreshRate_) {
        hasRefreshRate_ = getProc("xrGetDisplayRefreshRateFB", &pfnGetRefresh_) && getProc("xrEnumerateDisplayRefreshRatesFB", &pfnEnumRefresh_);
    }
    if (hasDebugUtils_ && getProc("xrCreateDebugUtilsMessengerEXT", &pfnCreateMessenger_) &&
        getProc("xrDestroyDebugUtilsMessengerEXT", &pfnDestroyMessenger_)) {
        XrDebugUtilsMessengerCreateInfoEXT ci{XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        ci.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                               XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        ci.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
        ci.userCallback = [](XrDebugUtilsMessageSeverityFlagsEXT sev, XrDebugUtilsMessageTypeFlagsEXT,
                             const XrDebugUtilsMessengerCallbackDataEXT* data, void* user) -> XrBool32 {
            const auto* self = static_cast<const OpenXrBackend*>(user);
            const LogLevel lvl = (sev & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)     ? LogLevel::Error
                                 : (sev & XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? LogLevel::Warn
                                                                                           : LogLevel::Debug;
            self->log_.Write(lvl, std::string("openxr: ") + (data && data->message ? data->message : ""));
            return XR_FALSE;
        };
        ci.userData = this;
        if (XR_FAILED(pfnCreateMessenger_(instance_, &ci, &messenger_))) messenger_ = XR_NULL_HANDLE;
    }

    // ---- system ----
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    xr = xrGetSystem(instance_, &sgi, &systemId_);
    if (xr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        log_.Warn("xrGetSystem: no headset available right now (XR_ERROR_FORM_FACTOR_UNAVAILABLE)");
        return Result::SystemUnavailable;
    }
    if (!Check(xr, "xrGetSystem")) return Result::SystemUnavailable;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (Check(xrGetSystemProperties(instance_, systemId_, &sp), "xrGetSystemProperties")) {
        info.systemName = sp.systemName;
        info.vendorId = sp.vendorId;
        info.maxSwapchainWidth = sp.graphicsProperties.maxSwapchainImageWidth;
        info.maxSwapchainHeight = sp.graphicsProperties.maxSwapchainImageHeight;
        info.orientationTracking = sp.trackingProperties.orientationTracking != XR_FALSE;
        info.positionTracking = sp.trackingProperties.positionTracking != XR_FALSE;
    }
    log_.Info("OpenXR system: '{}' vendor 0x{:X}", info.systemName, info.vendorId);

    // ---- graphics requirements: the runtime's adapter must be the device's adapter ----
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(pfnGetD3D11Req_(instance_, systemId_, &req), "xrGetD3D11GraphicsRequirementsKHR")) return Result::Error;
    // SteamVR reports LUID 0 until its compositor runs, which happens once a
    // session exists: ask again briefly, then continue with the device's adapter.
    for (int i = 0; i < 8 && req.adapterLuid.LowPart == 0 && req.adapterLuid.HighPart == 0; ++i) {
        if (i == 0) log_.Info("runtime reports no adapter yet (LUID 0); waiting for it to start");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!Check(pfnGetD3D11Req_(instance_, systemId_, &req), "xrGetD3D11GraphicsRequirementsKHR")) return Result::Error;
    }
    const bool luidKnown = req.adapterLuid.LowPart != 0 || req.adapterLuid.HighPart != 0;
    if (!luidKnown) log_.Info("runtime names no adapter yet; continuing with the device's adapter (SteamVR does this until its compositor runs)");
    info.adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(req.adapterLuid.HighPart)) << 32) | req.adapterLuid.LowPart;
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad))) {
            if (luidKnown && (ad.AdapterLuid.LowPart != req.adapterLuid.LowPart || ad.AdapterLuid.HighPart != req.adapterLuid.HighPart)) {
                log_.Error("the D3D11 device is on adapter '{}' but the runtime requires adapter LUID {:08X}:{:08X}",
                           WideToUtf8(ad.Description), static_cast<uint32_t>(req.adapterLuid.HighPart), req.adapterLuid.LowPart);
                return Result::GraphicsMismatch;
            }
            log_.Info("adapter: {}", WideToUtf8(ad.Description));
        }
        if (device_->GetFeatureLevel() < req.minFeatureLevel) {
            log_.Error("device feature level 0x{:X} below the runtime minimum 0x{:X}", static_cast<int>(device_->GetFeatureLevel()),
                       static_cast<int>(req.minFeatureLevel));
            return Result::GraphicsMismatch;
        }
    }

    // ---- views ----
    uint32_t viewCount = 0;
    if (!Check(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewConfig, 0, &viewCount, nullptr), "xrEnumerateViewConfigurationViews"))
        return Result::Error;
    if (viewCount != 2) {
        log_.Error("primary stereo view configuration has {} views", viewCount);
        return Result::Error;
    }
    XrViewConfigurationView vcv[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (!Check(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewConfig, 2, &viewCount, vcv), "xrEnumerateViewConfigurationViews"))
        return Result::Error;
    for (int e = 0; e < 2; ++e) {
        info.recommendedWidth[e] = vcv[e].recommendedImageRectWidth;
        info.recommendedHeight[e] = vcv[e].recommendedImageRectHeight;
        info.maxWidth[e] = vcv[e].maxImageRectWidth;
        info.maxHeight[e] = vcv[e].maxImageRectHeight;
    }

    {
        uint32_t n = 0;
        std::vector<XrEnvironmentBlendMode> modes;
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance_, systemId_, kViewConfig, 0, &n, nullptr)) && n) {
            modes.resize(n);
            xrEnumerateEnvironmentBlendModes(instance_, systemId_, kViewConfig, n, &n, modes.data());
        }
        blendMode_ = modes.empty() ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : modes[0];
        for (auto m : modes)
            if (m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) blendMode_ = m;
    }

    // ---- session ----
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_.Get();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = systemId_;
    xr = xrCreateSession(instance_, &sci, &session_);
    if (XR_FAILED(xr)) {
        log_.Error("xrCreateSession failed: {}", Name(xr));
        return xr == XR_ERROR_GRAPHICS_DEVICE_INVALID ? Result::GraphicsMismatch : Result::Error;
    }
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(session_, &rsci, &localSpace_), "xrCreateReferenceSpace(LOCAL)")) return Result::Error;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(session_, &rsci, &viewSpace_), "xrCreateReferenceSpace(VIEW)")) return Result::Error;

    // ---- swapchain format ----
    {
        uint32_t n = 0;
        if (!Check(xrEnumerateSwapchainFormats(session_, 0, &n, nullptr), "xrEnumerateSwapchainFormats")) return Result::Error;
        std::vector<int64_t> formats(n);
        if (!Check(xrEnumerateSwapchainFormats(session_, n, &n, formats.data()), "xrEnumerateSwapchainFormats")) return Result::Error;
        for (int64_t f : formats) info.runtimeFormats.push_back(static_cast<DXGI_FORMAT>(f));
        auto offered = [&](DXGI_FORMAT f) { return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end(); };
        format_ = DXGI_FORMAT_UNKNOWN;
        if (desc.swapchainFormat != DXGI_FORMAT_UNKNOWN) {
            if (offered(desc.swapchainFormat))
                format_ = desc.swapchainFormat;
            else
                log_.Warn("requested swapchain format {} not offered by the runtime; choosing automatically",
                          DxgiFormatName(desc.swapchainFormat));
        }
        for (DXGI_FORMAT f : kPreferredFormats)
            if (format_ == DXGI_FORMAT_UNKNOWN && offered(f)) format_ = f;
        if (format_ == DXGI_FORMAT_UNKNOWN) {
            log_.Error("no usable colour swapchain format offered by the runtime");
            return Result::Error;
        }
    }

    // ---- swapchains, one per eye ----
    for (int e = 0; e < 2; ++e) {
        uint32_t w = desc.eyeWidth, h = desc.eyeHeight;
        if (w == 0 || h == 0) {
            const float sc = desc.resolutionScale > 0.0f ? desc.resolutionScale : 1.0f;
            w = static_cast<uint32_t>(std::lround(vcv[e].recommendedImageRectWidth * sc));
            h = static_cast<uint32_t>(std::lround(vcv[e].recommendedImageRectHeight * sc));
        }
        w = std::clamp(w, 16u, std::max(16u, vcv[e].maxImageRectWidth));
        h = std::clamp(h, 16u, std::max(16u, vcv[e].maxImageRectHeight));
        if (const Result r = CreateSwapchain(w, h, format_, &eyes_[e]); r != Result::Ok) return r;
        info.eyeSwapchain[e] = SwapchainInfo{w, h, format_, static_cast<uint32_t>(eyes_[e].images.size())};
        D3D11_TEXTURE2D_DESC td{};
        if (!eyes_[e].images.empty()) eyes_[e].images[0]->GetDesc(&td);
        log_.Info("eye {} swapchain: {}x{} {} ({} images, texture format {}, bind 0x{:X})", e, w, h, DxgiFormatName(format_),
                  eyes_[e].images.size(), DxgiFormatName(td.Format), td.BindFlags);
    }

    // ---- refresh rate ----
    if (hasRefreshRate_) {
        float hz = 0;
        if (XR_SUCCEEDED(pfnGetRefresh_(session_, &hz))) info.refreshHz = hz;
        uint32_t n = 0;
        if (XR_SUCCEEDED(pfnEnumRefresh_(session_, 0, &n, nullptr)) && n) {
            info.availableRefreshHz.resize(n);
            pfnEnumRefresh_(session_, n, &n, info.availableRefreshHz.data());
        }
    }
    info.depthLayerSupported = hasDepth_;

    {
        std::lock_guard lk(infoMutex_);
        info_ = std::move(info);
    }
    xrState_ = XR_SESSION_STATE_UNKNOWN;
    exitRequested_ = false;
    sessionRunning_ = false;
    state_ = SessionState::Idle;
    return Result::Ok;
}

void OpenXrBackend::Shutdown() {
    if (!initialized_) return;
    // Graceful end: ask the runtime to stop and keep the frame loop going with
    // empty frames until it reaches STOPPING (bounded), then end the session.
    if (session_ != XR_NULL_HANDLE && sessionRunning_) {
        // First end frames the host waited for but never submitted; otherwise the
        // next xrWaitFrame would block.
        std::vector<uint64_t> open;
        {
            std::lock_guard fl(frameMutex_);
            for (const FrameRecord& r : ring_)
                if (r.id && r.epoch == epoch_ && !r.ended) open.push_back(r.id);
        }
        std::sort(open.begin(), open.end());
        for (uint64_t id : open) SkipFrame(id);
        if (XR_SUCCEEDED(xrRequestExitSession(session_))) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            while (sessionRunning_ && std::chrono::steady_clock::now() < deadline) {
                FrameInfo fi;
                if (WaitFrame(fi) != Result::Ok) break;
                if (fi.sessionRunning)
                    SkipFrame(fi.frameId);
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (sessionRunning_) {
            std::lock_guard lk(sessionMutex_);
            xrEndSession(session_);
            sessionRunning_ = false;
        }
    }
    DestroyAll();
    initialized_ = false;
    ShutdownCommon();
}

void OpenXrBackend::DestroyAll() {
    blitter_.ClearCache();  // drops cached views of swapchain images
    {
        std::lock_guard lk(quadMutex_);
        for (QuadSlot& q : quads_) {
            if (q.used) DestroySwapchain(q.sc);
            q = QuadSlot{};
        }
    }
    for (auto& e : eyes_) DestroySwapchain(e);
    if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
    if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
    viewSpace_ = localSpace_ = XR_NULL_HANDLE;
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
    if (messenger_ != XR_NULL_HANDLE && pfnDestroyMessenger_) pfnDestroyMessenger_(messenger_);
    messenger_ = XR_NULL_HANDLE;
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
    systemId_ = XR_NULL_SYSTEM_ID;
    sessionRunning_ = false;
    hasRefreshRate_ = hasDepth_ = hasDebugUtils_ = false;
    pfnGetD3D11Req_ = nullptr;
    pfnGetRefresh_ = nullptr;
    pfnEnumRefresh_ = nullptr;
    pfnCreateMessenger_ = nullptr;
    pfnDestroyMessenger_ = nullptr;
}

// ---------------------------------------------------------------------------
// Events and session state (game thread)
// ---------------------------------------------------------------------------
void OpenXrBackend::PollEvents() {
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult r = xrPollEvent(instance_, &ev);
        if (r != XR_SUCCESS) {
            if (XR_FAILED(r)) log_.Error("xrPollEvent failed: {}", Name(r));
            return;
        }
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& e = reinterpret_cast<const XrEventDataSessionStateChanged&>(ev);
                HandleSessionState(e.state);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                log_.Error("OpenXR instance loss pending");
                state_ = SessionState::Lost;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const auto& e = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(ev);
                log_.Info("runtime reference space change pending (space type {}, the runtime recentred)",
                          static_cast<int>(e.referenceSpaceType));
                break;
            }
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                const auto& e = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(ev);
                log_.Info("display refresh rate {} -> {} Hz", e.fromDisplayRefreshRate, e.toDisplayRefreshRate);
                std::lock_guard lk(infoMutex_);
                info_.refreshHz = e.toDisplayRefreshRate;
                break;
            }
            case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
                const auto& e = reinterpret_cast<const XrEventDataEventsLost&>(ev);
                log_.Warn("OpenXR: {} events lost", e.lostEventCount);
                break;
            }
            default: log_.Debug("OpenXR event type {}", static_cast<int>(ev.type)); break;
        }
    }
}

void OpenXrBackend::HandleSessionState(XrSessionState s) {
    log_.Info("session state {} -> {}", XrStateName(xrState_), XrStateName(s));
    xrState_ = s;
    switch (s) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = kViewConfig;
            std::lock_guard lk(sessionMutex_);
            if (Check(xrBeginSession(session_, &bi), "xrBeginSession")) {
                sessionRunning_ = true;
                state_ = SessionState::Running;
            }
            break;
        }
        case XR_SESSION_STATE_SYNCHRONIZED: state_ = SessionState::Running; break;
        case XR_SESSION_STATE_VISIBLE: state_ = SessionState::Visible; break;
        case XR_SESSION_STATE_FOCUSED: state_ = SessionState::Focused; break;
        case XR_SESSION_STATE_STOPPING: {
            std::lock_guard lk(sessionMutex_);
            {
                // Frames waited before this point are stale now.
                std::lock_guard fl(frameMutex_);
                ++epoch_;
            }
            Check(xrEndSession(session_), "xrEndSession");
            sessionRunning_ = false;
            state_ = SessionState::Stopping;
            break;
        }
        case XR_SESSION_STATE_IDLE: state_ = SessionState::Idle; break;
        case XR_SESSION_STATE_LOSS_PENDING: state_ = SessionState::Lost; break;
        case XR_SESSION_STATE_EXITING:
            exitRequested_ = true;
            state_ = SessionState::ExitRequested;
            break;
        default: break;
    }
}

bool OpenXrBackend::LocateViews(int64_t time, View raw[2], bool* orientationValid, bool* positionValid) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = kViewConfig;
    li.displayTime = time;
    li.space = localSpace_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    const XrResult r = xrLocateViews(session_, &li, &vs, 2, &n, views);
    if (XR_FAILED(r) || n != 2) {
        log_.Error("xrLocateViews failed: {}", Name(r));
        *orientationValid = *positionValid = false;
        return false;
    }
    *orientationValid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    *positionValid = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    for (int e = 0; e < 2; ++e) {
        raw[e].pose = ToPose(views[e].pose);
        raw[e].fov = ToFov(views[e].fov);
    }
    return true;
}

Result OpenXrBackend::WaitFrame(FrameInfo& info) {
    info = FrameInfo{};
    if (!initialized_) return Result::NotInitialized;
    PollEvents();
    info.state = state_.load();
    if (info.state == SessionState::Lost) return Result::SessionLost;
    if (!sessionRunning_) return Result::Ok;  // idle: host renders flat; call again next frame

    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    const XrResult xr = xrWaitFrame(session_, &wi, &fs);
    if (XR_FAILED(xr) || xr == XR_SESSION_LOSS_PENDING) {
        log_.Error("xrWaitFrame failed: {}", Name(xr));
        if (MapError(xr) == Result::SessionLost) state_ = SessionState::Lost;
        if (xr == XR_ERROR_SESSION_NOT_RUNNING) return Result::Ok;  // raced with STOPPING; next call polls it
        return MapError(xr);
    }

    View raw[2];
    bool ov = false, pv = false;
    LocateViews(fs.predictedDisplayTime, raw, &ov, &pv);
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    Pose rawHead;
    bool headValid = false;
    if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, localSpace_, fs.predictedDisplayTime, &head))) {
        headValid = (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        rawHead = ToPose(head.pose);
    }
    {
        std::lock_guard lk(frameMutex_);
        FrameRecord& r = NewFrameLocked();
        r.displayTime = fs.predictedDisplayTime;
        r.period = fs.predictedDisplayPeriod;
        r.shouldRender = fs.shouldRender != XR_FALSE;
        r.orientationValid = ov;
        r.positionValid = pv;
        r.raw[0] = raw[0];
        r.raw[1] = raw[1];
        r.rawHead = rawHead;
        r.recenter = UpdateRecenter(rawHead, headValid);
        FillFrameInfo(r, info);
    }
    {
        std::lock_guard lk(infoMutex_);
        info_.lastPredictedDisplayPeriod = fs.predictedDisplayPeriod;
        if (!hasRefreshRate_ && fs.predictedDisplayPeriod > 0) info_.refreshHz = static_cast<float>(1e9 / fs.predictedDisplayPeriod);
    }
    CountStat(&FrameStats::framesWaited);
    if (!info.shouldRender) CountStat(&FrameStats::framesNotRendered);
    return Result::Ok;
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
Result OpenXrBackend::StaleOrUnknown(uint64_t frameId) {
    // Caller holds frameMutex_.
    if (frameId == 0 || frameId >= nextFrameId_) {
        log_.Error("unknown frameId {}", frameId);
        return Result::CallOrder;
    }
    log_.Debug("frame {} is stale (session restarted or frame too old); ignored", frameId);
    return Result::Ok;
}

Result OpenXrBackend::BeginLocked(FrameRecord& r) {
    // Caller holds sessionMutex_ and frameMutex_.
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult xr;
    const int64_t t0 = QpcNowNs();
    {
        ScopedStateBackup backup(stateBackup_, context_.Get());
        xr = xrBeginFrame(session_, &bi);
    }
    AddMs(&FrameStats::beginFrameMs, t0);
    r.begun = true;
    if (xr == XR_FRAME_DISCARDED) {
        CountStat(&FrameStats::framesDiscarded);
    } else if (XR_FAILED(xr)) {
        log_.Error("xrBeginFrame failed: {}", Name(xr));
        if (MapError(xr) == Result::SessionLost) state_ = SessionState::Lost;
        return MapError(xr);
    }
    CountStat(&FrameStats::framesBegun);
    return Result::Ok;
}

Result OpenXrBackend::BeginFrame(uint64_t frameId) {
    if (!initialized_) return Result::NotInitialized;
    std::lock_guard sl(sessionMutex_);
    std::lock_guard fl(frameMutex_);
    FrameRecord* r = FindFrameLocked(frameId);
    if (!r || r->epoch != epoch_ || !sessionRunning_) return r ? Result::Ok : StaleOrUnknown(frameId);
    if (r->begun) {
        log_.Error("BeginFrame({}) called twice", frameId);
        return Result::CallOrder;
    }
    return BeginLocked(*r);
}

Result OpenXrBackend::RelocateViews(uint64_t frameId, View outViews[2]) {
    if (!initialized_) return Result::NotInitialized;
    int64_t t = 0;
    {
        std::lock_guard fl(frameMutex_);
        FrameRecord* r = FindFrameLocked(frameId);
        if (!r) return Result::CallOrder;
        t = r->displayTime;
    }
    View raw[2];
    bool ov = false, pv = false;
    if (!LocateViews(t, raw, &ov, &pv)) return Result::Error;
    std::lock_guard fl(frameMutex_);
    FrameRecord* r = FindFrameLocked(frameId);
    if (!r) return Result::CallOrder;
    if (ov) {
        r->raw[0] = raw[0];
        r->raw[1] = raw[1];
        r->orientationValid = ov;
        r->positionValid = pv;
    }
    for (int e = 0; e < 2; ++e) outViews[e] = ApplyRecenter(r->recenter, r->raw[e]);
    return Result::Ok;
}

Result OpenXrBackend::EndFrameLocked(int64_t displayTime, const XrCompositionLayerBaseHeader* const* layers, uint32_t layerCount) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = blendMode_;
    ei.layerCount = layerCount;
    ei.layers = layers;
    XrResult xr;
    const int64_t t0 = QpcNowNs();
    {
        ScopedStateBackup backup(stateBackup_, context_.Get());
        xr = xrEndFrame(session_, &ei);
    }
    AddMs(&FrameStats::endFrameMs, t0);
    if (XR_FAILED(xr)) {
        log_.Error("xrEndFrame failed: {}", Name(xr));
        if (MapError(xr) == Result::SessionLost) state_ = SessionState::Lost;
        return MapError(xr);
    }
    return Result::Ok;
}

Result OpenXrBackend::CreateSwapchain(uint32_t w, uint32_t h, DXGI_FORMAT fmt, SwapImages* out) {
    *out = SwapImages{};
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = static_cast<int64_t>(fmt);
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrSwapchain handle = XR_NULL_HANDLE;
    const XrResult xr = xrCreateSwapchain(session_, &ci, &handle);
    if (XR_FAILED(xr)) {
        log_.Error("xrCreateSwapchain({}x{} {}) failed: {}", w, h, DxgiFormatName(fmt), Name(xr));
        return MapError(xr) == Result::SessionLost ? Result::SessionLost : Result::Error;
    }
    out->xr = reinterpret_cast<uint64_t>(handle);
    uint32_t n = 0;
    std::vector<XrSwapchainImageD3D11KHR> imgs;
    bool ok = Check(xrEnumerateSwapchainImages(handle, 0, &n, nullptr), "xrEnumerateSwapchainImages");
    if (ok) {
        imgs.assign(n, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        ok = Check(xrEnumerateSwapchainImages(handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())),
                   "xrEnumerateSwapchainImages");
    }
    if (!ok || n == 0) {
        DestroySwapchain(*out);
        return Result::Error;
    }
    for (auto& i : imgs) out->images.push_back(i.texture);
    out->width = w;
    out->height = h;
    out->format = fmt;
    return Result::Ok;
}

void OpenXrBackend::DestroySwapchain(SwapImages& sc) {
    for (ID3D11Texture2D* t : sc.images) blitter_.Forget(t);
    if (sc.xr) xrDestroySwapchain(Handle(sc));
    sc = SwapImages{};
}

OpenXrBackend::ImageWait OpenXrBackend::AcquireAndWait(SwapImages& sc, uint32_t* index) {
    if (!sc.waitPending) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t idx = 0;
        if (!Check(xrAcquireSwapchainImage(Handle(sc), &ai, &idx), "xrAcquireSwapchainImage")) return ImageWait::Failed;
        sc.acquiredIndex = idx;
        sc.waitPending = true;
    }
    // A wait that times out leaves the image acquired: wait again for the same
    // image (it may neither be released unwaited nor skipped by acquiring the next).
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100'000'000;  // 100 ms per attempt
    for (int attempt = 0; attempt < 3; ++attempt) {
        const XrResult wr = xrWaitSwapchainImage(Handle(sc), &wi);
        if (wr == XR_SUCCESS) {
            sc.waitPending = false;
            *index = sc.acquiredIndex;
            return ImageWait::Ready;
        }
        if (wr != XR_TIMEOUT_EXPIRED) {
            log_.Error("xrWaitSwapchainImage: {}", Name(wr));
            return ImageWait::Failed;
        }
        CountStat(&FrameStats::imageWaitTimeouts);
    }
    log_.Warn("xrWaitSwapchainImage: image {} still not ready after 300 ms; waiting for it again next frame", sc.acquiredIndex);
    return ImageWait::NotReady;
}

template <class F>
OpenXrBackend::ImageWait OpenXrBackend::UpdateImage(SwapImages& sc, F&& transfer) {
    uint32_t idx = 0;
    const int64_t t0 = QpcNowNs();
    const ImageWait w = AcquireAndWait(sc, &idx);
    AddMs(&FrameStats::acquireWaitMs, t0);
    if (w != ImageWait::Ready) return w;
    uint32_t outW = 0, outH = 0;
    const EyeTarget t{sc.images[idx], sc.format, sc.width, sc.height, 0};
    const bool transferred = transfer(t, &outW, &outH);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const int64_t t1 = QpcNowNs();
    const bool released = Check(xrReleaseSwapchainImage(Handle(sc), &ri), "xrReleaseSwapchainImage");
    AddMs(&FrameStats::releaseMs, t1);
    if (!transferred) {
        // The newest released image now holds undefined content: stop showing it until a good copy.
        if (released) sc.hasImage = false;
        return ImageWait::Failed;
    }
    if (!released) return ImageWait::Failed;
    sc.hasImage = true;
    sc.lastIndex = idx;
    sc.lastW = outW;
    sc.lastH = outH;
    return ImageWait::Ready;
}

Result OpenXrBackend::SubmitFrame(uint64_t frameId, const SubmitDesc& desc) {
    if (!initialized_) return Result::NotInitialized;
    std::lock_guard sl(sessionMutex_);
    FrameRecord rec;
    {
        std::lock_guard fl(frameMutex_);
        FrameRecord* r = FindFrameLocked(frameId);
        if (!r || r->epoch != epoch_ || !sessionRunning_) return r ? Result::Ok : StaleOrUnknown(frameId);
        if (r->ended) {
            log_.Error("SubmitFrame({}): frame already ended", frameId);
            return Result::CallOrder;
        }
        if (!r->begun) {
            if (const Result br = BeginLocked(*r); br != Result::Ok) return br;
        }
        r->ended = true;
        rec = *r;
    }
    if (!ValidateSubmit(desc)) {
        EndFrameLocked(rec.displayTime, nullptr, 0);
        CountStat(&FrameStats::framesSkipped);
        return Result::InvalidArgument;
    }

    XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quads[kMaxQuadLayers]{};
    const XrCompositionLayerBaseHeader* layers[1 + kMaxQuadLayers]{};
    uint32_t layerCount = 0;
    bool ok = true;
    const bool render = rec.shouldRender && (desc.texture != nullptr || desc.quadCount > 0);
    if (render) {
        ScopedStateBackup backup(stateBackup_, context_.Get());
        capture_.BeginFrame(frameId);
        GpuFrameBegin();
        LayerImage captureImages[2]{};

        // ---- projection layer ----
        if (desc.texture) {
            bool eyesOk = true;
            for (int e = 0; e < 2; ++e) {
                SwapImages& sc = eyes_[e];
                if (desc.eyes[e].update || !sc.hasImage) {
                    const ImageWait w = UpdateImage(sc, [&](const EyeTarget& t, uint32_t* ow, uint32_t* oh) {
                        return TransferEye(static_cast<Eye>(e), desc, t, ow, oh);
                    });
                    if (w == ImageWait::Ready) sc.lastView = SubmittedView(rec, desc, e);
                    if (w == ImageWait::Failed) ok = false;
                }
                // Not updated (alternate-eye mode, or the image was not ready yet): show the last image.
                if (!sc.hasImage) {
                    eyesOk = false;
                    continue;
                }
                pv[e].pose = ToXr(sc.lastView.pose);
                pv[e].fov = ToXr(sc.lastView.fov);
                pv[e].subImage.swapchain = Handle(sc);
                pv[e].subImage.imageRect.offset = {0, 0};
                pv[e].subImage.imageRect.extent = {static_cast<int32_t>(sc.lastW), static_cast<int32_t>(sc.lastH)};
                pv[e].subImage.imageArrayIndex = 0;
                captureImages[e] = sc.Last();
            }
            // A projection layer needs valid tracking for its poses (or poses given by the host).
            const bool posesKnown = rec.orientationValid || (desc.eyes[0].viewOverride && desc.eyes[1].viewOverride);
            if (eyesOk && posesKnown) {
                projection.space = localSpace_;
                projection.viewCount = 2;
                projection.views = pv;
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            } else {
                captureImages[0] = captureImages[1] = LayerImage{};
            }
        }

        // ---- quad layers ----
        for (uint32_t i = 0; i < desc.quadCount; ++i) {
            const QuadLayer& q = desc.quads[i];
            QuadSlot* slot = FindQuad(q.layer);
            if (!slot) continue;
            if (q.texture &&
                UpdateImage(slot->sc, [&](const EyeTarget& t, uint32_t* ow, uint32_t* oh) { return TransferQuad(q, t, ow, oh); }) ==
                    ImageWait::Failed)
                ok = false;
            if (!slot->sc.hasImage) continue;
            XrCompositionLayerQuad& l = quads[i];
            l.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            l.next = nullptr;
            l.layerFlags = q.alphaBlend ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            l.space = q.space == LayerSpace::Head ? viewSpace_ : localSpace_;
            l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            l.subImage.swapchain = Handle(slot->sc);
            l.subImage.imageRect.offset = {0, 0};
            l.subImage.imageRect.extent = {static_cast<int32_t>(slot->sc.lastW), static_cast<int32_t>(slot->sc.lastH)};
            l.subImage.imageArrayIndex = 0;
            l.pose = ToXr(q.space == LayerSpace::Head ? q.pose : PoseMultiply(rec.recenter, q.pose));
            l.size = XrExtent2Df{q.width, q.height};
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&l);
        }

        GpuFrameEnd();
        CaptureComposited(rec, desc, captureImages, eyes_[0].width, eyes_[0].height);
        capture_.EndFrame();
    }

    const Result er = EndFrameLocked(rec.displayTime, layerCount ? layers : nullptr, layerCount);
    if (er != Result::Ok) return er;
    CountStat(render ? &FrameStats::framesSubmitted : &FrameStats::framesSkipped);
    return ok ? Result::Ok : Result::Error;
}

Result OpenXrBackend::CreateQuadLayer(const QuadLayerCreateDesc& desc, LayerHandle* out) {
    if (out) *out = 0;
    if (!initialized_) return Result::NotInitialized;
    if (!out || desc.width == 0 || desc.height == 0) return Result::InvalidArgument;
    std::vector<DXGI_FORMAT> offered;
    uint32_t maxW = 0, maxH = 0;
    {
        std::lock_guard lk(infoMutex_);
        offered = info_.runtimeFormats;
        maxW = info_.maxSwapchainWidth;
        maxH = info_.maxSwapchainHeight;
    }
    if ((maxW && desc.width > maxW) || (maxH && desc.height > maxH)) {
        log_.Error("quad layer {}x{} exceeds the runtime maximum {}x{}", desc.width, desc.height, maxW, maxH);
        return Result::InvalidArgument;
    }
    const DXGI_FORMAT fmt = PickLayerFormat(desc.sourceFormatHint, offered, kPreferredFormats, std::size(kPreferredFormats));
    {
        std::lock_guard lk(quadMutex_);
        if (FreeQuadSlotLocked() < 0) {
            log_.Error("no free quad layer slot (max {})", kMaxQuadLayers);
            return Result::Error;
        }
    }
    SwapImages sc;
    {
        std::lock_guard sl(sessionMutex_);  // must not race xrEndSession on the other thread
        if (const Result r = CreateSwapchain(desc.width, desc.height, fmt, &sc); r != Result::Ok) return r;
    }
    std::lock_guard lk(quadMutex_);
    const int idx = FreeQuadSlotLocked();
    if (idx < 0) {
        DestroySwapchain(sc);
        return Result::Error;
    }
    QuadSlot& s = quads_[idx];
    s = QuadSlot{};
    s.used = true;
    s.sc = std::move(sc);
    *out = MakeHandle(idx);
    log_.Info("quad layer 0x{:X}: {}x{} {} ({} images)", *out, desc.width, desc.height, DxgiFormatName(fmt), s.sc.images.size());
    return Result::Ok;
}

void OpenXrBackend::DestroyQuadLayer(LayerHandle layer) {
    std::lock_guard lk(quadMutex_);
    QuadSlot* s = FindQuad(layer);
    if (!s) return;
    DestroySwapchain(s->sc);
    *s = QuadSlot{};
}

Result OpenXrBackend::SkipFrame(uint64_t frameId) {
    if (!initialized_) return Result::NotInitialized;
    std::lock_guard sl(sessionMutex_);
    int64_t t = 0;
    {
        std::lock_guard fl(frameMutex_);
        FrameRecord* r = FindFrameLocked(frameId);
        if (!r || r->epoch != epoch_ || !sessionRunning_) return r ? Result::Ok : StaleOrUnknown(frameId);
        if (r->ended) return Result::CallOrder;
        if (!r->begun) {
            if (const Result br = BeginLocked(*r); br != Result::Ok) return br;
        }
        r->ended = true;
        t = r->displayTime;
    }
    const Result er = EndFrameLocked(t, nullptr, 0);
    if (er == Result::Ok) CountStat(&FrameStats::framesSkipped);
    return er;
}

}  // namespace

std::unique_ptr<IXrBackend> CreateOpenXrBackend() { return std::make_unique<OpenXrBackend>(); }

}  // namespace ff7vr::xr
