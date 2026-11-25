# SPDX-License-Identifier: Apache-2.0
"""Node values against Blender's own evaluation: the emission-plane bake (0005 Verification 6).

Run through Invoke-ArBlender. A textured plane whose only light is emission is rendered twice at the
same 16 x 16 view: by Cycles at 1 spp with a 0.01 px box filter (Blender evaluating the node graph
itself; the pixel is the node value) and by the engine through the addon's translation and image sync.
The engine is spectral, so it needs many samples per pixel to average out the wavelength noise of one:
the comparison is of node values, not of sample counts. Every lowered node that produces a value has a
case here, and `COVERAGE` names the case (or the reason there is none) for every row of the lowering
table, so a new lowering without a case fails `test_every_lowering_has_a_case`.
Ref: docs/decisions/0005-material-model.md Verification 6 (v1's emission-plane method, docs/v1-lessons).
"""
import importlib
import math
import os
import sys

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
EVIDENCE = os.path.join(T.ROOT, "out", "evidence", "189")

# Every row of lowering_table.LOWERING: the case that checks its value, or one of the fixed reasons it
# has none. A case name must be a test method of TestNodeValues. ShaderNodeMixShader is listed ahead of
# its lowering (166): Mix Shader selects stochastically, which is transport and not a node value.
REASONS = ("output node", "coordinate source: covered through its consumer", "spectral value: item 36",
           "selection: 148")
COVERAGE = {
    "ShaderNodeBsdfPrincipled": "test_linear_texture",  # Emission Color from a texture, colour to float below
    "ShaderNodeEmission": "test_emission_node",
    "ShaderNodeBlackbody": "spectral value: item 36",
    "ShaderNodeTexImage": "test_linear_texture",  # half and float EXR, Repeat, and the 16-bit PNG refusal after it
    "ShaderNodeTexEnvironment": "test_environment_texture_linear",
    "ShaderNodeTexCoord": "coordinate source: covered through its consumer",
    "ShaderNodeUVMap": "coordinate source: covered through its consumer",
    "ShaderNodeOutputMaterial": "output node",
    "ShaderNodeOutputWorld": "output node",
    "ShaderNodeBackground": "test_environment_texture_linear",  # Color times a Strength of 1.25
    "ShaderNodeMixShader": "selection: 148",
}


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


def gradient(size, base, step):
    """(size, size, 3) linear values base + step * (i, j, i + j), rows bottom-up: a gentle ramp per
    channel, so that the 0.005 px jitter of either renderer stays far below the tolerance."""
    i, j = np.meshgrid(np.arange(size), np.arange(size))
    return np.stack([base[0] + step[0] * i, base[1] + step[1] * j, base[2] + step[2] * (i + j)], axis=2)


def save_image(directory, name, rgb, file_format, depth):
    """A file-backed image of linear float `rgb`, (h, w, 3) rows bottom-up, loaded back from disk.
    Written by `Image.save_render` under a scratch scene (Standard view): an EXR holds the scene-linear
    values as they are (`depth` "16" is half, "32" float), a PNG their sRGB encoding. The expected values
    are whatever Blender reads back, never `rgb`."""
    h, w = rgb.shape[:2]
    src = bpy.data.images.new(name + "_src", w, h, alpha=False, float_buffer=True)
    rgba = np.ones((h, w, 4), np.float32)
    rgba[:, :, :3] = rgb
    src.pixels.foreach_set(rgba.reshape(-1))
    scene = bpy.data.scenes.new("save_" + name)
    st = scene.render.image_settings
    st.file_format, st.color_mode, st.color_depth = file_format, "RGB", depth
    scene.view_settings.view_transform, scene.view_settings.look = "Standard", "None"
    path = os.path.join(directory, name + {"PNG": ".png", "OPEN_EXR": ".exr"}[file_format])
    src.save_render(path, scene=scene)
    bpy.data.scenes.remove(scene)
    return bpy.data.images.load(path)


def save_exr(directory, name, rgb, depth):
    return save_image(directory, name, rgb, "OPEN_EXR", depth)


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
    return T.Rig(scene, plane, mats[0], case.rig.world, mats[1:], device=case.rig.device)


def cuda_offered():
    """Whether the addon offers CUDA (its own device list; it drops CUDA after a sticky GPU error)."""
    return "CUDA" in importlib.import_module("ar_addon.native").devices()


# The Closest case reads an image of 4 x 4 texel blocks (16 x 8 blocks): a Closest value jumps at a block
# edge, and a pixel whose centre lies within the 0.005 px filter jitter of an edge may land on either
# side in the two renderers. The view below puts every block edge at least EDGE_MARGIN px from every
# pixel centre, which `view_edge_margin` checks for the camera Blender actually holds.
BLOCK = 4
EDGE_MARGIN = 0.012


def view_the_world(scene, plane):
    """The camera at the origin, 98.6 degrees wide, pitched 60.2 degrees above the horizon toward -x
    (with a small roll and yaw, found by a random search over the camera for the two guards below,
    `view_edge_margin` and `linear_jitter_spread`): the view holds the seam of the equirectangular image
    (u = 0, straight behind +x) and the pole. The plane the session needs is out of sight below."""
    cam = scene.camera
    cam.location = (0.0, 0.0, 0.0)
    cam.rotation_euler = (math.radians(90.0 + 60.2), math.radians(1.69), math.radians(90.59))
    cam.data.angle = math.radians(98.6)
    plane.location = (0.0, 0.0, -100.0)
    bpy.context.view_layer.update()


def view_directions(cam, jitter=(0.0, 0.0)):
    """World directions through the centres of the N x N pixels (rows from the top) of a square
    perspective camera, shifted by `jitter` pixels."""
    t = math.tan(cam.data.angle / 2.0)
    col, row = np.meshgrid(np.arange(N), np.arange(N))
    sx, sy = 2.0 * (col + 0.5 + jitter[0]) / N - 1.0, 1.0 - 2.0 * (row + 0.5 + jitter[1]) / N
    d = np.stack([sx * t, sy * t, -np.ones_like(sx)], axis=2)
    w = d @ np.array(cam.matrix_world.to_3x3()).T
    return w / np.linalg.norm(w, axis=2, keepdims=True)


def equirect_blocks(w, blocks):
    """(U, V) of directions `w` in units of image blocks: Blender's equirectangular mapping, the same
    formula as Cycles' direction_to_equirectangular_range with range (-2 pi, pi, -pi, pi)."""
    u = (np.arctan2(w[..., 1], w[..., 0]) - math.pi) / (-2.0 * math.pi)
    v = (np.arccos(np.clip(w[..., 2], -1.0, 1.0)) - math.pi) / (-math.pi)
    return u * blocks[0], v * blocks[1]


def view_edge_margin(cam, blocks):
    """The smallest distance in pixels from a pixel centre to a block edge of the image, along either
    image axis (the edge's distance in blocks over the local gradient in blocks per pixel). This is
    only used to place the camera; the expected values are Cycles'."""
    e = 0.01
    U, V = equirect_blocks(view_directions(cam), blocks)
    Ux, Vx = equirect_blocks(view_directions(cam, (e, 0.0)), blocks)
    Uy, Vy = equirect_blocks(view_directions(cam, (0.0, e)), blocks)

    def wrap(a):
        return (a + blocks[0] / 2.0) % blocks[0] - blocks[0] / 2.0

    def edge(a):
        return np.minimum(a % 1.0, 1.0 - a % 1.0)

    du = edge(U) / np.hypot(wrap(Ux - U) / e, wrap(Uy - U) / e)
    dv = edge(V) / np.hypot((Vx - V) / e, (Vy - V) / e)
    return float(min(du.min(), dv.min())), U, V


def linear_jitter_spread(cam, rgb):
    """The largest change of any pixel's Linear value of the equirectangular image `rgb` (h, w, 3, rows
    bottom-up) when the sample moves by +-0.005 px (the 0.01 px box filter) in x or y, from a numpy
    model of Cycles' bilinear read: texel coordinates u * w and v * h, the -0.5 centre offset, REPEAT in
    both axes (so within half a texel of the pole the top row blends with the bottom row).
    Cycles and the engine jitter their samples independently, so the two renders of one pixel can
    differ by up to twice this; the camera is placed so that twice it stays under TOLERANCE.
    Ref: third_party/_cache/refs/cycles-ed69ef2c6bd3/src/kernel/util/image.h (interp_linear)."""
    h, w = rgb.shape[:2]

    def read(jitter):
        U, V = equirect_blocks(view_directions(cam, jitter), (w, h))
        x, y = U - 0.5, V - 0.5
        i, j = np.floor(x).astype(int), np.floor(y).astype(int)
        tx, ty = (x - i)[..., None], (y - j)[..., None]
        g = lambda a, b: rgb[b % h, a % w]  # noqa: E731
        return ((1 - ty) * (1 - tx) * g(i, j) + (1 - ty) * tx * g(i + 1, j)
                + ty * (1 - tx) * g(i, j + 1) + ty * tx * g(i + 1, j + 1))

    centre = read((0.0, 0.0))
    return float(max(np.abs(read((jx, jy)) - centre).max() for jx in (-0.005, 0.005) for jy in (-0.005, 0.005)))


def pole_in_view(cam):
    """Whether the +z pole projects inside the frame, at least a pixel from its edge."""
    t = math.tan(cam.data.angle / 2.0)
    c = np.array(cam.matrix_world.to_3x3()).T @ np.array([0.0, 0.0, 1.0])
    return bool(c[2] < 0 and abs(c[0] / -c[2] / t) < 1 - 2.0 / N and abs(c[1] / -c[2] / t) < 1 - 2.0 / N)


def view_the_plane(scene, plane):
    """The standard rig again, for the mosaic: the camera 5 m above the plane, 1 m per pixel."""
    cam = scene.camera
    cam.location = (0.0, 0.0, 5.0)
    cam.rotation_euler = (0.0, 0.0, 0.0)
    cam.data.angle = 2 * math.atan(8.0 / 5.0)
    plane.location = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()


def environment_rgb(w=64, h=32, block=1):
    """A 64 x 32 equirectangular image of values that are linear in the direction, 0.5 + 0.06 d_c per
    channel, from Blender's mapping (u = 0.5 is +x, +y is u = 0.25, v = 1 is +z): smooth across the
    seam and over the pole, so only a convention error can move a value. With block > 1 the value of the
    block's centre fills each block x block texels. Rows bottom-up."""
    iy, ix = np.meshgrid(np.arange(h) // block * block + block / 2.0 - 0.5,
                         np.arange(w) // block * block + block / 2.0 - 0.5, indexing="ij")
    phi = math.pi - 2.0 * math.pi * (ix + 0.5) / w
    theta = math.pi * (1.0 - (iy + 0.5) / h)
    d = np.stack([np.sin(theta) * np.cos(phi), np.sin(theta) * np.sin(phi), np.cos(theta)], axis=2)
    return 0.5 + 0.06 * d


class TestNodeValues(T.ImageCase):
    def measure(self, name, alternatives=None, device=None, mosaic_prepare=None):
        """Renders the case's scene (self.rig, configured by the caller) textured, bakes it with Cycles,
        bakes each alternative (a callable that changes the scene), then renders the mosaic of Cycles'
        value. Checks |textured - mosaic| < TOLERANCE and that each alternative differs from Cycles'
        value by more than 10 x TOLERANCE, so the case cannot pass without the node doing its work.
        Returns Cycles' value."""
        rig = self.rig
        lines = rig.sync(spp=ENGINE_SPP)
        textured, entries = rig.render()
        self.assertEqual((lines, T.unexpected(entries)), ([], []))
        blender = cycles_bake(rig.scene, self._dir.name)
        others = {}
        for label, change in (alternatives or {}).items():
            change()
            others[label] = cycles_bake(rig.scene, self._dir.name)
        if mosaic_prepare is not None:
            mosaic_prepare()
        mrig = mosaic_rig(self, blender)
        self.addCleanup(mrig.close)
        mrig.sync(spp=ENGINE_SPP)
        mosaic, entries = mrig.render()
        self.assertEqual(T.unexpected(entries), [])
        textured, mosaic = textured.astype(np.float64), mosaic.astype(np.float64)
        err = np.abs(textured - mosaic)
        print(f"RESULT node values {name} |textured - mosaic| max {err.max():.3e} mean {err.mean():.3e} "
              f"(tolerance {TOLERANCE:g}); |mosaic - blender| max {np.abs(mosaic - blender).max():.3e}, "
              f"|textured - blender| max {np.abs(textured - blender).max():.3e}; "
              f"blender range {blender.min():.3f}..{blender.max():.3f}")
        for label, other in others.items():
            apart = np.abs(blender - other).max()
            print(f"RESULT node values {name} vacuity: {label} differs from Blender's value by {apart:.3e} "
                  f"(needs more than {10 * TOLERANCE:g})")
            self.assertGreater(apart, 10 * TOLERANCE, f"{name}: {label} gives the same values, the case proves nothing")
        evidence_png(textured, blender, err, "node_values_" + name)
        self.assertLess(err.max(), TOLERANCE)
        return blender

    def texture_case(self, name, make_image, interpolation, device=None):
        """An Image Texture of `make_image()` into Principled Emission Color, measured. The image is made
        after the scene is, because starting a case resets Blender's data."""
        self.start(device)
        tex = T.emit_texture(self.mat, make_image(), interpolation, "EXTEND")
        alternatives = {}
        if interpolation == "Linear":  # the case would pass vacuously if Blender's Linear were Closest
            alternatives["Closest"] = lambda: setattr(tex, "interpolation", "Closest")
        return self.measure(name, alternatives)

    def test_closest_texture(self):
        self.texture_case("closest", lambda: smooth_png(self._dir.name), "Closest")

    def test_linear_texture(self):
        self.texture_case("linear", lambda: smooth_png(self._dir.name), "Linear")

    def test_emission_node(self):
        """Emission node: the textured Color times Strength 1.5."""
        rig = self.start()
        t = self.mat.node_tree
        emission = t.nodes.new("ShaderNodeEmission")
        emission.inputs["Strength"].default_value = 1.5
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image, tex.interpolation, tex.extension = smooth_png(self._dir.name), "Linear", "EXTEND"
        t.links.new(tex.outputs["Color"], emission.inputs["Color"])
        t.links.new(emission.outputs["Emission"], t.nodes["Material Output"].inputs["Surface"])
        t.nodes.remove(t.nodes["Principled BSDF"])
        self.measure("emission", {"Strength 1": lambda: emission.inputs["Strength"].__setattr__("default_value", 1.0)})

    def test_color_to_float(self):
        """Image Texture Color into Principled Emission Strength: Cycles converts a colour linked to a
        float input with NODE_CONVERT_CF, `linear_rgb_to_gray` (the working space's luminance row;
        third_party/_cache/refs/cycles-ed69ef2c6bd3/src/kernel/svm/convert.h line 35 and
        src/kernel/util/colorspace.h line 35). Against a white Emission Color the pixel is that luminance."""
        self.start()
        t = self.mat.node_tree
        p = t.nodes["Principled BSDF"]
        p.inputs["Emission Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image, tex.interpolation, tex.extension = smooth_png(self._dir.name), "Linear", "EXTEND"
        link = t.links.new(tex.outputs["Color"], p.inputs["Emission Strength"])
        plain = {}

        def colour_itself():
            """The same texture as the emission colour: the channels the conversion might have taken."""
            t.links.remove(link)
            t.links.new(tex.outputs["Color"], p.inputs["Emission Color"])
            p.inputs["Emission Strength"].default_value = 1.0
            plain["rgb"] = cycles_bake(self.rig.scene, self._dir.name)

        blender = self.measure("color_to_float", {"as colour": colour_itself})
        # The channel alternatives need the plain bake, which exists only after `colour_itself` ran.
        for label, rgb in (("red channel", plain["rgb"][:, :, 0]), ("green channel", plain["rgb"][:, :, 1]),
                           ("blue channel", plain["rgb"][:, :, 2]), ("mean", plain["rgb"].mean(axis=2))):
            apart = np.abs(blender[:, :, 0] - rgb).max()
            print(f"RESULT node values color_to_float vacuity: {label} differs from Blender's value by {apart:.3e}")
            self.assertGreater(apart, 10 * TOLERANCE, label)

    def exr_texture(self, kind, depth, interpolation):
        def make():
            # Values above 1 in every channel but red's first texels, 0.02 per texel (blue 0.01).
            rgb = gradient(8, (0.90, 1.20, 2.00), (0.02, 0.02, 0.01))
            image = save_exr(self._dir.name, kind, rgb, depth)
            got = T.pixels_of(image)[:, :, :3]
            print(f"RESULT node values {kind} file reads back {got.min():.3f}..{got.max():.3f}, "
                  f"float buffer {image.is_float}")
            self.assertGreater(got.max(), 2.0)  # the wide texels are in the file Blender reads
            return image

        suffix = "" if interpolation == "Linear" else "_closest"
        return self.texture_case(kind + suffix, make, interpolation)

    def test_half_exr_closest(self):
        self.exr_texture("half_exr", "16", "Closest")

    def test_half_exr_linear(self):
        self.exr_texture("half_exr", "16", "Linear")

    def test_float_exr_closest(self):
        self.exr_texture("float_exr", "32", "Closest")

    def test_float_exr_linear(self):
        self.exr_texture("float_exr", "32", "Linear")

    def test_png16_is_refused(self):
        """A 16-bit integer PNG has no value case because the engine refuses it by design (stb would
        narrow it to 8 bits silently, src/host/image/decode.cpp): the Image Texture renders magenta and
        the session raises the image-unreadable entry with the decoder's reason. The refusal is the
        behaviour under test."""
        rig = self.start()
        image = save_image(self._dir.name, "png16", gradient(8, (0.25, 0.25, 0.20), (0.012, 0.012, 0.006)), "PNG", "16")
        T.emit_texture(self.mat, image, "Linear")
        lines = rig.sync(spp=ENGINE_SPP)
        got, entries = rig.render()
        mine = T.unexpected(entries)
        mean = got.reshape(-1, 3).mean(axis=0)
        print(f"RESULT node values png16 refused: lines {lines} entries {mine} mean rgb {mean}")
        self.assertEqual([e[0] for e in mine], [T.ABI.AR_REPORT_IMAGE_UNREADABLE], mine)
        self.assertIn("16-bit integer", mine[0][1])
        self.assertLess(mean[1], 0.25 * min(mean[0], mean[2]))  # magenta: no green
        self.assertGreater(mean[0], 0.1)

    def test_float_exr_linear_cuda(self):
        if not cuda_offered():
            print("RESULT node values float_exr_cuda not generated: the addon offers no CUDA device")
            return
        self.texture_case("float_exr_cuda", lambda: save_exr(
            self._dir.name, "float_exr_cuda", gradient(8, (0.90, 1.20, 2.00), (0.02, 0.02, 0.01)), "32"),
            "Linear", device=T.ABI.AR_DEVICE_CUDA)

    def repeat_case(self, interpolation):
        """UVs over [-0.5, 1.5]: the outer quarters are Repeat's wrapped texels or Extend's edge texels. A
        4 x 4 texture whose rows and columns are periodic, so a Linear blend across the wrap is smooth."""
        rig = self.start()
        uv = rig.plane.data.uv_layers["UVMap"]
        for loop, co in zip(rig.plane.data.loops, ((-0.5, -0.5), (1.5, -0.5), (1.5, 1.5), (-0.5, 1.5))):
            uv.data[loop.index].uv = co
        k = 2.0 * math.pi * np.arange(4) / 4.0
        i, j = np.meshgrid(k, k)
        rgb = np.stack([0.45 + 0.02 * np.sin(i), 0.45 + 0.02 * np.cos(j), 0.40 + 0.01 * (np.sin(i) + np.cos(j))], axis=2)
        image = save_exr(self._dir.name, "repeat", rgb, "32")
        tex = T.emit_texture(self.mat, image, interpolation, "REPEAT")
        self.measure("repeat" + ("" if interpolation == "Linear" else "_closest"),
                     {"Extend": lambda: setattr(tex, "extension", "EXTEND")})

    def test_repeat_closest(self):
        self.repeat_case("Closest")

    def test_repeat_linear(self):
        self.repeat_case("Linear")

    def environment_case(self, interpolation):
        """An Environment Texture into the world. The camera looks into it; the reference mosaic is the
        plane that fills the standard view, so the world is switched off and the camera put back first."""
        rig = self.start()
        world, tw = rig.world, rig.world.node_tree
        image = save_exr(self._dir.name, "environment", environment_rgb(block=BLOCK if interpolation == "Closest" else 1), "32")
        env = tw.nodes.new("ShaderNodeTexEnvironment")
        env.image, env.interpolation, env.projection = image, interpolation, "EQUIRECTANGULAR"
        bg = tw.nodes["Background"]
        tw.links.new(env.outputs["Color"], bg.inputs["Color"])
        bg.inputs["Strength"].default_value = 1.25
        view_the_world(rig.scene, rig.plane)
        margin, U, V = view_edge_margin(rig.scene.camera, (64 // BLOCK, 32 // BLOCK))
        print(f"RESULT node values environment view: nearest block edge {margin:.3f} px from a pixel centre, "
              f"u blocks {U.min():.2f}..{U.max():.2f} of {64 // BLOCK}, pole in view {pole_in_view(rig.scene.camera)}")
        self.assertTrue(pole_in_view(rig.scene.camera), "the pole is not in the view")
        self.assertTrue((U < 1.0).any() and (U > 64 // BLOCK - 1.0).any(), "the u = 0 seam is not in the view")
        if interpolation == "Closest":
            self.assertGreater(margin, EDGE_MARGIN, "a pixel centre sits on a block edge: Closest may differ by a block")
        else:
            spread = linear_jitter_spread(rig.scene.camera, environment_rgb())
            print(f"RESULT node values environment view: Linear value moves by at most {spread:.3e} over the 0.005 px jitter")
            self.assertLess(2 * spread, TOLERANCE, "a pixel is steep enough that the two renders' jitter alone can exceed the tolerance")

        def world_off():
            for link in list(tw.links):
                if link.to_node == bg:
                    tw.links.remove(link)
            bg.inputs["Color"].default_value = (0, 0, 0, 1)
            view_the_plane(rig.scene, rig.plane)

        alternatives = {}
        if interpolation == "Linear":
            alternatives["Closest"] = lambda: setattr(env, "interpolation", "Closest")
        self.measure("environment" + ("" if interpolation == "Linear" else "_closest"), alternatives,
                     mosaic_prepare=world_off)

    def test_environment_texture_closest(self):
        self.environment_case("Closest")

    def test_environment_texture_linear(self):
        self.environment_case("Linear")

    def test_every_lowering_has_a_case(self):
        """Every row of the lowering table has a value case or a stated reason (V6, every implemented op)."""
        lowering = importlib.import_module("ar_addon.materials.lowering_table").LOWERING
        missing = sorted(set(lowering) - set(COVERAGE))
        self.assertEqual(missing, [], f"lowered node types with neither a value case nor a reason: {missing}")
        for node, case in COVERAGE.items():
            if case not in REASONS:
                self.assertTrue(callable(getattr(TestNodeValues, case, None)), f"{node}: no test {case}")
        print(f"RESULT node values coverage {len(lowering)} lowered types, "
              f"{sum(1 for k in lowering if COVERAGE[k] not in REASONS)} with a case")


if __name__ == "__main__":
    T.support.main()
