# SPDX-License-Identifier: Apache-2.0
"""Bitterli's Cornell box (CC0) in two variants: `cornell_area`, the original rectangular lamp as an
area lamp, and `cornell_point`, a point lamp of equal power at the lamp's centre (ROADMAP M1a "Owner
sees"). Materials are the M1a set: constant base colour from the Lambertian albedo, specular_weight 0.
Not a scene itself (no build.py here): scenes/cornell_area/build.py and scenes/cornell_point/build.py call
main(variant), so tools/scenes.py maps each directory to one recording. --blend writes <blend>.json, the
description scenes/lib/blend_writer.py turns into the .blend.
Lamp: an area lamp's radiance is L = P / (pi A) (0004 D7, front face only), so the authored radiance
(17, 12, 4) on the 0.47 x 0.38 rectangle is P = pi A max(L) with colour L / max(L), and the colour times
P is the flux in each channel. A point lamp has I = P / (4 pi) (0004 D6), so the same colour and P
radiate the same flux from the lamp's centre.
Ref: scenes/cornell/convert_bitterli.py (the archive's shapes, albedos, light and camera);
docs/decisions/0004-integrator.md D6, D7; 0009 d20 (G class)."""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
sys.path.insert(0, HERE)
import ar  # noqa: E402
import convert_bitterli  # noqa: E402

ARCHIVE = os.path.join(ROOT, "assets", "_cache", "cornell_box", "cornell-box.zip")
SPP, SEED = 256, 1
MAX_BOUNCES = 65        # the archive's max_depth
SENSOR_MM = 36.0


def lamp_fields(light):
    """(xf, rgb, power) of the lamp for the area variant: the frame emits along -z, as Blender's area lamp."""
    z = -light.normal
    x = light.x_axis
    y = np.cross(z, x)
    xf = np.concatenate([np.stack([x, y, z], axis=1), light.centre[:, None]], axis=1).reshape(12)
    radiance = np.array(light.radiance)
    area = light.size[0] * light.size[1]
    peak = float(radiance.max())
    return xf, tuple(float(c) for c in radiance / peak), math.pi * area * peak


def build(variant, out, blend=None):
    box = convert_bitterli.load(ARCHIVE)
    cam = box.camera
    focal = 0.5 * SENSOR_MM / math.tan(math.radians(cam.fov_deg) / 2)  # the fov is along x, the fitted side
    xf, rgb, power = lamp_fields(box.light)
    inward = np.array([0.0, 0.0, 1.0])  # the box's centre: every rectangle faces it
    triangles = 0
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        materials, instances = {}, []
        for shape in box.shapes:
            if shape.albedo not in materials:
                materials[shape.albedo] = s.material({"base_color": shape.albedo, "specular_weight": 0.0})
            pos, cv, nrm = convert_bitterli.shape_mesh(shape, inward)
            key = s.key()
            s.mesh(key, pos, cv, np.arange(len(cv) // 3, dtype=np.int32) * 3, nrm, None, shape.name)
            instances.append({"geom": key, "materials": [materials[shape.albedo]]})
            triangles += len(cv) // 3
        s.instances(instances)
        if variant == "area":
            s.light("rect", xf=xf, rgb=rgb, power=power, size=box.light.size)
        else:
            s.light("point", xf=ar.translation(box.light.centre), rgb=rgb, power=power, size=(0.0, 0.0))
        s.world((0.0, 0.0, 0.0), 0.0)
        s.settings(SPP, cam.width, cam.height, seed=SEED, max_bounces=MAX_BOUNCES, sampler="sobol")
        s.camera(cam.position, cam.target, focal, SENSOR_MM, cam.width, cam.height, up=cam.up)
        s.finish()
    return triangles, len(instances), len(materials)


def main(variant):
    """Command line of a variant's build.py: --out <path.arrec> [--blend <path.blend>]; prints the counts line."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this variant is for; writes <blend>.json beside it")
    a = ap.parse_args()
    t, n, m = build(variant, a.out, a.blend)
    print(f"counts: {t} triangles, {n} instances, {m} materials")
