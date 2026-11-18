# SPDX-License-Identifier: Apache-2.0
"""D2 builder: a few hundred instanced boxes, spheres and planes with one material each (its own
pass index, so IndexMA names the instance), varied per-axis scales and Euler orientations drawn
from a fixed NumPy seed, and one deliberate pair of instances with the same mesh and transform
(coincident faces), in a dim constant world (0009 d17(iv), d20). Usage: build.py --out <path.arrec>.
Parameters come from meta.toml. The AOVs are requested with the Position pass (0008 d10: Position
brings Normal, UV and IndexMA). Prints the pair's two pass indices on its `counts:` line.
Ref: docs/decisions/0009-build-test-process.md d17(iv), d19, d20; 0008 d10."""
import argparse
import ctypes
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

RNG_SEED = 20770  # the scene's layout; the render seed is [truth] seed


def euler_xyz(a, b, c):
    """Blender's XYZ Euler rotation as a matrix: Rz(c) Ry(b) Rx(a)."""
    ca, sa, cb, sb, cc, sc = math.cos(a), math.sin(a), math.cos(b), math.sin(b), math.cos(c), math.sin(c)
    rx = np.array([[1, 0, 0], [0, ca, -sa], [0, sa, ca]])
    ry = np.array([[cb, 0, sb], [0, 1, 0], [-sb, 0, cb]])
    rz = np.array([[cc, -sc, 0], [sc, cc, 0], [0, 0, 1]])
    return rz @ ry @ rx


def material(s, rgb, pass_index):
    """A diffuse material whose pass index is `pass_index` (ar.Scene.material leaves it 0)."""
    a = s.abi
    g = s.graph()
    g.store("base_color", rgb)
    gkey = s.key()
    s.set_graph(gkey, g)
    m = a.ar_material_desc()
    m.surface_graph, m.pass_index, m.emission_sampling = gkey, pass_index, a.AR_EMISSION_SAMPLING_AUTO
    key = s.key()
    s._call(s.lib.ar_edit_set_material(s.edit_h, key, ctypes.byref(m)), "ar_edit_set_material")
    return key


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    rng = np.random.default_rng(RNG_SEED)
    size = int(t["size"])
    with ar.Scene(out) as s:
        shapes = [s.key() for _ in range(3)]
        s.mesh_from(shapes[0], meshes.box(1.0, 1.0, 1.0), "box")
        s.mesh_from(shapes[1], meshes.uv_sphere(0.5, 24, 12), "sphere")
        s.mesh_from(shapes[2], meshes.plane(1.0, 1.0), "plane")
        items = []
        n = int(t["instances"])
        for i in range(n):
            rot = euler_xyz(*rng.uniform(0.0, 2.0 * math.pi, 3))
            scale = rng.uniform(t["scale_min"], t["scale_max"], 3)
            centre = rng.uniform(-t["extent"], t["extent"], 3)
            xf = np.concatenate([rot * scale[None, :], centre[:, None]], axis=1).reshape(12)
            rgb = tuple(float(v) for v in rng.uniform(0.1, 0.9, 3))
            items.append({"geom": shapes[i % 3], "materials": [material(s, rgb, i + 1)], "xf": xf})
        # The coincident pair: one box mesh, one transform, two materials.
        rot = euler_xyz(0.3, 0.5, 0.7) * float(t["pair_size"])
        xf = np.concatenate([rot, np.asarray(t["pair_position"], float)[:, None]], axis=1).reshape(12)
        pair = tuple(int(v) for v in t["pair_pass_indices"])
        if len(pair) != 2 or pair[0] == pair[1] or min(pair) <= n:
            raise ValueError("pair_pass_indices must be two distinct indices above the instance count")
        for idx, rgb in zip(pair, ((0.9, 0.1, 0.1), (0.1, 0.1, 0.9))):
            items.append({"geom": shapes[0], "materials": [material(s, rgb, idx)], "xf": xf})
        s.instances(items)
        s.world((1.0, 1.0, 1.0), 0.5)
        s.settings(int(t["spp"]), size, size, seed=int(t["seed"]), max_bounces=1, passes=("Position",))
        s.camera(t["camera_position"], (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], size, size)
        s.finish()
    print(f"counts: {n + 2} instances, coincident pair pass indices {pair[0]} {pair[1]}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
