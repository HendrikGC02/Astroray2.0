"""Q3 baseline: Cycles (OptiX on the 5070 Ti) 1-sample 1080p render in background mode, persistent data off.
Run: blender -b <scene.blend> --factory-startup --debug-cycles --verbose 3 --python cycles_bench.py -- S1|S2
Renders: 3 x 1 sample (1st is cold-in-process: device init + kernel load; 2nd/3rd are the warm-process figures),
then 1 x N samples; per-sample cost = (T_N - T_1) / (N - 1), so pre-first-sample time ~= T_1 - per_sample."""
import os, sys, time, json
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
which = sys.argv[sys.argv.index("--") + 1]
OUT = {"scene": which, "blender": bpy.app.version_string}
T = time.perf_counter

scene = bpy.context.scene
scene.render.engine = 'CYCLES'
cp = bpy.context.preferences.addons['cycles'].preferences
cp.compute_device_type = 'OPTIX'
cp.get_devices()
OUT["devices"] = [(d.name, d.type, bool(d.use)) for d in cp.devices]
for d in cp.devices:
    d.use = (d.type == 'OPTIX')
OUT["devices_after"] = [(d.name, d.type, bool(d.use)) for d in cp.devices]
scene.cycles.device = 'GPU'
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.render.use_persistent_data = False
scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = 1920, 1080, 100
scene.render.use_border = False
scene.cycles.max_bounces = 4
scene.render.use_lock_interface = True

ev = []
bpy.app.handlers.render_init.append(lambda *a: ev.append(("render_init", T())))
bpy.app.handlers.render_pre.append(lambda *a: ev.append(("render_pre", T())))
bpy.app.handlers.render_stats.append(lambda *a: ev.append(("render_stats", T())))
bpy.app.handlers.render_post.append(lambda *a: ev.append(("render_post", T())))
bpy.app.handlers.render_complete.append(lambda *a: ev.append(("render_complete", T())))


def render(n):
    scene.cycles.samples = n
    ev.clear()
    t0 = T()
    bpy.ops.render.render(write_still=False)
    dt = (T() - t0) * 1e3
    rec = {"samples": n, "render_op_ms": dt,
           "events_ms_from_op_start": [(k, round((t - t0) * 1e3, 1)) for k, t in ev]}
    return rec


OUT["renders"] = []
for n in (1, 1, 1, 1, 65, 65):
    r = render(n)
    OUT["renders"].append(r)
    print("CYCLES_RENDER", which, n, round(r["render_op_ms"], 1), "ms", flush=True)
    json.dump(OUT, open(os.path.join(HERE, "out", f"cycles_{which}{os.environ.get('CYCLES_TAG', '')}.json"), "w"), indent=1)

# save the last render for visual inspection
img = bpy.data.images.get("Render Result")
if img is not None:
    try:
        img.save_render(os.path.join(HERE, "out", f"cycles_{which}_render.png"))
    except Exception as e:
        OUT["save_render_error"] = str(e)
json.dump(OUT, open(os.path.join(HERE, "out", f"cycles_{which}{os.environ.get('CYCLES_TAG', '')}.json"), "w"), indent=1)
print("CYCLES_DONE", which, flush=True)
os._exit(0)
