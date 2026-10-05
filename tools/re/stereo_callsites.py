"""List every place the engine calls through GEngine->StereoRenderingDevice.

Finds loads of [reg + STEREO_DEVICE_OFFSET] in functions that also reference
GEngine, then follows the loaded pointer through `mov rX, [dev]` (vtable load)
to `call [rX + slot*8]`. Prints one line per call site with the vtable slot,
plus the slots that are called on the object returned by GetRenderTargetManager.

Usage: python tools/re/stereo_callsites.py [path-to-ff7remake_.exe]
"""
from __future__ import annotations

import collections
import sys

from capstone.x86 import X86_OP_MEM, X86_OP_REG

from ff7re import image
from signatures import resolve_all

# 64-bit register families so that `mov rcx, rax` style copies are tracked.
_FAMILY = {}
for fam in [("rax", "eax"), ("rbx", "ebx"), ("rcx", "ecx"), ("rdx", "edx"), ("rsi", "esi"), ("rdi", "edi"),
            ("rbp", "ebp"), ("r8", "r8d"), ("r9", "r9d"), ("r10", "r10d"), ("r11", "r11d"), ("r12", "r12d"),
            ("r13", "r13d"), ("r14", "r14d"), ("r15", "r15d")]:
    for r in fam:
        _FAMILY[r] = fam[0]

VOLATILE = {"rax", "rcx", "rdx", "r8", "r9", "r10", "r11"}


def reg_name(img, reg_id):
    return _FAMILY.get(img.md.reg_name(reg_id), img.md.reg_name(reg_id))


def find_disp_loads(img, disp):
    """Instructions in .text whose memory operand is [base + disp] (no RIP)."""
    needle = disp.to_bytes(4, "little")
    out = []
    t = img.text
    for hit in img.find_bytes(needle, t.rva, t.end, limit=200000):
        for back in (2, 3, 4):
            insn = img.insn_at(hit - back)
            if insn is None or insn.address + insn.size not in (hit + 4, hit + 5, hit + 8):
                continue
            ok = False
            for op in insn.operands:
                if op.type == X86_OP_MEM and op.mem.disp == disp and op.mem.base != 0 and img.md.reg_name(op.mem.base) != "rip":
                    ok = True
            if ok:
                out.append(insn)
                break
    return out


def follow(img, start_insn, dev_reg, max_insns=60):
    """From an instruction that loaded the device pointer into dev_reg, find vtable calls on it.

    Returns a list of (call_rva, slot, kind) where kind is 'device' or 'rtm' (call on the
    pointer returned by a device call, i.e. GetRenderTargetManager's result)."""
    calls = []
    dev = {dev_reg}
    vt = set()
    rtm = set()
    rtm_vt = set()
    last_dev_slot = None
    rtm_src = None
    pc = start_insn.address + start_insn.size
    for _ in range(max_insns):
        insn = img.insn_at(pc)
        if insn is None:
            break
        m = insn.mnemonic
        ops = insn.operands
        if m == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG:
            dst = reg_name(img, ops[0].reg)
            src = ops[1]
            for s in (dev, vt, rtm, rtm_vt):
                s.discard(dst)
            if src.type == X86_OP_REG:
                r = reg_name(img, src.reg)
                if r in dev:
                    dev.add(dst)
                if r in vt:
                    vt.add(dst)
                if r in rtm:
                    rtm.add(dst)
            elif src.type == X86_OP_MEM and src.mem.disp == 0 and src.mem.index == 0 and src.mem.base:
                b = reg_name(img, src.mem.base)
                if b in dev:
                    vt.add(dst)
                if b in rtm:
                    rtm_vt.add(dst)
        elif m == "call":
            if ops and ops[0].type == X86_OP_MEM and ops[0].mem.base and img.md.reg_name(ops[0].mem.base) != "rip":
                b = reg_name(img, ops[0].mem.base)
                slot = ops[0].mem.disp // 8
                if b in vt:
                    calls.append((insn.address, slot, "device"))
                    last_dev_slot = slot
                elif b in rtm_vt:
                    calls.append((insn.address, slot, f"ret{rtm_src}"))
            # calls clobber volatile registers; rax holds the return value
            for s in (dev, vt, rtm, rtm_vt):
                s -= VOLATILE
            if last_dev_slot is not None and calls and calls[-1][0] == insn.address and calls[-1][2] == "device":
                rtm.add("rax")
                rtm_src = last_dev_slot
        elif m in ("ret", "int3") or m == "jmp":
            break
        elif ops and ops[0].type == X86_OP_REG and m not in ("cmp", "test", "push"):
            dst = reg_name(img, ops[0].reg)
            for s in (dev, vt, rtm, rtm_vt):
                s.discard(dst)
        pc += insn.size
    return calls


def main():
    img = image(sys.argv[1] if len(sys.argv) > 1 else None)
    r = resolve_all(img, quiet=True)
    gengine = r["GEngine"]
    off = r["UEngine::StereoRenderingDevice"]
    print(f"GEngine RVA {gengine:#x}, StereoRenderingDevice offset {off:#x}")
    gengine_funcs = set()
    for a, _m, _o in img.code_refs(gengine):
        f = img.func_containing(a)
        if f:
            gengine_funcs.add(f[0])
    loads = find_disp_loads(img, off)
    by_slot = collections.defaultdict(list)
    rtm_by_slot = collections.defaultdict(list)  # (source device slot, slot) -> sites
    for insn in loads:
        f = img.func_containing(insn.address)
        fstart = f[0] if f else None
        if fstart not in gengine_funcs:
            continue
        ops = insn.operands
        if insn.mnemonic != "mov" or ops[0].type != X86_OP_REG:
            continue
        dev_reg = reg_name(img, ops[0].reg)
        for call_rva, slot, kind in follow(img, insn, dev_reg):
            if kind == "device":
                by_slot[slot].append((call_rva, fstart))
            else:
                rtm_by_slot[(int(kind[3:]), slot)].append((call_rva, fstart))
    print("\nIStereoRendering slots called through GEngine->StereoRenderingDevice:")
    for slot in sorted(by_slot):
        sites = by_slot[slot]
        funcs = sorted({f for _c, f in sites})
        print(f"  slot {slot:2d} (+{slot * 8:#05x}): {len(sites):3d} call sites in {len(funcs):3d} functions; e.g. "
              + ", ".join(f"{c:#x}" for c, _f in sites[:6]))
    print("\nSlots called on the pointer returned by a device call")
    print("(in this build device slot 12 is GetRenderTargetManager and 13 is GetStereoLayers):")
    for (src, slot) in sorted(rtm_by_slot):
        sites = rtm_by_slot[(src, slot)]
        print(f"  device slot {src:2d} result -> slot {slot:2d} (+{slot * 8:#05x}): {len(sites):3d} call sites; e.g. "
              + ", ".join(f"{c:#x}" for c, _f in sites[:6]))


if __name__ == "__main__":
    main()
