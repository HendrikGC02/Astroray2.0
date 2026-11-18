# SPDX-License-Identifier: Apache-2.0
"""d4 shadow-lifetime builder: d2's closed emissive furnace with opaque boxes inside in the same
material, Russian roulette on (0004 D4/D5 rows, opaque part; truths.py uniform_enclosure). The pool
sizes are chosen at render time (ASTRORAY_TEST_POOL). Usage: build.py --out <path.arrec>.
Self-contained (tools/scenes.py hashes only this folder and scenes/lib).
Ref: docs/decisions/0004-integrator.md D4, D5 (Verification row); 0002 D5; 0009 d19."""
import argparse
import dataclasses
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SEED = 1


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    rho = t["rho"]
    sphere = meshes.uv_sphere(t["sphere_radius"], t["segments"], t["rings"])
    # Flat facet normals, as d2: smooth ones would lose part of each reflected lobe (0005 item 8).
    p = sphere.positions[sphere.corner_vert.reshape(-1, 3)].astype(np.float64)
    n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    sphere = dataclasses.replace(sphere, normals=np.repeat(n / np.linalg.norm(n, axis=1, keepdims=True), 3, axis=0)
                                 .astype(np.float32))
    parts = [(sphere, ar.IDENTITY)] + [(meshes.box(*b["size"]), ar.translation(b["centre"])) for b in t["boxes"]]
    with ar.Scene(out) as s:
        mat = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                          "base_metalness": 0.0, "specular_weight": 0.0}, emission=((1.0, 1.0, 1.0), t["radiance"]))
        items = []
        for m, xf in parts:
            key = s.key()
            s.mesh_from(key, m, "enclosure")
            items.append({"geom": key, "materials": [mat], "xf": xf})
        s.instances(items)
        s.world((0.0, 0.0, 0.0), 0.0)
        s.settings(t["spp"], t["size"], t["size"], seed=SEED, sampler="independent", max_bounces=t["max_depth"],
                   min_bounces=0)
        s.camera((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), t["focal_mm"], t["sensor_mm"], t["size"], t["size"])
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
