# SPDX-License-Identifier: Apache-2.0
"""d8 stacked lamps builder: two rectangle lamps stacked above a grey Lambertian floor, both facing
down, seen from below by a low pinhole camera, world black. The NEE mode is chosen at render time
(ASTRORAY_TEST_NEE). Usage: build.py --out <path.arrec>. tools/analytic.py also calls populate()
on a scene it keeps open, to read the engine's crossing counters.
Ref: docs/decisions/0004-integrator.md D8 (Verification row), D29; RULINGS.md 77; 0009 d19."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SIZE = 32
SPP, SEED = 64, 1


def populate(s, spp=SPP, size=SIZE):
    """Everything but finish(): geometry, lamps, world, settings and camera."""
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    rho = t["rho"]
    floor = s.key()
    s.mesh_from(floor, meshes.plane(t["floor_size"], t["floor_size"]), "floor")
    grey = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                       "base_metalness": 0.0, "specular_weight": 0.0})
    s.instances([{"geom": floor, "materials": [grey]}])
    # Identity rotation: each lamp's front (local -Z) faces the floor and the camera below it.
    for lamp in (t["lower"], t["upper"]):
        s.light("rect", xf=ar.translation(lamp["position"]), power=lamp["power"], size=(lamp["size"], lamp["size"]))
    s.world((0.0, 0.0, 0.0), 0.0)
    s.settings(spp, size, size, seed=SEED, sampler="independent")
    s.camera(tuple(t["camera_position"]), tuple(t["camera_target"]), t["focal_mm"], t["sensor_mm"], size, size,
             up=(0.0, 0.0, 1.0))


def build(out):
    with ar.Scene(out) as s:
        populate(s)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
