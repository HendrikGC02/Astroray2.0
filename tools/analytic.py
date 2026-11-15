# SPDX-License-Identifier: Apache-2.0
"""Analytic scene harness (0009 d16): renders out/scenes/<name>.arrec through `astroray_cli dump`
at 32x32, reads each pixel's mean and variance of the mean from the dump and compares every pixel
with the closed-form truth of scenes/lib/truths.py.

Usage: <python> -I tools/analytic.py --scene <name> --device cpu|cuda [--spp N] [--seed S] [--png P]
Prints one line per scene channel and a final PASS or FAIL line with the worst pixel's z and a
reproduce command; exits non-zero on failure.

Rule per pixel: |mean - truth| <= z SE + eps_sys, SE = sqrt(VAR) (0008 d3, exact in the
sampler's independent mode, which the harness requires), eps_sys from meta.toml (default 0).
Also, per channel: not black where truth is not, not constant where truth varies.
Ref: docs/decisions/0009-build-test-process.md d15, d16; Sidak 1967 (thresholds).
"""
import argparse
import importlib.util
import math
import os
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import tomllib
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
SCENES_OUT = os.path.join(ROOT, "out", "scenes")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")
BUDGETS = os.path.join(ROOT, "tools", "budgets.toml")
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
import meta  # noqa: E402
import truths  # noqa: E402

SIZE = 32  # 0009 d11: analytic scenes at 32x32 in gate
# Lead ruling for 072: 1024 spp in gate, so a 1% emission error is about 7.7 SE per pixel in A5's
# green channel against the per-pixel threshold of about 5.5.
DEFAULT_SPP = 1024
FAMILY_ALPHA = 0.01  # 0009 d15
# tests/common/stat.cpp kRepoConstant and the FNV-1a / MixBits rule of seed_for() (task 020).
REPO_CONSTANT = 0x415354524159
MASK64 = (1 << 64) - 1


def fnv1a(text):
    # Ref: Fowler-Noll-Vo FNV-1a, 64-bit offset basis and prime (as tests/common/stat.cpp).
    h = 14695981039346656037
    for c in text.encode():
        h = ((h ^ c) * 1099511628211) & MASK64
    return h


def mix_bits(v):
    # Ref: pbrt-v4 src/pbrt/util/hash.h MixBits (as tests/common/stat.cpp).
    v ^= v >> 31
    v = (v * 0x7FB5D329728EA185) & MASK64
    v ^= v >> 27
    v = (v * 0x81DADEF4BC2DD44D) & MASK64
    v ^= v >> 33
    return v


def seed_for(name):
    """020's seed_for(name), ASTRORAY_SEED_SHA mixed in when set (full runs); the low 32 bits,
    because ar_settings.seed and the CLI's --seed are 32-bit."""
    h = mix_bits(fnv1a(name) ^ REPO_CONSTANT)
    sha = os.environ.get("ASTRORAY_SEED_SHA", "")
    if sha:
        h = mix_bits(h ^ fnv1a(sha))
    return h & 0xFFFFFFFF


def family_m():
    with open(BUDGETS, "rb") as f:
        b = tomllib.load(f)
    for table in ("family", "family_provisional"):
        if "analytic_scenes" in b.get(table, {}):
            return int(b[table]["analytic_scenes"])
    raise ValueError(f"{BUDGETS}: no analytic_scenes family size")


def sidak(alpha, n):
    """Per-test alpha so that n independent tests together have size alpha: 1 - (1 - alpha)^(1/n).
    Ref: Sidak 1967. expm1/log1p keep precision for tiny results."""
    return -math.expm1(math.log1p(-alpha) / n)


def pixel_z(m, pixels):
    """Two-sided per-pixel critical value. Each scene channel is one member of the analytic family
    (size m, family alpha 0.01), so its test has size alpha_t = sidak(0.01, m); the channel passes
    only if all its pixels pass, and its pixel means are independent, so each pixel is tested at
    sidak(alpha_t, pixels) (Sidak 1967 twice). m = 256, 1024 pixels: z = 5.50."""
    a = sidak(sidak(FAMILY_ALPHA, m), pixels)
    return -statistics.NormalDist().inv_cdf(a / 2.0)


def check_channel(mean, var, truth, z, eps_sys):
    """One scene channel. mean, var (variance of the mean) and truth are (h, w) arrays. Returns
    (pass, reasons, worst) with worst = (z of the worst pixel, (x, y)), the worst pixel being the one
    with the largest |mean - truth| / (z SE + eps_sys)."""
    mean, var, truth = (np.asarray(a, dtype=np.float64) for a in (mean, var, truth))
    reasons = []
    if not (np.isfinite(mean).all() and np.isfinite(var).all()) or (var < 0).any():
        return False, ["non-finite mean or variance, or negative variance"], (math.inf, (0, 0))
    se = np.sqrt(var)
    d = mean - truth
    tol = z * se + eps_sys
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.where(tol > 0, np.abs(d) / tol, np.where(d == 0, 0.0, np.inf))
        zs = np.where(se > 0, d / se, np.where(d == 0, 0.0, np.copysign(np.inf, d)))
    y, x = np.unravel_index(int(np.argmax(ratio)), ratio.shape)
    worst = (float(zs[y, x]), (int(x), int(y)))
    bad = int((ratio > 1).sum())
    if bad:
        reasons.append(f"{bad} pixels outside z SE + eps_sys")
    lit = truth != 0
    if lit.any() and not (mean[lit] != 0).any():
        reasons.append("black where truth is not")
    if truth.max() != truth.min() and mean.max() == mean.min():
        reasons.append("constant where truth varies")
    return not reasons, reasons, worst


def load_dump(path, spp):
    spec = importlib.util.spec_from_file_location("_fits_read_analytic", FITS_READ)
    fr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fr)
    with open(path, "rb") as f:
        hdus = fr.read_hdus(f.read())
    head, planes = hdus[0][0], {h["EXTNAME"]: d for h, d in hdus[1:]}
    if head.get("SAMPLER") != "independent":
        raise RuntimeError(f"the dump's SAMPLER is {head.get('SAMPLER')!r}: the harness needs the exact "
                           "variance of independent mode (0008 d3); set sampler='independent' in the builder")
    if head.get("SPP") != spp:
        raise RuntimeError(f"the dump has SPP {head.get('SPP')}, asked for {spp}")
    mean = np.stack([planes[f"display.{c}"] for c in range(3)]).astype(np.float64)
    var = np.stack([planes[f"display.{c}.VAR"] for c in range(3)]).astype(np.float64)
    nsamp = np.stack([planes[f"display.{c}.NSAMP"] for c in range(3)])
    if mean.shape != (3, SIZE, SIZE) or not (nsamp == spp).all():
        raise RuntimeError(f"the dump is {mean.shape[1:]} with NSAMP {np.unique(nsamp)}, expected {SIZE}x{SIZE} at {spp}")
    return mean, var


def write_png(path, rgb, scale, enlarge=8):
    """rgb (3, h, w), rows bottom-up (the film's order), times `scale`, sRGB-encoded 8-bit, each
    pixel enlarged to enlarge x enlarge. Ref: IEC 61966-2-1 (sRGB transfer); PNG ISO/IEC 15948."""
    v = np.clip(np.asarray(rgb, dtype=np.float64) * scale, 0.0, 1.0)
    v = np.where(v <= 0.0031308, 12.92 * v, 1.055 * v ** (1 / 2.4) - 0.055)
    img = np.round(v * 255).astype(np.uint8).transpose(1, 2, 0)[::-1]
    img = img.repeat(enlarge, axis=0).repeat(enlarge, axis=1)
    h, w = img.shape[:2]
    raw = b"".join(b"\x00" + img[r].tobytes() for r in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def run(scene, device, spp, seed, png=None):
    """Renders and checks one scene; prints its lines and returns True on PASS."""
    t0 = time.perf_counter()
    m = meta.load(os.path.join(ROOT, "scenes", scene, "meta.toml"))
    if m["class"] != "A":
        raise ValueError(f"scene {scene} is class {m['class']}, not analytic")
    rec = os.path.join(SCENES_OUT, scene + ".arrec")
    for p in (CLI, rec):
        if not os.path.exists(p):
            raise FileNotFoundError(f"{p} missing: run tools/build.ps1 and tools/scenes.py first")
    repro = (f"<python> -I tools/analytic.py --scene {scene} --device {device} --spp {spp} --seed {seed}")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "dump.fits")
        r = subprocess.run([CLI, "dump", rec, "-o", out, "--device", device, "--spp", str(spp),
                            "--seed", str(seed), "--size", f"{SIZE}x{SIZE}"],
                           capture_output=True, text=True, cwd=tmp, timeout=600)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print(f"FAIL analytic {scene} {device}: astroray_cli exited {r.returncode}; reproduce: {repro}")
            return False
        mean, var = load_dump(out, spp)
    z = pixel_z(family_m(), SIZE * SIZE)
    ok, worst_all = True, None
    for name, w, truth in truths.evaluate(m["truth"], SIZE, SIZE):
        w = np.asarray(w, dtype=np.float64)
        cmean = np.tensordot(w, mean, axes=1)
        if np.count_nonzero(w) == 1:
            cvar = np.tensordot(w * w, var, axes=1)
        else:
            # The dump has no covariance between channels, so the SE of a combination is bounded
            # by the sum of the parts' SEs (Minkowski inequality for standard deviations).
            cvar = np.tensordot(np.abs(w), np.sqrt(var), axes=1) ** 2
        passed, reasons, worst = check_channel(cmean, cvar, truth, z, m["eps_sys"])
        ok = ok and passed
        if worst_all is None or abs(worst[0]) > abs(worst_all[0]):
            worst_all = (worst[0], worst[1], name)
        print(f"analytic {scene} {device} {name}: {'ok' if passed else 'FAIL'} mean {cmean.mean():.6f} "
              f"truth {truth.mean():.6f} worst z {worst[0]:+.2f} at {worst[1]}"
              + ("" if passed else "; " + "; ".join(reasons)))
    if png:
        tmax = max(float(np.max(t)) for _, _, t in truths.evaluate(m["truth"], SIZE, SIZE))
        write_png(png, mean, 0.5 / tmax if tmax > 0 else 1.0)
    secs = time.perf_counter() - t0
    print(f"{'PASS' if ok else 'FAIL'} analytic {scene} {device}: worst z {worst_all[0]:+.2f} "
          f"({worst_all[2]} at {worst_all[1]}) against z {z:.2f} + eps_sys {m['eps_sys']:g}, {spp} spp, "
          f"seed {seed}, {secs:.1f} s; reproduce: {repro}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--scene", required=True, action="append", help="scene name (repeatable)")
    ap.add_argument("--device", required=True, choices=("cpu", "cuda"))
    ap.add_argument("--spp", type=int, default=DEFAULT_SPP)
    ap.add_argument("--seed", type=int, help="default: seed_for(scene name), task 020's rule")
    ap.add_argument("--png", help="write the render here (one scene only), display scaled so truth is 0.5")
    a = ap.parse_args()
    if a.spp < 2:
        ap.error("--spp must be at least 2 (the variance of the mean needs two samples)")
    if a.png and len(a.scene) != 1:
        ap.error("--png takes one scene")
    ok = True
    for s in a.scene:
        ok = run(s, a.device, a.spp, seed_for(s) if a.seed is None else a.seed, a.png) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
