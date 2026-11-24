# SPDX-License-Identifier: Apache-2.0
"""The test medium through the generated binding on the CPU device (task 181; 0006 A.3 and D20,
0007 appendix ar_medium_desc): ar_edit_set_medium creates, replaces and (through ar_edit_remove) removes
a medium; an instance names it with ar_instances_desc.medium_keys; an unknown key or a medium removed
while instanced fails the edit; a medium-bearing mesh that is open or inconsistently wound raises one
MEDIUM_MESH_OPEN report entry naming it, a closed one none; the slab scene builds and replays."""
import ctypes
import importlib.util
import os
import subprocess
import sys
import tempfile
import time
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ABI = os.path.join(ROOT, "out", "build", "gen", "_abi.py")
SCENES_LIB = os.path.join(ROOT, "scenes", "lib")

CUBE, OPEN_CUBE, FLIPPED_CUBE = 10, 11, 12
MEDIUM_A, MEDIUM_B = 100, 101


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def cube_triangles():
    """The 12 triangles of scenes/lib/meshes.py's box, as (positions, corner_vert): closed, shared
    vertex indices, counter-clockwise from outside."""
    sys.path.insert(0, SCENES_LIB)
    try:
        import meshes
    finally:
        sys.path.remove(SCENES_LIB)
    m = meshes.box(1.0, 1.0, 1.0)
    return m.positions, m.corner_vert.reshape(-1, 3)


class MediumApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, ABI):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load_module("_abi_under_test", ABI)
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
        sdesc = a.ar_scene_desc(None, 2, 0)
        self.scene = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_create(self.ctx, ctypes.byref(sdesc), ctypes.byref(self.scene)), a.AR_OK)
        self.addCleanup(lib.ar_scene_destroy, self.scene)
        vdesc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, 8, 8, a.AR_DENOISER_NONE)
        self.session = ctypes.c_void_p()
        self.assertEqual(lib.ar_session_create(self.scene, ctypes.byref(vdesc), ctypes.byref(self.session)), a.AR_OK)
        self.addCleanup(self.destroy_session)
        self.assertEqual(lib.ar_session_start(self.session), a.AR_OK)
        self.assertEqual(lib.ar_session_wait(self.session, 20000), a.AR_OK)

    def destroy_session(self):
        self.lib.ar_session_close(self.session)
        end = time.perf_counter() + 20.0
        while self.progress().state != self.abi.AR_STATE_CLOSED and time.perf_counter() < end:
            self.lib.ar_session_wait(self.session, 10)
        self.lib.ar_session_destroy(self.session)

    def progress(self):
        p = self.abi.ar_progress()
        self.assertEqual(self.lib.ar_session_get_progress(self.session, ctypes.byref(p)), self.abi.AR_OK)
        return p

    # -- one edit ------------------------------------------------------------------------------------
    def begin(self):
        e = ctypes.c_void_p()
        self.assertEqual(self.lib.ar_scene_edit_begin(self.scene, ctypes.byref(e)), self.abi.AR_OK)
        return e

    def last_error(self, e):
        buf = (ctypes.c_uint8 * 512)()
        self.assertEqual(self.lib.ar_edit_last_error(e, 512, buf), self.abi.AR_OK)
        return bytes(buf).split(b"\0")[0].decode()

    def medium_desc(self, sigma_a=(0.5, 1.0, 2.0), sigma_s=(0.0, 0.0, 0.0), g=0.0, flags=0):
        d = self.abi.ar_medium_desc()
        d.sigma_a = (ctypes.c_float * 3)(*sigma_a)
        d.sigma_s = (ctypes.c_float * 3)(*sigma_s)
        d.g, d.flags = g, flags
        return d

    def set_medium(self, e, key, **kw):
        return self.lib.ar_edit_set_medium(e, key, ctypes.byref(self.medium_desc(**kw)))

    def mesh(self, e, key, positions, tris, name):
        a, lib = self.abi, self.lib
        positions = np.ascontiguousarray(positions, dtype=np.float32).reshape(-1, 3)
        corner_vert = np.ascontiguousarray(tris, dtype=np.int32).reshape(-1)
        d = a.ar_mesh_desc()
        d.key, d.vertex_count, d.corner_count, d.face_count = key, len(positions), corner_vert.size, corner_vert.size // 3
        d.motion_steps = 1
        d.name = (ctypes.c_uint8 * 64)(*(name.encode() + b"\0"))
        b = a.ar_mesh_buffers()
        self.assertEqual(lib.ar_edit_mesh_begin(e, ctypes.byref(d), ctypes.byref(b)), a.AR_OK)

        def fill(slot, count, values):
            ptr = ctypes.cast(slot, ctypes.POINTER(ctypes.c_void_p))[0]
            dst = np.frombuffer((ctypes.c_char * (count * values.itemsize)).from_address(ptr), dtype=values.dtype)
            dst[:] = values.reshape(-1)

        fill(b.positions, b.positions_count, positions)
        fill(b.corner_vert, b.corner_vert_count, corner_vert)
        fill(b.face_start, b.face_start_count, np.arange(corner_vert.size // 3, dtype=np.int32) * 3)
        fill(b.corner_normals, b.corner_normals_count, np.tile(np.float32([0, 0, 1]), (corner_vert.size, 1)))
        self.assertEqual(lib.ar_edit_item_commit(e, key), a.AR_OK)

    def instances(self, e, items):
        """items: (geometry key, object key, medium key or None); None for every item sends no array."""
        a = self.abi
        n = len(items)
        d = a.ar_instances_desc()
        d.instance_count, d.motion_steps, d.transform_count, d.material_key_count = n, 1, n, 0

        def arr(ctype, values):
            return (ctype * max(1, len(values)))(*values)

        ident = a.ar_xform()
        for i in (0, 5, 10):
            ident.m[i] = 1.0
        keep = dict(
            geom=arr(ctypes.c_uint64, [it[0] for it in items]), xf=(a.ar_xform * n)(*([ident] * n)),
            obj=arr(ctypes.c_uint64, [it[1] for it in items]), z32=arr(ctypes.c_uint32, [0] * n),
            col=(a.ar_rgba * n)(*[a.ar_rgba((ctypes.c_float * 4)(1, 1, 1, 1))] * n),
            vis=arr(ctypes.c_uint32, [0x3F] * n), lg=arr(ctypes.c_uint8, [0] * n), keys=arr(ctypes.c_uint64, []),
            med=arr(ctypes.c_uint64, [it[2] or 0 for it in items]))
        d.geom_keys, d.xf, d.object_keys = keep["geom"], keep["xf"], keep["obj"]
        d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = keep["z32"]
        d.colors, d.visibility, d.flags, d.light_groups = keep["col"], keep["vis"], keep["z32"], keep["lg"]
        d.material_offsets = d.material_counts = keep["z32"]
        d.material_keys = keep["keys"]
        if any(it[2] is not None for it in items):
            d.medium_keys = keep["med"]
        self.assertEqual(self.lib.ar_edit_set_instances(e, ctypes.byref(d)), a.AR_OK)

    def commit(self, e):
        """Commits and waits for the generation to settle; returns (generation, became_active)."""
        a = self.abi
        gen = ctypes.c_uint64()
        self.assertEqual(self.lib.ar_edit_commit(e, ctypes.byref(gen)), a.AR_OK)
        end = time.perf_counter() + 20.0
        while True:
            p = self.progress()
            if p.active_generation >= gen.value:
                return gen.value, True
            if p.failed_generation >= gen.value:
                return gen.value, False
            self.assertLess(time.perf_counter(), end, f"generation {gen.value} never settled")
            time.sleep(0.005)

    def entries(self, code, subject=None):
        """(subject, count, detail) of the session report's entries of `code`."""
        a, lib = self.abi, self.lib
        info = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(self.session, ctypes.byref(info)), a.AR_OK)
        out = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            self.assertEqual(lib.ar_session_report_entry(self.session, i, ctypes.byref(e)), a.AR_OK)
            if e.code == code and (subject is None or e.subject == subject):
                out.append((e.subject, e.count, bytes(e.detail).split(b"\0")[0].decode()))
        return out

    def meshes_edit(self):
        """One edit with the closed cube, the cube without its last two triangles, and the cube with one
        triangle flipped; returns after the generation is active."""
        positions, tris = cube_triangles()
        e = self.begin()
        self.mesh(e, CUBE, positions, tris, "closed_cube")
        self.mesh(e, OPEN_CUBE, positions, tris[:-2], "open_cube")
        flipped = tris.copy()
        flipped[0] = flipped[0][::-1]
        self.mesh(e, FLIPPED_CUBE, positions, flipped, "flipped_cube")
        self.assertTrue(self.commit(e)[1])

    # -- tests -----------------------------------------------------------------------------------------
    def test_create_and_replace(self):
        a, lib = self.abi, self.lib
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_A), a.AR_OK)
        self.assertEqual(self.set_medium(e, MEDIUM_B, sigma_s=(1.0, 2.0, 3.0), g=0.3), a.AR_OK)
        self.assertTrue(self.commit(e)[1])
        # Replacing a key in a later edit is allowed, with other coefficients.
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_A, sigma_a=(4.0, 0.0, 0.5)), a.AR_OK)
        self.assertTrue(self.commit(e)[1])
        # An out-of-range anisotropy is clamped and reported against the medium, and the report goes
        # when the medium is replaced by one that needs no clamp.
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_B, sigma_s=(1.0, 2.0, 3.0), g=1.5), a.AR_OK)
        self.assertTrue(self.commit(e)[1])
        got = self.entries(a.AR_REPORT_NODE_APPROXIMATED, MEDIUM_B)
        self.assertEqual(len(got), 1)
        self.assertIn("anisotropy clamped", got[0][2])
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_B, sigma_s=(1.0, 2.0, 3.0), g=0.5), a.AR_OK)
        self.assertTrue(self.commit(e)[1])
        self.assertEqual(self.entries(a.AR_REPORT_NODE_APPROXIMATED, MEDIUM_B), [])
        # The same key twice in one edit is the edit's usual duplicate-key refusal.
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_A), a.AR_OK)
        self.assertEqual(self.set_medium(e, MEDIUM_A), a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(lib.ar_edit_abort(e), a.AR_OK)

    def test_refused_descriptors(self):
        a = self.abi
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_A, flags=1), a.AR_ERR_UNSUPPORTED)
        self.assertIn("flags 1", self.last_error(e))
        for kw in (dict(sigma_a=(-1.0, 0.0, 0.0)), dict(sigma_s=(0.0, float("nan"), 0.0)),
                   dict(sigma_a=(float("inf"), 0.0, 0.0)), dict(g=float("nan"))):
            self.assertEqual(self.set_medium(e, MEDIUM_A, **kw), a.AR_ERR_INVALID_ARGUMENT, kw)
        self.assertEqual(self.lib.ar_edit_set_medium(e, MEDIUM_A, None), a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(self.set_medium(e, 0), a.AR_ERR_INVALID_ARGUMENT)
        self.assertIn("key 0", self.last_error(e))
        self.assertEqual(self.lib.ar_edit_abort(e), a.AR_OK)

    def test_remove(self):
        a, lib = self.abi, self.lib
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_A), a.AR_OK)
        self.assertTrue(self.commit(e)[1])
        e = self.begin()
        self.assertEqual(lib.ar_edit_remove(e, a.AR_KIND_MEDIUM, MEDIUM_B), a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(lib.ar_edit_remove(e, a.AR_KIND_MEDIUM, MEDIUM_A), a.AR_OK)
        self.assertTrue(self.commit(e)[1])
        # Gone: removing it again is a remove of an unknown key.
        e = self.begin()
        self.assertEqual(lib.ar_edit_remove(e, a.AR_KIND_MEDIUM, MEDIUM_A), a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(lib.ar_edit_abort(e), a.AR_OK)

    def test_instance_medium_keys(self):
        a, lib = self.abi, self.lib
        positions, tris = cube_triangles()
        e = self.begin()
        self.mesh(e, CUBE, positions, tris, "closed_cube")
        self.assertEqual(self.set_medium(e, MEDIUM_A), a.AR_OK)
        self.instances(e, [(CUBE, 1000, MEDIUM_A), (CUBE, 1001, None)])
        self.assertTrue(self.commit(e)[1], "an instance may name a medium of its own edit")
        # An unknown medium key fails the generation, naming the instance and the key.
        e = self.begin()
        self.instances(e, [(CUBE, 1000, 777)])
        gen, ok = self.commit(e)
        self.assertFalse(ok)
        rejected = self.entries(a.AR_REPORT_GENERATION_REJECTED, 1000)
        self.assertEqual(len(rejected), 1)
        self.assertIn("unknown medium key 777", rejected[0][2])
        # A medium removed in an edit that leaves it instanced fails too, and the old table stays.
        e = self.begin()
        self.assertEqual(lib.ar_edit_remove(e, a.AR_KIND_MEDIUM, MEDIUM_A), a.AR_OK)
        gen, ok = self.commit(e)
        self.assertFalse(ok)
        # Removing it together with the instances that name it is fine.
        e = self.begin()
        self.assertEqual(lib.ar_edit_remove(e, a.AR_KIND_MEDIUM, MEDIUM_A), a.AR_OK)
        self.instances(e, [(CUBE, 1001, None)])
        self.assertTrue(self.commit(e)[1])

    def test_open_mesh_warns(self):
        a = self.abi
        self.meshes_edit()
        e = self.begin()
        self.assertEqual(self.set_medium(e, MEDIUM_A), a.AR_OK)
        # The closed cube twice (two instances share the mesh), the open and the flipped cube once each,
        # and an open cube with no medium.
        self.instances(e, [(CUBE, 1000, MEDIUM_A), (CUBE, 1001, MEDIUM_A), (OPEN_CUBE, 1002, MEDIUM_A),
                           (FLIPPED_CUBE, 1003, MEDIUM_A), (OPEN_CUBE, 1004, None)])
        self.assertTrue(self.commit(e)[1], "an open mesh still renders: the edit is not refused")
        self.assertEqual(self.entries(a.AR_REPORT_MEDIUM_MESH_OPEN, CUBE), [])
        for key, name in ((OPEN_CUBE, "open_cube"), (FLIPPED_CUBE, "flipped_cube")):
            got = self.entries(a.AR_REPORT_MEDIUM_MESH_OPEN, key)
            self.assertEqual(len(got), 1, name)
            self.assertEqual(got[0][1], 1, f"{name} is reported once")
            self.assertIn(name, got[0][2])
        # Setting the instances again does not count the open mesh a second time.
        e = self.begin()
        self.instances(e, [(OPEN_CUBE, 1002, MEDIUM_A)])
        self.assertTrue(self.commit(e)[1])
        got = self.entries(a.AR_REPORT_MEDIUM_MESH_OPEN, OPEN_CUBE)
        self.assertEqual([c for _, c, _ in got], [1])
        self.assertEqual(self.entries(a.AR_REPORT_MEDIUM_MESH_OPEN, FLIPPED_CUBE), [])
        # Without its medium the open mesh is no longer reported.
        e = self.begin()
        self.instances(e, [(OPEN_CUBE, 1002, None)])
        self.assertTrue(self.commit(e)[1])
        self.assertEqual(self.entries(a.AR_REPORT_MEDIUM_MESH_OPEN), [])

    def test_slab_scene_builds_and_replays(self):
        """scenes/v1_slab builds a recording through the ABI, and the replayer plays it back to the recorded statuses."""
        with tempfile.TemporaryDirectory() as d:
            rec = os.path.join(d, "v1_slab.arrec")
            build = os.path.join(ROOT, "scenes", "v1_slab", "build.py")
            r = subprocess.run([sys.executable, "-I", build, "--out", rec], capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertGreater(os.path.getsize(rec), 0)
            driver = os.path.join(ROOT, "out", "bin", "replay_driver.exe")
            r = subprocess.run([driver, rec, os.path.join(d, "replay.log")], capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn("matched 0", r.stdout)

if __name__ == "__main__":
    unittest.main()
