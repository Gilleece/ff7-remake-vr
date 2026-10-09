"""Radial brightness ratio of two eye captures, to measure a vignette.

    python vignette_check.py <a.png> <b.png>

For rings around the image centre (radius in half the image's width and height, so 1.0 is
the middle of each edge and about 1.41 the corners) it prints the median of the per-pixel
ratio b / a in linear light (pixels darker than 0.2 % of white in a are left out) and the
ratio of the ring means. With a = the vignette on and b = off, a ratio above 1 that grows
towards the edge is the light the vignette took away.
"""
import sys

import numpy as np
from PIL import Image


def linear_luma(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(np.float64) / 255.0
    a = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = linear_luma(sys.argv[1]), linear_luma(sys.argv[2])
    h, w = a.shape
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot((xx - w / 2) / (w / 2), (yy - h / 2) / (h / 2))
    print(f"whole image: mean {a.mean():.5f} -> {b.mean():.5f} (x {b.mean() / a.mean():.3f})")
    for lo in np.arange(0.0, 1.5, 0.1):
        m = (r >= lo) & (r < lo + 0.1) & (a > 0.002)
        if m.sum() < 1000:
            continue
        print(f"ring {lo:.1f}-{lo + 0.1:.1f}: pixels {m.sum():8d}  median b/a {np.median(b[m] / a[m]):.3f}  "
              f"ratio of means {b[m].mean() / a[m].mean():.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
