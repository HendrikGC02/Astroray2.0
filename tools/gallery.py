# SPDX-License-Identifier: Apache-2.0
"""Showcase gallery of a milestone, rendered through Blender and the addon (0009 d23).

Run: <python> -I tools/gallery.py --milestone M1a [--scene name ...] [--no-heat]
One file, two roles. Run by Blender's python it is the driver: it reads bench/scenes.toml, builds each showcase
scene's description with scenes/<name>/build.py, starts Blender through tools/lib/blender.ps1 (Invoke-ArBlender,
so the machine lock and the isolated profile apply) with this same file as the worker (`--job <json>`) and checks
what the workers wrote. As the worker it runs inside Blender with the staged extension enabled:
  f12:  the .blend of one scene (scenes/lib/blend_writer.py), F12 through the addon on the CPU and on CUDA at the
        scene's gallery size and spp, one PNG per device saved by Blender (so the view transform is the one a user
        sees), a thumbnail, a JSON sidecar {scene, device, spp, time, settings, build id}, and the per-pixel
        relative difference of the two devices (informational: agreement is gated elsewhere, 0009 d17).
  heat: a windowed Blender (the viewport draws only there), the Sample count and the Bounce count display modes
        of the scene, one device per run, a screenshot of the viewport region each (0007 d40).
Images go to out/gallery/<milestone>/ and are never committed (0009 d23). A PNG over 1600 px or 2 MB, or whose
mean is below 1 % of white, fails the run (d21).
bench/scenes.toml carries the showcase entries: `showcase` (milestones), `gallery_size`, `gallery_spp`, `heat`.
Ref: docs/decisions/0009-build-test-process.md d21, d22, d23; docs/decisions/0007-blender-integration.md d40.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import time
import tomllib
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

MAX_PX, MAX_BYTES, MIN_MEAN = 1600, 2 * 1024 * 1024, 0.01
THUMB_PX = 480
DEVICES = ("CPU", "CUDA")


def out_dir(milestone):
    return os.path.join(ROOT, "out", "gallery", milestone)


def showcase(milestone):
    with open(os.path.join(ROOT, "bench", "scenes.toml"), "rb") as f:
        scenes = [s for s in tomllib.load(f)["scene"] if milestone in s.get("showcase", [])]
    for s in scenes:
        for key in ("gallery_size", "gallery_spp"):
            if key not in s:
                raise SystemExit(f"FAIL gallery: bench/scenes.toml entry {s['name']} has no {key}")
    return scenes


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise SystemExit(f"FAIL gallery: {path} is not a PNG")
    return struct.unpack(">II", head[16:24])


# ------------------------------------------------------------------------------------------------ driver

def run_blender(script_args, timeout_s, gui):
    """One Blender start through Invoke-ArBlender; returns its exit code (-1 on timeout)."""
    ps = (f". '{ROOT}/tools/lib/blender.ps1'; "
          f"$r = Invoke-ArBlender -Script '{ROOT}/tools/gallery.py' -Args @({','.join(repr(a) for a in script_args)}) "
          f"-TimeoutS {timeout_s}{' -Gui' if gui else ''}; exit $r.ExitCode")
    return subprocess.run(["pwsh", "-NoProfile", "-Command", ps], cwd=ROOT).returncode


def build_description(entry, work):
    name = os.path.splitext(entry["recording"])[0]   # the builder's folder: bench id b_tx is scenes/s3_textured
    blend = os.path.join(work, f"{name}.blend")
    cmd = [sys.executable, "-I", os.path.join(ROOT, "scenes", name, "build.py"),
           "--out", os.path.join(work, f"{name}.arrec"), "--blend", blend]
    if name == "a1_furnace":
        cmd += ["--sampler", "sobol"]   # a .blend cannot carry another sampler
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    if p.returncode != 0:
        raise SystemExit(f"FAIL gallery: scenes/{name}/build.py failed:\n{p.stdout}{p.stderr}")
    return blend


def expected_files(entry, devices):
    n = entry["name"]
    files = [f"{n}_{d.lower()}.png" for d in devices] + [f"{n}_{d.lower()}.json" for d in devices]
    if len(devices) == 2:
        files += [f"{n}_diff.png", f"{n}_diff.json"]
    return files


def driver(a):
    milestone = a.milestone
    entries = showcase(milestone)
    if a.scene:
        entries = [e for e in entries if e["name"] in a.scene]
    if not entries:
        raise SystemExit(f"FAIL gallery: no showcase scene for {milestone}")
    out = out_dir(milestone)
    work = os.path.join(out, "work")
    os.makedirs(work, exist_ok=True)
    produced = []
    for e in entries:
        blend = build_description(e, work)
        job = {"mode": "f12", "entry": e, "blend": blend, "desc": blend + ".json", "out": out, "devices": list(DEVICES)}
        jp = os.path.join(work, f"{e['name']}_f12.json")
        with open(jp, "w") as f:
            json.dump(job, f)
        t0 = time.perf_counter()
        code = run_blender(["--job", jp], 3600, gui=False)
        print(f"gallery: {e['name']} f12 exit {code} in {time.perf_counter() - t0:.0f} s", flush=True)
        if code != 0:
            raise SystemExit(f"FAIL gallery: {e['name']} render failed (exit {code}; -1 is the timeout)")
        produced += expected_files(e, DEVICES)
    if not a.no_heat:
        for e in (e for e in entries if e.get("heat")):
            blend = os.path.join(work, os.path.splitext(e["recording"])[0] + ".blend")
            for dev in DEVICES:
                job = {"mode": "heat", "entry": e, "blend": blend, "desc": blend + ".json", "out": out, "device": dev}
                jp = os.path.join(work, f"{e['name']}_heat_{dev.lower()}.json")
                with open(jp, "w") as f:
                    json.dump(job, f)
                code = run_blender(["--job", jp], 1200, gui=True)
                print(f"gallery: {e['name']} heat {dev} exit {code}", flush=True)
                if code != 0:
                    raise SystemExit(f"FAIL gallery: {e['name']} heat maps on {dev} failed (exit {code})")
                for kind in ("samples", "bounces"):
                    produced += [f"{e['name']}_heat_{kind}_{dev.lower()}.png", f"{e['name']}_heat_{kind}_{dev.lower()}.json"]
    print(f"gallery: images in {out}")
    bad = []
    for name in produced:
        p = os.path.join(out, name)
        if not os.path.exists(p):
            bad.append(f"{name} is missing")
            continue
        size = os.path.getsize(p)
        if name.endswith(".png"):
            w, h = png_size(p)
            print(f"  {name:<44} {w:>5} x {h:<5} {size / 1024:>8.1f} KiB")
            if max(w, h) > MAX_PX or size > MAX_BYTES:
                bad.append(f"{name}: {w} x {h}, {size} bytes is over {MAX_PX} px / 2 MB (0009 d21)")
        else:
            print(f"  {name:<44} sidecar {size:>8} B")
    if bad:
        print("FAIL gallery: " + "; ".join(bad))
        return 1
    print(f"PASS gallery {milestone}: {sum(n.endswith('.png') for n in produced)} images")
    return 0


# ------------------------------------------------------------------------------------------------ worker

def build_id(module):
    """The core's version line (git sha, build type, CUDA, OptiX): what native.load() prints at start."""
    import importlib
    native = importlib.import_module(module + ".native")
    return native._build_line(importlib.import_module(module + "._abi"), native.load())


def settings_of(scene, desc):
    st = json.load(open(desc))["settings"]
    vs = scene.view_settings
    return {"width": scene.render.resolution_x, "height": scene.render.resolution_y, "seed": st["seed"],
            "max_bounces": st["max_bounces"], "min_bounces": st["min_bounces"], "pixel_filter": st["pixel_filter"],
            "view_transform": vs.view_transform, "look": vs.look, "exposure": vs.exposure, "gamma": vs.gamma,
            "display_device": scene.display_settings.display_device}


def load_u8(path):
    """A PNG's bytes as (h, w, 3) uint8, top row first: Non-Color so Blender converts nothing."""
    import bpy
    import numpy as np
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = "Non-Color"
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return np.rint(px.reshape(h, w, 4)[::-1, :, :3] * 255.0).astype(np.uint8)


def thumbnail(rgb, px=THUMB_PX):
    """Box-averaged by the integer factor that brings the width to at most `px`."""
    import numpy as np
    f = max(1, -(-rgb.shape[1] // px))
    h, w = rgb.shape[0] // f * f, rgb.shape[1] // f * f
    t = rgb[:h, :w].reshape(h // f, f, w // f, f, 3).mean(axis=(1, 3))
    return np.rint(t).astype(np.uint8)


def check_image(rgb, name):
    mean = float(rgb.mean()) / 255.0
    if mean < MIN_MEAN:
        raise RuntimeError(f"{name} is blank or near black: mean {mean:.4f} of white (limit {MIN_MEAN})")
    if max(rgb.shape[:2]) > MAX_PX:
        raise RuntimeError(f"{name} is {rgb.shape[1]} x {rgb.shape[0]}, over {MAX_PX} px")
    return mean


def write_sidecar(path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, sort_keys=True)


def diff_map(cpu, gpu):
    """Relative difference |g - c| / mean(g, c) of the luminance (mean of RGB), as a black-to-yellow ramp that
    saturates at 100 %, and its statistics. Pixels below 1e-3 in both are treated as black, not as noise."""
    import numpy as np
    c, g = cpu[..., :3].mean(-1), gpu[..., :3].mean(-1)
    d = np.abs(g - c) / np.maximum(0.5 * (g + c), 1e-3)
    v = np.clip(d, 0.0, 1.0)
    ramp = np.stack([np.minimum(1.0, 2 * v), np.clip(2 * v - 1, 0.0, 1.0) * 0.9 + 0.0 * v, 0.0 * v], -1)
    stats = {"median_rel": float(np.median(d)), "p95_rel": float(np.percentile(d, 95)), "mean_rel": float(d.mean()),
             "mean_gpu_over_cpu": float(g.mean() / max(float(c.mean()), 1e-12)), "ramp": "black 0 %, red 50 %, yellow 100 %"}
    return np.rint(ramp * 255).astype(np.uint8), stats


def worker_f12(job):
    import bpy
    import numpy as np
    import report
    sys.path.insert(0, os.path.join(ROOT, "tests", "blender"))
    import smoke_a1
    import blend_writer
    smoke_a1.install_and_enable()
    blend_writer.build(job["desc"], job["blend"])
    bpy.ops.wm.open_mainfile(filepath=job["blend"], load_ui=False)
    scene, e = bpy.context.scene, job["entry"]
    w0, h0 = scene.render.resolution_x, scene.render.resolution_y
    w, h = e["gallery_size"]
    if w * h0 != h * w0:
        raise RuntimeError(f"gallery size {w} x {h} changes the aspect of the scene's {w0} x {h0}")
    scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = w, h, 100
    scene.astroray.samples = scene.astroray.viewport_samples = e["gallery_spp"]
    im = scene.render.image_settings
    im.file_format, im.color_mode, im.color_depth, im.compression = "PNG", "RGB", "8", 100
    build = build_id("bl_ext.user_default.astroray")
    out = job["out"]
    os.makedirs(os.path.join(out, "thumbs"), exist_ok=True)
    linear = {}
    for dev in job["devices"]:
        scene.astroray.device = dev
        t0 = time.perf_counter()
        bpy.ops.render.render(write_still=False)
        secs = time.perf_counter() - t0
        res = bpy.data.images["Render Result"]
        rw, rh = res.size
        px = np.empty(rw * rh * 4, np.float32)
        res.pixels.foreach_get(px)
        linear[dev] = px.reshape(rh, rw, 4)[::-1]
        name = f"{e['name']}_{dev.lower()}"
        path = os.path.join(out, name + ".png")
        res.save_render(path)
        rgb = load_u8(path)
        mean = check_image(rgb, name + ".png")
        report.write_png(os.path.join(out, "thumbs", name + ".png"), thumbnail(rgb))
        write_sidecar(os.path.join(out, name + ".json"), {
            "scene": e["name"], "device": dev, "spp": e["gallery_spp"], "time": secs, "time_unit": "s",
            "time_note": "F12 wall clock: sync, engine start (OptiX cold compile on the first CUDA use in a fresh profile), render, resolve",
            "settings": settings_of(scene, job["desc"]), "build": build, "file": name + ".png",
            "bytes": os.path.getsize(path), "mean_of_white": mean})
        print(f"gallery: {name} {rw} x {rh} {e['gallery_spp']} spp in {secs:.1f} s, mean {mean:.3f}", flush=True)
    if len(job["devices"]) == 2:
        cpu, gpu = linear["CPU"], linear["CUDA"]
        if not (np.isfinite(cpu).all() and np.isfinite(gpu).all()):
            raise RuntimeError(f"{e['name']}: a render has non-finite pixels")
        d, stats = diff_map(cpu, gpu)
        name = f"{e['name']}_diff"
        report.write_png(os.path.join(out, name + ".png"), d)
        report.write_png(os.path.join(out, "thumbs", name + ".png"), thumbnail(d))
        write_sidecar(os.path.join(out, name + ".json"), dict(stats, scene=e["name"], build=build, file=name + ".png",
                                                           note="informational; agreement is gated by 0009 d17"))
        print(f"gallery: {name} median {stats['median_rel']:.3f} p95 {stats['p95_rel']:.3f} "
              f"gpu/cpu mean {stats['mean_gpu_over_cpu']:.4f}", flush=True)


def worker_heat(job):
    """The heat-map screenshots, one device. Mirrors tests/blender/gui_batching.py's heat phase (task 086)."""
    import bpy
    import importlib
    import report
    sys.path.insert(0, os.path.join(ROOT, "tests", "blender"))
    import blend_writer
    import gui_present
    import smoke_a1
    module = "bl_ext.user_default.astroray"
    smoke_a1.install_and_enable()
    rf = importlib.import_module(module + ".render_final")
    blend_writer.build(job["desc"], job["blend"])
    bpy.ops.wm.open_mainfile(filepath=job["blend"], load_ui=False)
    scene, e, dev = bpy.context.scene, job["entry"], job["device"]
    build = build_id(module)
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = dev
    scene.astroray.viewport_samples = 64
    scene.astroray.display_mode = "SAMPLE_COUNT"
    scene.view_settings.view_transform = "AgX"   # heat maps ignore it (0007 d40); set so that is what is shown
    scene.render.dither_intensity = 0.0
    scene.camera.data.passepartout_alpha = 0.0
    state = {"t0": time.monotonic()}

    def finish(code):
        sys.stdout.flush()
        os._exit(code)

    class Driver(gui_present.Driver):
        def find_job(self):
            held = self.rf._contexts.get(dev)
            return next((j for j in (held.jobs if held else ()) if hasattr(j, "frames")), None)

        def tick(self):
            try:
                if time.monotonic() - state["t0"] > 900:
                    raise RuntimeError(f"watchdog in step {self.step_name}")
                return getattr(self, "step_" + self.step_name)()
            except Exception:
                traceback.print_exc()
                sys.stdout.flush()
                self.rf.shutdown()
                finish(1)

        def heat_frame(self, mode):
            job_ = self.find_job()
            got = [f for f in (job_.frames if job_ else ()) if f.get("heat") and f["mode"] == mode]
            return got[-1] if got else None

        def converged(self, job_):
            import ctypes
            prog = job_.abi.ar_progress()
            job_.lib.ar_session_get_progress(job_.session, ctypes.byref(prog))
            return prog.state == job_.abi.AR_STATE_CONVERGED

        def settled(self, mode):
            """No new frame of this mode since the last tick: the one on screen is the one logged."""
            f = self.heat_frame(mode)
            if f["id"] != self.__dict__.get("seen_id"):
                self.seen_id = f["id"]
                self.find_area()[1].tag_redraw()
                return False
            return True

        def shot(self, kind, mode):
            win, area, region = self.find_area()
            tmp = os.path.join(job["out"], "work", f"{e['name']}_heat_{kind}_{dev.lower()}_area.png")
            with bpy.context.temp_override(window=win, area=area, region=region):
                bpy.ops.screen.screenshot_area(filepath=tmp)
            rgb = load_u8(tmp)   # top row first
            ah = area.height
            x0, y0, rw, rh = region.x - area.x, region.y - area.y, region.width, region.height
            rgb = rgb[ah - (y0 + rh):ah - y0, x0:x0 + rw]
            step = max(1, -(-rgb.shape[1] // MAX_PX))
            rgb = rgb[::step, ::step]   # nearest: a heat map's pixels are values, never blended
            name = f"{e['name']}_heat_{kind}_{dev.lower()}"
            mean = check_image(rgb, name + ".png")
            report.write_png(os.path.join(job["out"], name + ".png"), rgb)
            os.makedirs(os.path.join(job["out"], "thumbs"), exist_ok=True)
            report.write_png(os.path.join(job["out"], "thumbs", name + ".png"), thumbnail(rgb))
            write_sidecar(os.path.join(job["out"], name + ".json"), {
                "scene": e["name"], "device": dev, "display_mode": ("SAMPLE_COUNT", "BOUNCE_COUNT")[mode - 1],
                "spp": 64, "viewport_samples": 64, "time": None, "region": [rw, rh],
                "settings": settings_of(scene, job["desc"]), "build": build, "file": name + ".png",
                "bytes": os.path.getsize(os.path.join(job["out"], name + ".png")), "mean_of_white": mean})
            os.remove(tmp)
            print(f"gallery: {name} {rgb.shape[1]} x {rgb.shape[0]}", flush=True)

        def step_setup(self):
            self.toggle_maximize()
            _, area, region = self.find_area()
            space = area.spaces.active
            space.shading.type = "RENDERED"
            space.overlay.show_overlays = False
            space.show_gizmo = False
            for n in ("show_region_header", "show_region_toolbar", "show_region_ui", "show_region_tool_header", "show_region_hud"):
                if hasattr(space, n):
                    setattr(space, n, False)
            space.region_3d.view_perspective = "CAMERA"
            self.go("job")
            return 0.5

        def step_job(self):
            j = self.find_job()
            if j is None:
                if self.timed_out(120):
                    raise RuntimeError("no viewport job appeared")
                return 0.2
            j.frames, j.acquired = [], []
            self.job = j
            self.go("samples")
            return 0.5

        def step_samples(self):
            self.find_area()[1].tag_redraw()
            f = self.heat_frame(1)
            if f is None or not self.converged(self.job):
                if self.timed_out(300):
                    raise RuntimeError("no converged Sample count frame")
                return 0.2
            if not self.settled(1):
                return 0.7
            self.shot("samples", 1)
            scene.astroray.display_mode = "BOUNCE_COUNT"
            self.go("bounces")
            self.seen_id = None
            return 0.5

        def step_bounces(self):
            self.find_area()[1].tag_redraw()
            f = self.heat_frame(2)
            if f is None or not self.converged(self.job):
                if self.timed_out(300):
                    raise RuntimeError("no converged Bounce count frame")
                return 0.2
            if not self.settled(2):
                return 0.7
            self.shot("bounces", 2)
            self.rf.shutdown()
            finish(0)

    bpy.app.timers.register(Driver(rf).tick, first_interval=1.0)


def worker(job_path):
    with open(job_path) as f:
        job = json.load(f)
    try:
        {"f12": worker_f12, "heat": worker_heat}[job["mode"]](job)
    except Exception:
        traceback.print_exc()
        sys.stdout.flush()
        os._exit(1)
    if job["mode"] == "f12":
        import importlib
        importlib.import_module("bl_ext.user_default.astroray.render_final").shutdown()   # contexts closed in order


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    if "--job" in argv:
        return worker(argv[argv.index("--job") + 1])
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--milestone", required=True)
    ap.add_argument("--scene", action="append", help="only this showcase scene (repeatable)")
    ap.add_argument("--no-heat", action="store_true", help="skip the windowed heat-map runs")
    return driver(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
