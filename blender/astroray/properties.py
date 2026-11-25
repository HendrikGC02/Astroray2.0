# SPDX-License-Identifier: GPL-3.0-or-later
"""`scene.astroray`: only the settings the M1a sync uses (0007 d37).

Ref: docs/decisions/0007-blender-integration.md decision 37 (native properties are read directly,
never mirrored; `render.filter_size` is EEVEE's filter, so the Astroray filter has its own width);
Cycles intern/cycles/blender/addon/properties.py (Apache-2.0, read 2026-11-16) for the names and
defaults that mean the same here: samples 4096, preview_samples 1024, seed 0, max_bounces 12,
transparent_max_bounces 8, pixel_filter_type BLACKMAN_HARRIS, filter_width 1.5, film_exposure 1.0.
`device` (CPU or CUDA, default CPU) is the analogue of Cycles' scene.cycles.device and selects the F12 device.
"""
import struct

import bpy

FILTER_ITEMS = (
    ("BOX", "Box", "Box filter"),
    ("GAUSSIAN", "Gaussian", "Gaussian filter"),
    ("BLACKMAN_HARRIS", "Blackman-Harris", "Blackman-Harris filter"),
)

# 0007 d40: the modes M1 draws; the rest of the list arrives with M8. Values are the AR_DISPLAY_* ids.
DISPLAY_MODE_ITEMS = (
    ("BEAUTY", "Beauty", "The rendered image", 0),
    ("SAMPLE_COUNT", "Sample count", "Samples per pixel, viridis from 0 to the viewport sample limit", 1),
    ("BOUNCE_COUNT", "Bounce count", "Bounces per path, viridis from 0 to Max Bounces", 2),
)


_DEVICE_ITEMS = {
    "CPU": ("CPU", "CPU", "Render on the CPU", 0),
    "CUDA": ("CUDA", "CUDA", "Render on the NVIDIA GPU", 1),
}
_offered = []  # Blender keeps no copy of the strings an items callback returns


def device_items(self, context):
    """Only what native.devices() offers: after a sticky CUDA error just the CPU (0007 d44). The numbers
    are fixed, so a scene saved with CUDA selected no longer matches an item; device_of refuses it loudly."""
    if __package__:
        from . import native
        offered = native.devices()
    else:  # loaded as a bare file by a headless test with no native module beside it: both devices
        offered = list(_DEVICE_ITEMS)
    _offered[:] = [_DEVICE_ITEMS[d] for d in offered]
    return _offered


def _tag_scene(self, context):
    """A property without an update callback only redraws: no depsgraph update reaches a viewport's
    view_update, so an edit would never be classified (0007 d20, sync/classify.settings_signature)."""
    self.id_data.update_tag()


class AstrorayScene(bpy.types.PropertyGroup):
    device: bpy.props.EnumProperty(
        update=_tag_scene, name="Device", description="Device that renders F12", items=device_items)
    display_mode: bpy.props.EnumProperty(
        name="Display Mode", description="What the viewport shows; heat maps ignore the view transform",
        items=DISPLAY_MODE_ITEMS, default="BEAUTY")
    samples: bpy.props.IntProperty(
        update=_tag_scene, name="Render Samples", description="Samples per pixel for F12 renders", default=4096, min=1)
    viewport_samples: bpy.props.IntProperty(
        update=_tag_scene, name="Viewport Samples", description="Samples per pixel after which the viewport stops",
        default=1024, min=1)
    seed: bpy.props.IntProperty(
        update=_tag_scene, name="Seed", description="Seed of the sampler's random numbers", default=0, min=0)
    use_animated_seed: bpy.props.BoolProperty(
        update=_tag_scene, name="Animate Seed", description="Change the seed with the frame number", default=False)
    max_bounces: bpy.props.IntProperty(
        update=_tag_scene, name="Max Bounces", description="Total number of bounces along a path", default=12, min=0)
    min_bounces: bpy.props.IntProperty(
        update=_tag_scene, name="Min Bounces",
        description="Bounces before Russian roulette starts; at or above Max Bounces it never starts",
        default=0, min=0)
    transparent_bounces: bpy.props.IntProperty(
        update=_tag_scene, name="Transparent Bounces",
        description="Opacity pass-throughs along a path; the next surface after the limit emits and ends the path",
        default=8, min=0)
    pixel_filter_type: bpy.props.EnumProperty(
        update=_tag_scene, name="Pixel Filter", description="Reconstruction filter of the film", items=FILTER_ITEMS,
        default="BLACKMAN_HARRIS")
    pixel_filter_width: bpy.props.FloatProperty(
        update=_tag_scene, name="Filter Width", description="Width of the pixel filter in pixels", default=1.5,
        min=0.01, max=10.0)
    film_exposure: bpy.props.FloatProperty(
        update=_tag_scene, name="Film Exposure", description="Linear multiplier applied to the display channels",
        default=1.0, min=0.0, soft_max=10.0)


# The authored GRATING block of ar_material_desc::ext_constants (0005 items 16, 23, 24): the field order and
# format mirror AR_GRATING_AUTHORED_FIELDS in src/host/material/grating_authored.h, and the limits are the ones
# parse_grating_authored enforces. The installed extension cannot read the header, so
# tests/blender/headless/test_material_panel.py compares both tables with the header and scenes/lib/ar.py.
# The RNA limits and the packer read this one table.
GRATING_BLOCK_FORMAT = "<ffffBBbb44x"
GRATING_BLOCK_FIELDS = ("pitch_nm", "depth_nm", "coating_n", "coating_k", "mode", "profile", "m_min", "m_max")
GRATING_MODE_REFLECT, GRATING_PROFILE_SINUSOIDAL = 0, 0  # the panel offers neither choice (186 refuses the others)
GRATING_ORDER_LIMIT = 8  # |m| <= 8
# The header demands pitch and depth > 0 and coating n, k >= 0; an RNA minimum cannot be exclusive, so the
# smallest value a panel field accepts is a thousandth of a nanometre.
GRATING_POSITIVE_MIN = 1e-3
# UI starting values, not presets (0005 item 24: presets are provisional and none is shipped).
GRATING_DEFAULTS = {"pitch_nm": 1600.0, "depth_nm": 100.0, "coating_n": 1.0, "coating_k": 6.0, "m_min": -2, "m_max": 2}


def pack_grating(pitch_nm, depth_nm, coating_n, coating_k, m_min, m_max):
    """The 64 bytes of a reflective sinusoidal GRATING block."""
    values = dict(pitch_nm=pitch_nm, depth_nm=depth_nm, coating_n=coating_n, coating_k=coating_k,
                  mode=GRATING_MODE_REFLECT, profile=GRATING_PROFILE_SINUSOIDAL, m_min=m_min, m_max=m_max)
    return struct.pack(GRATING_BLOCK_FORMAT, *[values[n] for n in GRATING_BLOCK_FIELDS])


def ext_block(props):
    """(kind name, 64 bytes) of a material's `astroray` group; the caller maps the name to the ABI number
    (`_ops.EXT_KINDS`, which tools/opsgen.py emits from ext_fields.def). NONE is 64 zero bytes."""
    if props.ext_kind == "GRATING":
        return "GRATING", pack_grating(props.grating_pitch_nm, props.grating_depth_nm, props.grating_coating_n,
                                         props.grating_coating_k, props.grating_m_min, props.grating_m_max)
    return "NONE", bytes(64)


def _tag_material(self, context):
    """Same reason as _tag_scene: without it an edit never reaches view_update, so the material would not be
    re-translated."""
    self.id_data.update_tag()


class AstrorayMaterial(bpy.types.PropertyGroup):
    ext_kind: bpy.props.EnumProperty(
        update=_tag_material, name="Extension", description="The one non-OpenPBR behaviour of this material; "
        "it replaces the whole OpenPBR surface", default="NONE",
        items=(("NONE", "None", "OpenPBR surface"),
               ("GRATING", "Diffraction Grating", "A reflective diffraction grating")))
    grating_pitch_nm: bpy.props.FloatProperty(
        update=_tag_material, name="Pitch", description="Groove spacing in nanometres", default=GRATING_DEFAULTS["pitch_nm"],
        min=GRATING_POSITIVE_MIN, soft_max=20000.0, precision=1)
    grating_depth_nm: bpy.props.FloatProperty(
        update=_tag_material, name="Groove Depth", description="Peak-to-peak depth of the sinusoidal grooves in nanometres",
        default=GRATING_DEFAULTS["depth_nm"], min=GRATING_POSITIVE_MIN, soft_max=2000.0, precision=1)
    grating_m_min: bpy.props.IntProperty(
        update=_tag_material, name="Lowest Order", description="Lowest diffraction order m that is traced",
        default=GRATING_DEFAULTS["m_min"], min=-GRATING_ORDER_LIMIT, max=GRATING_ORDER_LIMIT)
    grating_m_max: bpy.props.IntProperty(
        update=_tag_material, name="Highest Order", description="Highest diffraction order m that is traced",
        default=GRATING_DEFAULTS["m_max"], min=-GRATING_ORDER_LIMIT, max=GRATING_ORDER_LIMIT)
    grating_coating_n: bpy.props.FloatProperty(
        update=_tag_material, name="Coating n", description="Real part of the coating's refractive index (constant over wavelength)",
        default=GRATING_DEFAULTS["coating_n"], min=0.0, soft_max=10.0)
    grating_coating_k: bpy.props.FloatProperty(
        update=_tag_material, name="Coating k", description="Imaginary part of the coating's refractive index "
        "(extinction; constant over wavelength)", default=GRATING_DEFAULTS["coating_k"], min=0.0, soft_max=50.0)


_classes = (AstrorayScene, AstrorayMaterial)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.astroray = bpy.props.PointerProperty(type=AstrorayScene)
    bpy.types.Material.astroray = bpy.props.PointerProperty(type=AstrorayMaterial)


def unregister():
    del bpy.types.Material.astroray
    del bpy.types.Scene.astroray
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
