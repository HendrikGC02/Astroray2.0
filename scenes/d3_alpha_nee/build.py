# SPDX-License-Identifier: Apache-2.0
"""d3 alpha NEE builder: an emissive quad of opacity 0.5 over a grey Lambertian plane, seen by a
pinhole camera on the +z axis looking down, world black. The NEE mode is chosen at render time
(ASTRORAY_TEST_NEE). The quad's Le carries its opacity (0005 item 38) at a BSDF hit and in NEE alike,
so the three estimators agree. Usage: build.py --out <path.arrec>.
Ref: docs/decisions/0004-integrator.md D1, D3, D14; 0005 items 4, 38; RULINGS.md 77; 0009 d19."""
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
# Rotation by pi about x: the quad's +z face turns to face the plane.
FLIP_X = ((1.0, 0.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, -1.0))


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    seen = t["camera_height"] * (t["sensor_mm"] / t["focal_mm"]) / 2 * math.sqrt(2)
    if t["plane_size"] / 2 < seen:
        raise ValueError("the plane does not fill the camera's view")
    rho = t["rho"]
    with ar.Scene(out) as s:
        floor, quad = s.key(), s.key()
        s.mesh_from(floor, meshes.plane(t["plane_size"], t["plane_size"]), "floor")
        q = t["quad"]
        s.mesh_from(quad, meshes.plane(q["size"], q["size"]), "quad")
        grey = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                           "base_metalness": 0.0, "specular_weight": 0.0})
        # A black emitter of opacity a: it only emits a x Le and passes or stops a ray, so every mode
        # sees the same reflected light (none). Front-only sampling with its front toward the plane.
        # The world is black: shadow rays treat the quad as opaque (transparent shadows are M2), so a
        # light behind it would be seen through it by BSDF rays and blocked for NEE.
        glow = s.material({"base_weight": 0.0, "specular_weight": 0.0, "geometry_opacity": q["opacity"]},
                          emission=((1.0, 1.0, 1.0), q["strength"]), emission_sampling=s.abi.AR_EMISSION_SAMPLING_FRONT)
        xq = [list(FLIP_X[i]) + [q["position"][i]] for i in range(3)]
        s.instances([{"geom": floor, "materials": [grey]}, {"geom": quad, "materials": [glow], "xf": xq}])
        s.world((0.0, 0.0, 0.0), 0.0)
        s.settings(SPP, SIZE, SIZE, seed=SEED, sampler="independent")
        s.camera((0.0, 0.0, t["camera_height"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"],
                 SIZE, SIZE, up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
