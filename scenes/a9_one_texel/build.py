# SPDX-License-Identifier: Apache-2.0
"""A9 one-texel builder: a Lambertian sphere under an equirectangular environment that is dim
everywhere but one bright texel (Closest filtering), seen by A1's camera; the image is generated
here (RGBE bytes, scenes/lib/ar.py rgbe_bytes) from meta.toml, so it and the truth cannot drift
apart. Usage: build.py --out <path.arrec>.
Ref: docs/decisions/0009-build-test-process.md d16, d19, d20; 0004 D11; research 09 S12."""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

WIDTH = HEIGHT = 32
SPP, SEED = 64, 1


def environment(t):
    """(rows, columns, 3) radiance in file order (top row first): `dim` with the bright texel, whose
    row is counted from the bottom (the image's rows are stored bottom-up, v = 1 is the top)."""
    w, h = (int(v) for v in t["image_size"])
    i, j = (int(v) for v in t["texel"])
    img = np.full((h, w, 3), float(t["dim"]))
    img[h - 1 - j, i] = float(t["bright"])
    return img


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    if t["camera_distance"] <= t["sphere_radius"]:
        raise ValueError("the camera is inside the sphere")
    rho = t["rho"]
    with ar.Scene(out, workers=0) as s:
        mesh = s.key()
        s.mesh_from(mesh, meshes.uv_sphere(t["sphere_radius"], t["segments"], t["rings"]), "sphere")
        mat = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                          "base_metalness": 0.0, "specular_weight": 0.0})
        s.instances([{"geom": mesh, "materials": [mat]}])
        image = s.key()
        s.image(image, data=ar.rgbe_bytes(environment(t)), interop=s.abi.AR_COLOR_INTEROP_LIN_REC709)
        s.world_image(image, t["strength"], linear=False)
        # Independent mode: the analytic harness needs the exact variance of each pixel's mean (0008 d3).
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler="independent")
        s.camera((0.0, -t["camera_distance"], 0.0), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], WIDTH, HEIGHT)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
