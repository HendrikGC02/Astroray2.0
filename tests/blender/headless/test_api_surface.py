# SPDX-License-Identifier: Apache-2.0
"""0007 d46: the API-surface check names what disappeared."""
import contextlib
import importlib.util
import io
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import support  # noqa: E402
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

spec = importlib.util.spec_from_file_location("api_surface", os.path.join(ROOT, "tools", "api_surface.py"))
api_surface = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api_surface)


def run(lines, addon_passes=None):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        result = api_surface.check(lines, addon_passes)
    return result, out.getvalue()


class TestApiSurface(unittest.TestCase):
    def test_missing_attribute_named(self):
        invented = "attr Mesh.astroray_invented_attribute"
        (checked, missing, _), out = run(["attr Mesh.vertices", invented, "attr Window.astroray_invented"])
        self.assertEqual(checked, 3)
        self.assertEqual(missing, [invented, "attr Window.astroray_invented"])
        self.assertIn("MISSING " + invented, out)
        self.assertIn("OK attr Mesh.vertices", out)

    def test_missing_indexed_path_named(self):
        line = "attr Mesh.attributes['.astroray_no_such'].data.foreach_get"
        (_, missing, _), out = run([line])
        self.assertEqual(missing, [line])
        self.assertIn("MISSING " + line, out)

    def test_listed_surface_is_present(self):
        with open(api_surface.LIST, encoding="utf-8") as f:
            (checked, missing, _), _ = run(f.read().splitlines(), api_surface._addon_pass_flags())
        self.assertGreater(checked, 50)
        self.assertEqual(missing, [])

    def test_unlisted_addon_pass_named(self):
        (_, missing, _), out = run(["pass use_pass_z Depth"], addon_passes=[("use_pass_z", "Depth"), ("use_pass_ao", "AO")])
        self.assertEqual(missing, ["pass use_pass_ao AO"])


if __name__ == "__main__":
    support.main()
