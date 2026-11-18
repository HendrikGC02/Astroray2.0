# SPDX-License-Identifier: Apache-2.0
"""Node values against Blender's own evaluation: the emission-plane bake (0005 Verification 6).

Run through Invoke-ArBlender. A textured plane whose only light is Principled emission is rendered
twice at the same 16 x 16 view: by Cycles at 1 spp with a 0.01 px box filter (Blender evaluating the
node graph itself; the pixel is the node value) and by the engine through the addon's translation and
image sync. The engine is spectral, so it needs many samples per pixel to average out the wavelength
noise of one: the comparison is of node values, not of sample counts.
Ref: docs/decisions/0005-material-model.md Verification 6 (v1's emission-plane method, docs/v1-lessons).
"""
import math
import os
import sys
import tempfile
import unittest

import numpy as np

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_images as T  # noqa: E402

N = T.N

# 0005 V6: node values within 1e-4 of Blender's own evaluation. The engine is spectral, so a render of
# an RGB colour carries wavelength noise and a spectral round trip (1e-5 to 2.7e-4 for a constant
# colour) that are properties of the light path, not of node values. The reference is therefore a
# mosaic rendered the same way: one constant-emission quad per pixel, coloured with Blender's value
# for that pixel. Both renders use the same camera, seed, sample count and device, and the sampler is
# keyed on pixel, sample, dimension and seed (0003 d23), so each pixel draws the same wavelengths in
# both and textured - mosaic is the node value through one shared round trip.
TOLERANCE = 1e-4
ENGINE_SPP = 64
EVIDENCE = os.path.join(T.ROOT, "out", "evidence", "110")


def smooth_png(directory):
    """An 8 x 8 sRGB PNG that changes by about 0.012 per texel: with 2 px per texel the 0.005 px
    filter jitter of either renderer then moves a Linear value by well under the tolerance, while
    Closest and Linear still differ by about 0.005."""
    i, j = np.meshgrid(np.arange(8), np.arange(8))
    rgba = np.ones((8, 8, 4), np.float32)
    rgba[:, :, 0] = (0.35 + 0.012 * i) ** (1 / 2.2)
    rgba[:, :, 1] = (0.35 + 0.012 * j) ** (1 / 2.2)
    rgba[:, :, 2] = (0.30 + 0.006 * (i + j)) ** (1 / 2.2)
    src = bpy.data.images.new("smooth_src", 8, 8, alpha=False, float_buffer=False)
    src.pixels[0]
    src.pixels.foreach_set(rgba.reshape(-1))
    src.file_format = "PNG"
    src.filepath_raw = os.path.join(directory, "smooth.png")
    src.save()
    return bpy.data.images.load(src.filepath_raw)


def cycles_bake(scene, directory):
    """Cycles' own evaluation of the scene: 1 spp, 0.01 px box filter, Standard view transform, no
    denoiser; the linear float image it writes, (N, N, 3) with rows bottom-up like `image.pixels`."""
    r, cy = scene.render, scene.cycles
    r.engine = "CYCLES"
    cy.device, cy.samples, cy.use_denoising = "CPU", 1, False
    cy.pixel_filter_type, cy.filter_width = "BOX", 0.01
    r.film_transparent = False
    scene.view_settings.view_transform, scene.view_settings.look = "Standard", "None"
    scene.view_settings.exposure, scene.view_settings.gamma = 0.0, 1.0
    r.image_settings.file_format, r.image_settings.color_depth = "OPEN_EXR", "32"
    r.image_settings.color_mode = "RGB"
    path = os.path.join(directory, "cycles.exr")
    r.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = "Linear Rec.709"
    return T.pixels_of(img)[:, :, :3].astype(np.float64)


def evidence_png(engine, cycles, diff, name):
    """Engine textured | Cycles | 1000 x |textured - mosaic|, each pixel as an 8 x 8 block, in display sRGB."""
    os.makedirs(EVIDENCE, exist_ok=True)
    diff = np.clip(diff * 1000.0, 0.0, 1.0)
    row = np.concatenate([T.srgb_encode(engine), T.srgb_encode(cycles), diff], axis=1)
    big = row.repeat(8, axis=0).repeat(8, axis=1)
    rgba = np.ones(big.shape[:2] + (4,), np.float32)
    rgba[:, :, :3] = big
    img = bpy.data.images.new(name, big.shape[1], big.shape[0], alpha=False, float_buffer=False)
    img.pixels[0]
    img.pixels.foreach_set(rgba.reshape(-1))
    img.file_format = "PNG"
    img.filepath_raw = os.path.join(EVIDENCE, name + ".png")
    img.save()


def mosaic_rig(case, bake):
    """A second engine scene over the same camera, in which pixel (row, col) is a 1 m quad (1:1 with
    the render's pixels) of constant emission equal to Blender's value there."""
    scene, plane = case.rig.scene, case.rig.plane
    me = bpy.data.meshes.new("Mosaic")
    verts = [(-8 + c, -8 + r, 0) for r in range(N + 1) for c in range(N + 1)]
    quads = [(r * (N + 1) + c, r * (N + 1) + c + 1, (r + 1) * (N + 1) + c + 1, (r + 1) * (N + 1) + c)
             for r in range(N) for c in range(N)]
    me.from_pydata(verts, [], quads)
    mats = []
    for r in range(N):
        for c in range(N):
            m = bpy.data.materials.new(f"mosaic_{r}_{c}")
            m.use_nodes = True
            p = m.node_tree.nodes["Principled BSDF"]
            p.inputs["Base Color"].default_value = (0, 0, 0, 1)
            p.inputs["Emission Color"].default_value = (*[float(v) for v in bake[r, c]], 1.0)
            p.inputs["Emission Strength"].default_value = 1.0
            me.materials.append(m)
            mats.append(m)
    me.polygons.foreach_set("material_index", list(range(N * N)))
    plane.data = me
    bpy.context.view_layer.update()
    return T.Rig(scene, plane, mats[0], case.rig.world, mats[1:])


class TestNodeValues(T.ImageCase):
    def bake(self, interpolation):
        """(engine textured, Cycles' value, engine mosaic of Cycles' value), (N, N, 3) each."""
        rig = self.start()
        image = smooth_png(self._dir.name)
        T.emit_texture(self.mat, image, interpolation, "EXTEND")
        lines = rig.sync(spp=ENGINE_SPP)
        textured, entries = rig.render()
        self.assertEqual((lines, T.unexpected(entries)), ([], []))
        blender = cycles_bake(rig.scene, self._dir.name)
        mrig = mosaic_rig(self, blender)
        self.addCleanup(mrig.close)
        mrig.sync(spp=ENGINE_SPP)
        mosaic, entries = mrig.render()
        self.assertEqual(T.unexpected(entries), [])
        return textured.astype(np.float64), blender, mosaic.astype(np.float64)

    def check(self, interpolation):
        textured, blender, mosaic = self.bake(interpolation)
        err = np.abs(textured - mosaic)
        print(f"RESULT node values {interpolation} |textured - mosaic| max {err.max():.3e} mean {err.mean():.3e} "
              f"(tolerance {TOLERANCE:g}); |mosaic - blender| max {np.abs(mosaic - blender).max():.3e}, "
              f"|textured - blender| max {np.abs(textured - blender).max():.3e}; "
              f"blender range {blender.min():.3f}..{blender.max():.3f}")
        evidence_png(textured, blender, err, "node_values" if interpolation == "Linear" else "node_values_closest")
        self.assertLess(err.max(), TOLERANCE)
        return blender

    def test_closest_texture(self):
        self.check("Closest")

    def test_linear_texture(self):
        linear = self.check("Linear")
        # The case would pass vacuously if Blender's Linear were Closest: it is not.
        closest = cycles_bake_of_other_filter(self, "Closest")
        self.assertGreater(np.abs(linear - closest).max(), 10 * TOLERANCE)


def cycles_bake_of_other_filter(case, interpolation):
    """Cycles' value with the same scene and another filter (the scene is the case's own)."""
    for n in case.mat.node_tree.nodes:
        if n.bl_idname == "ShaderNodeTexImage":
            n.interpolation = interpolation
    return cycles_bake(case.rig.scene, case._dir.name)


if __name__ == "__main__":
    T.support.main()
