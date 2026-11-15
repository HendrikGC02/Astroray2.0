# SPDX-License-Identifier: Apache-2.0
"""Installs the built extension zip into the isolated profile and checks identity (0007 V1, V16).

Run through Invoke-ArBlender. The mismatch case runs in a second Blender
process with its own extension directory so no module state leaks between cases.
"""
import glob
import os
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile

import bpy

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODULE = "bl_ext.user_default.astroray"


def zip_path():
    found = sorted(glob.glob(os.path.join(ROOT, "out", "extension", "astroray-*-windows_x64.zip")))
    if not found:
        raise RuntimeError("no extension zip; run tools/build.ps1 -Target astroray_extension")
    return found[-1]


def run_case(path, tmp):
    """Installs path with the extension CLI into a fresh profile, then enables it in a second process."""
    env = dict(os.environ)
    for k in ("config", "scripts", "extensions", "datafiles"):
        d = os.path.join(tmp, "profile", k)
        os.makedirs(d, exist_ok=True)
        env["BLENDER_USER_" + k.upper()] = d
    exe = bpy.app.binary_path
    inst = subprocess.run([exe, "--command", "extension", "install-file", "-r", "user_default", path],
                          env=env, capture_output=True, text=True, timeout=240)
    if inst.returncode != 0:
        raise RuntimeError("install-file failed: " + inst.stdout + inst.stderr)
    return subprocess.run(
        [exe, "--background", "--factory-startup", "--python-exit-code", "1",
         "--python", os.path.abspath(__file__), "--", "child"],
        env=env, capture_output=True, text=True, timeout=240)


def has_engine():
    # Registered classes only: an imported but unregistered subclass stays in __subclasses__.
    return bpy.types.RenderEngine.bl_rna_get_subclass_py("ASTRORAY") is not None


def child_main():
    import addon_utils

    addon_utils.modules_refresh()
    errors = []
    mod = addon_utils.enable(MODULE, default_set=False, handle_error=errors.append)
    print("CHILD-ENGINE: %s" % has_engine())
    print("CHILD-ERROR: %s" % (errors[0] if errors else None))
    sys.exit(0 if mod else 4)

class TestInstall(unittest.TestCase):
    def test_install_and_enable(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = run_case(zip_path(), tmp)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("CHILD-ENGINE: True", p.stdout)
        m = re.search(r"Astroray core \S+ git \S+.*", p.stdout)
        self.assertIsNotNone(m, "build line was not printed:\n" + p.stdout)
        print("RESULT install_and_enable: ASTRORAY registered; " + m.group(0))

    def test_mismatched_hash_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = os.path.join(tmp, "astroray-2.0.0-windows_x64.zip")
            with zipfile.ZipFile(zip_path()) as zin, zipfile.ZipFile(bad, "w", zipfile.ZIP_DEFLATED) as zout:
                for item in zin.infolist():
                    data = zin.read(item.filename)
                    if item.filename == "_abi.py":
                        text, n = re.subn(r"^ABI_HASH = 0x([0-9a-f]{16})$",
                                          lambda m: "ABI_HASH = 0x%016x" % (int(m.group(1), 16) ^ 1),
                                          data.decode("utf-8"), flags=re.M)
                        self.assertEqual(n, 1)
                        data = text.encode("utf-8")
                    zout.writestr(item, data)
            p = run_case(bad, tmp)
        self.assertNotEqual(p.returncode, 0, "registration was not refused:\n" + p.stdout)
        self.assertIn("CHILD-ENGINE: False", p.stdout)
        self.assertRegex(p.stdout, r"CHILD-ERROR: .*ABI hash mismatch: binding 0x[0-9a-f]{16}, library 0x[0-9a-f]{16}")
        print("RESULT mismatched_hash_refused: " + re.search(r"CHILD-ERROR: .*", p.stdout).group(0))

if "--" in sys.argv and sys.argv[sys.argv.index("--") + 1:][:1] == ["child"]:
    child_main()
else:
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(TestInstall))
    if not res.wasSuccessful():
        raise RuntimeError("test_install failed")
