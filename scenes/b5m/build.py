# SPDX-License-Identifier: Apache-2.0
"""B-5M builder: spike B's S2, 2,000 unique meshes of 2,520 triangles (5.04 M unique triangles),
300 materials with distinct constant base colours (roughness 0.5, make_scenes.py line 46), and 10,000 mesh instances: 2,000 objects with
their own mesh, 5,000 linked duplicates, 2,000 collection instances and 1,000 geometry-nodes
instances (0009 d20). The random draws are the spike's, in its order, so meshes, colours and
transforms are the same. Usage: build.py --out <path.arrec> [--blend <path.blend>]; --blend also
writes <blend>.json for scenes/lib/blend_writer.py, which makes the three instance kinds in Blender.
The engine receives every instance flat; the kinds matter to the addon's sync only. The spike's sun
(3 W/m2) is an area lamp of equal irradiance at the scene centre (see b1m/build.py, meta.toml).
Ref: git show spikes-final:spikes/B-blender-end-to-end/make_scenes.py build_s2; docs/decisions/
0009-build-test-process.md d19, d20; 0004 D7."""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402

SEED, N_UNIQUE, N_MATERIALS, COLS = 12345, 2000, 300, 50
N_LINKED, N_COLLECTION, N_COLLECTION_SOURCES = 5000, 2000, 40
GN_GRID, GN_SIZE, GN_TEMPLATE = (40, 25), (90.0, 60.0), 7   # Instance on Points over a 40 x 25 grid
SPP = 16
SUN_EULER, SUN_STRENGTH = (0.6, 0.2, 0.4), 3.0
LAMP_DISTANCE, LAMP_SIZE = 1000.0, 50.0  # m; the scene is 150 m wide, so the lamp is far and 2.9 degrees


def euler_xyz(a, b, c):
    """Blender's XYZ Euler rotation as a matrix: Rz(c) Ry(b) Rx(a)."""
    ca, sa, cb, sb, cc, sc = math.cos(a), math.sin(a), math.cos(b), math.sin(b), math.cos(c), math.sin(c)
    rx = np.array([[1, 0, 0], [0, ca, -sa], [0, sa, ca]])
    ry = np.array([[cb, 0, sb], [0, 1, 0], [-sb, 0, cb]])
    rz = np.array([[cc, -sc, 0], [sc, cc, 0], [0, 0, 1]])
    return rz @ ry @ rx


def sun_lamp(s, centre, distance, size, strength, euler):
    """An area lamp whose irradiance at `centre` equals a sun of `strength` (E = P / (pi d^2))."""
    r = euler_xyz(*euler)
    pos = np.asarray(centre, float) - distance * (r @ np.array([0.0, 0.0, -1.0]))
    s.light("rect", xf=np.concatenate([r, pos[:, None]], axis=1).reshape(12), power=math.pi * distance ** 2 * strength,
            size=(size, size))


# Blender's create_uvsphere spins one meridian and merges the seam, which leaves vertices in an order the
# 5 degree grid does not give: vertex 0 is the top pole, 1-24 and 25 (the bottom pole) the first
# meridian's merged part at azimuth 90 degrees, then 35 meridians of 35 rings (azimuth 80, 70, ... 0, 350,
# ... 100 degrees, ring 1 to 35 from the top), then the 11 remaining azimuth-90 vertices. The spike draws
# noise per vertex index, so the order matters; these are the ring numbers (angle from +z in 5 degree
# steps) of the first and last parts, read from bmesh.ops.create_uvsphere(u_segments=36, v_segments=36,
# radius=1) in Blender 5.3 and verified there for every index (position difference in the task report).
_SEAM_HEAD = (0, 2, 3, 5, 6, 10, 11, 12, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 28, 30, 31, 34, 35, 36)
_SEAM_TAIL = (1, 4, 7, 8, 9, 13, 18, 27, 29, 32, 33)


def sphere_template(segments=36, rings=36):
    """Blender's create_uvsphere(36, 36, radius 1) vertex order (1,262 vertices; poles on +-z) and
    faces (1,296: quads and pole triangles, 2,520 triangles), outward counter-clockwise.
    Returns positions and the faces as index tuples (the face order is ours; the set matches)."""
    if (segments, rings) != (36, 36):
        raise ValueError("the vertex order table is for a 36 x 36 sphere")
    order = [(t, 9) for t in _SEAM_HEAD]
    for m in list(range(8, -1, -1)) + list(range(35, 9, -1)):
        order += [(t, m) for t in range(1, rings)]
    order += [(t, 9) for t in _SEAM_TAIL]
    theta = np.pi * np.array([t for t, _ in order]) / rings
    phi = 2 * np.pi * np.array([m for _, m in order]) / segments
    pos = np.stack([np.sin(theta) * np.cos(phi), np.sin(theta) * np.sin(phi), np.cos(theta)], axis=-1).astype(np.float32)
    index = {}
    for i, (t, m) in enumerate(order):
        index[(t, m)] = i
        if t in (0, rings):
            index.update({(t, k): i for k in range(segments)})
    at = lambda t, m: index[(t, m % segments)]  # noqa: E731
    faces = [(at(0, k), at(1, k), at(1, k + 1)) for k in range(segments)]
    for t in range(1, rings - 1):
        faces += [(at(t, k), at(t + 1, k), at(t + 1, k + 1), at(t, k + 1)) for k in range(segments)]
    faces += [(at(rings, k), at(rings - 1, k + 1), at(rings - 1, k)) for k in range(segments)]
    return pos, faces


def flat_normals(pos, cv, fs):
    """Per-corner face normals: Blender shades a from_pydata mesh flat, as the spike's meshes are."""
    ends = np.append(fs, len(cv))
    face_of = np.repeat(np.arange(len(fs)), np.diff(ends))
    p = pos[cv]
    a, b, c = p[fs], p[fs + 1], p[fs + 2]
    n = np.cross(b - a, c - a)
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-20)
    return n[face_of].astype(np.float32)


def instance_plan(rng):
    """The instance list in the spike's draw order: dicts with mesh index, translation or full
    transform, kind and group. Draw order: materials, 2,000 unique meshes (scale, noise), linked
    duplicates, collection instances; the geometry-nodes instances use no draws."""
    items = []
    for i in range(N_UNIQUE):
        items.append({"mesh": i, "xf": ar.translation(((i % COLS) * 3.0, (i // COLS) * 3.0, 0.0)), "kind": "object"})
    for _ in range(N_LINKED):
        src = int(rng.integers(0, N_UNIQUE))
        loc = (float(rng.uniform(0, COLS * 3.0)), float(rng.uniform(0, 40 * 3.0)), float(rng.uniform(2.5, 8.0)))
        rot = euler_xyz(*rng.uniform(0, 6.28, 3).tolist())
        items.append({"mesh": src, "xf": np.concatenate([rot, np.array(loc)[:, None]], axis=1).reshape(12),
                      "kind": "object"})
    for j in range(N_COLLECTION):
        loc = (float(rng.uniform(0, COLS * 3.0)), float(rng.uniform(0, 40 * 3.0)), float(rng.uniform(9.0, 14.0)))
        k = j % N_COLLECTION_SOURCES
        items.append({"mesh": (k * 37) % N_UNIQUE, "xf": ar.translation(loc), "kind": "collection", "group": k})
    # make_scenes.py lines 148-172: GNTEMPLATE (meshes[7]) at the origin, a GeometryNodeMeshGrid of
    # 90 x 60 (Size X, Size Y) with 40 x 25 vertices (lines 157-160) feeding Instance on Points, Object
    # Info with transform_space RELATIVE (line 164), the host GNHOST at (45, 60, 16) (line 172). In
    # Relative space the template's transform is taken relative to the host, so the host's offset
    # cancels and an instance sits at its grid point, x in [-45, 45], y in [-30, 30], z = 0: the grid
    # overlaps the corner of the unique-object grid at x, y in [0, 147]. That is the spike's own layout
    # (checked against the evaluated depsgraph in Blender: transforms agree to 6e-5).
    nx, ny = GN_GRID
    for y in np.linspace(-GN_SIZE[1] / 2, GN_SIZE[1] / 2, ny):
        for x in np.linspace(-GN_SIZE[0] / 2, GN_SIZE[0] / 2, nx):
            items.append({"mesh": GN_TEMPLATE, "xf": ar.translation((float(x), float(y), 0.0)), "kind": "points",
                          "group": 0})
    return items


def build(out, blend=None):
    rng = np.random.default_rng(SEED)
    pos0, faces = sphere_template()
    colours = [rng.random(3).tolist() for _ in range(N_MATERIALS)]
    cv = np.array([v for f in faces for v in f], np.int32)
    fs = np.cumsum([0] + [len(f) for f in faces[:-1]]).astype(np.int32)
    tri_count = sum(len(f) - 2 for f in faces)
    outward = flat_normals(pos0, cv, fs) * pos0[cv]  # every corner normal points away from the centre
    if tri_count != 2520 or len(pos0) != 1262 or (outward.sum(axis=1) <= 0).any():
        raise ValueError("the sphere template is not 1,262 vertices / 2,520 triangles with outward winding")
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        mats = [s.material({"base_color": c, "specular_roughness": 0.5}) for c in colours]
        meshes, nrm_cache = [], None
        for i in range(N_UNIQUE):
            scale = rng.uniform(0.6, 1.0, 3).astype(np.float32)
            noise = (rng.random(pos0.shape).astype(np.float32) - 0.5) * 0.03
            pos = pos0 * scale + noise
            key = s.key()
            uv = (pos[cv, :2] * 0.1 + 0.5).astype(np.float32)  # the spike's planar add_uv
            s.mesh(key, pos, cv, fs, flat_normals(pos, cv, fs), uv, f"U{i:04d}")
            meshes.append(key)
        items = instance_plan(rng)
        for it in items:
            it["geom"] = meshes[it["mesh"]]
            it["materials"] = [mats[it.pop("mesh") % N_MATERIALS]]  # a mesh keeps its slot material
        s.instances(items)
        sun_lamp(s, (75.0, 60.0, 4.0), LAMP_DISTANCE, LAMP_SIZE, SUN_STRENGTH, SUN_EULER)
        s.world((0.3, 0.35, 0.4), 1.0)
        s.settings(SPP, 1920, 1080, seed=SEED, sampler="sobol")
        s.camera((75.0, -90.0, 70.0), (75.0, 60.0, 4.0), 30.0, 36.0, 1920, 1080)
        s.finish()
    print(f"counts: {N_UNIQUE * tri_count} unique triangles in {N_UNIQUE} meshes, {len(items)} instances, "
          f"{len(mats)} materials")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
