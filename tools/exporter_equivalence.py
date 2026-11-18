# SPDX-License-Identifier: Apache-2.0
"""Exporter equivalence (0009 d17a): two film dumps of one scene, rendered by `astroray_cli dump` from
the addon's recording and from the builder's, must agree on the deterministic primary-hit AOVs.

Used by tests/blender/smoke_a1.py, which supplies A1's geometry (a sphere of radius R at the origin,
z up, built with `segments` x `rings`, a pinhole camera at (0, -D, 0)) and the two cameras as sent to
the engine. `main` is the same comparison for two dumps and a JSON file of those inputs.
The scene state itself cannot be dumped (R59), so equivalence is measured through film output: position,
normal, UV and material id of the first hit at sample 0 depend only on geometry, transforms, camera and
the material table, not on shading. Both dumps must come from one seed, size, spp and sampler mode.

Every tolerance is computed from the data or a stated error source; none is fitted to a measurement.
 - Rays. `ray_angles` builds, in float64, the primary ray of both cameras (the float32 matrices as
   sent: 4x4 inverse projection, divide, 3x3 of the camera-to-world 3x4) at the centre and corners of
   every pixel and takes the largest angle between the two directions, plus the distance between the two
   eyes. Each engine then rounds in float32: matrix-vector products (4, 3), the normalisation (3) and
   o + t d (2) are n = 10 / 2 sums of products, each carrying the factor gamma_n = n u / (1 - n u),
   u = 2^-24 (Higham, Accuracy and Stability of Numerical Algorithms, 2nd ed., section 3.1, eq. 3.2).
 - Position. Rays angle theta apart, from eyes `shift` apart, meet a surface at incidence cosine c at
   points at most (|eye - P| (theta + 2 gamma_10) + shift) / c apart, plus 2 gamma_2 (|eye| + |P|) for
   o + t d. Pixels with c < MIN_COS (grazing) are excluded by rule, as A1 excludes silhouette pixels,
   because there one ray may hit and the other miss.
 - UV. u = phi / 2 pi moves by dpos / (2 pi R sin(polar angle)) and v by dpos / (pi R) per metre over
   the true sphere. The mesh interpolates UV linearly over chords, and a chord of angle theta is shorter
   than its arc by sin(theta/2) / (theta/2), so UV changes faster by f(theta) = (theta/2) / sin(theta/2),
   theta = 2 pi / segments for u and pi / rings for v. Pixels within MIN_SIN_POLAR of a pole, and within
   SEAM of the u = 0/1 seam (where the dumps may pick triangles on either side), are excluded for UV.
 - Normal. Blender 5.3 stores custom corner normals quantised: the attribute `custom_normal` has
   data_type INT16_2D, domain CORNER (checked with Mesh.attributes in Blender 5.3.0). The bound Q is the
   mesh's own storage error, the largest |stored - builder| over its corners, read back by the caller
   from `Mesh.corner_normals` (what the addon reads). An interpolated normal is a convex combination,
   within Q of the builder's, but shorter than unit; normalising divides the error by its length, at
   least min_length (`min_interpolated_length`: all corners of a triangle lie within a cone around their
   mean direction, so every combination has length >= the smallest cosine to that mean). The hit point
   error moves the normal by dpos / R more.
 - The material id is an integer and must match exactly.
Ref: docs/decisions/0009-build-test-process.md d17a, d19; docs/decisions/RULINGS.md R59.
"""
import json
import math
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "abi"))
import fits_read  # noqa: E402

U32 = 2.0 ** -24                # unit roundoff of float32
MIN_COS = 0.1                   # excluded: grazing incidence (about 84 degrees)
MIN_SIN_POLAR = 0.1             # excluded for UV: within about 6 degrees of a pole
SEAM = 1e-3                     # excluded for UV: u within this of 0 or 1
# Header keys that must agree for the comparison to mean anything.
SAME = ("SEED", "SPP", "SAMPLER", "DEVICE", "FILTER")


def gamma(n):
    """Higham's gamma_n = n u / (1 - n u) for float32 (section 3.1, eq. 3.2)."""
    return n * U32 / (1.0 - n * U32)


def load(path):
    with open(path, "rb") as f:
        hdus = fits_read.read_hdus(f.read())
    return hdus[0][0], {h["EXTNAME"]: d.astype(np.float64) for h, d in hdus[1:]}


def _matrices(cam):
    proj = np.asarray(cam["projection_inverse"], np.float32).astype(np.float64).reshape(4, 4)
    xf = np.asarray(cam["cam_to_world"], np.float32).astype(np.float64).reshape(3, 4)
    return proj, xf


def _primary_rays(cam, nx, ny):
    """World directions (3, ...) of the camera's rays through NDC points: the float32 matrices as
    sent, evaluated in float64."""
    proj, xf = _matrices(cam)
    v = np.stack([nx.ravel(), ny.ravel(), np.zeros(nx.size), np.ones(nx.size)])
    p = proj @ v
    d = p[:3] / p[3]
    d /= np.linalg.norm(d, axis=0)
    return (xf[:, :3] @ d).reshape((3,) + nx.shape)


def ray_angles(cam_a, cam_b, width, height):
    """(theta, shift): theta (height, width) the largest angle between the two cameras' primary rays
    over each pixel's centre and corners (rows bottom-up), shift the distance between the eyes. A
    camera is {cam_to_world: 12 numbers, projection_inverse: 16 numbers}, row-major, as sent."""
    theta = np.zeros((height, width))
    col, row = np.meshgrid(np.arange(width), np.arange(height))
    for dx, dy in ((0.5, 0.5), (0, 0), (1, 0), (0, 1), (1, 1)):
        nx, ny = -1 + 2 * (col + dx) / width, -1 + 2 * (row + dy) / height
        a, b = _primary_rays(cam_a, nx, ny), _primary_rays(cam_b, nx, ny)
        ang = np.arctan2(np.linalg.norm(np.cross(a, b, axis=0), axis=0), (a * b).sum(axis=0))
        theta = np.maximum(theta, ang)
    shift = np.linalg.norm(_matrices(cam_a)[1][:, 3] - _matrices(cam_b)[1][:, 3])
    return theta, float(shift)


def min_interpolated_length(normals):
    """Lower bound of the length of any convex combination of the corner normals of one triangle,
    over all triangles: the smallest cosine between a corner normal and the mean direction of its
    triangle's three (consecutive triples of `normals`, shape (C, 3))."""
    n = np.asarray(normals, np.float64).reshape(-1, 3, 3)
    n = n / np.linalg.norm(n, axis=2, keepdims=True)
    m = n.sum(axis=1)
    m /= np.linalg.norm(m, axis=1, keepdims=True)
    return float(np.einsum("tki,ti->tk", n, m).min())


def _chord(theta):
    """UV changes faster over a chord of angle theta than over its arc by this factor."""
    return (theta / 2) / math.sin(theta / 2)


def compare(path_a, path_b, geometry, cam_a, cam_b, normal_quant, min_length):
    """Returns (ok, lines). geometry = dict(camera_distance, radius, segments, rings); cam_a and
    cam_b are the two cameras (see ray_angles); normal_quant and min_length as in the docstring."""
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
    radius, dist0 = geometry["radius"], geometry["camera_distance"]
    pos_a = np.stack([pa[f"aov_position.{i}"] for i in range(3)])
    pos_b = np.stack([pb[f"aov_position.{i}"] for i in range(3)])
    hit_a, hit_b = (pos_a != 0).any(axis=0), (pos_b != 0).any(axis=0)
    height, width = hit_a.shape
    theta, shift = ray_angles(cam_a, cam_b, width, height)
    pos = np.where(hit_a, pos_a, pos_b)
    eye = np.array([0.0, -dist0, 0.0])[:, None, None]
    view = eye - pos
    dist = np.linalg.norm(view, axis=0)
    with np.errstate(divide="ignore", invalid="ignore"):
        cos = np.abs((pos / radius * view / dist).sum(axis=0))
        dpos = (dist * (theta + 2 * gamma(10)) + shift) / cos \
            + 2 * gamma(2) * (dist0 + np.linalg.norm(pos, axis=0))
        sin_polar = np.hypot(pos[0], pos[1]) / radius
        du = _chord(2 * math.pi / geometry["segments"]) * dpos / (2.0 * math.pi * radius * sin_polar)
        dv = _chord(math.pi / geometry["rings"]) * dpos / (math.pi * radius)
    anyhit = hit_a | hit_b
    grazing = anyhit & ~(cos >= MIN_COS)
    u_b = pb["aov_uv.0"]
    uv_skip = grazing | (anyhit & (~(sin_polar >= MIN_SIN_POLAR) | (u_b < SEAM) | (u_b > 1 - SEAM)))
    lines, ok = [], True
    lines.append(f"excluded: {int(grazing.sum())} grazing pixels (cos < {MIN_COS}), "
                 f"{int((uv_skip & ~grazing).sum())} more for UV (poles, seam); ray angle up to "
                 f"{theta.max():.2g} rad, eye shift {shift:.2g} m, normal length >= {min_length:.4f}")

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
              normal_quant / min_length + dpos / radius, grazing)
    judge("aov_uv.0", np.abs(pa["aov_uv.0"] - pb["aov_uv.0"]), du, uv_skip)
    judge("aov_uv.1", np.abs(pa["aov_uv.1"] - pb["aov_uv.1"]), dv, uv_skip)
    judge("aov_material", np.abs(pa["aov_material"] - pb["aov_material"]), 0.0, np.zeros_like(grazing))
    return ok, lines


def main(argv):
    """<a.fits> <b.fits> <inputs.json>: inputs = {geometry, cam_a, cam_b, normal_quant, min_length}."""
    if len(argv) != 4:
        print(__doc__.splitlines()[0])
        return 2
    with open(argv[3], encoding="utf-8") as f:
        i = json.load(f)
    ok, lines = compare(argv[1], argv[2], i["geometry"], i["cam_a"], i["cam_b"], i["normal_quant"], i["min_length"])
    print("\n".join(lines))
    print(f"{'PASS' if ok else 'FAIL'} exporter equivalence: {argv[1]} vs {argv[2]}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
