# SPDX-License-Identifier: GPL-3.0-or-later
"""Render settings to ar_settings (0007 d37, 0003 d15).

Ref: docs/decisions/0007-appendix-abi.md (ar_settings); 0003 decision 15 (the working space is read
from `bpy.data.colorspace.working_space_interop_id`; any other value refuses the render);
0007 decision 37 (native properties such as `render.filter_size` are read directly).
`build` returns plain values so it runs without the engine DLL; only `set_settings` needs the
generated binding.
"""
import ctypes

# include/astroray/astroray.h AR_COLOR_INTEROP_* and AR_FILTER_*
WORKING_SPACES = {"lin_rec709_scene": 1, "lin_rec2020_scene": 2, "lin_ap1_scene": 3}
FILTERS = {"BOX": 0, "GAUSSIAN": 1, "BLACKMAN_HARRIS": 2}


class UnsupportedWorkingSpace(RuntimeError):
    pass


def working_space(interop_id):
    ws = WORKING_SPACES.get(interop_id)
    if ws is None:
        raise UnsupportedWorkingSpace(
            f"astroray sync: working space '{interop_id}' is not supported; the engine renders in "
            f"{', '.join(sorted(WORKING_SPACES))} (set Color Management > Working Space)")
    return ws


def build(scene, colorspace, passes=()):
    """Returns (fields, lines): the ar_settings values and the visible report lines for what
    Blender asks for and the engine does not do.

    `colorspace` is `bpy.data.colorspace`, passed in so tests can supply another id.
    """
    a, r = scene.astroray, scene.render
    lines = []
    if r.use_border:
        lines.append("render border is not supported yet and is ignored")
    if r.film_transparent:
        lines.append("Film > Transparent is not supported yet and is ignored")
    if r.use_motion_blur:
        lines.append("motion blur lands in M8 and is ignored")
    fields = {
        "output_width": r.resolution_x, "output_height": r.resolution_y,
        "resolution_percent": r.resolution_percentage,
        "samples": a.samples, "viewport_samples": a.viewport_samples,
        "seed": a.seed, "animated_seed": int(a.use_animated_seed),
        "max_bounces": a.max_bounces, "min_bounces": a.min_bounces,
        # Only the total limit exists in M1a; a per-type limit of 0 would end such paths at once,
        # so each equals the total and never binds before it.
        "diffuse_bounces": a.max_bounces, "glossy_bounces": a.max_bounces,
        "transmission_bounces": a.max_bounces, "volume_bounces": a.max_bounces,
        "transparent_bounces": a.max_bounces,
        "pixel_filter": FILTERS[a.pixel_filter_type], "pixel_filter_width": r.filter_size,
        "film_exposure": a.film_exposure,
        "working_space": working_space(colorspace.working_space_interop_id),
        "passes": tuple(passes),
    }
    return fields, lines


def set_settings(edit, abi, fields):
    """One ar_edit_set_settings call; the arrays stay referenced until it returns."""
    s = abi.ar_settings()
    names = (abi.ar_name * max(1, len(fields["passes"])))()
    for i, p in enumerate(fields["passes"]):
        raw = p.encode("utf-8")
        if len(raw) >= 64:
            raise ValueError(f"astroray sync: pass name '{p}' is longer than 63 bytes")
        names[i].text[:len(raw) + 1] = list(raw) + [0]
    s.passes = ctypes.cast(names, ctypes.POINTER(abi.ar_name))
    s.pass_count = len(fields["passes"])
    for k, v in fields.items():
        if k != "passes":
            setattr(s, k, v)
    edit.call("ar_edit_set_settings", ctypes.byref(s))
