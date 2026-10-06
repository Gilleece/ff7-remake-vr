"""Checks the [picture] colour adjustment on `capture <prefix>+raw` captures.

For each capture the frame's source texture (<prefix>_src.png, the engine's eye
image reduced to 8 bits) is run through a reference implementation of the
adjustment and compared with what the eye swapchain received
(<prefix>_rawL.png / _rawR.png), pixel by pixel. It also prints the statistics
the adjustment is expected to move (mean, percentiles, chroma).

    python tools/re/picture_check.py <prefix> [brightness contrast saturation gamma black_level]

Without values the defaults (no change) are assumed.
"""

import sys

import numpy as np
from PIL import Image


def srgb_to_linear(e):
    e = np.clip(e, 0.0, 1.0)
    return np.where(e <= 0.04045, e / 12.92, ((e + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def adjust(lin, brightness, contrast, saturation, gamma, black):
    """Same steps as AdjustPicture in src/xr/src/shaders/blit.hlsl."""
    lin = lin * 2.0 ** brightness
    y = lin @ np.array([0.2126, 0.7152, 0.0722])
    lin = np.maximum(y[..., None] + (lin - y[..., None]) * saturation, 0.0)
    lin = 0.18 * np.power(lin / 0.18, contrast)
    e = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1.0 / 2.4) - 0.055)
    e = np.power(e, 1.0 / gamma)
    e = np.maximum(e * (1.0 - black) + black, 0.0)
    return np.where(e <= 0.04045, e / 12.92, ((e + 0.055) / 1.055) ** 2.4)


def stats(name, img, band=None):
    """img: HxWx3 uint8 (sRGB encoded). band: mask of the pixels to report separately."""
    f = img.astype(np.float64)
    luma = f @ np.array([0.2126, 0.7152, 0.0722])
    chroma = f.max(axis=2) - f.min(axis=2)
    p = np.percentile(luma, [1, 50, 99])
    # Spread in stops: standard deviation of log2 of the linear luminance (pixels above black).
    y = srgb_to_linear(f / 255.0) @ np.array([0.2126, 0.7152, 0.0722])
    stops = np.log2(y[y > 1e-4]).std()
    extra = f"  mid-grey band mean {luma[band].mean():6.2f}" if band is not None else ""
    print(f"  {name:<10} mean {luma.mean():7.2f}  p1 {p[0]:6.2f}  p50 {p[1]:6.2f}  p99 {p[2]:6.2f}  "
          f"std {luma.std():6.2f}  stops std {stops:5.3f}  chroma mean {chroma.mean():6.2f} max {chroma.max():5.0f}{extra}")


def main():
    prefix = sys.argv[1]
    v = [float(x) for x in sys.argv[2:7]] if len(sys.argv) >= 7 else [0.0, 1.0, 1.0, 1.0, 0.0]
    src = np.asarray(Image.open(prefix + "_src.png").convert("RGB"))
    print(f"{prefix}: brightness {v[0]} contrast {v[1]} saturation {v[2]} gamma {v[3]} black_level {v[4]}")
    for eye, x0 in (("L", 0), ("R", src.shape[1] // 2)):
        raw = np.asarray(Image.open(f"{prefix}_raw{eye}.png").convert("RGB"))
        h, w = raw.shape[:2]
        s = src[:h, x0:x0 + w]
        pred = linear_to_srgb(adjust(srgb_to_linear(s / 255.0), *v)) * 255.0
        d = raw.astype(np.float64) - pred
        ad = np.abs(d)
        print(f" eye {eye} {w}x{h}: raw - reference: mean {d.mean():+.3f}  mean abs {ad.mean():.3f}  "
              f">1: {np.mean(ad > 1.0) * 100:.3f} %  >2: {np.mean(ad > 2.0) * 100:.4f} %  max {ad.max():.1f}")
        # Mid-grey: 18 % light is 118 of 255 encoded; grey-ish pixels within +-2 of it.
        sl = s.astype(np.float64) @ np.array([0.2126, 0.7152, 0.0722])
        band = (np.abs(sl - 118.0) <= 2.0) & ((s.max(axis=2).astype(int) - s.min(axis=2)) <= 12)
        print(f"  mid-grey band: {band.sum()} pixels")
        stats("source", s, band)
        stats("reference", np.clip(np.rint(pred), 0, 255).astype(np.uint8), band)
        stats("received", raw, band)


if __name__ == "__main__":
    main()
