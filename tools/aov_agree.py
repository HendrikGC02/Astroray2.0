# SPDX-License-Identifier: Apache-2.0
"""Deterministic AOV agreement, CPU against GPU (0009 d17(iv)): renders out/scenes/<scene>.arrec
through `astroray_cli dump` on both devices at the scene's [truth] size, spp and seed and compares
the primary-hit AOVs per pixel.

Usage: <python> -I tools/aov_agree.py --scene d2_aov_geometry [--png P]
Prints the mismatch count per AOV, the excluded pixel count and a final PASS or FAIL line; exits
non-zero on failure.

Rules. 0009 d17(iv) bounds two AOVs: material ids (IndexMA, the hit material's pass index) equal,
and positions within POS_REL relative (|P_cpu - P_gpu| <= POS_REL |P_cpu|, Euclidean norms). Normals
and UV have no record bound (lead ruling, task 077): each is checked for consistency with the
position difference measured at that pixel, through bounds computed from the scene's own
description (<recording>.layout.json, written by the builder), never fitted to a run:
 - Both devices interpolate the same linear functions of the hit point over the hit triangle (0005
   item 33: P, the unnormalised world normal cof(M) n and UV are barycentric blends), so a point
   moved by dP within the triangle moves UV component k by at most |grad uv_k| |dP| and the
   unnormalised normal by at most ||J||_F |dP|, J its Jacobian along the surface. Normalising divides
   by the interpolated length, at least min_i (c_i . m) over the corner normals c_i (m their mean
   direction), as in tools/exporter_equivalence.py min_interpolated_length. Rates are maxima over
   each instance's triangles; flat boxes and planes have J = 0.
 - Float32 rounding (Higham, Accuracy and Stability of Numerical Algorithms, 2nd ed., section 3.1,
   eq. 3.2, gamma_n = n u / (1 - n u)): each device's P (a 3-term blend, then a 3x4 transform: 6
   rounded operations per component) is off by at most sqrt(3) gamma_6 (|P| + 2 r), r the instance's
   largest corner distance from its origin, so the measured |dP| is widened by twice that; a UV blend
   of values in [0, 1] is off by gamma_3 per device; a normal (blend, cofactor transform, normalise:
   10 operations) by gamma_10 / cos_min radians, cos_min the smallest cosine of a corner normal to
   its triangle's mean.
 - normal angle <= rate_N (|dP| + eps_P) + 2 gamma_10 / cos_min; |du_k| <= rate_k (|dP| + eps_P) + 2 gamma_3.
A pixel is a hit when its material id is not 0 (the builder gives every material a pass index from
1). A pixel that is a hit on one device and not the other is a material mismatch. Pixels whose
material id is one of the coincident pair's ([truth] pair_pass_indices) on either device are a
traversal tie: excluded from every comparison and counted. A run in which the pair covers no pixel
fails, since the exclusion would then be untested; so does one with no hits, or with fewer than
MIN_IDS distinct ids (0009 d16: not constant, not black). Prints the worst ratio to bound per AOV.
Ref: docs/decisions/0009-build-test-process.md d16, d17(iv); 0008 d10 (the AOVs; Position and
IndexMA keep sample 0, Normal and UV are means over samples); 0005 item 33;
src/kernel/stages/shade_common.h shade_context_geometry (the interpolation the bounds follow).
"""
import argparse
import json
import math
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
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fits_read  # noqa: E402
import meta  # noqa: E402
from exporter_equivalence import gamma  # noqa: E402  Higham's gamma_n for float32

DEVICES = ("cpu", "cuda")
POS_REL = 1e-5  # 0009 d17(iv)
MIN_IDS = 10


def instance_rates(layout):
    """pass index -> (rate_N, (rate_u, rate_v), cos_min, r) of each instance (module docstring)."""
    out = {}
    for inst in layout["instances"]:
        mesh = layout["meshes"][inst["mesh"]]
        x = np.asarray(inst["xf"], np.float64).reshape(3, 4)
        m, t = x[:, :3], x[:, 3]
        p = np.asarray(mesh["positions"], np.float64).reshape(-1, 3, 3) @ m.T + t
        cof = np.stack([np.cross(m[1], m[2]), np.cross(m[2], m[0]), np.cross(m[0], m[1])])
        sign = -1.0 if np.dot(m[0], cof[0]) < 0 else 1.0
        c = np.asarray(mesh["normals"], np.float64).reshape(-1, 3, 3) @ cof.T * sign
        uv = np.asarray(mesh["uv"], np.float64).reshape(-1, 3, 2)
        # In-plane gradient of a linear function with corner values f: g = E (E^T E)^-1 (f1 - f0, f2 - f0),
        # E = (e1 e2) the world edges from corner 0.
        e = np.stack([p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]], axis=2)            # (T, 3, 2)
        pinv = e @ np.linalg.inv(np.transpose(e, (0, 2, 1)) @ e)                 # (T, 3, 2)

        def grad(f):  # f (T, 3, k) -> (T, 3, k)
            return pinv @ np.stack([f[:, 1] - f[:, 0], f[:, 2] - f[:, 0]], axis=1)
        guv = np.linalg.norm(grad(uv), axis=1)                                   # (T, 2)
        jn = np.sqrt((grad(c) ** 2).sum(axis=(1, 2)))                            # (T,)
        cn = c / np.linalg.norm(c, axis=2, keepdims=True)
        mean = cn.sum(axis=1)
        mean /= np.linalg.norm(mean, axis=1, keepdims=True)
        length = np.einsum("tki,ti->tk", c, mean).min(axis=1)
        cos_min = float(np.einsum("tki,ti->tk", cn, mean).min())
        if not (length > 0).all():
            raise ValueError(f"instance {inst['pass_index']}: a triangle's corner normals span a half-space")
        r = float(np.linalg.norm(np.asarray(mesh["positions"], np.float64) @ m.T, axis=1).max())
        out[int(inst["pass_index"])] = (float((jn / length).max()), tuple(guv.max(axis=0)), cos_min, r)
    return out


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


def compare(a, b, pair, rates):
    """a, b: render() of the two devices; rates: instance_rates(). Returns (counts, worst, excluded,
    hits, ids): mismatch counts and the worst ratio of difference to bound per AOV (for material,
    the largest id difference) over the compared pixels."""
    hit_a, hit_b = a["ID"] != 0, b["ID"] != 0
    tie = np.isin(a["ID"], pair) | np.isin(b["ID"], pair)
    both = hit_a & hit_b & ~tie & (a["ID"] == b["ID"])
    counts, worst = {}, {}
    counts["material"] = int(((a["ID"] != b["ID"]) & ~tie).sum())
    worst["material"] = float(np.abs(a["ID"] - b["ID"])[~tie].max(initial=0.0))
    unknown = set(np.unique(a["ID"][both]).astype(int)) - set(rates)
    if unknown:
        raise ValueError(f"material ids {sorted(unknown)} are not in the scene's layout")
    # Per-id lookup tables (id 0, the background, and ids outside the layout get rate 0, cos 1).
    table = np.zeros((max(rates) + 1, 5))
    table[:, 3] = 1.0
    for i, (rn, (ru, rv), cmin, r) in rates.items():
        table[i] = (rn, ru, rv, cmin, r)
    ids = np.where(both, a["ID"], 0).astype(int)
    rate_n, rate_uv, cos_min, radius = table[ids, 0], table[ids, 1:3].transpose(2, 0, 1), table[ids, 3], table[ids, 4]
    pnorm = np.linalg.norm(a["P"], axis=0)
    dp = np.linalg.norm(a["P"] - b["P"], axis=0)
    eps_p = 2.0 * math.sqrt(3.0) * gamma(6) * (pnorm + 2.0 * radius)

    def judge(name, d, bound):
        ratio = np.where(both, d / np.maximum(bound, 1e-300), 0.0)
        counts[name], worst[name] = int((ratio > 1.0).sum()), float(ratio.max())

    judge("position", dp, POS_REL * pnorm)
    na, nb = a["N"], b["N"]
    ang = np.arctan2(np.linalg.norm(np.cross(na, nb, axis=0), axis=0), (na * nb).sum(axis=0))
    judge("normal", ang, rate_n * (dp + eps_p) + 2.0 * gamma(10) / cos_min)

    duv = np.abs(a["UV"] - b["UV"])
    ratio = np.where(both, (duv / (rate_uv * (dp + eps_p) + 2.0 * gamma(3))).max(axis=0), 0.0)
    counts["uv"], worst["uv"] = int((ratio > 1.0).sum()), float(ratio.max())
    nids = np.unique(a["ID"][hit_a & ~tie]).size
    return counts, worst, int(tie.sum()), int((hit_a | hit_b).sum()), nids

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
    layout_path = rec + ".layout.json"
    if not os.path.exists(layout_path):
        raise FileNotFoundError(f"{layout_path} missing: the builder writes it beside the recording (tools/scenes.py --force)")
    with open(layout_path, encoding="utf-8") as f:
        rates = instance_rates(json.load(f))
    a, b = (render(rec, d, size, spp, seed) for d in DEVICES)
    counts, worst, excluded, hits, ids = compare(a, b, pair, rates)
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
    print(f"aov {scene}: mismatches material {counts['material']}, position {counts['position']}, "
          f"normal {counts['normal']}, uv {counts['uv']}; worst difference / bound: position "
          f"{worst['position']:.3g} (bound {POS_REL:g} relative), normal {worst['normal']:.3g}, uv {worst['uv']:.3g}")
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
