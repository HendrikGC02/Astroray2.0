# SPDX-License-Identifier: Apache-2.0
"""Exporter equivalence (0009 d17a): two film dumps of one scene, rendered by `astroray_cli dump` from
the addon's recording and from the builder's, must agree on the deterministic primary-hit AOVs.

Usage: <python> -I tools/exporter_equivalence.py <a.fits> <b.fits> --camera-distance D --radius R
--normal-quant Q (A1's geometry: a sphere of radius R at the origin, a pinhole camera at (0, -D, 0), z up; exit 0 and
"PASS exporter equivalence ..." on agreement, 1 and "FAIL ..." lines otherwise).
The scene state itself cannot be dumped (R59), so equivalence is measured through film output: position,
normal, UV and material id of the first hit at sample 0 depend only on geometry, transforms, camera and
the material table, not on shading. Both dumps must come from one seed, size, spp and sampler mode.

Every tolerance is computed from a stated error source, none is fitted to a measurement.
 - Ray direction. The builder and Blender build the camera ray from different float32 matrices
   (ar.projection_inverse vs Object.calc_matrix_camera, then the camera-to-world 3x4): about 8 rounded
   operations each, every one off by at most EPS32 = 2^-24 (unit roundoff of float32, IEEE 754),
   so the angle between the two rays is at most RAY_ANGLE = 16 EPS32 (about 1e-6 rad).
 - Position. Two rays RAY_ANGLE apart from the eye meet a surface at incidence cosine c at points
   |eye - P| RAY_ANGLE / c apart. A pixel with c < MIN_COS (grazing) is excluded by rule, as A1 excludes
   its silhouette pixels, because there one ray may hit and the other miss.
 - UV. The sphere's u = phi / 2 pi moves by at most (position error) / (2 pi R sin(polar angle)) and
   v by less. Pixels within MIN_SIN_POLAR of a pole, and within SEAM of the u = 0/1 seam (where the two
   dumps may pick triangles on either side), are excluded from the UV comparison.
 - Normal. Blender 5.3 stores custom corner normals quantised: the attribute `custom_normal` has
   data_type INT16_2D, domain CORNER (checked with Mesh.attributes in Blender 5.3.0), so the normals
   the addon reads differ from the builder's float32 ones. The encoding's step is not derived here;
   the bound is that mesh's own storage error, Q = the largest |stored - builder| over its corners,
   which the caller reads back from Blender (`Mesh.corner_normals`, the values the addon reads). An
   interpolated normal lies within Q of the builder's because it is a convex combination of corner
   normals (renormalising does not increase the distance of unit vectors); the hit point error moves
   it by (position error) / R more.
 - The material id is an integer and must match exactly.
Ref: docs/decisions/0009-build-test-process.md d17a, d19; docs/decisions/RULINGS.md R59.
"""
import math
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "abi"))
import fits_read  # noqa: E402

EPS32 = 2.0 ** -24
RAY_ANGLE = 16 * EPS32          # rad: two float32 matrix chains, about 8 roundings each
MIN_COS = 0.1                   # excluded: grazing incidence (about 84 degrees)
MIN_SIN_POLAR = 0.1             # excluded for UV: within about 6 degrees of a pole
SEAM = 1e-3                     # excluded for UV: u within this of 0 or 1
# Header keys that must agree for the comparison to mean anything.
SAME = ("SEED", "SPP", "SAMPLER", "DEVICE", "FILTER")


def load(path):
    with open(path, "rb") as f:
        hdus = fits_read.read_hdus(f.read())
    return hdus[0][0], {h["EXTNAME"]: d.astype(np.float64) for h, d in hdus[1:]}


def compare(path_a, path_b, camera_distance, radius, normal_quant):
    """Returns (ok, lines). The sphere is at the origin, the camera at (0, -camera_distance, 0)."""
    (ha, pa), (hb, pb) = load(path_a), load(path_b)
    for k in SAME:
        if ha.get(k) != hb.get(k):
            return False, [f"FAIL exporter equivalence: header {k} differs: {ha.get(k)!r} vs {hb.get(k)!r}"]
    need = [f"aov_position.{i}" for i in range(3)] + [f"aov_normal.{i}" for i in range(3)] + \
           ["aov_uv.0", "aov_uv.1", "aov_material"]
    for n in need:
        for path, planes in ((path_a, pa), (path_b, pb)):
            if n not in planes:
                return False, [f"FAIL exporter equivalence: {path} has no {n} plane (is the pass requested?)"]
    pos_a = np.stack([pa[f"aov_position.{i}"] for i in range(3)])
    pos_b = np.stack([pb[f"aov_position.{i}"] for i in range(3)])
    hit_a, hit_b = (pos_a != 0).any(axis=0), (pos_b != 0).any(axis=0)
    pos = np.where(hit_a, pos_a, pos_b)
    eye = np.array([0.0, -camera_distance, 0.0])[:, None, None]
    view = eye - pos
    dist = np.linalg.norm(view, axis=0)
    with np.errstate(divide="ignore", invalid="ignore"):
        cos = np.abs((pos / radius * view / dist).sum(axis=0))
        dpos = dist * RAY_ANGLE / cos                       # metres
        sin_polar = np.hypot(pos[0], pos[1]) / radius
        duv = dpos / (2.0 * math.pi * radius * sin_polar)
    anyhit = hit_a | hit_b
    grazing = anyhit & ~(cos >= MIN_COS)
    u_b = pb["aov_uv.0"]
    uv_skip = grazing | (anyhit & (~(sin_polar >= MIN_SIN_POLAR) | (u_b < SEAM) | (u_b > 1 - SEAM)))
    lines, ok = [], True
    lines.append(f"excluded: {int(grazing.sum())} grazing pixels (cos < {MIN_COS}), "
                 f"{int((uv_skip & ~grazing).sum())} more for UV (poles, seam)")

    def judge(name, d, tol, skip):
        nonlocal ok
        bad = int(((d > tol) & ~skip & anyhit).sum() + ((hit_a != hit_b) & ~skip).sum())
        ok = ok and bad == 0
        worst = float(np.max(np.where(skip | ~anyhit, 0.0, d / np.maximum(tol, 1e-300))))
        lines.append(f"{'ok' if bad == 0 else 'FAIL'} {name}: worst |diff| / bound {worst:.3g}, {bad} pixels outside")

    for i in range(3):
        judge(f"aov_position.{i}", np.abs(pos_a[i] - pos_b[i]), dpos, grazing)
    for i in range(3):
        judge(f"aov_normal.{i}", np.abs(pa[f"aov_normal.{i}"] - pb[f"aov_normal.{i}"]),
              normal_quant + dpos / radius, grazing)
    for i in range(2):
        judge(f"aov_uv.{i}", np.abs(pa[f"aov_uv.{i}"] - pb[f"aov_uv.{i}"]), duv, uv_skip)
    judge("aov_material", np.abs(pa["aov_material"] - pb["aov_material"]), 0.0, np.zeros_like(grazing))
    return ok, lines


def main(argv):
    try:
        a, b = argv[1], argv[2]
        d = float(argv[argv.index("--camera-distance") + 1])
        r = float(argv[argv.index("--radius") + 1])
        q = float(argv[argv.index("--normal-quant") + 1])
    except (IndexError, ValueError):
        print(__doc__.splitlines()[0])
        return 2
    ok, lines = compare(a, b, d, r, q)
    print("\n".join(lines))
    print(f"{'PASS' if ok else 'FAIL'} exporter equivalence: {a} vs {b}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
