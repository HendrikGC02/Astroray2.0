# SPDX-License-Identifier: Apache-2.0
"""The gate's Blender smoke (0009 d11, d17a): the staged extension zip renders A1 through F12.

Run through Invoke-ArBlender: -- [-k name] [--work <dir>] [--evidence <dir>] [--with-materials]
It first runs scenes/a1_furnace/build.py (the description a1.blend.json for the blend writer and the
builder's recording a1_builder.arrec, Sobol sampler, AOV passes requested) into <dir> (out/smoke).
One Blender start does everything: the zip is installed and enabled, scenes/lib/blend_writer.py builds
the .blend from the description, F12 renders it at 64x64 on the CPU device at the analytic harness's spp
with ASTRORAY_RECORD set, and the cases below check the Combined pass against A1's closed-form truth
(tools/analytic.py check_scene) and the addon's recording against the builder's (tools/exporter_equivalence.py).
--with-materials first runs tests/blender/test_materials.py in the same process (before the extension is
enabled, because that file registers the addon's properties itself).
Then TestF12Textured renders a second scene in the same start: a plane textured with a 2048x2048 noise PNG,
lit by one point lamp, through F12 on the CPU device and on CUDA. Each delivered mean must be over a
luminance floor (an image-job scene that renders black on either device fails: the defect that went unseen
for days) and the two means must agree within the statistical bound of the pair_image check. Like A1's
cases these checks are not counted in budgets.toml [family_members]; m is analytic.family_m().
Ref: docs/decisions/0009-build-test-process.md d11, d16, d17a; 0007 d27.
"""
import glob
import importlib.util
import json
import math
import os
import subprocess
import sys
import time
import unittest

import numpy as np

import bpy

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODULE = "bl_ext.user_default.astroray"
SIZE = 64
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import analytic  # noqa: E402
import ar  # noqa: E402
import blend_writer  # noqa: E402
import exporter_equivalence  # noqa: E402
import meta  # noqa: E402
import truths  # noqa: E402

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def arg(name, default=None):
    return ARGS[ARGS.index(name) + 1] if name in ARGS else default


def zip_path():
    found = sorted(glob.glob(os.path.join(ROOT, "out", "extension", "astroray-*-windows_x64.zip")))
    if not found:
        raise RuntimeError("no extension zip; run tools/build.ps1 -Target astroray_extension")
    return found[-1]


def install_and_enable():
    """Installs the staged zip into the isolated profile (the extension CLI, as test_install.py does)
    and enables it in this process."""
    import addon_utils

    # The zip must be this tree's: a stale one would test yesterday's addon (land's canary changes
    # sources and builds only `all`). The nested Blender of the extension build shares the machine lock.
    b = subprocess.run(["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "build.ps1"),
                        "-Target", "astroray_extension"], capture_output=True, text=True, timeout=600)
    if b.returncode != 0:
        raise RuntimeError("building the extension zip failed: " + b.stdout + b.stderr)
    r = subprocess.run([bpy.app.binary_path, "--command", "extension", "install-file", "-r", "user_default",
                        zip_path()], capture_output=True, text=True, timeout=240)
    if r.returncode != 0:
        raise RuntimeError("install-file failed: " + r.stdout + r.stderr)
    addon_utils.modules_refresh()
    errors = []
    mod = addon_utils.enable(MODULE, default_set=False, handle_error=errors.append)
    if mod is None:
        raise RuntimeError(f"enabling {MODULE} failed: {errors}")
    return mod


def ensure_extension():
    """The extension is enabled once per Blender start, by whichever class runs first (a canary run
    selects TestF12Textured alone)."""
    if MODULE not in sys.modules:
        install_and_enable()


def write_fits(path, planes, header):
    """A FITS file the way src/host/film/fits_writer.cpp writes a dump: an empty primary HDU with
    keywords, then one float32 image extension per plane (rows bottom-up). Ref: FITS Standard 4.0."""
    def card(k, v):
        if isinstance(v, bool):
            v = "T" if v else "F"
            return f"{k:<8}= {v:>20}".ljust(80)
        if isinstance(v, str):
            return f"{k:<8}= '{v}'".ljust(80)
        return f"{k:<8}= {v:>20}".ljust(80)

    def block(cards):
        text = "".join(cards) + "END".ljust(80)
        return text.ljust(-(-len(text) // 2880) * 2880).encode("ascii")

    out = block([card("SIMPLE", True), card("BITPIX", 8), card("NAXIS", 0), card("EXTEND", True)]
                + [card(k, v) for k, v in header.items()])
    for name, a in planes.items():
        a = np.asarray(a, ">f4")
        h, w = a.shape
        out += block([card("XTENSION", "IMAGE"), card("BITPIX", -32), card("NAXIS", 2), card("NAXIS1", w),
                      card("NAXIS2", h), card("PCOUNT", 0), card("GCOUNT", 1), card("EXTNAME", name)])
        data = a.tobytes()
        out += data.ljust(-(-len(data) // 2880) * 2880, b"\0")
    with open(path, "wb") as f:
        f.write(out)


class Capture:
    """Reads Combined MEAN, VARIANCE and COUNT before the job closes its session (the engine holds no
    pixels afterwards); installed over Job._close_session of the enabled extension."""

    def __init__(self, render_final):
        self.rf, self.arrays = render_final, None
        self._orig = render_final.Job._close_session
        capture = self

        def close(job):
            if job.session.value and capture.arrays is None:
                a = render_final._abi()
                capture.arrays = tuple(
                    render_final.read_pass(job.lib, a, job.session, "Combined", q)
                    for q in (a.AR_QUANTITY_MEAN, a.AR_QUANTITY_VARIANCE, a.AR_QUANTITY_COUNT))
            capture._orig(job)

        render_final.Job._close_session = close

    def restore(self):
        self.rf.Job._close_session = self._orig


def run_smoke(work, evidence):
    """Everything that costs time, once. Returns a dict the cases read."""
    import importlib

    os.makedirs(evidence, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    blend = os.path.join(work, "a1.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_builder.arrec"), "--blend", blend, "--sampler", "sobol",
                        "--passes", "Position,Normal,UV,IndexMA"], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    ensure_extension()
    rf = importlib.import_module(MODULE + ".render_final")
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend)
    scene = bpy.context.scene
    scene.astroray.device = "CPU"
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.astroray.samples = analytic.MIN_SPP
    scene.astroray.seed = analytic.seed_for("a1_furnace")
    rec = os.path.join(work, "a1_addon.arrec")
    if os.path.exists(rec):
        os.remove(rec)
    os.environ["ASTRORAY_RECORD"] = rec
    cap = Capture(rf)
    try:
        # EXR at 32 bit: the file is the exact float Combined the engine handed to Blender.
        s = scene.render
        s.filepath = os.path.join(work, "f12.exr")
        s.image_settings.file_format, s.image_settings.color_depth = "OPEN_EXR", "32"
        s.image_settings.exr_codec = "NONE"
        r = bpy.ops.render.render(write_still=True)
        if r != {"FINISHED"}:
            raise RuntimeError(f"bpy.ops.render.render returned {r}")
    finally:
        cap.restore()
        rf.shutdown()  # a recording is complete only after its context is destroyed
        os.environ.pop("ASTRORAY_RECORD")
    if cap.arrays is None:
        raise RuntimeError("the render closed no session")
    img = bpy.data.images["Render Result"]
    png = os.path.join(evidence, "a1_f12.png")
    scene.render.image_settings.file_format, scene.render.image_settings.color_depth = "PNG", "8"
    img.save_render(png, scene=scene)
    exr = bpy.data.images.load(os.path.join(work, "f12.exr"))
    exr.colorspace_settings.name = "Non-Color"
    mean, var = (np.moveaxis(a, 2, 0).astype(np.float64) for a in cap.arrays[:2])
    count = cap.arrays[2][:, :, 0]
    rr = np.array(exr.pixels[:], np.float32).reshape(SIZE, SIZE, 4)[:, :, :3]
    write_fits(os.path.join(evidence, "a1_f12_combined.fits"),
               {**{f"display.{c}": mean[c] for c in range(3)}, **{f"display.{c}.VAR": var[c] for c in range(3)},
                **{f"display.{c}.NSAMP": count for c in range(3)}},
               {"SPP": int(count.max()), "SEED": scene.astroray.seed, "SAMPLER": "sobol", "DEVICE": "cpu"})
    dumps = {}
    for name, path in (("addon", rec), ("builder", os.path.join(work, "a1_builder.arrec"))):
        out = os.path.join(work, f"a1_{name}.fits")
        p = subprocess.run([CLI, "dump", path, "-o", out, "--device", "cpu", "--seed", "7", "--spp", "4",
                            "--size", f"{SIZE}x{SIZE}"], capture_output=True, text=True, timeout=300)
        if p.returncode != 0:
            raise RuntimeError(f"astroray_cli dump {path} exited {p.returncode}:\n{p.stdout}{p.stderr}")
        dumps[name] = out
    return {"mean": mean, "var": var, "count": count, "result": rr, "png": png, "dumps": dumps, "rf": rf,
            "work": work}


class TestSmoke(unittest.TestCase):
    state = None

    @classmethod
    def setUpClass(cls):
        cls.state = run_smoke(arg("--work", os.path.join(ROOT, "out", "smoke")), arg("--evidence", os.path.join(ROOT, "out", "evidence", "084")))

    def test_a1_analytic(self):
        """The F12 Combined pass against A1's truth, every pixel, as tools/analytic.py judges it."""
        t = meta.load(os.path.join(ROOT, "scenes", "a1_furnace", "meta.toml"))
        checks = truths.evaluate(t["truth"], SIZE, SIZE)
        results = analytic.check_scene(self.state["mean"], self.state["var"], checks, analytic.family_m(),
                                       t["eps_sys"])
        for name, passed, reasons, worst, cmean, tmean in results:
            print(f"RESULT smoke a1 {name}: {'ok' if passed else 'FAIL'} mean {cmean:.6f} truth {tmean:.6f} "
                  f"worst z {worst[0]:+.2f} at {worst[1]}")
            self.assertTrue(passed, f"{name}: {reasons}")

    def test_render_result_is_combined(self):
        """What Blender shows is the pass the engine read: the RenderResult pixels equal Combined."""
        a = self.state["mean"].transpose(1, 2, 0)
        np.testing.assert_allclose(self.state["result"], a, rtol=0, atol=1e-6)

    def test_samples_reached(self):
        """The render ran every requested sample: the engine's per-pixel COUNT equals samples."""
        print(f"RESULT smoke COUNT {int(self.state['count'].min())}..{int(self.state['count'].max())} of {analytic.MIN_SPP}")
        self.assertTrue((self.state["count"] == analytic.MIN_SPP).all(),
                        f"COUNT {np.unique(self.state['count'])}, asked for {analytic.MIN_SPP}")

    def _render(self, samples):
        """F12 again on the open scene; returns the Capture and the seconds taken."""
        rf = self.state["rf"]
        scene = bpy.context.scene
        scene.astroray.samples = samples
        scene.render.image_settings.file_format = "OPEN_EXR"
        cap = Capture(rf)
        t0 = time.monotonic()
        try:
            bpy.ops.render.render(write_still=False)
        finally:
            cap.restore()
            rf.shutdown()
        return cap, time.monotonic() - t0

    def test_cancel_returns_partial_result(self):
        """test_break() ends the render early through ar_session_cancel and the partial result is kept
        (0007 d27). 8192 samples take about half a minute; the cancel comes after 0.5 s."""
        rf = self.state["rf"]
        samples, stop_at = 8192, time.monotonic() + 0.5
        original = rf.break_requested
        rf.break_requested = lambda engine: time.monotonic() > stop_at
        try:
            cap, seconds = self._render(samples)
        finally:
            rf.break_requested = original
        self.assertIsNotNone(cap.arrays, "no partial result was read")
        done = cap.arrays[2]
        print(f"RESULT smoke cancel: {seconds:.1f} s, per-pixel count {int(done.min())}..{int(done.max())} of {samples}")
        self.assertLess(int(done.max()), samples // 4)  # cancelled after 0.5 s: a few percent of the target at most
        self.assertGreater(int(done.max()), 0)
        self.assertLess(seconds, 15.0)

    def test_error_entry_fails_render(self):
        """An engine ERROR entry (a NaN vertex: the generation is rejected) fails the render and its
        text reaches the error (0001 D2.11)."""
        me = next(m for m in bpy.data.meshes if len(m.vertices) > 10)
        keep = me.vertices[0].co.copy()
        me.vertices[0].co = (float("nan"),) * 3
        try:
            with self.assertRaises(RuntimeError) as cm:
                self._render(1)
        finally:
            me.vertices[0].co = keep
        print(f"RESULT smoke error entry: {str(cm.exception)[:300]!r}")
        # Blender fails any render whose engine reported an ERROR, with that report's own text; the
        # "astroray: " prefix is added only where _report raises RenderFailed for an ERROR entry.
        self.assertIn("astroray: GENERATION_REJECTED:", str(cm.exception))

    def test_exporter_equivalence(self):
        t = meta.load(os.path.join(ROOT, "scenes", "a1_furnace", "meta.toml"))["truth"]
        # The storage error of the normals Blender holds (INT16_2D), as the addon reads them.
        me = next(m for m in bpy.data.meshes if len(m.vertices) > 10)
        got = np.empty(len(me.loops) * 3, np.float32)
        me.corner_normals.foreach_get("vector", got)
        with open(os.path.join(self.state["work"], "a1.blend.json"), encoding="utf-8") as f:
            want = blend_writer._array(json.load(f)["meshes"][0]["normals"])
        quant = float(np.linalg.norm(got.reshape(-1, 3) - want, axis=1).max())
        min_len = exporter_equivalence.min_interpolated_length(want)
        print(f"RESULT smoke stored-normal error Q = {quant:.3g}, minimum interpolated length {min_len:.5f}")
        # The two cameras as sent: the builder's from the description, the addon's from its own sync.
        c = json.loads(open(os.path.join(self.state["work"], "a1.blend.json"), encoding="utf-8").read())["camera"]
        cam_b = {"cam_to_world": c["cam_to_world"], "projection_inverse": list(
            ar.projection_inverse(c["width"], c["height"], c["focal_mm"], c["sensor_mm"]))}
        f, _ = importlib.import_module(MODULE + ".sync.camera").final(bpy.context.evaluated_depsgraph_get(),
                                                                       bpy.context.scene)
        cam_a = {"cam_to_world": f["cam_to_world"], "projection_inverse": f["projection_inverse"]}
        geometry = {"camera_distance": t["camera_distance"], "radius": t["sphere_radius"],
                    "segments": t["segments"], "rings": t["rings"]}
        ok, lines = exporter_equivalence.compare(self.state["dumps"]["addon"], self.state["dumps"]["builder"],
                                                 geometry, cam_a, cam_b, quant, min_len)
        for ln in lines:
            print("RESULT smoke " + ln)
        self.assertTrue(ok, "\n".join(lines))


F12_NAME = "f12_textured"
F12_SIZE = 32        # pixels per side
F12_TEX = 2048       # texels per side: the decode outlasts a CPU session's start
F12_SPP = 64         # one device's image-mean SE is 0.00019 (0.19 % of the mean 0.100), under the 1 % allowed
F12_POWER = 600.0    # lamp watts (Blender units), 4 m above the plane
F12_FLOOR = 0.05     # half the mean luminance of the first good run (0.1002 on both devices)
F12_LUMA = (0.2126, 0.7152, 0.0722)  # Rec.709 luminance of scene-linear RGB


def build_f12_scene(work):
    """A UV-mapped plane filling the view, Principled Base Color from a seeded noise PNG (linear mean
    about 0.5), black world, one point lamp above the plane, a pinhole camera."""
    scene = bpy.data.scenes.new(F12_NAME)
    scene.render.engine = "ASTRORAY"
    scene.render.resolution_x = scene.render.resolution_y = F12_SIZE
    scene.render.resolution_percentage = 100
    me = bpy.data.meshes.new("F12Plane")
    me.from_pydata([(-8, -8, 0), (8, -8, 0), (8, 8, 0), (-8, 8, 0)], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new(name="UVMap")
    for loop, co in zip(me.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[loop.index].uv = co
    plane = bpy.data.objects.new("F12Plane", me)
    scene.collection.objects.link(plane)
    cd = bpy.data.cameras.new("F12Cam")
    cd.sensor_fit, cd.angle = "AUTO", 2 * math.atan(8.0 / 5.0)  # the plane exactly fills the frame
    cam = bpy.data.objects.new("F12Cam", cd)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, 5.0)
    scene.camera = cam
    ld = bpy.data.lights.new("F12Lamp", "POINT")
    ld.energy, ld.shadow_soft_size, ld.use_soft_falloff = F12_POWER, 0.25, False
    lamp = bpy.data.objects.new("F12Lamp", ld)
    scene.collection.objects.link(lamp)
    lamp.location = (0.0, 0.0, 4.0)
    world = bpy.data.worlds.new("F12World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
    scene.world = world
    src = bpy.data.images.new("f12_tex_src", F12_TEX, F12_TEX, alpha=False, float_buffer=False)
    src.pixels[0]  # allocates the buffer: foreach_set on a fresh image writes nothing otherwise
    rgba = np.ones((F12_TEX * F12_TEX, 4), np.float32)
    # Per-texel noise alone averages out over the 64 texels a pixel covers; 8 x 8 checks of two noise
    # ranges (sRGB codes 153..255 and 100..200) keep the texture visible in the 32-pixel frame.
    codes = np.random.RandomState(139).randint(0, 103, (F12_TEX, F12_TEX))
    dark = (np.add.outer(np.arange(F12_TEX) // (F12_TEX // 8), np.arange(F12_TEX) // (F12_TEX // 8)) % 2) == 1
    rgba[:, :3] = (np.where(dark, 100 + codes, 153 + codes).reshape(-1, 1)) / 255.0
    src.pixels.foreach_set(rgba.reshape(-1))
    src.file_format, src.filepath_raw = "PNG", os.path.join(work, "f12_tex.png")
    src.save()
    mat = bpy.data.materials.new("F12Mat")
    mat.use_nodes = True
    plane.data.materials.append(mat)
    t = mat.node_tree
    tex = t.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(src.filepath_raw)
    t.links.new(tex.outputs["Color"], t.nodes["Principled BSDF"].inputs["Base Color"])
    scene.astroray.samples = F12_SPP
    scene.astroray.seed = analytic.seed_for("f12_textured")
    return scene


class TestF12Textured(unittest.TestCase):
    """The textured, lamp-lit plane through F12 on both devices; one render each, before any case."""
    runs = None

    @classmethod
    def setUpClass(cls):
        import importlib

        work = arg("--work", os.path.join(ROOT, "out", "smoke"))
        evidence = os.path.join(ROOT, "out", "evidence", "139")
        os.makedirs(evidence, exist_ok=True)
        os.makedirs(work, exist_ok=True)
        ensure_extension()
        rf = importlib.import_module(MODULE + ".render_final")
        scene = build_f12_scene(work)
        cls.runs = {}
        for device in ("CPU", "CUDA"):
            scene.astroray.device = device
            s = scene.render
            s.filepath = os.path.join(work, f"f12_{device.lower()}.exr")
            s.image_settings.file_format, s.image_settings.color_depth = "OPEN_EXR", "32"
            s.image_settings.exr_codec = "NONE"
            cap = Capture(rf)
            t0 = time.monotonic()
            try:
                r = bpy.ops.render.render(write_still=True, scene=F12_NAME)
            finally:
                cap.restore()
                rf.shutdown()
            seconds = time.monotonic() - t0
            if r != {"FINISHED"} or cap.arrays is None:
                raise RuntimeError(f"F12 on {device} returned {r}, session read {cap.arrays is not None}")
            exr = bpy.data.images.load(s.filepath)
            exr.colorspace_settings.name = "Non-Color"
            rgb = np.array(exr.pixels[:], np.float32).reshape(F12_SIZE, F12_SIZE, 4)[:, :, :3]
            bpy.data.images.remove(exr)
            planes = np.moveaxis(rgb, 2, 0).astype(np.float64)  # (3, h, w), rows bottom-up
            analytic.write_png(os.path.join(evidence, f"f12_{device.lower()}.png"), planes, 1.0)
            luma = float(np.tensordot(np.array(F12_LUMA), planes, axes=1).mean())
            cls.runs[device.lower()] = {"mean": planes, "var": cap.arrays[1].astype(np.float64).transpose(2, 0, 1),
                                        "luma": luma, "seconds": seconds}
            print(f"RESULT smoke f12 {device.lower()} mean {luma:.6f} floor {F12_FLOOR:.6f} time {seconds:.1f}")

    def _floor(self, device):
        self.assertGreater(self.runs[device]["luma"], F12_FLOOR,
                           f"{device}: the frame is black or nearly so (mean luminance {self.runs[device]['luma']})")

    def test_cpu_not_black(self):
        self._floor("cpu")

    def test_cuda_not_black(self):
        self._floor("cuda")

    def test_cpu_cuda_agree(self):
        """The two devices' image means differ by no more than the pair_image check's bound (pixel means
        are independent, so the difference has variance VAR_cpu + VAR_cuda; tools/analytic.py)."""
        m, w = analytic.family_m(), np.array(F12_LUMA)
        renders = {k: (v["mean"], v["var"]) for k, v in self.runs.items()}
        (name, passed, reasons, worst, cmean, tmean), = analytic.check_scene(
            None, None, [("f12 cpu~cuda", w, ("cpu", "cuda"), "pair_image")], m, 0.0, renders=renders)
        (_, va), (_, vb) = (analytic.channel(w, *renders[k]) for k in ("cpu", "cuda"))
        se = math.sqrt(va.sum() + vb.sum()) / va.size
        bound = analytic.pixel_z(m, 1) * se
        print(f"RESULT smoke f12 agree: {'ok' if passed else 'FAIL'} cpu {cmean:.6f} cuda {tmean:.6f} "
              f"|diff| {abs(cmean - tmean):.6f} bound {bound:.6f} SE of the difference {se:.6f}")
        self.assertTrue(passed, f"{name}: {reasons}")
        self.assertLessEqual(abs(cmean - tmean), bound)


def run_materials():
    spec = importlib.util.spec_from_file_location("test_materials", os.path.join(ROOT, "tests", "blender",
                                                                                   "test_materials.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules["test_materials"] = mod
    spec.loader.exec_module(mod)
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromModule(mod))
    return res.wasSuccessful()


if __name__ == "__main__":
    ok = True
    if "--with-materials" in ARGS:
        ok = run_materials()
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]  # as unittest's own -k
    suite = unittest.TestSuite(loader.loadTestsFromTestCase(c) for c in (TestSmoke, TestF12Textured))
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
    sys.exit(0 if ok and res.wasSuccessful() else 1)
