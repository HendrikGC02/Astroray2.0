# SPDX-License-Identifier: Apache-2.0
"""Deterministic AOV agreement, CPU against GPU (0009 d17(iv)): renders out/scenes/<scene>.arrec
through `astroray_cli dump` on both devices at the scene's [truth] size, spp and seed and compares
the primary-hit AOVs per pixel.

Usage: <python> -I tools/aov_agree.py --scene d2_aov_geometry [--png P]
Prints the mismatch count per AOV, the excluded pixel count and a final PASS or FAIL line; exits
non-zero on failure.

Rules (0009 d17(iv)): material ids (IndexMA, the hit material's pass index) equal; positions within
POS_REL relative (|P_cpu - P_gpu| <= POS_REL |P_cpu|, Euclidean norms); normals within NORMAL_RAD
radians of each other; each UV component within UV_ABS. A pixel is a hit when its material id is
not 0 (the builder gives every material a pass index from 1). A pixel that is a hit on one device
and not the other is a material mismatch. Pixels whose material id is one of the coincident pair's
([truth] pair_pass_indices) on either device are a traversal tie: excluded from every comparison and
counted. A run in which the pair covers no pixel fails, since the exclusion would then be untested;
so does one with no hits, or with fewer than MIN_IDS distinct ids (0009 d16: not constant, not black).
Ref: docs/decisions/0009-build-test-process.md d16, d17(iv); 0008 d10 (the AOVs; Position and
IndexMA keep sample 0, Normal and UV are means over samples).
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
SCENES_OUT = os.path.join(ROOT, "out", "scenes")
sys.path.insert(0, os.path.join(ROOT, "tests", "abi"))
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
import fits_read  # noqa: E402
import meta  # noqa: E402

DEVICES = ("cpu", "cuda")
# 0009 d17(iv) for positions; the task's bounds for normals and UV (tasks/077, step 2).
POS_REL, NORMAL_RAD, UV_ABS = 1e-5, 1e-5, 1e-5
MIN_IDS = 10


def render(rec, device, size, spp, seed):
    """The AOV planes of one dump: dict name -> (h, w) float64, rows bottom-up."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "dump.fits")
        r = subprocess.run([CLI, "dump", rec, "-o", out, "--device", device, "--spp", str(spp), "--seed", str(seed),
                            "--size", f"{size}x{size}"], capture_output=True, text=True, cwd=tmp, timeout=300)
        if r.returncode != 0:
            raise RuntimeError(f"astroray_cli dump on {device} exited {r.returncode}\n{r.stdout}{r.stderr}")
        with open(out, "rb") as f:
            hdus = fits_read.read_hdus(f.read())
    head, planes = hdus[0][0], {h["EXTNAME"]: d.astype(np.float64) for h, d in hdus[1:]}
    if head.get("DEVICE") != device or head.get("SPP") != spp or head.get("SEED") != seed:
        raise RuntimeError(f"the {device} dump says DEVICE {head.get('DEVICE')!r}, SPP {head.get('SPP')}, "
                           f"SEED {head.get('SEED')}: not the render asked for")
    need = [f"aov_position.{i}" for i in range(3)] + [f"aov_normal.{i}" for i in range(3)] + \
           ["aov_uv.0", "aov_uv.1", "aov_material"]
    missing = [n for n in need if n not in planes]
    if missing:
        raise RuntimeError(f"the {device} dump has no {missing} planes: does the builder request the Position pass?")
    return {"P": np.stack([planes[f"aov_position.{i}"] for i in range(3)]),
            "N": np.stack([planes[f"aov_normal.{i}"] for i in range(3)]),
            "UV": np.stack([planes["aov_uv.0"], planes["aov_uv.1"]]), "ID": planes["aov_material"]}


def compare(a, b, pair):
    """a, b: render() of the two devices. Returns (counts, worst, excluded, hits, ids): mismatch
    counts and worst values per AOV over the compared pixels."""
    hit_a, hit_b = a["ID"] != 0, b["ID"] != 0
    tie = np.isin(a["ID"], pair) | np.isin(b["ID"], pair)
    both = hit_a & hit_b & ~tie
    counts, worst = {}, {}
    counts["material"] = int(((a["ID"] != b["ID"]) & ~tie).sum())
    worst["material"] = float(np.abs(a["ID"] - b["ID"])[~tie].max(initial=0.0))
    dp = np.linalg.norm(a["P"] - b["P"], axis=0)
    rel = np.where(both, dp / np.maximum(np.linalg.norm(a["P"], axis=0), 1e-30), 0.0)
    counts["position"], worst["position"] = int((rel > POS_REL).sum()), float(rel.max())
    na, nb = a["N"], b["N"]
    ang = np.arctan2(np.linalg.norm(np.cross(na, nb, axis=0), axis=0), (na * nb).sum(axis=0))
    ang = np.where(both, ang, 0.0)
    counts["normal"], worst["normal"] = int((ang > NORMAL_RAD).sum()), float(ang.max())
    duv = np.where(both, np.abs(a["UV"] - b["UV"]).max(axis=0), 0.0)
    counts["uv"], worst["uv"] = int((duv > UV_ABS).sum()), float(duv.max())
    ids = np.unique(a["ID"][hit_a & ~tie]).size
    return counts, worst, int(tie.sum()), int((hit_a | hit_b).sum()), ids


def write_png(path, ids):
    """The CPU material ids as a false-colour PNG (rows flipped to top-down), for looking at."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import analytic  # noqa: E402  write_png: sRGB-encoded 8-bit, rows bottom-up in
    rng = np.random.default_rng(0)
    lut = rng.uniform(0.05, 1.0, (int(ids.max()) + 1, 3))
    lut[0] = 0.0
    analytic.write_png(path, lut[ids.astype(int)].transpose(2, 0, 1), 1.0)


def run(scene, png=None):
    t0 = time.perf_counter()
    m = meta.load(os.path.join(ROOT, "scenes", scene, "meta.toml"))
    t = m.get("truth", {})
    for k in ("size", "spp", "seed", "pair_pass_indices"):
        if k not in t:
            raise ValueError(f"scenes/{scene}/meta.toml [truth] has no {k}")
    rec = os.path.join(SCENES_OUT, scene + ".arrec")
    for p in (CLI, rec):
        if not os.path.exists(p):
            raise FileNotFoundError(f"{p} missing: run tools/build.ps1 and tools/scenes.py first")
    size, spp, seed, pair = int(t["size"]), int(t["spp"]), int(t["seed"]), [int(v) for v in t["pair_pass_indices"]]
    a, b = (render(rec, d, size, spp, seed) for d in DEVICES)
    counts, worst, excluded, hits, ids = compare(a, b, pair)
    reasons = []
    if hits == 0:
        reasons.append("no pixel hits geometry")
    if ids < MIN_IDS:
        reasons.append(f"only {ids} distinct material ids (at least {MIN_IDS})")
    if excluded == 0:
        reasons.append("the coincident pair covers no pixel, so the exclusion is untested")
    for k, v in counts.items():
        if v:
            reasons.append(f"{v} {k} mismatches")
    print(f"aov {scene}: mismatches material {counts['material']}, position {counts['position']} "
          f"(worst {worst['position']:.2g} relative, bound {POS_REL:g}), normal {counts['normal']} "
          f"(worst {worst['normal']:.2g} rad, bound {NORMAL_RAD:g}), uv {counts['uv']} "
          f"(worst {worst['uv']:.2g}, bound {UV_ABS:g})")
    print(f"aov {scene}: excluded {excluded} pixels on the coincident pair {pair}; {hits} hit pixels of "
          f"{size * size}, {ids} distinct material ids")
    if png:
        write_png(png, a["ID"])
    ok = not reasons
    print(f"{'PASS' if ok else 'FAIL'} aov {scene} cpu~cuda: {size}x{size} at {spp} spp, seed {seed}, "
          f"{time.perf_counter() - t0:.1f} s" + ("" if ok else "; " + "; ".join(reasons)))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--scene", required=True, action="append", help="scene name (repeatable)")
    ap.add_argument("--png", help="write the CPU material ids as a PNG (one scene only)")
    a = ap.parse_args()
    if a.png and len(a.scene) != 1:
        ap.error("--png takes one scene")
    ok = True
    for s in a.scene:
        ok = run(s, a.png) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
