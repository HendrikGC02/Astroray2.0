"""GUI probe: what the Blender 5.3 gpu module actually accepts (run inside view_draw on the real backend)."""
import os, sys, time, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import gpu
import gui_common as G
import sbn

pc = time.perf_counter


def trial(res, name, fn, repeat=1):
    ts = []
    last = None
    for _ in range(repeat):
        t0 = pc()
        try:
            last = fn()
            ts.append((pc() - t0) * 1e3)
        except Exception as e:
            res[name] = {"error": f"{type(e).__name__}: {e}"}
            return None
    res[name] = {"ms_min": min(ts), "ms_med": sorted(ts)[len(ts) // 2], "ok": True}
    return last


class Probe:
    done = False
    ran = False

    def view_update(self, eng, ctx, dg):
        pass

    def view_draw(self, eng, ctx, dg):
        if self.ran:
            return
        self.ran = True
        res = G.OUT.setdefault("probe", {})
        res["backend"] = gpu.platform.backend_type_get()
        res["renderer"] = gpu.platform.renderer_get()
        res["vendor"] = gpu.platform.vendor_get()
        res["version"] = gpu.platform.version_get()
        caps = {}
        for nm in dir(gpu.capabilities):
            if nm.startswith("_") or nm == "compute_shader_support_get":
                continue
            try:
                caps[nm] = getattr(gpu.capabilities, nm)()
            except Exception as e:
                caps[nm] = "n/a: " + str(e)[:60]
        res["capabilities"] = caps
        w, h = 1920, 1080
        n = w * h * 4
        fb = sbn.Framebuffer(w, h, sbn.FMT_F32, 1.0)
        time.sleep(0.2)
        a32 = fb.front_array()
        flat = a32.reshape(-1)
        t = res.setdefault("buffer", {})
        # Buffer from numpy: which forms are accepted, and what do they cost at 1080p?
        buf = trial(t, "Buffer('FLOAT', n, numpy_flat_f32)", lambda: gpu.types.Buffer('FLOAT', n, flat), repeat=3)
        trial(t, "Buffer('FLOAT', [h,w,4], numpy_3d_f32)", lambda: gpu.types.Buffer('FLOAT', [h, w, 4], a32), repeat=3)
        # does the Buffer share memory with numpy (zero-copy) or copy?
        if buf is not None:
            flat[0] = 123.0
            t["buffer_shares_memory_with_numpy"] = bool(buf[0] == 123.0) if False else None
            try:
                t["buf[0]_after_numpy_write"] = float(buf[0]) if not hasattr(buf[0], "__len__") else str(buf[0])
            except Exception as e:
                t["buf[0]_after_numpy_write"] = repr(e)
            t["numpy_wrote"] = 123.0
        # Buffer from memoryview / array.array / list (slow path)
        small = flat[:4096].copy()
        trial(t, "Buffer('FLOAT', 4096, list)", lambda: gpu.types.Buffer('FLOAT', 4096, small.tolist()))
        trial(t, "Buffer('FLOAT', n) zero-init", lambda: gpu.types.Buffer('FLOAT', n), repeat=3)
        # does a gpu Buffer export the buffer protocol (-> native worker could write straight into it)?
        zb = gpu.types.Buffer('FLOAT', n)
        try:
            mv = memoryview(zb)
            t["memoryview(Buffer)"] = {"ok": True, "format": mv.format, "shape": list(mv.shape), "readonly": mv.readonly, "nbytes": mv.nbytes}
            na = np.frombuffer(zb, dtype=np.float32)
            t["np.frombuffer(Buffer)"] = {"ok": True, "size": int(na.size), "writeable": bool(na.flags.writeable)}
        except Exception as e:
            t["memoryview(Buffer)"] = {"error": f"{type(e).__name__}: {e}"}
        # other Buffer formats
        a16 = None
        fb16 = sbn.Framebuffer(w, h, sbn.FMT_F16, 1.0)
        fb8 = sbn.Framebuffer(w, h, sbn.FMT_U8, 1.0)
        time.sleep(0.2)
        a16 = fb16.front_array(); a8 = fb8.front_array()
        trial(t, "Buffer('UBYTE', n, numpy_u8)", lambda: gpu.types.Buffer('UBYTE', n, a8.reshape(-1)), repeat=3)
        trial(t, "Buffer('FLOAT', n, numpy_f16)  [dtype mismatch]", lambda: gpu.types.Buffer('FLOAT', n, a16.reshape(-1)))
        trial(t, "Buffer('UINT', n//2, numpy_f16.view(uint32))", lambda: gpu.types.Buffer('UINT', n // 2, a16.reshape(-1).view(np.uint32)), repeat=3)
        trial(t, "Buffer('INT', n//2, numpy_f16.view(int32))", lambda: gpu.types.Buffer('INT', n // 2, a16.reshape(-1).view(np.int32)), repeat=3)

        # Textures: which (format, Buffer format) combos are accepted, cost at 1080p
        tx = res.setdefault("texture", {})
        b32 = gpu.types.Buffer('FLOAT', n, flat)
        b8 = gpu.types.Buffer('UBYTE', n, a8.reshape(-1))
        trial(tx, "RGBA32F <- FLOAT buf", lambda: gpu.types.GPUTexture((w, h), format='RGBA32F', data=b32), repeat=5)
        trial(tx, "RGBA16F <- FLOAT buf", lambda: gpu.types.GPUTexture((w, h), format='RGBA16F', data=b32), repeat=5)
        trial(tx, "RGBA8 <- FLOAT buf", lambda: gpu.types.GPUTexture((w, h), format='RGBA8', data=b32), repeat=5)
        trial(tx, "RGBA8 <- UBYTE buf", lambda: gpu.types.GPUTexture((w, h), format='RGBA8', data=b8), repeat=5)
        trial(tx, "R11F_G11F_B10F <- FLOAT buf(4ch?)", lambda: gpu.types.GPUTexture((w, h), format='R11F_G11F_B10F', data=b32))
        trial(tx, "RGBA16F no data", lambda: gpu.types.GPUTexture((w, h), format='RGBA16F'), repeat=5)
        trial(tx, "RGBA32F no data", lambda: gpu.types.GPUTexture((w, h), format='RGBA32F'), repeat=5)

        # Storage buffer: persistent, in-place update
        sb = res.setdefault("storage_buf", {})
        sbuf = trial(sb, "GPUStorageBuf(Buffer FLOAT n)", lambda: gpu.types.GPUStorageBuf(b32), repeat=1)
        if sbuf is not None:
            trial(sb, "GPUStorageBuf.update(FLOAT buf)", lambda: sbuf.update(b32), repeat=5)
            trial(sb, "GPUStorageBuf.update(new Buffer from numpy)", lambda: sbuf.update(gpu.types.Buffer('FLOAT', n, flat)), repeat=5)
        # f16 source as UINT words (2 halves per uint) for the compute-unpack path
        bu = gpu.types.Buffer('UINT', n // 2, a16.reshape(-1).view(np.uint32))
        sbu = trial(sb, "GPUStorageBuf(Buffer UINT n/2) f16 words", lambda: gpu.types.GPUStorageBuf(bu))
        if sbu is not None:
            trial(sb, "GPUStorageBuf(UINT).update", lambda: sbu.update(bu), repeat=5)

        cs = res.setdefault("compute", {})
        ci = gpu.types.GPUShaderCreateInfo
        for nme in ("storage_buf", "image", "compute_source", "local_group_size", "push_constant", "typedef_source"):
            cs["doc_" + nme] = getattr(ci, nme).__doc__
        cs["GPUShader.image doc"] = gpu.types.GPUShader.image.__doc__
        cs["GPUShader.storage_block doc"] = gpu.types.GPUShader.storage_block.__doc__
        cs["gpu.shader"] = [n for n in dir(gpu.shader) if not n.startswith("_")]
        G.save()
        self.done = True


if __name__ == "__main__" or True:
    kv = G.parse_args()
    out = os.path.join(G.HERE, "out", "gui_probe.json")
    G.start(Probe(), out, kv)



