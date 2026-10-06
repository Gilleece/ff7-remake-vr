// ff7vr XR layer: backend interface.
//
// A static library that talks to the VR runtime. It knows nothing about Unreal
// Engine. Two backends implement the same interface:
//   * OpenXR (D3D11, XR_KHR_D3D11_enable), loader linked statically.
//   * Null: no runtime. Fixed resolution/FOV, scripted head motion, PNG capture.
//
// See xr_math.h for coordinate conventions (right-handed, Y up, -Z forward,
// metres) and helpers that convert to Unreal's space.
//
// ============================ THREADING ====================================
// Designed for an engine with a game thread (GT) and a render thread (RT) that
// owns the D3D11 immediate context. Per frame:
//
//   GT: WaitFrame(info)            -> blocks for frame pacing (xrWaitFrame), polls
//                                     runtime events, returns poses for the
//                                     predicted display time and a frameId.
//   RT: BeginFrame(info.frameId)   -> xrBeginFrame.
//   RT: (optional) RelocateViews(frameId, views)   late pose update.
//   RT: render both eyes into one texture (side by side or any layout).
//   RT: SubmitFrame(frameId, desc) -> copy/blit into runtime swapchains, xrEndFrame.
//
// Rules:
//   1. WaitFrame is called from ONE thread (normally GT). Not reentrant.
//   2. BeginFrame/SubmitFrame/SkipFrame/RelocateViews are called from ONE thread
//      (normally RT), and that thread must be the only user of the device's
//      immediate context while they run (they use it, and so may the runtime).
//      They may run concurrently with WaitFrame on the other thread.
//   3. Every frameId returned with info.sessionRunning == true MUST eventually
//      get BeginFrame + (SubmitFrame or SkipFrame), in increasing frameId order.
//      OpenXR blocks the next xrWaitFrame until the previous frame was begun,
//      so a missing BeginFrame deadlocks the game thread. If the engine drops
//      a frame, call SkipFrame(frameId) for it (from the RT).
//   4. The engine may run one frame ahead: WaitFrame(N+1) may be called before
//      SubmitFrame(N). Up to kMaxFramesInFlight frames may be outstanding.
//   5. Init and Shutdown: call with no other call in progress on any thread.
//   6. Recenter/ResetRecenter/RequestCapture/GetState/GetRuntimeInfo: any thread.
//   7. frameIds of frames that became stale (session stopped/restarted between
//      WaitFrame and SubmitFrame) are accepted and ignored (Result::Ok).
//
// D3D11 state: BeginFrame, SubmitFrame and SkipFrame save the immediate
// context's pipeline state on entry and restore it on exit (covering both our
// own draws and anything the runtime does on the context inside
// xrBeginFrame/xrEndFrame). Saved: IA (layout, topology, all vertex buffers,
// index buffer), VS/PS/GS/HS/DS shaders with constant buffers, SRVs and
// samplers in slots 0-1 (the only slots this library binds), RS state,
// viewports and scissors, OM render targets, depth view, UAVs, blend and
// depth-stencil state, and predication. Compute shader state is never touched.
//
// ============================ SWAPCHAINS ===================================
// The OpenXR backend creates one swapchain per eye and submits one projection
// layer whose two views each use their own swapchain at imageRect (0,0,w,h).
// Chosen over one double-wide swapchain with a sub-image per eye because:
//   * alternate-eye mode is free: an eye that is not updated simply keeps its
//     last released image (no copy of the old half into a new image);
//   * it is what every runtime supports best (per-view swapchains are the
//     common path in SteamVR, VDXR, Oculus);
//   * the cost difference is one extra CopySubresourceRegion per frame.
// A depth layer (XR_KHR_composition_layer_depth) will add one depth
// swapchain per eye chained to the same views.
//
// ============================ QUAD LAYERS ==================================
// A quad layer is a flat rectangle placed in space (a virtual screen, a HUD
// panel). Each one owns its own runtime swapchain, created with
// CreateQuadLayer. Per frame, SubmitDesc::quads lists the quads to show, in
// back-to-front order after the projection layer. A quad entry either brings
// new content (a texture region that is copied into the layer's swapchain)
// or none, in which case the runtime keeps showing the last image copied in.
// The runtime re-projects quads with the newest head pose on every display
// refresh, so a quad stays steady even when the host submits slowly.
// A frame may contain only quads (SubmitDesc::texture == nullptr): the
// projection layer is then omitted and the runtime shows the quads on black.
//
// The Null backend composites the layers itself (projection image stretched
// over each eye's field of view, then each quad drawn with perspective using
// the frame's eye views), so its PNG captures show what the user would see.
// The OpenXR backend does the same for captures only.
//
// ============================ POSE VALIDITY ================================
// xrLocateViews (viewStateFlags) and xrLocateSpace (locationFlags) say per
// frame whether orientation and position are valid; the values of an invalid
// part are undefined. Both backends pass every frame's poses through one
// filter (BackendBase::SanitizePoses) so the host never sees NaN or zeros:
//   * orientation invalid (views or head), or any value not finite: the last
//     views and head that had a valid orientation are repeated; the recenter
//     basis is not updated;
//   * orientation valid, position invalid (3DoF): the runtime's orientation,
//     the head at the last fully tracked position, each eye at its last known
//     offset from the head (half the IPD if none was seen yet);
//   * the filter's state changes are logged once each, not per frame.
// The projection layer is submitted only once usable poses exist (a held pose
// counts: the image is shown where it was rendered).
// ===========================================================================
#pragma once

#include "ff7vr/xr/xr_math.h"

#include <dxgiformat.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ff7vr::xr {

constexpr uint32_t kMaxFramesInFlight = 4;

// Notice: an informational line the host should show even while it quiets the
// messages of repeated failed attempts (the runtime choice, one line per probe round).
enum class LogLevel { Debug, Info, Warn, Error, Notice };

// Host-provided log sink. May be called from any thread, concurrently.
using LogCallback = std::function<void(LogLevel level, std::string_view message)>;

enum class BackendType { Null, OpenXR };

enum class Result {
    Ok = 0,
    NotInitialized,
    InvalidArgument,
    CallOrder,          // e.g. SubmitFrame for a frame that was never begun
    RuntimeUnavailable, // no runtime / runtime JSON missing / instance creation failed
    SystemUnavailable,  // runtime OK but no headset right now (retry Init later)
    GraphicsMismatch,   // device on wrong adapter / feature level too low
    SessionLost,        // runtime lost the session; Shutdown + Init to recover
    Error,
};
const char* ToString(Result r);

enum class Eye : uint32_t { Left = 0, Right = 1 };

// High-level session state for the host.
enum class SessionState {
    Uninitialized,  // before Init / after Shutdown
    Idle,           // session exists, runtime not ready (headset idle, app not focused in runtime)
    Running,        // frame loop active, runtime not showing our frames (SYNCHRONIZED)
    Visible,        // our frames are visible, input not focused
    Focused,        // visible and focused
    Stopping,       // runtime asked to stop; library ends the session itself
    Lost,           // session/instance lost: Shutdown, then Init again later
    ExitRequested,  // runtime wants the app to exit (XR_SESSION_STATE_EXITING); Shutdown
};
const char* ToString(SessionState s);

struct View {
    Pose pose{};  // eye pose in tracking space (after library recenter)
    Fov fov{};
};

// Head motion script for the Null backend. Deterministic: a function of frameId only.
enum class NullMotion {
    Static,    // identity head pose
    YawSweep,  // yaw +-30 deg sine, 8 s period
    Sway,      // position sway: x +-3 cm (4 s), y +-2 cm (3 s), z +-1 cm (5 s)
    YawAndSway,
};

struct NullOptions {
    // Quest 3 class defaults (2064x2208 panel per eye; Meta default render target is a bit lower).
    uint32_t eyeWidth = 2064;
    uint32_t eyeHeight = 2208;
    float refreshHz = 90.0f;
    // Quest-3-like asymmetric FOV, left eye (radians). Right eye mirrors left/right.
    Fov fovLeftEye{-52.0f * kDegToRad, 42.0f * kDegToRad, 46.0f * kDegToRad, -50.0f * kDegToRad};
    float ipdMetres = 0.064f;
    NullMotion motion = NullMotion::Static;
    bool paceToRefresh = false;  // WaitFrame sleeps to emulate vsync at refreshHz
    // Report a hidden area (GetHiddenAreaMesh): the image corners outside an ellipse
    // around the view axis, like a headset's visibility mask.
    bool hiddenArea = true;
    // Format of the Null backend's emulated swapchain images.
    DXGI_FORMAT swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
};

struct InitDesc {
    BackendType backend = BackendType::OpenXR;
    ID3D11Device* device = nullptr;  // required; the library AddRefs it
    LogCallback log;                 // optional (nullptr = silent)

    // OpenXR runtime selection, applied per process before the loader creates
    // the instance (see ApplyRuntimeSelection). "auto" (default) = probe the
    // runtimes found on the machine and take the first one that has a headset
    // (see EnumerateRuntimeCandidates). "" or "inherit" = leave the process
    // environment as is. The machine's default runtime is never changed.
    std::string runtime = "auto";
    std::string appName = "ff7-remake-vr";
    uint32_t appVersion = 1;

    // Per-eye swapchain size. 0 = runtime recommended size * resolutionScale.
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;
    float resolutionScale = 1.0f;

    // Preferred swapchain format. UNKNOWN = automatic: first of
    // R8G8B8A8_UNORM_SRGB, B8G8R8A8_UNORM_SRGB, R16G16B16A16_FLOAT,
    // R10G10B10A2_UNORM, R8G8B8A8_UNORM, B8G8R8A8_UNORM offered by the runtime.
    DXGI_FORMAT swapchainFormat = DXGI_FORMAT_UNKNOWN;

    // Request XR_KHR_composition_layer_depth if available (not used yet; reserved
    // so a depth layer can be added without API change).
    bool requestDepthExtension = true;
    // Enable XR_EXT_debug_utils messages from the runtime/loader into the log (if offered).
    bool enableDebugUtils = false;
    // Disable every implicit OpenXR API layer registered on the machine (OpenXR
    // Toolkit, ReShade, vendor compatibility layers...) for this process only,
    // by setting each layer's own disable_environment variable before the
    // instance is created. The layers found are listed in RuntimeInfo either way.
    bool disableImplicitApiLayers = false;
    // Disable only the implicit layers whose name or manifest path contains one
    // of these strings (case-insensitive), e.g. "reshade". Same mechanism.
    std::vector<std::string> disableImplicitApiLayersMatching;
    // Measure the GPU time of the library's own copies with timestamp queries
    // (read with TakeGpuCopyTimes). Costs a few queries per frame.
    bool gpuTiming = false;

    NullOptions null;
};

struct SwapchainInfo {
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t imageCount = 0;
};

struct RuntimeInfo {
    BackendType backend = BackendType::Null;
    std::string runtimeName;      // e.g. "SteamVR/OpenXR", "VirtualDesktopXR", "Null"
    std::string runtimeVersion;   // "major.minor.patch"
    std::string runtimeJson;      // resolved runtime JSON (OpenXR), "" = system default
    std::string systemName;       // XrSystemProperties::systemName
    uint32_t vendorId = 0;
    uint32_t maxSwapchainWidth = 0, maxSwapchainHeight = 0;
    uint32_t recommendedWidth[2]{}, recommendedHeight[2]{};  // per eye
    uint32_t maxWidth[2]{}, maxHeight[2]{};
    float refreshHz = 0;          // current refresh rate (XR_FB_display_refresh_rate), else estimated from frame period, 0 = unknown
    std::vector<float> availableRefreshHz;
    std::vector<DXGI_FORMAT> runtimeFormats;  // swapchain formats offered by the runtime, runtime preference order
    SwapchainInfo eyeSwapchain[2];
    bool orientationTracking = false, positionTracking = false;
    bool depthLayerSupported = false;  // XR_KHR_composition_layer_depth enabled
    std::vector<std::string> enabledExtensions;
    std::vector<std::string> apiLayers;       // API layers the loader reports (implicit ones load automatically)
    std::vector<std::string> implicitLayers;  // implicit layer manifests in the registry, "<json> (enabled|disabled|disabled for this process)"
    uint64_t adapterLuid = 0;                 // adapter required by the runtime (LowPart | HighPart<<32)
    int64_t lastPredictedDisplayPeriod = 0;   // nanoseconds, from the last WaitFrame
};

struct FrameInfo {
    uint64_t frameId = 0;           // 0 when sessionRunning is false
    bool sessionRunning = false;    // false: no XR frame this time; do not call Begin/Submit
    bool shouldRender = false;      // runtime wants pixels; if false still Begin + Submit (or SkipFrame)
    int64_t predictedDisplayTime = 0;   // runtime clock, nanoseconds (XrTime)
    int64_t predictedDisplayPeriod = 0; // nanoseconds
    // Tracking this frame (see POSE VALIDITY below). The views and head are always
    // finite and usable, whatever these say:
    //   orientationValid && positionValid: as the runtime located them;
    //   orientationValid only (3DoF): the runtime's orientation, the position held at
    //       the last fully tracked head position (or the origin if there was none);
    //   neither: the last views and head that had a valid orientation, held.
    bool orientationValid = false;
    bool positionValid = false;
    View views[2]{};                // [0] left, [1] right; tracking space after recenter
    Pose head{};                    // head (VIEW space origin) in tracking space after recenter
    SessionState state = SessionState::Uninitialized;
};

struct Rect {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

// How the source texture's values are encoded.
enum class ColorEncoding {
    Srgb,    // gamma (sRGB) encoded values in a UNORM format: what a game back buffer holds
    Linear,  // linear light (scene colour, FP16, or a texture read through an _SRGB view)
};

struct EyeSubmit {
    // Region of the source texture holding this eye, in pixels of the selected
    // mip, origin top-left. width or height 0 = the left (Eye::Left) or right
    // half of the texture. If the region is larger than the eye swapchain it is
    // scaled down to fit (aspect preserved); otherwise it is copied 1:1 and the
    // layer's imageRect is the region's size.
    Rect rect{};
    // false: alternate-eye mode. The eye keeps showing its last submitted image,
    // re-projected by the runtime with the pose it was rendered with. If the eye
    // was never submitted, it is updated anyway.
    bool update = true;
    // Pose/FOV the image was actually rendered with (tracking space after
    // recenter, like FrameInfo::views). null = the frame's views (or the views
    // from RelocateViews).
    const View* viewOverride = nullptr;
};

// ---- quad layers (see QUAD LAYERS above) ----
using LayerHandle = uint32_t;  // 0 = none
constexpr uint32_t kMaxQuadLayers = 8;

struct QuadLayerCreateDesc {
    uint32_t width = 0, height = 0;  // image size in pixels
    // Format of the textures that will be copied in. The backend picks that
    // format's sRGB variant when the runtime offers it (plain copy, no
    // conversion), otherwise the same automatic list as the eye swapchains.
    DXGI_FORMAT sourceFormatHint = DXGI_FORMAT_UNKNOWN;
};

enum class LayerSpace {
    World,  // tracking space after recenter, like FrameInfo::views: the quad stays put in the room
    Head,   // relative to the head (OpenXR VIEW space): the quad follows the head
};

// How a blended quad's source texture stores transparency. A quad layer's
// image always holds premultiplied alpha (what OpenXR assumes when
// XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT is not set, and what the Null
// compositor blends with); other conventions are converted when the content is
// copied in (shader blit instead of a plain copy).
enum class SourceAlpha {
    Premultiplied,          // rgb already multiplied by alpha, alpha = coverage
    Straight,               // rgb not multiplied, alpha = coverage
    PremultipliedInverted,  // rgb premultiplied, alpha = 1 - coverage (Unreal's convention for
                            // translucency and UI targets: composite = background * a + rgb)
};

struct QuadLayer {
    LayerHandle layer = 0;
    // New content: this region is copied into the layer's swapchain (scaled
    // down, aspect preserved, if larger than the layer image). null = keep
    // the last image (nothing is copied).
    ID3D11Texture2D* texture = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // as SubmitDesc::viewFormat
    ColorEncoding encoding = ColorEncoding::Srgb;
    uint32_t arraySlice = 0;
    uint32_t mipLevel = 0;
    Rect rect{};  // width or height 0 = the whole texture
    LayerSpace space = LayerSpace::World;
    Pose pose{};                        // centre of the quad; the image faces +Z of this pose
    float width = 1.0f, height = 1.0f;  // metres
    bool alphaBlend = false;            // false: opaque; true: blend over the layers below with the image's alpha
    SourceAlpha sourceAlpha = SourceAlpha::Premultiplied;  // with alphaBlend: how `texture` stores alpha
};

struct SubmitDesc {
    // Projection layer source. null: no projection layer this frame (quads only,
    // or an empty frame when there are no quads either).
    ID3D11Texture2D* texture = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;  // format to read the texture as (required if typeless); UNKNOWN = texture format
    ColorEncoding encoding = ColorEncoding::Srgb;
    uint32_t arraySlice = 0;
    uint32_t mipLevel = 0;
    EyeSubmit eyes[2]{};
    // Quad layers drawn over the projection layer, back to front.
    const QuadLayer* quads = nullptr;
    uint32_t quadCount = 0;
};

// The part of an eye's image the headset cannot show (lens edges, display corners),
// from XR_KHR_visibility_mask: a triangle mesh covering the hidden area. Vertices are
// tangents of the angles from the eye's view axis on the plane z = -1 (x right, y up),
// the same units as Fov tangents, so they map onto the image through the eye's FOV.
struct HiddenAreaMesh {
    std::vector<float> xy;          // vertex positions, x0 y0 x1 y1 ...
    std::vector<uint32_t> indices;  // three per triangle
};

struct CaptureRequest {
    std::string pathPrefix;  // writes <pathPrefix>_L.png and <pathPrefix>_R.png (UTF-8 path)
};

struct CaptureResult {
    uint64_t frameId = 0;
    bool ok = false;
    std::string files[2];
    std::string error;
};

struct FrameStats {
    uint64_t framesWaited = 0, framesBegun = 0, framesSubmitted = 0, framesSkipped = 0;
    uint64_t framesNotRendered = 0;   // shouldRender == false
    uint64_t framesDiscarded = 0;     // xrBeginFrame returned XR_FRAME_DISCARDED
    uint64_t copyPath = 0, blitPath = 0;  // per-image submission paths used (eyes and quads)
    uint64_t quadUpdates = 0;             // quad layer images copied in
    uint64_t imageWaitTimeouts = 0;       // xrWaitSwapchainImage timed out (image kept for the next frame)
    // Cumulative CPU time spent inside runtime calls on the submitting thread (OpenXR), milliseconds.
    double acquireWaitMs = 0;  // xrAcquireSwapchainImage + xrWaitSwapchainImage
    double releaseMs = 0;      // xrReleaseSwapchainImage
    double beginFrameMs = 0;   // xrBeginFrame
    double endFrameMs = 0;     // xrEndFrame
};

class IXrBackend {
public:
    virtual ~IXrBackend() = default;

    virtual Result Init(const InitDesc& desc) = 0;
    virtual void Shutdown() = 0;
    virtual BackendType Type() const = 0;

    // Snapshot of what the runtime reported. Valid after a successful Init.
    virtual RuntimeInfo GetRuntimeInfo() const = 0;
    virtual SessionState GetState() const = 0;

    // GT. See threading rules above. Returns Ok with info.sessionRunning=false
    // while the session is idle (host keeps rendering flat). Returns
    // SessionLost / Error when the host must Shutdown.
    virtual Result WaitFrame(FrameInfo& info) = 0;

    // RT.
    virtual Result BeginFrame(uint64_t frameId) = 0;
    // RT, optional: re-locate the eyes for frameId's display time (late latching).
    // The returned views replace the frame's views for submission.
    virtual Result RelocateViews(uint64_t frameId, View outViews[2]) = 0;
    // RT. Copies the eye regions into the runtime swapchains and ends the frame.
    virtual Result SubmitFrame(uint64_t frameId, const SubmitDesc& desc) = 0;
    // RT. Ends the frame with no layers (begins it first if needed).
    virtual Result SkipFrame(uint64_t frameId) = 0;

    // RT (same thread rules as SubmitFrame). Creates a quad layer and its
    // swapchain. Handles stay valid until DestroyQuadLayer or Shutdown; after
    // a new Init, create the layers again. At most kMaxQuadLayers.
    virtual Result CreateQuadLayer(const QuadLayerCreateDesc& desc, LayerHandle* out) = 0;
    // RT. Unknown or stale handles are ignored.
    virtual void DestroyQuadLayer(LayerHandle layer) = 0;
    // Any thread. Size and format of a layer's image; false if the handle is not valid.
    virtual bool GetQuadLayerInfo(LayerHandle layer, SwapchainInfo* out) const = 0;

    // RT (same thread rules as SubmitFrame). Draws q.texture's q.rect over `target` at
    // `targetRect`, stretched, blended with its alpha (q.sourceAlpha) the way a game draws
    // its UI: for a desktop mirror of what the headset shows. targetEncoding: how the
    // target stores colour when targetFormat is not an _SRGB format (a game back buffer:
    // Srgb). Saves and restores the context state. Only q.texture, viewFormat, encoding,
    // rect and sourceAlpha are used.
    virtual bool DrawOverlay(const QuadLayer& q, ID3D11Texture2D* target, DXGI_FORMAT targetFormat, ColorEncoding targetEncoding,
                             const Rect& targetRect) = 0;

    // Any thread. Recenter: make the current head yaw and position the new
    // origin (pitch/roll untouched). Applied from the next WaitFrame with a
    // valid head orientation.
    //
    // A recenter by the runtime itself (the headset's own recenter: the LOCAL
    // space's origin moves to the user's leveled head, announced with
    // XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) clears this library's
    // recenter from the first frame whose display time reaches the event's
    // changeTime: the new LOCAL origin already is where the user faces, so the
    // offset taken against the old origin would no longer fit.
    virtual void Recenter() = 0;
    virtual void ResetRecenter() = 0;

    // Any thread. Development aid: drives the Null backend's emulated headset
    // (head pose, runtime recenter events, tracking loss); see the Null backend for
    // the commands. Returns "ok ..." or "err ...". Other backends do not support it.
    virtual std::string Simulate(std::string_view command) {
        (void)command;
        return "err only the Null backend simulates headset events";
    }

    // Any thread. Captures what each eye would see in the next submitted frame
    // (projection layer and quad layers composited with that frame's views),
    // converted to 8-bit sRGB PNG on a worker thread.
    virtual void RequestCapture(const CaptureRequest& req) = 0;
    // Blocks until all requested captures were written (or timeout). Returns results since last call.
    virtual std::vector<CaptureResult> WaitForCaptures(uint32_t timeoutMs) = 0;

    virtual FrameStats GetStats() const = 0;

    // Any thread. The hidden area of an eye's image (XR_KHR_visibility_mask; the Null
    // backend emulates one). False when the runtime offers none (or an empty mesh).
    // The version changes whenever the runtime reports a new mask.
    virtual bool GetHiddenAreaMesh(Eye eye, HiddenAreaMesh* out) const = 0;
    virtual uint32_t HiddenAreaMeshVersion() const = 0;

    // Any thread. With InitDesc::gpuTiming: GPU milliseconds of the copies into
    // the swapchains, one value per submitted frame, collected since the last call
    // (results arrive a few frames late). Excludes what the runtime itself does
    // on the context inside xrEndFrame/xrReleaseSwapchainImage.
    virtual std::vector<float> TakeGpuCopyTimes() = 0;
};

std::unique_ptr<IXrBackend> CreateBackend(BackendType type);

// ---- runtime selection (OpenXR) ----
// selection: "system"         -> the machine's default runtime (registry ActiveRuntime);
//                                removes XR_RUNTIME_JSON from this process
//            "steamvr"        -> SteamVR's steamxr_win64.json (found via openvrpaths.vrpath,
//                                the Steam install, or the AvailableRuntimes registry list)
//            "virtualdesktop" | "vdxr" -> Virtual Desktop's virtualdesktop-openxr.json
//            "<path>.json"    -> that file
//            "" | "inherit"   -> leave the process environment untouched
// ApplyRuntimeSelection sets XR_RUNTIME_JSON in this process only (Win32 and
// CRT environment). The OpenXR backend additionally passes the same path to
// the loader as a loader property (XR_EXT_loader_init_properties), which also
// works in an elevated process where the loader ignores environment variables.
// Never touches the registry. Returns the resolved JSON path ("" for system)
// or an error message in *error.
//            "auto"           -> not a single runtime: see EnumerateRuntimeCandidates;
//                                ResolveRuntimeJson rejects it, IsAutoRuntimeSelection tells
bool ResolveRuntimeJson(std::string_view selection, std::string* outPath, std::string* error);
bool ApplyRuntimeSelection(std::string_view selection, std::string* outPath, std::string* error);
bool IsAutoRuntimeSelection(std::string_view selection);  // "auto", "any"

// ---- automatic runtime choice ([xr] runtime = auto) ----
// A runtime that could drive the headset, found in the registry or at a known
// install path. Never written anywhere; reading only.
struct RuntimeCandidate {
    std::string name;            // "Virtual Desktop", "SteamVR", ... or the manifest's runtime name
    std::string manifest;        // absolute path of the runtime JSON
    std::string origin;          // "active runtime", "registered", "install folder"
    bool active = false;         // the machine's default (registry ActiveRuntime)
    std::string runningProcess;  // the runtime's server/streamer process seen running ("" = none)
    std::string processesLookedFor;  // the process names that count as running ("" = none known)
    // Non-empty: probed only while its process runs (see ShouldProbeRuntime), because
    // loading it would start its server (SteamVR, Oculus, Windows Mixed Reality) or
    // because its answer does not tell whether a headset is there (PICO).
    std::string needsRunningBecause;
    bool probeWhenActive = false;  // ... unless it is the active runtime (then it is probed like any OpenXR game would)
};
// False with the reason when the candidate is to be skipped without loading it.
bool ShouldProbeRuntime(const RuntimeCandidate& c, std::string* skipReason);
// Order: runtimes whose server/streamer process is running, then the active
// runtime, then every other registered or installed one. One entry per manifest.
std::vector<RuntimeCandidate> EnumerateRuntimeCandidates();

// Loads one runtime in this process (XR_RUNTIME_JSON for this process plus the
// loader property; the loader unloads whatever runtime it had loaded, which it
// allows while no XrInstance exists), creates an instance with XR_KHR_D3D11_enable
// and asks for a head-mounted system. The instance is destroyed again, which
// unloads the runtime. Ok: a headset is there. SystemUnavailable: the runtime
// works but has no headset (XR_ERROR_FORM_FACTOR_UNAVAILABLE). RuntimeUnavailable:
// it did not load, lacks D3D11 or failed. Never call while an XrInstance exists.
struct RuntimeProbe {
    Result result = Result::RuntimeUnavailable;
    std::string reason;       // why it was rejected ("" when Ok)
    std::string runtimeName;  // XrInstanceProperties::runtimeName, when an instance was created
    std::string runtimeVersion;
    std::string systemName;   // when Ok
    double ms = 0;            // time the probe took
};
RuntimeProbe ProbeRuntime(const std::string& manifest);

// Implicit API layers registered for this user/machine (HKLM and HKCU).
struct ImplicitLayer {
    std::string manifest;            // JSON path
    std::string name;                // layer name from the manifest ("" if unreadable)
    std::string disableEnvironment;  // variable that disables it ("" if none)
    bool enabledInRegistry = false;
};
std::vector<ImplicitLayer> EnumerateImplicitApiLayers();
// Sets the disable_environment variable of every enabled implicit layer in
// this process. Returns how many layers were disabled (their manifests in *disabled).
int DisableImplicitApiLayers(std::vector<std::string>* disabled = nullptr);
// Same for the layers whose name or manifest path contains one of `patterns`
// (case-insensitive).
int DisableImplicitApiLayersMatching(const std::vector<std::string>& patterns, std::vector<std::string>* disabled = nullptr);

const char* DxgiFormatName(DXGI_FORMAT f);

// Development aid: reads back `texture` (array slice 0, mip 0) and writes it to
// a PNG with its alpha channel. 8-bit RGBA/BGRA families only (UNORM, _SRGB or
// TYPELESS); the stored bytes are written as they are (an _SRGB texture's bytes
// are sRGB encoded, so the PNG looks like what the texture shows). Blocks the
// calling thread until the GPU copy is done. The caller must own the immediate
// context. Returns false with a message in *error.
bool WriteTexturePng(ID3D11DeviceContext* ctx, ID3D11Texture2D* texture, const std::string& pathUtf8, std::string* error);

}  // namespace ff7vr::xr
