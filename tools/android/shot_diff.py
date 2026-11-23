"""Frame difference of two screenshots of the same spot (A/B runs from a bench save with the camera still): mean
absolute difference per channel (0-255), the share of pixels differing by more than 24, and the worst 32x32 tile.
Animated things (fire, particles, foliage) differ between any two runs: compare against two runs of the same variant.
Optionally writes an amplified difference image.
  out/venv/Scripts/python.exe -I tools/android/shot_diff.py <a.png> <b.png> [<diff.png>]
"""
import sys

import numpy as np
from PIL import Image


def main():
    a = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(sys.argv[2]).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        print(f"size differs: {a.shape} vs {b.shape}")
        return 1
    d = np.abs(a - b)
    per_pixel = d.max(axis=2)
    h, w = per_pixel.shape
    worst, where = 0.0, (0, 0)
    for y in range(0, h - 31, 32):
        for x in range(0, w - 31, 32):
            m = float(per_pixel[y:y + 32, x:x + 32].mean())
            if m > worst:
                worst, where = m, (x, y)
    print(f"mean abs diff {d.mean():.2f}, pixels > 24: {100.0 * (per_pixel > 24).mean():.2f} %, "
          f"worst 32x32 tile {worst:.1f} at {where}")
    if len(sys.argv) > 3:
        Image.fromarray(np.clip(per_pixel * 4, 0, 255).astype(np.uint8)).save(sys.argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main())
