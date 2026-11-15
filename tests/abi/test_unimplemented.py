# SPDX-License-Identifier: Apache-2.0
"""Declared functions without an implementation refuse loudly (0001 D2.8, task 031):
every prototype in the header is exported by the DLL, every stub listed in
src/capi/unimplemented.cpp is a declared function, and each stub returns AR_ERR_UNSUPPORTED."""
import ctypes
import importlib.util
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ABI = os.path.join(ROOT, "out", "build", "gen", "_abi.py")
STUBS = os.path.join(ROOT, "src", "capi", "unimplemented.cpp")


def load_binding(path):
    spec = importlib.util.spec_from_file_location("_abi_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def stub_names():
    with open(STUBS, encoding="utf-8") as f:
        return re.findall(r"^AR_API ar_status (\w+)\(", f.read(), flags=re.M)


def null_args(argtypes):
    """Zero for every integer parameter, NULL for every pointer or handle."""
    ints = (ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint64)
    return [0 if t in ints else None for t in argtypes]


class TestUnimplemented(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, ABI):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load_binding(ABI)
        cls.lib = ctypes.CDLL(DLL)
        cls.abi.check(cls.lib)
        cls.abi.bind(cls.lib)

    def test_curves_begin_unsupported(self):
        # Curves land in M7: the call refuses, it never pretends to stage anything.
        st = self.lib.ar_edit_curves_begin(None, None, None)
        self.assertEqual(st, self.abi.AR_ERR_UNSUPPORTED)

    def test_every_declared_function_is_exported(self):
        missing = [n for n in self.abi.PROTOTYPES if not hasattr(self.lib, n)]
        self.assertEqual(missing, [], "declared in astroray.h but not exported by the DLL")

    def test_stub_list_names_declared_functions(self):
        names = stub_names()
        # Every AR_API line must parse as a stub, so a regex that silently misses entries fails here;
        # a minimum count would instead break each time a task implements stubs.
        with open(STUBS, encoding="utf-8") as f:
            api_lines = len(re.findall(r"^AR_API\b", f.read(), flags=re.M))
        self.assertEqual(len(names), api_lines, "an AR_API line in unimplemented.cpp did not parse as a stub")
        self.assertEqual(len(names), len(set(names)), "a stub is listed twice")
        stale = [n for n in names if n not in self.abi.PROTOTYPES]
        self.assertEqual(stale, [], "listed in unimplemented.cpp but not declared in astroray.h")

    def test_every_stub_refuses_with_unsupported(self):
        a = self.abi
        for name in stub_names():
            with self.subTest(function=name):
                args = null_args(a.PROTOTYPES[name])
                self.assertEqual(getattr(self.lib, name)(*args), a.AR_ERR_UNSUPPORTED)

    def test_implemented_functions_are_not_stubs(self):
        # The identity and context functions have real bodies; a stub for one would shadow it.
        stubs = set(stub_names())
        for name in ("ar_abi_hash", "ar_abi_layout", "ar_build_info", "ar_device_count",
                     "ar_context_create", "ar_context_destroy", "ar_context_last_error"):
            self.assertNotIn(name, stubs)


if __name__ == "__main__":
    unittest.main()
