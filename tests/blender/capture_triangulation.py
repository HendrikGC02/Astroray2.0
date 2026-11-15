# SPDX-License-Identifier: Apache-2.0
# Captures Blender's loop_triangles for fixture polygons (task 034, 0007 d10, V2).
# Run: Invoke-ArBlender -Script tests/blender/capture_triangulation.py -Args <out.json>
# Each case is one polygon: corner i uses vertex i, so "tris" are corner indices.
import json
import math
import random
import sys

import bpy


def capture(name, kind, pts):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(p) for p in pts], [], [list(range(len(pts)))])
    mesh.update(calc_edges=True)
    assert len(mesh.polygons) == 1 and len(mesh.loops) == len(pts), name
    mesh.calc_loop_triangles()
    tris = [list(t.loops) for t in mesh.loop_triangles]
    # Positions as stored by Blender (float32), so the C++ test sees the same bits.
    co = [list(map(float, v.co)) for v in mesh.vertices]
    bpy.data.meshes.remove(mesh)
    return {"name": name, "kind": kind, "positions": co, "tris": tris}


def ngon(n, r=1.0, z=0.0, rot=0.0):
    return [(r * math.cos(rot + 2 * math.pi * i / n), r * math.sin(rot + 2 * math.pi * i / n), z) for i in range(n)]


def main():
    cases = []
    cases.append(capture("tri", "tri", [(0, 0, 0), (1, 0, 0), (0, 1, 0)]))
    cases.append(capture("tri_cw", "tri", [(0, 0, 0), (0, 1, 0), (1, 0, 0)]))

    # Planar convex quads.
    convex = {
        "square": [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
        "rect": [(0, 0, 0), (3, 0, 0), (3, 1, 0), (0, 1, 0)],
        "kite": [(0, 0, 0), (2, 0, 0), (3, 3, 0), (0, 1, 0)],
        "trapezoid": [(0, 0, 0), (4, 0, 0), (3, 1, 0), (1, 1, 0)],
        "rhombus": [(0, 0, 0), (2, 1, 0), (4, 0, 0), (2, -1, 0)],
        "skewed": [(0, 0, 0), (4, 0, 0), (5, 1, 0), (1.5, 1, 0)],
    }
    for k, p in convex.items():
        cases.append(capture("quad_convex_" + k, "quad", p))
        cases.append(capture("quad_convex_%s_r1" % k, "quad", p[1:] + p[:1]))
        cases.append(capture("quad_convex_%s_rev" % k, "quad", p[::-1]))

    # Concave quads (planar), reflex vertex at each position.
    base = [(0, 0, 0), (4, 0, 0), (4, 4, 0), (0, 4, 0)]
    # Each reflex corner is pushed past the centre, so the interior angle there exceeds 180.
    reflex = [(2.8, 2.8, 0), (1.2, 2.8, 0), (1.2, 1.2, 0), (2.8, 1.2, 0)]
    for i in range(4):
        p = list(base)
        p[i] = reflex[i]
        cases.append(capture("quad_concave_v%d" % i, "quad", p))
        cases.append(capture("quad_concave_v%d_rev" % i, "quad", p[::-1]))
        # Same shape lifted out of plane: the rule must survive non-planarity.
        lifted = [(x, y, 0.4 * ((j + i) % 2)) for j, (x, y, _) in enumerate(p)]
        cases.append(capture("quad_concave_nonplanar_v%d" % i, "quad", lifted))

    # Non-planar quads: lift one or more corners; fixed seed for the random ones.
    for k, lift in enumerate([(0.3, 0, 0, 0), (0, 0.3, 0, 0), (0, 0, 0.3, 0), (0, 0, 0, 0.3),
                              (0.3, 0, 0.3, 0), (0, 0.3, 0, 0.3), (0.3, -0.3, 0.3, -0.3)]):
        p = [(x, y, z + l) for (x, y, z), l in zip(base, lift)]
        cases.append(capture("quad_nonplanar_%d" % k, "quad", p))
    rng = random.Random(34)
    for k in range(16):
        p = [(rng.uniform(-1, 1) + dx, rng.uniform(-1, 1) + dy, rng.uniform(-0.5, 0.5))
             for dx, dy in ((0, 0), (3, 0), (3, 3), (0, 3))]
        cases.append(capture("quad_random_%d" % k, "quad", p))

    # n-gons, 5 to 12 corners.
    for n in range(5, 13):
        cases.append(capture("ngon_convex_%d" % n, "ngon", ngon(n)))
    # Tilted convex n-gon: area must be measured in the polygon plane, not xy.
    for n in (6, 9):
        c, s = math.cos(0.9), math.sin(0.9)
        p = [(x, y * c, y * s + 2.0) for x, y, _ in ngon(n)]
        cases.append(capture("ngon_convex_tilted_%d" % n, "ngon", p))
    # Concave: stars with 5 and 8 points, an L, a comb.
    for pts in (5, 8):
        p = []
        for i in range(2 * pts):
            r = 1.0 if i % 2 == 0 else 0.45
            a = math.pi * i / pts
            p.append((r * math.cos(a), r * math.sin(a), 0.0))
        cases.append(capture("ngon_star_%d" % pts, "ngon", p))
    L = [(0, 0, 0), (3, 0, 0), (3, 1, 0), (1, 1, 0), (1, 3, 0), (0, 3, 0)]
    cases.append(capture("ngon_L", "ngon", L))
    cases.append(capture("ngon_L_cw", "ngon", L[::-1]))
    cases.append(capture("ngon_comb", "ngon", [(0, 0, 0), (7, 0, 0), (7, 3, 0), (6, 3, 0), (6, 1, 0), (5, 1, 0), (5, 3, 0),
                                                 (4, 3, 0), (4, 1, 0), (3, 1, 0), (3, 3, 0), (0, 3, 0)]))
    # Collinear vertices on edges of a rectangle and of a pentagon.
    cases.append(capture("ngon_collinear_rect", "ngon", [(0, 0, 0), (1, 0, 0), (2, 0, 0), (2, 1, 0), (2, 2, 0), (1, 2, 0), (0, 2, 0), (0, 1, 0)]))
    cases.append(capture("ngon_collinear_5", "ngon", [(0, 0, 0), (2, 0, 0), (4, 0, 0), (4, 3, 0), (0, 3, 0)]))

    with open(sys.argv[sys.argv.index("--") + 1], "w", newline="\n") as f:
        json.dump({"blender": bpy.app.version_string, "cases": cases}, f, sort_keys=True, separators=(",", ":"))
        f.write("\n")


main()
