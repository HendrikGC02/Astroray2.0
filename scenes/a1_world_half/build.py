# SPDX-License-Identifier: Apache-2.0
"""A1 world-half builder: the A1 furnace with a white Lambertian sphere (rho 1) in a constant world of
radiance 0.5, so every pixel reads 0.5; the sphere is lit by world NEE and BSDF rays combined by the
balance heuristic, so a wrong MIS weight on either strategy shows (0004 D1, D7). Usage: build.py
--out <path.arrec>. Parameters come from meta.toml. Ref: docs/decisions/0009-build-test-process.md
d16, d19, d20; 0004 D1, D7."""
import argparse
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
    if t["camera_distance"] <= t["sphere_radius"]:
        raise ValueError("the camera is inside the sphere")
    rho = t["rho"]
    with ar.Scene(out) as s:
        mesh = s.key()
        s.mesh_from(mesh, meshes.uv_sphere(t["sphere_radius"], t["segments"], t["rings"]), "sphere")
        mat = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                          "base_metalness": 0.0, "specular_weight": 0.0})
        s.instances([{"geom": mesh, "materials": [mat]}])
        s.world((1.0, 1.0, 1.0), t["world_radiance"])
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler="independent")
        s.camera((0.0, -t["camera_distance"], 0.0), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], WIDTH, HEIGHT)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
