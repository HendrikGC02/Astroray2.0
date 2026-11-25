# SPDX-License-Identifier: Apache-2.0
"""S3 under an HDRI (M1b showcase): scenes/s3_textured's meshes, instances and textured materials with
no lamp and the Poly Haven studio_small_09 EXR as the world; see meta.toml.
Usage: build.py --out <path.arrec> [--blend <path.blend>]; --blend also writes <blend>.json for
scenes/lib/blend_writer.py.
Ref: scenes/s3_textured/build.py (the geometry, the one definition of S3); assets/manifest.toml
(studio_small_09); scenes/lib/ar.py Scene.world_environment."""
import argparse
import importlib.util
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
S3 = os.path.join(ROOT, "scenes", "s3_textured")
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
sys.path.insert(0, S3)
import ar  # noqa: E402
import make_textures  # noqa: E402

_spec = importlib.util.spec_from_file_location("s3_textured_build", os.path.join(S3, "build.py"))
s3 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(s3)

HDRI = os.path.join(ROOT, "assets", "_cache", "studio_small_09", "studio_small_09_1k.exr")
SPP = 64
WIDTH, HEIGHT = 1280, 720


def build(out, blend=None):
    if not os.path.isfile(HDRI):
        raise SystemExit(f"{HDRI} is missing: run tools/fetch.ps1 assets")
    rng = np.random.default_rng(s3.SEED)
    paths = make_textures.ensure()
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        image_keys = [s.key() for _ in range(s3.N_TEXTURES)]
        for k, p in zip(image_keys, paths):
            s.image(k, path=p)
        materials = [s.textured_material(k, p, s3.ROUGHNESS) for k, p in zip(image_keys, paths)]
        mesh_keys = []
        for i in range(s3.N_MESHES):
            pos, cv, fs, nrm, uv = s3.unique_mesh(i, rng)
            key = s.key()
            s.mesh(key, pos, cv, fs, nrm, uv, f"S3_mesh{i:02d}")
            mesh_keys.append(key)
        items = []
        for n in range(s3.N_INSTANCES):
            ix, iy = n % s3.GRID[0], n // s3.GRID[0]
            a, sc = rng.uniform(0, 2 * math.pi), rng.uniform(0.8, 1.0)
            c, d = math.cos(a) * sc, math.sin(a) * sc
            xf = [c, -d, 0, (ix - (s3.GRID[0] - 1) / 2) * s3.PITCH, d, c, 0, iy * s3.PITCH, 0, 0, sc, rng.uniform(0.0, 3.0)]
            m = (n * 7) % s3.N_MESHES  # the same assignment as s3_textured
            items.append({"geom": mesh_keys[m], "materials": [materials[m % s3.N_TEXTURES]], "xf": xf})
        s.instances(items)
        s.world_environment(HDRI, 1.0)
        s.settings(SPP, WIDTH, HEIGHT, seed=s3.SEED, sampler="sobol")
        centre_y = (s3.GRID[1] - 1) * s3.PITCH / 2
        s.camera((0.0, -60.0, 55.0), (0.0, centre_y, 0.0), 35.0, 36.0, WIDTH, HEIGHT)
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
