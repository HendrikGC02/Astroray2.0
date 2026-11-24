# SPDX-License-Identifier: Apache-2.0
"""The DLL against its generated binding (0001 V1): the real binding passes,
a binding with an edited hash or a swapped field pair is refused."""
import ctypes
import importlib.util
import os
import re
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ABI = os.path.join(ROOT, "out", "build", "gen", "_abi.py")


def load_binding(path):
    spec = importlib.util.spec_from_file_location("_abi_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestMatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, ABI):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.lib = ctypes.CDLL(DLL)
        with open(ABI, encoding="utf-8") as f:
            cls.source = f.read()

    def edited(self, text):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        path = os.path.join(d.name, "_abi.py")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return load_binding(path)

    def test_real_binding_passes(self):
        load_binding(ABI).check(self.lib)

    def test_hash_edit_refused(self):
        real = re.search(r"^ABI_HASH = (0x[0-9a-f]+)$", self.source, re.M).group(1)
        fake = "0x%016x" % (int(real, 16) ^ 1)
        binding = self.edited(self.source.replace(f"ABI_HASH = {real}", f"ABI_HASH = {fake}", 1))
        with self.assertRaises(binding.AbiMismatch) as cm:
            binding.check(self.lib)
        self.assertIn(real, str(cm.exception))
        self.assertIn(fake, str(cm.exception))

    def test_swapped_field_refused(self):
        # Two fields of one struct exchanged in the binding: same size, other offsets.
        a = '        ("device_kind", ctypes.c_uint32),\n'
        b = '        ("log_path", ctypes.c_char_p),\n'
        self.assertIn(a, self.source)
        self.assertIn(b, self.source)
        text = self.source.replace(a, "@@A@@").replace(b, a).replace("@@A@@", b)
        binding = self.edited(text)
        with self.assertRaises(binding.AbiMismatch) as cm:
            binding.check(self.lib)
        self.assertRegex(str(cm.exception), r"ar_context_desc\.\w+: offset")

    def test_medium_surface_in_binding(self):
        # The binding names the medium entry point and descriptor, and the DLL exports the function.
        binding = load_binding(ABI)
        self.assertIn("ar_edit_set_medium", binding.PROTOTYPES)
        self.assertTrue(hasattr(self.lib, "ar_edit_set_medium"))
        fields = [n for n, _ in binding.ar_medium_desc._fields_]
        self.assertEqual(fields, ["sigma_a", "sigma_s", "g", "flags"])
        self.assertIn("medium_keys", [n for n, _ in binding.ar_instances_desc._fields_])


if __name__ == "__main__":
    unittest.main()
