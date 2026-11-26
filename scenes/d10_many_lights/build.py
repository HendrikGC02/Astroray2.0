# SPDX-License-Identifier: Apache-2.0
"""d10 many-lights builder: 64 down-facing rectangular lamps of mixed power, size and height over a
grey Lambertian floor with occluding blocks, a black world, a pinhole camera straight above looking
down (image +x is world +x, image up is world +y). The light sampler is chosen at render time
(ASTRORAY_TEST_LIGHT_SAMPLER). The definition is in meta.toml. Usage: build.py --out <path.arrec>.
Ref: docs/decisions/0004-integrator.md D10 (Verification row "D10 tree"); 0009 d17(v), d19."""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

# A half turn about x: the lamp's emitting side (local -z, scenes/b23_lights) faces the floor.
FLIP_X = (1, 0, 0, 0, -1, 0, 0, 0, -1)


def truth():
    return meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]


def _uniforms(seed):
    """splitmix64 (Steele, Lea and Flood 2014), written out so the layout does not depend on a
    library's generator stream; yields doubles in [0, 1)."""
    s = seed & 0xFFFFFFFFFFFFFFFF
    while True:
        s = (s + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        z ^= z >> 31
        yield (z >> 11) * (1.0 / 9007199254740992.0)


def lamps(t=None):
    """[(x, y, z, side_x, side_y, power)] for the 64 lamps, row by row from -y."""
    t = t or truth()
    u = _uniforms(t["layout_seed"])
    out = []
    for j in range(8):
        for i in range(8):
            x = (i - 3.5) * t["pitch"] + t["jitter"] * (2 * next(u) - 1)
            y = (j - 3.5) * t["pitch"] + t["jitter"] * (2 * next(u) - 1)
            z = t["height_min"] + (t["height_max"] - t["height_min"]) * next(u)
            sx = t["side_min"] + (t["side_max"] - t["side_min"]) * next(u)
            sy = t["side_min"] + (t["side_max"] - t["side_min"]) * next(u)
            p = t["power_min"] * (t["power_max"] / t["power_min"]) ** next(u)
            out.append((x, y, z, sx, sy, p))
    return out


def lamp_boxes_px(width, height, t=None):
    """Each lamp's rectangle as the camera sees it: (x0, y0, x1, y1) in pixels, rows counted from the
    bottom (the film's order). Pinhole straight down from camera_height (ar.projection_inverse's
    model: the sensor spans the image width at the focal length)."""
    t = t or truth()
    k = t["focal_mm"] / (0.5 * t["sensor_mm"])
    out = []
    for x, y, z, sx, sy, _ in lamps(t):
        d = t["camera_height"] - z

        def px(v, n):
            return (v / d * k + 1.0) * 0.5 * n
        out.append((px(x - sx / 2, width), px(y - sy / 2, height), px(x + sx / 2, width), px(y + sy / 2, height)))
    return out


def build(out):
    t = truth()
    seen = t["camera_height"] * t["sensor_mm"] / t["focal_mm"] / 2
    if t["floor_size"] / 2 < seen:
        raise ValueError("the floor does not fill the camera's view")
    rho = t["rho"]
    size = t["size"]
    with ar.Scene(out) as s:
        grey = s.material({"base_weight": 1.0, "base_color": (rho, rho, rho), "base_diffuse_roughness": 0.0,
                           "base_metalness": 0.0, "specular_weight": 0.0})
        floor = s.key()
        s.mesh_from(floor, meshes.plane(t["floor_size"], t["floor_size"]), "floor")
        items = [{"geom": floor, "materials": [grey]}]
        for n, (bx, by, side, h) in enumerate(t["blocks"]):
            key = s.key()
            s.mesh_from(key, meshes.box(side, side, h), f"block{n}")
            items.append({"geom": key, "materials": [grey], "xf": [[1, 0, 0, bx], [0, 1, 0, by], [0, 0, 1, h / 2]]})
        s.instances(items)
        for x, y, z, sx, sy, p in lamps(t):
            f = FLIP_X
            s.light("rect", xf=[f[0], f[1], f[2], x, f[3], f[4], f[5], y, f[6], f[7], f[8], z], power=p, size=(sx, sy))
        s.world((0.0, 0.0, 0.0), 1.0)
        s.settings(t["spp"], size, size, seed=1, max_bounces=t["max_bounces"], sampler="independent")
        s.camera((0.0, 0.0, t["camera_height"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], size, size,
                 up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    build(ap.parse_args().out)
