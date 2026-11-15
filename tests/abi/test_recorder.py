# SPDX-License-Identifier: Apache-2.0
"""The ABI recorder, the .arrec decoder and the generated replayer (task 033; 0001 D1.4, V11)."""
import ctypes
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
DRIVER = os.path.join(ROOT, "out", "bin", "replay_driver.exe")
GEN = os.path.join(ROOT, "out", "build", "gen")
HEADER = os.path.join(ROOT, "include", "astroray", "astroray.h")

QUAD = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], dtype=np.float32)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def query_names():
    with open(HEADER, encoding="utf-8") as f:
        return set(re.findall(r"AR_API AR_QUERY ar_status (\w+)\(", f.read()))


def name_bytes(s, n):
    return (ctypes.c_uint8 * n)(*s.encode().ljust(n, b"\0"))


class TestRecorder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, DRIVER, os.path.join(GEN, "_abi.py"), os.path.join(GEN, "arrec.py")):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load("_abi_rec", os.path.join(GEN, "_abi.py"))
        cls.arrec = load("_arrec_rec", os.path.join(GEN, "arrec.py"))
        cls.lib = ctypes.CDLL(DLL)
        cls.abi.check(cls.lib)
        cls.abi.bind(cls.lib)

    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.dir = d.name

    def view(self, slot, count, dtype):
        # `slot` is the T** field: the engine-owned slot that holds the array pointer.
        ptr = ctypes.cast(slot, ctypes.POINTER(ctypes.c_void_p))[0]
        n = count * np.dtype(dtype).itemsize
        return np.frombuffer((ctypes.c_char * n).from_address(ptr), dtype=dtype)

    def record(self, path):
        """Issues the calls of the task's list, plus queries that must not be stored."""
        a, lib = self.abi, self.lib
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, os.path.join(self.dir, "ctx.log").encode(), path.encode())
        ctx = ctypes.c_void_p()
        self.assertEqual(lib.ar_context_create(ctypes.byref(desc), ctypes.byref(ctx)), a.AR_OK)
        n = ctypes.c_uint32()
        self.assertEqual(lib.ar_device_count(ctypes.byref(n)), a.AR_OK)
        buf = (ctypes.c_uint8 * 64)()
        self.assertEqual(lib.ar_context_last_error(ctx, 64, buf), a.AR_OK)
        scene = ctypes.c_void_p()
        sd = a.ar_scene_desc(None, 2, 0)
        self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(sd), ctypes.byref(scene)), a.AR_OK)
        edit = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)

        s = a.ar_settings()
        names = (a.ar_name * 2)(a.ar_name(name_bytes("Combined", 64)), a.ar_name(name_bytes("Depth", 64)))
        wl = (ctypes.c_float * 3)(400.0, 550.0, 700.0)
        val = (ctypes.c_float * 3)(0.1, 1.0, 0.2)
        ch = (a.ar_response_channel * 1)()
        ch[0].name = name_bytes("sensor", 32)
        ch[0].curve_count = 3
        ch[0].wavelength_nm = ctypes.cast(wl, ctypes.POINTER(ctypes.c_float))
        ch[0].value = ctypes.cast(val, ctypes.POINTER(ctypes.c_float))
        s.passes = ctypes.cast(names, ctypes.POINTER(a.ar_name))
        s.pass_count = 2
        s.channels = ctypes.cast(ch, ctypes.POINTER(a.ar_response_channel))
        s.channel_count = 1
        s.output_width, s.output_height, s.samples, s.seed = 64, 48, 16, 7
        self.assertEqual(lib.ar_edit_set_settings(edit, ctypes.byref(s)), a.AR_OK)

        astro = a.ar_astro_desc()
        astro.astro_kind = a.AR_ASTRO_BLACK_HOLE
        for i in (0, 5, 10):
            astro.xf.m[i] = 1.0
        astro.mass, astro.spin, astro.meters_per_rg = 4.0e6, 0.5, 1.0e3
        self.assertEqual(lib.ar_edit_set_astro(edit, 7, ctypes.byref(astro)), a.AR_OK)

        md = a.ar_mesh_desc()
        md.key, md.vertex_count, md.corner_count, md.face_count, md.motion_steps = 3, 4, 4, 1, 1
        md.name = name_bytes("quad", 64)
        b = a.ar_mesh_buffers()
        self.assertEqual(lib.ar_edit_mesh_begin(edit, ctypes.byref(md), ctypes.byref(b)), a.AR_OK)
        self.view(b.positions, b.positions_count, np.float32)[:] = QUAD.ravel()
        self.view(b.corner_vert, b.corner_vert_count, np.int32)[:] = (0, 1, 2, 3)
        self.view(b.face_start, b.face_start_count, np.int32)[:] = [0]
        self.view(b.corner_normals, b.corner_normals_count, np.float32)[:] = np.tile([0, 0, 1], 4)
        self.assertEqual(lib.ar_edit_item_commit(edit, 3), a.AR_OK)
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(gen)), a.AR_OK)
        self.assertEqual(lib.ar_edit_last_error(edit, 64, buf), a.AR_OK)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)
        self.assertEqual(lib.ar_context_destroy(ctx), a.AR_OK)

    def recorded(self, name="first.arrec"):
        path = os.path.join(self.dir, name)
        self.record(path)
        return path, self.arrec.decode(path)

    def test_no_query_calls_recorded(self):
        path, (header, calls) = self.recorded()
        names = [n for n, _ in calls]
        print("decoded call list:")
        for n in names:
            print("  " + n)
        self.assertEqual(names, [
            "ar_context_create", "ar_scene_create", "ar_scene_edit_begin", "ar_edit_set_settings",
            "ar_edit_set_astro", "ar_edit_mesh_begin", "ar_edit_item_commit", "ar_edit_commit",
            "ar_scene_destroy", "ar_context_destroy"])
        self.assertFalse(set(names) & query_names())
        self.assertEqual(header["abi_hash"], self.abi.ABI_HASH)
        # Handles are ids in creation order, the same id wherever the handle is used.
        by = {n: a for n, a in calls}
        self.assertEqual(by["ar_scene_create"]["ctx"], 1)
        self.assertEqual(by["ar_scene_edit_begin"]["scene"], 2)
        self.assertEqual({by[n]["edit"] for n in ("ar_edit_set_astro", "ar_edit_mesh_begin", "ar_edit_commit")}, {3})
        # Nested arrays and the staged payload were captured by value.
        st = by["ar_edit_set_settings"]["settings"]
        self.assertEqual([bytes(p["text"]).rstrip(b"\0") for p in st["passes"]], [b"Combined", b"Depth"])
        self.assertEqual(st["channels"][0]["value"], list(struct.unpack("<3f", struct.pack("<3f", 0.1, 1.0, 0.2))))
        staged = {f: (dt, n, data) for f, dt, n, data in by["ar_edit_item_commit"]["staged"]}
        dt, n, data = staged["positions"]
        self.assertEqual((dt, n), ("float32", 12))
        np.testing.assert_array_equal(np.frombuffer(data, dtype=np.float32), QUAD.ravel())
        np.testing.assert_array_equal(np.frombuffer(staged["corner_vert"][2], dtype=np.int32), [0, 1, 2, 3])

    def run_driver(self, *args):
        return subprocess.run([DRIVER, *args], capture_output=True, text=True)

    def test_hash_mismatch_refused(self):
        path, _ = self.recorded()
        bad = os.path.join(self.dir, "bad.arrec")
        shutil.copy(path, bad)
        with open(bad, "r+b") as f:
            f.seek(8)
            h = struct.unpack("<Q", f.read(8))[0]
            f.seek(8)
            f.write(struct.pack("<Q", h ^ 1))
        r = self.run_driver(bad, os.path.join(self.dir, "replay.log"))
        print(r.stdout.strip())
        self.assertEqual(r.returncode, self.abi.AR_ERR_ABI_MISMATCH)
        self.assertIn(f"0x{h ^ 1:016x}", r.stdout)
        self.assertIn(f"0x{self.abi.ABI_HASH:016x}", r.stdout)
        # The untouched file replays.
        ok = self.run_driver(path, os.path.join(self.dir, "replay2.log"))
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)

    def replay_identical(self, path, first):
        """Replays path with recording on; the re-recording decodes to the same calls (arguments,
        staged payloads and returned statuses). Returns the second call list."""
        second_path = os.path.join(self.dir, "second.arrec")
        r = self.run_driver(path, os.path.join(self.dir, "replay.log"), second_path)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)  # every status matched the recording
        _, second = self.arrec.decode(second_path)
        self.assertEqual([n for n, _ in first], [n for n, _ in second])
        for (name, x), (_, y) in zip(first, second):
            if name == "ar_context_create":  # the replay chooses its own log and record paths
                x, y = dict(x), dict(y)
                x["desc"] = {k: v for k, v in x["desc"].items() if not k.endswith("_path")}
                y["desc"] = {k: v for k, v in y["desc"].items() if not k.endswith("_path")}
            self.assertEqual(x, y, name)
        return second

    def test_replay_rerecords_identical(self):
        path, (_, first) = self.recorded()
        second = self.replay_identical(path, first)
        staged = [a["staged"] for n, a in second if n == "ar_edit_item_commit"][0]
        self.assertEqual(len(staged), 7)
        self.assertEqual(np.frombuffer(staged[0][3], dtype=np.float32).tolist(), QUAD.ravel().tolist())

    def open_scene(self, path):
        a, lib = self.abi, self.lib
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, os.path.join(self.dir, "ctx2.log").encode(), path.encode())
        ctx = ctypes.c_void_p()
        self.assertEqual(lib.ar_context_create(ctypes.byref(desc), ctypes.byref(ctx)), a.AR_OK)
        return ctx

    def stage_quad(self, edit, key, scale):
        a, lib = self.abi, self.lib
        md = a.ar_mesh_desc()
        md.key, md.vertex_count, md.corner_count, md.face_count, md.motion_steps = key, 4, 4, 1, 1
        md.name = name_bytes("quad", 64)
        b = a.ar_mesh_buffers()
        self.assertEqual(lib.ar_edit_mesh_begin(edit, ctypes.byref(md), ctypes.byref(b)), a.AR_OK)
        self.view(b.positions, b.positions_count, np.float32)[:] = (QUAD * scale).ravel()
        self.view(b.corner_vert, b.corner_vert_count, np.int32)[:] = (0, 1, 2, 3)
        self.view(b.face_start, b.face_start_count, np.int32)[:] = [0]
        self.view(b.corner_normals, b.corner_normals_count, np.float32)[:] = np.tile([0, 0, 1], 4)

    def test_refused_edit_commit_records_open_staged_items(self):
        # An item that was begun and filled but never item-committed is captured at the edit
        # commit, in begin order, even though the engine refuses that commit; the recording keeps
        # the arrays and the refusal, and the replay reproduces both.
        a, lib = self.abi, self.lib
        path = os.path.join(self.dir, "open.arrec")
        ctx = self.open_scene(path)
        scene, edit = ctypes.c_void_p(), ctypes.c_void_p()
        sd = a.ar_scene_desc(None, 1, 0)
        self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(sd), ctypes.byref(scene)), a.AR_OK)
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.stage_quad(edit, 9, 3.0)
        self.stage_quad(edit, 4, 2.0)
        gen = ctypes.c_uint64()
        st = lib.ar_edit_commit(edit, ctypes.byref(gen))
        self.assertEqual(lib.ar_edit_abort(edit), a.AR_OK)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)
        self.assertEqual(lib.ar_context_destroy(ctx), a.AR_OK)
        _, calls = self.arrec.decode(path)
        commit = [x for n, x in calls if n == "ar_edit_commit"][0]
        # The engine refuses a commit with an item that was never item-committed (0007 d9).
        self.assertEqual(st, a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(commit["status"], st)
        self.assertEqual([k for k, _ in commit["staged_items"]], [9, 4])  # begin order, not key order
        for (key, items), scale in zip(commit["staged_items"], (3.0, 2.0)):
            np.testing.assert_array_equal(np.frombuffer(items[0][3], dtype=np.float32), (QUAD * scale).ravel())
        self.replay_identical(path, calls)

    def test_refused_commit_keeps_open_staged_items_for_retry(self):
        # A refused commit leaves the edit open (032): the item's buffers are still valid, so the
        # retry's item commit carries them, and only the commit that succeeds drops the entry.
        a, lib = self.abi, self.lib
        path = os.path.join(self.dir, "retry.arrec")
        ctx = self.open_scene(path)
        scene, edit = ctypes.c_void_p(), ctypes.c_void_p()
        sd = a.ar_scene_desc(None, 1, 0)
        self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(sd), ctypes.byref(scene)), a.AR_OK)
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.stage_quad(edit, 6, 5.0)
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(gen)), a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(lib.ar_edit_item_commit(edit, 6), a.AR_OK)
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(gen)), a.AR_OK)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)
        self.assertEqual(lib.ar_context_destroy(ctx), a.AR_OK)
        _, calls = self.arrec.decode(path)
        commits = [x for n, x in calls if n == "ar_edit_commit"]
        self.assertEqual([c["status"] for c in commits], [a.AR_ERR_INVALID_ARGUMENT, a.AR_OK])
        self.assertEqual([k for k, _ in commits[0]["staged_items"]], [6])
        self.assertEqual(commits[1]["staged_items"], [])
        item = [x for n, x in calls if n == "ar_edit_item_commit"][0]["staged"]
        np.testing.assert_array_equal(np.frombuffer(item[0][3], dtype=np.float32), (QUAD * 5.0).ravel())
        self.replay_identical(path, calls)

    def test_abort_drops_open_staged_items(self):
        # After an abort the first begin's buffers are gone: the next edit's item commit
        # carries only its own arrays, and the abort record carries none.
        a, lib = self.abi, self.lib
        path = os.path.join(self.dir, "abort.arrec")
        ctx = self.open_scene(path)
        scene, edit = ctypes.c_void_p(), ctypes.c_void_p()
        sd = a.ar_scene_desc(None, 1, 0)
        self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(sd), ctypes.byref(scene)), a.AR_OK)
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.stage_quad(edit, 5, 7.0)
        self.assertEqual(lib.ar_edit_abort(edit), a.AR_OK)
        edit2 = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit2)), a.AR_OK)
        self.stage_quad(edit2, 5, 1.0)
        self.assertEqual(lib.ar_edit_item_commit(edit2, 5), a.AR_OK)
        self.assertEqual(lib.ar_edit_commit(edit2, ctypes.byref(ctypes.c_uint64())), a.AR_OK)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)
        self.assertEqual(lib.ar_context_destroy(ctx), a.AR_OK)
        _, calls = self.arrec.decode(path)
        by = dict(calls)  # last call of each name
        self.assertEqual({k: v for k, v in calls if k == "ar_edit_abort"}["ar_edit_abort"]["status"], 0)
        np.testing.assert_array_equal(np.frombuffer(by["ar_edit_item_commit"]["staged"][0][3], dtype=np.float32),
                                      QUAD.ravel())
        self.assertEqual(by["ar_edit_commit"]["staged_items"], [])
        self.replay_identical(path, calls)

    def test_status_mismatch_reported(self):
        # The recording stores each call's status; a replay that returns another one says so.
        path, _ = self.recorded()
        bad = os.path.join(self.dir, "status.arrec")
        shutil.copy(path, bad)
        with open(bad, "r+b") as f:
            data = f.read()
            pos = 16 + len(self.arrec.decode(path)[0]["build"])
            while True:
                fn, flags, n = struct.unpack_from("<IIQ", data, pos)
                if self.arrec.FUNCS[fn][0] == "ar_edit_set_astro":
                    break
                pos += 16 + n
            self.assertEqual(flags, 1)  # AR_OK + 1
            f.seek(pos + 4)
            f.write(struct.pack("<I", 1 + self.abi.AR_ERR_UNSUPPORTED))
        r = self.run_driver(bad, os.path.join(self.dir, "replay.log"))
        print(r.stdout.strip())
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("matched 0", r.stdout)
        self.assertIn("ar_edit_set_astro", r.stdout)

    def test_unknown_status_counts_as_mismatch(self):
        path, _ = self.recorded()
        bad = os.path.join(self.dir, "nostatus.arrec")
        shutil.copy(path, bad)
        with open(bad, "r+b") as f:
            f.seek(8 + 8 + len(self.arrec.decode(path)[0]["build"]) + 4)  # flags of the first record
            f.write(struct.pack("<I", 0))
        r = self.run_driver(bad, os.path.join(self.dir, "replay.log"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("matched 0", r.stdout)
        self.assertIn("no status", r.stdout)

    def test_unopenable_record_path_is_logged(self):
        a, lib = self.abi, self.lib
        log = os.path.join(self.dir, "nopath.log")
        missing = os.path.join(self.dir, "no_such_dir", "x.arrec")
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, log.encode(), missing.encode())
        ctx = ctypes.c_void_p()
        self.assertEqual(lib.ar_context_create(ctypes.byref(desc), ctypes.byref(ctx)), a.AR_ERR_INVALID_ARGUMENT)
        with open(log, errors="replace") as f:
            self.assertIn("cannot open recording", f.read())

    def test_successful_remove_drops_open_staged_item(self):
        # remove(key) returning AR_OK frees that key's staged buffers, so the later item commit
        # carries no arrays. Without the drop the recorder would read them again.
        a, lib = self.abi, self.lib
        path = os.path.join(self.dir, "remove.arrec")
        ctx = self.open_scene(path)
        scene, edit = ctypes.c_void_p(), ctypes.c_void_p()
        sd = a.ar_scene_desc(None, 1, 0)
        self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(sd), ctypes.byref(scene)), a.AR_OK)
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.stage_quad(edit, 1, 1.0)
        self.assertEqual(lib.ar_edit_item_commit(edit, 1), a.AR_OK)
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(ctypes.c_uint64())), a.AR_OK)
        edit2 = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit2)), a.AR_OK)
        self.stage_quad(edit2, 1, 2.0)
        self.assertEqual(lib.ar_edit_remove(edit2, a.AR_KIND_GEOMETRY, 1), a.AR_OK)
        lib.ar_edit_item_commit(edit2, 1)  # whatever the engine says, the recorder holds no arrays now
        self.assertEqual(lib.ar_edit_abort(edit2), a.AR_OK)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)
        self.assertEqual(lib.ar_context_destroy(ctx), a.AR_OK)
        _, calls = self.arrec.decode(path)
        commits = [x for n, x in calls if n == "ar_edit_item_commit"]
        self.assertEqual(len(commits[0]["staged"]), 7)
        self.assertEqual(commits[1]["staged"], [])
        self.replay_identical(path, calls)

    def test_failed_create_keeps_ids_lined_up(self):
        a, lib = self.abi, self.lib
        path = os.path.join(self.dir, "fail.arrec")
        ctx = self.open_scene(path)
        bad_desc = a.ar_scene_desc(None, 300, 0)  # more than the 256 worker cap
        scene, edit = ctypes.c_void_p(), ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(bad_desc), ctypes.byref(scene)), a.AR_ERR_INVALID_ARGUMENT)
        sd = a.ar_scene_desc(None, 1, 0)
        self.assertEqual(lib.ar_scene_create(ctx, ctypes.byref(sd), ctypes.byref(scene)), a.AR_OK)
        self.assertEqual(lib.ar_scene_edit_begin(scene, ctypes.byref(edit)), a.AR_OK)
        self.stage_quad(edit, 2, 1.0)
        self.assertEqual(lib.ar_edit_item_commit(edit, 2), a.AR_OK)
        self.assertEqual(lib.ar_edit_commit(edit, ctypes.byref(ctypes.c_uint64())), a.AR_OK)
        self.assertEqual(lib.ar_scene_destroy(scene), a.AR_OK)
        self.assertEqual(lib.ar_context_destroy(ctx), a.AR_OK)
        _, calls = self.arrec.decode(path)
        creates = [x for n, x in calls if n == "ar_scene_create"]
        self.assertEqual([c["status"] for c in creates], [a.AR_ERR_INVALID_ARGUMENT, a.AR_OK])
        # ids: context 1, the failed create reserved 2, the good scene 3, its edit 4.
        by = dict(calls)
        self.assertEqual(by["ar_scene_edit_begin"]["scene"], 3)
        self.assertEqual(by["ar_edit_commit"]["edit"], 4)
        self.assertEqual(by["ar_scene_destroy"]["scene"], 3)
        second = self.replay_identical(path, calls)
        self.assertEqual(dict(second)["ar_edit_commit"]["edit"], 4)


if __name__ == "__main__":
    unittest.main()
