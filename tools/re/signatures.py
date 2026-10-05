"""Signature database for ff7remake_.exe: generation, resolution and verification.

signatures.json holds one entry per engine fact. Every entry has an IDA-style
byte pattern that must match exactly once in .text, plus a rule that turns the
match into the value we want. The rules use the same arguments as the C++
helper `ff7vr::pattern::rip(insn, disp_offset, insn_len)` so the runtime code
can apply them unchanged:

  "resolve": {"type": "rip",   "insn": 7, "disp": 3, "len": 7}
        value = RVA of (match + insn) + len + int32 at (match + insn + disp)
        Used for globals (mov reg,[rip+x]), vtables (lea reg,[rip+x]) and
        call targets (E8 rel32: disp 1, len 5). An optional "add" is added to
        the result (when the code addresses a member of a global object).
  "resolve": {"type": "match", "offset": 0}
        value = RVA of (match + offset). Used when the pattern sits at a
        function start or at a patch site.
  "resolve": {"type": "i32", "offset": 3}  /  {"type": "u8", "offset": 3}
        value = the signed 32-bit or unsigned 8-bit number stored at
        match + offset. Used for structure offsets and vtable slot offsets
        (divide by 8 for the slot index).

Each entry also records `expect` (the value for the build in
`build`), so a scan on a different build reports what moved.

Usage:
  python tools/re/signatures.py [exe]            resolve and check every entry
  python tools/re/signatures.py --make RVA [exe] print a unique signature
                                                 starting at the instruction at RVA
"""
from __future__ import annotations

import json
import os
import re
import sys

from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

from ff7re import Image, image

HERE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(HERE, "signatures.json")


# ---------------------------------------------------------------- patterns

def parse(sig: str):
    pat, mask = bytearray(), bytearray()
    for t in sig.split():
        if t in ("?", "??"):
            pat.append(0)
            mask.append(0)
        else:
            pat.append(int(t, 16))
            mask.append(1)
    return bytes(pat), bytes(mask)


def _regex(sig: str):
    pat, mask = parse(sig)
    return re.compile(b"".join(re.escape(bytes([b])) if m else b"." for b, m in zip(pat, mask)), re.DOTALL)


def scan(img: Image, sig: str, limit=8, section=None):
    """RVAs where sig matches in .text (or the given section)."""
    sec = section or img.text
    rx = _regex(sig)
    out = []
    view = img.mem[sec.rva:sec.end]
    for m in rx.finditer(view):
        out.append(sec.rva + m.start())
        if len(out) >= limit:
            break
    return out


def _insn_wildcards(img: Image, insn):
    """Byte positions (relative to insn start) that must be wildcarded: rel32 branch
    targets and RIP-relative displacements (both change whenever code moves)."""
    raw = bytes(insn.bytes)
    wild = set()
    m = insn.mnemonic
    if (m == "call" or m.startswith("j")) and insn.operands and insn.operands[0].type == X86_OP_IMM:
        if insn.size >= 5 and raw[0] in (0xE8, 0xE9):
            wild.update(range(1, 5))
        elif insn.size == 6 and raw[0] == 0x0F and 0x80 <= raw[1] <= 0x8F:
            wild.update(range(2, 6))
    for op in insn.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            # displacement is the 4 bytes just before any trailing immediate
            imm_size = insn.size - insn.disp_offset - 4 if insn.disp_offset else 0
            start = insn.disp_offset if insn.disp_offset else insn.size - 4 - imm_size
            wild.update(range(start, start + 4))
    return wild


def make_sig(img: Image, rva: int, max_insns=24, extra_wild=None):
    """Shortest instruction-aligned signature starting at rva that is unique in .text.

    extra_wild: optional set of absolute RVAs to wildcard as well (for example the
    displacement of a structure offset we want to read out of the match)."""
    parts = []
    pc = rva
    for _ in range(max_insns):
        insn = img.insn_at(pc)
        if insn is None:
            break
        wild = _insn_wildcards(img, insn)
        for i, b in enumerate(insn.bytes):
            if i in wild or (extra_wild and pc + i in extra_wild):
                parts.append("??")
            else:
                parts.append(f"{b:02X}")
        pc += insn.size
        if sum(1 for p in parts if p != "??") >= 6:
            sig = " ".join(parts)
            hits = scan(img, sig, limit=2)
            if hits == [rva]:
                return sig
    return None


# ---------------------------------------------------------------- resolution

def apply_rule(img: Image, match: int, rule: dict):
    t = rule["type"]
    if t == "rip":
        insn = match + rule.get("insn", 0)
        return insn + rule["len"] + img.i32(insn + rule["disp"]) + rule.get("add", 0)
    if t == "match":
        return match + rule.get("offset", 0)
    if t == "i32":
        return img.i32(match + rule["offset"])
    if t == "u8":
        return img.mem[match + rule["offset"]]
    raise ValueError(f"unknown rule type {t}")


def load_db(path=DB_PATH):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _parse_num(v):
    if isinstance(v, str):
        return int(v, 0)
    return v


def resolve_entry(img: Image, e: dict):
    """Returns (value, matches, error)."""
    hits = scan(img, e["pattern"], limit=4)
    if len(hits) != 1:
        return None, hits, f"pattern matched {len(hits)} times"
    try:
        return apply_rule(img, hits[0], e["resolve"]), hits, None
    except Exception as ex:  # noqa: BLE001
        return None, hits, str(ex)


def resolve_all(img: Image | None = None, quiet=False, db=None):
    """Resolve every entry. Returns {name: value}. Falls back to `expect` when the
    pattern fails (with a warning) so dependent tools still run on this build."""
    img = img or image()
    db = db or load_db()
    out = {}
    for e in db["entries"]:
        val, hits, err = resolve_entry(img, e)
        exp = _parse_num(e.get("expect"))
        if err:
            if not quiet:
                print(f"WARN {e['name']}: {err}; using expected value", file=sys.stderr)
            val = exp
        out[e["name"]] = val
    return out


def report(img: Image, db: dict):
    ok = bad = moved = 0
    width = max(len(e["name"]) for e in db["entries"])
    for e in db["entries"]:
        val, hits, err = resolve_entry(img, e)
        exp = _parse_num(e.get("expect"))
        kind = e.get("kind", "")
        if err:
            bad += 1
            status = f"FAIL ({err})"
            vs = "-"
        else:
            vs = f"{val:#x}" if isinstance(val, int) else str(val)
            if exp is not None and val != exp:
                moved += 1
                status = f"CHANGED (expected {exp:#x})"
            else:
                ok += 1
                status = "ok"
        print(f"{e['name']:<{width}}  {kind:<8} {vs:>12}  {status}")
    print(f"\n{ok} ok, {moved} changed, {bad} failed, {len(db['entries'])} total")
    return bad == 0


def main(argv):
    if len(argv) >= 2 and argv[0] == "--make":
        rva = int(argv[1], 0)
        img = image(argv[2] if len(argv) > 2 else None)
        print(make_sig(img, rva))
        return 0
    img = image(argv[0] if argv else None)
    db = load_db()
    b = db.get("build", {})
    print(f"exe: {img.path}")
    print(f"signatures recorded against file version {b.get('file_version')}, sha256 {b.get('sha256', '')[:16]}...\n")
    return 0 if report(img, db) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
