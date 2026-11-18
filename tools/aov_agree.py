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
 - Float32 rounding (Higham, Accuracy and Stability of Numerical Algorithms, 2nd ed.: gamma_n =
   n u / (1 - n u), section 3.1 eq. 3.2; an n-term inner product is off by gamma_n sum |x_i y_i|,
   eq. 3.4; gamma_a + gamma_b + gamma_a gamma_b <= gamma_(a+b), lemma 3.3), counted per device from
   src/kernel/stages/shade_common.h:
   - P (lines 190, 193-196 and xform_point, lines 148-149): w0 = 1 - u - v has 2 roundings, so
     |dw0| <= 2 gamma_2; the 3-term blend adds gamma_3, so each object-space component q_j is off by
     gamma_7 M_j, M_j = max over the mesh's corners of |p_j|; the transform is a 4-term sum
     (3 products and t_i), gamma_4, so |dP_i| <= gamma_11 (sum_j |m_ij| M_j + |t_i|). The measured
     |dP| is widened by eps_P = 2 gamma_11 |(sum_j |m_ij| M_j + |t_i|)_i| (both devices).
   - Normal (lines 197, 206-210): the blend of corner normals is off by gamma_7 N_j (N_j = max over
     the triangle's corners of |n_j|), each cofactor entry (a cross product, ab - cd) by gamma_2
     (|a b| + |c d|), the 3-term dots by gamma_3: |dn_raw_i| <= gamma_12 sum_j C_ij N_j, C the
     cofactor of |M| entry-wise, which carries the spread of an anisotropic scale. Its angle is that
     over the interpolated length (at least the triangle's min_k c_k . m), plus gamma_5 for the
     normalisation (a 3-term dot, sqrt, divide): eps_N = 2 max_t (gamma_12 |C N_t| / length_t + gamma_5).
   - UV (lines 214-221): the same 3-term blend as P's, eps_uv_k = 2 gamma_7 max |uv_k|.
 - normal angle <= rate_N (|dP| + eps_P) + eps_N; |du_k| <= rate_k (|dP| + eps_P) + eps_uv_k.A pixel is a hit when its material id is not 0 (the builder gives every material a pass index from
1). A pixel that is a hit on one device and not the other is a material mismatch. Pixels whose
material id is one of the coincident pair's ([truth] pair_pass_indices) on both devices are a
traversal tie: excluded from every comparison and counted; a pair id on one device only is a
material mismatch. A run in which the pair covers no pixel
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
    """pass index -> (rate_N, rate_u, rate_v, eps_N, eps_P, eps_u, eps_v) of each instance: the
    largest rates and float32 terms over its triangles (module docstring)."""
    out = {}
    for inst in layout["instances"]:
        mesh = layout["meshes"][inst["mesh"]]
        x = np.asarray(inst["xf"], np.float64).reshape(3, 4)
        m, t = x[:, :3], x[:, 3]
        pobj = np.asarray(mesh["positions"], np.float64).reshape(-1, 3, 3)
        p = pobj @ m.T + t
        cof = np.stack([np.cross(m[1], m[2]), np.cross(m[2], m[0]), np.cross(m[0], m[1])])
        sign = -1.0 if np.dot(m[0], cof[0]) < 0 else 1.0
        nobj = np.asarray(mesh["normals"], np.float64).reshape(-1, 3, 3)
        c = nobj @ cof.T * sign
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
        if not (length > 0).all():
            raise ValueError(f"instance {inst['pass_index']}: a triangle's corner normals span a half-space")
        a = np.abs(m)
        cabs = np.stack([np.abs(np.outer(m[1], m[2])), np.abs(np.outer(m[2], m[0])), np.abs(np.outer(m[0], m[1]))])
        # |cof_ij| bound per rounding: |a b| + |c d| of the cross product's two terms (index pairs below).
        pairs = ((1, 2), (2, 0), (0, 1))
        cmag = np.array([[cabs[i][pairs[j][0], pairs[j][1]] + cabs[i][pairs[j][1], pairs[j][0]] for j in range(3)]
                         for i in range(3)])
        n_max = np.abs(nobj).max(axis=1)                                         # (T, 3)
        eps_n = 2.0 * float((gamma(12) * np.linalg.norm(n_max @ cmag.T, axis=1) / length + gamma(5)).max())
        eps_p = 2.0 * gamma(11) * float(np.linalg.norm(a @ np.abs(pobj).reshape(-1, 3).max(axis=0) + np.abs(t)))
        eps_uv = 2.0 * gamma(7) * np.abs(uv).reshape(-1, 2).max(axis=0)
        out[int(inst["pass_index"])] = (float((jn / length).max()), *guv.max(axis=0), eps_n, eps_p, *eps_uv)
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
    tie = np.isin(a["ID"], pair) & np.isin(b["ID"], pair)
    both = hit_a & hit_b & ~tie & (a["ID"] == b["ID"])
    counts, worst = {}, {}
    counts["material"] = int(((a["ID"] != b["ID"]) & ~tie).sum())
    worst["material"] = float(np.abs(a["ID"] - b["ID"])[~tie].max(initial=0.0))
    unknown = set(np.unique(a["ID"][both]).astype(int)) - set(rates)
    if unknown:
        raise ValueError(f"material ids {sorted(unknown)} are not in the scene's layout")
    # Per-id lookup table; id 0 (the background) and pixels not compared read row 0, never used.
    table = np.zeros((max(rates) + 1, 7))
    for i, row in rates.items():
        table[i] = row
    look = table[np.where(both, a["ID"], 0).astype(int)]                        # (h, w, 7)
    rate_n, rate_uv, eps_n, eps_p, eps_uv = (look[..., 0], look[..., 1:3].transpose(2, 0, 1), look[..., 3],
                                             look[..., 4], look[..., 5:7].transpose(2, 0, 1))
    pnorm = np.linalg.norm(a["P"], axis=0)
    dp = np.linalg.norm(a["P"] - b["P"], axis=0)

    def judge(name, d, bound):
        ratio = np.where(both, d / np.maximum(bound, 1e-300), 0.0)
        counts[name], worst[name] = int((ratio > 1.0).sum()), float(ratio.max())

    judge("position", dp, POS_REL * pnorm)
    na, nb = a["N"], b["N"]
    ang = np.arctan2(np.linalg.norm(np.cross(na, nb, axis=0), axis=0), (na * nb).sum(axis=0))
    judge("normal", ang, rate_n * (dp + eps_p) + eps_n)
    duv = np.abs(a["UV"] - b["UV"])
    ratio = np.where(both, (duv / np.maximum(rate_uv * (dp + eps_p) + eps_uv, 1e-300)).max(axis=0), 0.0)
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
