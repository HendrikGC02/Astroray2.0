# SPDX-License-Identifier: Apache-2.0
"""d3 alpha cards builder: ten stacked black cards of opacity 0.5 (one mesh, ten instances, 0.1 m
apart) between a pinhole camera and a view-filling emitter plane of radiance L (the A5
construction), world black. Each card passes a camera ray with probability 1 - a and stops it
otherwise (black, no specular), so a pixel's mean is L (1 - a)^n. Usage: build.py --out
<path.arrec> [--cards N]; --cards builds the variant with N cards for the look-at render.
Ref: docs/decisions/0004-integrator.md D3, D14 and the D3/D14 Verification row; 0002 D11;
0009 d19, d20."""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402
import meta as meta_mod  # noqa: E402

SIZE = 32
SPP, SEED = 64, 1


def build(out, cards=None):
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    n = t["cards"] if cards is None else cards
    tan_half = (t["sensor_mm"] / t["focal_mm"]) / 2 * math.sqrt(2)
    # The farthest card from the camera is the lowest; it and the emitter must fill the view.
    if t["plane_size"] / 2 < t["camera_distance"] * tan_half:
        raise ValueError("the emitter does not fill the camera's view")
    if t["card_size"] / 2 < (t["camera_distance"] - t["spacing"]) * tan_half:
        raise ValueError("the lowest card does not fill the camera's view")
    if n * t["spacing"] >= t["camera_distance"]:
        raise ValueError("the cards reach the camera")
    if n > t["transparent_bounces"]:
        raise ValueError("more cards than the transparent-bounce limit passes")
    with ar.Scene(out) as s:
        emitter, card = s.key(), s.key()
        s.mesh_from(emitter, meshes.plane(t["plane_size"], t["plane_size"]), "emitter")
        s.mesh_from(card, meshes.plane(t["card_size"], t["card_size"]), "card")
        # Black base and no specular: the emitter only emits, a card only stops or passes a ray.
        glow = s.material({"base_color": (0.0, 0.0, 0.0), "specular_weight": 0.0}, emission=(t["colour"], t["radiance"]))
        alpha = s.material({"base_color": (0.0, 0.0, 0.0), "specular_weight": 0.0, "geometry_opacity": t["opacity"]})
        items = [{"geom": emitter, "materials": [glow]}]
        items += [{"geom": card, "materials": [alpha], "xf": ar.translation((0.0, 0.0, t["spacing"] * (k + 1)))}
                  for k in range(n)]
        s.instances(items)
        s.world((0.0, 0.0, 0.0), 0.0)
        # Independent mode: the analytic harness needs the exact variance of the image mean (0008 d3).
        s.settings(SPP, SIZE, SIZE, seed=SEED, sampler="independent", transparent_bounces=t["transparent_bounces"])
        s.camera((0.0, 0.0, t["camera_distance"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"],
                 SIZE, SIZE, up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--cards", type=int, help="number of cards (default: meta.toml)")
    a = ap.parse_args()
    build(a.out, a.cards)
