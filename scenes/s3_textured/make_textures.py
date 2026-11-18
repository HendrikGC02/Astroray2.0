# SPDX-License-Identifier: Apache-2.0
"""The 32 textures of S3 (B-TX): 2048 x 2048 RGBA8 sRGB PNG files from eight procedural patterns,
four parameter sets each, generated deterministically (NumPy only, fixed seeds, no clock) into
out/scenes/s3_textures/tex00.png .. tex31.png. No external asset.
Usage: make_textures.py [--out <dir>]; `ensure(out)` is what build.py calls: it regenerates only when
this file's hash differs from the stamp beside the textures.
The PNG writer is the stdlib's (zlib, struct): filter type 0, one IDAT.
Ref: PNG specification 1.2 (chunk layout, CRC-32, filter type 0); the patterns are textbook
(checker, stripes, rings, bricks, dots, fractal value noise, Worley cells, marble)."""
import argparse
import hashlib
import os
import struct
import zlib

import numpy as np

SIZE = 2048
COUNT = 32
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.dirname(HERE)), "out", "scenes", "s3_textures")
PATTERNS = ("checker", "stripes", "rings", "bricks", "dots", "fbm", "cells", "marble")


def _grid():
    c = (np.arange(SIZE, dtype=np.float32) + 0.5) / SIZE
    return np.meshgrid(c, c, indexing="xy")  # u varies along columns, v along rows


def _palette(rng):
    """Two well separated colours (dark and light), so every pattern has contrast."""
    hue = rng.random(3).astype(np.float32)
    a = 0.05 + 0.35 * hue
    b = 0.55 + 0.45 * np.roll(hue, 1)
    return a, b


def _mix(t, a, b):
    t = np.clip(t, 0.0, 1.0)[..., None]
    return a * (1.0 - t) + b * t


def _value_noise(rng, cells):
    """Smoothly interpolated lattice noise in [0, 1], `cells` lattice cells across the texture."""
    u, v = _grid()
    lat = rng.random((cells + 2, cells + 2)).astype(np.float32)
    x, y = u * cells, v * cells
    i, j = x.astype(np.int32), y.astype(np.int32)
    fx, fy = x - i, y - j
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    return (lat[j, i] * (1 - fx) * (1 - fy) + lat[j, i + 1] * fx * (1 - fy)
            + lat[j + 1, i] * (1 - fx) * fy + lat[j + 1, i + 1] * fx * fy)


def _fbm(rng, base_cells, octaves=5):
    total, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        total = total + amp * _value_noise(rng, base_cells * 2 ** o)
        norm += amp
        amp *= 0.5
    return total / norm


def pattern(index):
    """The float RGB image (SIZE, SIZE, 3) of texture `index`: pattern index % 8, variant index // 8."""
    kind, variant = PATTERNS[index % len(PATTERNS)], index // len(PATTERNS)
    rng = np.random.default_rng(1000 + index)
    a, b = _palette(rng)
    u, v = _grid()
    n = 4 * 2 ** variant  # a coarse-to-fine scale per variant: 4, 8, 16, 32 repeats
    if kind == "checker":
        t = ((np.floor(u * n) + np.floor(v * n)) % 2).astype(np.float32)
    elif kind == "stripes":
        t = (np.sin(2 * np.pi * n * (u + 0.5 * v)) > 0).astype(np.float32)
    elif kind == "rings":
        r = np.hypot(u - 0.5, v - 0.5)
        t = 0.5 + 0.5 * np.sin(2 * np.pi * n * r)
    elif kind == "bricks":
        row = np.floor(v * n)
        x = u * n * 0.5 + 0.5 * (row % 2)
        fx, fy = x - np.floor(x), v * n - row
        mortar = (fx < 0.06) | (fy < 0.12)
        t = np.where(mortar, 0.0, 0.55 + 0.45 * rng.random((n, n)).astype(np.float32)[
            row.astype(np.int32) % n, np.floor(x).astype(np.int32) % n])
    elif kind == "dots":
        fx, fy = (u * n) % 1.0 - 0.5, (v * n) % 1.0 - 0.5
        t = (np.hypot(fx, fy) < 0.32).astype(np.float32)
    elif kind == "fbm":
        t = _fbm(rng, n)
    elif kind == "cells":
        # Worley: distance to the nearest of one jittered point per cell, 3 x 3 neighbourhood.
        pts = rng.random((n, n, 2)).astype(np.float32)
        x, y = u * n, v * n
        ci, cj = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
        best = np.full(u.shape, 4.0, np.float32)
        for dj in (-1, 0, 1):
            for di in (-1, 0, 1):
                ii, jj = ci + di, cj + dj
                p = pts[jj % n, ii % n]
                best = np.minimum(best, np.hypot(x - (ii + p[..., 0]), y - (jj + p[..., 1])))
        t = np.clip(best, 0.0, 1.0)
    else:  # marble
        t = 0.5 + 0.5 * np.sin(2 * np.pi * (n * 0.25 * u + 3.0 * _fbm(rng, 4)))
    return _mix(t, a, b).astype(np.float32)


def srgb_encode(lin):
    """Linear [0, 1] to 8-bit sRGB (IEC 61966-2-1): the textures hold sRGB, the engine decodes it."""
    lin = np.clip(lin, 0.0, 1.0)
    s = np.where(lin <= 0.0031308, 12.92 * lin, 1.055 * np.power(lin, 1 / 2.4) - 0.055)
    return np.rint(s * 255.0).astype(np.uint8)


def png_bytes(rgba):
    """An 8-bit RGBA PNG of an (H, W, 4) uint8 array."""
    h, w, _ = rgba.shape
    raw = np.concatenate([np.zeros((h, 1), np.uint8), rgba.reshape(h, w * 4)], axis=1).tobytes()

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 3)) + chunk(b"IEND", b""))


def texture_path(out, index):
    return os.path.join(out, f"tex{index:02d}.png")


def _source_hash():
    with open(os.path.abspath(__file__), "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def ensure(out=DEFAULT_OUT):
    """Writes the 32 textures unless the stamp says they are current. Returns the paths."""
    os.makedirs(out, exist_ok=True)
    stamp = os.path.join(out, "stamp.txt")
    paths = [texture_path(out, i) for i in range(COUNT)]
    if os.path.exists(stamp) and open(stamp, encoding="ascii").read().strip() == _source_hash() \
            and all(os.path.exists(p) for p in paths):
        return paths
    for i, p in enumerate(paths):
        rgb = srgb_encode(pattern(i))
        rgba = np.concatenate([rgb, np.full(rgb.shape[:2] + (1,), 255, np.uint8)], axis=2)
        with open(p + ".partial", "wb") as f:
            f.write(png_bytes(rgba))
        os.replace(p + ".partial", p)
    with open(stamp, "w", encoding="ascii") as f:
        f.write(_source_hash() + "\n")
    return paths


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args()
    print(f"{len(ensure(a.out))} textures in {a.out}")
