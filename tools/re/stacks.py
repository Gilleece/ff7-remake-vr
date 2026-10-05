"""Print the call stack of every thread of a running ff7remake_.exe.

For diagnosing hangs: each thread is suspended for a moment, its context read
and its stack walked with dbghelp (unwind data from the modules' .pdata, so no
symbols are needed). Frames are printed as module+RVA; functions of modules
with a PDB on the symbol path (the mod's DLL, system DLLs with a local symbol
cache) get their names. RVAs inside ff7remake_.exe can be looked up in
docs/re/engine.md or disassembled with ff7re.py.

Usage:
  python tools/re/stacks.py [--pid PID] [--sympath DIR;DIR] [--max-frames 48] [--only window|all]

The thread that owns the game window (the game thread) is listed first.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
dbghelp = ctypes.WinDLL(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "dbghelp.dll"))

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_GET_CONTEXT = 0x0008
THREAD_SUSPEND_RESUME = 0x0002
THREAD_QUERY_INFORMATION = 0x0040
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPTHREAD = 0x4
CONTEXT_AMD64_FULL = 0x0010000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664
AddrModeFlat = 3
SYMOPT_UNDNAME = 0x2
SYMOPT_DEFERRED_LOADS = 0x4
SYMOPT_FAIL_CRITICAL_ERRORS = 0x200
SYMOPT_NO_PROMPTS = 0x80000


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", wt.WORD), ("Mode", wt.DWORD)]


class STACKFRAME64(ctypes.Structure):
    # KdHelp and the rest are covered by the padding; only the addresses are used.
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", wt.BOOL), ("Virtual", wt.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("Pad", ctypes.c_ubyte * 512)]


class IMAGEHLP_MODULEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("BaseOfImage", ctypes.c_uint64), ("ImageSize", wt.DWORD),
                ("TimeDateStamp", wt.DWORD), ("CheckSum", wt.DWORD), ("NumSyms", wt.DWORD), ("SymType", ctypes.c_int),
                ("ModuleName", ctypes.c_wchar * 32), ("ImageName", ctypes.c_wchar * 256),
                ("LoadedImageName", ctypes.c_wchar * 256), ("LoadedPdbName", ctypes.c_wchar * 256),
                ("CVSig", wt.DWORD), ("CVData", ctypes.c_wchar * 780), ("PdbSig", wt.DWORD), ("PdbSig70", ctypes.c_byte * 16),
                ("PdbAge", wt.DWORD), ("PdbUnmatched", wt.BOOL), ("DbgUnmatched", wt.BOOL), ("LineNumbers", wt.BOOL),
                ("GlobalSymbols", wt.BOOL), ("TypeInfo", wt.BOOL), ("SourceIndexed", wt.BOOL), ("Publics", wt.BOOL),
                ("MachineType", wt.DWORD), ("Reserved", wt.DWORD)]


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG), ("Scope", wt.ULONG),
                ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_wchar * 512)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.CloseHandle.argtypes = [wt.HANDLE]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbghelp.SymCleanup.argtypes = [wt.HANDLE]
dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbghelp.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymGetModuleInfoW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(IMAGEHLP_MODULEW64)]
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]

FUNC_TABLE = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)
MOD_BASE = ctypes.WINFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                                ctypes.c_void_p, FUNC_TABLE, MOD_BASE, ctypes.c_void_p]

_func_table = FUNC_TABLE(lambda h, a: dbghelp.SymFunctionTableAccess64(h, a))
_mod_base = MOD_BASE(lambda h, a: dbghelp.SymGetModuleBase64(h, a))


def find_pid(name: str) -> int | None:
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    pid = None
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            pid = e.th32ProcessID
            break
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def threads_of(pid: int) -> list[int]:
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def window_threads(pid: int) -> set[int]:
    found: set[int] = set()
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        p = wt.DWORD()
        tid = u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(hwnd):
            found.add(tid)
        return True

    u32.EnumWindows(proc(cb), 0)
    return found


def thread_name(h) -> str:
    try:
        p = ctypes.c_wchar_p()
        k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
        if k32.GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value:
            name = p.value
            k32.LocalFree(p)
            return name
    except (AttributeError, OSError):
        pass
    return ""


def start_address(h) -> int:
    addr = ctypes.c_uint64()
    if ntdll.NtQueryInformationThread(h, 9, ctypes.byref(addr), 8, None) == 0:
        return addr.value
    return 0


class Describer:
    def __init__(self, hproc):
        self.h = hproc
        self.cache: dict[int, tuple[int, str]] = {}

    def module(self, addr: int) -> tuple[int, str]:
        base = dbghelp.SymGetModuleBase64(self.h, addr)
        if not base:
            return 0, "?"
        if base not in self.cache:
            mi = IMAGEHLP_MODULEW64()
            mi.SizeOfStruct = ctypes.sizeof(mi)
            name = "?"
            if dbghelp.SymGetModuleInfoW64(self.h, base, ctypes.byref(mi)):
                name = os.path.basename(mi.ImageName) or mi.ModuleName
            self.cache[base] = (base, name)
        return self.cache[base]

    def __call__(self, addr: int) -> str:
        base, name = self.module(addr)
        if not base:
            return f"{addr:#x}"
        text = f"{name}+{addr - base:#x}"
        if name.lower() != "ff7remake_.exe":
            si = SYMBOL_INFOW()
            si.SizeOfStruct = 88  # sizeof(SYMBOL_INFOW) in C (one name character)
            si.MaxNameLen = 511
            disp = ctypes.c_uint64()
            if dbghelp.SymFromAddrW(self.h, addr, ctypes.byref(disp), ctypes.byref(si)) and disp.value < 0x10000:
                text += f" ({si.Name}+{disp.value:#x})"
        return text


def walk(hproc, hthread, describe, max_frames: int) -> list[str]:
    buf = ctypes.create_string_buffer(1232 + 16)
    ctx = (ctypes.addressof(buf) + 15) & ~15
    ctypes.c_uint32.from_address(ctx + 0x30).value = CONTEXT_AMD64_FULL
    if not k32.GetThreadContext(hthread, ctx):
        return [f"(GetThreadContext failed: {ctypes.get_last_error()})"]
    rip = ctypes.c_uint64.from_address(ctx + 0xF8).value
    rsp = ctypes.c_uint64.from_address(ctx + 0x98).value
    rbp = ctypes.c_uint64.from_address(ctx + 0xA0).value
    sf = STACKFRAME64()
    sf.AddrPC.Offset, sf.AddrPC.Mode = rip, AddrModeFlat
    sf.AddrFrame.Offset, sf.AddrFrame.Mode = rbp, AddrModeFlat
    sf.AddrStack.Offset, sf.AddrStack.Mode = rsp, AddrModeFlat
    frames = []
    for _ in range(max_frames):
        if not dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, hproc, hthread, ctypes.byref(sf), ctx, None, _func_table,
                                   _mod_base, None):
            break
        if not sf.AddrPC.Offset:
            break
        frames.append(describe(sf.AddrPC.Offset))
    return frames


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--process", default="ff7remake_.exe")
    ap.add_argument("--sympath", default="")
    ap.add_argument("--max-frames", type=int, default=48)
    ap.add_argument("--only", choices=["window", "all"], default="all")
    args = ap.parse_args()

    pid = args.pid or find_pid(args.process)
    if not pid:
        print(f"{args.process} is not running")
        return 1
    hproc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not hproc:
        print(f"OpenProcess failed: {ctypes.get_last_error()}")
        return 1
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS)
    if not dbghelp.SymInitializeW(hproc, args.sympath or None, True):
        print(f"SymInitialize failed: {ctypes.get_last_error()}")
        return 1
    describe = Describer(hproc)
    wins = window_threads(pid)
    tids = threads_of(pid)
    tids.sort(key=lambda t: (t not in wins, t))
    print(f"pid {pid}, {len(tids)} threads; window thread(s): {sorted(wins)}")
    for tid in tids:
        if args.only == "window" and tid not in wins:
            continue
        h = k32.OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, False, tid)
        if not h:
            print(f"\n== thread {tid}: OpenThread failed")
            continue
        name = thread_name(h)
        start = start_address(h)
        suspended = k32.SuspendThread(h) != 0xFFFFFFFF
        try:
            frames = walk(hproc, h, describe, args.max_frames)
        finally:
            if suspended:
                k32.ResumeThread(h)
            k32.CloseHandle(h)
        tag = " [window thread]" if tid in wins else ""
        print(f"\n== thread {tid}{tag} {name!r} start {describe(start) if start else '?'}")
        for i, f in enumerate(frames):
            print(f"  #{i:02d} {f}")
    dbghelp.SymCleanup(hproc)
    k32.CloseHandle(hproc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
