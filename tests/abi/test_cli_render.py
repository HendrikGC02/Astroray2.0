# SPDX-License-Identifier: Apache-2.0
"""astroray_cli render, dump and bench (task 069; 0001 D1.4, D2.10, D2.11, R38, R59).
The recording is made through the generated binding: a CPU scene with no geometry and a constant
world, one 4x4 viewport session at 16 spp, plus a second recording adds one world graph node the engine does not support."""
import ctypes
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
GEN = os.path.join(ROOT, "out", "build", "gen")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")
SCENES = os.path.join(ROOT, "out", "scenes")

RGB = (1.0, 0.5, 0.25)
STRENGTH = 0.8
SPP = 16
WORLD_GRAPH, BAD_MESH = 40, 41


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_recording(abi, ops, lib, path, ctx_log, bad_generation=False, close_session=False,
                   unsupported_node=False, no_settings=False, sessions=1, final=False):
    """Records the scene; with bad_generation a mesh the engine rejects is committed after the
    session was created (an ERROR report entry once it is validated)."""
    a = abi
    desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, ctx_log.encode(), path.encode())
    ctx = ctypes.c_void_p()
    assert lib.ar_context_create(ctypes.byref(desc), ctypes.byref(ctx)) == a.AR_OK
    scene = ctypes.c_void_p()
    assert lib.ar_scene_create(ctx, ctypes.byref(a.ar_scene_desc(None, 2, 0)), ctypes.byref(scene)) == a.AR_OK
    edit = ctypes.c_void_p()
    assert lib.ar_scene_edit_begin(scene, ctypes.byref(edit)) == a.AR_OK

    nodes, links, f32 = [], [], []

    def node(op, vals=(), string=None):
        n = a.ar_graph_node()
        n.op = ops.OP_ID[op]
        n.f32_first, n.f32_count = len(f32), len(vals)
        if string is not None:
            n.string_first, n.string_count = 0, 1
        f32.extend(vals)
        nodes.append(n)
        return len(nodes) - 1

    colour, strength, emit = node("CONST_F32X3", RGB), node("CONST_F32", (STRENGTH,)), node("EMIT_LE")
    links.append(a.ar_graph_link(colour, 0, emit, 0))
    links.append(a.ar_graph_link(strength, 0, emit, 1))
    strings = b""
    if unsupported_node:  # the engine reports it and renders the whole world magenta
        node("UNSUPPORTED", string="ShaderNodeBevel")
        strings = b"ShaderNodeBevel\0"
    g = a.ar_graph_desc()
    g.node_count, g.link_count, g.f32_count, g.string_bytes_count = len(nodes), len(links), len(f32), len(strings)
    keep = ((a.ar_graph_node * len(nodes))(*nodes), (a.ar_graph_link * len(links))(*links),
            (ctypes.c_float * len(f32))(*f32), (ctypes.c_int32 * 1)(0), (ctypes.c_uint8 * len(strings))(*strings))
    g.nodes, g.links, g.params_f32, g.params_i32, g.string_bytes = keep
    assert lib.ar_edit_set_graph(edit, WORLD_GRAPH, a.AR_TARGET_WORLD, ctypes.byref(g)) == a.AR_OK
    w = a.ar_world_desc()
    w.surface_graph = WORLD_GRAPH
    assert lib.ar_edit_set_world(edit, ctypes.byref(w)) == a.AR_OK

    s = a.ar_settings()
    s.samples = s.viewport_samples = SPP
    s.max_bounces = 4
    s.pixel_filter, s.pixel_filter_width, s.film_exposure, s.seed = a.AR_FILTER_BOX, 1.0, 1.0, 11
    if not no_settings:
        assert lib.ar_edit_set_settings(edit, ctypes.byref(s)) == a.AR_OK
    gen = ctypes.c_uint64()
    assert lib.ar_edit_commit(edit, ctypes.byref(gen)) == a.AR_OK

    mode = a.AR_SESSION_MODE_FINAL if final else a.AR_SESSION_MODE_VIEWPORT
    sdesc = a.ar_session_desc(mode, 4, 4, a.AR_DENOISER_NONE)
    made = []
    for _ in range(sessions):
        session = ctypes.c_void_p()
        made.append(session)
        assert lib.ar_session_create(scene, ctypes.byref(sdesc), ctypes.byref(session)) == a.AR_OK
        record_view(a, lib, session)
    if bad_generation:
        add_bad_mesh(a, lib, scene, edit, gen)
    if close_session:  # the addon closes its sessions; the replay must still render them
        assert lib.ar_session_close(session) == a.AR_OK
    # Destroys are recorded too; the CLI replays with keep_alive and owns the handles.
    for session in made:
        lib.ar_session_destroy(session)
    lib.ar_scene_destroy(scene)
    lib.ar_context_destroy(ctx)


def record_view(a, lib, session):
    v = a.ar_view_desc()
    v.region_width, v.region_height, v.render_scale = 4, 4, 1.0
    for i in (0, 5, 10):
        v.camera.cam_to_world.m[i] = 1.0
    for i, x in ((0, 0.5), (5, 0.375), (11, -1.0), (15, 1.0)):
        v.camera.projection_inverse[i] = x
    v.camera.type = a.AR_CAMERA_PERSP
    assert lib.ar_session_set_view(session, ctypes.byref(v)) == a.AR_OK
    assert lib.ar_session_start(session) == a.AR_OK


def add_bad_mesh(a, lib, scene, edit, gen):
    if True:
        quad = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], dtype=np.float32)
        assert lib.ar_scene_edit_begin(scene, ctypes.byref(edit)) == a.AR_OK
        d = a.ar_mesh_desc()
        d.key, d.vertex_count, d.corner_count, d.face_count, d.motion_steps = BAD_MESH, 4, 4, 1, 1
        d.name = (ctypes.c_uint8 * 64)(*b"quad".ljust(64, b"\0"))
        b = a.ar_mesh_buffers()
        assert lib.ar_edit_mesh_begin(edit, ctypes.byref(d), ctypes.byref(b)) == a.AR_OK

        def fill(slot, count, dtype, values):
            ptr = ctypes.cast(slot, ctypes.POINTER(ctypes.c_void_p))[0]
            arr = np.frombuffer((ctypes.c_char * (count * np.dtype(dtype).itemsize)).from_address(ptr), dtype=dtype)
            arr[:] = values

        fill(b.positions, b.positions_count, np.float32, quad.ravel())
        fill(b.corner_vert, b.corner_vert_count, np.int32, (0, 1, 9, 3))  # corner 2 is out of range
        fill(b.face_start, b.face_start_count, np.int32, [0])
        fill(b.corner_normals, b.corner_normals_count, np.float32, np.tile([0, 0, 1], 4))
        assert lib.ar_edit_item_commit(edit, BAD_MESH) == a.AR_OK
        assert lib.ar_edit_commit(edit, ctypes.byref(gen)) == a.AR_OK


class TestCliRender(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (CLI, DLL, os.path.join(GEN, "_abi.py"), os.path.join(GEN, "_ops.py")):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load("_abi_cli_render", os.path.join(GEN, "_abi.py"))
        cls.ops = load("_ops_cli_render", os.path.join(GEN, "_ops.py"))
        cls.fits = load("_fits_read_cli_render", FITS_READ)
        cls.lib = ctypes.CDLL(DLL)
        cls.abi.check(cls.lib)
        cls.abi.bind(cls.lib)
        cls.tmp = tempfile.TemporaryDirectory()
        os.makedirs(SCENES, exist_ok=True)
        cls.rec = os.path.join(SCENES, "069_empty_world.arrec")
        make_recording(cls.abi, cls.ops, cls.lib, cls.rec, os.path.join(cls.tmp.name, "rec.log"))
        cls.rec_closed = os.path.join(SCENES, "069_closed_session.arrec")
        make_recording(cls.abi, cls.ops, cls.lib, cls.rec_closed, os.path.join(cls.tmp.name, "rec3.log"),
                       close_session=True)
        cls.rec_unsupported = os.path.join(SCENES, "069_unsupported_world_node.arrec")
        make_recording(cls.abi, cls.ops, cls.lib, cls.rec_unsupported, os.path.join(cls.tmp.name, "rec4.log"),
                       unsupported_node=True)
        cls.rec_no_settings = os.path.join(SCENES, "069_no_settings.arrec")
        make_recording(cls.abi, cls.ops, cls.lib, cls.rec_no_settings, os.path.join(cls.tmp.name, "rec5.log"),
                       no_settings=True)
        cls.rec_two = os.path.join(SCENES, "069_two_sessions.arrec")
        make_recording(cls.abi, cls.ops, cls.lib, cls.rec_two, os.path.join(cls.tmp.name, "rec6.log"), sessions=2)
        cls.rec_final = os.path.join(SCENES, "197_final_session.arrec")
        make_recording(cls.abi, cls.ops, cls.lib, cls.rec_final, os.path.join(cls.tmp.name, "rec7.log"), final=True)
        cls.rec_bad = os.path.join(SCENES, "069_rejected_generation.arrec")
        make_recording(cls.abi, cls.ops, cls.lib, cls.rec_bad, os.path.join(cls.tmp.name, "rec2.log"),
                       bad_generation=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_cli(self, args, **env):
        e = dict(os.environ)
        e.update(env)
        return subprocess.run([CLI] + args, capture_output=True, text=True, env=e, cwd=self.tmp.name, timeout=120)

    def render(self, extra=(), cmd="render", rec=None, name="out.fits"):
        out = os.path.join(self.tmp.name, name)
        if os.path.exists(out):
            os.remove(out)
        r = self.run_cli([cmd, rec or self.rec, "-o", out] + list(extra))
        return r, out

    def planes(self, path):
        with open(path, "rb") as f:
            hdus = self.fits.read_hdus(f.read())
        return hdus[0][0], {h["EXTNAME"]: d for h, d in hdus[1:]}

    def test_world_colour(self):
        r, out = self.render()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        head, planes = self.planes(out)
        self.assertEqual(head["SPP"], SPP)
        expect = [c * STRENGTH for c in RGB]
        worst = 0.0
        for c in range(3):
            mean, var, n = planes[f"display.{c}"], planes[f"display.{c}.VAR"], planes[f"display.{c}.NSAMP"]
            self.assertEqual(mean.shape, (4, 4))
            self.assertTrue((n == SPP).all(), n)  # an empty film has n = 1 and a zero mean
            se = np.sqrt(var.astype(np.float64))
            dev = np.abs(mean.astype(np.float64) - expect[c])
            self.assertTrue((dev <= 5.0 * se + 1e-6).all(), f"channel {c}: mean {mean}, expected {expect[c]}")
            worst = max(worst, float((dev / (se + 1e-7)).max()))
        print(f"cli render: world display RGB {expect}, worst pixel z = {worst:.2f}")

    def test_report_table_lists_the_unsupported_world_node(self):
        r, _ = self.render(rec=self.rec_unsupported)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("level code subject count detail", r.stdout)
        self.assertRegex(r.stdout, r"(?m)^\w+ %d \d+ \d+ ShaderNodeBevel$" % self.abi.AR_REPORT_NODE_UNSUPPORTED)

    def test_error_entry_exits_nonzero(self):
        r, _ = self.render(rec=self.rec_bad)
        print(r.stdout)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"(?m)^ERROR %d " % self.abi.AR_REPORT_GENERATION_REJECTED)

    def test_hash_mismatch_exits_3(self):
        edited = os.path.join(self.tmp.name, "edited.arrec")
        shutil.copy(self.rec, edited)
        with open(edited, "r+b") as f:  # the ABI hash follows the 8-byte magic
            f.seek(8)
            h = bytearray(f.read(8))
            h[0] ^= 0xFF
            f.seek(8)
            f.write(h)
        r, out = self.render(rec=edited)
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("ABI mismatch", r.stdout)
        self.assertFalse(os.path.exists(out))

    def test_recorded_close_still_renders(self):
        r, out = self.render(rec=self.rec_closed)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.planes(out)[0]["SPP"], SPP)

    def test_help_says_render_is_not_science(self):
        for cmd in ("render", "dump"):
            r = self.run_cli([cmd, "--help"])
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("not a science file", r.stdout)

    def test_spp_override(self):
        r, out = self.render(["--spp", "4"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("override applied: --spp 4", r.stdout)
        head, planes = self.planes(out)
        self.assertEqual(head["SPP"], 4)
        self.assertTrue((planes["display.0.NSAMP"] == 4).all())

    def test_size_override_same_aspect(self):
        r, out = self.render(["--size", "8x8", "--spp", "2"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("override applied: --size 8x8", r.stdout)
        _, planes = self.planes(out)
        self.assertEqual(planes["display.0"].shape, (8, 8))

    def test_size_override_refuses_another_aspect(self):
        r, out = self.render(["--size", "8x6"])
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("--size refused", r.stdout)
        self.assertFalse(os.path.exists(out))

    def test_override_without_a_matching_call_is_an_error(self):
        for flag, value in (("--spp", "4"), ("--seed", "5"), ("--size", "8x8")):
            r, out = self.render([flag, value], rec=self.rec_no_settings)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn(f"override {flag} was not applied (the recording has no matching call)", r.stdout)
            self.assertFalse(os.path.exists(out))

    def test_seed_override(self):
        r, out = self.render()
        self.assertEqual(self.planes(out)[0]["SEED"], 11)  # the recorded seed
        r, out = self.render(["--seed", "99"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("override applied: --seed 99", r.stdout)
        self.assertEqual(self.planes(out)[0]["SEED"], 99)

    def test_device_override(self):
        r, _ = self.render(["--device", "cpu"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("override applied: --device cpu", r.stdout)
        r = self.run_cli(["render", self.rec, "-o", os.path.join(self.tmp.name, "n.fits"), "--device", "cuda"],
                         CUDA_VISIBLE_DEVICES="-1")
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)  # no silent fallback to the CPU
        self.assertIn("replay failed", r.stdout)

    def test_report_is_headed_by_the_build(self):
        r, _ = self.render()
        self.assertRegex(r.stdout, r"(?m)^report build: sha=\w+ cuda=\S*$")

    def test_dump_writes_every_channel(self):
        r, out = self.render(cmd="dump")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _, planes = self.planes(out)
        self.assertIn("display.0", planes)
        self.assertGreater(len(planes), 9, sorted(planes))  # display plus statistics and counters

    def test_bench_history_lines(self):
        r = self.run_cli(["bench", self.rec, "--passes", "32", "--warmup", "2"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        lines = [json.loads(ln) for ln in r.stdout.splitlines() if ln.startswith("{")]
        keys = {"sha", "machine", "driver", "cuda", "scene", "metric", "value", "unit", "n", "spread"}  # 0009 d22
        for ln in lines:
            self.assertEqual(set(ln), keys)
            self.assertEqual(ln["scene"], "069_empty_world")
        metrics = {ln["metric"]: ln for ln in lines}
        self.assertEqual(set(metrics), {"pass_ms", "kernel_ms_per_sample", "rays_closest", "rays_shadow",
                                        "steps_per_pass"})
        self.assertGreater(metrics["pass_ms"]["value"], 0.0)
        self.assertGreaterEqual(metrics["pass_ms"]["n"], 1)
        self.assertEqual(metrics["rays_closest"]["value"], 16)  # one camera ray per pixel of the 4x4 film
        self.assertAlmostEqual(metrics["kernel_ms_per_sample"]["value"], metrics["pass_ms"]["value"] / 16,
                               delta=1e-6 * max(1.0, metrics["pass_ms"]["value"]))

    def test_bench_refuses_two_sessions(self):
        r = self.run_cli(["bench", self.rec_two])
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("exactly one", r.stdout)

    def read_pfm(self, path):
        with open(path, "rb") as f:
            data = f.read()
        magic, size, scale, rest = data.split(b"\n", 3)
        self.assertEqual(magic, b"PF")
        self.assertEqual(scale, b"-1.0")  # little-endian
        w, h = (int(x) for x in size.split())
        return np.frombuffer(rest, dtype="<f4").reshape(h, w, 3)

    def test_denoise_flag(self):
        # 197: --denoise oidn sets the recorded FINAL session's denoiser; Combined is written denoised
        # to <out>_combined.pfm, --dump adds the film's Noisy Image; the FITS dump stays the film's.
        r, out = self.render(["--denoise", "oidn", "--dump"], rec=self.rec_final)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("override applied: --denoise oidn", r.stdout)
        base = os.path.splitext(out)[0]
        self.assertIn(f"wrote {base}_combined.pfm (Combined, denoised)", r.stdout)
        self.assertIn(f"wrote {base}_noisy_image.pfm (Noisy Image, not denoised)", r.stdout)
        combined, noisy = self.read_pfm(base + "_combined.pfm"), self.read_pfm(base + "_noisy_image.pfm")
        self.assertEqual(combined.shape, (4, 4, 3))
        self.assertTrue(np.isfinite(combined).all(), combined)
        head, planes = self.planes(out)
        self.assertIs(head["DENOISED"], False)
        for c in range(3):  # Noisy Image is the film's display mean, as the dump holds it
            self.assertTrue(np.array_equal(noisy[:, :, c], planes[f"display.{c}"]), c)
        print(f"cli --denoise oidn: Combined mean {combined.mean(axis=(0, 1))}, Noisy Image mean {noisy.mean(axis=(0, 1))}")
        # A VIEWPORT session cannot take the override: the replay refuses it loudly.
        r, out = self.render(["--denoise", "oidn"])
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("replay failed", r.stdout)
        self.assertIn("VIEWPORT session", r.stdout)
        self.assertFalse(os.path.exists(out))
        self.assertEqual(self.run_cli(["render", self.rec_final, "-o", "x.fits", "--dump"]).returncode, 2)
        self.assertEqual(self.run_cli(["render", self.rec_final, "-o", "x.fits", "--denoise", "dlss"]).returncode, 2)

    def test_usage_errors(self):
        self.assertEqual(self.run_cli(["render", self.rec]).returncode, 2)
        self.assertEqual(self.run_cli(["render", self.rec, "-o", "x.fits", "--size", "bad"]).returncode, 2)


if __name__ == "__main__":
    unittest.main()
