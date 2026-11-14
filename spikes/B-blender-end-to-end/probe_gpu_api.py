import bpy, gpu, inspect
out = {}
def d(o): return [n for n in dir(o) if not n.startswith("_")]
out["gpu"] = d(gpu)
out["gpu.types"] = d(gpu.types)
out["gpu.compute"] = d(gpu.compute)
out["gpu.texture"] = d(gpu.texture)
out["gpu.state"] = d(gpu.state)
for c in ("GPUTexture", "GPUStorageBuf", "GPUShader", "Buffer", "GPUUniformBuf", "GPUBatch", "GPUOffScreen", "GPUFrameBuffer", "GPUShaderCreateInfo"):
    t = getattr(gpu.types, c, None)
    out[c] = d(t) if t else None
for c in ("GPUTexture", "GPUStorageBuf", "Buffer"):
    t = getattr(gpu.types, c, None)
    out[c + ".__doc__"] = (t.__doc__ or "")[:1500] if t else None
out["GPUTexture.__init__ doc"] = gpu.types.GPUTexture.__init__.__doc__
out["GPUStorageBuf.update doc"] = gpu.types.GPUStorageBuf.update.__doc__
out["gpu.compute.dispatch doc"] = gpu.compute.dispatch.__doc__
out["gpu.platform"] = d(gpu.platform)
try:
    out["backend"] = gpu.platform.backend_type_get()
    out["vendor"] = gpu.platform.vendor_get(); out["device"] = gpu.platform.device_type_get()
except Exception as e:
    out["backend"] = repr(e)
out["RenderEngine"] = d(bpy.types.RenderEngine)
out["RenderEngine.tag_redraw doc"] = bpy.types.RenderEngine.bl_rna.functions["tag_redraw"].description
out["RenderEngine.bind_display_space_shader doc"] = bpy.types.RenderEngine.bl_rna.functions["bind_display_space_shader"].description
out["RenderEngine funcs"] = [f.identifier for f in bpy.types.RenderEngine.bl_rna.functions]
import gpu_extras.presets as p
out["draw_texture_2d src"] = inspect.getsource(p.draw_texture_2d)
import json
print("PROBE_JSON_BEGIN"); print(json.dumps(out, indent=1, default=str)); print("PROBE_JSON_END")

