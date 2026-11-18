# SPDX-License-Identifier: Apache-2.0
"""d28 closed-box builder: a pinhole camera at the centre of a closed grey Lambertian box with a
point lamp inside, black world, Russian roulette off (0004 D28 row, 0008 V8). Usage: build.py --out
<path.arrec>.
Ref: docs/decisions/0004-integrator.md D28 (Verification row); 0008 V8; 0009 d18, d19."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SEED = 1


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    rho, d = t["rho"], t["max_depth"]
    with ar.Scene(out) as s:
        box = s.key()
        s.mesh_from(box, meshes.box(*t["box_size"]), "box")
        grey = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                           "base_metalness": 0.0, "specular_weight": 0.0})
        s.instances([{"geom": box, "materials": [grey]}])
        s.light("point", xf=ar.translation(t["lamp_position"]), power=t["lamp_power"])
        s.world((0.0, 0.0, 0.0), 0.0)
        # min_bounces = max_depth + 1: the kernel's roulette applies from depth max(2, min_bounces) and a
        # path's last continuation is at depth max_depth, so min_bounces = max_depth would still roulette it.
        s.settings(t["spp"], t["size"], t["size"], seed=SEED, sampler="independent", max_bounces=d, min_bounces=d + 1)
        s.camera((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), t["focal_mm"], t["sensor_mm"], t["size"], t["size"])
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
