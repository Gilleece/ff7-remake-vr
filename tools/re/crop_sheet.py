"""Side-by-side crops of eye captures, for comparing settings by eye.

    python crop_sheet.py <out.png> --region x,y,w,h [--region ...] [--scale 2] \
        [--diff] label=path.png [label=path.png ...]

One row per region, one column per image (labelled). --diff adds, per row, the absolute
difference of each image against the first (amplified 4x), to show where a setting
changes the picture. Also prints, per image and region, the mean luma and a sharpness
figure (mean absolute Laplacian of luma) so the comparison has numbers too.
"""
import argparse
import sys

import numpy as np
from PIL import Image, ImageDraw


def luma(a):
    return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722


def sharpness(y):
    lap = 4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:]
    return float(np.mean(np.abs(lap)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("images", nargs="+")
    ap.add_argument("--region", action="append", required=True)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--diff", action="store_true")
    args = ap.parse_args()
    items = []
    for spec in args.images:
        label, _, path = spec.partition("=")
        items.append((label, np.asarray(Image.open(path).convert("RGB")).astype(np.float32)))
    regions = [tuple(int(v) for v in r.split(",")) for r in args.region]
    s = args.scale
    cols = len(items) + (len(items) - 1 if args.diff else 0)
    cw = max(r[2] for r in regions) * s
    rh = [r[3] * s for r in regions]
    pad, head = 4, 18
    sheet = Image.new("RGB", (cols * (cw + pad), sum(h + head + pad for h in rh)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    y0 = 0
    for ri, (x, y, w, h) in enumerate(regions):
        base = items[0][1][y:y + h, x:x + w]
        col = 0
        for li, (label, a) in enumerate(items):
            c = a[y:y + h, x:x + w]
            yl = luma(c)
            print(f"region {ri} ({x},{y},{w},{h}) {label}: mean luma {yl.mean():.2f} sharpness {sharpness(yl):.3f}")
            img = Image.fromarray(c.astype(np.uint8)).resize((w * s, h * s), Image.NEAREST)
            sheet.paste(img, (col * (cw + pad), y0 + head))
            draw.text((col * (cw + pad) + 2, y0 + 2), f"r{ri} {label}", fill=(255, 255, 0))
            col += 1
            if args.diff and li > 0:
                d = np.clip(np.abs(c - base) * 4, 0, 255).astype(np.uint8)
                print(f"region {ri} {label} vs {items[0][0]}: mean abs diff {np.abs(c - base).mean():.3f}")
                sheet.paste(Image.fromarray(d).resize((w * s, h * s), Image.NEAREST), (col * (cw + pad), y0 + head))
                draw.text((col * (cw + pad) + 2, y0 + 2), f"|{label}-{items[0][0]}| x4", fill=(255, 128, 0))
                col += 1
        y0 += rh[ri] + head + pad
    sheet.save(args.out)
    print("wrote", args.out)


if __name__ == "__main__":
    sys.exit(main())
