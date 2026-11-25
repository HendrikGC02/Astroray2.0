# SPDX-License-Identifier: Apache-2.0
"""Shader balls (M1b showcase): a 5 x 2 grid of UV spheres on a diffuse ground under the Poly Haven
studio_small_09 HDRI; see meta.toml for the layout.
Usage: build.py --out <path.arrec> [--blend <path.blend>]; --blend also writes <blend>.json for
scenes/lib/blend_writer.py.
Ref: assets/manifest.toml (studio_small_09); scenes/lib/ar.py Scene.world_environment;
scenes/lib/meshes.py uv_sphere, plane."""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402

HDRI = os.path.join(ROOT, "assets", "_cache", "studio_small_09", "studio_small_09_1k.exr")
ROUGHNESS = (0.05, 0.2, 0.4, 0.7, 1.0)
GOLD, GREY = (1.0, 0.78, 0.34), (0.8, 0.8, 0.8)
RADIUS, PITCH, ROW_PITCH = 1.0, 2.5, 3.4
SPP = 256
WIDTH, HEIGHT = 1280, 720
SEED = 1


def build(out, blend=None):
    if not os.path.isfile(HDRI):
        raise SystemExit(f"{HDRI} is missing: run tools/fetch.ps1 assets")
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        ground = s.key()
        s.mesh_from(ground, meshes.plane(60.0, 60.0), "ground")
        items = [{"geom": ground, "materials": [s.material({"base_color": (0.5, 0.5, 0.5), "specular_roughness": 1.0})]}]
        # Back row metal, front row dielectric; the camera looks down on them so the back row sits higher.
        for row, (y, metalness, colour) in enumerate(((ROW_PITCH / 2, 1.0, GOLD), (-ROW_PITCH / 2, 0.0, GREY))):
            for i, rough in enumerate(ROUGHNESS):
                mat = s.material({"base_color": colour, "base_metalness": metalness, "specular_roughness": rough})
                # One mesh per ball: the .blend writer gives a mesh one material list.
                sphere = s.key()
                s.mesh_from(sphere, meshes.uv_sphere(RADIUS, 96, 48), f"ball {row}{i}")
                x = (i - (len(ROUGHNESS) - 1) / 2) * PITCH
                items.append({"geom": sphere, "materials": [mat], "xf": ar.translation((x, y, RADIUS))})
        s.instances(items)
        s.world_environment(HDRI, 1.0)
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler="sobol")
        s.camera((0.0, -19.0, 11.0), (0.0, 0.0, 1.0), 50.0, 36.0, WIDTH, HEIGHT)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
