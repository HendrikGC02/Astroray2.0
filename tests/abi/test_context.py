# SPDX-License-Identifier: Apache-2.0
"""Devices and contexts through the generated binding (0001 D2.1, D2.5, D2.8)."""
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


def text(arr):
    return bytes(arr).split(b"\0")[0].decode()


class TestContext(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, ABI):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load_binding(ABI)
        cls.lib = ctypes.CDLL(DLL)
        cls.abi.check(cls.lib)
        cls.abi.bind(cls.lib)

    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.log_path = os.path.join(d.name, "context.log")

    def create(self, kind, index, log_path=None):
        desc = self.abi.ar_context_desc(kind, index, (self.log_path if log_path is None else log_path).encode(), None)
        ctx = ctypes.c_void_p()
        st = self.lib.ar_context_create(ctypes.byref(desc), ctypes.byref(ctx))
        return st, ctx

    def devices(self):
        n = ctypes.c_uint32()
        self.assertEqual(self.lib.ar_device_count(ctypes.byref(n)), self.abi.AR_OK)
        out = []
        for i in range(n.value):
            info = self.abi.ar_device_info()
            self.assertEqual(self.lib.ar_device_get_info(i, ctypes.byref(info)), self.abi.AR_OK)
            out.append(info)
        return out

    def test_device_list(self):
        devs = self.devices()
        for i, d in enumerate(devs):
            kind = "CPU" if d.kind == self.abi.AR_DEVICE_CPU else "CUDA"
            print(f"device {i}: {kind} index={d.index} status={d.status} name='{text(d.name)}' "
                  f"cc={d.compute_capability} memory={d.memory_bytes >> 20} MiB reason='{text(d.reason)}'")
        self.assertGreaterEqual(len(devs), 1)
        self.assertEqual(devs[0].kind, self.abi.AR_DEVICE_CPU)
        self.assertEqual(devs[0].status, self.abi.AR_OK)
        self.assertEqual(text(devs[0].reason), "")
        for i, d in enumerate(devs[1:]):
            self.assertEqual(d.kind, self.abi.AR_DEVICE_CUDA)
            self.assertEqual(d.index, i)
            self.assertTrue(text(d.name))
        info = self.abi.ar_device_info()
        n = len(devs)
        self.assertEqual(self.lib.ar_device_get_info(n, ctypes.byref(info)), self.abi.AR_ERR_INVALID_ARGUMENT)

    def test_cpu_context_log(self):
        st, ctx = self.create(self.abi.AR_DEVICE_CPU, 0)
        self.assertEqual(st, self.abi.AR_OK)
        self.assertTrue(ctx.value)
        buf = (ctypes.c_uint8 * 8)(*([0xFF] * 8))
        self.assertEqual(self.lib.ar_context_last_error(ctx, 8, buf), self.abi.AR_OK)
        self.assertEqual(text(buf), "")
        self.assertEqual(self.lib.ar_context_destroy(ctx), self.abi.AR_OK)
        with open(self.log_path, encoding="utf-8") as f:
            lines = f.read().splitlines()
        stamp = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z"
        self.assertTrue(lines)
        for ln in lines:
            self.assertRegex(ln, "^" + stamp + r" (INFO|WARNING|ERROR) \S")
        self.assertTrue(any(re.search(r" INFO build version=\S+ git=[0-9a-f]+ dirty=[01] cuda=\S+ optix=\S+ embree=\S+ oidn=\S+ type=\S+ kernels=", ln) for ln in lines), lines)
        self.assertTrue(any(" INFO device kind=cpu index=0 " in ln for ln in lines), lines)

    def test_last_error_text_and_truncation(self):
        a = self.abi
        st, ctx = self.create(a.AR_DEVICE_CPU, 0)
        self.assertEqual(st, a.AR_OK)
        try:
            # A rejected call on a live handle stores its message as the last error.
            self.assertEqual(self.lib.ar_context_last_error(ctx, 8, None), a.AR_ERR_INVALID_ARGUMENT)
            full = (ctypes.c_uint8 * 256)()
            self.assertEqual(self.lib.ar_context_last_error(ctx, 256, full), a.AR_OK)
            message = text(full)
            self.assertIn("invalid argument", message)
            self.assertIn("last_error", message)
            # A small capacity: truncated, NUL-terminated inside the capacity, nothing written past it.
            small = (ctypes.c_uint8 * 16)(*([0xFF] * 16))
            self.assertEqual(self.lib.ar_context_last_error(ctx, 8, small), a.AR_OK)
            self.assertEqual(bytes(small[:8]), message.encode()[:7] + bytes([0]))
            self.assertEqual(bytes(small[8:]), bytes([0xFF]) * 8)
            # Capacity 1 leaves just the terminator.
            one = (ctypes.c_uint8 * 4)(*([0xFF] * 4))
            self.assertEqual(self.lib.ar_context_last_error(ctx, 1, one), a.AR_OK)
            self.assertEqual(bytes(one), bytes([0, 0xFF, 0xFF, 0xFF]))
        finally:
            self.assertEqual(self.lib.ar_context_destroy(ctx), a.AR_OK)

    def test_log_appends(self):
        for _ in range(2):
            st, ctx = self.create(self.abi.AR_DEVICE_CPU, 0)
            self.assertEqual(st, self.abi.AR_OK)
            self.assertEqual(self.lib.ar_context_destroy(ctx), self.abi.AR_OK)
        with open(self.log_path, encoding="utf-8") as f:
            self.assertEqual(sum(" INFO build " in ln for ln in f), 2)

    def test_bad_arguments(self):
        a = self.abi
        st, ctx = self.create(a.AR_DEVICE_CUDA, 99)
        self.assertEqual(st, a.AR_ERR_INVALID_ARGUMENT)
        self.assertFalse(ctx.value)
        self.assertEqual(self.create(a.AR_DEVICE_CPU, 1)[0], a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(self.create(77, 0)[0], a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(self.create(a.AR_DEVICE_CPU, 0, log_path="")[0], a.AR_ERR_INVALID_ARGUMENT)
        missing_dir = os.path.join(os.path.dirname(self.log_path), "no", "such", "dir", "x.log")
        self.assertEqual(self.create(a.AR_DEVICE_CPU, 0, log_path=missing_dir)[0], a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(self.lib.ar_context_destroy(None), a.AR_ERR_INVALID_ARGUMENT)


if __name__ == "__main__":
    unittest.main()
