# SPDX-License-Identifier: Apache-2.0
"""astroray_cli info (0001 D1.4, D2.8, V6 CLI part)."""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
TMP = tempfile.TemporaryDirectory()  # the CLI writes its context log into its working directory


def run(args, **env):
    e = dict(os.environ)
    e.update(env)
    return subprocess.run([CLI] + args, capture_output=True, text=True, env=e, cwd=TMP.name, timeout=60)


class TestCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(CLI):
            raise AssertionError(f"{CLI} missing: run tools/build.ps1 first")

    def test_info_lists_cpu(self):
        r = run(["info"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(r.stdout.startswith("build: sha="), r.stdout)
        lines = [ln for ln in r.stdout.splitlines() if ln.startswith("0 ")]
        self.assertEqual(len(lines), 1, r.stdout)
        self.assertIn(" cpu ", lines[0])
        self.assertIn("status=AR_OK", lines[0])

    def test_no_cuda_no_fallback(self):
        r = run(["info"], CUDA_VISIBLE_DEVICES="-1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("no CUDA device", r.stdout)
        r = run(["info", "--create", "cuda"], CUDA_VISIBLE_DEVICES="-1")
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("AR_ERR_DEVICE", r.stdout)
        self.assertNotIn("context ok", r.stdout)

    def test_create_cpu(self):
        r = run(["info", "--create", "cpu"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("context ok", r.stdout)


if __name__ == "__main__":
    unittest.main()
