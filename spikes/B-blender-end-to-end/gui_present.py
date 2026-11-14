"""Q1: present-cost / latency measurements inside a windowed, isolated Blender 5.3 (Vulkan). Throwaway.
Usage (after --): group=matrix|paced|stall|latency|sweep  [full=1] [res=1080,4k] [paths=...] [tag=...]
Every view_draw: wrap the native front buffer, build/update the GPU texture, draw it, tag_redraw.
Timings are CPU wall time inside view_draw (GPU work is asynchronous); the achieved redraw interval is the
end-to-end number. The native producer thread runs concurrently (paced at fps, or free-running with fps=0)."""
import os, sys, time, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import bpy
import gpu
from gpu_extras.batch import batch_for_shader
from gpu_extras.presets import draw_texture_2d
import gui_common as G
import sbn

pc = time.perf_counter
RES = {"1080": (1920, 1080), "4k": (3840, 2160)}
WARM, MEAS, TIMED_S = 60, 300, 5.0

# path -> (native source format, texture format, how, 32-bit words per pixel in the SSBO)
SRC_FMT = {
    "A32": (sbn.FMT_F32, "RGBA32F", "create_per_frame", 0),
    "A16": (sbn.FMT_F32, "RGBA16F", "create_per_frame", 0),   # f32 source: GPUTexture only accepts FLOAT Buffers
    "B16": (sbn.FMT_F16, "RGBA16F", "persistent_compute", 2),  # f16 words in an SSBO, unpackHalf2x16 in compute
    "B32": (sbn.FMT_F32, "RGBA16F", "persistent_compute", 4),  # f32 in an SSBO, store to persistent RGBA16F
    "B11": (sbn.FMT_R11, "RGBA16F", "persistent_compute", 1),  # packed R11G11B10F word, unpacked in compute
    "C8B": (sbn.FMT_U8, "RGBA8", "persistent_compute", 1),     # u8 words in an SSBO, unpackUnorm4x8 -> RGBA8
    "C8A": (sbn.FMT_F32, "RGBA8", "create_per_frame", 0),      # only direct route to RGBA8 (UBYTE Buffers rejected)
    "BASE": (sbn.FMT_F32, "RGBA16F", "none", 0),               # no upload: draw one persistent texture (Blender floor)
}

UNPACK11 = ("float u11(uint v){ uint e = (v >> 6u) & 31u; uint m = v & 63u; if (e == 0u) return float(m) * exp2(-20.0);"
            " return (1.0 + float(m) / 64.0) * exp2(float(e) - 15.0); }"
            "float u10(uint v){ uint e = (v >> 5u) & 31u; uint m = v & 31u; if (e == 0u) return float(m) * exp2(-19.0);"
            " return (1.0 + float(m) / 32.0) * exp2(float(e) - 15.0); }")
HEAD = "void main(){ ivec2 p = ivec2(gl_GlobalInvocationID.xy); if (p.x >= W || p.y >= SH) return; int i = p.y * W + p.x; ivec2 q = ivec2(p.x, p.y + Y0); "
COMPUTE_SRC = {
    "B32": HEAD + "imageStore(dst, q, src[i]); }",
    "B16": HEAD + "uvec2 s = src[i]; imageStore(dst, q, vec4(unpackHalf2x16(s.x), unpackHalf2x16(s.y))); }",
    "C8B": HEAD + "imageStore(dst, q, unpackUnorm4x8(src[i])); }",
    "B11": UNPACK11 + HEAD + "uint v = src[i]; imageStore(dst, q, vec4(u11(v), u11(v >> 11u), u10(v >> 22u), 1.0)); }",
}
SSBO_TYPE = {"B32": "vec4", "B16": "uvec2", "C8B": "uint", "B11": "uint"}
IMG_FMT = {"B32": "RGBA16F", "B16": "RGBA16F", "C8B": "RGBA8", "B11": "RGBA16F"}


def make_compute(path):
    info = gpu.types.GPUShaderCreateInfo()
    info.local_group_size(16, 16, 1)
    info.storage_buf(0, SSBO_TYPE[path], "src[]", qualifiers={'READ'})
    info.image(0, IMG_FMT[path], "FLOAT_2D", "dst", qualifiers={'WRITE'})
    for n in ("W", "SH", "Y0"):
        info.push_constant('INT', n)
    info.compute_source(COMPUTE_SRC[path])
    return gpu.shader.create_from_info(info)


def words_for(c, flat):
    return flat if c.path == "B32" else flat.view(np.uint32)


def wrap_buffer(c, a):
    return gpu.types.Buffer('FLOAT' if c.path in ("A32", "A16", "C8A", "B32") else 'UINT', a.size, a)


class Config:
    def __init__(self, path, res, draw="tex2d", fps=60.0, new_only=False, stripes=1, mode="free"):
        self.path, self.res, self.draw, self.fps, self.new_only, self.stripes, self.mode = path, res, draw, fps, new_only, stripes, mode
        self.name = f"{path}{'x%d' % stripes if stripes > 1 else ''}/{res}/{draw}/fps{int(fps)}" + ("/new-only" if new_only else "") + (f"/{mode}" if mode != "free" else "")
        self.w, self.h = RES[res]
        self.fb = None
        self.state = "setup"
        self.n = 0
        self.rec = {"wrap": [], "tex": [], "draw": [], "total": [], "interval": [], "lat_start": [], "lat_end": []}
        self.last_start = None
        self.tex = None; self.ssbos = []; self.shader = None; self.last_tex = None
        self.draw_shader = None; self.draw_batch = None
        self.result = {"name": self.name, "path": path, "res": res, "draw": draw, "w": self.w, "h": self.h, "fps_target": fps,
                       "stripes": stripes, "mode": mode, "new_only": new_only}
        self.t_meas0 = None; self.c_meas0 = 0; self.last_counter = -1; self.distinct = 0
        self.signalled = -1; self.thread_stop = False; self.thread = None


def build_batch():
    coords = ((0, 0), (1, 0), (1, 1), (0, 1))
    sh = gpu.shader.from_builtin('IMAGE')
    return sh, batch_for_shader(sh, 'TRI_FAN', {"pos": coords, "texCoord": coords})


class Present:
    done = False

    def __init__(self, configs):
        self.cfgs = configs
        self.i = 0
        self.cur = None
        self.eng = None
        self.res = G.OUT.setdefault("configs", [])

    def view_update(self, eng, ctx, dg):
        self.eng = eng

    # ---- wake-up mechanisms for the latency group (view_draw does not self-tag in these modes)
    def start_poll(self, c):
        if c.mode == "timer":
            def poll():
                if self.cur is not c:
                    return None
                ctr = c.fb.counter()
                if ctr != c.signalled:
                    c.signalled = ctr
                    self.eng.tag_redraw()
                return 0.001
            bpy.app.timers.register(poll, first_interval=0.0)
        elif c.mode == "thread":
            def run():
                while not c.thread_stop:
                    ctr = c.fb.counter()
                    if ctr != c.signalled:
                        c.signalled = ctr
                        self.eng.tag_redraw()
                    time.sleep(0.0005)
            c.thread = threading.Thread(target=run, daemon=True)
            c.thread.start()

    # ---- one frame of work for a config
    def present(self, eng, c, rw, rh, scene, upload=True):
        t0 = pc()
        if not upload or c.how == "none":
            tex = c.last_tex if c.how != "none" else c.tex
            return self._draw(eng, c, tex, rw, rh, scene, t0, t0, t0)
        arr = c.fb.front_array()
        flat = arr.reshape(-1)
        p = c.path
        if c.how == "create_per_frame":
            if c.stripes == 1:
                buf = wrap_buffer(c, flat)
                t1 = pc()
                tex = gpu.types.GPUTexture((c.w, c.h), format=SRC_FMT[p][1], data=buf)
            else:  # tiles: several smaller textures, each created from a slice of the same numpy memory
                bufs = []
                for j in range(c.stripes):
                    r0, r1 = c.h * j // c.stripes, c.h * (j + 1) // c.stripes
                    bufs.append((r0, r1, wrap_buffer(c, flat[r0 * c.w * 4: r1 * c.w * 4])))
                t1 = pc()
                tex = [(r0, r1, gpu.types.GPUTexture((c.w, r1 - r0), format=SRC_FMT[p][1], data=b)) for r0, r1, b in bufs]
        else:
            words = words_for(c, flat)
            per_row = c.w * SRC_FMT[p][3]
            sh = c.shader
            bufs = []
            for j in range(c.stripes):
                r0 = c.h * j // c.stripes
                r1 = c.h * (j + 1) // c.stripes
                bufs.append((r0, r1, wrap_buffer(c, words[r0 * per_row: r1 * per_row])))
            t1 = pc()
            sh.bind()
            sh.image("dst", c.tex)
            sh.uniform_int("W", c.w)
            for j, (r0, r1, b) in enumerate(bufs):
                c.ssbos[j].update(b)
                sh.storage_block("src", c.ssbos[j])
                sh.uniform_int("SH", r1 - r0)
                sh.uniform_int("Y0", r0)
                gpu.compute.dispatch(sh, (c.w + 15) // 16, (r1 - r0 + 15) // 16, 1)
            tex = c.tex
        c.last_tex = tex
        t2 = pc()
        return self._draw(eng, c, tex, rw, rh, scene, t0, t1, t2)

    def _draw(self, eng, c, tex, rw, rh, scene, t0, t1, t2):
        gpu.state.blend_set('ALPHA_PREMULT')
        eng.bind_display_space_shader(scene)
        tiles = tex if isinstance(tex, list) else [(0, c.h, tex)]
        for r0, r1, tt in tiles:
            if c.draw == "tex2d":
                draw_texture_2d(tt, (0, r0 * rh / c.h), rw, (r1 - r0) * rh / c.h)
            else:
                with gpu.matrix.push_pop():
                    gpu.matrix.translate((0, r0 * rh / c.h))
                    gpu.matrix.scale((rw, (r1 - r0) * rh / c.h))
                    c.draw_shader.uniform_sampler("image", tt)
                    c.draw_batch.draw(c.draw_shader)
        eng.unbind_display_space_shader()
        gpu.state.blend_set('NONE')
        t3 = pc()
        return tex, (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3

    def setup(self, c):
        src_fmt, tex_fmt, c.how, wpp = SRC_FMT[c.path]
        c.fb = sbn.Framebuffer(c.w, c.h, src_fmt, c.fps, 3)
        c.result["texture_fmt"], c.result["how"] = tex_fmt, c.how
        if c.how == "persistent_compute":
            c.tex = gpu.types.GPUTexture((c.w, c.h), format=IMG_FMT[c.path])
            words = words_for(c, c.fb.front_array().reshape(-1))
            per_row = c.w * wpp
            for j in range(c.stripes):
                r0, r1 = c.h * j // c.stripes, c.h * (j + 1) // c.stripes
                c.ssbos.append(gpu.types.GPUStorageBuf(wrap_buffer(c, words[r0 * per_row: r1 * per_row])))
            c.shader = make_compute(c.path)
        if c.how == "none":
            c.tex = gpu.types.GPUTexture((c.w, c.h), format="RGBA16F")
        if c.draw == "cached":
            c.draw_shader, c.draw_batch = build_batch()
        if c.mode in ("timer", "thread"):
            self.start_poll(c)

    def verify(self, eng, c, rw, rh, scene):
        """Pause the producer, upload one frame by this path, read pixels back, compare with the source."""
        if c.how == "none":
            c.result["verify_ok"] = None
            return
        c.fb.pause(True)
        time.sleep(0.08)
        tex, *_ = self.present(eng, c, rw, rh, scene)
        src = c.fb.front_array()
        if isinstance(tex, list):
            got = np.concatenate([np.array(t.read()).reshape(r1 - r0, c.w, 4) for r0, r1, t in tex]).astype(np.float32)
        else:
            got = np.array(tex.read()).reshape(c.h, c.w, 4).astype(np.float32)
        if SRC_FMT[c.path][1] == "RGBA8" and got.max() > 1.5:
            got = got / 255.0
        pts = [(0, 0), (c.h // 2, c.w // 3), (c.h - 1, c.w - 1), (c.h // 4, c.w // 2), (c.h // 4 * 3, c.w // 5)]
        errs = []
        for (y, x) in pts:
            if c.fb.fmt == sbn.FMT_R11:
                v = int(src[y, x, 0])
                def d(sh, mb):
                    e = (v >> (sh + mb)) & 31
                    m = (v >> sh) & ((1 << mb) - 1)
                    return m * 2.0 ** -(14 + mb) if e == 0 else (1 + m / (1 << mb)) * 2.0 ** (e - 15)
                s = np.array([d(0, 6), d(11, 6), d(22, 5), 1.0], np.float32)
            else:
                s = src[y, x].astype(np.float32)
                if c.fb.fmt == sbn.FMT_U8:
                    s = s / 255.0
            if SRC_FMT[c.path][1] == "RGBA8":
                s = np.clip(s, 0, 1)
            errs.append(float(np.max(np.abs(got[y, x] - s))))
        c.result["verify_max_abs_err"] = max(errs)
        c.result["verify_ok"] = max(errs) < (0.01 if SRC_FMT[c.path][1] == "RGBA8" else 2e-3)
        c.fb.pause(False)

    def finish(self, c, rw, rh):
        dt = pc() - c.t_meas0
        st = c.fb.stats()
        c.result.update({
            "region": [rw, rh],
            "view_draw_total_ms": sbn.stats_summary(c.rec["total"]),
            "wrap_ms": sbn.stats_summary(c.rec["wrap"]),
            "texture_ms": sbn.stats_summary(c.rec["tex"]),
            "draw_ms": sbn.stats_summary(c.rec["draw"]),
            "interval_ms": sbn.stats_summary(c.rec["interval"]),
            "latency_swap_to_draw_start_ms": sbn.stats_summary(c.rec["lat_start"]),
            "latency_swap_to_draw_end_cpu_ms": sbn.stats_summary(c.rec["lat_end"]),
            "view_draw_calls_per_s": c.n / dt,
            "distinct_source_frames_shown_per_s": c.distinct / dt,
            "producer_frames_per_s": (c.fb.counter() - c.c_meas0) / dt,
            "producer": st,
            "measure_s": dt,
        })
        self.res.append(c.result)
        G.save()
        c.thread_stop = True
        c.fb.destroy()
        c.tex = c.shader = None
        c.ssbos = []
        self.i += 1
        self.cur = None

    def view_draw(self, eng, ctx, dg):
        t_in = pc()
        t_in_ns = sbn.now_ns()
        self.eng = eng
        region = ctx.region
        rw, rh = region.width, region.height
        scene = dg.scene
        if self.cur is None:
            if self.i >= len(self.cfgs):
                self.done = True
                return
            self.cur = self.cfgs[self.i]
            self.setup(self.cur)
            self.cur.state = "warm"
            self.cur.n = 0
        c = self.cur
        ctr_before = c.fb.counter()
        # upload only when needed: always in 'free' matrix runs, only on a new frame otherwise
        upload = True
        if c.state == "meas" and (c.new_only or c.mode != "free") and ctr_before == c.last_counter:
            upload = False
        tex, tw, tt, td = self.present(eng, c, rw, rh, scene, upload)
        c.n += 1
        timed = c.new_only or c.mode != "free"
        if c.state == "warm":
            if c.n == 1:
                c.fb.stats(reset=True)
            if c.n >= WARM and (not timed or pc() - c.warm0 > 1.0 if hasattr(c, "warm0") else True):
                self.verify(eng, c, rw, rh, scene)
                c.state = "meas"
                c.n = 0
                c.fb.stats(reset=True)
                c.t_meas0 = pc()
                c.c_meas0 = c.fb.counter()
                c.last_counter = c.c_meas0
                c.last_start = None
        elif c.state == "meas":
            tend = pc()
            tend_ns = sbn.now_ns()
            if upload:
                c.rec["wrap"].append(tw); c.rec["tex"].append(tt); c.rec["draw"].append(td)
                c.rec["total"].append((tend - t_in) * 1e3)
                if c.last_start is not None:
                    c.rec["interval"].append((t_in - c.last_start) * 1e3)
                c.last_start = t_in
            if ctr_before != c.last_counter and upload:
                c.distinct += 1
                c.last_counter = ctr_before
                ts = c.fb.ring(ctr_before, 1)[0]
                if ts > 0:
                    c.rec["lat_start"].append((t_in_ns - ts) * 1e-6)
                    c.rec["lat_end"].append((tend_ns - ts) * 1e-6)
            if (not timed and c.n >= MEAS) or (timed and pc() - c.t_meas0 >= TIMED_S):
                self.finish(c, rw, rh)
        if (c.mode == "free") or self.cur is None:
            eng.tag_redraw()


class Sweep:
    """Host->GPU transfer cost versus size: GPUStorageBuf.update and GPUTexture(data=) create."""
    done = False
    ran = False

    def view_update(self, eng, ctx, dg):
        pass

    def view_draw(self, eng, ctx, dg):
        if self.ran:
            return
        self.ran = True
        out = G.OUT.setdefault("sweep", [])
        for mb in (2, 4, 8, 16, 24, 32, 48, 56, 64, 72, 96, 128, 192, 256):
            n = mb * 1024 * 1024 // 4
            a = np.zeros(n, np.float32)
            a[::1024] = 1.0
            b = gpu.types.Buffer('FLOAT', n, a)
            row = {"MB": mb}
            ssbo = gpu.types.GPUStorageBuf(b)
            ts = []
            for _ in range(12):
                t0 = pc(); ssbo.update(b); ts.append((pc() - t0) * 1e3)
            row["ssbo_update_ms_min"] = min(ts); row["ssbo_update_ms_med"] = sorted(ts)[len(ts) // 2]
            row["ssbo_GBps_at_min"] = mb / 1024.0 / (min(ts) * 1e-3)
            w = 4096
            h = n // 4 // w
            ts = []
            for _ in range(12):
                t0 = pc(); t = gpu.types.GPUTexture((w, h), format='RGBA32F', data=b); ts.append((pc() - t0) * 1e3)
            row["tex_create_ms_min"] = min(ts); row["tex_create_ms_med"] = sorted(ts)[len(ts) // 2]
            row["tex_GBps_at_min"] = mb / 1024.0 / (min(ts) * 1e-3)
            out.append(row)
            del ssbo, t
            G.save()
        self.done = True


def build_configs(kv):
    ress = kv.get("res", "1080,4k").split(",")
    group = kv.get("group", "matrix")
    cfgs = []
    if group == "matrix":
        for r in ress:
            for p in kv.get("paths", "BASE,A32,A16,B16,B32,B11,C8B,C8A").split(","):
                cfgs.append(Config(p, r))
            if r == "4k":
                for p, s in (("B16", 2), ("B16", 4), ("B32", 2), ("B32", 4), ("A32", 4), ("A32", 8), ("A16", 4)):
                    cfgs.append(Config(p, r, stripes=s))
        for r in ress:
            for p in [x for x in kv.get("cached", "A32,A16,B16,B11").split(",") if x]:
                cfgs.append(Config(p, r, draw="cached"))
    elif group == "paced":
        for r in ress:
            for p in kv.get("paths", "A32,B16,B11").split(","):
                for fps in (24, 30, 60, 120, 0):
                    cfgs.append(Config(p, r, draw="cached", fps=float(fps), new_only=True))
            if r == "4k":   # striped uploads (each update <= ~33 MB): see matrix
                for p, s in (("B32", 4), ("B16", 4), ("A32", 4)):
                    for fps in (60, 120, 0):
                        cfgs.append(Config(p, r, draw="cached", fps=float(fps), new_only=True, stripes=s))
    elif group == "stall":
        for r in ress:
            for p in kv.get("paths", "BASE,A32,B16").split(","):
                cfgs.append(Config(p, r, fps=0.0))
    elif group == "latency":
        for r in ress:
            for p in kv.get("paths", "B16,A32").split(","):
                for mode, fps in (("free", 60.0), ("timer", 60.0), ("thread", 60.0), ("timer", 30.0), ("timer", 120.0)):
                    cfgs.append(Config(p, r, draw="cached", fps=fps, new_only=(mode == "free"), mode=mode))
    return cfgs


kv = G.parse_args()
G.WATCHDOG_S = 1200.0
group = kv.get("group", "matrix")
scenario = Sweep() if group == "sweep" else Present(build_configs(kv))
G.start(scenario, os.path.join(G.HERE, "out", f"gui_present_{group}{kv.get('tag', '')}.json"), kv)
