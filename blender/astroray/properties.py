# SPDX-License-Identifier: GPL-3.0-or-later
"""`scene.astroray`: only the settings the M1a sync uses (0007 d37).

Ref: docs/decisions/0007-blender-integration.md decision 37 (native properties are read directly,
never mirrored; `render.filter_size` is EEVEE's filter, so the Astroray filter has its own width);
Cycles intern/cycles/blender/addon/properties.py (Apache-2.0, read 2026-11-16) for the names and
defaults that mean the same here: samples 4096, preview_samples 1024, seed 0, max_bounces 12,
transparent_max_bounces 8, pixel_filter_type BLACKMAN_HARRIS, filter_width 1.5, film_exposure 1.0.
`device` (CPU or CUDA, default CPU) is the analogue of Cycles' scene.cycles.device and selects the F12 device.
"""
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


_classes = (AstrorayScene,)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.astroray = bpy.props.PointerProperty(type=AstrorayScene)


def unregister():
    del bpy.types.Scene.astroray
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
