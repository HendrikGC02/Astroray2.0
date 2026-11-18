# SPDX-License-Identifier: Apache-2.0
"""D1 builder: an emission-only plane textured with the UV chart through Closest filtering, filling
the view of a head-on pinhole camera, world black (0009 d20). Usage: build.py --out <path.arrec>, or
build.py --write-chart to (re)write tests/data/images/uv_chart.png from chart(). The build refuses a
committed chart that differs from chart(), so the PNG and this script cannot drift apart. The image
goes into the recording as the file's bytes, decoded by the engine (8-bit sRGB, 0005 item 34).
Ref: docs/decisions/0009-build-test-process.md d19, d20; 0008 d1 (rows bottom-up); PNG, ISO/IEC
15948 (the encoding written here: 8-bit RGB, filter 0, no interlace)."""
import argparse
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SEED = 1


def chart(size, cell, marker_block):
    """The chart as (size, size, 3) uint8 in file order (top row first). Cell (cx, cy), counted from
    the bottom-left with rows bottom-up, has R = 40 + 20 cx, G = 40 + 20 cy (distinct per cell, at most
    180) and B = 30 + 20 ((3 cx + 5 cy) mod 8); the marker cell is white (255), brighter than every
    other cell in every channel."""
    n = size // cell
    if n * cell != size or n > 8:
        raise ValueError("the chart holds at most 8 x 8 whole cells")
    img = np.zeros((size, size, 3), dtype=np.uint8)  # bottom-up while filled
    for cy in range(n):
        for cx in range(n):
            rgb = (255, 255, 255) if [cx, cy] == list(marker_block) else \
                (40 + 20 * cx, 40 + 20 * cy, 30 + 20 * ((3 * cx + 5 * cy) % 8))
            img[cy * cell:(cy + 1) * cell, cx * cell:(cx + 1) * cell] = rgb
    return img[::-1].copy()


def encode_png(img):
    """8-bit RGB PNG bytes, every row with filter type 0, one IDAT."""
    h, w = img.shape[:2]
    raw = b"".join(b"\x00" + img[r].tobytes() for r in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def chart_bytes(t):
    return encode_png(chart(int(t["size"]), int(t["cell"]), t["marker_block"]))


def build(out):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    path = os.path.join(ROOT, t["chart"])
    with open(path, "rb") as f:
        data = f.read()
    if data != chart_bytes(t):
        raise ValueError(f"{path} differs from chart(): run build.py --write-chart and commit it")
    size, spp = int(t["size"]), int(t["spp"])
    side = t["camera_distance"] * t["sensor_mm"] / t["focal_mm"]  # the view's full width at the plane
    with ar.Scene(out) as s:
        mesh = s.key()
        s.mesh_from(mesh, meshes.plane(side, side), "chart")
        image = s.key()
        s.image(image, data=data)
        g = s.graph()
        g.store("base_color", (0.0, 0.0, 0.0))
        g.store("specular_weight", 0.0)
        g.emission(g.tex_image(g.uv(), image, linear=False), float(t["strength"]))
        mat = s.material_from_graph(g)
        s.instances([{"geom": mesh, "materials": [mat]}])
        s.world((0.0, 0.0, 0.0), 0.0)
        # Independent mode: the analytic harness needs the exact variance of each pixel's mean (0008 d3).
        s.settings(spp, size, size, seed=SEED, sampler="independent")
        s.camera((0.0, 0.0, t["camera_distance"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"], size, size,
                 up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", help="the .arrec to write")
    ap.add_argument("--write-chart", action="store_true", help="write the chart PNG named in meta.toml")
    a = ap.parse_args()
    if a.write_chart:
        tt = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
        with open(os.path.join(ROOT, tt["chart"]), "wb") as fh:
            fh.write(chart_bytes(tt))
    elif a.out:
        build(a.out)
    else:
        ap.error("give --out or --write-chart")
