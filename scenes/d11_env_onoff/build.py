# SPDX-License-Identifier: Apache-2.0
"""d11 environment NEE on/off builder: a grey Lambertian plane under an equirectangular environment of
a smooth sky with a bright patch (Linear filtering), seen by a pinhole camera on the +z axis looking
down. The image is generated here (RGBE bytes, scenes/lib/ar.py rgbe_bytes) from meta.toml. The NEE
mode is chosen at render time (ASTRORAY_TEST_NEE). Usage: build.py --out <path.arrec>.
Ref: docs/decisions/0004-integrator.md D1, D11 (Verification rows); RULINGS.md 77; 0009 d19."""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SIZE = 32
SPP, SEED = 64, 1


def environment(t):
    """(rows, columns, 3) radiance in file order (top row first). Row j from the bottom has centre
    height z = -cos(pi (j + 0.5) / rows) (Blender's equirectangular map: v = 1 is +z)."""
    w, h = (int(v) for v in t["image_size"])
    img = np.zeros((h, w, 3))
    for j in range(h):
        z = -math.cos(math.pi * (j + 0.5) / h)
        img[h - 1 - j, :, :] = t["sky_base"] + t["sky_gain"] * (1.0 + z) / 2.0
    (pi, pj), (pw, ph) = t["patch"], t["patch_size"]
    for j in range(pj, pj + ph):
        img[h - 1 - j, pi:pi + pw, :] = t["patch_radiance"]
    return img


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    seen = t["camera_height"] * (t["sensor_mm"] / t["focal_mm"]) / 2 * math.sqrt(2)
    if t["plane_size"] / 2 < seen:
        raise ValueError("the plane does not fill the camera's view")
    rho = t["rho"]
    with ar.Scene(out, workers=0) as s:
        floor = s.key()
        s.mesh_from(floor, meshes.plane(t["plane_size"], t["plane_size"]), "floor")
        grey = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                           "base_metalness": 0.0, "specular_weight": 0.0})
        s.instances([{"geom": floor, "materials": [grey]}])
        image = s.key()
        s.image(image, data=ar.rgbe_bytes(environment(t)), interop=s.abi.AR_COLOR_INTEROP_LIN_REC709)
        s.world_image(image, 1.0, linear=True)
        s.settings(SPP, SIZE, SIZE, seed=SEED, sampler="independent")
        s.camera((0.0, 0.0, t["camera_height"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"],
                 SIZE, SIZE, up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
