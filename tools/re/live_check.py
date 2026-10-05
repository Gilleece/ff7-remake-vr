"""Read-only inspection of a running ff7remake_.exe.

Attaches with PROCESS_VM_READ only (no code is injected, nothing is written),
resolves every address from signatures.json against the exe on disk, adds the
module base of the running process and prints the engine state that matters
for stereo rendering: GEngine, the stereo device and its vtable, XRSystem, the
game viewport and its render target, the local player's view states, and a
few console variables.

Usage:
  python tools/re/live_check.py [--json out.json] [--wait SECONDS]

Exit code 0 when the process was found and GEngine could be read.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import struct
import sys
import time

from ff7re import image
from signatures import load_db, resolve_all

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def find_pid(name="ff7remake_.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.lower() == name.lower():
            pid = e.th32ProcessID
            break
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    e = MODULEENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Module32FirstW(snap, ctypes.byref(e))
    while ok:
        out.append((e.szModule, e.modBaseAddr, e.modBaseSize, e.szExePath))
        ok = k32.Module32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


class Proc:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise OSError(f"OpenProcess failed: {ctypes.get_last_error()}")

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if not addr or not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)) or n.value != size:
            return None
        return buf.raw

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b else None

    def i32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<i", b)[0] if b else None

    def f32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<f", b)[0] if b else None

    def u8(self, addr):
        b = self.read(addr, 1)
        return b[0] if b else None


# Structure offsets that are read here but are not worth a signature of their own.
# Each one was read from the disassembly named next to it (see docs/re/engine.md).
OFS = {
    "UGameViewportClient::Viewport": 0xA0,          # ULocalPlayer::GetProjectionData: ViewportClient->Viewport
    "ULocalPlayer::ViewportClient": 0x58,           # same function
    "ULocalPlayer::PlayerController": 0x30,         # same function
    "ULocalPlayer::ViewState": 0x90,                # ULocalPlayer::PostInitProperties
    "ULocalPlayer::StereoViewState": 0xB8,          # same
    "ULocalPlayer::MonoViewState": 0xE0,            # same
    "FViewport::RenderTargetTextureRHI": 0x08,      # FSceneViewport::InitDynamicRHI / GetRenderTargetTexture
    "FViewport::SizeX": 0xB8,                       # FViewport vtable [3] GetSizeXY
    "FViewport::SizeY": 0xBC,
    "FViewport::WindowMode": 0xC4,                  # EnqueueBeginRenderFrame -> UpdateViewportRHI args
    "FSceneViewport::bUseSeparateRenderTarget": 0x27B,   # relative to the FViewport subobject
    "FSceneViewport::bForceSeparateRenderTarget": 0x27C,
    "FSceneViewport::RTTSize": 0x2D4,
    "FSceneViewport::NumBufferedFrames": 0x320,
    "FRHITexture2D::SizeX": 0x60,                   # D3D11 texture vtable [6] GetSizeXYZ
    "FRHITexture2D::SizeY": 0x64,
    "FD3D11Texture2D::Resource": 0xA0,              # vtable [7] GetNativeResource
    "UGameEngine::GameInstance": 0x1070,            # UGameEngine::Init: GameInstance->GetWorldContext()
    "UGameInstance::LocalPlayers": 0x38,            # TArray<ULocalPlayer*>, right after WorldContext (+0x30)
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", help="also write the report as JSON")
    ap.add_argument("--wait", type=float, default=0, help="seconds to wait for the process")
    args = ap.parse_args()

    pid = find_pid()
    t0 = time.time()
    while pid is None and time.time() - t0 < args.wait:
        time.sleep(1)
        pid = find_pid()
    if pid is None:
        print("ff7remake_.exe is not running")
        return 2
    mods = modules(pid)
    exe = next((m for m in mods if m[0].lower() == "ff7remake_.exe"), None)
    base = exe[1]
    img = image(exe[3])
    rva = resolve_all(img, quiet=True)
    db = load_db()
    p = Proc(pid)
    rep = {"pid": pid, "base": hex(base), "size_of_image": hex(exe[2])}
    # Offsets that have their own signature override the built-in table.
    for k in ("UGameViewportClient::Viewport",):
        OFS[k] = rva.get(k, OFS[k])
    if "FSceneViewport::bForceSeparateRenderTarget" in rva:
        OFS["FSceneViewport::bForceSeparateRenderTarget"] = rva["FSceneViewport::bForceSeparateRenderTarget"]
        OFS["FSceneViewport::bUseSeparateRenderTarget"] = rva["FSceneViewport::bForceSeparateRenderTarget"] - 1
    if "ULocalPlayer::StereoViewState" in rva:
        OFS["ULocalPlayer::StereoViewState"] = rva["ULocalPlayer::StereoViewState"]
        OFS["ULocalPlayer::ViewState"] = rva["ULocalPlayer::StereoViewState"] - 0x28
        OFS["ULocalPlayer::MonoViewState"] = rva["ULocalPlayer::StereoViewState"] + 0x28

    def line(k, v):
        rep[k] = v
        print(f"{k:48s} {v}")

    def name_of(addr):
        """Name an address by matching it against vtables/functions in the database."""
        if not addr:
            return "null"
        r = addr - base
        for e in db["entries"]:
            if e["kind"] in ("vtable", "function") and rva.get(e["name"]) == r:
                return f"{e['name']} (RVA {r:#x})"
        if 0 <= r < exe[2]:
            return f"RVA {r:#x}"
        mod = next((m for m in mods if m[1] <= addr < m[1] + m[2]), None)
        return f"{addr:#x}" + (f" ({mod[0]}+{addr - mod[1]:#x})" if mod else "")

    line("module base", f"{base:#x} (SizeOfImage {exe[2]:#x}, expected {db['build']['size_of_image']})")
    loaded = [m[0] for m in mods]
    line("dxgi.dll proxy loaded from game folder", any(m[0].lower() == "dxgi.dll" and "Win64" in m[3] for m in mods))
    line("modules of interest", [m for m in loaded if m.lower() in ("xinput1_3.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll",
                                                                     "openxr_loader.dll", "uevrbackend.dll")])

    gengine = p.u64(base + rva["GEngine"])
    line("GEngine", f"{gengine:#x}" if gengine else "null")
    if not gengine:
        return 1
    line("GEngine vtable", name_of(p.u64(gengine)))

    dev = p.u64(gengine + rva["UEngine::StereoRenderingDevice"])
    ctrl = p.u64(gengine + rva["UEngine::StereoRenderingDevice"] + 8)
    line("StereoRenderingDevice", f"{dev:#x}" if dev else "null")
    if dev:
        dev_vt = p.u64(dev)
        line("  device vtable", name_of(dev_vt))
        if dev_vt and base <= dev_vt < base + exe[2]:
            fov, unk, w, h = struct.unpack("<fIii", p.read(dev + 8, 16))
            line("  fake device fields (FOV, +0xC, Width, Height)", (round(fov, 3), unk, w, h))
        else:
            # The mod's device: a static object in the mod DLL whose first field is its table.
            line("  device is the mod's", any(m[1] <= dev_vt < m[1] + m[2] and m[0].lower() == "xinput1_3.dll" for m in mods))
        if ctrl:
            vt, shared, weak, obj = struct.unpack("<QiiQ", p.read(ctrl, 24))
            line("  reference controller", f"vtable {name_of(vt)}, shared {shared}, weak {weak}, object {obj:#x}")
    xr = p.u64(gengine + rva["UEngine::XRSystem"])
    line("XRSystem", name_of(p.u64(xr)) if xr else "null")
    vx = p.u64(gengine + rva["UEngine::ViewExtensions"])
    line("ViewExtensions", f"{vx:#x}" if vx else "null")

    gvc = p.u64(gengine + rva["UEngine::GameViewport"])
    line("GameViewport (UGameViewportClient)", f"{gvc:#x}" if gvc else "null")
    if gvc:
        vp = p.u64(gvc + OFS["UGameViewportClient::Viewport"])
        line("  Viewport (FViewport)", f"{vp:#x}" if vp else "null")
        if vp:
            line("  viewport vtable", name_of(p.u64(vp)))
            sx, sy = p.u32(vp + OFS["FViewport::SizeX"]), p.u32(vp + OFS["FViewport::SizeY"])
            line("  viewport size", (sx, sy))
            line("  window mode", p.u32(vp + OFS["FViewport::WindowMode"]))
            line("  bUseSeparateRenderTarget / bForce", (p.u8(vp + OFS["FSceneViewport::bUseSeparateRenderTarget"]),
                                                          p.u8(vp + OFS["FSceneViewport::bForceSeparateRenderTarget"])))
            line("  RTTSize", (p.i32(vp + OFS["FSceneViewport::RTTSize"]), p.i32(vp + OFS["FSceneViewport::RTTSize"] + 4)))
            line("  NumBufferedFrames", p.i32(vp + OFS["FSceneViewport::NumBufferedFrames"]))
            tex = p.u64(vp + OFS["FViewport::RenderTargetTextureRHI"])
            line("  RenderTargetTextureRHI", f"{tex:#x}" if tex else "null")
            if tex:
                line("    texture vtable", name_of(p.u64(tex)))
                line("    texture size", (p.u32(tex + OFS["FRHITexture2D::SizeX"]), p.u32(tex + OFS["FRHITexture2D::SizeY"])))
                res = p.u64(tex + OFS["FD3D11Texture2D::Resource"])
                line("    native ID3D11Resource", name_of(res) if res else "null")
                if res:
                    line("    native resource vtable", name_of(p.u64(res)))

    rx, ry = p.i32(base + rva["GSystemResolution"]), p.i32(base + rva["GSystemResolution"] + 4)
    line("GSystemResolution (ResX, ResY)", (rx, ry))
    if "FSceneViewport separate target format" in rva:
        # EPixelFormat the engine allocates the separate (stereo) render target with when the
        # render target manager does not allocate it: 2 = PF_B8G8R8A8, 35 = PF_A2B10G10R10.
        line("separate render target EPixelFormat", p.u8(base + rva["FSceneViewport separate target format"]))
    gi = p.u64(gengine + OFS["UGameEngine::GameInstance"])
    line("GameInstance", f"{gi:#x}" if gi else "null")
    if gi:
        arr, num = p.u64(gi + OFS["UGameInstance::LocalPlayers"]), p.i32(gi + OFS["UGameInstance::LocalPlayers"] + 8)
        line("  LocalPlayers.Num", num)
        if arr and num and 0 < num < 8:
            lp = p.u64(arr)
            line("  LocalPlayers[0]", f"{lp:#x}" if lp else "null")
            if lp:
                line("    vtable", name_of(p.u64(lp)))
                line("    ViewportClient == GameViewport", p.u64(lp + OFS["ULocalPlayer::ViewportClient"]) == gvc)
                for k in ("ULocalPlayer::ViewState", "ULocalPlayer::StereoViewState", "ULocalPlayer::MonoViewState"):
                    ref = p.u64(lp + OFS[k] + 8)  # FSceneViewStateReference: vptr, Reference
                    line(f"    {k.split('::')[1]} (FSceneViewStateReference.Reference)", f"{ref:#x}" if ref else "null")

    # Console variables registered as statics next to InitializeHMDDevice / the fake device constructor.
    # TAutoConsoleVariable keeps the IConsoleVariable* at +8 and the TConsoleVariableData<T>* at +0x10.
    # r.EnableStereoEmulation comes from the database; the three emulation-size cvars are the statics
    # registered by the FFakeStereoRendering constructor of this build (RVAs of the TAutoConsoleVariable objects).
    for name, static_rva, kind in (("r.EnableStereoEmulation", rva["r.EnableStereoEmulation data"] - 0x10, "i"),
                                   ("r.StereoEmulationFOV", 0x5A06900, "f"),
                                   ("r.StereoEmulationWidth", 0x5A06920, "i"),
                                   ("r.StereoEmulationHeight", 0x5A06940, "i")):
        data = p.u64(base + static_rva + 0x10)
        if data:
            v = p.f32(data) if kind == "f" else p.i32(data)
            line(f"cvar {name}", v)
        else:
            line(f"cvar {name}", "not registered yet")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=2, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
