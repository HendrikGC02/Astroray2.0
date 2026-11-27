# SPDX-License-Identifier: Apache-2.0
"""Requested passes through the generated binding on the CPU device (task 117; 0008 d10, d13a;
rulings 74, 75): settings that request Position make ar_session_pass_count and ar_session_pass_info
list the primary-hit AOVs, ar_session_read_pass returns finite values for Position, a pass name the
engine cannot produce gives one PASS_UNSUPPORTED entry, and after a commit that rebuilds the film
an index from the old pass list is refused until pass_count is called again. ar_settings.statistics
(126; 0008 d12, on by default) switches the statistics channels: off, the film has no AR Bounces pass
and the bounce display raises one PASS_UNSUPPORTED entry; on again, the pass is back; another value
is refused. A FINAL session with AR_DENOISER_OIDN (task 197; 0008 d10, d13a, d16) lists Denoising
Albedo, Denoising Normal and Noisy Image, flags its Combined AR_PASS_DENOISED once converged and refuses
that Combined's variance; with the denoiser forced to fail (ASTRORAY_TEST_DENOISER_FAIL=library, set
before the session is created) Combined is not flagged and the report holds one DENOISER_UNAVAILABLE
entry. The scene has no geometry (every camera ray misses); tests/cpu/test_session_passes.cpp checks
Position on a hit and tests/cpu/test_session_denoise.cpp the denoised pixels."""
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
SIZE = 4
UNKNOWN = b"AR No Such Pass"
STALE = "the pass list changed; call ar_session_pass_count again"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestSessionPasses(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, ABI):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load_module("_abi_session_passes", ABI)
        cls.lib = ctypes.CDLL(DLL)
        cls.abi.check(cls.lib)
        cls.abi.bind(cls.lib)

    def setUp(self):
        a, lib = self.abi, self.lib
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, os.path.join(d.name, "context.log").encode(), None)
        self.ctx = ctypes.c_void_p()
        self.assertEqual(lib.ar_context_create(ctypes.byref(desc), ctypes.byref(self.ctx)), a.AR_OK)
        self.addCleanup(lib.ar_context_destroy, self.ctx)
        self.scene = ctypes.c_void_p()
        sdesc = a.ar_scene_desc(None, 2, 0)
        self.assertEqual(lib.ar_scene_create(self.ctx, ctypes.byref(sdesc), ctypes.byref(self.scene)), a.AR_OK)
        self.addCleanup(lib.ar_scene_destroy, self.scene)

    def commit_settings(self, passes, statistics=None, expect=None, film_transparent=None):
        a, lib = self.abi, self.lib
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(self.scene, ctypes.byref(edit)), a.AR_OK)
        names = (a.ar_name * max(1, len(passes)))()
        for n, text in zip(names, passes):
            n.text = (ctypes.c_uint8 * 64)(*text.ljust(64, b"\0"))
        s = a.ar_settings()
        s.passes, s.pass_count = names, len(passes)
        s.samples = s.viewport_samples = 2
        s.pixel_filter, s.pixel_filter_width, s.film_exposure = a.AR_FILTER_BOX, 1.0, 1.0
        s.max_bounces = 4  # the bounce heat map's range
        if statistics is not None:
            s.statistics = statistics
        if film_transparent is not None:
            s.film_transparent = film_transparent
        self.assertEqual(lib.ar_edit_set_settings(edit, ctypes.byref(s)), a.AR_OK)
        gen = ctypes.c_uint64()
        st = lib.ar_edit_commit(edit, ctypes.byref(gen))
        if expect is not None:
            self.assertEqual(st, expect)
            lib.ar_edit_abort(edit)
            return None
        self.assertEqual(st, a.AR_OK)
        return gen.value

    def start_session(self, mode=None, denoiser=None):
        a, lib = self.abi, self.lib
        sdesc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT if mode is None else mode, SIZE, SIZE,
                                  a.AR_DENOISER_NONE if denoiser is None else denoiser)
        session = ctypes.c_void_p()
        self.assertEqual(lib.ar_session_create(self.scene, ctypes.byref(sdesc), ctypes.byref(session)), a.AR_OK)
        self.addCleanup(self.destroy, session)
        v = a.ar_view_desc()
        v.region_width, v.region_height, v.render_scale = SIZE, SIZE, 1.0
        for i in (0, 5, 10):
            v.camera.cam_to_world.m[i] = 1.0
        for i, x in ((0, 0.5), (5, 0.375), (11, -1.0), (15, 1.0)):
            v.camera.projection_inverse[i] = x
        v.camera.type = a.AR_CAMERA_PERSP
        self.assertEqual(lib.ar_session_set_view(session, ctypes.byref(v)), a.AR_OK)
        self.assertEqual(lib.ar_session_start(session), a.AR_OK)
        return session

    def settle(self, session, gen):
        a = self.abi
        end = time.perf_counter() + 30.0
        while True:
            p = a.ar_progress()
            self.assertEqual(self.lib.ar_session_get_progress(session, ctypes.byref(p)), a.AR_OK)
            if p.state == a.AR_STATE_CONVERGED and p.active_generation == gen:
                return
            self.assertNotEqual(p.state, a.AR_STATE_FAILED)
            self.assertLess(time.perf_counter(), end, "never converged")
            self.lib.ar_session_wait(session, 50)

    def passes(self, session):
        """Name -> (index, components), enumerated with pass_count first (ruling 75)."""
        a, lib = self.abi, self.lib
        count = ctypes.c_uint32()
        self.assertEqual(lib.ar_session_pass_count(session, ctypes.byref(count)), a.AR_OK)
        listed = {}
        for i in range(count.value):
            info = a.ar_pass_info()
            self.assertEqual(lib.ar_session_pass_info(session, i, ctypes.byref(info)), a.AR_OK)
            listed[bytes(info.name).split(b"\0")[0].decode()] = (i, info.component_count)
        return listed

    def read(self, session, index, comps, quantity=None):
        n = SIZE * SIZE * comps
        buf = (ctypes.c_float * n)()
        q = self.abi.AR_QUANTITY_MEAN if quantity is None else quantity
        return self.lib.ar_session_read_pass(session, index, q, n, buf), list(buf)

    def flags(self, session):
        """Name -> ar_pass_info.flags."""
        a, lib = self.abi, self.lib
        count = ctypes.c_uint32()
        self.assertEqual(lib.ar_session_pass_count(session, ctypes.byref(count)), a.AR_OK)
        out = {}
        for i in range(count.value):
            info = a.ar_pass_info()
            self.assertEqual(lib.ar_session_pass_info(session, i, ctypes.byref(info)), a.AR_OK)
            out[bytes(info.name).split(b"\0")[0].decode()] = info.flags
        return out

    def entries(self, session, code):
        a, lib = self.abi, self.lib
        info = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(session, ctypes.byref(info)), a.AR_OK)
        hits = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            self.assertEqual(lib.ar_session_report_entry(session, i, ctypes.byref(e)), a.AR_OK)
            if e.code == code:
                hits.append(bytes(e.detail).split(b"\0")[0].decode())
        return hits

    def last_error(self, session):
        buf = (ctypes.c_uint8 * 256)()
        self.assertEqual(self.lib.ar_session_last_error(session, 256, buf), self.abi.AR_OK)
        return bytes(buf).split(b"\0")[0].decode()

    def test_position_and_an_unknown_pass(self):
        a, lib = self.abi, self.lib
        gen = self.commit_settings([b"Position", UNKNOWN])
        session = self.start_session()
        self.settle(session, gen)
        listed = self.passes(session)
        print(f"session passes: {sorted(listed)}")
        for aov in ("Position", "Normal", "UV", "IndexMA"):
            self.assertIn(aov, listed)
        index, comps = listed["Position"]
        self.assertEqual(comps, 3)
        st, values = self.read(session, index, comps)
        self.assertEqual(st, a.AR_OK)
        self.assertTrue(all(math.isfinite(x) for x in values), values)

        info = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(session, ctypes.byref(info)), a.AR_OK)
        hits = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            self.assertEqual(lib.ar_session_report_entry(session, i, ctypes.byref(e)), a.AR_OK)
            if e.code == a.AR_REPORT_PASS_UNSUPPORTED:
                hits.append((bytes(e.detail).split(b"\0")[0], e.level, e.subject, e.count))
        self.assertEqual(hits, [(UNKNOWN, a.AR_LEVEL_WARNING, 0, 1)])

    def test_depth_and_alpha_with_film_transparent(self):
        # Depth and Alpha through the ABI (0008 d10; Cycles film/read.h): Depth follows Position's order
        # (right after Combined), Alpha closes the AOVs. Every ray of this empty scene reaches the world,
        # so a ray that scattered nowhere is a transparent-background ray with the setting on: Alpha 0.
        # With it off Alpha is exactly 1. Depth reads 1e10 where nothing was hit, either way.
        a = self.abi
        for on in (1, 0):
            with self.subTest(film_transparent=on):
                gen = self.commit_settings([b"Depth", b"Alpha", b"Position"], film_transparent=on)
                session = self.start_session()
                self.settle(session, gen)
                listed = self.passes(session)
                order = [name for name, _ in sorted(listed.items(), key=lambda kv: kv[1][0])]
                self.assertEqual(order[:3], ["Combined", "Depth", "Position"], order)
                self.assertLess(order.index("IndexMA"), order.index("Alpha"))
                for name, want in (("Alpha", 0.0 if on else 1.0), ("Depth", 1e10)):
                    index, comps = listed[name]
                    self.assertEqual(comps, 1)
                    st, values = self.read(session, index, comps)
                    self.assertEqual(st, a.AR_OK)
                    self.assertTrue(all(v == ctypes.c_float(want).value for v in values), (name, values[:4]))
                print(f"film_transparent {on}: Alpha and Depth read as Cycles' conversions give")

    def pass_unsupported(self, session):
        a, lib = self.abi, self.lib
        info = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(session, ctypes.byref(info)), a.AR_OK)
        hits = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            self.assertEqual(lib.ar_session_report_entry(session, i, ctypes.byref(e)), a.AR_OK)
            if e.code == a.AR_REPORT_PASS_UNSUPPORTED:
                hits.append((bytes(e.detail).split(b"\0")[0], e.level, e.subject, e.count))
        return hits

    def test_statistics_off_and_on(self):
        a, lib = self.abi, self.lib
        gen = self.commit_settings([], statistics=a.AR_STATISTICS_OFF)
        session = self.start_session()
        self.assertEqual(lib.ar_session_set_display_mode(session, a.AR_DISPLAY_BOUNCE_COUNT), a.AR_OK)
        self.settle(session, gen)
        off = self.passes(session)
        self.assertNotIn("AR Bounces", off)
        self.assertIn("AR Sample Count", off)  # n is the resolve's, not a statistics channel
        # The bounce display met a film without statistics: one warning, the render goes on.
        self.assertEqual(self.pass_unsupported(session), [(b"AR Bounces", a.AR_LEVEL_WARNING, 0, 1)])
        gen = self.commit_settings([], statistics=a.AR_STATISTICS_ON)
        self.settle(session, gen)
        on = self.passes(session)
        self.assertIn("AR Bounces", on)
        index, comps = on["AR Bounces"]
        st, values = self.read(session, index, comps)
        self.assertEqual(st, a.AR_OK)
        self.assertTrue(all(math.isfinite(x) for x in values), values)
        self.assertEqual(len(self.pass_unsupported(session)), 1, "the statistics are on again: no new entry")
        self.commit_settings([], statistics=a.AR_STATISTICS_OFF + 1, expect=a.AR_ERR_INVALID_ARGUMENT)
        print(f"statistics off: {len(off)} passes; on: {len(on)} passes (AR Bounces at {index})")

    def test_statistics_pass_requested_while_off(self):
        # A statistics pass named while the statistics are off is a pass this film does not produce
        # (ruling 74): one PASS_UNSUPPORTED entry, never a silent omission.
        a = self.abi
        gen = self.commit_settings([b"AR Bounces"], statistics=a.AR_STATISTICS_OFF)
        session = self.start_session()
        self.settle(session, gen)
        self.assertNotIn("AR Bounces", self.passes(session))
        self.assertEqual(self.pass_unsupported(session), [(b"AR Bounces", a.AR_LEVEL_WARNING, 0, 1)])

    def test_stale_index_after_a_rebuild_is_refused(self):
        # With the AOVs, IndexMA sits where AR Invalid sits without them; both are one-component
        # means, so only the revision check tells a stale index from a valid one.
        a, lib = self.abi, self.lib
        gen = self.commit_settings([b"Position"])
        session = self.start_session()
        self.settle(session, gen)
        index, comps = self.passes(session)["IndexMA"]
        self.assertEqual(self.read(session, index, comps)[0], a.AR_OK)
        gen = self.commit_settings([])
        self.settle(session, gen)
        st, _ = self.read(session, index, comps)
        self.assertEqual(st, a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn(STALE, self.last_error(session))
        info = a.ar_pass_info()
        self.assertEqual(lib.ar_session_pass_info(session, 0, ctypes.byref(info)), a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn(STALE, self.last_error(session))
        listed = self.passes(session)  # a new pass_count accepts the new list
        self.assertNotIn("IndexMA", listed)
        self.assertEqual(listed["AR Invalid"][0], index)
        self.assertEqual(self.read(session, index, comps)[0], a.AR_OK)
        print(f"stale index {index} refused after the rebuild; reads again after pass_count")

    def test_denoised_combined_and_noisy_image(self):
        a, lib = self.abi, self.lib
        # Viewport OIDN is M2 (0008 d16): refused at create, never a session that silently skips it.
        sdesc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, SIZE, SIZE, a.AR_DENOISER_OIDN)
        refused = ctypes.c_void_p()
        self.assertEqual(lib.ar_session_create(self.scene, ctypes.byref(sdesc), ctypes.byref(refused)), a.AR_ERR_UNSUPPORTED)

        gen = self.commit_settings([])
        session = self.start_session(a.AR_SESSION_MODE_FINAL, a.AR_DENOISER_OIDN)
        self.settle(session, gen)
        flags = self.flags(session)
        print(f"denoiser session passes: {flags}")
        for name in ("Combined", "Denoising Albedo", "Denoising Normal", "Noisy Image"):
            self.assertIn(name, flags)
        self.assertEqual(flags["Combined"], a.AR_PASS_DENOISED)
        for name in ("Denoising Albedo", "Denoising Normal", "Noisy Image"):
            self.assertEqual(flags[name], 0, name)
        self.assertEqual(self.entries(session, a.AR_REPORT_DENOISER_UNAVAILABLE), [])
        listed = self.passes(session)
        index, comps = listed["Combined"]
        st, values = self.read(session, index, comps)
        self.assertEqual(st, a.AR_OK)
        self.assertTrue(all(math.isfinite(x) for x in values), values)
        st, _ = self.read(session, index, comps, a.AR_QUANTITY_VARIANCE)
        self.assertEqual(st, a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("Noisy Image", self.last_error(session))
        noisy_index, noisy_comps = listed["Noisy Image"]
        self.assertEqual(self.read(session, noisy_index, noisy_comps, a.AR_QUANTITY_VARIANCE)[0], a.AR_OK)

        # A denoiser that cannot be created: the noisy image, no flag, one entry with OIDN's text, CONVERGED.
        os.environ["ASTRORAY_TEST_DENOISER_FAIL"] = "library"
        try:
            failed = self.start_session(a.AR_SESSION_MODE_FINAL, a.AR_DENOISER_OIDN)
        finally:
            del os.environ["ASTRORAY_TEST_DENOISER_FAIL"]
        self.settle(failed, gen)
        hooks = self.entries(failed, a.AR_REPORT_TEST_HOOK_ACTIVE)
        self.assertIn("DENOISER_FAIL=library", hooks)
        self.assertEqual(self.flags(failed)["Combined"], 0)
        unavailable = self.entries(failed, a.AR_REPORT_DENOISER_UNAVAILABLE)
        self.assertEqual(len(unavailable), 1, unavailable)
        self.assertIn("no library folder given", unavailable[0])
        print(f"forced failure entry: {unavailable[0]}")

    def destroy(self, session):
        self.lib.ar_session_close(session)
        end = time.perf_counter() + 20.0
        while time.perf_counter() < end:
            p = self.abi.ar_progress()
            self.lib.ar_session_get_progress(session, ctypes.byref(p))
            if p.state == self.abi.AR_STATE_CLOSED:
                break
            self.lib.ar_session_wait(session, 10)
        self.lib.ar_session_destroy(session)


if __name__ == "__main__":
    unittest.main()
