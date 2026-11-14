"""Cycles RENDERED-viewport first-draw timing in a windowed isolated Blender (OptiX, camera view).
Run: blender --factory-startup <scene.blend> --window-geometry 0 0 1920 1080 --python gui_cycles_viewport.py -- S1|S2 [full=1]
A POST_PIXEL draw handler samples a 32x32 patch of the viewport framebuffer on every redraw; the trace shows
solid-view -> (blank) -> first Cycles image. First meaningful frame = first patch change that persists."""
import os, sys, time, json
import numpy as np
import bpy, gpu

HERE = os.path.dirname(os.path.abspath(__file__))
which = sys.argv[sys.argv.index("--") + 1]
kv = dict(a.split("=", 1) for a in sys.argv[sys.argv.index("--") + 1:] if "=" in a)
T = time.perf_counter
OUT = {"scene": which, "blender": bpy.app.version_string, "trace": []}
OUT_PATH = os.path.join(HERE, "out", f"gui_cycles_viewport_{which}{kv.get('tag', '')}.json")
t_start = T()
state = {"t_switch": None, "area": None, "region": None, "done": False}


def save():
    json.dump(OUT, open(OUT_PATH, "w"), indent=1)


def quit_now(reason):
    OUT["quit_reason"] = reason
    save()
    print("SPIKE_DONE", reason, flush=True)
    os._exit(0)


def proc_elapsed_s():
    '''Seconds since the launcher started this process (SB_LAUNCH_EPOCH); same origin as the --debug-cycles log to ~0.1 s.'''
    return time.time() - float(os.environ['SB_LAUNCH_EPOCH'])


scene = bpy.context.scene
scene.render.engine = 'CYCLES'
cp = bpy.context.preferences.addons['cycles'].preferences
cp.compute_device_type = 'OPTIX'
cp.get_devices()
for d in cp.devices:
    d.use = (d.type == 'OPTIX')
scene.cycles.device = 'GPU'
try:
    scene.cycles.use_preview_denoising = False
except Exception:
    pass
scene.cycles.preview_samples = 32


def draw_cb():
    if state.get("t_pre") is None:
        return
    region = bpy.context.region
    fb = gpu.state.active_framebuffer_get()
    w, h = region.width, region.height
    buf = fb.read_color(w // 2 - 16, h // 2 - 16, 32, 32, 4, 0, 'FLOAT')
    a = np.array(buf, dtype=np.float32).reshape(-1, 4)
    t = T() - state["t_pre"]
    OUT["trace"].append((round(t * 1e3, 1), [round(float(x), 4) for x in a.mean(axis=0)[:3]]))
    OUT["region"] = [w, h]
    if state["t_switch"] is None:
        return
    t = T() - state["t_switch"]
    if t > 40.0:
        quit_now("timeout")
    # done when the patch has been stable for >1.5 s after at least 3 distinct values
    tr = OUT["trace"]
    if len(tr) > 10 and t > 3.0:
        t_abs = T() - state["t_pre"]
        recent = [x[1] for x in tr if x[0] > (t_abs - 1.5) * 1e3]
        if recent and all(max(abs(r[i] - recent[0][i]) for i in range(3)) < 0.003 for r in recent) and len(recent) > 3:
            quit_now("stable")


def switch():
    wm = bpy.context.window_manager
    win = wm.windows[0]
    area = next(a for a in win.screen.areas if a.type == 'VIEW_3D')
    region = next(r for r in area.regions if r.type == 'WINDOW')
    sp = area.spaces.active
    sp.overlay.show_overlays = False
    sp.show_gizmo = False
    if kv.get("full", "0") == "1":
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
    OUT["region_before_switch"] = [region.width, region.height]
    bpy.types.SpaceView3D.draw_handler_add(draw_cb, (), 'WINDOW', 'POST_PIXEL')
    area.tag_redraw()
    state["t_pre"] = T()
    # let one solid-view frame be drawn so the trace has a baseline, then switch
    def go():
        state["t_switch"] = T()
        OUT['t_switch_since_process_start_s'] = proc_elapsed_s()
        OUT["switch_offset_ms_from_trace_zero"] = (state["t_switch"] - state["t_pre"]) * 1e3
        win2 = bpy.context.window_manager.windows[0]
        area2 = next(a for a in win2.screen.areas if a.type == 'VIEW_3D')
        reg2 = next(r for r in area2.regions if r.type == 'WINDOW')
        OUT['region_at_switch'] = [reg2.width, reg2.height]
        reg2.data.view_perspective = 'CAMERA'
        area2.spaces.active.shading.type = 'RENDERED'
        area2.tag_redraw()
        bpy.app.timers.register(lambda: quit_now('fixed 25 s after switch'), first_interval=25.0)
        return None
    bpy.app.timers.register(go, first_interval=1.0)
    return None


def watchdog():
    if T() - t_start > 100:
        quit_now("watchdog")
    return 1.0


bpy.app.timers.register(switch, first_interval=1.0)
bpy.app.timers.register(watchdog, first_interval=5.0)
