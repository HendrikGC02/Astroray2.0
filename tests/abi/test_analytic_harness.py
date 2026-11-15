# SPDX-License-Identifier: Apache-2.0
"""The analytic harness (tools/analytic.py, 0009 d16) and the sampler mode it relies on.

TestHarness: synthetic dumps; a pixel offset by 10 SE fails, a constant image where truth varies
fails, a black image fails, the exact truth passes; thresholds and seeds follow task 020's rules.
TestSamplerMode (CPU, through astroray_cli dump): a recording in independent mode writes
SAMPLER = 'independent' and its per-pixel z spread over 1024 pixels is that of 1024 standard
normal draws, while Sobol mode, whose variance is conservative (0008 d3), writes 'sobol' and falls
below that spread; an out-of-range sampler mode is refused at commit (ruling 60).
"""
import ctypes
import importlib.util
import os
import statistics
import subprocess
import sys
import tempfile
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


harness = _load("_analytic_under_test", os.path.join(ROOT, "tools", "analytic.py"))

N = 32
Z = harness.pixel_z(256, N * N)


def gradient():
    y, x = np.mgrid[0:N, 0:N]
    return 0.5 + (x + 2 * y) / (3.0 * N)


class TestHarness(unittest.TestCase):
    def setUp(self):
        self.truth = gradient()
        self.var = np.full((N, N), 1e-4)  # SE 0.01

    def test_exact_truth_passes(self):
        ok, reasons, worst = harness.check_channel(self.truth.copy(), self.var, self.truth, Z, 0.0)
        self.assertTrue(ok, reasons)
        self.assertEqual(worst[0], 0.0)

    def test_offset_pixel_fails(self):
        mean = self.truth.copy()
        mean[7, 19] += 10 * 0.01
        ok, reasons, worst = harness.check_channel(mean, self.var, self.truth, Z, 0.0)
        self.assertFalse(ok)
        self.assertIn("1 pixels outside z SE + eps_sys", reasons)
        self.assertAlmostEqual(worst[0], 10.0, places=6)
        self.assertEqual(worst[1], (19, 7))

    def test_constant_where_truth_varies_fails(self):
        # The variance is large enough that every pixel passes the z test on its own.
        mean = np.full((N, N), self.truth.mean())
        ok, reasons, _ = harness.check_channel(mean, np.full((N, N), 1.0), self.truth, Z, 0.0)
        self.assertFalse(ok)
        self.assertEqual(reasons, ["constant where truth varies"])

    def test_black_fails(self):
        mean = np.zeros((N, N))
        ok, reasons, _ = harness.check_channel(mean, np.full((N, N), 1.0), self.truth, Z, 0.0)
        self.assertFalse(ok)
        self.assertIn("black where truth is not", reasons)

    def test_white_point_error_fails(self):
        # A white emitter rendered exactly (display RGB 1) with a y_anchor-like SE of 0.005: the
        # literal truth Y = 1 passes, and a luminance row (M_ws normalisation) off by 1% fails the
        # image-mean check, whose statistical term is about 1e-3 against the 1e-2 error.
        truths = harness.truths
        table = {"formula": "display_luminance", "colour": [1.0, 1.0, 1.0], "radiance": 1.0}
        mean, var = np.ones((3, N, N)), np.full((3, N, N), 2.5e-5)
        good = harness.check_scene(mean, var, truths.evaluate(table, N, N), 256, 1e-3)
        self.assertTrue(all(r[1] for r in good), good)
        row = truths.working_luminance_row
        truths.working_luminance_row = lambda: 1.01 * row()
        try:
            bad = harness.check_scene(mean, var, truths.evaluate(table, N, N), 256, 1e-3)
        finally:
            truths.working_luminance_row = row
        image = [r for r in bad if r[0] == "Y image mean"]
        self.assertEqual(len(image), 1)
        self.assertFalse(image[0][1], image)

    def test_spp_below_calibrated_minimum_is_refused(self):
        harness.check_spp(1024)
        with self.assertRaises(ValueError):
            harness.check_spp(512)

    def test_thresholds_and_seeds_match_task_020(self):
        # tests/cpu/test_stat_harness.cpp pins z = 4.5637347874563305 for m = 2000 and the seed of
        # "stat_harness/a"; the harness uses the same rules (ar_settings.seed takes the low 32 bits).
        self.assertAlmostEqual(harness.pixel_z(2000, 1), 4.5637347874563305, places=8)
        self.assertEqual(harness.seed_for("stat_harness/a"), 4808046070232382604 & 0xFFFFFFFF)
        self.assertAlmostEqual(Z, 5.50, places=2)


def max_abs_z_bounds(n, alpha):
    """Two-sided acceptance range of max |z| over n independent standard normals at size alpha:
    P(max < a) = (2 Phi(a) - 1)^n, alpha / 2 in each tail."""
    inv = statistics.NormalDist().inv_cdf
    lo = inv((1.0 + (alpha / 2) ** (1.0 / n)) / 2.0)
    hi = inv((1.0 + (1.0 - alpha / 2) ** (1.0 / n)) / 2.0)
    return lo, hi


class TestSamplerMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(CLI):
            raise AssertionError(f"{CLI} missing: run tools/build.ps1 first")
        cls.ar = _load("_ar_sampler_test", os.path.join(ROOT, "scenes", "lib", "ar.py"))
        cls.meshes = _load("_meshes_sampler_test", os.path.join(ROOT, "scenes", "lib", "meshes.py"))
        cls.fits = _load("_fits_sampler_test", os.path.join(ROOT, "tests", "abi", "fits_read.py"))
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def emitter(self, s, sampler):
        """A5's view-filling emitter (radiance 2) at 32x32 with the given sampler mode."""
        mesh = s.key()
        s.mesh_from(mesh, self.meshes.plane(4.0, 4.0), "emitter")
        mat = s.material({"base_color": (0.0, 0.0, 0.0), "specular_weight": 0.0}, emission=((1.0, 1.0, 1.0), 2.0))
        s.instances([{"geom": mesh, "materials": [mat]}])
        s.world((0.0, 0.0, 0.0), 0.0)
        s.settings(64, N, N, seed=7, sampler=sampler)
        s.camera((0.0, 0.0, 2.0), (0.0, 0.0, 0.0), 50.0, 36.0, N, N, up=(0.0, 1.0, 0.0))

    def render(self, sampler, spp=1024):
        rec = os.path.join(self.tmp.name, f"{sampler}.arrec")
        with self.ar.Scene(rec) as s:
            self.emitter(s, sampler)
            s.finish()
        out = os.path.join(self.tmp.name, f"{sampler}.fits")
        r = subprocess.run([CLI, "dump", rec, "-o", out, "--device", "cpu", "--spp", str(spp)],
                           capture_output=True, text=True, cwd=self.tmp.name, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(out, "rb") as f:
            hdus = self.fits.read_hdus(f.read())
        head, planes = hdus[0][0], {h["EXTNAME"]: d for h, d in hdus[1:]}
        maxz = []
        for c in range(3):
            mean = planes[f"display.{c}"].astype(np.float64)
            se = np.sqrt(planes[f"display.{c}.VAR"].astype(np.float64))
            self.assertTrue((se > 0).all())
            maxz.append(float(np.max(np.abs(mean - 2.0) / se)))
        return head["SAMPLER"], maxz

    def test_independent_mode_variance_is_exact(self):
        lo, hi = max_abs_z_bounds(N * N, harness.sidak(harness.FAMILY_ALPHA, harness.family_m()))
        sampler, maxz = self.render("independent")
        print(f"independent mode: max |z| per channel {['%.2f' % z for z in maxz]}, accepted [{lo:.2f}, {hi:.2f}]")
        self.assertEqual(sampler, "independent")
        for c, z in enumerate(maxz):
            self.assertTrue(lo <= z <= hi, f"channel {c}: max |z| {z} outside [{lo}, {hi}]")

    def test_sobol_mode_is_named_and_conservative(self):
        lo, _ = max_abs_z_bounds(N * N, harness.sidak(harness.FAMILY_ALPHA, harness.family_m()))
        sampler, maxz = self.render("sobol", spp=64)
        self.assertEqual(sampler, "sobol")
        self.assertLess(max(maxz), lo, maxz)

    def test_out_of_range_sampler_mode_is_refused(self):
        with self.ar.Scene(os.path.join(self.tmp.name, "refused.arrec")) as s:
            self.emitter(s, "sobol")
            a = s.abi
            st = a.ar_settings()
            st.samples = st.viewport_samples = 4
            st.output_width, st.output_height, st.resolution_percent = N, N, 100
            st.max_bounces, st.pixel_filter_width, st.film_exposure = 8, 1.0, 1.0
            st.sampler_mode = a.AR_SAMPLER_INDEPENDENT + 1
            self.assertEqual(s.lib.ar_edit_set_settings(s.edit_h, ctypes.byref(st)), a.AR_OK)
            gen = ctypes.c_uint64()
            self.assertEqual(s.lib.ar_edit_commit(s.edit_h, ctypes.byref(gen)), a.AR_ERR_INVALID_ARGUMENT)
            self.assertIn("sampler_mode out of range", s._last_error())


if __name__ == "__main__":
    unittest.main()
