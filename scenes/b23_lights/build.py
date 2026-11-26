# SPDX-License-Identifier: Apache-2.0
"""B-23 builder: N lights for the light-structure build measurement (tools/light_build_bench.py).
Default: 10,000 rectangular area lamps on a grid. --triangles N: one mesh of N separate emissive
triangles instead (the table then holds N emissive-triangle lights). The definition is in meta.toml.
Usage: build.py --out <path.arrec> [--lamps N | --triangles N]
Ref: docs/decisions/0009-build-test-process.md d13 row B-23; scenes/b1m/build.py (the scene shape)."""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402

LAMP_SIZE, LAMP_HEIGHT, PITCH, LEG, EMISSION = 0.2, 3.0, 1.0, 0.4, 10.0


def build(out, lamps, triangles):
    n = triangles or lamps
    width = int(math.ceil(math.sqrt(n)))
    with ar.Scene(out) as s:
        if triangles:
            gx, gy = np.meshgrid(np.arange(width), np.arange(width))
            origin = np.stack([gx.ravel(), gy.ravel(), np.zeros(width * width)], axis=1)[:triangles].astype(np.float32) * PITCH
            legs = np.array([[0, 0, 0], [LEG, 0, 0], [0, LEG, 0]], np.float32)
            pos = (origin[:, None, :] + legs[None]).reshape(-1, 3)
            corners = np.arange(3 * triangles, dtype=np.int32)
            normals = np.tile(np.array([0, 0, 1], np.float32), (3 * triangles, 1))
            mat = s.material(emission=((1.0, 1.0, 1.0), EMISSION))
            key = s.key()
            s.mesh(key, pos, corners, np.arange(0, 3 * triangles, 3, dtype=np.int32), normals, None, "B23_triangles")
            s.instances([{"geom": key, "materials": [mat]}])
        else:
            for i in range(lamps):
                x, y = i % width, i // width
                # A half turn about x points the lamp's emitting side down.
                s.light("rect", xf=[1, 0, 0, x * PITCH, 0, -1, 0, y * PITCH, 0, 0, -1, LAMP_HEIGHT],
                        power=10.0, size=(LAMP_SIZE, LAMP_SIZE))
        c = (width - 1) * PITCH / 2
        s.world((0.0, 0.0, 0.0), 1.0)
        s.settings(1, 64, 64, seed=1, sampler="sobol")
        s.camera((c, c, width * PITCH), (c, c, 0.0), 35.0, 36.0, 64, 64, up=(0.0, 1.0, 0.0))
        s.finish()
    print(f"counts: {lamps} lamps, {triangles} emissive triangles")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--lamps", type=int, default=0)
    g.add_argument("--triangles", type=int, default=0)
    a = ap.parse_args()
    build(a.out, a.lamps or (0 if a.triangles else 10000), a.triangles)
