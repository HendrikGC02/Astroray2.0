# SPDX-License-Identifier: Apache-2.0
"""astroray_cli render on a CUDA session (task 115). Needs a CUDA device, so the name is outside the
cpu `abi tests` step's test_*.py discovery; the local `gpu abi tests` gate step runs gpu_test_*.py.
A5 (scenes/a5_emitter): camera rays traced by the session's OptiX pipeline hit the view-filling
emitter, so every channel's image mean is L within 5 standard errors (0009 d20)."""
import importlib.util
import os
import subprocess
import tempfile
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")
REC = os.path.join(ROOT, "out", "scenes", "a5_emitter.arrec")
RADIANCE = 2.0  # scenes/a5_emitter/meta.toml [truth]: colour (1, 1, 1) times radiance 2


class TestCliRenderCuda(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (CLI, REC):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 and tools/check.ps1 (scenes) first")
        spec = importlib.util.spec_from_file_location("_fits_read_gpu_cli", FITS_READ)
        cls.fits = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.fits)
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_cuda_a5(self):
        out = os.path.join(self.tmp.name, "a5_cuda.fits")
        r = subprocess.run([CLI, "render", REC, "-o", out, "--device", "cuda", "--spp", "4", "--size", "32x32"],
                           capture_output=True, text=True, cwd=self.tmp.name, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        with open(out, "rb") as f:
            hdus = self.fits.read_hdus(f.read())
        head, planes = hdus[0][0], {h["EXTNAME"]: d for h, d in hdus[1:]}
        self.assertEqual(head["SPP"], 4)
        for c in range(3):
            mean, var, n = planes[f"display.{c}"], planes[f"display.{c}.VAR"], planes[f"display.{c}.NSAMP"]
            self.assertEqual(mean.shape, (32, 32))
            self.assertTrue((n == 4).all(), n)
            # The image mean of independent pixel means; VAR is each pixel mean's variance.
            m = float(mean.astype(np.float64).mean())
            se = float(np.sqrt(var.astype(np.float64).sum())) / mean.size
            z = (m - RADIANCE) / se if se > 0 else float("inf")
            print(f"cli render cuda a5: channel {c} mean {m:.6f} (L {RADIANCE}), se {se:.2e}, z {z:+.2f}")
            self.assertLessEqual(abs(m - RADIANCE), 5.0 * se, f"channel {c}: mean {m}, se {se}")


if __name__ == "__main__":
    unittest.main()
