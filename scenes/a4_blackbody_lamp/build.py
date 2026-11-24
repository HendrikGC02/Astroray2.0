# SPDX-License-Identifier: Apache-2.0
"""A4 blackbody lamp builder: a point lamp of Planck colour (radius 0, a delta light found by NEE only) over a grey Lambertian plane filling the view of a pinhole
camera on the +z axis looking down, world black (0009 d20: pixel = rho / pi E). Usage: build.py
--out <path.arrec>. Parameters come from meta.toml. Ref: docs/decisions/0009-build-test-process.md
d19, d20; 0004 D6, D8; 0003 d11."""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

WIDTH = HEIGHT = 32
SPP, SEED = 64, 1


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    # As A5: the view's corner reaches tan(half fov) sqrt 2 times the height at the plane.
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
        # A point lamp of radius 0 is the delta point light; a radius makes it the sphere lamp (0004 D6).
        # The colour is Planck(T) with display Y = scale (0003 d11); the truth reads the same two keys.
        s.light("point", xf=ar.translation(t["lamp_position"]), power=t["power"], size=(t["lamp_radius"], 0.0),
                blackbody_kelvin=t["blackbody_kelvin"], blackbody_scale=t["blackbody_scale"])
        s.world((0.0, 0.0, 0.0), 0.0)
        # Independent mode: the analytic harness needs the exact variance of each pixel's mean (0008 d3).
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler="independent")
        s.camera((0.0, 0.0, t["camera_height"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"],
                 WIDTH, HEIGHT, up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)