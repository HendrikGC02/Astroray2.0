# SPDX-License-Identifier: Apache-2.0
"""B-1M builder: spike B's S1, a displaced 707 x 707 grid (499,849 quads, 999,698 triangles) with
corner normals and a UV map, one material, camera and world as in the spike (0009 d20).
Usage: build.py --out <path.arrec> [--blend <path.blend>]; --blend also writes <blend>.json, the
description scenes/lib/blend_writer.py turns into the .blend (tools/scenes.py does not run Blender).
The spike's sun lamp (3 W/m2) is a rectangular area lamp of equal irradiance at the scene centre
(sun lamps land in M4, meta.toml): L = P / (pi A) (0004 D7), so a lamp of area A at distance d
facing the centre gives E = P / (pi d^2) there, and P = pi d^2 E.
Ref: git show spikes-final:spikes/B-blender-end-to-end/make_scenes.py build_s1 (geometry, material,
camera, sun direction); docs/decisions/0009-build-test-process.md d19, d20; 0004 D7."""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402

N = 707                      # quads per side; Blender's create_grid(size=10) spans -10..10
HALF = 10.0
SPP, SEED = 16, 12345
SUN_EULER, SUN_STRENGTH = (0.6, 0.2, 0.4), 3.0   # the spike's lamp: rotation_euler XYZ, W/m2
LAMP_DISTANCE, LAMP_SIZE = 200.0, 10.0           # m; the lamp subtends 2.9 degrees (the sun 0.5)


def euler_xyz(a, b, c):
    """Blender's XYZ Euler rotation as a matrix: Rz(c) Ry(b) Rx(a)."""
    ca, sa, cb, sb, cc, sc = math.cos(a), math.sin(a), math.cos(b), math.sin(b), math.cos(c), math.sin(c)
    rx = np.array([[1, 0, 0], [0, ca, -sa], [0, sa, ca]])
    ry = np.array([[cb, 0, sb], [0, 1, 0], [-sb, 0, cb]])
    rz = np.array([[cc, -sc, 0], [sc, cc, 0], [0, 0, 1]])
    return rz @ ry @ rx


def sun_lamp(s, centre, distance, size, strength, euler):
    """An area lamp whose irradiance at `centre` equals a sun of `strength` shining along the
    Euler-rotated -z axis."""
    r = euler_xyz(*euler)
    pos = np.asarray(centre, float) - distance * (r @ np.array([0.0, 0.0, -1.0]))
    xf = np.concatenate([r, pos[:, None]], axis=1).reshape(12)
    s.light("rect", xf=xf, power=math.pi * distance ** 2 * strength, size=(size, size))


def smooth_vertex_normals(pos, quads):
    """Blender's smooth vertex normal: each face normal (Newell, here the cross of a quad's
    diagonals) weighted by the face's corner angle at the vertex, summed and normalised. The spike
    sets use_smooth on every polygon, so this is what Blender shades with; verified against
    Mesh.corner_normals of the same mesh in Blender 5.3 (the maximum angle difference is in the task's
    report).
    Ref: Blender source/blender/blenkernel/intern/mesh_normals.cc (vertex normals from face normals
    with corner-angle weights); make_scenes.py build_s1 (use_smooth = True)."""
    p = pos.astype(np.float64)
    fn = np.cross(p[quads[:, 2]] - p[quads[:, 0]], p[quads[:, 3]] - p[quads[:, 1]])
    fn /= np.linalg.norm(fn, axis=1, keepdims=True)
    acc = np.zeros((len(p), 3))
    for c in range(4):
        a = p[quads[:, (c + 3) % 4]] - p[quads[:, c]]
        b = p[quads[:, (c + 1) % 4]] - p[quads[:, c]]
        cosang = (a * b).sum(1) / (np.linalg.norm(a, axis=1) * np.linalg.norm(b, axis=1))
        w = np.arccos(np.clip(cosang, -1.0, 1.0))
        for k in range(3):
            acc[:, k] += np.bincount(quads[:, c], weights=w * fn[:, k], minlength=len(p))
    return (acc / np.linalg.norm(acc, axis=1, keepdims=True)).astype(np.float32)


def grid():
    """(positions, corner_vert, face_start, normals, uv) of the displaced grid. Vertex (i, j) is
    j * (N + 1) + i; a quad is counter-clockwise seen from +z, as Blender's create_grid."""
    xs = np.linspace(-HALF, HALF, N + 1, dtype=np.float32)
    x, y = np.meshgrid(xs, xs)  # x varies fastest in memory
    z = 0.4 * np.sin(x * 1.7) * np.cos(y * 1.3) + 0.05 * np.sin(x * 9) * np.sin(y * 11)
    pos = np.stack([x, y, z.astype(np.float32)], axis=-1).reshape(-1, 3)
    i, j = np.meshgrid(np.arange(N), np.arange(N))
    v00 = (j * (N + 1) + i).reshape(-1)
    corner_vert = np.stack([v00, v00 + 1, v00 + N + 2, v00 + N + 1], axis=1).reshape(-1).astype(np.int32)
    face_start = (np.arange(N * N, dtype=np.int32) * 4)
    vn = smooth_vertex_normals(pos, corner_vert.reshape(-1, 4))
    # Planar UV over the grid, the spike's add_uv: xy * 0.1 + 0.5.
    uv = pos[corner_vert, :2] * 0.1 + 0.5
    return pos, corner_vert, face_start, vn[corner_vert], uv.astype(np.float32)


def build(out, blend=None):
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        mesh = s.key()
        pos, cv, fs, nrm, uv = grid()
        s.mesh(mesh, pos, cv, fs, nrm, uv, "S1_mesh")
        mat = s.material({"base_color": (0.8, 0.4, 0.2), "specular_roughness": 0.5})
        s.instances([{"geom": mesh, "materials": [mat]}])
        sun_lamp(s, (0.0, 0.0, 0.0), LAMP_DISTANCE, LAMP_SIZE, SUN_STRENGTH, SUN_EULER)
        s.world((0.3, 0.35, 0.4), 1.0)
        s.settings(SPP, 1920, 1080, seed=SEED, sampler="sobol")
        s.camera((0.0, -16.0, 12.0), (0.0, 0.0, 0.0), 35.0, 36.0, 1920, 1080)
        s.finish()
    print(f"counts: {2 * len(fs)} triangles, 1 instances, {len(pos)} vertices")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
