# SPDX-License-Identifier: Apache-2.0
"""Mesh staging and ingestion through the generated binding, numpy views on the arena (task 035)."""
import ctypes
import importlib.util
import os
import tempfile
import time
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ABI = os.path.join(ROOT, "out", "build", "gen", "_abi.py")


def load_binding(path):
    spec = importlib.util.spec_from_file_location("_abi_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


QUAD = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], dtype=np.float32)


class TestMeshApi(unittest.TestCase):
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
        a = self.abi
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.log_path = os.path.join(d.name, "context.log")
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, self.log_path.encode(), None)
        self.ctx = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_context_create(ctypes.byref(desc), ctypes.byref(self.ctx)), a.AR_OK)
        self.addCleanup(self.lib.ar_context_destroy, self.ctx)
        scene_desc = a.ar_scene_desc(None, 2, 0)
        self.scene = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_scene_create(self.ctx, ctypes.byref(scene_desc), ctypes.byref(self.scene)), a.AR_OK)
        self.addCleanup(self.lib.ar_scene_destroy, self.scene)

    def view(self, slot, count, dtype):
        # `slot` is the T** field: the engine-owned slot that holds the array pointer.
        ptr = ctypes.cast(slot, ctypes.POINTER(ctypes.c_void_p))[0]
        if count == 0:
            return np.zeros(0, dtype=dtype)
        n = count * np.dtype(dtype).itemsize
        return np.frombuffer((ctypes.c_char * n).from_address(ptr), dtype=dtype)

    def begin_mesh(self, edit, key, V=4, C=4, F=1):
        a = self.abi
        d = a.ar_mesh_desc()
        d.key, d.vertex_count, d.corner_count, d.face_count, d.motion_steps = key, V, C, F, 1
        d.name = (ctypes.c_uint8 * 64)(*b"quad".ljust(64, b"\0"))
        b = a.ar_mesh_buffers()
        st = self.lib.ar_edit_mesh_begin(edit, ctypes.byref(d), ctypes.byref(b))
        return st, b

    def fill_quad(self, b, corner_vert=(0, 1, 2, 3)):
        self.view(b.positions, b.positions_count, np.float32)[:] = QUAD.ravel()
        self.view(b.corner_vert, b.corner_vert_count, np.int32)[:] = corner_vert
        self.view(b.face_start, b.face_start_count, np.int32)[:] = [0]
        self.view(b.corner_normals, b.corner_normals_count, np.float32)[:] = np.tile([0, 0, 1], 4)

    def commit_mesh(self, key, mutate=None):
        a, lib = self.abi, self.lib
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(self.scene, ctypes.byref(edit)), a.AR_OK)
        st, b = self.begin_mesh(edit, key)
        self.assertEqual(st, a.AR_OK)
        self.assertEqual((b.positions_count, b.corner_vert_count, b.face_start_count, b.corner_normals_count), (12, 4, 1, 12))
        self.fill_quad(b)
        if mutate:
            mutate(b)
        self.assertEqual(lib.ar_edit_item_commit(edit, key), a.AR_OK)
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(gen)), a.AR_OK)
        return gen.value

    def log_text(self):
        with open(self.log_path, "r", errors="replace") as f:
            return f.read()

    def wait_log(self, needle, seconds=5.0):
        t0 = time.time()
        while time.time() - t0 < seconds:
            if needle in self.log_text():
                return [l for l in self.log_text().splitlines() if needle in l][0]
            time.sleep(0.02)
        self.fail(f"{needle!r} not in the context log:\n{self.log_text()}")

    def test_numpy_views_commit_and_bad_descriptor(self):
        a, lib = self.abi, self.lib
        self.assertEqual(self.commit_mesh(1), 1)
        # A positions-only edit sees the committed V and C; wait for generation 1 first.
        t0 = time.time()
        edit = ctypes.c_void_p()
        while True:
            self.assertEqual(lib.ar_scene_edit_begin(self.scene, ctypes.byref(edit)), a.AR_OK)
            pb = a.ar_positions_buffers()
            if lib.ar_edit_positions_begin(edit, 1, ctypes.byref(pb)) == a.AR_OK:
                break
            self.assertEqual(lib.ar_edit_abort(edit), a.AR_OK)
            self.assertLess(time.time() - t0, 5.0, "mesh 1 never became active")
            time.sleep(0.02)
        self.assertEqual((pb.positions_count, pb.corner_normals_count), (12, 12))
        pos = self.view(pb.positions, pb.positions_count, np.float32)
        pos[:] = (QUAD * 2).ravel()
        self.view(pb.corner_normals, pb.corner_normals_count, np.float32)[:] = np.tile([0, 0, 1], 4)
        self.assertEqual(lib.ar_edit_item_commit(edit, 1), a.AR_OK)
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(gen)), a.AR_OK)
        self.assertEqual(gen.value, 2)
        # Unknown key and a bad descriptor are the caller's errors with a reason.
        edit2 = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(self.scene, ctypes.byref(edit2)), a.AR_OK)
        self.assertEqual(lib.ar_edit_positions_begin(edit2, 77, ctypes.byref(pb)), a.AR_ERR_INVALID_ARGUMENT)
        st, _ = self.begin_mesh(edit2, 3, V=4, C=5, F=2)
        self.assertEqual(st, a.AR_ERR_INVALID_ARGUMENT)
        buf = (ctypes.c_uint8 * 256)()
        self.assertEqual(lib.ar_edit_last_error(edit2, 256, buf), a.AR_OK)
        self.assertIn("corner_count", bytes(buf).split(b"\0")[0].decode())

    def test_nan_position_rejects_generation(self):
        def poison(b):
            self.view(b.positions, b.positions_count, np.float32)[5] = np.nan

        gen = self.commit_mesh(4, poison)
        line = self.wait_log(f"generation {gen} rejected")
        print(f"rejected generation report detail: {line}")
        self.assertIn("key 4", line)
        self.assertIn("positions[5] is not finite", line)
        self.assertIn("quad", line)

    def test_corner_vert_out_of_range_rejects_generation(self):
        def poison(b):
            self.view(b.corner_vert, b.corner_vert_count, np.int32)[1] = 9

        gen = self.commit_mesh(5, poison)
        line = self.wait_log(f"generation {gen} rejected")
        self.assertIn("corner_vert[1] = 9", line)


if __name__ == "__main__":
    unittest.main()
