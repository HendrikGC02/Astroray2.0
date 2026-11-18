# SPDX-License-Identifier: Apache-2.0
"""d15 radiance-scale builder: a grey Lambertian plane under a rectangle lamp, a pinhole camera
looking down, black world (0003 Verification check 15). Usage: build.py --out <path.arrec>.
Ref: docs/decisions/0003-spectral-model-and-path-state.md d10, Verification check 15; 0009 d19."""
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
    rho = t["rho"]
    with ar.Scene(out) as s:
        plane = s.key()
        s.mesh_from(plane, meshes.plane(6.0, 6.0), "floor")
        grey = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                           "base_metalness": 0.0, "specular_weight": 0.0})
        s.instances([{"geom": plane, "materials": [grey]}])
        s.light("rect", xf=ar.translation(t["lamp_position"]), power=t["lamp_power"],
                size=(t["lamp_size"], t["lamp_size"]))
        s.world((0.0, 0.0, 0.0), 0.0)
        s.settings(t["spp"], t["size"], t["size"], seed=SEED, sampler="independent")
        s.camera((0.0, 0.0, t["camera_height"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], t["size"], t["size"],
                 up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
