# SPDX-License-Identifier: Apache-2.0
"""The render parts of 0001 V15 (task 107; 0001 D2.11): `astroray_cli render` exits non-zero only
when the report holds an ERROR entry (a WARNING entry, here the magenta material's, leaves it 0), and
an edit that replaces the magenta (unsupported-node) material removes its entries in a rendering
session. Not here, for want of a graph that can inject a non-finite sample (the graph compiler
refuses TEST_INJECT_NAN, 0005 item 31 reserves it to the VM): the INVALID_SAMPLE count and the film
reset's removal of FILM entries. The key-level report parts are test_material_api.py's."""
import ctypes
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ar  # noqa: E402
import meshes  # noqa: E402
from test_cli_render import make_recording  # noqa: E402  (the ERROR-entry recording: a rejected mesh)
from test_material_api import Graph  # noqa: E402  (graphs with unsupported-node strings)

CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
SIZE, SPP = 8, 4
SLOT_BASE_WEIGHT = 0  # first record scalar (src/kernel/vm/ops/store_slot.h, AR_RECORD_SCALARS order)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestV15(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (CLI, ar.DLL, os.path.join(ar.GEN, "_abi.py"), os.path.join(ar.GEN, "_ops.py")):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")

    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.dir = d.name
        self.session = None

    # -- scene ---------------------------------------------------------------------------------
    def scene(self, name="s.arrec"):
        """An ar.Scene over a camera-filling plane under a constant world; no material yet."""
        s = ar.Scene(os.path.join(self.dir, name))
        self.addCleanup(s.__exit__, None, None, None)
        self.plane = s.key()
        s.mesh_from(self.plane, meshes.plane(8.0, 8.0), "plane")
        s.world((1.0, 1.0, 1.0), 1.0)
        s.settings(SPP, SIZE, SIZE, seed=3, sampler="independent")
        s.camera((0.0, 0.0, 3.0), (0.0, 0.0, 0.0), 35.0, 36.0, SIZE, SIZE, up=(0.0, 1.0, 0.0))
        return s

    def bad_material(self, s):
        """A magenta material: two instances of one unsupported node type, one of another."""
        g = Graph(s.abi, s.op_id)
        g.add("UNSUPPORTED", string="ShaderNodeBevel")
        g.add("UNSUPPORTED", string="ShaderNodeBevel")
        g.add("UNSUPPORTED", string="ShaderNodeAmbientOcclusion")
        g.store(g.add("CONST_F32", f32=(1.0,)), SLOT_BASE_WEIGHT)
        gkey, key = s.key(), s.key()
        s._call(s.lib.ar_edit_set_graph(s.edit_h, gkey, s.abi.AR_TARGET_SURFACE, ctypes.byref(g.desc())), "set_graph")
        m = s.abi.ar_material_desc()
        m.surface_graph = gkey
        s._call(s.lib.ar_edit_set_material(s.edit_h, key, ctypes.byref(m)), "set_material")
        return key

    def render(self, s):
        """Commits the scene's edit, starts a viewport session and settles it."""
        a, lib = s.abi, s.lib
        gen = ctypes.c_uint64()
        s._call(lib.ar_edit_commit(s.edit_h, ctypes.byref(gen)), "ar_edit_commit")
        sd = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, SIZE, SIZE, a.AR_DENOISER_NONE)
        sh = ctypes.c_void_p()
        s._call(lib.ar_session_create(s.scene_h, ctypes.byref(sd), ctypes.byref(sh)), "ar_session_create")
        s.session_h = sh
        self.session = sh
        self.addCleanup(self.destroy_session, s)
        self.view = a.ar_view_desc()
        ctypes.memmove(ctypes.byref(self.view.camera), ctypes.byref(s._camera), ctypes.sizeof(a.ar_camera_desc))
        self.view.region_width, self.view.region_height, self.view.render_scale = SIZE, SIZE, 1.0
        s._call(lib.ar_session_set_view(sh, ctypes.byref(self.view)), "ar_session_set_view")
        s._call(lib.ar_session_start(sh), "ar_session_start")
        self.settle(s, gen.value, -1)

    def settle(self, s, gen, after_epoch):
        a = s.abi
        end = time.perf_counter() + 60.0
        while True:
            p = a.ar_progress()
            self.assertEqual(s.lib.ar_session_get_progress(self.session, ctypes.byref(p)), a.AR_OK)
            self.assertNotEqual(p.state, a.AR_STATE_FAILED)
            if p.state == a.AR_STATE_CONVERGED and p.active_generation == gen and p.epoch > after_epoch:
                return p
            self.assertLess(time.perf_counter(), end, f"generation {gen} never converged")
            s.lib.ar_session_wait(self.session, 20)

    def destroy_session(self, s):
        if not self.session:
            return
        s.lib.ar_session_close(self.session)
        end = time.perf_counter() + 20.0
        p = s.abi.ar_progress()
        while time.perf_counter() < end:
            s.lib.ar_session_get_progress(self.session, ctypes.byref(p))
            if p.state == s.abi.AR_STATE_CLOSED:
                break
            s.lib.ar_session_wait(self.session, 10)
        s.lib.ar_session_destroy(self.session)
        self.session = None
        s.session_h = ctypes.c_void_p()

    def edit(self, s, change):
        """One edit through `change(scene)`; settles it in a new epoch and returns the progress."""
        a, lib = s.abi, s.lib
        p = a.ar_progress()
        lib.ar_session_get_progress(self.session, ctypes.byref(p))
        s._call(lib.ar_scene_edit_begin(s.scene_h, ctypes.byref(s.edit_h)), "ar_scene_edit_begin")
        change(s)
        gen = ctypes.c_uint64()
        s._call(lib.ar_edit_commit(s.edit_h, ctypes.byref(gen)), "ar_edit_commit")
        return self.settle(s, gen.value, p.epoch)

    def report(self, s):
        """(revision, [(code, level, subject, count, detail)]) of the session's report."""
        a, lib = s.abi, s.lib
        info = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(self.session, ctypes.byref(info)), a.AR_OK)
        rows = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            self.assertEqual(lib.ar_session_report_entry(self.session, i, ctypes.byref(e)), a.AR_OK)
            rows.append((e.code, e.level, e.subject, e.count, bytes(e.detail).split(b"\0")[0].decode()))
        return info.revision, rows

    def counts(self, s, code):
        return [r[3] for r in self.report(s)[1] if r[0] == code]

    # -- tests ---------------------------------------------------------------------------------
    def test_replacing_the_magenta_material_removes_its_entries(self):
        s = self.scene()
        bad = self.bad_material(s)
        s.instances([{"geom": self.plane, "materials": [bad]}])
        self.render(s)
        code = s.abi.AR_REPORT_NODE_UNSUPPORTED
        rev, rows = self.report(s)
        self.assertEqual(sorted(r[4] for r in rows if r[0] == code), ["ShaderNodeAmbientOcclusion", "ShaderNodeBevel"])
        self.assertTrue(all(r[2] == bad for r in rows if r[0] == code))

        def replace(scene):
            g = scene.graph()
            g.store("base_weight", 1.0)
            good = scene.material_from_graph(g)
            scene.instances([{"geom": self.plane, "materials": [good]}])
            scene._call(scene.lib.ar_edit_remove(scene.edit_h, scene.abi.AR_KIND_MATERIAL, bad), "ar_edit_remove")

        self.edit(s, replace)
        rev2, rows = self.report(s)
        self.assertEqual([r for r in rows if r[0] == code], [])
        self.assertNotEqual(rev2, rev, "the report revision did not move with the entries")

    def cli(self, rec):
        out = os.path.join(self.dir, "out.fits")
        return subprocess.run([CLI, "render", rec, "-o", out], capture_output=True, text=True, cwd=self.dir, timeout=120)

    def test_cli_exit_status_follows_error_entries(self):
        s = self.scene("warn.arrec")
        bad = self.bad_material(s)
        s.instances([{"geom": self.plane, "materials": [bad]}])
        s.finish()
        r = self.cli(os.path.join(self.dir, "warn.arrec"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)  # a WARNING entry alone is not a failure
        code = s.abi.AR_REPORT_NODE_UNSUPPORTED
        self.assertRegex(r.stdout, r"(?m)^WARNING %d %d \d+ ShaderNodeBevel$" % (code, bad))
        a, ops = ar_abi(), load_module("_ops_v15", os.path.join(ar.GEN, "_ops.py"))
        lib = ctypes.CDLL(ar.DLL)
        a.check(lib)
        a.bind(lib)
        err = os.path.join(self.dir, "err.arrec")
        make_recording(a, ops, lib, err, os.path.join(self.dir, "err.log"), bad_generation=True)
        r = self.cli(err)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"(?m)^ERROR %d " % a.AR_REPORT_GENERATION_REJECTED)


_ABI = None


def ar_abi():
    global _ABI
    if _ABI is None:
        _ABI = load_module("_abi_v15", os.path.join(ar.GEN, "_abi.py"))
    return _ABI


if __name__ == "__main__":
    unittest.main()
