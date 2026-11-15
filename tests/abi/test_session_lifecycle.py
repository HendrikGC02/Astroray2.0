# SPDX-License-Identifier: Apache-2.0
"""Session lifecycle through the generated binding on the CPU device (task 052, 0007 d5, d23):
create, start, wait, pause, resume, close, destroy. Every call but the named wait returns within
0009 B-21 (5 ms); each call's time is printed, and the final state is CLOSED."""
import ctypes
import importlib.util
import os
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ABI = os.path.join(ROOT, "out", "build", "gen", "_abi.py")
B21_MS = 5.0

STATE_NAMES = {0: "INITIALIZING", 1: "RENDERING", 2: "PAUSED", 3: "CONVERGED", 4: "TRIMMED", 5: "CLOSED", 6: "FAILED"}


def load_binding(path):
    spec = importlib.util.spec_from_file_location("_abi_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestSessionLifecycle(unittest.TestCase):
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
        sdesc = a.ar_scene_desc(None, 2, 0)
        self.scene = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_scene_create(self.ctx, ctypes.byref(sdesc), ctypes.byref(self.scene)), a.AR_OK)
        self.addCleanup(self.lib.ar_scene_destroy, self.scene)

    def timed(self, name, fn, *args, budget=True):
        t0 = time.perf_counter()
        st = fn(*args)
        ms = (time.perf_counter() - t0) * 1000.0
        print(f"{name}: {ms:.3f} ms" + (f" (B-21 {B21_MS:g} ms)" if budget else " (named wait)"))
        self.assertEqual(st, self.abi.AR_OK, name)
        if budget:
            self.assertLess(ms, B21_MS, f"{name} took {ms:.3f} ms")
        return ms

    def state(self, session):
        p = self.abi.ar_progress()
        self.assertEqual(self.lib.ar_session_get_progress(session, ctypes.byref(p)), self.abi.AR_OK)
        return p.state

    def wait_for(self, session, want, timeout_s=20.0):
        # The named wait returns on CONVERGED, CLOSED or FAILED; PAUSED is polled.
        end = time.perf_counter() + timeout_s
        while self.state(session) != want:
            self.assertLess(time.perf_counter(), end, f"state {STATE_NAMES[self.state(session)]}, want {STATE_NAMES[want]}")
            self.lib.ar_session_wait(session, 10)
        print(f"state: {STATE_NAMES[want]}")

    def test_lifecycle(self):
        a, lib = self.abi, self.lib
        desc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, 64, 64, a.AR_DENOISER_NONE)
        session = ctypes.c_void_p()
        self.timed("ar_session_create", lib.ar_session_create, self.scene, ctypes.byref(desc), ctypes.byref(session))
        self.assertTrue(session.value)
        self.assertIn(self.state(session), (a.AR_STATE_INITIALIZING,))
        self.timed("ar_session_start", lib.ar_session_start, session)
        self.timed("ar_session_wait", lib.ar_session_wait, session, 20000, budget=False)
        self.assertEqual(self.state(session), a.AR_STATE_CONVERGED)
        print("state: CONVERGED")
        self.timed("ar_session_pause", lib.ar_session_pause, session)
        self.wait_for(session, a.AR_STATE_PAUSED)
        self.timed("ar_session_resume", lib.ar_session_resume, session)
        self.timed("ar_session_close", lib.ar_session_close, session)
        self.wait_for(session, a.AR_STATE_CLOSED)
        # After close only wait, progress and destroy are accepted.
        self.assertEqual(lib.ar_session_start(session), a.AR_ERR_INVALID_ARGUMENT)
        final = self.state(session)
        self.timed("ar_session_destroy", lib.ar_session_destroy, session)
        print(f"final state: {STATE_NAMES[final]}")
        self.assertEqual(final, a.AR_STATE_CLOSED)

    def test_unsupported_input_refused(self):
        a, lib = self.abi, self.lib
        desc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, 16, 16, a.AR_DENOISER_NONE)
        session = ctypes.c_void_p()
        self.assertEqual(lib.ar_session_create(self.scene, ctypes.byref(desc), ctypes.byref(session)), a.AR_OK)
        self.addCleanup(lib.ar_session_destroy, session)
        self.assertEqual(lib.ar_session_set_display_mode(session, a.AR_DISPLAY_DEPTH), a.AR_ERR_UNSUPPORTED)
        view = a.ar_view_desc()
        view.region_width, view.region_height, view.render_scale = 32, 16, 1.0
        self.assertEqual(lib.ar_session_set_view(session, ctypes.byref(view)), a.AR_ERR_INVALID_ARGUMENT)
        buf = (ctypes.c_uint8 * 256)()
        self.assertEqual(lib.ar_session_last_error(session, 256, buf), a.AR_OK)
        self.assertIn("region", bytes(buf).split(b"\0")[0].decode())


if __name__ == "__main__":
    unittest.main()
