# SPDX-License-Identifier: Apache-2.0
"""A5 builder: a black-based emissive plane of radiance L facing a pinhole camera and filling its
view, world black (0009 d20). Usage: build.py --out <path.arrec>. Parameters come from meta.toml.
Ref: docs/decisions/0009-build-test-process.md d19, d20."""
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
    # Half the sensor over the focal length is the tangent of the half field of view; the view's
    # corner reaches that times sqrt 2 at the plane, and the plane's inscribed half-size must cover it.
    seen = t["camera_distance"] * (t["sensor_mm"] / t["focal_mm"]) / 2 * math.sqrt(2)
    if t["plane_size"] / 2 < seen:
        raise ValueError("the plane does not fill the camera's view")
    with ar.Scene(out) as s:
        mesh = s.key()
        s.mesh_from(mesh, meshes.plane(t["plane_size"], t["plane_size"]), "emitter")
        # Black base and no specular: the plane only emits, so its pixel is L whatever it reflects.
        mat = s.material({"base_color": (0.0, 0.0, 0.0), "specular_weight": 0.0},
                         emission=(t["colour"], t["radiance"]))
        s.instances([{"geom": mesh, "materials": [mat]}])
        s.world((0.0, 0.0, 0.0), 0.0)
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED)
        s.camera((0.0, 0.0, t["camera_distance"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"],
                 WIDTH, HEIGHT, up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
