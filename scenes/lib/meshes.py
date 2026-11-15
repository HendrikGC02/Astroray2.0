# SPDX-License-Identifier: Apache-2.0
"""Deterministic triangle meshes in metres for scene builders (no random numbers, no time).

Every mesh is triangles only (face_start = 0, 3, 6, ...), with per-corner normals and UVs, the
layout `scenes/lib/ar.py` submits. Triangle winding is counter-clockwise seen from outside, so
geometric normals point out.
"""
from dataclasses import dataclass

import numpy as np


@dataclass
class Mesh:
    positions: np.ndarray    # (V, 3) float32
    corner_vert: np.ndarray  # (C,) int32
    face_start: np.ndarray   # (F,) int32
    normals: np.ndarray      # (C, 3) float32, unit
    uv: np.ndarray           # (C, 2) float32

    @property
    def triangle_count(self):
        return int(self.face_start.size)


def _mesh(positions, tris, corner_normals, corner_uv):
    tris = np.asarray(tris, dtype=np.int32).reshape(-1, 3)
    n = np.asarray(corner_normals, dtype=np.float64).reshape(-1, 3)
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    return Mesh(np.asarray(positions, dtype=np.float32).reshape(-1, 3), tris.reshape(-1),
                (np.arange(len(tris), dtype=np.int32) * 3), n.astype(np.float32),
                np.asarray(corner_uv, dtype=np.float32).reshape(-1, 2))


def plane(size_x, size_y):
    """A rectangle in the xy plane centred on the origin, facing +z: two triangles."""
    hx, hy = size_x / 2, size_y / 2
    p = [[-hx, -hy, 0], [hx, -hy, 0], [hx, hy, 0], [-hx, hy, 0]]
    tris = [[0, 1, 2], [0, 2, 3]]
    uv_v = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], dtype=np.float32)
    idx = np.array(tris).reshape(-1)
    return _mesh(p, tris, np.tile([0, 0, 1], (6, 1)), uv_v[idx])


def box(sx, sy, sz):
    """An axis-aligned box centred on the origin: 12 triangles, flat normals per face."""
    h = np.array([sx, sy, sz]) / 2
    corners = np.array([[x, y, z] for z in (-1, 1) for y in (-1, 1) for x in (-1, 1)], dtype=np.float64) * h
    # (outward normal, four corner indices counter-clockwise seen from outside)
    faces = [((1, 0, 0), (1, 3, 7, 5)), ((-1, 0, 0), (0, 4, 6, 2)), ((0, 1, 0), (2, 6, 7, 3)),
             ((0, -1, 0), (0, 1, 5, 4)), ((0, 0, 1), (4, 5, 7, 6)), ((0, 0, -1), (0, 2, 3, 1))]
    tris, normals, uvs = [], [], []
    quad_uv = [[0, 0], [1, 0], [1, 1], [0, 1]]
    for nrm, q in faces:
        for a, b, c in ((0, 1, 2), (0, 2, 3)):
            tris.append([q[a], q[b], q[c]])
            normals.extend([nrm] * 3)
            uvs.extend([quad_uv[a], quad_uv[b], quad_uv[c]])
    return _mesh(corners, tris, normals, uvs)


def uv_sphere(radius, segments=64, rings=32):
    """A UV sphere with smooth normals (the analytic ones, so tessellation does not tilt them).
    A pole at each end, `rings - 1` rings of `segments` vertices between; 2 * segments * (rings - 1)
    triangles. UVs wrap once around: u = longitude / 2 pi, v = 1 - latitude from the south pole."""
    if segments < 3 or rings < 2:
        raise ValueError("a sphere needs at least 3 segments and 2 rings")
    ang = np.arange(segments) * 2 * np.pi / segments
    ring_theta = np.arange(1, rings) * np.pi / rings
    verts = [[0.0, 0.0, radius]]
    for t in ring_theta:
        for a in ang:
            verts.append([radius * np.sin(t) * np.cos(a), radius * np.sin(t) * np.sin(a), radius * np.cos(t)])
    verts.append([0.0, 0.0, -radius])
    south = len(verts) - 1

    def vid(ring, j):  # ring 1..rings-1, column j (wraps)
        return 1 + (ring - 1) * segments + (j % segments)

    tris, uvs = [], []

    def add(ids, cols, rings_of):
        tris.append(ids)
        uvs.extend([[c / segments, 1.0 - r / rings] for c, r in zip(cols, rings_of)])

    for j in range(segments):  # top cap; the pole corner takes the column centre
        tris.append([0, vid(1, j), vid(1, j + 1)])
        uvs.extend([[(j + 0.5) / segments, 1.0], [j / segments, 1.0 - 1 / rings], [(j + 1) / segments, 1.0 - 1 / rings]])
    for r in range(1, rings - 1):
        for j in range(segments):
            a, b, c, d = vid(r, j), vid(r + 1, j), vid(r + 1, j + 1), vid(r, j + 1)
            add([a, b, c], [j, j, j + 1], [r, r + 1, r + 1])
            add([a, c, d], [j, j + 1, j + 1], [r, r + 1, r])
    for j in range(segments):  # bottom cap
        tris.append([vid(rings - 1, j), south, vid(rings - 1, j + 1)])
        uvs.extend([[j / segments, 1.0 - (rings - 1) / rings], [(j + 0.5) / segments, 0.0],
                    [(j + 1) / segments, 1.0 - (rings - 1) / rings]])
    pos = np.asarray(verts)
    tri = np.asarray(tris, dtype=np.int32)
    normals = pos[tri.reshape(-1)] / radius
    return _mesh(pos, tri, normals, uvs)


def displaced_grid(size, cells, amplitude, waves=3):
    """A square grid in the xy plane centred on the origin, displaced along z by
    amplitude * sin(waves * 2 pi x / size) * sin(waves * 2 pi y / size); normals from the analytic
    gradient. 2 * cells^2 triangles."""
    n = cells + 1
    xs = np.linspace(-size / 2, size / 2, n)
    X, Y = np.meshgrid(xs, xs, indexing="ij")
    k = waves * 2 * np.pi / size
    Z = amplitude * np.sin(k * X) * np.sin(k * Y)
    dzdx = amplitude * k * np.cos(k * X) * np.sin(k * Y)
    dzdy = amplitude * k * np.sin(k * X) * np.cos(k * Y)
    pos = np.stack([X, Y, Z], axis=-1).reshape(-1, 3)
    nrm = np.stack([-dzdx, -dzdy, np.ones_like(Z)], axis=-1).reshape(-1, 3)
    uv_v = np.stack([(X - xs[0]) / size, (Y - xs[0]) / size], axis=-1).reshape(-1, 2)
    i, j = np.meshgrid(np.arange(cells), np.arange(cells), indexing="ij")
    a, b, c, d = (i * n + j).ravel(), ((i + 1) * n + j).ravel(), ((i + 1) * n + j + 1).ravel(), (i * n + j + 1).ravel()
    tris = np.stack([np.stack([a, b, c], 1), np.stack([a, c, d], 1)], axis=1).reshape(-1, 3)
    flat = tris.reshape(-1)
    return _mesh(pos, tris, nrm[flat], uv_v[flat])
