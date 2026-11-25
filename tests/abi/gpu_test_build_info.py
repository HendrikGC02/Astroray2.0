# SPDX-License-Identifier: Apache-2.0
"""The build record names the toolkits of a GPU build (0009 d5). Outside the cpu `abi tests` step's test_*.py
discovery because it holds only for the GPU preset; the local `gpu abi tests` gate step runs gpu_test_*.py."""
import os
import re
import subprocess
import tomllib
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
CACHE = os.path.join(ROOT, "out", "build", "CMakeCache.txt")


class BuildInfo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (CLI, CACHE):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")

    def test_toolkits_are_named(self):
        r = subprocess.run([CLI, "info"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        # The CLI prints the two GPU toolkits ("built with cuda=.. optix=.."); embree and oidn are in the log line
        # that test_context.py checks.
        line = r.stdout + r.stderr
        fields = dict(re.findall(r"(cuda|optix)=([\w.]+)", line))
        self.assertEqual(sorted(fields), ["cuda", "optix"], line)
        for k, v in fields.items():
            self.assertRegex(v, r"^\d+(\.\d+)+$", f"{k} in {line}")
        with open(os.path.join(ROOT, "third_party", "deps.toml"), "rb") as f:
            pins = {d["name"]: d["version"] for d in tomllib.load(f)["dep"]}
        self.assertEqual(fields["optix"], pins["optix-headers"])
        # cuda is the release of the nvcc the build recorded.
        with open(CACHE, encoding="utf-8") as f:
            m = re.search(r"^CMAKE_CUDA_COMPILER:FILEPATH=(.+)$", f.read(), re.M)
        self.assertTrue(m, "no CMAKE_CUDA_COMPILER in CMakeCache.txt: not a GPU build")
        out = subprocess.run([m.group(1), "--version"], capture_output=True, text=True, timeout=60).stdout
        rel = re.search(r"release (\d+\.\d+)", out)
        self.assertTrue(rel, out)
        self.assertEqual(fields["cuda"], rel.group(1))


if __name__ == "__main__":
    unittest.main()
