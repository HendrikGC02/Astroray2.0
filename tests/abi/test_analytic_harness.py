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

    def test_excluded_pixels_are_not_tested(self):
        # A NaN truth excludes its pixel: a wild value there passes, the same value on a tested
        # pixel fails, and a truth that excludes everything fails.
        truth = self.truth.copy()
        truth[3, 4] = np.nan
        mean = self.truth.copy()
        mean[3, 4] = 1e6
        ok, reasons, _ = harness.check_channel(mean, self.var, truth, Z, 0.0)
        self.assertTrue(ok, reasons)
        mean[3, 5] = 1e6
        ok, _, worst = harness.check_channel(mean, self.var, truth, Z, 0.0)
        self.assertFalse(ok)
        self.assertEqual(worst[1], (5, 3))
        ok, reasons, _ = harness.check_channel(mean, self.var, np.full((N, N), np.nan), Z, 0.0)
        self.assertFalse(ok)
        self.assertEqual(reasons, ["the truth excludes every pixel"])

    def test_lambertian_furnace_truth(self):
        # A1 (0009 d20): rho Lw on pixels whose rays all hit the mesh, Lw on pixels whose rays all
        # miss the sphere, the silhouette's pixels excluded; the classes are a disc, symmetric
        # about the image centre, with both kinds of tested pixel present.
        t = harness.meta.load(os.path.join(ROOT, "scenes", "a1_furnace", "meta.toml"))["truth"]
        checks = harness.truths.evaluate(t, N, N)
        self.assertEqual([c[0] for c in checks], ["R", "G", "B"])
        truth = checks[0][2]
        rho_lw, lw = t["rho"] * t["world_radiance"], t["world_radiance"]
        excluded = np.isnan(truth)
        self.assertTrue(set(np.unique(truth[~excluded])) == {rho_lw, lw})
        self.assertTrue((truth[N // 2 - 1:N // 2 + 1, N // 2 - 1:N // 2 + 1] == rho_lw).all())
        self.assertTrue((truth[0, :] == lw).all())
        self.assertTrue(0 < excluded.sum() < (truth == rho_lw).sum())
        np.testing.assert_array_equal(excluded, excluded[::-1, ::-1])
        # The exact truth passes whatever the excluded pixels hold; the sphere at twice rho Lw fails.
        mean = np.where(excluded, 0.6, truth)[None].repeat(3, 0)
        var = np.full((3, N, N), 1e-6)
        self.assertTrue(all(r[1] for r in harness.check_scene(mean, var, checks, 256, 0.0)))
        bad = np.where(truth == rho_lw, 2 * rho_lw, mean[0])[None].repeat(3, 0)
        self.assertFalse(any(r[1] for r in harness.check_scene(bad, var, checks, 256, 0.0)))

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


class TestOrderCentroids(unittest.TestCase):
    """The `order_centroids` kind (V8's order angles) on synthetic profiles: three channels of one
    order, a bump of 1 degree width on a 1024-column axis of 60 degrees."""
    W = 1024
    THETA = np.linspace(0.0, 60.0, W)

    def spec(self, order, centre, tol=0.1):
        bump = np.stack([np.exp(-0.5 * ((self.THETA - centre - s) / 0.5) ** 2) for s in (0.0, 0.3, -0.3)])
        return {"order": order, "theta_deg": self.THETA, "windows": [(0, self.W)] * 3, "predicted": bump,
                "tolerance_deg": tol}

    def render(self, order, centre, shifts):
        """Render planes whose channel c is the prediction's bump moved by shifts[c] degrees."""
        mean = np.stack([np.exp(-0.5 * ((self.THETA - centre - s - o) / 0.5) ** 2)
                         for s, o in zip((0.0, 0.3, -0.3), shifts)])[:, None, :].repeat(16, axis=1)
        return mean, np.full(mean.shape, 1e-8)  # SE of a centroid about 1e-4 degrees

    def test_exact_prediction_passes(self):
        mean, var = self.render(1, 20.0, (0.0, 0.0, 0.0))
        ok, reasons, _, lines, biggest = harness.order_centroid_rows(mean, var, self.spec(1, 20.0), Z)
        self.assertTrue(ok, reasons)
        self.assertEqual(len(lines), 3)
        self.assertLess(biggest, 1e-6)

    def test_shifted_render_fails(self):
        # A grating-equation error moves an order's image: 0.3 degrees is three times V8's tolerance.
        mean, var = self.render(1, 20.0, (0.0, 0.3, 0.0))
        ok, reasons, worst, _, biggest = harness.order_centroid_rows(mean, var, self.spec(1, 20.0), Z)
        self.assertFalse(ok)
        self.assertEqual(len(reasons), 1)
        self.assertIn("G:", reasons[0])
        self.assertAlmostEqual(biggest, 0.3, places=3)
        self.assertEqual(worst[1][0], 1)

    def test_noise_widens_the_tolerance_by_z_se(self):
        # |difference| <= tolerance + z SE: 0.15 degrees fails when the SE is tiny and passes when
        # z SE exceeds 0.05 degrees (variance 4e-6 gives SE about 0.015 here, z SE about 0.06 <= 0.1).
        mean, var = self.render(1, 20.0, (0.15, 0.0, 0.0))
        self.assertFalse(harness.order_centroid_rows(mean, var, self.spec(1, 20.0), Z)[0])
        noisy = np.full(mean.shape, 4e-6)
        ok, reasons, _, lines, _ = harness.order_centroid_rows(mean, noisy, self.spec(1, 20.0), Z)
        self.assertTrue(ok, reasons)

    def test_noise_guard_refuses_a_blind_row(self):
        # z SE above the tolerance refuses even an exact render: the test would read beyond 0.2 degrees.
        mean, _ = self.render(1, 20.0, (0.0, 0.0, 0.0))
        blind = np.full(mean.shape, 1e-2)
        ok, reasons, _, _, _ = harness.order_centroid_rows(mean, blind, self.spec(1, 20.0), Z)
        self.assertFalse(ok)
        self.assertTrue(any("z SE" in r and "exceeds" in r for r in reasons))

    def test_black_window_fails_instead_of_raising(self):
        # An engine that drops an order renders black there: a FAIL line naming it, not a division by zero.
        mean, var = self.render(1, 20.0, (0.0, 0.0, 0.0))
        mean[1] = 0.0
        ok, reasons, _, lines, _ = harness.order_centroid_rows(mean, var, self.spec(1, 20.0), Z)
        self.assertFalse(ok)
        self.assertTrue(any("G:" in r and "order dropped" in r for r in reasons))
        self.assertEqual(len(lines), 3)

    def test_order_zero_must_be_achromatic(self):
        # Every channel is within the tolerance of the prediction (0.09 degrees at most) but the
        # channels disagree by 0.18 degrees: only order 0 asserts they agree.
        for order, achromatic in ((0, False), (1, True)):
            mean, var = self.render(order, 20.0, (-0.09, 0.0, 0.09))
            ok, reasons, _, _, _ = harness.order_centroid_rows(mean, var, self.spec(order, 20.0), Z)
            self.assertEqual(ok, achromatic, reasons)
            if not achromatic:
                self.assertTrue(any("achromatic" in r for r in reasons))

    def test_cancelling_profile_is_refused(self):
        spec = self.spec(1, 20.0)
        spec["predicted"] = spec["predicted"] * np.where(self.THETA < 20.0, -1.0, 1.0)
        mean, var = self.render(1, 20.0, (0.0, 0.0, 0.0))
        with self.assertRaises(ValueError):
            harness.order_centroid_rows(mean, var, spec, Z)

    def test_rectangular_scene_plan(self):
        # V8's strip: [truth] width x height and its own spp, no calibrated-minimum refusal.
        t = harness.meta.load(os.path.join(ROOT, "scenes", "v8_grating_card", "meta.toml"))["truth"]
        self.assertEqual(harness.comparison_plan(t), ((1024, 16), int(t["spp"]), 1))
        self.assertEqual(harness.size_wh(harness.plan_size(t)), (1024, 16))
        self.assertEqual(harness.size_wh(32), (32, 32))
        checks = harness.truths.evaluate(t, 1024, 16)
        self.assertEqual([c[3] for c in checks], ["order_centroids"] * 3 + ["pixels"] * 3)
        self.assertEqual(checks[3][2].shape, (16, 1024))


class TestGratingPredictor(unittest.TestCase):
    """truths.py's V8 predictor against closed forms and limits, never against the engine."""

    def test_bessel_integral_matches_recurrence(self):
        t = harness.truths
        for m in (0, 1, 2, 3):
            for x in (0.3, 1.7, 3.05, 5.9):
                self.assertAlmostEqual(float(t.bessel_j_integral(m, x)), t.bessel_j_recurrence(m, x), places=12)
        # J_0(2.404825557695773) = 0, the first zero (Abramowitz and Stegun table 9.5).
        self.assertAlmostEqual(float(t.bessel_j_integral(0, 2.404825557695773)), 0.0, places=12)

    def test_fresnel_at_normal_incidence(self):
        # R = ((n - 1)^2 + k^2) / ((n + 1)^2 + k^2) at normal incidence, whatever the polarisation.
        n, k = 1.0, 6.0
        want = ((n - 1) ** 2 + k * k) / ((n + 1) ** 2 + k * k)
        self.assertAlmostEqual(float(harness.truths.fresnel_conductor_mean(1.0, n, k)), want, places=12)
        self.assertAlmostEqual(float(harness.truths.fresnel_conductor_mean(0.0, n, k)), 1.0, places=12)

    def test_v8_angles(self):
        got = harness.truths.v8_selfcheck()
        self.assertAlmostEqual(got[0][1], 22.31, places=2)
        self.assertAlmostEqual(got[1][1], 49.41, places=2)
        self.assertIsNone(got[2][1])

    def test_lamp_image_in_the_far_lamp_limit_is_the_grating_equation(self):
        # A lamp at 1e7 m straight above and a camera 1 m above the card: the card point from which the
        # lamp's centre is seen in order m is where the camera direction is asin(m lambda / d), and the
        # camera angle atan2(-P_y, h) must be that angle.
        t = harness.truths
        for lam in (450.0, 632.8):
            for m in (1, 2):
                want = t.grating_order_angle_deg(m, lam, 1e6 / 600.0)
                if want is None:
                    continue
                p_y = t._lamp_image_edges(m, lam, 1e6 / 600.0, 1.0, 1e7, 0.0)
                self.assertAlmostEqual(float(np.degrees(np.arctan2(-p_y, 1.0))), want, places=4)

    def test_order_zero_is_the_mirror(self):
        # Order 0, camera 0.2 m up, lamp 60 m up: by the image method the mirror point of a lamp point
        # (e, 60) for the camera at (0, 0.2) is where the line from the camera's mirror image (0, -0.2)
        # to the lamp point crosses the card, y = e 0.2 / 60.2.
        t = harness.truths
        for e in (0.0, 0.525, -0.525):
            got = float(t._lamp_image_edges(0, 550.0, 1e6 / 600.0, 0.2, 60.0, e))
            self.assertAlmostEqual(got, e * 0.2 / 60.2, places=9)


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
