# SPDX-License-Identifier: Apache-2.0
"""Grating card builder: a 0.6 m square reflective GRATING card at z = 0 facing +z (a CD-like 1600 nm
pitch, 120 nm sinusoidal grooves, orders -2..2, a constant (1, 6) coating), grooves along x (the default
tangent of +z, core/frame.h), so the grating vector runs along y. A 5 cm square area lamp hangs at
(0, -0.25, 0.5) facing down and the camera at (0, 0.25, 0.5) looks at the card's centre, rolled so the
image columns run along y. The mirror point of the lamp is the card's centre (order 0); order +-1 at
lambda lands where (y_l - y) / r_l + (y_c - y) / r_c = +-lambda / d, about 0.09 m (450 nm) to 0.16 m
(700 nm) from the centre. Black world. Usage: build.py --out <path.arrec> [--blend <path.blend>]; --blend also writes <blend>.json for
scenes/lib/blend_writer.py.
Ref: docs/decisions/0005-material-model.md items 21-24 (model, sampling, the authored block); Palmer,
Diffraction Grating Handbook, eq. 2-1."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402

SIZE, SPP, SEED = 32, 16, 1
CARD = 0.6
LAMP_POSITION, LAMP_SIZE, LAMP_POWER = (0.0, -0.25, 0.5), 0.05, 5.0
CAMERA = (0.0, 0.25, 0.5)
FOCAL_MM, SENSOR_MM = 30.0, 36.0


def build(out, blend=None):
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        card = s.key()
        s.mesh_from(card, meshes.plane(CARD, CARD), "card")
        grating = s.grating_material(1600.0, 120.0, coating=(1.0, 6.0), m_min=-2, m_max=2)
        s.instances([{"geom": card, "materials": [grating]}])
        # Identity rotation: the area lamp's front (local -Z) faces the card.
        s.light("rect", xf=ar.translation(LAMP_POSITION), power=LAMP_POWER, size=(LAMP_SIZE, LAMP_SIZE))
        s.world((0.0, 0.0, 0.0), 1.0)  # explicit black: the .blend writer needs a world entry
        s.settings(SPP, SIZE, SIZE, seed=SEED)
        # Up = +x: the image's vertical is the groove direction, its columns the grating vector.
        s.camera(CAMERA, (0.0, 0.0, 0.0), FOCAL_MM, SENSOR_MM, SIZE, SIZE, up=(1.0, 0.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
