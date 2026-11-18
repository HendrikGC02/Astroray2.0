# SPDX-License-Identifier: Apache-2.0
"""Exporter equivalence (0009 d17a): two film dumps of one scene, rendered by `astroray_cli dump` from
the addon's recording and from the builder's, must agree on the deterministic primary-hit AOVs.

Usage: <python> -I tools/exporter_equivalence.py <a.fits> <b.fits>   (exit 0 and "PASS exporter
equivalence ..." on agreement, 1 and "FAIL ..." lines otherwise).
The scene state itself cannot be dumped (R59), so equivalence is measured through film output: position,
normal, UV and material id of the first hit at sample 0 depend only on geometry, transforms, camera and
the material table, not on shading. Both dumps must come from one seed, size, spp and sampler mode.
Tolerances are absolute, per plane, from the first measurement on A1 at 64x64 (differences seen: position
9.4e-6, UV 1.6e-5, normal 1.2e-4): Position (metres) and UV follow the ray through two camera matrices
(the builder's and Blender's calc_matrix_camera), which differ in the last float32 bits and are amplified
at grazing hits (1e-4 and 5e-5); Normal is looser (2e-3) because Blender stores custom corner normals
quantised and renormalises them; the material id is an integer and must match exactly.
Ref: docs/decisions/0009-build-test-process.md d17a, d19; docs/decisions/RULINGS.md R59.
"""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "abi"))
import fits_read  # noqa: E402

TOLERANCE = {"aov_position": 1e-4, "aov_normal": 2e-3, "aov_uv": 5e-5, "aov_material": 0.0}
# Header keys that must agree for the comparison to mean anything.
SAME = ("SEED", "SPP", "SAMPLER", "DEVICE", "FILTER")


def load(path):
    with open(path, "rb") as f:
        hdus = fits_read.read_hdus(f.read())
    return hdus[0][0], {h["EXTNAME"]: d.astype(np.float64) for h, d in hdus[1:]}


def compare(path_a, path_b):
    """Returns (ok, lines)."""
    (ha, pa), (hb, pb) = load(path_a), load(path_b)
    lines, ok = [], True
    for k in SAME:
        if ha.get(k) != hb.get(k):
            return False, [f"FAIL exporter equivalence: header {k} differs: {ha.get(k)!r} vs {hb.get(k)!r}"]
    for aov, tol in TOLERANCE.items():
        names = sorted(n for n in pa if n == aov or n.startswith(aov + "."))
        if not names:
            return False, [f"FAIL exporter equivalence: {path_a} has no {aov} plane (is the pass requested?)"]
        for n in names:
            if n not in pb:
                return False, [f"FAIL exporter equivalence: {n} is missing from {path_b}"]
            d = np.abs(pa[n] - pb[n])
            bad = int((d > tol).sum())
            ok = ok and bad == 0
            lines.append(f"{'ok' if bad == 0 else 'FAIL'} {n}: max |diff| {d.max():.3g} (tolerance {tol:g}), "
                         f"{bad} of {d.size} pixels outside")
    return ok, lines


def main(argv):
    if len(argv) != 3:
        print(__doc__.splitlines()[0])
        return 2
    ok, lines = compare(argv[1], argv[2])
    print("\n".join(lines))
    print(f"{'PASS' if ok else 'FAIL'} exporter equivalence: {argv[1]} vs {argv[2]}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
