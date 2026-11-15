# SPDX-License-Identifier: GPL-3.0-or-later
"""Point and area lamps to ar_light_desc (0007 d19-d20, 0003 d11).

Ref: Cycles intern/cycles/blender/light.cpp, BlenderSync::sync_light (Apache-2.0,
projects.blender.org/blender/blender, read 2026-11-16): a point lamp's radius is `shadow_soft_size`
(Light.radius); an area lamp's `size` is the full extent u, SQUARE and DISK take v = u, RECTANGLE
and ELLIPSE take `size_y`, DISK and ELLIPSE are ellipses; strength is colour x energy x
exp2(exposure) with the temperature colour multiplied into the colour; `normalize` and
cast_shadow come from the lamp's flags. Watts pass through with factor 1 (0003 d11).
Ref: 0007-appendix-abi.md ar_light_desc (AREA_ELLIPSE sizes are diameters, spread in radians).
Spot and sun lamps, node trees and IES are M4: skipped or ignored with a visible line.
"""
import ctypes
import math

from . import instances, keys

# include/astroray/astroray.h
POINT, SPOT, SUN, AREA_RECT, AREA_ELLIPSE = 0, 1, 2, 3, 4
SPECTRUM_RGB = 0
# Cycles ignores node trees made only of these; anything else is an M4 feature.
_PLAIN_NODES = frozenset(("OUTPUT_LIGHT", "EMISSION"))


def _has_light_graph(light):
    """True when the tree does more than the default Emission -> Light Output pair: Cycles would
    light with the emission node's colour and strength, which the ABI does not carry yet."""
    tree = light.node_tree if light.use_nodes else None
    if tree is None:
        return False
    if any(n.type not in _PLAIN_NODES for n in tree.nodes):
        return True
    if any(l.from_node.type not in _PLAIN_NODES or l.to_node.type not in _PLAIN_NODES for l in tree.links):
        return True
    for n in tree.nodes:
        if n.type == "EMISSION":
            if tuple(n.inputs["Color"].default_value)[:3] != (1.0, 1.0, 1.0) or                     n.inputs["Strength"].default_value != 1.0:
                return True
    return False


def describe(inst, origin, lightgroups):
    """(fields, lines) for one lamp instance; fields is None when the lamp is not rendered."""
    ob = inst.object
    light = ob.data
    name = ob.original.name
    lines = []
    if light.type in ("SPOT", "SUN"):
        return None, [f"lamp '{name}': lamp type {light.type} lands in M4 and is not rendered"]
    if _has_light_graph(light):
        lines.append(f"lamp '{name}': light node tree and IES land in M4 and are ignored")
    color = tuple(light.color)
    if light.use_temperature:
        color = tuple(c * t for c, t in zip(color, light.temperature_color))
    f = {
        "color": color,
        "power": light.energy * math.exp2(light.exposure),
        "normalize": int(light.normalize), "cast_shadow": int(light.use_shadow),
        "spread": math.pi, "size_x": 0.0, "size_y": 0.0,
        "light_group": lightgroups.get(ob.original.lightgroup, 0),
    }
    if light.type == "POINT":
        f["type"], f["size_x"] = POINT, light.shadow_soft_size
        if light.use_soft_falloff and light.shadow_soft_size > 0:
            # Cycles makes a soft-falloff lamp a disc-like emitter, not a sphere; the ABI has one
            # sphere lamp (a POINT with radius > 0).
            lines.append(f"lamp '{name}': Soft Falloff renders as a sphere lamp (approximated)")
    else:  # AREA
        round_shape = light.shape in ("DISK", "ELLIPSE")
        f["type"] = AREA_ELLIPSE if round_shape else AREA_RECT
        f["size_x"] = light.size
        f["size_y"] = light.size_y if light.shape in ("RECTANGLE", "ELLIPSE") else light.size
        f["spread"] = light.spread
    vis = 0
    for attr, bit in instances._VISIBILITY:
        if getattr(ob.original, attr):
            vis |= bit
    f["visibility"] = vis
    m = [v for row in inst.matrix_world[:3] for v in row]
    m[3] -= origin[0]
    m[7] -= origin[1]
    m[11] -= origin[2]
    f["xf"] = tuple(m)
    return f, lines


def build(depsgraph, keymap, origin=(0.0, 0.0, 0.0), lightgroups=None):
    """One pass over depsgraph.object_instances: ([(ar_key, fields)], report lines)."""
    lightgroups = lightgroups or {}
    out, lines = [], []
    for inst in depsgraph.object_instances:
        if inst.object.type != "LIGHT":
            continue
        f, ls = describe(inst, origin, lightgroups)
        lines.extend(ls)
        if f is not None:
            out.append((keymap.key(("light",) + keys.object_identity(inst)), f))
    return out, lines


def set_light(edit, abi, key, f):
    d = abi.ar_light_desc()
    d.color.kind = SPECTRUM_RGB
    d.color.rgb = (ctypes.c_float * 3)(*f["color"])
    d.color.scale = 1.0
    d.xf = abi.ar_xform((ctypes.c_float * 12)(*f["xf"]))
    for k in ("type", "visibility", "size_x", "size_y", "spread", "power", "normalize",
              "cast_shadow", "light_group"):
        setattr(d, k, f[k])
    edit.call("ar_edit_set_light", key, ctypes.byref(d))


def sync(edit, abi, depsgraph, keymap, origin=(0.0, 0.0, 0.0), lightgroups=None):
    """Sends every renderable lamp; returns the report lines for the rest."""
    lamps, lines = build(depsgraph, keymap, origin, lightgroups)
    for key, f in lamps:
        set_light(edit, abi, key, f)
    return lines
