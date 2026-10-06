// Probing one OpenXR runtime in this process: does it load, does it support
// D3D11, does it have a headset right now. Used by [xr] runtime = auto.
//
// Switching runtimes inside one process: the loader (linked statically) keeps
// the runtime library it loaded until the last XrInstance is destroyed, or
// until xrInitializeLoaderKHR is called while no instance exists; that call
// unloads the library and replaces the loader properties, and the next call
// that needs a runtime reads XR_RUNTIME_JSON again (OpenXR-SDK src/loader:
// loader_init_data.cpp InitializeLoaderInitData, runtime_interface.cpp
// LoadRuntime, manifest_file.cpp FindManifestFiles). So every probe first
// points the loader at its manifest that way, and destroys its instance again.
#include "xr_common.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace ff7vr::xr {
namespace {

std::string ResultName(XrInstance instance, XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf))) return buf;
    switch (r) {
        case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
        default: return std::format("XrResult({})", static_cast<int>(r));
    }
}

}  // namespace

bool PointLoaderAtRuntime(const std::string& manifest, std::string* error) {
    // The environment variable for this process, and the same value as a loader
    // property (honoured even where the loader ignores the environment). The
    // property call also unloads a runtime library loaded earlier.
    std::string path;
    if (!ApplyRuntimeSelection(manifest.empty() ? std::string_view("system") : std::string_view(manifest), &path, error)) return false;
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initLoader))) &&
        initLoader) {
        XrLoaderInitPropertyValueEXT prop{"XR_RUNTIME_JSON", path.c_str()};
        XrLoaderInitInfoPropertiesEXT props{XR_TYPE_LOADER_INIT_INFO_PROPERTIES_EXT};
        props.propertyValueCount = path.empty() ? 0u : 1u;
        props.propertyValues = &prop;
        const XrResult r = initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&props));
        if (XR_FAILED(r)) {
            if (error) *error = "xrInitializeLoaderKHR: " + ResultName(XR_NULL_HANDLE, r);
            return false;
        }
    }
    return true;
}

RuntimeProbe ProbeRuntime(const std::string& manifest) {
    RuntimeProbe p;
    const auto t0 = std::chrono::steady_clock::now();
    auto done = [&](Result r, std::string reason) {
        p.result = r;
        p.reason = std::move(reason);
        p.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return p;
    };
    std::string err;
    if (!PointLoaderAtRuntime(manifest, &err)) return done(Result::RuntimeUnavailable, err);

    uint32_t n = 0;
    XrResult xr = xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    if (XR_FAILED(xr)) return done(Result::RuntimeUnavailable, "the runtime did not load (" + ResultName(XR_NULL_HANDLE, xr) + ")");
    std::vector<XrExtensionProperties> exts(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xr = xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    if (XR_FAILED(xr)) return done(Result::RuntimeUnavailable, "extension list failed (" + ResultName(XR_NULL_HANDLE, xr) + ")");
    bool d3d11 = false;
    for (const auto& e : exts) d3d11 = d3d11 || std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    if (!d3d11) return done(Result::RuntimeUnavailable, "the runtime does not support Direct3D 11");

    const char* enable[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ici.applicationInfo.applicationName, "ff7-remake-vr", _TRUNCATE);
    ici.applicationInfo.applicationVersion = 1;
    strncpy_s(ici.applicationInfo.engineName, "ff7vr", _TRUNCATE);
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enable;
    XrInstance instance = XR_NULL_HANDLE;
    xr = xrCreateInstance(&ici, &instance);
    if (XR_FAILED(xr)) return done(Result::RuntimeUnavailable, "xrCreateInstance failed (" + ResultName(XR_NULL_HANDLE, xr) + ")");

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip))) {
        p.runtimeName = ip.runtimeName;
        p.runtimeVersion = std::format("{}.{}.{}", XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                                       XR_VERSION_PATCH(ip.runtimeVersion));
    }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    xr = xrGetSystem(instance, &sgi, &system);
    Result result = Result::Ok;
    std::string reason;
    if (xr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        result = Result::SystemUnavailable;
        reason = "no headset connected (XR_ERROR_FORM_FACTOR_UNAVAILABLE)";
    } else if (XR_FAILED(xr)) {
        result = Result::SystemUnavailable;
        reason = "xrGetSystem failed (" + ResultName(instance, xr) + ")";
    } else {
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &sp))) p.systemName = sp.systemName;
    }
    xrDestroyInstance(instance);  // also unloads the runtime library
    return done(result, std::move(reason));
}

}  // namespace ff7vr::xr
