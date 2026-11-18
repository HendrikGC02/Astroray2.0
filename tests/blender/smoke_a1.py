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
Ref: docs/decisions/0009-build-test-process.md d11, d16, d17a; 0007 d27.
"""
import glob
import importlib.util
import os
import struct
import subprocess
import sys
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
                    for q in (a.AR_QUANTITY_MEAN, a.AR_QUANTITY_VARIANCE))
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
    install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend)
    scene = bpy.context.scene
    scene["astroray_device"] = "CPU"
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
    mean, var = (np.moveaxis(a, 2, 0).astype(np.float64) for a in cap.arrays)
    rr = np.array(exr.pixels[:], np.float32).reshape(SIZE, SIZE, 4)[:, :, :3]
    write_fits(os.path.join(evidence, "a1_f12_combined.fits"),
               {**{f"display.{c}": mean[c] for c in range(3)}, **{f"display.{c}.VAR": var[c] for c in range(3)},
                **{f"display.{c}.NSAMP": np.full((SIZE, SIZE), analytic.MIN_SPP) for c in range(3)}},
               {"SPP": analytic.MIN_SPP, "SEED": scene.astroray.seed, "SAMPLER": "sobol", "DEVICE": "cpu"})
    dumps = {}
    for name, path in (("addon", rec), ("builder", os.path.join(work, "a1_builder.arrec"))):
        out = os.path.join(work, f"a1_{name}.fits")
        p = subprocess.run([CLI, "dump", path, "-o", out, "--device", "cpu", "--seed", "7", "--spp", "4",
                            "--size", f"{SIZE}x{SIZE}"], capture_output=True, text=True, timeout=300)
        if p.returncode != 0:
            raise RuntimeError(f"astroray_cli dump {path} exited {p.returncode}:\n{p.stdout}{p.stderr}")
        dumps[name] = out
    return {"mean": mean, "var": var, "result": rr, "png": png, "dumps": dumps}


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

    def test_exporter_equivalence(self):
        ok, lines = exporter_equivalence.compare(self.state["dumps"]["addon"], self.state["dumps"]["builder"])
        for ln in lines:
            print("RESULT smoke " + ln)
        self.assertTrue(ok, "\n".join(lines))


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
        loader.testNamePatterns = [ARGS[ARGS.index("-k") + 1]]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestSmoke))
    sys.exit(0 if ok and res.wasSuccessful() else 1)
