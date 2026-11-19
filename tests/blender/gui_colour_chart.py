# SPDX-License-Identifier: Apache-2.0
"""V9, the colour check (task 097): Blender's view transform applies exactly once to the viewport.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_colour_chart.py` (the land canary adds `-k <case>`).
Timers drive everything; the script ends the process itself, exit code 1 on any failed case.
A chart of 12 emission patches (a grey ramp that runs past 1.0 and six colours; each patch is a quad whose
Principled BSDF has black base colour and an Emission Color of the patch's scene-linear value, strength 1)
is rendered by Astroray in a Rendered viewport of a camera looking straight at it, and the viewport area is
screenshotted once the frame has converged. The reference is the same 12 values in a float image of the
working space (Linear Rec.709), written by `Image.save_render`, which applies the scene's colour management
(view transform, look, exposure, gamma) once through Blender's own code. Cases:
  Standard, AgX, Standard + exposure +1, Standard + gamma 1.4, AgX + the Punchy look.
Per patch, per channel, the mean of the middle of the patch in the screenshot against the reference:
|viewport - reference| <= 2/255 (0007 V9). Patch positions in the screenshot come from Blender's world-to-region
projection, not from the pixels; the reference's patches must be flat, so a wrong layout cannot pass.
Pixels are read with NumPy as Non-Color bytes. The Image Editor is not the reference: in Blender 5.3 it showed the
image under Standard whatever the scene's view settings and "view as render".
Ref: docs/decisions/0007-blender-integration.md V9, d32 (no engine tonemapping in the beauty path), d29
(draw through bind_display_space_shader).
"""
import importlib
import json
import os
import subprocess
import sys
import time
import traceback
import unittest

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer  # noqa: E402
import gui_present  # noqa: E402  (the windowed driver: job lookup)
import smoke_a1  # noqa: E402

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 240.0
STEP_TIMEOUT_S = 40.0
TOLERANCE = 2          # 0007 V9, in 8-bit codes
UNIFORM = 2            # a patch window must be this flat (max - min per channel, codes)
COLS, ROWS = 4, 3
INNER = 0.5            # the middle fraction of a patch that is sampled
PITCH, SIZE = 1.0, 0.8
RES = (1200, 900)
PATCHES = [  # scene-linear values, row by row from the top
    (0.02, 0.02, 0.02), (0.05, 0.05, 0.05), (0.18, 0.18, 0.18), (0.5, 0.5, 0.5),
    (1.0, 1.0, 1.0), (4.0, 4.0, 4.0), (0.5, 0.1, 0.1), (0.1, 0.4, 0.1),
    (0.1, 0.1, 0.5), (0.6, 0.5, 0.1), (0.1, 0.5, 0.5), (0.5, 0.1, 0.5),
]
LOOK = "AgX - Punchy"
CASES = [  # name, view transform, look, exposure, gamma
    ("standard", "Standard", "None", 0.0, 1.0),
    ("agx", "AgX", "None", 0.0, 1.0),
    ("standard_exposure_plus1", "Standard", "None", 1.0, 1.0),
    ("standard_gamma", "Standard", "None", 0.0, 1.4),
    ("agx_look", "AgX", LOOK, 0.0, 1.0),
]
T0 = time.monotonic()
STATE = {"errors": {}, "shots": {}, "results": {}}
EVIDENCE = os.path.join(ROOT, "out", "evidence", "097")


def finish(code):
    print(f"gui_colour_chart exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)


def patch_centre(k):
    """World position of patch k (row-major from the top left) on the chart plane."""
    i, j = k % COLS, k // COLS
    return ((i - (COLS - 1) / 2) * PITCH, ((ROWS - 1) / 2 - j) * PITCH, 0.0)


def build_chart(scene):
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.0
    scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = *RES, 100
    h = SIZE / 2
    for k, rgb in enumerate(PATCHES):
        me = bpy.data.meshes.new(f"patch{k}")
        me.from_pydata([(-h, -h, 0), (h, -h, 0), (h, h, 0), (-h, h, 0)], [], [(0, 1, 2, 3)])
        mat = bpy.data.materials.new(f"patch{k}")
        mat.use_nodes = True
        tree = mat.node_tree
        tree.nodes.remove(next(n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"))
        node = tree.nodes.new("ShaderNodeBsdfPrincipled")
        tree.links.new(node.outputs["BSDF"], next(n for n in tree.nodes if n.type == "OUTPUT_MATERIAL").inputs["Surface"])
        node.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
        node.inputs["Emission Color"].default_value = (*rgb, 1.0)
        node.inputs["Emission Strength"].default_value = 1.0
        me.update()
        me.materials.append(mat)
        ob = bpy.data.objects.new(f"patch{k}", me)
        ob.location = patch_centre(k)
        scene.collection.objects.link(ob)
    cam = bpy.data.cameras.new("Camera")
    # Perspective, looking straight at the chart: 36 mm sensor, 50 mm lens at 6 m sees 4.32 m across.
    # (An orthographic camera renders black in the viewport, so the chart avoids one.)
    cam.type = "PERSP"
    cam.lens, cam.sensor_width = 50.0, 36.0
    cam.passepartout_alpha = 0.0
    cob = bpy.data.objects.new("Camera", cam)
    cob.location = (0.0, 0.0, 6.0)
    scene.collection.objects.link(cob)
    scene.camera = cob


def chart_image():
    """The 12 values as a float image of the same 4 x 3 layout, in the working space."""
    w, h = RES
    px = np.zeros((h, w, 4), np.float32)
    px[:, :, 3] = 1.0
    for k, rgb in enumerate(PATCHES):
        i, j = k % COLS, k // COLS
        x0, x1 = i * w // COLS, (i + 1) * w // COLS
        y1, y0 = h - j * h // ROWS, h - (j + 1) * h // ROWS  # rows are bottom-up
        px[y0:y1, x0:x1, :3] = rgb
    img = bpy.data.images.new("v9_chart", w, h, alpha=False, float_buffer=True)
    img.colorspace_settings.name = "Linear Rec.709"
    img.pixels.foreach_set(px.ravel())
    img.update()
    return img


class Driver(gui_present.Driver):
    def __init__(self, rf):
        super().__init__(rf)
        self.case = 0
        self.settle = 0

    def phase(self):
        return "chart"

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                STATE["errors"]["watchdog"] = f"watchdog: still in step {self.step_name}"
                print(STATE["errors"]["watchdog"], flush=True)
                return self.end()
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["errors"]["chart"] = traceback.format_exc()
            print(f"phase chart failed:\n{STATE['errors']['chart']}", flush=True)
            return self.end()

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return super().timed_out(limit)

    def end(self):
        job = self.job
        if job is not None:
            print(f"viewport job: failure {job.failure}, errors {job.errors}, events tail {job.events[-6:]}, "
                  f"engine frames {STATE.get('frames')}, geometries {len(job.geom_keys)}, lines {job.lines}, "
                  f"objects {[(o.name, o.type) for o in bpy.context.scene.objects][:3]}, camera {bpy.context.scene.camera}", flush=True)
        self.rf.shutdown()
        run_cases()
        return None

    def areas(self):
        win = bpy.context.window_manager.windows[0]
        out = {}
        for a in win.screen.areas:
            if a.type == "VIEW_3D":
                out[a.type] = (win, a, next(r for r in a.regions if r.type == "WINDOW"))
        return out

    def find_job_any(self):
        held = self.rf._contexts.get("CPU")
        vp = importlib.import_module(MODULE + ".viewport")
        for j in (held.jobs if held else ()):
            if isinstance(j, vp.ViewportJob):
                return j
        return None

    def converged(self, job):
        import ctypes
        prog = job.abi.ar_progress()
        job.lib.ar_session_get_progress(job.session, ctypes.byref(prog))
        return prog.state == job.abi.AR_STATE_CONVERGED

    def step_setup(self):
        win, area, region = self.find_area()
        space = area.spaces.active
        space.shading.type = "RENDERED"
        space.overlay.show_overlays = False
        space.show_gizmo = False
        for name in ("show_region_header", "show_region_toolbar", "show_region_ui", "show_region_tool_header",
                     "show_region_hud"):
            if hasattr(space, name):
                setattr(space, name, False)
        space.region_3d.view_perspective = "CAMERA"
        chart_image()
        self.go("first_frame")
        return 0.5

    def step_first_frame(self):
        self.job = self.job or self.find_job_any()
        if self.job is None or self.job.t_first_frame is None:
            if self.timed_out():
                raise RuntimeError("no first frame")
            return 0.2
        self.job.frames, self.job.acquired = [], []  # the engine's own frames, for the diagnosis below
        self.go("apply")
        return 0.1

    def step_apply(self):
        name, vt, look, exposure, gamma = CASES[self.case]
        vs = bpy.context.scene.view_settings
        vs.view_transform, vs.look, vs.exposure, vs.gamma = vt, look, exposure, gamma
        for _, area, _ in self.areas().values():
            area.tag_redraw()
        self.settle = 0
        self.go("wait")
        return 0.5

    def step_wait(self):
        for _, area, _ in self.areas().values():
            area.tag_redraw()
        if not self.converged(self.job):
            if self.timed_out():
                raise RuntimeError("the viewport never converged")
            self.settle = 0
            return 0.2
        self.settle += 1
        if self.settle < 4:  # a few quiet, converged ticks: the last frame is on screen
            return 0.25
        self.shoot()
        self.case += 1
        if self.case == len(CASES):
            return self.end()
        self.go("apply")
        return 0.1

    def shoot(self):
        name = CASES[self.case][0]
        win = bpy.context.window_manager.windows[0]
        with bpy.context.temp_override(window=win):  # draw every region now, so the screenshots are of this state
            bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
        STATE.setdefault("frames", {})[name] = [{"spp": f["spp"], "max": float(f["rgb"].max()), "mean": float(f["rgb"].mean())}
                                                for f in self.job.frames[-2:]]
        shots = {}
        for kind, (win, area, region) in self.areas().items():
            path = os.path.join(EVIDENCE, f"colour_chart_{name}_viewport.png")
            with bpy.context.temp_override(window=win, area=area, region=region):
                bpy.ops.screen.screenshot_area(filepath=path)
            shots["viewport"] = (path, region.x - area.x, region.y - area.y, region.width, region.height)
            from bpy_extras import view3d_utils
            rv3d = area.spaces.active.region_3d
            half = SIZE / 2
            cs = []
            for k in range(len(PATCHES)):
                x, y, _ = patch_centre(k)
                p = [view3d_utils.location_3d_to_region_2d(region, rv3d, (x + dx, y + dy, 0.0))
                     for dx, dy in ((0, 0), (half, 0), (0, half))]
                cs.append((p[0].x, p[0].y, abs(p[1].x - p[0].x), abs(p[2].y - p[0].y)))
            shots["centres_viewport"] = cs
        # The reference: the same values through Blender's own colour management, once.
        w, h = RES
        ref = os.path.join(EVIDENCE, f"colour_chart_{name}_reference.png")
        bpy.data.images["v9_chart"].save_render(ref, scene=bpy.context.scene)
        shots["reference"] = (ref, 0, 0, w, h)
        shots["centres_reference"] = [((k % COLS + 0.5) * w / COLS, h - (k // COLS + 0.5) * h / ROWS,
                                       w / COLS / 2, h / ROWS / 2) for k in range(len(PATCHES))]
        STATE["shots"][name] = shots


def region_pixels(path, x0, y0, w, h):
    """The region as (h, w, 3) float codes 0..255, raw (Non-Color), rows bottom-up like Blender's."""
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = "Non-Color"
    px = np.array(img.pixels[:], np.float32).reshape(img.size[1], img.size[0], 4)[:, :, :3]
    bpy.data.images.remove(img)
    return px[y0:y0 + h, x0:x0 + w] * 255.0


def patch_stats(pix, centres):
    """Mean and flatness (max - min) of the middle INNER of each patch's window, per channel."""
    means, flat = [], []
    for cx, cy, hw, hh in centres:
        x0, x1 = int(round(cx - hw * INNER)), int(round(cx + hw * INNER))
        y0, y1 = int(round(cy - hh * INNER)), int(round(cy + hh * INNER))
        win = pix[y0:y1, x0:x1].reshape(-1, 3)
        means.append(win.mean(axis=0))
        flat.append(float((win.max(axis=0) - win.min(axis=0)).max()))
    return np.array(means), np.array(flat)


class TestColourChart(unittest.TestCase):
    def setUp(self):
        if "watchdog" in STATE["errors"]:
            self.fail(STATE["errors"]["watchdog"])
        if STATE["errors"]:
            self.fail("the chart phase did not complete:\n" + "\n".join(STATE["errors"].values()))

    def measure(self, name):
        s = STATE["shots"][name]
        out = {}
        for kind in ("viewport", "reference"):
            path, x0, y0, w, h = s[kind]
            out[kind] = patch_stats(region_pixels(path, x0, y0, w, h), s["centres_" + kind])
        return out

    def run_case(self, name):
        m = self.measure(name)
        (vm, vf), (rm, rf_) = m["viewport"], m["reference"]
        d = np.abs(vm - rm).max(axis=1)
        STATE["results"][name] = {"max_diff": float(d.max()), "per_patch": d.round(2).tolist(),
                                  "viewport_spread": float(vf.max()), "reference_spread": float(rf_.max()),
                                  "reference_means": rm.round(1).tolist(), "viewport_means": vm.round(1).tolist()}
        print(f"RESULT V9 {name}: per-patch max |viewport - reference| (of 255) {d.round(2).tolist()}; "
              f"max {d.max():.2f}; window spread (max - min, noise) viewport {vf.max():.1f}, reference {rf_.max():.1f}")
        self.assertLessEqual(float(rf_.max()), UNIFORM, "the reference's patch windows are not flat: the layout is off")
        self.assertLessEqual(float(d.max()), TOLERANCE)

    def test_standard(self):
        self.run_case("standard")

    def test_agx(self):
        self.run_case("agx")

    def test_exposure_plus1(self):
        self.run_case("standard_exposure_plus1")

    def test_gamma(self):
        self.run_case("standard_gamma")

    def test_look(self):
        self.run_case("agx_look")

    def test_the_chart_is_sensitive_to_the_transform(self):
        """Each case's reference differs from its base case by more than the tolerance, so a viewport that
        dropped the exposure, the gamma or the look (or the view transform) would fail that case."""
        ref = {n: self.measure(n)["reference"][0] for n in ("standard", "agx", "standard_exposure_plus1",
                                                             "standard_gamma", "agx_look")}
        for other, base in (("agx", "standard"), ("standard_exposure_plus1", "standard"),
                            ("standard_gamma", "standard"), ("agx_look", "agx")):
            self.assertGreater(float(np.abs(ref[other] - ref[base]).max()), TOLERANCE, f"{other} vs {base}")


def save_evidence():
    """out/evidence/097/colour_chart.png: the viewport screenshot (left) and the reference (right) of the AgX case."""
    s = STATE["shots"].get("agx")
    if not s:
        return
    halves = []
    for kind in ("viewport", "reference"):
        path, x0, y0, w, h = s[kind]
        halves.append(region_pixels(path, x0, y0, w, h) / 255.0)
    h = max(x.shape[0] for x in halves)
    both = np.concatenate([np.pad(x, ((0, h - x.shape[0]), (0, 0), (0, 0))) for x in halves], axis=1)
    img = bpy.data.images.new("v9_evidence", both.shape[1], both.shape[0], alpha=False)
    img.colorspace_settings.name = "Non-Color"
    rgba = np.ones((both.shape[0], both.shape[1], 4), np.float32)
    rgba[:, :, :3] = both
    img.pixels.foreach_set(rgba.ravel())
    img.filepath_raw = os.path.join(EVIDENCE, "colour_chart.png")
    img.file_format = "PNG"
    img.save()


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestColourChart))
    try:
        save_evidence()
    except Exception:
        print("saving the evidence image failed:\n" + traceback.format_exc(), flush=True)
    json.dump(STATE["results"], open(os.path.join(EVIDENCE, "colour_chart.json"), "w"), indent=1)
    finish(0 if res.wasSuccessful() else 1)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    os.makedirs(EVIDENCE, exist_ok=True)
    blend = os.path.join(work, "a1_chart.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_chart.arrec"), "--blend", blend, "--sampler", "sobol"],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    blend_writer.build(blend + ".json", blend)  # a scene the extension is set up for, then the chart replaces its content
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CPU"
    scene.astroray.viewport_samples = 128  # colour noise of the spectral sampling averages out over a patch
    scene.render.dither_intensity = 0.0  # Blender's display dither adds noise of a few codes
    ims = scene.render.image_settings
    ims.file_format, ims.color_mode, ims.color_depth = "PNG", "RGB", "8"
    build_chart(scene)
    driver = Driver(rf)
    bpy.app.timers.register(driver.tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["main"] = traceback.format_exc()
        print(STATE["errors"]["main"], flush=True)
        os.makedirs(EVIDENCE, exist_ok=True)
        run_cases()
