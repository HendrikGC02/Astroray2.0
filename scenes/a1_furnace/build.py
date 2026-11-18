# SPDX-License-Identifier: Apache-2.0
"""A1 builder: a Lambertian sphere (base colour rho, specular_weight 0, diffuse roughness 0) in a
constant world of radiance Lw, camera outside (0009 d20). Usage: build.py --out <path.arrec>.
Parameters come from meta.toml.
--blend <path.blend> also writes the scene description <path.blend>.json that scenes/lib/blend_writer.py
turns into a .blend inside Blender (Principled with Specular IOR Level 0, UV sphere, world Background colour);
--passes names film passes and --sampler picks the sampler mode (the F12 smoke compares Position, Normal, UV
and IndexMA AOVs of this recording and the addon's, which samples in Sobol mode, task 084).
Ref: docs/decisions/0009-build-test-process.md d16, d19, d20."""
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


def build(out, blend=None, passes=(), sampler="independent"):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    if t["camera_distance"] <= t["sphere_radius"]:
        raise ValueError("the camera is inside the sphere")
    rho = t["rho"]
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        mesh = s.key()
        s.mesh_from(mesh, meshes.uv_sphere(t["sphere_radius"], t["segments"], t["rings"]), "sphere")
        mat = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                          "base_metalness": 0.0, "specular_weight": 0.0,
                          "specular_roughness": 0.0})
        s.instances([{"geom": mesh, "materials": [mat]}])
        s.world((1.0, 1.0, 1.0), t["world_radiance"])
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler=sampler, passes=passes)
        s.camera((0.0, -t["camera_distance"], 0.0), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], WIDTH, HEIGHT)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    ap.add_argument("--passes", default="", help="comma-separated film pass names")
    ap.add_argument("--sampler", default="independent", choices=("independent", "sobol"))
    a = ap.parse_args()
    build(a.out, a.blend, tuple(p for p in a.passes.split(",") if p), a.sampler)
