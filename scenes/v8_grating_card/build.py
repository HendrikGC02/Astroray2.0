# SPDX-License-Identifier: Apache-2.0
"""V8 grating card builder: a flat 2 m square reflective sinusoidal GRATING card at z = 0 facing +z (600
lines/mm, grooves along x, so the grating vector runs along +y), a rectangular area lamp 60 m straight
above it facing down, and a pinhole camera at (0, 0, 0.2) tilted toward -y so that a film strip of
1024 x 16 pixels shows the camera directions from -3.5 to +58.5 degrees from the card's normal (the
card point a pixel sees is -h tan(theta) along y; by reciprocity a pixel is an outgoing angle). Image
up is +x (the groove direction), black world, independent sampler (the analytic harness needs the exact
variance of each pixel's mean, 0008 d3). Everything comes from meta.toml [truth], read by
truths.grating_orders too. Usage: build.py --out <path.arrec>.
Ref: docs/decisions/0005-material-model.md items 21-25, Verification 8; docs/decisions/0009-build-test-process.md
d16, d20; Palmer, Diffraction Grating Handbook, eq. 2-1."""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SEED = 1


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    width, height, spp = int(t["width"]), int(t["height"]), int(t["spp"])
    psi = math.radians(float(t["camera_axis_deg"]))
    cam = (0.0, 0.0, float(t["camera_height"]))
    # The camera looks along (0, -sin psi, -cos psi): toward the side of the grating vector's minus
    # direction, from which the positive orders are seen (truths.grating_profiles).
    target = (0.0, -math.sin(psi), cam[2] - math.cos(psi))
    sx, sy = float(t["lamp_size_x"]), float(t["lamp_size_y"])
    # Rectangle lamp radiance is P / (pi A) in Blender watts (0003 d11, as A3's disk): radiance 1 at this power.
    power = math.pi * sx * sy * float(t["radiance"])
    with ar.Scene(out) as s:
        card = s.key()
        s.mesh_from(card, meshes.plane(float(t["card_size"]), float(t["card_size"])), "card")
        grating = s.grating_material(1e6 / float(t["lines_per_mm"]), float(t["depth_nm"]),
                                     coating=(float(t["coating_n"]), float(t["coating_k"])),
                                     m_min=int(t["m_min"]), m_max=int(t["m_max"]))
        s.instances([{"geom": card, "materials": [grating]}])
        # Identity rotation: the lamp's front (local -Z) faces the card.
        s.light("rect", xf=ar.translation((0.0, 0.0, float(t["lamp_height"]))), power=power, size=(sx, sy))
        s.world((0.0, 0.0, 0.0), 0.0)
        s.settings(spp, width, height, seed=SEED, sampler="independent")
        # Up = +x: the image's vertical is the groove direction; image right is toward -y and larger angle.
        s.camera(cam, target, float(t["focal_mm"]), float(t["sensor_mm"]), width, height, up=(1.0, 0.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
