# SPDX-License-Identifier: Apache-2.0
"""A GPU kernel fault (0001 V14, GPU part; 0002 D15 "Sticky CUDA error"), in child processes only: the
fault leaves CUDA broken for the process that ran it. With ASTRORAY_TEST_GPU_FAULT=1 the CUDA device
runs src/kernel_gpu/test_fault.cu's out-of-bounds write before its first launch.
- test_cli_session_fails: `astroray_cli render --device cuda` exits non-zero and its report holds a
  SESSION_FAILED entry naming the sticky CUDA error.
- test_cpu_survives_gpu_fault: one child process through the C ABI: the CUDA session ends FAILED, its
  calls return AR_ERR_DEVICE with 0001 D2.5's restart message, a second CUDA session is refused, and
  a CPU session of the same process still renders a non-black image.
Needs a gate-hook build (the dev preset) and a CUDA device; the fault module is loose, so the child
reads kernels from out/bin/kernels (ASTRORAY_KERNEL_DIR)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
import ar  # noqa: E402
import meshes  # noqa: E402

CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
KERNELS = os.path.join(ROOT, "out", "bin", "kernels")
SIZE = 8
RESTART = "Save your work and restart Blender."

# The child: builds three scenes (CUDA, CUDA, CPU) of a camera-filling plane under a constant world
# and prints one JSON object with what it observed.
CHILD = r'''
import ctypes, json, os, sys, tempfile, time
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, "scenes", "lib"))
sys.path.insert(0, os.path.join(root, "tests", "abi"))
import ar, meshes, fits_read
SIZE = int(sys.argv[2])
tmp = tempfile.mkdtemp()

def build(device, name):
    s = ar.Scene(os.path.join(tmp, name), device=device)
    plane = s.key()
    s.mesh_from(plane, meshes.plane(8.0, 8.0), "plane")
    s.world((1.0, 1.0, 1.0), 1.0)
    s.settings(4, SIZE, SIZE, seed=3, sampler="independent")
    s.camera((0.0, 0.0, 3.0), (0.0, 0.0, 0.0), 35.0, 36.0, SIZE, SIZE, up=(0.0, 1.0, 0.0))
    g = s.graph()
    g.store("base_weight", 1.0)
    s.instances([{"geom": plane, "materials": [s.material_from_graph(g)]}])
    return s

def text(buf):
    return bytes(buf).split(b"\0")[0].decode(errors="replace")

def create_session(s):
    a, lib = s.abi, s.lib
    gen = ctypes.c_uint64()
    st = lib.ar_edit_commit(s.edit_h, ctypes.byref(gen))
    if st != a.AR_OK:
        return st, None
    sd = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, SIZE, SIZE, a.AR_DENOISER_NONE)
    sh = ctypes.c_void_p()
    st = lib.ar_session_create(s.scene_h, ctypes.byref(sd), ctypes.byref(sh))
    if st != a.AR_OK:
        return st, None
    v = a.ar_view_desc()
    ctypes.memmove(ctypes.byref(v.camera), ctypes.byref(s._camera), ctypes.sizeof(a.ar_camera_desc))
    v.region_width, v.region_height, v.render_scale = SIZE, SIZE, 1.0
    assert lib.ar_session_set_view(sh, ctypes.byref(v)) == a.AR_OK
    s.session_h = sh
    return a.AR_OK, sh

def wait(s, sh, states, seconds=90.0):
    a, lib = s.abi, s.lib
    end = time.perf_counter() + seconds
    p = a.ar_progress()
    while time.perf_counter() < end:
        assert lib.ar_session_get_progress(sh, ctypes.byref(p)) == a.AR_OK
        if p.state in states:
            return p
        lib.ar_session_wait(sh, 20)
    raise RuntimeError("session state %d is not one of %s after %g s" % (p.state, states, seconds))

def report_codes(s, sh):
    a, lib = s.abi, s.lib
    info = a.ar_report_info()
    assert lib.ar_session_report_info(sh, ctypes.byref(info)) == a.AR_OK
    rows = []
    for i in range(info.entry_count):
        e = a.ar_report_entry()
        assert lib.ar_session_report_entry(sh, i, ctypes.byref(e)) == a.AR_OK
        rows.append((e.code, text(e.detail)))
    return rows

out = {}
# 1. The CUDA session faults on its first launch.
s1 = build("cuda", "cuda1.arrec")
a, lib = s1.abi, s1.lib
st, sh1 = create_session(s1)
assert st == a.AR_OK, st
assert lib.ar_session_start(sh1) == a.AR_OK
p = wait(s1, sh1, (a.AR_STATE_FAILED, a.AR_STATE_CONVERGED))
out["state"] = p.state
buf = (ctypes.c_uint8 * 512)()
out["start_status"] = lib.ar_session_start(sh1)
lib.ar_session_last_error(sh1, 512, buf)
out["start_error"] = text(buf)
out["failed_codes"] = [c for c, d in report_codes(s1, sh1)]
out["failed_details"] = [d for c, d in report_codes(s1, sh1) if c == a.AR_REPORT_SESSION_FAILED]
# 2. A second CUDA session is refused.
s2 = build("cuda", "cuda2.arrec")
st2, sh2 = create_session(s2)
out["second_status"] = st2
s2.lib.ar_scene_last_error(s2.scene_h, 512, buf)
out["second_error"] = text(buf)
# 3. A CPU session of the same process renders.
s3 = build("cpu", "cpu.arrec")
st3, sh3 = create_session(s3)
assert st3 == a.AR_OK, st3
assert lib.ar_session_start(sh3) == a.AR_OK
p3 = wait(s3, sh3, (a.AR_STATE_CONVERGED, a.AR_STATE_FAILED))
out["cpu_state"] = p3.state
fits_path = os.path.join(tmp, "cpu.fits")
out["cpu_write"] = lib.ar_session_write_fits(sh3, fits_path.encode(), a.AR_FITS_DUMP)
with open(fits_path, "rb") as f:
    hdus = fits_read.read_hdus(f.read())
planes = {h["EXTNAME"]: d for h, d in hdus[1:]}
out["cpu_mean"] = [float(planes["display.%d" % c].mean()) for c in range(3)]
print("RESULT " + json.dumps(out), flush=True)
'''


def child_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("ASTRORAY_TEST_")}
    env.update(extra)
    return env


class TestFault(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (CLI, ar.DLL, os.path.join(KERNELS, "test_fault.cubin")):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 (a dev build makes the gate-hook modules)")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.rec = os.path.join(cls.tmp.name, "plane.arrec")
        with ar.Scene(cls.rec) as s:  # a CPU recording; the CLI replays it on --device cuda
            plane = s.key()
            s.mesh_from(plane, meshes.plane(8.0, 8.0), "plane")
            s.world((1.0, 1.0, 1.0), 1.0)
            s.settings(4, SIZE, SIZE, seed=3, sampler="independent")
            s.camera((0.0, 0.0, 3.0), (0.0, 0.0, 0.0), 35.0, 36.0, SIZE, SIZE, up=(0.0, 1.0, 0.0))
            g = s.graph()
            g.store("base_weight", 1.0)
            s.instances([{"geom": plane, "materials": [s.material_from_graph(g)]}])
            s.finish()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_cli_session_fails(self):
        out = os.path.join(self.tmp.name, "fault.fits")
        env = child_env(ASTRORAY_TEST_GPU_FAULT="1", ASTRORAY_KERNEL_DIR=KERNELS)
        r = subprocess.run([CLI, "render", self.rec, "-o", out, "--device", "cuda"], capture_output=True, text=True,
                           env=env, cwd=self.tmp.name, timeout=180)
        print(r.stdout + r.stderr)
        self.assertNotEqual(r.returncode, 0, "a GPU fault must fail the render")
        self.assertFalse(os.path.exists(out), "a failed render wrote a film")
        # The report's SESSION_FAILED entry (code 1, ERROR) carries the sticky CUDA error.
        self.assertRegex(r.stdout, r"(?m)^ERROR 1 0 1 GPU error: CUDA_ERROR_ILLEGAL_ADDRESS")

    def test_cpu_survives_gpu_fault(self):
        env = child_env(ASTRORAY_TEST_GPU_FAULT="1", ASTRORAY_KERNEL_DIR=KERNELS)
        r = subprocess.run([sys.executable, "-I", "-c", CHILD, ROOT, str(SIZE)], capture_output=True, text=True, env=env,
                           cwd=self.tmp.name, timeout=300)
        self.assertEqual(r.returncode, 0, "the child process died:\n" + r.stdout + r.stderr)
        lines = [ln for ln in r.stdout.splitlines() if ln.startswith("RESULT ")]
        self.assertEqual(len(lines), 1, r.stdout + r.stderr)
        got = json.loads(lines[0][len("RESULT "):])
        print(got)
        abi = ar._load("_ar_abi_fault", os.path.join(ar.GEN, "_abi.py"))
        # The CUDA session ended FAILED; its calls return AR_ERR_DEVICE with the restart message.
        self.assertEqual(got["state"], abi.AR_STATE_FAILED)
        self.assertEqual(got["start_status"], abi.AR_ERR_DEVICE)
        self.assertIn("CUDA_ERROR_ILLEGAL_ADDRESS", got["start_error"])
        self.assertIn(RESTART, got["start_error"])
        self.assertIn(abi.AR_REPORT_SESSION_FAILED, got["failed_codes"])
        self.assertEqual(len(got["failed_details"]), 1)
        self.assertTrue(got["failed_details"][0].startswith("GPU error: CUDA_ERROR_ILLEGAL_ADDRESS"))
        # A second CUDA session is refused with the same restart message.
        self.assertEqual(got["second_status"], abi.AR_ERR_DEVICE)
        self.assertIn(RESTART, got["second_error"])
        # The CPU session of the same process converged on a lit scene: not black.
        self.assertEqual(got["cpu_state"], abi.AR_STATE_CONVERGED)
        self.assertEqual(got["cpu_write"], abi.AR_OK)
        self.assertEqual(len(got["cpu_mean"]), 3)
        for c, m in enumerate(got["cpu_mean"]):
            self.assertGreater(m, 0.01, f"channel {c} of the CPU image is black")


if __name__ == "__main__":
    unittest.main()
