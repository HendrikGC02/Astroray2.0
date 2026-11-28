# SPDX-License-Identifier: Apache-2.0
"""B-1M at M1b's viewport reach: b1m's displaced grid, area lamp and camera under the studio_small_09 HDRI (the
world is the environment the light tree samples), one 4 m GRATING card floating over the grid, and a row of
upright alpha cards between the camera and the grid. One Blender viewport session therefore has non-opaque
materials, an extension-routed material and an environment light in the scene.
Usage: build.py --out <path.arrec> [--blend <path.blend>]; --blend also writes <blend>.json for
scenes/lib/blend_writer.py. Not an analytic scene: timed by tools/measure_viewport.ps1 only.
Ref: scenes/b1m/build.py (grid, lamp, camera; imported, not copied); scenes/s3_hdri/build.py (the HDRI world);
scenes/ext_grating_card/build.py (the grating's pitch, depth and coating); scenes/b_foliage/build.py (an alpha
material as geometry_opacity)."""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENES = os.path.dirname(HERE)
ROOT = os.path.dirname(SCENES)
sys.path.insert(0, os.path.join(SCENES, "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402

_spec = importlib.util.spec_from_file_location("b1m_build", os.path.join(SCENES, "b1m", "build.py"))
b1m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b1m)

HDRI = os.path.join(ROOT, "assets", "_cache", "studio_small_09", "studio_small_09_1k.exr")
GRATING_SIZE, GRATING_CENTRE = 4.0, (0.0, 0.0, 1.5)     # m; above the grid's 0.45 m highest point
GRATING = dict(pitch_nm=1600.0, depth_nm=120.0, coating=(1.0, 6.0), m_min=-2, m_max=2)
ALPHA_COUNT, ALPHA_OPACITY, ALPHA_SIZE = 5, 0.5, 2.0    # cards of 2 m by 2 m, upright, facing the camera
ALPHA_ROW_Y, ALPHA_ROW_Z, ALPHA_PITCH = -5.0, 1.2, 2.4  # m: the row centre line, its x spacing


def upright(centre):
    """A plane (facing +z) turned to face -y, the camera's side: rotation of +90 degrees about x."""
    return [1, 0, 0, centre[0], 0, 0, -1, centre[1], 0, 1, 0, centre[2]]


def build(out, blend=None):
    if not os.path.isfile(HDRI):
        raise SystemExit(f"{HDRI} is missing: run tools/fetch.ps1 assets")
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        mesh, card, leaf = s.key(), s.key(), s.key()
        pos, cv, fs, nrm, uv = b1m.grid()
        s.mesh(mesh, pos, cv, fs, nrm, uv, "S1_mesh")
        s.mesh_from(card, meshes.plane(GRATING_SIZE, GRATING_SIZE), "grating_card")
        mat = s.material({"base_color": (0.8, 0.4, 0.2), "specular_roughness": 0.5})
        grating = s.grating_material(**GRATING)
        items = [{"geom": mesh, "materials": [mat]},
                 {"geom": card, "materials": [grating], "xf": ar.translation(GRATING_CENTRE)}]
        s.mesh_from(leaf, meshes.plane(ALPHA_SIZE, ALPHA_SIZE), "alpha_card")
        glass = s.material({"base_color": (0.1, 0.5, 0.15), "specular_roughness": 0.6, "geometry_opacity": ALPHA_OPACITY})
        first = -(ALPHA_COUNT - 1) / 2 * ALPHA_PITCH
        items += [{"geom": leaf, "materials": [glass], "xf": upright((first + k * ALPHA_PITCH, ALPHA_ROW_Y, ALPHA_ROW_Z))}
                  for k in range(ALPHA_COUNT)]
    s.instances(items)
        b1m.sun_lamp(s, (0.0, 0.0, 0.0), b1m.LAMP_DISTANCE, b1m.LAMP_SIZE, b1m.SUN_STRENGTH, b1m.SUN_EULER)
        s.world_environment(HDRI, 1.0)
        s.settings(b1m.SPP, 1920, 1080, seed=b1m.SEED, sampler="sobol")
        s.camera((0.0, -16.0, 12.0), (0.0, 0.0, 0.0), 35.0, 36.0, 1920, 1080)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
