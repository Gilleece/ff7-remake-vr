"""Views the output of the engine module's one-frame GPU trace (dev pipe `gpu trace`).

usage:
  gpu_trace_view.py png <prefix> [--from N] [--to M]
      converts <prefix>_<seq>.rgba read-backs to PNG
  gpu_trace_view.py sheet <prefix> <out.png> [--from N] [--to M] [--width 320] [--cols 6] [--half left|right]
      contact sheet of the read-backs, labelled with sequence number and call, optionally only
      the left or right half of each image (the two eyes of the side-by-side target)
  gpu_trace_view.py passes <prefix> [--min-us 20]
      GPU time per render target (summed over consecutive events that draw into it), largest first

The trace log <prefix>.txt has one line per call: sequence number, call, bound render targets
(rtN), depth target (ds), viewport (vp x y w h), shaders, shader resources (tN), constant
buffers (cbN) and the GPU time since the previous event. Textures are shown as
`<pointer> <w>x<h> f<dxgi format> [pool names]`.
"""
import argparse
import glob
import os
import re
import struct

import numpy as np
from PIL import Image, ImageDraw


def read_rgba(path):
    with open(path, "rb") as f:
        magic, w, h, fmt = struct.unpack("<4I", f.read(16))
        if magic != 0x31445447:
            raise ValueError(f"{path}: not a trace read-back")
        data = np.frombuffer(f.read(w * h * 4), dtype=np.uint8).reshape(h, w, 4)
    return data, fmt


def read_log(prefix):
    lines = {}
    with open(prefix + ".txt", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"(\d+) (.*)", line)
            if m:
                lines[int(m.group(1))] = m.group(2).rstrip()
    return lines


def dumps(prefix, lo, hi):
    out = []
    for p in sorted(glob.glob(glob.escape(prefix) + "_*.rgba")):
        seq = int(re.search(r"_(\d+)\.rgba$", p).group(1))
        if lo <= seq <= hi:
            out.append((seq, p))
    return out


def cmd_png(a):
    for seq, p in dumps(a.prefix, a.lo, a.hi):
        img, _ = read_rgba(p)
        Image.fromarray(img[:, :, :3]).save(p[:-5] + ".png")
        print(p[:-5] + ".png")


def cmd_sheet(a):
    log = read_log(a.prefix)
    items = dumps(a.prefix, a.lo, a.hi)
    if not items:
        raise SystemExit("no read-backs in range")
    tiles = []
    for seq, p in items:
        img, _ = read_rgba(p)
        img = img[:, :, :3]
        if a.half == "left":
            img = img[:, : img.shape[1] // 2]
        elif a.half == "right":
            img = img[:, img.shape[1] // 2:]
        im = Image.fromarray(np.ascontiguousarray(img))
        th = max(1, int(im.height * a.width / im.width))
        tiles.append((seq, im.resize((a.width, th), Image.BILINEAR)))
    th = max(t.height for _, t in tiles)
    cols = min(a.cols, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * a.width, rows * (th + 26)), (0, 0, 0))
    d = ImageDraw.Draw(sheet)
    for i, (seq, t) in enumerate(tiles):
        x, y = (i % cols) * a.width, (i // cols) * (th + 26)
        sheet.paste(t, (x, y + 26))
        text = log.get(seq, "")
        call = text.split(" | ")[0]
        rt = re.search(r"\| (rt0|u0) [0-9A-Fa-fx]+ (\S+) f(\d+)[^|]*?(\[[^\]]*\])?", text)
        d.text((x + 3, y + 1), f"{seq} {call}"[:48], fill=(255, 255, 0))
        if rt:
            d.text((x + 3, y + 13), f"{rt.group(2)} f{rt.group(3)} {rt.group(4) or ''}"[:48], fill=(0, 255, 255))
    sheet.save(a.out)
    print(a.out, sheet.size, len(tiles), "tiles")


def cmd_passes(a):
    log = read_log(a.prefix)
    groups = []
    for seq in sorted(log):
        text = log[seq]
        t = re.search(r"gpu_us (-?[\d.]+)", text)
        us = float(t.group(1)) if t else 0.0
        rt = re.search(r"\| (rt0|u0) ([0-9A-Fa-fx]+) (\S+) f(\d+)[^|]*?(\[[^\]]*\])?", text)
        key = f"{rt.group(3)} f{rt.group(4)} {rt.group(5) or rt.group(2)}" if rt else text.split(" ")[0]
        if groups and groups[-1][0] == key:
            groups[-1][1] += us
            groups[-1][3] = seq
            groups[-1][4] += 1
        else:
            groups.append([key, us, seq, seq, 1])
    total = sum(g[1] for g in groups)
    print(f"total {total:.0f} us over {len(log)} events")
    for key, us, s0, s1, n in sorted(groups, key=lambda g: -g[1]):
        if us < a.min_us:
            break
        print(f"{us:8.0f} us {100 * us / total:5.1f}%  seq {s0}-{s1} ({n})  {key}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("png")
    p.add_argument("prefix")
    s = sub.add_parser("sheet")
    s.add_argument("prefix")
    s.add_argument("out")
    s.add_argument("--width", type=int, default=320)
    s.add_argument("--cols", type=int, default=6)
    s.add_argument("--half", choices=["left", "right"])
    q = sub.add_parser("passes")
    q.add_argument("prefix")
    q.add_argument("--min-us", type=float, default=20.0)
    for x in (p, s):
        x.add_argument("--from", dest="lo", type=int, default=0)
        x.add_argument("--to", dest="hi", type=int, default=1 << 30)
    a = ap.parse_args()
    {"png": cmd_png, "sheet": cmd_sheet, "passes": cmd_passes}[a.cmd](a)


if __name__ == "__main__":
    main()
