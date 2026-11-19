# SPDX-License-Identifier: Apache-2.0
"""render_final's user directories outside an installed extension (0007 d45).

test_images loads render_final under a plain package and replaces only _log_path; the OptiX cache
directory must follow it, or every F12 update raises from bpy.utils.extension_path_user."""
import importlib
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

_PKG = "ar_rf_addon"


def load_render_final():
    """The addon's render_final without its __init__ (which needs the engine DLL); native loads the DLL
    lazily, so importing it needs no build."""
    if _PKG not in sys.modules:
        pkg = types.ModuleType(_PKG)
        pkg.__path__ = [os.path.join(support.ROOT, "blender", "astroray")]
        sys.modules[_PKG] = pkg
    return importlib.import_module(_PKG + ".render_final")


class TestUserDirs(unittest.TestCase):
    def test_cache_dir_follows_the_replaced_log_path(self):
        rf = load_render_final()
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "context.log")
            with mock.patch.object(rf, "_log_path", lambda: log):
                self.assertEqual(rf.cache_dir(), d)


if __name__ == "__main__":
    support.main()
