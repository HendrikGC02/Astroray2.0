# SPDX-License-Identifier: Apache-2.0
"""d2 closed furnace builder: a pinhole camera at the centre of a closed, faceted, emissive grey
Lambertian sphere, black world (0004 D2 row; truths.py uniform_enclosure). Usage: build.py --out
<path.arrec> [--rr off]. Russian roulette is on by default; off sets min_bounces = max_bounces + 1
(shade_common.h roulette_q applies it from depth max(2, min_bounces), and the last continuation of
a path is at depth max_bounces).
The sphere's normals are its facets' own: smooth normals would reject part of each reflected lobe
below the facet (0005 item 8) and bias every bounce (scenes/a1_furnace/meta.toml).
Ref: docs/decisions/0004-integrator.md D2 (Verification row); 0009 d19, d20."""
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


def flat(m):
    """The mesh with every corner's normal set to its triangle's geometric normal."""
    p = m.positions[m.corner_vert.reshape(-1, 3)].astype(np.float64)
    n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    return dataclasses.replace(m, normals=np.repeat(n, 3, axis=0).astype(np.float32))


def populate(s, t, rr=True):
    """The sphere in one grey Lambertian material of albedo rho emitting white Le; black world;
    settings and the camera at the centre."""
    rho = t["rho"]
    mat = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                      "base_metalness": 0.0, "specular_weight": 0.0}, emission=((1.0, 1.0, 1.0), t["radiance"]))
    key = s.key()
    s.mesh_from(key, flat(meshes.uv_sphere(t["sphere_radius"], t["segments"], t["rings"])), "enclosure")
    s.instances([{"geom": key, "materials": [mat]}])
    s.world((0.0, 0.0, 0.0), 0.0)
    d = t["max_depth"]
    s.settings(t["spp"], t["size"], t["size"], seed=SEED, sampler="independent", max_bounces=d,
               min_bounces=0 if rr else d + 1)
    s.camera((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), t["focal_mm"], t["sensor_mm"], t["size"], t["size"])


def build(out, rr=True):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    with ar.Scene(out) as s:
        populate(s, t, rr)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--rr", choices=("on", "off"), default="on")
    a = ap.parse_args()
    build(a.out, a.rr == "on")
