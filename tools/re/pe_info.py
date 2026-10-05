"""Exe identity report: hashes, PE header, sections (with entropy), imports, TLS,
and simple packer / DRM indicators.

Usage: .venv/Scripts/python.exe tools/re/pe_info.py [path-to-ff7remake_.exe]
"""
import hashlib
import math
import sys
import datetime as dt

import numpy as np
import pefile

from ff7re import find_game_exe


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = np.bincount(np.frombuffer(data, dtype=np.uint8), minlength=256)
    p = counts[counts > 0] / len(data)
    return float(-(p * np.log2(p)).sum())


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else find_game_exe()
    raw = open(path, "rb").read()
    print(f"file      : {path}")
    print(f"size      : {len(raw)}")
    print(f"sha256    : {hashlib.sha256(raw).hexdigest()}")
    print(f"md5       : {hashlib.md5(raw).hexdigest()}")
    pe = pefile.PE(data=raw, fast_load=False)
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    ts = fh.TimeDateStamp
    print(f"timestamp : 0x{ts:08x} ({dt.datetime.fromtimestamp(ts, dt.timezone.utc).isoformat()})")
    print(f"machine   : 0x{fh.Machine:x}  characteristics 0x{fh.Characteristics:x}")
    print(f"imagebase : 0x{oh.ImageBase:x}  sizeofimage 0x{oh.SizeOfImage:x}  entry RVA 0x{oh.AddressOfEntryPoint:x}")
    print(f"subsystem : {oh.Subsystem}  dllchars 0x{oh.DllCharacteristics:x} (ASLR={bool(oh.DllCharacteristics & 0x40)}, CFG={bool(oh.DllCharacteristics & 0x4000)})")
    print(f"checksum  : header 0x{oh.CheckSum:x}")
    # version resource
    try:
        for fi in getattr(pe, "FileInfo", []) or []:
            for entry in fi:
                if hasattr(entry, "StringTable"):
                    for st in entry.StringTable:
                        for k, v in st.entries.items():
                            print(f"version   : {k.decode(errors='replace')} = {v.decode(errors='replace')}")
        if hasattr(pe, "VS_FIXEDFILEINFO"):
            ffi = pe.VS_FIXEDFILEINFO[0]
            print(f"fixedver  : {ffi.FileVersionMS >> 16}.{ffi.FileVersionMS & 0xffff}.{ffi.FileVersionLS >> 16}.{ffi.FileVersionLS & 0xffff}")
    except Exception as e:  # noqa
        print("version   : error", e)
    print()
    print("sections:")
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        data = s.get_data()
        print(f"  {name:10s} RVA 0x{s.VirtualAddress:08x} VSize 0x{s.Misc_VirtualSize:08x} Raw 0x{s.SizeOfRawData:08x} "
              f"chars 0x{s.Characteristics:08x} entropy {entropy(data):.3f}")
    ep = oh.AddressOfEntryPoint
    ep_sec = next((s for s in pe.sections if s.VirtualAddress <= ep < s.VirtualAddress + s.Misc_VirtualSize), None)
    print(f"entry point section: {ep_sec.Name.rstrip(b'\\0').decode() if ep_sec else '?'}")
    print()
    print("imports (DLL: count):")
    for imp in pe.DIRECTORY_ENTRY_IMPORT:
        names = [i.name.decode() if i.name else f"#{i.ordinal}" for i in imp.imports]
        print(f"  {imp.dll.decode():40s} {len(names)}")
    if hasattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT"):
        print("delay imports:")
        for imp in pe.DIRECTORY_ENTRY_DELAY_IMPORT:
            print(f"  {imp.dll.decode():40s} {len(imp.imports)}")
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        tls = pe.DIRECTORY_ENTRY_TLS.struct
        print(f"TLS callbacks array VA: 0x{tls.AddressOfCallBacks:x}")
        cb_rva = tls.AddressOfCallBacks - oh.ImageBase
        cbs = []
        for i in range(16):
            v = pe.get_qword_at_rva(cb_rva + 8 * i)
            if not v:
                break
            cbs.append(v - oh.ImageBase)
        print("TLS callbacks RVA:", [hex(c) for c in cbs])
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        print("exports:")
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            print(f"  {e.name}  RVA 0x{e.address:x}")
    sec_dir = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_SECURITY"]]
    print(f"authenticode: size {sec_dir.Size}")
    dbg = getattr(pe, "DIRECTORY_ENTRY_DEBUG", [])
    for d in dbg:
        e = d.entry
        if e is not None and hasattr(e, "PdbFileName"):
            print(f"pdb path  : {e.PdbFileName.rstrip(b'\\0').decode(errors='replace')}")
    print()
    # Packer / DRM indicators
    lower = raw.lower()
    for needle in [b".bind", b"steam_api64.dll", b"steamstub", b"denuvo", b"vmprotect", b".vmp", b"themida",
                   b"easyanticheat", b"enigma", b"arxan", b"SteamAPI_RestartAppIfNecessary".lower(),
                   b"isdebuggerpresent", b"checkremotedebuggerpresent", b"ntqueryinformationprocess"]:
        print(f"indicator {needle.decode():35s}: {lower.count(needle)}")


if __name__ == "__main__":
    main()
