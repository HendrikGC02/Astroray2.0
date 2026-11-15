# SPDX-License-Identifier: Apache-2.0
"""Session readback through the generated binding on the CPU device (task 104, 0007 d5, 0008
d13a): progress and its state sequence, report polling, display frames, pass enumeration and
reads on an empty render, per-pass counters and the FITS film dump parsed by fits_read.py.
An empty render has no camera samples (the active-pixel list does not exist yet), so every pixel
has n = 1 and a zero mean."""
import ctypes
import importlib.util
import math
import os
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ABI = os.path.join(ROOT, "out", "build", "gen", "_abi.py")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")

STATE_NAMES = {0: "INITIALIZING", 1: "RENDERING", 2: "PAUSED", 3: "CONVERGED", 4: "TRIMMED", 5: "CLOSED", 6: "FAILED"}
# Ledger categories in host order (0007 d14): the ones a session fills.
FILM, TABLES, DISPLAY_FRAME = 7, 8, 9


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestSessionApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, ABI):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load_module("_abi_under_test", ABI)
        cls.fits_read = load_module("_fits_read_under_test", FITS_READ)
        cls.lib = ctypes.CDLL(DLL)
        cls.abi.check(cls.lib)
        cls.abi.bind(cls.lib)

    def setUp(self):
        a = self.abi
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.dir = d.name
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, os.path.join(d.name, "context.log").encode(), None)
        self.ctx = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_context_create(ctypes.byref(desc), ctypes.byref(self.ctx)), a.AR_OK)
        self.addCleanup(self.lib.ar_context_destroy, self.ctx)
        sdesc = a.ar_scene_desc(None, 2, 0)
        self.scene = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_scene_create(self.ctx, ctypes.byref(sdesc), ctypes.byref(self.scene)), a.AR_OK)
        self.addCleanup(self.lib.ar_scene_destroy, self.scene)

    def make_session(self, size):
        a = self.abi
        desc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, size, size, a.AR_DENOISER_NONE)
        session = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_session_create(self.scene, ctypes.byref(desc), ctypes.byref(session)), a.AR_OK)
        self.addCleanup(self.destroy_session, session)
        return session

    def destroy_session(self, session):
        self.lib.ar_session_close(session)
        self.lib.ar_session_wait(session, 20000)
        end = time.perf_counter() + 20.0
        while self.progress(session).state != self.abi.AR_STATE_CLOSED and time.perf_counter() < end:
            self.lib.ar_session_wait(session, 10)
        self.lib.ar_session_destroy(session)

    def progress(self, session):
        p = self.abi.ar_progress()
        self.assertEqual(self.lib.ar_session_get_progress(session, ctypes.byref(p)), self.abi.AR_OK)
        return p

    def converge(self, session):
        a = self.abi
        self.assertEqual(self.lib.ar_session_start(session), a.AR_OK)
        self.assertEqual(self.lib.ar_session_wait(session, 20000), a.AR_OK)
        self.assertEqual(self.progress(session).state, a.AR_STATE_CONVERGED)

    def last_error(self, session):
        buf = (ctypes.c_uint8 * 256)()
        self.assertEqual(self.lib.ar_session_last_error(session, 256, buf), self.abi.AR_OK)
        return bytes(buf).split(b"\0")[0].decode()

    def view(self, w, h):
        v = self.abi.ar_view_desc()
        v.region_width, v.region_height, v.render_scale = w, h, 1.0
        for i in (0, 5, 10):
            v.camera.cam_to_world.m[i] = 1.0
        return v

    def read_pass(self, session, index, quantity, n):
        buf = (ctypes.c_float * n)()
        st = self.lib.ar_session_read_pass(session, index, quantity, n, buf)
        return st, list(buf)

    def test_state_sequence(self):
        # A large film makes allocation and the first pass long enough for a polling loop to see
        # INITIALIZING and RENDERING; the sequence ends in CLOSED.
        a, lib = self.abi, self.lib
        session = self.make_session(1024)
        seen = []

        def poll():
            s = self.progress(session).state
            if not seen or seen[-1] != s:
                seen.append(s)
            return s

        self.assertEqual(poll(), a.AR_STATE_INITIALIZING)
        self.assertEqual(lib.ar_session_start(session), a.AR_OK)
        end = time.perf_counter() + 60.0
        while poll() != a.AR_STATE_CONVERGED:
            self.assertLess(time.perf_counter(), end, "never converged")
        p = self.progress(session)
        self.assertGreaterEqual(p.samples_per_pixel, 1)
        self.assertGreater(p.last_pass_ms, 0.0)
        self.assertEqual(lib.ar_session_close(session), a.AR_OK)
        while poll() != a.AR_STATE_CLOSED:
            self.assertLess(time.perf_counter(), end, "never closed")
        print("state sequence: " + " -> ".join(STATE_NAMES[s] for s in seen))
        self.assertEqual(seen[0], a.AR_STATE_INITIALIZING)
        self.assertIn(a.AR_STATE_RENDERING, seen)
        self.assertEqual(seen[-1], a.AR_STATE_CLOSED)
        order = [s for s in seen if s in (0, 1, 3, 5)]
        self.assertEqual(order, sorted(order), "states went backwards")

    def test_progress_ledger(self):
        a = self.abi
        session = self.make_session(64)
        self.converge(session)
        p = self.progress(session)
        # The session's own film, tables and one RGBA16F frame at the largest size.
        self.assertGreater(p.ledger_bytes[FILM], 0)
        self.assertGreater(p.ledger_bytes[TABLES], 0)
        self.assertEqual(p.ledger_bytes[DISPLAY_FRAME], 64 * 64 * 8)
        self.assertEqual(self.lib.ar_session_close(session), a.AR_OK)
        self.lib.ar_session_wait(session, 20000)
        end = time.perf_counter() + 20.0
        while self.progress(session).state != a.AR_STATE_CLOSED:
            self.assertLess(time.perf_counter(), end)
            self.lib.ar_session_wait(session, 10)
        self.assertEqual(sum(self.progress(session).ledger_bytes), 0, "a closed session still holds memory")

    def test_report_polling(self):
        a, lib = self.abi, self.lib
        session = self.make_session(16)
        self.converge(session)
        info = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(session, ctypes.byref(info)), a.AR_OK)
        entry = a.ar_report_entry()
        for i in range(info.entry_count):
            self.assertEqual(lib.ar_session_report_entry(session, i, ctypes.byref(entry)), a.AR_OK)
        print(f"report: {info.entry_count} entries, revision {info.revision}")
        self.assertEqual(lib.ar_session_report_entry(session, info.entry_count, ctypes.byref(entry)),
                         a.AR_ERR_INVALID_ARGUMENT)
        again = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(session, ctypes.byref(again)), a.AR_OK)
        self.assertEqual(again.revision, info.revision, "an unchanged report changed revision")

    def test_display_frames(self):
        a, lib = self.abi, self.lib
        session = self.make_session(32)
        self.converge(session)
        frame = a.ar_display_frame()
        last_id, last_generation, ids = 0, 0, []
        for k in range(4):
            t0 = time.perf_counter()
            self.assertEqual(lib.ar_display_acquire(session, last_id, ctypes.byref(frame)), a.AR_OK)
            self.assertLess((time.perf_counter() - t0) * 1000.0, 5.0, "acquire waited")
            self.assertGreater(frame.frame_id, last_id, f"no new frame at step {k}")
            self.assertGreaterEqual(frame.generation, last_generation)
            w, h = (32, 32) if k == 0 else (16 + k, 16 + k)
            self.assertEqual((frame.width, frame.height), (w, h))
            self.assertEqual(frame.format, a.AR_DISPLAY_FORMAT_RGBA16F)
            self.assertEqual(frame.row_bytes, w * 8)
            self.assertEqual(len(ctypes.string_at(frame.data, frame.row_bytes * frame.height)), w * 8 * h)
            ids.append(frame.frame_id)
            last_id, last_generation = frame.frame_id, frame.generation
            self.assertEqual(lib.ar_display_release(session, frame.frame_id), a.AR_OK)
            # Nothing newer than what was just taken.
            none = a.ar_display_frame()
            self.assertEqual(lib.ar_display_acquire(session, last_id, ctypes.byref(none)), a.AR_OK)
            self.assertEqual(none.frame_id, 0)
            v = self.view(16 + k + 1, 16 + k + 1)
            self.assertEqual(lib.ar_session_set_view(session, ctypes.byref(v)), a.AR_OK)
            self.assertEqual(lib.ar_session_wait(session, 20000), a.AR_OK)
        print(f"frames {ids}, last generation {last_generation}")
        self.assertEqual(lib.ar_display_release(session, 999999), a.AR_ERR_INVALID_ARGUMENT)

    def test_passes_of_an_empty_render(self):
        a, lib = self.abi, self.lib
        session = self.make_session(8)
        count = ctypes.c_uint32()
        self.assertEqual(lib.ar_session_pass_count(session, ctypes.byref(count)), a.AR_OK)
        names = []
        for i in range(count.value):
            info = a.ar_pass_info()
            self.assertEqual(lib.ar_session_pass_info(session, i, ctypes.byref(info)), a.AR_OK)
            names.append(bytes(info.name).split(b"\0")[0].decode())
        self.assertEqual(names, ["Combined", "AR Sample Count", "AR Bounces", "AR Rel Error", "AR Invalid"])
        self.assertEqual(lib.ar_session_pass_info(session, count.value, ctypes.byref(info)), a.AR_ERR_INVALID_ARGUMENT)
        self.converge(session)
        info = a.ar_pass_info()
        lib.ar_session_pass_info(session, 0, ctypes.byref(info))
        self.assertEqual((info.component_count, info.width, info.height, info.unit), (3, 8, 8, a.AR_UNIT_ENERGY))
        n = 8 * 8
        st, mean = self.read_pass(session, 0, a.AR_QUANTITY_MEAN, n * 3)
        self.assertEqual(st, a.AR_OK)
        self.assertEqual(mean, [0.0] * (n * 3))
        st, var = self.read_pass(session, 0, a.AR_QUANTITY_VARIANCE, n * 3)
        self.assertEqual(st, a.AR_OK)
        self.assertTrue(all(math.isnan(v) for v in var), "one sample has no variance")
        st, cnt = self.read_pass(session, 0, a.AR_QUANTITY_COUNT, n)
        self.assertEqual((st, cnt), (a.AR_OK, [1.0] * n))
        st, rel = self.read_pass(session, 3, a.AR_QUANTITY_MEAN, n)
        self.assertEqual((st, rel), (a.AR_OK, [-1.0] * n))
        # A wrong size or a quantity the pass cannot give fails loudly and says why.
        self.assertEqual(self.read_pass(session, 0, a.AR_QUANTITY_MEAN, n * 3 - 1)[0], a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("exactly", self.last_error(session))
        self.assertEqual(self.read_pass(session, 2, a.AR_QUANTITY_VARIANCE, n)[0], a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(self.read_pass(session, 99, a.AR_QUANTITY_MEAN, n)[0], a.AR_ERR_INVALID_ARGUMENT)

    def test_counters(self):
        a, lib = self.abi, self.lib
        session = self.make_session(8)
        self.converge(session)
        buf = (ctypes.c_uint64 * 1)()
        self.assertEqual(lib.ar_session_read_counters(session, 0, buf), a.AR_ERR_INVALID_ARGUMENT)
        words = int(self.last_error(session).rsplit("need ", 1)[1])
        buf = (ctypes.c_uint64 * (words + 2))(*([7] * (words + 2)))
        self.assertEqual(lib.ar_session_read_counters(session, words + 2, buf), a.AR_OK)
        self.assertEqual(list(buf)[words:], [0, 0])
        self.assertEqual(list(buf)[:4], [0, 0, 0, 0], "an empty pass launched work")

    def test_write_fits(self):
        a, lib = self.abi, self.lib
        session = self.make_session(8)
        self.converge(session)
        path = os.path.join(self.dir, "film.fits")
        self.assertEqual(lib.ar_session_write_fits(session, path.encode(), a.AR_FITS_SCIENCE), a.AR_ERR_UNSUPPORTED)
        self.assertEqual(lib.ar_session_write_fits(session, path.encode(), a.AR_FITS_DUMP), a.AR_OK)
        with open(path, "rb") as f:
            hdus = self.fits_read.read_hdus(f.read())
        primary = hdus[0][0]
        self.assertIs(primary["DENOISED"], False)
        self.assertEqual(primary["SPP"], 1)
        self.assertEqual(primary["SAMPLER"], "sobol")
        self.assertEqual(len(primary["OBSHASH"]), 16)
        names = {h["EXTNAME"]: d for h, d in hdus[1:]}
        for c in range(3):
            self.assertEqual(names[f"display.{c}"].shape, (8, 8))
            self.assertTrue((names[f"display.{c}"] == 0).all())
        print(f"fits: {len(names)} extensions: {sorted(names)}")
        self.assertIn("bounces", names)
        self.assertEqual(lib.ar_session_write_fits(session, os.path.join(self.dir, "no", "such", "x.fits").encode(),
                                                   a.AR_FITS_DUMP), a.AR_ERR_INTERNAL)


if __name__ == "__main__":
    unittest.main()
