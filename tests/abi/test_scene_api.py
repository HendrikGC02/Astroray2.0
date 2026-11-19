# SPDX-License-Identifier: Apache-2.0
"""Scene creation and edits through the generated binding on the CPU device (task 032)."""
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


def text(arr):
    return bytes(arr).split(b"\0")[0].decode()


class TestSceneApi(unittest.TestCase):
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
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, os.path.join(d.name, "context.log").encode(), None)
        self.ctx = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_context_create(ctypes.byref(desc), ctypes.byref(self.ctx)), a.AR_OK)
        self.addCleanup(self.lib.ar_context_destroy, self.ctx)

    def new_scene(self):
        a = self.abi
        desc = a.ar_scene_desc(None, 2, 0)
        scene = ctypes.c_void_p()
        t0 = time.perf_counter()
        st = self.lib.ar_scene_create(self.ctx, ctypes.byref(desc), ctypes.byref(scene))
        ms = (time.perf_counter() - t0) * 1000.0
        self.assertEqual(st, a.AR_OK)
        self.assertTrue(scene.value)
        return scene, ms

    def last_error(self, fn, handle):
        buf = (ctypes.c_uint8 * 256)()
        self.assertEqual(fn(handle, 256, buf), self.abi.AR_OK)
        return text(buf)

    def test_create_edit_commit_destroy(self):
        a, lib = self.abi, self.lib
        scene, ms = self.new_scene()
        print(f"ar_scene_create: {ms:.3f} ms (budget 5 ms, B-21)")
        self.assertLess(ms, 5.0)
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.assertEqual(lib.ar_edit_reset(edit), a.AR_OK)
        astro = a.ar_astro_desc()
        astro.astro_kind = a.AR_ASTRO_BLACK_HOLE
        for i in (0, 5, 10):
            astro.xf.m[i] = 1.0
        astro.mass, astro.spin, astro.meters_per_rg = 4.0e6, 0.5, 1.0e3
        self.assertEqual(lib.ar_edit_set_astro(edit, 7, ctypes.byref(astro)), a.AR_OK)
        astro.spin = 1.0
        self.assertEqual(lib.ar_edit_set_astro(edit, 8, ctypes.byref(astro)), a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("spin", self.last_error(lib.ar_edit_last_error, edit))
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(gen)), a.AR_OK)
        self.assertEqual(gen.value, 1)
        # The committed edit freed the slot: a second edit opens and numbers on.
        edit2 = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit2)), a.AR_OK)
        self.assertEqual(lib.ar_edit_commit(edit2, ctypes.byref(gen)), a.AR_OK)
        self.assertEqual(gen.value, 2)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)

    def test_one_open_edit_and_abort(self):
        a, lib = self.abi, self.lib
        scene, _ = self.new_scene()
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        other = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(other)), a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("open edit", self.last_error(lib.ar_scene_last_error, scene))
        self.assertEqual(lib.ar_edit_abort(edit), a.AR_OK)
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(other)), a.AR_OK)
        # Destroying the scene with an edit open aborts it.
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)

    def test_unimplemented_entry_names_function_on_live_handle(self):
        a, lib = self.abi, self.lib
        scene, _ = self.new_scene()
        self.assertEqual(lib.ar_scene_trim(scene), a.AR_ERR_UNSUPPORTED)
        self.assertIn("ar_scene_trim", self.last_error(lib.ar_scene_last_error, scene))
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.assertEqual(lib.ar_edit_curves_begin(edit, None, None), a.AR_ERR_UNSUPPORTED)
        self.assertIn("ar_edit_curves_begin", self.last_error(lib.ar_edit_last_error, edit))
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)

    def commit_grid(self, scene, n):
        """Stages and commits an n x n quad grid (2 n^2 triangles) in one edit."""
        a, lib = self.abi, self.lib
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        d = a.ar_mesh_desc()
        d.key, d.vertex_count, d.corner_count, d.face_count, d.motion_steps = 1, (n + 1) ** 2, 4 * n * n, n * n, 1
        d.name = (ctypes.c_uint8 * 64)(*b"grid".ljust(64, b"\0"))
        b = a.ar_mesh_buffers()
        self.assertEqual(lib.ar_edit_mesh_begin(edit, ctypes.byref(d), ctypes.byref(b)), a.AR_OK)

        def view(slot, count, dtype):
            ptr = ctypes.cast(slot, ctypes.POINTER(ctypes.c_void_p))[0]
            return np.frombuffer((ctypes.c_char * (count * np.dtype(dtype).itemsize)).from_address(ptr), dtype=dtype)

        g = np.arange(n + 1, dtype=np.float32)
        x, y = np.meshgrid(g, g)
        view(b.positions, b.positions_count, np.float32)[:] = np.stack([x, y, np.zeros_like(x)], -1).ravel()
        i, j = np.meshgrid(np.arange(n), np.arange(n))
        v0 = (j * (n + 1) + i).ravel().astype(np.int32)
        view(b.corner_vert, b.corner_vert_count, np.int32)[:] = np.stack([v0, v0 + 1, v0 + n + 2, v0 + n + 1], -1).ravel()
        view(b.face_start, b.face_start_count, np.int32)[:] = np.arange(0, 4 * n * n, 4, dtype=np.int32)
        normals = view(b.corner_normals, b.corner_normals_count, np.float32).reshape(-1, 3)
        normals[:] = (0.0, 0.0, 1.0)
        self.assertEqual(lib.ar_edit_item_commit(edit, 1), a.AR_OK)
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(gen)), a.AR_OK)

    def test_destroyed_scene_handle_fails_loudly(self):
        """ar_scene_destroy hands the scene to the context's reaper (0007 d23); the handle is dead at
        return, and every later call on it is refused with a last error instead of using freed memory."""
        a, lib = self.abi, self.lib
        scene, _ = self.new_scene()
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)  # with an edit open: aborted by the free
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("destroyed", self.last_error(lib.ar_scene_last_error, scene))
        session = ctypes.c_void_p()
        desc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, 64, 64, a.AR_DENOISER_NONE)
        self.assertEqual(lib.ar_session_create(scene, ctypes.byref(desc), ctypes.byref(session)),
                         a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("destroyed", self.last_error(lib.ar_scene_last_error, scene))
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_ERR_INVALID_ARGUMENT)  # never a double free

    def test_context_destroy_waits_for_pending_scene_frees(self):
        """ar_context_destroy drains the reaper: every destroyed scene is freed (one log line each)
        before the context is, so nothing leaks and nothing is freed after its context."""
        a, lib = self.abi, self.lib
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        log = os.path.join(d.name, "own.log")
        ctx = ctypes.c_void_p()
        cdesc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, log.encode(), None)
        self.assertEqual(lib.ar_context_create(ctypes.byref(cdesc), ctypes.byref(ctx)), a.AR_OK)
        n = 6
        for i in range(n):
            scene = ctypes.c_void_p()
            self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(a.ar_scene_desc(None, 2, 0)), ctypes.byref(scene)), a.AR_OK)
            if i == n - 1:
                # The last free is slow (its mesh's ingest jobs still run when the pool shuts down), so a
                # context destroy that did not wait would log "context destroyed" before it.
                self.commit_grid(scene, 700)
            self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)
        self.assertEqual(lib.ar_context_destroy(ctx), a.AR_OK)
        with open(log, encoding="utf-8") as f:
            lines = f.read().splitlines()
        freed = [i for i, line in enumerate(lines) if " scene freed in " in line]
        last = max(i for i, line in enumerate(lines) if line.endswith("context destroyed"))
        print("last free:", lines[freed[-1]] if freed else None)
        self.assertEqual(len(freed), n, lines[-12:])
        self.assertLess(max(freed), last)

    def test_create_errors_set_context_last_error(self):
        a, lib = self.abi, self.lib
        scene = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_create(self.ctx, None, ctypes.byref(scene)), a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("ar_scene_create", self.last_error(lib.ar_context_last_error, self.ctx))


if __name__ == "__main__":
    unittest.main()
