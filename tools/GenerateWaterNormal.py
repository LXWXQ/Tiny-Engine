#!/usr/bin/env python3
"""Generate the tileable water normal map used by the water pass.

The texture deliberately keeps only high-frequency slope detail (K >= --cutoff).
Low-frequency FFT ridges repeat with the 18 m tiling period and, at a low render
scale (for example the editor viewport at ~0.37x), project to a visible grid of
bright/dark patches. The large waves already provide the low-frequency shape, so
removing it here leaves only sub-pixel ripples that mip filtering can average.

Usage:
    python tools/GenerateWaterNormal.py
    python tools/GenerateWaterNormal.py --out Resources/Texture/Water/water_normal.png
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image


def generate(size: int, cutoff: float, exponent: float, slope_rms: float,
             seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    kx = np.fft.fftfreq(size, d=1.0 / size)
    ky = np.fft.fftfreq(size, d=1.0 / size)
    kx_grid, ky_grid = np.meshgrid(kx, ky)
    k = np.hypot(kx_grid, ky_grid)
    k[0, 0] = 1.0

    spectrum = np.fft.fft2(rng.standard_normal((size, size)))
    amplitude = np.power(k, -exponent)
    amplitude *= np.exp(-np.square(k / 260.0))
    amplitude[k < cutoff] = 0.0
    spectrum *= amplitude

    gx = np.real(np.fft.ifft2(spectrum * (1j * kx_grid)))
    gy = np.real(np.fft.ifft2(spectrum * (1j * ky_grid)))
    gx = (gx - gx.mean()) / gx.std() * slope_rms
    gy = (gy - gy.mean()) / gy.std() * slope_rms

    inv = 1.0 / np.sqrt(gx * gx + gy * gy + 1.0)
    nx, ny = -gx * inv, -gy * inv
    nz = inv
    rgb = np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, nz * 0.5 + 0.5], axis=-1)
    return np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path,
                        default=Path("Resources/Texture/Water/water_normal.png"))
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--cutoff", type=float, default=32.0,
                        help="remove FFT bins with K below this value")
    parser.add_argument("--exponent", type=float, default=1.9)
    parser.add_argument("--slope-rms", type=float, default=0.22)
    parser.add_argument("--seed", type=int, default=44)
    args = parser.parse_args()

    image = generate(args.size, args.cutoff, args.exponent, args.slope_rms, args.seed)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(image, "RGB").save(args.out)
    print(f"wrote {args.out} ({args.size}x{args.size}, cutoff K<{args.cutoff:g}, seed={args.seed})")


if __name__ == "__main__":
    main()
