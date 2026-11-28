# SPDX-License-Identifier: Apache-2.0
"""B-1M with a medium: b1m's displaced grid, lamp, world and camera, plus one closed box that bears the test
medium of constant coefficients and floats over the right half of the grid (its lowest face is above the
grid's highest point, so the surfaces do not overlap). The box's own surface has opacity 0, as scenes/v1_slab:
a ray passes it and the medium alone acts. It exists because the addon cannot send a medium, so this is the
only way a 1080p pass meets M1b's volume stages.
Usage: build.py --out <path.arrec>. Not an analytic scene: timed by tools/bench.ps1 only.
Ref: scenes/b1m/build.py (grid, lamp, world, camera); scenes/v1_slab/build.py (the opacity-0 shell and the
medium on an instance); docs/decisions/0006-volumes-and-astrophysics.md d3 (a medium bounded by a closed mesh)."""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENES = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SCENES, "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402

# One grid, lamp and camera for both scenes: b1m's, not a copy.
_spec = importlib.util.spec_from_file_location("b1m_build", os.path.join(SCENES, "b1m", "build.py"))
b1m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b1m)

BOX_SIZE = (8.0, 8.0, 2.0)          # m
BOX_CENTRE = (3.0, 0.0, 1.6)        # m: z from 0.6 to 2.6, over the grid's 0.45 m highest point
SIGMA_A, SIGMA_S, G = (0.02, 0.02, 0.02), (0.3, 0.3, 0.3), 0.3   # 1/m; a mild scattering fog, not a truth


def build(out):
    with ar.Scene(out) as s:
        mesh, box = s.key(), s.key()
        pos, cv, fs, nrm, uv = b1m.grid()
        s.mesh(mesh, pos, cv, fs, nrm, uv, "S1_mesh")
        s.mesh_from(box, meshes.box(*BOX_SIZE), "fog_box")
        mat = s.material({"base_color": (0.8, 0.4, 0.2), "specular_roughness": 0.5})
        shell = s.material({"base_color": (0.0, 0.0, 0.0), "specular_weight": 0.0, "geometry_opacity": 0.0})
        fog = s.medium(SIGMA_A, SIGMA_S, G)
        s.instances([{"geom": mesh, "materials": [mat]},
                     {"geom": box, "materials": [shell], "medium": fog, "xf": ar.translation(BOX_CENTRE)}])
        b1m.sun_lamp(s, (0.0, 0.0, 0.0), b1m.LAMP_DISTANCE, b1m.LAMP_SIZE, b1m.SUN_STRENGTH, b1m.SUN_EULER)
        s.world((0.3, 0.35, 0.4), 1.0)
        s.settings(b1m.SPP, 1920, 1080, seed=b1m.SEED, sampler="sobol")
        s.camera((0.0, -16.0, 12.0), (0.0, 0.0, 0.0), 35.0, 36.0, 1920, 1080)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
