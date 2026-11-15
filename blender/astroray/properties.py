# SPDX-License-Identifier: GPL-3.0-or-later
"""`scene.astroray`: only the settings the M1a sync uses (0007 d37).

Ref: docs/decisions/0007-blender-integration.md decision 37 (native properties are read directly,
never mirrored: the pixel filter width is `scene.render.filter_size`, film transparency and the
border are `scene.render`); Cycles' defaults (samples 4096, preview 1024, 12 bounces, Blackman-
Harris filter) for the names and values that mean the same here.
"""
import bpy

FILTER_ITEMS = (
    ("BOX", "Box", "Box filter"),
    ("GAUSSIAN", "Gaussian", "Gaussian filter"),
    ("BLACKMAN_HARRIS", "Blackman-Harris", "Blackman-Harris filter"),
)


class AstrorayScene(bpy.types.PropertyGroup):
    samples: bpy.props.IntProperty(
        name="Render Samples", description="Samples per pixel for F12 renders", default=4096, min=1)
    viewport_samples: bpy.props.IntProperty(
        name="Viewport Samples", description="Samples per pixel after which the viewport stops",
        default=1024, min=1)
    seed: bpy.props.IntProperty(
        name="Seed", description="Seed of the sampler's random numbers", default=0, min=0)
    use_animated_seed: bpy.props.BoolProperty(
        name="Animate Seed", description="Change the seed with the frame number", default=False)
    max_bounces: bpy.props.IntProperty(
        name="Max Bounces", description="Total number of bounces along a path", default=12, min=0)
    min_bounces: bpy.props.IntProperty(
        name="Min Bounces",
        description="Bounces before Russian roulette starts; at or above Max Bounces it never starts",
        default=0, min=0)
    pixel_filter_type: bpy.props.EnumProperty(
        name="Pixel Filter", description="Reconstruction filter of the film", items=FILTER_ITEMS,
        default="BLACKMAN_HARRIS")
    film_exposure: bpy.props.FloatProperty(
        name="Film Exposure", description="Linear multiplier applied to the display channels",
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
