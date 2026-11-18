# SPDX-License-Identifier: Apache-2.0
"""Small lit furnace builder: a grey Lambertian plane filling the view of a pinhole camera on the
+z axis looking down, a constant white world and a small sphere lamp outside the view, max_depth
from meta.toml (0004 D1 row; truths.py lit_furnace). Usage: build.py --out <path.arrec>.
Ref: docs/decisions/0004-integrator.md D1 (Verification row), D6, D8; 0009 d19, d20."""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SIZE = 32
SPP, SEED = 64, 1


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    seen = t["camera_height"] * (t["sensor_mm"] / t["focal_mm"]) / 2 * math.sqrt(2)
    if t["plane_size"] / 2 < seen:
        raise ValueError("the plane does not fill the camera's view")
    rho = t["rho"]
    with ar.Scene(out) as s:
        mesh = s.key()
        s.mesh_from(mesh, meshes.plane(t["plane_size"], t["plane_size"]), "floor")
        mat = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                          "base_metalness": 0.0, "specular_weight": 0.0})
        s.instances([{"geom": mesh, "materials": [mat]}])
        s.light("point", xf=ar.translation(t["lamp_position"]), rgb=tuple(t["colour"]), power=t["power"],
                size=(t["lamp_radius"], 0.0))
        s.world((1.0, 1.0, 1.0), t["world_radiance"])
        # max_bounces = max_depth with Russian roulette off (min_bounces at the limit).
        s.settings(SPP, SIZE, SIZE, seed=SEED, sampler="independent", max_bounces=t["max_depth"],
                   min_bounces=t["max_depth"])
        s.camera((0.0, 0.0, t["camera_height"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"],
                 SIZE, SIZE, up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
