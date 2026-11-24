# SPDX-License-Identifier: Apache-2.0
"""A1 with a TABLE base colour: a Lambertian sphere (specular_weight 0, diffuse roughness 0) whose
base colour is a SPECTRUM_TABLE (0005 item 9) in a constant white world of radiance Lw, camera outside
(0009 d20). Usage: build.py --out <path.arrec>. Parameters come from meta.toml.
Ref: docs/decisions/0005-material-model.md item 9, Verification 4; 0009 d16, d19, d20."""
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


def build(out, passes=(), sampler="independent"):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    if t["camera_distance"] <= t["sphere_radius"]:
        raise ValueError("the camera is inside the sphere")
    with ar.Scene(out) as s:
        mesh = s.key()
        s.mesh_from(mesh, meshes.uv_sphere(t["sphere_radius"], t["segments"], t["rings"]), "sphere")
        g = s.graph()
        g.store("base_weight", 1.0)
        g.store("base_color", g.spectrum_table(t["table_nm"], t["table_value"], t["table_scale"]))
        g.store("base_diffuse_roughness", 0.0)
        g.store("base_metalness", 0.0)
        g.store("specular_weight", 0.0)
        g.store("specular_roughness", 0.0)
        mat = s.material_from_graph(g)
        s.instances([{"geom": mesh, "materials": [mat]}])
        s.world((1.0, 1.0, 1.0), t["world_radiance"])
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler=sampler, passes=passes)
        s.camera((0.0, -t["camera_distance"], 0.0), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], WIDTH, HEIGHT)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--passes", default="", help="comma-separated film pass names")
    ap.add_argument("--sampler", default="independent", choices=("independent", "sobol"))
    a = ap.parse_args()
    build(a.out, tuple(p for p in a.passes.split(",") if p), a.sampler)
