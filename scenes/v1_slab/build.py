# SPDX-License-Identifier: Apache-2.0
"""V1 slab builder: a closed box of thickness d with a chromatic absorbing test medium inside, between
a pinhole camera and a view-filling white emitter plane, world black. The box covers the left half of
the view (world x < 0, its side face in the plane x = 0 through the camera, so the split falls between
the two middle pixel columns); the right half sees the emitter directly. The box's material has opacity 0
(alpha pass-through), so a camera ray enters the box and the medium alone attenuates it. Usage:
build.py --out <path.arrec> [--sigma-zero | --scattering]. Parameters come from meta.toml; --sigma-zero builds
the variant whose medium has no absorption and no scattering (the render must equal the scene without a
medium); --scattering the chromatic scattering variant of the hero-lane variance measurement (no truth):
extinction sigma_t = meta.toml's sigma_a per channel, albedo SCATTER_ALBEDO, Henyey-Greenstein g
SCATTER_G, up to SCATTER_BOUNCES volume scatters.
Ref: docs/decisions/0006-volumes-and-astrophysics.md V1, D20; 0004 D3 (opacity pass-through); 0003 Risk 11
(the scattering variant)."""
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
# The scattering variant: albedo and g of 183's scattering slab (tests/common/slab_rig.h), on the chromatic
# extinction, so the hero lane's distance and event choices differ between lanes; 128 scatters cut
# 0.8^128 ~ 4e-13 of the weight.
SCATTER_ALBEDO, SCATTER_G, SCATTER_BOUNCES = 0.8, 0.5, 128


def build(out, sigma_zero=False, scattering=False):
    if sigma_zero and scattering:
        raise ValueError("--sigma-zero and --scattering are different variants")
    t = meta_mod.load(os.path.join(HERE, "meta.toml"))["truth"]
    tan_half = (t["sensor_mm"] / t["focal_mm"]) / 2 * math.sqrt(2)
    top = t["gap"] + t["thickness"]
    if t["plane_size"] / 2 < t["camera_distance"] * tan_half:
        raise ValueError("the emitter does not fill the camera's view")
    # The lowest point of the box's span must still cover the left half's widest ray.
    if t["slab_size"] / 2 < (t["camera_distance"] - t["gap"]) * tan_half:
        raise ValueError("the slab does not cover the left half of the camera's view")
    if SIZE % 2:
        raise ValueError("the image needs an even width for its halves")
    if top >= t["camera_distance"]:
        raise ValueError("the slab reaches the camera")
    with ar.Scene(out) as s:
        emitter, slab = s.key(), s.key()
        s.mesh_from(emitter, meshes.plane(t["plane_size"], t["plane_size"]), "emitter")
        s.mesh_from(slab, meshes.box(t["slab_size"], t["slab_size"], t["thickness"]), "slab")
        glow = s.material({"base_color": (0.0, 0.0, 0.0), "specular_weight": 0.0}, emission=(t["colour"], t["radiance"]))
        # Opacity 0: the surface is invisible and only encloses the medium.
        shell = s.material({"base_color": (0.0, 0.0, 0.0), "specular_weight": 0.0, "geometry_opacity": 0.0})
        zero = (0.0, 0.0, 0.0)
        if scattering:
            sigma_t = t["sigma_a"]
            medium = s.medium([(1.0 - SCATTER_ALBEDO) * x for x in sigma_t], [SCATTER_ALBEDO * x for x in sigma_t],
                              SCATTER_G)
        else:
            medium = s.medium(zero if sigma_zero else t["sigma_a"], zero if sigma_zero else t["sigma_s"], t["g"])
        s.instances([{"geom": emitter, "materials": [glow]},
                     {"geom": slab, "materials": [shell], "medium": medium,
                      "xf": ar.translation((-t["slab_size"] / 2, 0.0, t["gap"] + t["thickness"] / 2))}])
        s.world((0.0, 0.0, 0.0), 0.0)
        if scattering:
            s.settings(SPP, SIZE, SIZE, seed=SEED, sampler="independent", max_bounces=SCATTER_BOUNCES,
                       volume_bounces=SCATTER_BOUNCES)
        else:
            s.settings(SPP, SIZE, SIZE, seed=SEED, sampler="independent")
        s.camera((0.0, 0.0, t["camera_distance"]), (0.0, 0.0, 0.0), t["focal_mm"], t["sensor_mm"],
                 SIZE, SIZE, up=(0.0, 1.0, 0.0))
        s.finish()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    ap.add_argument("--sigma-zero", action="store_true", help="a medium with sigma_a = sigma_s = 0")
    ap.add_argument("--scattering", action="store_true", help="the chromatic scattering variant (no truth)")
    a = ap.parse_args()
    build(a.out, a.sigma_zero, a.scattering)
