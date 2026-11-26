# SPDX-License-Identifier: Apache-2.0
"""B-19 alpha-foliage builder: N thin cards (two triangles each) instanced from four meshes, random
orientation and position inside the camera's frustum, one constant geometry_opacity and a diffuse
base colour, lit as b1m is (an area lamp of the spike's sun irradiance, constant world). A camera
ray crosses many cards; each passes it with probability 1 - opacity, so a pass needs about one
scheduler step per alpha layer on top of the bounce steps (0002 D7).
Usage: build.py --out <path.arrec> [--blend <path.blend>]. Prints the mean and longest number of
cards a camera ray crosses and the mean number it passes (numpy, seeded, so the same every build).
Ref: docs/decisions/0002-kernel-architecture.md D7 and the foliage row of its verification table;
0004 D3 (geometry opacity as a null event); scenes/b1m/build.py (lamp, world); 0009 d19, d20;
Shoemake, Graphics Gems III (1992), uniform random rotations from unit quaternions."""
import argparse
import importlib.util
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SCENES = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(SCENES, "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402

# One lamp builder for both B-class scenes: b1m's, not a copy.
_spec = importlib.util.spec_from_file_location("b1m_build", os.path.join(SCENES, "b1m", "build.py"))
b1m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b1m)

N_CARDS = 3000
OPACITY = 0.15                 # a card stops a camera ray with this probability and passes it otherwise
CARD_SIZES = ((0.8, 0.5), (1.0, 0.6), (0.6, 0.9), (1.2, 0.7))   # m, four meshes
DEPTH = (3.0, 14.0)            # m in front of the camera
SPP, SEED, LAYOUT_SEED = 16, 12345, 170
FOCAL_MM, SENSOR_MM, WIDTH, HEIGHT = 35.0, 36.0, 1920, 1080
CENTRE = (0.0, 8.5, 0.0)       # the lamp's aim point
TRANSPARENT_MARGIN = 8         # pass-throughs allowed beyond the longest geometric chain of a camera ray
PROBE_RAYS = 8192


def random_rotations(rng, n):
    """Uniformly distributed rotation matrices from unit quaternions (Shoemake)."""
    q = rng.normal(size=(n, 4))
    q /= np.linalg.norm(q, axis=1, keepdims=True)
    w, x, y, z = q.T
    return np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                     2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                     2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)], axis=1).reshape(n, 3, 3)


def layout():
    """(rotations (N,3,3), positions (N,3), mesh index (N,)): cards scattered over the frustum slice
    DEPTH, widened by 10 % so the edge pixels see a full density."""
    rng = np.random.default_rng(LAYOUT_SEED)
    th = (SENSOR_MM / 2) / FOCAL_MM
    tv = th * HEIGHT / WIDTH
    y = rng.uniform(*DEPTH, N_CARDS)
    pos = np.stack([rng.uniform(-1, 1, N_CARDS) * y * th * 1.1, y, rng.uniform(-1, 1, N_CARDS) * y * tv * 1.1], axis=1)
    return random_rotations(rng, N_CARDS), pos, rng.integers(0, len(CARD_SIZES), N_CARDS)


def layer_statistics(rot, pos, mesh_idx, opacity):
    """Cards crossed by camera rays (PROBE_RAYS rays through random film positions): (mean, longest,
    mean passed). A ray reaches its k-th card with probability (1 - a)^(k-1) and passes it with 1 - a,
    so it passes sum_k (1 - a)^k of the L cards it crosses in expectation."""
    rng = np.random.default_rng(LAYOUT_SEED + 1)
    th = (SENSOR_MM / 2) / FOCAL_MM
    d = np.stack([rng.uniform(-1, 1, PROBE_RAYS) * th, np.ones(PROBE_RAYS),
                  rng.uniform(-1, 1, PROBE_RAYS) * th * HEIGHT / WIDTH], axis=1)
    half = np.array(CARD_SIZES)[mesh_idx] / 2
    crossed = np.zeros(PROBE_RAYS, int)
    nrm = rot[:, :, 2]
    for lo in range(0, PROBE_RAYS, 1024):
        dd = d[lo:lo + 1024]
        t = (pos * nrm).sum(1)[None, :] / (dd @ nrm.T)          # the ray t d meets the card's plane
        hit = t[:, :, None] * dd[:, None, :] - pos[None, :, :]   # offset from the card centre
        lx = np.einsum("rnk,nk->rn", hit, rot[:, :, 0])
        ly = np.einsum("rnk,nk->rn", hit, rot[:, :, 1])
        crossed[lo:lo + 1024] = ((t > 0) & (np.abs(lx) < half[None, :, 0]) & (np.abs(ly) < half[None, :, 1])).sum(1)
    passed = (1 - opacity) * (1 - (1 - opacity) ** crossed) / opacity
    return float(crossed.mean()), int(crossed.max()), float(passed.mean())


def build(out, blend=None):
    rot, pos, mesh_idx = layout()
    mean_l, max_l, mean_pass = layer_statistics(rot, pos, mesh_idx, OPACITY)
    if mean_pass < 4.0:
        raise ValueError(f"a camera ray passes {mean_pass:.2f} cards on average; the scene needs at least 4")
    transparent = max_l + TRANSPARENT_MARGIN
    with ar.Scene(out, description=blend + ".json" if blend else None) as s:
        keys = [s.key() for _ in CARD_SIZES]
        for k, (w, h) in zip(keys, CARD_SIZES):
            s.mesh_from(k, meshes.plane(w, h), f"card_{w}x{h}")
        mat = s.material({"base_color": (0.15, 0.45, 0.10), "specular_roughness": 0.6, "geometry_opacity": OPACITY})
        s.instances([{"geom": keys[m], "materials": [mat],
                      "xf": np.concatenate([rot[i], pos[i][:, None]], axis=1).reshape(12)}
                     for i, m in enumerate(mesh_idx)])
        b1m.sun_lamp(s, CENTRE, b1m.LAMP_DISTANCE, b1m.LAMP_SIZE, b1m.SUN_STRENGTH, b1m.SUN_EULER)
        s.world((0.3, 0.35, 0.4), 1.0)
        s.settings(SPP, WIDTH, HEIGHT, seed=SEED, sampler="sobol", transparent_bounces=transparent)
        s.camera((0.0, 0.0, 0.0), (0.0, 1.0, 0.0), FOCAL_MM, SENSOR_MM, WIDTH, HEIGHT)
        s.finish()
    print(f"counts: {2 * N_CARDS} triangles, {N_CARDS} instances, {len(CARD_SIZES)} meshes; opacity {OPACITY}; "
          f"camera ray crosses {mean_l:.2f} cards on average (longest {max_l}), passes {mean_pass:.2f}; "
          f"transparent_bounces {transparent}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--blend", help="the .blend this scene is for; writes <blend>.json beside it")
    a = ap.parse_args()
    build(a.out, a.blend)
