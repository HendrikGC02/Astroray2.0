# SPDX-License-Identifier: Apache-2.0
"""S3 (B-TX), the textured benchmark scene of 0007 d35 and 0009 d20, which neither record specifies:
1,000 instances of 50 unique UV-mapped meshes (about 1 M triangles instanced), 32 distinct 2048 x 2048
RGBA8 sRGB textures (make_textures.py, procedural, written to out/scenes/s3_textures/), each driving
the base colour of its own material through an Image Texture node (roughness 0.5), one rectangular area
lamp, a constant world. The definition is recorded in meta.toml. Textures dominate this scene's sync
and first frame, which is what the gate wants S3 to measure.
Usage: build.py --out <path.arrec> [--blend <path.blend>]; --blend also writes <blend>.json, the
description scenes/lib/blend_writer.py turns into the .blend (file-backed images: the engine refuses
generated images until M1b).
Lamp: L = P / (pi A) (0004 D7); a lamp of area A facing down at height d over the grid's centre gives
about E = P / (pi d^2) there for d large against its size, so P = pi d^2 E with E = 3 W/m2, B-1M's.
Ref: docs/decisions/0007-blender-integration.md d35; 0009-build-test-process.md d19, d20, d21 (S3 is
the textured scene); scenes/lib/ar.py Graph.tex_image; scenes/lib/meshes.py uv_sphere."""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
sys.path.insert(0, HERE)
import ar  # noqa: E402
import make_textures  # noqa: E402
import meshes  # noqa: E402

SEED, N_MESHES, N_INSTANCES, N_TEXTURES = 4242, 50, 1000, make_textures.COUNT
GRID = (40, 25)          # instances along x and y
PITCH = 2.4              # m between instance centres
RINGS = 21               # 2 * segments * (RINGS - 1) = 40 * segments triangles per sphere
ROUGHNESS = 0.5
LAMP_HEIGHT, LAMP_SIZE, LAMP_IRRADIANCE = 45.0, 60.0, 3.0
SPP = 16
WIDTH, HEIGHT = 1920, 1080


class TexturedScene(ar.Scene):
    """ar.Scene with a material whose base colour is an Image Texture. The .blend description gets the
    file path so blend_writer.py can make the same node."""

    def textured_material(self, image_key, path, roughness):
        g = self.graph()
        g.store("base_color", g.tex_image(g.uv(), image_key, linear=True, extend=False))
        g.store("specular_roughness", roughness)
        key = self.material_from_graph(g)
        self._desc["materials"].append({
            "key": key, "inputs": {"specular_roughness": float(roughness)}, "emission": None,
            "base_color_image": {"path": os.path.abspath(path), "colorspace": "sRGB",
                                 "interpolation": "Linear", "extension": "REPEAT"}})
        return key


def vertex_normals(pos, cv):
    """Smooth per-vertex normals (area-weighted face normals), as corner normals."""
    t = cv.reshape(-1, 3)
    p = pos.astype(np.float64)
    fn = np.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]])
    acc = np.zeros_like(p)
    for c in range(3):
        np.add.at(acc, t[:, c], fn)
    acc /= np.maximum(np.linalg.norm(acc, axis=1, keepdims=True), 1e-20)
    return acc[cv].astype(np.float32)


def unique_mesh(i, rng):
    """Mesh i: a UV sphere of 840 to 1,160 triangles, squashed and bumped differently per mesh."""
    base = meshes.uv_sphere(1.0, segments=21 + i % 9, rings=RINGS)
    pos = base.positions.astype(np.float64)
    theta = np.arccos(np.clip(pos[:, 2], -1.0, 1.0))
    phi = np.arctan2(pos[:, 1], pos[:, 0])
    bump = 1.0 + 0.10 * np.sin(rng.integers(2, 6) * phi + rng.random() * 6.28) * np.sin(rng.integers(1, 4) * theta)
    scale = rng.uniform(0.7, 1.1, 3)
    pos = (pos * bump[:, None] * scale).astype(np.float32)
    return pos, base.corner_vert, base.face_start, vertex_normals(pos, base.corner_vert), base.uv


def build(out, blend=None):
    rng = np.random.default_rng(SEED)
    paths = make_textures.ensure()
    with TexturedScene(out, description=blend + ".json" if blend else None) as s:
        image_keys = [s.key() for _ in range(N_TEXTURES)]
        for k, p in zip(image_keys, paths):
            s.image(k, path=p)
        materials = [s.textured_material(k, p, ROUGHNESS) for k, p in zip(image_keys, paths)]
        mesh_keys, unique_tris = [], 0
        for i in range(N_MESHES):
            pos, cv, fs, nrm, uv = unique_mesh(i, rng)
            key = s.key()
            s.mesh(key, pos, cv, fs, nrm, uv, f"S3_mesh{i:02d}")
            mesh_keys.append((key, len(fs)))
            unique_tris += len(fs)
        items, instanced_tris = [], 0
        for n in range(N_INSTANCES):
            ix, iy = n % GRID[0], n // GRID[0]
            a, sc = rng.uniform(0, 2 * math.pi), rng.uniform(0.8, 1.0)
            c, d = math.cos(a) * sc, math.sin(a) * sc
            xf = [c, -d, 0, (ix - (GRID[0] - 1) / 2) * PITCH, d, c, 0, iy * PITCH, 0, 0, sc, rng.uniform(0.0, 3.0)]
            m = (n * 7) % N_MESHES  # 7 is coprime with 50: neighbours differ and every mesh is used 20 times
            items.append({"geom": mesh_keys[m][0], "materials": [materials[m % N_TEXTURES]], "xf": xf})
            instanced_tris += mesh_keys[m][1]
        s.instances(items)
        centre = (0.0, (GRID[1] - 1) * PITCH / 2, 0.0)
        s.light("rect", xf=ar.translation((centre[0], centre[1], LAMP_HEIGHT)),
                power=math.pi * LAMP_HEIGHT ** 2 * LAMP_IRRADIANCE, size=(LAMP_SIZE, LAMP_SIZE))
        s.world((0.3, 0.35, 0.4), 1.0)
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler="sobol")
        s.camera((0.0, -60.0, 55.0), (0.0, centre[1], 0.0), 35.0, 36.0, WIDTH, HEIGHT)
        s.finish()
    print(f"counts: {instanced_tris} instanced triangles ({unique_tris} unique) in {N_MESHES} meshes, "
          f"{len(items)} instances, {N_TEXTURES} textures of {make_textures.SIZE}x{make_textures.SIZE}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
