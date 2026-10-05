"""Small static-analysis toolkit for ff7remake_.exe (x64 PE, no PDB).

Targeted helpers only (no full auto-analysis):
  - Image: memory-mapped image addressable by RVA
  - strings (ASCII / UTF-16LE) -> RVAs
  - RIP-relative xrefs to an RVA (numpy scan of .text), plus E8/E9 call/jmp xrefs
  - function bounds from .pdata (exact on x64 MSVC builds)
  - capstone disassembly helpers
  - IDA-style signature scanning ("48 8B 05 ?? ?? ?? ??")
  - vtable reading

All addresses handled here are RVAs (module base = 0).
"""
from __future__ import annotations

import bisect
import functools
import re
import struct
from dataclasses import dataclass

import numpy as np
import pefile
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

STEAM_APP_ID = "1462040"
EXE_REL = r"End\Binaries\Win64\ff7remake_.exe"


def find_game_exe() -> str:
    """Locate ff7remake_.exe: $FF7R_EXE, else the Steam library that holds app 1462040."""
    import os
    env = os.environ.get("FF7R_EXE")
    if env:
        return env
    steam = None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            steam = winreg.QueryValueEx(k, "SteamPath")[0]
    except OSError:
        pass
    if not steam:
        raise SystemExit("Steam not found; set FF7R_EXE or pass the exe path")
    vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
    libs = [steam]
    if os.path.exists(vdf):
        cur = None
        for line in open(vdf, encoding="utf-8", errors="replace"):
            m = re.match(r'\s*"path"\s*"(.*)"', line)
            if m:
                cur = m.group(1).replace("\\\\", "\\")
                libs.append(cur)
    for lib in libs:
        acf = os.path.join(lib, "steamapps", f"appmanifest_{STEAM_APP_ID}.acf")
        if os.path.exists(acf):
            installdir = None
            for line in open(acf, encoding="utf-8", errors="replace"):
                m = re.match(r'\s*"installdir"\s*"(.*)"', line)
                if m:
                    installdir = m.group(1)
            if installdir:
                p = os.path.join(lib, "steamapps", "common", installdir, EXE_REL)
                if os.path.exists(p):
                    return p
    raise SystemExit("ff7remake_.exe not found in any Steam library; set FF7R_EXE or pass the exe path")


DEFAULT_EXE = None  # resolved lazily by find_game_exe()


@dataclass
class Section:
    name: str
    rva: int
    vsize: int
    chars: int

    @property
    def end(self):
        return self.rva + self.vsize

    @property
    def executable(self):
        return bool(self.chars & 0x20000000)


class Image:
    def __init__(self, path: str | None = None):
        path = path or find_game_exe()
        self.path = path
        raw = open(path, "rb").read()
        self.raw = raw
        # Headers only; section mapping and .pdata parsing are done by hand (pefile is far too slow on 96 MB).
        self.pe = pefile.PE(data=raw[:0x1000], fast_load=True)
        oh = self.pe.OPTIONAL_HEADER
        self.image_base = oh.ImageBase
        self.size = oh.SizeOfImage
        buf = bytearray(self.size)
        buf[: oh.SizeOfHeaders] = raw[: oh.SizeOfHeaders]
        self.sections = []
        for s in self.pe.sections:
            n = min(s.SizeOfRawData, s.Misc_VirtualSize)
            buf[s.VirtualAddress:s.VirtualAddress + n] = raw[s.PointerToRawData:s.PointerToRawData + n]
            self.sections.append(Section(s.Name.rstrip(b"\0").decode(), s.VirtualAddress, s.Misc_VirtualSize,
                                         s.Characteristics))
        self.mem = bytes(buf)
        self.np = np.frombuffer(self.mem, dtype=np.uint8)
        self.text = next(s for s in self.sections if s.name == ".text")
        self.rdata = next(s for s in self.sections if s.name == ".rdata")
        self.data = next(s for s in self.sections if s.name == ".data")
        # .pdata function table (RUNTIME_FUNCTION = 3 x uint32)
        exc = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
        tbl = np.frombuffer(self.mem, dtype=np.uint32, count=exc.Size // 4, offset=exc.VirtualAddress).reshape(-1, 3)
        order = np.argsort(tbl[:, 0], kind="stable")
        tbl = tbl[order]
        self._rf = tbl
        self.fstarts = tbl[:, 0].tolist()
        self.fends = tbl[:, 1].tolist()
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True
        self.md_fast = Cs(CS_ARCH_X86, CS_MODE_64)

    # ---------- raw reads ----------
    def u8(self, rva):
        return self.mem[rva]

    def u32(self, rva):
        return struct.unpack_from("<I", self.mem, rva)[0]

    def i32(self, rva):
        return struct.unpack_from("<i", self.mem, rva)[0]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.mem, rva)[0]

    def ptr_rva(self, rva):
        """Read an absolute pointer stored in the image and convert it to an RVA (None if outside)."""
        v = self.u64(rva)
        r = v - self.image_base
        return r if 0 <= r < self.size else None

    def section_of(self, rva):
        for s in self.sections:
            if s.rva <= rva < s.end:
                return s
        return None

    def cstr(self, rva, maxlen=256):
        end = self.mem.find(b"\0", rva, rva + maxlen)
        return self.mem[rva:end if end >= 0 else rva + maxlen].decode("latin-1")

    def wstr(self, rva, maxlen=256):
        out = []
        for i in range(maxlen):
            c = struct.unpack_from("<H", self.mem, rva + 2 * i)[0]
            if c == 0:
                break
            out.append(chr(c))
        return "".join(out)

    # ---------- functions (.pdata) ----------
    def func_containing(self, rva):
        """(start, end) of the RUNTIME_FUNCTION containing rva. Follows chained unwind info back to the primary entry."""
        i = bisect.bisect_right(self.fstarts, rva) - 1
        if i < 0:
            return None
        if not (self.fstarts[i] <= rva < self.fends[i]):
            return None
        start = self.fstarts[i]
        # Chained unwind info (UNW_FLAG_CHAININFO = 4): walk back to the primary function.
        primary = self._primary_func(start)
        return primary, self.fends[i]

    def _primary_func(self, start):
        for _ in range(16):
            i = bisect.bisect_left(self.fstarts, start)
            # find the unwind info for this entry
            rf = self._runtime_function(i)
            if rf is None:
                return start
            unwind_rva = rf[2]
            ver_flags = self.mem[unwind_rva]
            flags = ver_flags >> 3
            if not (flags & 0x4):
                return start
            count_codes = self.mem[unwind_rva + 2]
            off = unwind_rva + 4 + ((count_codes + 1) & ~1) * 2
            start = self.u32(off)
        return start

    def _runtime_function(self, i):
        if i >= len(self.fstarts):
            return None
        r = self._rf[i]
        return int(r[0]), int(r[1]), int(r[2])

    def is_func_start(self, rva):
        i = bisect.bisect_left(self.fstarts, rva)
        return i < len(self.fstarts) and self.fstarts[i] == rva

    # ---------- strings ----------
    def find_bytes(self, needle: bytes, start=0, end=None, limit=1000):
        end = self.size if end is None else end
        out, i = [], self.mem.find(needle, start, end)
        while i >= 0 and len(out) < limit:
            out.append(i)
            i = self.mem.find(needle, i + 1, end)
        return out

    def find_string(self, s: str, wide=None, exact=True):
        """RVAs of a string in .rdata/.data. exact=True requires a NUL terminator and a NUL (or start) before it."""
        res = []
        variants = []
        if wide in (None, False):
            variants.append((s.encode("latin-1"), 1))
        if wide in (None, True):
            variants.append((s.encode("utf-16-le"), 2))
        for enc, w in variants:
            needle = enc + (b"\0" * w if exact else b"")
            for sec in (self.rdata, self.data):
                for hit in self.find_bytes(needle, sec.rva, sec.end):
                    if exact and hit >= w and self.mem[hit - w:hit] != b"\0" * w:
                        continue
                    if w == 2 and hit % 2:
                        continue
                    res.append(hit)
        return sorted(set(res))

    # ---------- xrefs ----------
    @functools.cached_property
    def _text_i32(self):
        """int32 view of .text at each of the 4 byte alignments, plus disp + position (int64) per alignment."""
        t = self.text
        planes = []
        for a in range(4):
            cnt = (t.vsize - a - 4) // 4
            v = np.frombuffer(self.mem, dtype="<i4", count=cnt, offset=t.rva + a).astype(np.int64)
            v += np.arange(t.rva + a, t.rva + a + 4 * cnt, 4, dtype=np.int64)  # v = pos + disp
            planes.append((t.rva + a, v))
        return planes

    def xrefs_disp(self, target: int, extra=(0, 1, 2, 4)):
        """Candidate code positions p where a rip-relative disp32 at p resolves to target.
        `extra` = bytes of immediate following the disp (0 for lea/mov/call [rip], 1/2/4 for cmp/mov imm)."""
        hits = []
        for base, v in self._text_i32:
            for k in extra:
                m = np.nonzero(v == target - 4 - k)[0]
                hits.extend((int(base + 4 * i), k) for i in m)
        return sorted(hits)

    def code_refs(self, target: int):
        """Verified instruction-level references to target (rip-relative operands, E8 call, E9 jmp).
        Returns list of (insn_rva, mnemonic, op_str)."""
        out = []
        for p, k in self.xrefs_disp(target):
            insn = self._insn_covering(p)
            if insn is not None:
                if insn.address + insn.size != p + 4 + k:
                    continue
                if self._insn_refs(insn, target):
                    out.append((insn.address, insn.mnemonic, insn.op_str))
                continue
            # No unwind info (leaf function): take the longest decode that ends right after the displacement.
            best = None
            for back in range(1, 12):
                start = p - back
                cand = self.insn_at(start)
                if cand is None or start + cand.size != p + 4 + k:
                    continue
                if self._insn_refs(cand, target):
                    best = cand
            if best is not None:
                out.append((best.address, best.mnemonic, best.op_str))
        return sorted(set(out))

    def _insn_refs(self, insn, target):
        if insn.mnemonic in ("call", "jmp") and insn.size == 5 and self.mem[insn.address] in (0xE8, 0xE9):
            return insn.address + 5 + self.i32(insn.address + 1) == target
        return self.rip_target(insn) == target

    def _insn_covering(self, rva, max_func=0x20000):
        """Linear-sweep decode from the start of the containing .pdata function; return the insn covering rva."""
        i = bisect.bisect_right(self.fstarts, rva) - 1
        if i < 0 or not (self.fstarts[i] <= rva < self.fends[i]):
            return None
        start = self.fstarts[i]
        if rva - start > max_func:
            return None
        last_end = start
        for (a, size, _m, _o) in self.md_fast.disasm_lite(self.mem[start:rva + 16], start):
            if a + size > rva:
                return self.insn_at(a)
            last_end = a + size
        return None

    def calls_to(self, target: int):
        return [r for r in self.code_refs(target) if r[1] in ("call", "jmp")]

    def data_refs(self, target: int, sections=None):
        """Absolute 8-byte pointers to target (e.g. vtable slots, cvar tables)."""
        va = target + self.image_base
        needle = struct.pack("<Q", va)
        out = []
        for sec in sections or (self.rdata, self.data):
            for hit in self.find_bytes(needle, sec.rva, sec.end, limit=10000):
                if hit % 8 == 0:
                    out.append(hit)
        return out

    # ---------- disassembly ----------
    def insn_at(self, rva):
        try:
            return next(self.md.disasm(self.mem[rva:rva + 16], rva))
        except StopIteration:
            return None

    def disasm(self, rva, count=40, stop_at_ret=False):
        out = []
        for insn in self.md.disasm(self.mem[rva:rva + count * 16], rva):
            out.append(insn)
            if len(out) >= count:
                break
            if stop_at_ret and insn.mnemonic in ("ret", "int3"):
                break
        return out

    def disasm_func(self, rva, max_insns=4000):
        f = self.func_containing(rva)
        start, end = (f if f else (rva, rva + 0x400))
        out = []
        for insn in self.md.disasm(self.mem[start:end], start):
            out.append(insn)
            if len(out) >= max_insns:
                break
        return out

    def disasm_flow(self, rva, max_insns=3000, follow_tail_jumps=False):
        """Recursive-descent disassembly of the function at rva.

        Follows conditional and unconditional jumps (MSVC splits functions into
        cold chunks that live far away), does not follow calls. A direct jmp to
        another .pdata function start is treated as a tail call unless
        follow_tail_jumps is set. Returns instructions sorted by address."""
        seen = {}
        work = [rva]
        while work and len(seen) < max_insns:
            pc = work.pop()
            while pc not in seen and len(seen) < max_insns:
                insn = self.insn_at(pc)
                if insn is None:
                    break
                seen[pc] = insn
                m = insn.mnemonic
                if m in ("ret", "int3", "ud2") or m.startswith("ret"):
                    break
                if m == "jmp" or (m.startswith("j") and m != "jmp"):
                    tgt = self.rip_target(insn) if insn.operands and insn.operands[0].type == 2 else None
                    if m == "jmp":
                        if tgt is not None and (follow_tail_jumps or not self.is_func_start(tgt) or tgt == rva):
                            work.append(tgt)
                        break
                    if tgt is not None:
                        work.append(tgt)
                pc += insn.size
        return [seen[k] for k in sorted(seen)]

    def fmt(self, insns, base=None):
        lines = []
        for i in insns:
            extra = ""
            tgt = self.rip_target(i)
            if tgt is not None:
                extra = self.describe(tgt)
            lines.append(f"  {i.address:08x}: {i.bytes.hex():<24s} {i.mnemonic:6s} {i.op_str}{('   ; ' + extra) if extra else ''}")
        return "\n".join(lines)

    def rip_target(self, insn):
        if insn.mnemonic in ("call", "jmp") or insn.mnemonic.startswith("j"):
            if insn.operands and insn.operands[0].type == 2:  # IMM
                return insn.operands[0].imm
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                return insn.address + insn.size + op.mem.disp
        return None

    def describe(self, rva):
        sec = self.section_of(rva)
        if sec is None:
            return f"{rva:#x}"
        s = f"{sec.name}+{rva:#x}"
        if sec.name in (".rdata", ".data"):
            # try ascii / wide string
            b = self.mem[rva:rva + 64]
            if len(b) > 4 and all(32 <= c < 127 for c in b[:4]):
                s += f' "{self.cstr(rva, 64)}"'
            elif len(b) > 8 and b[1] == 0 and b[3] == 0 and 32 <= b[0] < 127 and 32 <= b[2] < 127:
                s += f' L"{self.wstr(rva, 64)}"'
            else:
                p = self.ptr_rva(rva) if rva + 8 <= self.size else None
                if p is not None and self.section_of(p) and self.section_of(p).executable:
                    s += f" -> fn {p:#x}"
        return s

    # ---------- signatures ----------
    @staticmethod
    def parse_sig(sig: str):
        toks = sig.split()
        pat = bytearray()
        mask = bytearray()
        for t in toks:
            if t in ("?", "??"):
                pat.append(0)
                mask.append(0)
            else:
                pat.append(int(t, 16))
                mask.append(1)
        return bytes(pat), bytes(mask)

    def sig_scan(self, sig: str, section=None, limit=50):
        pat, mask = self.parse_sig(sig)
        sec = section or self.text
        # regex with wildcards
        rx = b"".join(re.escape(bytes([b])) if m else b"." for b, m in zip(pat, mask))
        out = []
        for m in re.finditer(rx, self.mem[sec.rva:sec.end], flags=re.DOTALL):
            out.append(sec.rva + m.start())
            if len(out) >= limit:
                break
        return out

    def sig_resolve(self, hit: int, kind: str, offset=0, insn_len=None):
        """kind: 'rva' (hit+offset), 'rip' (disp32 at hit+offset relative to end of insn),
        'call' (E8 at hit+offset), 'deref' not supported statically."""
        if kind == "rva":
            return hit + offset
        if kind == "rip":
            insn = self.insn_at(hit + offset)
            return self.rip_target(insn)
        if kind == "call":
            return hit + offset + 5 + self.i32(hit + offset + 1)
        raise ValueError(kind)

    # ---------- vtables ----------
    def vtable(self, rva, max_entries=200):
        out = []
        for i in range(max_entries):
            p = self.ptr_rva(rva + 8 * i)
            if p is None:
                break
            sec = self.section_of(p)
            if sec is None or not sec.executable:
                break
            out.append(p)
            # stop at next vtable's RTTI locator (heuristic): if the next slot is referenced by code as a vtable start
        return out

    def rtti_name(self, vtable_rva):
        """MSVC RTTI: vtable[-1] -> CompleteObjectLocator -> TypeDescriptor name."""
        col = self.ptr_rva(vtable_rva - 8)
        if col is None:
            return None
        sig = self.u32(col)
        if sig != 1:
            return None
        td_rva = self.u32(col + 12)
        if not (0 < td_rva < self.size):
            return None
        return self.cstr(td_rva + 16, 200)


def hexs(x):
    return f"0x{x:x}" if x is not None else "None"


_IMG = None


def image(path=None) -> Image:
    global _IMG
    if _IMG is None or (path and _IMG.path != path):
        _IMG = Image(path)
    return _IMG
