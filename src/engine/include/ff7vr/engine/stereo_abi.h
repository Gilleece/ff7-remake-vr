#pragma once
// Binary layouts of the UE 4.18 (Square Enix fork) stereo interfaces in ff7remake_.exe.
//
// Everything here was read from the game executable; see docs/re/engine.md for how each
// value was found and verified, and tools/re/signatures.json for the signature of every
// offset and address (entry names are given in the comments). The RVA constants are the
// values for file version 1.0.0.7 and serve only as a cross-check for the signature scan.
//
// The interfaces are declared as plain function-pointer tables with an explicit `self`
// and explicit hidden return pointers, so the ABI does not depend on how the compiler
// handles member functions that return structures.

#include <cstddef>
#include <cstdint>

namespace ff7vr::engine::ue {

// ------------------------------------------------------------------ basic types
struct FVector { float X, Y, Z; };
struct FVector2D { float X, Y; };
struct FRotator { float Pitch, Yaw, Roll; };  // degrees
struct alignas(16) FMatrix { float M[4][4]; };  // row-vector convention, M[row][col]
struct FIntPoint { std::int32_t X, Y; };
struct FIntRect { FIntPoint Min, Max; };

enum EStereoscopicPass : std::int32_t {
    eSSP_FULL = 0,
    eSSP_LEFT_EYE = 1,
    eSSP_RIGHT_EYE = 2,
    eSSP_MONOSCOPIC_EYE = 3,
};

enum EPixelFormat : std::uint8_t { PF_Unknown = 0, PF_B8G8R8A8 = 2 };

struct FRHITexture2D;
struct FTexture2DRHIRef { FRHITexture2D* Reference; };
struct FViewport;
struct FSceneView;
struct UCanvas;
struct FRHICommandListImmediate;
struct UEngine;

static_assert(sizeof(FVector2D) == 8, "FVector2D is passed by value in one 8-byte slot");
static_assert(sizeof(FMatrix) == 64);

// ------------------------------------------------------------------ IStereoRendering
// 14 interface slots plus the virtual destructor in slot 14. Thread that calls each slot
// in brackets. Slot offsets are checked by the "IStereoRendering slot ..." signatures.
struct StereoDeviceVtbl {
    bool (*IsStereoEnabled)(const void* self);                                            // 0  +0x00 [game, render]
    bool (*IsStereoEnabledOnNextFrame)(const void* self);                                 // 1  +0x08 [render, game]
    bool (*EnableStereo)(void* self, bool stereo);                                        // 2  +0x10 [game]
    void (*AdjustViewRect)(const void* self, EStereoscopicPass pass, std::int32_t* x, std::int32_t* y,
                           std::uint32_t* sizeX, std::uint32_t* sizeY);                    // 3  +0x18 [game]
    FVector2D* (*GetTextSafeRegionBounds)(const void* self, FVector2D* out);              // 4  +0x20 [game]
    void (*CalculateStereoViewOffset)(void* self, EStereoscopicPass pass, FRotator* viewRotation,
                                      float worldToMeters, FVector* viewLocation);        // 5  +0x28 [game]
    FMatrix* (*GetStereoProjectionMatrix)(const void* self, FMatrix* out, EStereoscopicPass pass); // 6 +0x30 [game]
    void (*InitCanvasFromView)(void* self, FSceneView* view, UCanvas* canvas);            // 7  +0x38 [game]
    bool (*Unknown8)(void* self);                                                         // 8  +0x40 not called via GEngine; return false
    void (*RenderTexture_RenderThread)(const void* self, FRHICommandListImmediate* cmdList,
                                       FRHITexture2D* backBuffer, FRHITexture2D* srcTexture,
                                       FVector2D windowSize);                             // 9  +0x48 [render]
    void (*GetOrthoProjection)(const void* self, std::int32_t rtWidth, std::int32_t rtHeight,
                               float orthoDistance, FMatrix* orthoProjection2);           // 10 +0x50 [game]
    void* (*Unknown11)(void* self);                                                       // 11 +0x58 not called via GEngine; return nullptr
    void* (*GetRenderTargetManager)(void* self);                                          // 12 +0x60 [game, render]
    void* (*GetStereoLayers)(void* self);                                                 // 13 +0x68 [game]
    void* (*ScalarDeletingDestructor)(void* self, std::uint32_t flags);                   // 14 +0x70 [any]
};
static_assert(offsetof(StereoDeviceVtbl, IsStereoEnabledOnNextFrame) == 0x08);
static_assert(offsetof(StereoDeviceVtbl, AdjustViewRect) == 0x18);
static_assert(offsetof(StereoDeviceVtbl, CalculateStereoViewOffset) == 0x28);
static_assert(offsetof(StereoDeviceVtbl, GetStereoProjectionMatrix) == 0x30);
static_assert(offsetof(StereoDeviceVtbl, InitCanvasFromView) == 0x38);
static_assert(offsetof(StereoDeviceVtbl, RenderTexture_RenderThread) == 0x48);
static_assert(offsetof(StereoDeviceVtbl, GetRenderTargetManager) == 0x60);
static_assert(offsetof(StereoDeviceVtbl, ScalarDeletingDestructor) == 0x70);

// ------------------------------------------------------------------ IStereoRenderTargetManager
// 8 slots, no destructor.
struct RenderTargetManagerVtbl {
    bool (*ShouldUseSeparateRenderTarget)(const void* self);                              // 0 +0x00 [game]
    void (*UpdateViewport)(void* self, bool useSeparateRenderTarget, const FViewport* viewport,
                           void* viewportWidget);                                         // 1 +0x08 [game]
    void (*CalculateRenderTargetSize)(void* self, const FViewport* viewport, std::uint32_t* sizeX,
                                      std::uint32_t* sizeY);                              // 2 +0x10 [render]
    bool (*NeedReAllocateViewportRenderTarget)(void* self, const FViewport* viewport);    // 3 +0x18 [game]
    bool (*NeedReAllocateDepthTexture)(void* self, const void* depthTargetRef);           // 4 +0x20 [render]
    std::uint32_t (*GetNumberOfBufferedFrames)(const void* self);                         // 5 +0x28 [render]
    bool (*AllocateRenderTargetTexture)(void* self, std::uint32_t index, std::uint32_t sizeX, std::uint32_t sizeY,
                                        std::uint8_t format, std::uint32_t numMips, std::uint32_t flags,
                                        std::uint32_t targetableTextureFlags, FTexture2DRHIRef* outTargetable,
                                        FTexture2DRHIRef* outShaderResource, std::uint32_t numSamples); // 6 +0x30 [render]
    bool (*AllocateDepthTexture)(void* self, std::uint32_t index, std::uint32_t sizeX, std::uint32_t sizeY,
                                 std::uint8_t format, std::uint32_t numMips, std::uint32_t flags,
                                 std::uint32_t targetableTextureFlags, FTexture2DRHIRef* outTargetable,
                                 FTexture2DRHIRef* outShaderResource, std::uint32_t numSamples); // 7 +0x38 [render]
};
static_assert(offsetof(RenderTargetManagerVtbl, CalculateRenderTargetSize) == 0x10);
static_assert(offsetof(RenderTargetManagerVtbl, GetNumberOfBufferedFrames) == 0x28);
static_assert(offsetof(RenderTargetManagerVtbl, AllocateRenderTargetTexture) == 0x30);
static_assert(offsetof(RenderTargetManagerVtbl, AllocateDepthTexture) == 0x38);

// ------------------------------------------------------------------ TSharedPtr<..., ThreadSafe>
struct SharedReferenceControllerVtbl {
    void (*DestroyObject)(void* self);                                   // when SharedReferenceCount reaches 0
    void* (*ScalarDeletingDestructor)(void* self, std::uint32_t flags);  // when WeakReferenceCount reaches 0
};
struct SharedReferenceController {
    const SharedReferenceControllerVtbl* vtbl;
    volatile std::int32_t SharedReferenceCount;
    volatile std::int32_t WeakReferenceCount;
    void* Object;
};
struct SharedPtrRaw {
    void* Object;
    SharedReferenceController* Controller;
};
static_assert(sizeof(SharedReferenceController) == 0x18);
static_assert(sizeof(SharedPtrRaw) == 0x10);

// ------------------------------------------------------------------ structure offsets
namespace offsets {
// UEngine (signature names in quotes)
inline constexpr std::size_t UEngine_StereoRenderingDevice = 0xD50;  // "UEngine::StereoRenderingDevice"
inline constexpr std::size_t UEngine_XRSystem = 0xD60;               // "UEngine::XRSystem"
inline constexpr std::size_t UEngine_ViewExtensions = 0xD70;         // "UEngine::ViewExtensions"
inline constexpr std::size_t UEngine_GameViewport = 0x980;           // "UEngine::GameViewport"
inline constexpr std::size_t UGameEngine_GameInstance = 0x1070;
inline constexpr std::size_t UGameInstance_LocalPlayers = 0x38;      // TArray<ULocalPlayer*>

// UGameViewportClient / FViewport (FViewport* = FSceneViewport + 8)
inline constexpr std::size_t UGameViewportClient_Viewport = 0xA0;    // "UGameViewportClient::Viewport"
inline constexpr std::size_t FViewport_RenderTargetTextureRHI = 0x08;
inline constexpr std::size_t FViewport_SizeX = 0xB8;
inline constexpr std::size_t FViewport_SizeY = 0xBC;
inline constexpr std::size_t FViewport_WindowMode = 0xC4;
inline constexpr std::size_t FSceneViewport_bUseSeparateRenderTarget = 0x27B;
inline constexpr std::size_t FSceneViewport_bForceSeparateRenderTarget = 0x27C;  // "FSceneViewport::bForceSeparateRenderTarget"
inline constexpr std::size_t FSceneViewport_RTTSize = 0x2D4;
inline constexpr std::size_t FSceneViewport_NumBufferedFrames = 0x320;

// FViewport vtable slots (byte offsets)
inline constexpr std::size_t FViewport_vt_GetRenderTargetTexture = 0x08;
inline constexpr std::size_t FViewport_vt_GetSizeXY = 0x18;
inline constexpr std::size_t FViewport_vt_IsStereoRenderingAllowed = 0x180;  // "FViewport slot IsStereoRenderingAllowed"
inline constexpr std::size_t FViewport_vt_UpdateViewportRHI = 0x190;         // "FViewport slot UpdateViewportRHI"

// D3D11 RHI texture
inline constexpr std::size_t FRHITexture2D_SizeX = 0x60;
inline constexpr std::size_t FRHITexture2D_SizeY = 0x64;
inline constexpr std::size_t FD3D11Texture2D_Resource = 0xA0;     // ID3D11Resource*, = vtable slot 7
inline constexpr std::size_t FRHITexture_vt_GetNativeResource = 0x38;

// ULocalPlayer
inline constexpr std::size_t ULocalPlayer_PlayerController = 0x30;
inline constexpr std::size_t ULocalPlayer_ViewportClient = 0x58;
inline constexpr std::size_t ULocalPlayer_ViewState = 0x90;        // FSceneViewStateReference, Reference at +8
inline constexpr std::size_t ULocalPlayer_StereoViewState = 0xB8;  // "ULocalPlayer::StereoViewState"
inline constexpr std::size_t ULocalPlayer_MonoViewState = 0xE0;
inline constexpr std::size_t FSceneViewStateReference_Reference = 0x08;

// Other
inline constexpr std::size_t FSceneView_StereoPass = 0x970;            // "FSceneView::StereoPass"
inline constexpr std::size_t AWorldSettings_WorldToMeters = 0x488;     // "AWorldSettings::WorldToMeters"
inline constexpr std::size_t UEngine_vt_InitializeHMDDevice = 0x378;   // "UEngine slot InitializeHMDDevice"
inline constexpr std::size_t IConsoleManager_vt_FindConsoleVariable = 0x90;
inline constexpr std::size_t IConsoleVariable_vt_Set = 0x60;           // Set(const TCHAR*, uint32 SetBy)
inline constexpr std::size_t IConsoleVariable_vt_GetInt = 0x68;
inline constexpr std::size_t IConsoleVariable_vt_GetFloat = 0x70;
}  // namespace offsets

// ------------------------------------------------------------------ expected RVAs (file version 1.0.0.7)
namespace rva_1_0_0_7 {
inline constexpr std::uint32_t SizeOfImage = 0x5EFC000;
inline constexpr std::uint32_t TimeDateStamp = 0x698BA49C;
inline constexpr std::uint32_t GEngine = 0x5831188;
inline constexpr std::uint32_t GUObjectArray = 0x53BD470;
inline constexpr std::uint32_t FNamePool = 0x5981300;
inline constexpr std::uint32_t GMalloc = 0x5825680;
inline constexpr std::uint32_t GNearClippingPlane = 0x53A49E4;
inline constexpr std::uint32_t GSystemResolution = 0x53E3AE0;
inline constexpr std::uint32_t IConsoleManager_Singleton = 0x57F9CA0;
inline constexpr std::uint32_t UEngine_InitializeHMDDevice = 0x3317CA0;
inline constexpr std::uint32_t UGameEngine_vftable = 0x4E56FA8;
inline constexpr std::uint32_t FFakeStereoRendering_vftable = 0x4EBA6C0;
inline constexpr std::uint32_t FSceneViewport_FViewport_vftable = 0x4EAEFF8;
inline constexpr std::uint32_t FSceneViewStateReference_Allocate = 0x32085C0;
inline constexpr std::uint32_t RenderLights_LightSortKeyPatchSite = 0x22351B0;  // BE 40 00 00 00; community fix writes 0x60 at +1
}  // namespace rva_1_0_0_7

}  // namespace ff7vr::engine::ue
