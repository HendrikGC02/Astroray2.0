# Viewport presentation: GPU framebuffer to Blender 5.3

Research date 2026-11-14. Tags: VERIFIED = I read the primary source (release note, source file, API response).
REPORTED = secondary or search-summary only. DERIVED = my arithmetic, not measured. UNVERIFIED = not confirmed.
Source files were read from the `blender-v5.3-release` branch where noted, otherwise from `main` (5.4 dev).

## Question

What is the fastest legitimate path from an engine's GPU-resident framebuffer to Blender 5.3's viewport, now that
Vulkan is the default backend? Is there a zero-copy route for a Python-loaded native module, or is one CPU upload
per frame the practical floor? What does each frame cost, and how do we reduce it?

## Findings

### F1. Backend status and corrections to the recon file

- Vulkan is the default backend on Windows (not ARM) and Linux in 5.3. Minimum Vulkan lowered to 1.1. The note does
  not say whether OpenGL remains selectable or is removed. VERIFIED
  https://developer.blender.org/docs/release_notes/5.3/eevee/ (cites PR #158803). This upgrades the recon item
  that rested on one secondary source.
- 5.3 Python notes list exactly two GPU-relevant additions: `gpu.types.GPUStorageBuf` and `bl_write_viewport_depth`.
  No texture or buffer API change. VERIFIED https://developer.blender.org/docs/release_notes/5.3/python_api/
  5.4 notes (alpha) list no GPU or RenderEngine API changes yet. VERIFIED
  https://developer.blender.org/docs/release_notes/5.4/python_api/ . 5.2 added only `gpu.init()` for background mode
  and moved Hydra to the Hydra 2.0 API. VERIFIED https://developer.blender.org/docs/release_notes/5.2/python_api/
- Recon error: it cites issue #147815 as "about 200x slower display updates without interop". #147815 is a
  different bug (5.0 beta Vulkan viewport 7 fps vs 250 fps on a subdivided icosphere, a memory-flag regression,
  closed Resolved 2025-10-13). VERIFIED via https://projects.blender.org/api/v1/repos/blender/blender/issues/147815
  and its `/comments`. The 200x-class numbers come from #130106, see F5.

### F2. The `gpu` Python module in 5.3 (what an addon can do)

All of this is from `source/blender/python/gpu/*.cc` on `blender-v5.3-release` or `main`. Base URL
`https://raw.githubusercontent.com/blender/blender/<ref>/source/blender/python/gpu/`. The 5.3-branch directory
listing has 42 files and none wraps a pixel buffer, native handle or external memory
(https://projects.blender.org/api/v1/repos/blender/blender/contents/source/blender/python/gpu?ref=blender-v5.3-release).
VERIFIED

- `GPUTexture(size, *, layers=0, is_cubemap=False, format='RGBA8', data=None)` (`gpu_py_texture.cc`). VERIFIED
  - `data` must be a `gpu.types.Buffer` of format `FLOAT`. Any other format raises "Only Buffer of format `FLOAT`
    is currently supported". Checked on the 5.3 branch. So host data is always float32, even for `RGBA16F` or
    `RGBA8` textures. There is no UBYTE or half-float upload.
  - Data can only be supplied at construction. The method table is `clear` (single-value fill), `read` (synchronous
    readback), `free` (conditional), sampler setters (`extend_mode*`, `filter_mode`, `mipmap_mode`,
    `anisotropic_filter`). There is no in-place pixel update, no sub-rect update, no external-memory import.
  - Texture usage is hard-coded `GPU_TEXTURE_USAGE_GENERAL`, so a Python texture can be bound as a compute image.
  - Formats include `RGBA32F`, `RGBA16F`, `RGBA16`, `RGBA8`, `R11F_G11F_B10F`, `RGB16F`, `RG16F`, `R16F`.
- `gpu.texture.from_image(image)` returns the GPUTexture of an Image datablock ("memory is shared with Blender"),
  but there is no write method. VERIFIED. A February 2024 forum request for `GPUTexture.write()` / external-handle
  wrapping got a design response from Brecht and Jeroen Bakker (no cross-backend blit API; handles are raw addresses;
  format/mipmap issues) and no patch. VERIFIED https://forum.blender.org/t/adding-a-write-method-to-gpu-types-gputexture/33226
- `gpu.types.Buffer(format, dimensions, data)` (`gpu_py_buffer.cc`). VERIFIED
  - If `data` supports the buffer protocol (numpy does), the Buffer stores a reference and points at the source
    memory. No copy, no per-element Python work. Cost is proportional to the number of dimensions.
  - The Buffer's `format` argument is not checked against the source dtype. The caller's format decides how the bytes
    are read, so a float32 ndarray with `'FLOAT'` is the only safe pairing for a texture.
  - A sequence source is filled element by element (`PyArg_Parse` each). Never use it per frame.
  - Indexing, assignment and `to_list()` create Python objects per element. Never in the hot path.
- `gpu.types.GPUStorageBuf(data)` (new in 5.3; present on the 5.3 branch). VERIFIED
  - `data` is anything with the buffer protocol, requested as `PyBUF_SIMPLE`, so a numpy `uint8`/`float16`/`uint32`
    array works with no dtype restriction. Size is rounded up to 16 bytes.
  - `update(data)` rewrites the contents in place (padded size must match). `clear_to_zero()`, `read()` (blocks
    until the GPU is idle) and optional `free()`.
  - Bind to a shader with `GPUShader.storage_block(name, ssbo)`. Declare in `GPUShaderCreateInfo.storage_buf(slot,
    type_name, name, *, qualifiers={'READ','WRITE'})`.
- Compute from Python exists: `gpu.compute.dispatch(shader, gx, gy, gz)` with `GPUShaderCreateInfo.compute_source`,
  `.image(slot, format, type, name, qualifiers=...)`, `.local_group_size`, and `GPUShader.image(name, texture)`.
  VERIFIED (`gpu_py_compute.cc`, `gpu_py_shader_create_info.cc`, `gpu_py_shader.cc`). The official example
  "Custom compute shader (using image store)" is `doc/python_api/examples/gpu.11.py` (present in the 5.0.1 Debian
  source package). REPORTED (search result, page not opened).
- Shader creation is from `GPUShaderCreateInfo` only; creating shaders from GLSL files was removed in 5.0. VERIFIED
  https://developer.blender.org/docs/release_notes/5.0/python_api/ . `bgl` and `Image.bindcode` are gone.
- `gpu.platform` exposes vendor, renderer, driver version, device type, backend name (`'VULKAN'`/`'OPENGL'`) and a
  device list with `index`, `identifier`, `name`. No UUID, no VkDevice, no native handle. VERIFIED
  (`gpu_py_platform.cc`).
- `draw_texture_2d(texture, position, width, height, is_scene_linear_with_rec709_srgb_target=False)` builds a
  two-triangle batch with the built-in `IMAGE` shader (or `IMAGE_SCENE_LINEAR_TO_REC709_SRGB`), translates and scales
  to `width x height`, and binds the texture to sampler `"image"`. VERIFIED
  (`scripts/modules/gpu_extras/presets.py`). The width/height need not equal the texture size, so rendering at a
  lower resolution and stretching is one call; `GPUTexture.filter_mode()` picks nearest or linear.
- Streaming data into GPU objects from Python has been a known weak spot for years (readback via `Buffer.to_list()`
  took about a second for a 1024x1024 float image in 2021). REPORTED
  https://forum.blender.org/t/slow-access-to-buffer-and-gputexture-in-new-gpu-module/18962 . That thread is about
  readback, not upload. It is evidence only that per-element Python must be avoided.

### F3. What happens inside Blender when you upload (Vulkan backend, 5.3 branch)

- `VKTexture::update_sub` is asynchronous. It creates a host-visible staging buffer (`VMA_MEMORY_USAGE_AUTO_PREFER_HOST`),
  `memcpy`s into it, flushes the mapped range, and records a `VKCopyBufferToImageNode` on the render graph. It does
  not flush or wait. If format conversion is needed and rows are packed it converts the whole region into a temporary
  vector first. VERIFIED (`source/blender/gpu/vulkan/vk_texture.cc`, main).
- Conversion cost by pair (`vk_data_conversion.cc`, checked on the 5.3 branch). VERIFIED
  - float32 host to `RGBA32F` device: pass-through `memcpy`.
  - float32 host to `RGBA16F` device: NOT pass-through. `threading::parallel_for` in 4 Mi-element chunks calling
    `float_to_half_make_finite_array` (clamps Inf/NaN to finite). So at 1080p RGBA (8.3 M elements) only about 2 chunks
    exist; at 4K (33 M elements) about 8.
  - float32 host to `RGBA8`: serial scalar loop over every component, no threading, no SIMD. This is the slowest
    pair, so the Python-visible way to upload 8-bit data is also the slowest.
  - half or ubyte host data have memcpy fast paths, but the Python `GPUTexture` constructor cannot supply them.
- There is a `vkCopyMemoryToImageEXT` host-image-copy fast path (`allow_host_image_copy_`) that skips staging when the
  extension is available. Whether the driver on the 5070 Ti uses it for these textures is UNVERIFIED.
- `VKStorageBuffer::update` (default usage) also goes through a `VKStagingBuffer` host-to-device copy, buffer in
  `AUTO_PREFER_DEVICE`. No wait. VERIFIED (`vk_storage_buffer.cc`, main). It is in place, so no per-frame
  allocation of the destination.
- `VKTexture::read` flushes the render graph with `WAIT_FOR_COMPLETION`. Never call `GPUTexture.read()` or
  `GPUStorageBuf.read()` per frame. VERIFIED.

### F4. How Cycles gets zero-copy, and why a Python addon cannot

- `DisplayDriver` (`intern/cycles/session/display_driver.h`) is a C++ interface: `update_begin/end`,
  `map_texture_buffer()` returning `half4*`, `draw()` (may run concurrent with updates), plus
  `graphics_interop_get_device/update_buffer/activate/deactivate`. The interop buffer must be half-float RGBA.
  Device types `OPENGL`, `VULKAN`, `METAL`, each with a UUID. Vulkan handle is an opaque `VkBuffer` handle (Win32)
  or fd (Unix), and for Vulkan "this transfers ownership of the handle". VERIFIED
  https://raw.githubusercontent.com/blender/blender/main/intern/cycles/session/display_driver.h
  Design page: allocate a GPU texture, update it from a render thread, use a dedicated or thread-safe graphics
  context, use interop to avoid CPU-GPU copies. VERIFIED https://developer.blender.org/docs/features/cycles/drivers/
- Blender side (`intern/cycles/blender/display_driver.cpp`): `GPU_pixel_buffer_get_native_handle()` returns the
  handle and size; the UUID is `GPU_platform_uuid()`; when the PBO is unmapped Blender copies it into the texture with
  `GPU_texture_update_sub_from_pixel_buffer(... GPU_DATA_HALF_FLOAT)`. VERIFIED
- Vulkan backend (`vk_pixel_buffer.cc`): gated on `device.extensions_get().external_memory`; creates the buffer
  with export enabled, then `vkGetMemoryWin32HandleKHR` with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`
  (fd variant on Unix). VERIFIED
- CUDA side (`intern/cycles/device/cuda/graphics_interop.cpp`): `cuImportExternalMemory` with
  `CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32` (not owning the handle on Windows), then
  `cuExternalMemoryGetMappedBuffer(offset=0)`. "Vulkan buffer is always mapped", so no per-frame map call. The
  OpenGL path uses `cuGraphicsGLRegisterBuffer` with per-use map. VERIFIED. HIP has a matching file.
- Shipped: commit "Cycles: Vulkan CUDA graphics interop" b174e5f0 (Brecht Van Lommel; authored 2025-04-12, committed
  2025-04-28; PR #137363; 10 files, HIP included). VERIFIED
  https://projects.blender.org/api/v1/repos/blender/blender/git/commits/b174e5f0d1b02c71938068a0892c65ef87db3699
  The file at tag `v4.5.0` already contains the Vulkan import; at `v4.4.0` it does not; `blender-v5.3-release` has it.
  VERIFIED (raw files at those refs). So the recon "did it ship" question is answered: yes, 4.5 onward.
- Restrictions from the PR: interop only when driver and device ids match exactly; the GPU fence is a CPU-side
  `vkWaitSemaphores` event (GPU-only sync would need external semaphores); a tester reported final-render
  failures, which Brecht could reproduce randomly with and without the PR. VERIFIED
  https://projects.blender.org/api/v1/repos/blender/blender/issues/137363/comments
- Developer docs: Vulkan is for UI drawing, "isn't planned or viable" for running Cycles. VERIFIED
  https://developer.blender.org/docs/features/gpu/vulkan/ . That page says nothing about external memory.
- Consequence: Cycles zero-copy relies on in-tree C++ access to `GPUPixelBuffer`, which Python does not wrap.
  Nothing in the Python `gpu` module, `gpu.platform`, `RenderEngine` RNA, or the 5.2-5.4 notes exposes the VkDevice,
  a handle, or an import call. VERIFIED (files above) for 5.3; 5.4 only for "no change announced so far".

### F5. Cost of the missing interop, measured by others

- Issue #130106 (Cycles viewport on Vulkan vs OpenGL, 4.4 era). Wall display-update time: CPU 0.0905 s vs 36.6 s
  (406x); OptiX with interop on 0.0290 s vs 5.13 s (183x); OptiX interop off 0.146 s vs 5.995 s (41x). Sergey
  Sharybin doubted "lack of interop" alone explained it (slowdown persisted with interop disabled), and no root cause
  is recorded in the thread. VERIFIED (numbers read from the issue comments)
  https://projects.blender.org/api/v1/repos/blender/blender/issues/130106/comments
  Treat as a warning that early Vulkan paths had unrelated bugs, not as a per-frame cost for a 2026 build. The
  interop PR landed after these measurements.
- No 5.x measurement of Python `GPUTexture` upload cost exists in what I found. All per-frame figures below are
  DERIVED and must be replaced by the spike.

### F6. Colour management in 5.x (changes what the engine must output)

- `RenderEngine.bind_display_space_shader(scene)` on `blender-v5.3-release` binds `GPU_SHADER_3D_IMAGE` and nothing
  else (note "image binding slot is 0"); `support_display_space_shader` returns `true` unconditionally; `unbind`
  calls `GPU_shader_unbind()`. The RNA description still says it converts linear to display space with scene colour
  management. VERIFIED (`makesrna/intern/rna_render.cc`, 5.3 branch). Do not rely on it for colour management.
- GPU colour design: each region has a Rec.709 sRGB framebuffer; some regions, named as including the 3D Viewport
  Render, also have a Scene Linear framebuffer; the display transform and OCIO options are applied when regions are
  blitted to the window framebuffer, not during region drawing. Shader output is expected in the target
  framebuffer's colour space. The page says a non-Rec.709 scene-linear working space was not fully working "at the
  time of writing". VERIFIED https://developer.blender.org/docs/features/gpu/color_management/ . The page does not
  state what `view_draw` must output. Verification pass: the page does name "3D Viewport Render" as a region with a
  Scene Linear framebuffer "dedicated to user content", and `external_engine.cc` (5.3) calls `view_draw` into the
  DRW viewport `default_fb` (cleared to transparent black) after `ED_region_pixelspace`. Whether `default_fb` is that
  scene-linear buffer is still not shown in code, so keep it as DERIVED, to be confirmed in S3.
- Implication: the engine should hand Blender scene-linear values and let Blender apply the view transform (Filmic,
  AgX, ACES, exposure, gamma, HDR). An engine-side tonemap to display-referred 8-bit would be transformed a second
  time. See Options.
- Official example `bpy.types.RenderEngine.1.py` (main): `view_draw` sets `gpu.state.blend_set('ALPHA_PREMULT')`, calls
  `bind_display_space_shader(scene)`, rebuilds `CustomDrawData` only when the viewport size changes (a
  `GPUTexture(format='RGBA16F', data=Buffer('FLOAT',...))`), calls `draw_texture_2d`, then `unbind_display_space_shader()`
  and `blend_set('NONE')`. It says "The renderer is expected to quickly draw the render". `view_update` runs in the main
  thread. VERIFIED (read via summary of the raw file).
- RenderEngine RNA: `tag_redraw()` only does `engine->flag |= RE_ENGINE_DO_DRAW`; `tag_update()` sets
  `RE_ENGINE_DO_UPDATE`. `bl_use_gpu_context` is "for engines that render using OpenGL"; `bl_write_viewport_depth` lets
  the engine write its own depth in `view_draw`. VERIFIED (`rna_render.cc`, 5.3 branch). Where the flag is polled and
  how fast the viewport wakes when `tag_redraw` is set from a non-main thread is UNVERIFIED.

### F7. How other engines present

- Radeon ProRender addon: RPR framebuffer read to a numpy array (`rpr_context.get_image()`), then
  `GPUTexture(format='RGBA16F', data=gpu.types.Buffer('FLOAT', ...))` and `draw_texture_2d` with premultiplied blend;
  the raw path gets a new array each draw, so a new texture is built each draw; it never calls
  `bind_display_space_shader`, only unbinds. VERIFIED
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/RadeonProRenderBlenderAddon/master/src/rprblender/engine/viewport_engine.py
- LuxCore: pulls the film into numpy (`GetOutputFloat(RGBA_IMAGEPIPELINE)`), clamps to 65519 to avoid fp16 black,
  wraps in a `Buffer`, builds a new `GPUTexture` in `draw()` (`RGBA16F` or `RGB16F`), draws with the built-in `IMAGE`
  shader. Tone mapping is done by LuxCore's own image pipeline, not Blender's. VERIFIED
  https://raw.githubusercontent.com/LuxCoreRender/BlendLuxCore/master/draw/viewport.py
- Blender's own Hydra viewport (C++, in tree): GPU path takes the colour AOV as a `GPUTexture` from the render task
  delegate with no CPU copy; buffer fallback creates `SFLOAT_16_16_16_16`/`SFLOAT_32_32_32_32` and calls
  `GPU_texture_update`; draws with `GPU_SHADER_3D_IMAGE`. VERIFIED
  https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/source/blender/render/hydra/viewport_engine.cc
  This is available to in-tree C++ only; the Hydra scene delegate itself is a separate concern (another workstream).
- Spectra (Rust + Vulkan addon, solo developer): out-of-process Vulkan daemon, "zero-stall IPC", no stated
  readback method or viewport timings. REPORTED (author's forum post only)
  https://blenderartists.org/t/spectra-realtime-restir-caustics-true-spectral-engine-for-blender-rust-vulkan-addon/1655156
- Octane, Redshift, Malt, appleseed: not researched.
- Pattern: every shipped Python-addon engine I could read does one CPU round trip per presented frame and builds a
  new `GPUTexture` from a float32 `Buffer`. Only in-tree C++ engines avoid it.

### F8. Inter-process transport (for an out-of-process engine)

- Windows named shared memory: Python `mmap(-1, size, tagname=...)` opens an existing tag or creates one, and mmap
  objects behave like `bytearray`. VERIFIED https://docs.python.org/3/library/mmap.html . The docs do not use the phrase
  "shared between processes" for `tagname`; the Win32 `CreateFileMapping` name semantics are what makes it work (not
  re-verified here). An mmap view can be handed to numpy and then to `GPUStorageBuf.update` (buffer protocol).
- CUDA IPC exists on Windows but "is supported for compatibility purposes but not recommended as it comes with
  performance cost". VERIFIED https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__DEVICE.html (cudaIpcCloseMemHandle
  entry). It is irrelevant for presentation because Blender cannot import device memory anyway.
- The process boundary does not change Blender-side cost. It adds a host copy (engine pinned buffer to shared memory)
  unless the engine's device-to-host copy can target the mapped region directly. `cudaHostRegister` on a file-mapping
  view is UNVERIFIED.

### F9. Hardware facts used for estimates

- RTX 5070 Ti: PCI Express Gen 5, 16 GB GDDR7, 256-bit. VERIFIED
  https://www.nvidia.com/en-us/geforce/graphics-cards/50-series/rtx-5070-family/ (spec table). Bandwidth 896 GB/s
  is REPORTED (reviews). PCIe 5.0 x16 theoretical is about 63 GB/s per direction (DERIVED from 32 GT/s x 16 lanes x
  128/130); real host-to-device throughput from pageable or staged memory is far lower and not measured.

### Derived payload sizes (DERIVED, exact arithmetic)

| Resolution | pixels | RGBA8 or R11G11B10F or RGB10A2 (4 B) | RGBA16F (8 B) | RGBA32F (16 B) |
|---|---|---|---|---|
| 1920x1080 | 2,073,600 | 8.3 MB | 16.6 MB | 33.2 MB |
| 3840x2160 | 8,294,400 | 33.2 MB | 66.4 MB | 132.7 MB |

Under the Python `GPUTexture` path the host payload is always the RGBA32F row, whatever the texture format. As an
illustration only: at an assumed 10 GB/s effective host memcpy, Blender's staging copy of 33.2 MB is about 3 ms and of
132.7 MB about 13 ms, before the engine's own device-to-host copy and the float-to-half conversion. These are
assumptions to be replaced by S1, not findings.

## Options and tradeoffs

The real axes are: (1) who owns the pixels' path into Blender's GPU memory (in-tree C++ vs Python-reachable API),
(2) host payload encoding, (3) persistent vs per-frame GPU objects, (4) in-process vs out-of-process engine.

**Axis 1: zero-copy.** Only one legitimate route exists and it is not available to an addon.
- In-tree C++ `DisplayDriver`-style integration (Cycles, Hydra). Needs a Blender fork or an upstream change. Not
  shippable as a normal extension. Upstream receptiveness to a Python API for importing external memory is unproven;
  the one public request (2024) was met with design caveats and no patch.
- Unsupported tricks for a native module inside Blender's process (an implicit Vulkan layer intercepting
  `vkCreateDevice`, or resolving Blender-internal symbols). Even with the VkDevice you would still lack the VkImage
  behind a `GPUTexture` and Blender's render-graph synchronisation, so you could not legitimately write into it.
  UNVERIFIED in detail, fragile across versions, not recommended.
- Drawing from the engine into its own overlay window or swap chain over the viewport: not a legitimate
  integration (no depth, overlays, colour management or tiling with Blender's region).
- Therefore one CPU hop per presented frame is the practical floor for a Python-loaded module on 5.3, regardless of
  whether the engine is CUDA/OptiX or Vulkan-native. This also means presentation does not favour NVIDIA-only over a
  vendor-neutral backend (see Recommendation).

**Axis 2/3: how to do the one hop.**
- A. Per-frame `GPUTexture(format=..., data=Buffer('FLOAT', ...))` (what ProRender, LuxCore and the official example do).
  Simplest, well-trodden. Costs: float32 host payload (16 B/px), per-frame VkImage allocation and deferred destruction,
  CPU float-to-half conversion for `RGBA16F` (parallel, 4 Mi-element chunks), `RGBA32F` is memcpy but doubles the bytes.
  `RGBA8` is the slowest conversion. Works on any 5.x without shaders.
- B. Persistent `GPUStorageBuf` updated in place with raw fp16 (or packed) bytes, a compute shader unpacking it into a
  persistent `RGBA16F` `GPUTexture`, then `draw_texture_2d`. Avoids the float32 expansion (half the bytes), the
  conversion, and per-frame texture allocation. Costs: more moving parts in Python (create-info, compute dispatch),
  an extra GPU pass, and several unverified details (S2). Still one full copy per frame; no sub-rect upload API
  (`update` needs the full padded size), so partial updates would need multiple smaller SSBOs.
- C. As B but a fragment shader reads the SSBO directly and writes the viewport framebuffer, skipping the texture.
  Saves a pass but loses sampler filtering for the resolution divider and needs care with blending/depth. Only worth
  it if S2 shows the compute pass is measurable.

**Encoding choices (for B or C; for A only float32 is possible).**
- Scene-linear `RGBA16F` (8 B/px). Safe for HDR and all view transforms.
- Packed `R11G11B10F` as a `uint32` (4 B/px): half the bytes, HDR-capable, about 5-6 bit mantissa. Plausible for
  navigation previews; check banding in S3. No alpha.
- Engine-side tonemapped 8-bit display-referred: wrong with Blender's pipeline as described in F6 (double
  transform). Only valid if a custom shader inverts the view transform, which loses fidelity and cannot track Filmic/
  AgX/ACES changes. Acceptable at most as a fast preview during navigation when the view transform is Standard.
  Avoid in the baseline.

**Frame-rate levers independent of path.**
- Render at a resolution divider during navigation and stretch with `draw_texture_2d(width, height)` to the region.
  Payload falls with the square of the divider (divider 2 is 1/4 of the bytes), the same approach Cycles uses.
  Combine with nearest or linear filter per zoom.
- Present only when the engine has new pixels (skip identical frames). During convergence, reduce present frequency
  (for example geometric spacing of samples) instead of every pass.
- Double-buffer the engine's device-to-host copy into pinned memory (`cudaHostAlloc`) on a copy stream so it overlaps
  the next render; the Blender-side memcpy is then the only serialized cost. Pinned memory does not speed up Blender's
  own staging copy.
- Never read back from Blender (`read()` blocks on GPU completion).

**Axis 4: in-process vs out-of-process.**
- In-process native module (nanobind/pybind11) can expose the pinned buffer through the buffer protocol: zero extra
  host copies, the numpy view goes straight into `Buffer` or `GPUStorageBuf.update`. Costs: crash takes down Blender;
  CUDA context lifetime in the host process; GIL must be released in the render thread.
- Out-of-process engine: isolation and restartability. Costs: one more host copy (or a shared-memory target for the
  D2H copy, unverified) plus a sync mechanism. Presentation latency is not improved.

## Recommendation for Astroray 2.0

1. Plan for exactly one host hop per presented frame and treat zero-copy as out of scope for the addon product. The
   evidence: the Python `gpu` module in 5.3 has no external-memory, native-handle or in-place texture-write API (F2,
   F4); Cycles' zero-copy uses in-tree C++ `GPUPixelBuffer` handles (F4); Blender's own Python-addon engines all do a CPU hop
   (F7); the only upstream request for an external-texture API stalled (F2). If zero-copy ever matters, the route is an
   upstream API contribution, not a workaround. Keep the engine's framebuffer interface (device pointer + format +
   stream) separate from the presenter so a future in-tree DisplayDriver or upstream API plugs in.
2. Output scene-linear and let Blender do the display transform. The engine does not tonemap for display. Evidence: F6
   (display transform applied at the window stage; `bind_display_space_shader` is now a plain image shader). Verify
   with S3 before building on it.
3. Baseline presenter: build option A first (a day of work, works on any 5.x). Use `RGBA16F` texture and a float32
   `Buffer` wrapped over the pinned numpy view; measure it. Because `RGBA16F` costs a conversion and `RGBA32F` costs
   double the bytes, S1 decides between them.
4. Prototype option B during the spike and adopt it if it beats A by a clear margin at 4K (candidate rule: save more than
   30% of present time or remove stutter from per-frame texture allocation). Its evidence is F2/F3: `GPUStorageBuf.update`
   takes raw bytes in place, compute with image store is available from Python, and the textures are created with
   general usage.
5. Navigation: resolution divider with stretched draw as the primary lever, optionally packed `R11G11B10F` via B.
   No 8-bit display-referred present in the baseline.
6. Host boundary: in-process native module as the default, with the engine render thread and D2H copy off the main
   thread, and presentation inside `view_draw` on the main thread. An out-of-process split is a separate decision to
   take on crash isolation and CUDA-context grounds, not on presentation grounds (another workstream owns that).
7. GPU backend choice: presentation is neutral. A Vulkan-native, vendor-neutral engine pays the same single CPU hop as a
   CUDA/OptiX engine; neither can reach Blender's device memory from Python. The only presentation asymmetry is the
   engine-side device-to-host copy, trivially available on both. Do not let viewport presentation drive the backend
   decision unless the owner chooses the fork/in-tree route, where CUDA external memory into Blender's Vulkan pixel
   buffer is proven today (F4) while a Vulkan-native engine would need its own export of an equivalent buffer.
8. Test on both Blender backends if OpenGL is still selectable in 5.3 (UNVERIFIED), since the upload code path differs;
   the Python API is backend-agnostic, so the same presenter code runs on both.

## What a measurement spike must settle

Run in Blender 5.3 on Windows 11 with the RTX 5070 Ti, Vulkan (and OpenGL if selectable), a dummy engine producing a
GPU-resident test pattern (no renderer needed). Record command and output for each; save a screenshot per case.

- S1. Present-cost matrix. For 1920x1080 and 3840x2160, and formats RGBA32F / RGBA16F / RGBA8 / R11F_G11F_B10F, path
  A: time `view_draw` main-thread wall (perf_counter around the upload + draw), UI frame time while orbiting, CPU
  per-thread utilisation, steady-state fps, and memory churn from per-frame textures. Include the engine-side D2H
  copy (pinned, async) separately. Report min/median/p99 over at least 300 frames. Replace every DERIVED number
  above.
- S2. Option B viability. Does `gpu.compute.dispatch` work inside `view_draw` on Vulkan? How is an unsized `uint`
  array declared in `storage_buf`? Maximum SSBO size (4K RGBA16F is 66.4 MB; check `maxStorageBufferRange`, likely
  fine on NVIDIA but UNVERIFIED)? Is `GPUStorageBuf.update` per frame cheaper than option A at 4K? Does it need a
  barrier between dispatch and sampling?
- S3. Colour correctness. Draw a known scene-linear ramp (0.18 grey, values above 1.0, saturated primaries) and
  compare against the same image in an Image Editor under Standard, Filmic/AgX/ACES view transforms, exposure and
  gamma changes, and an HDR display if available. Confirm that the target is scene linear (no double transform)
  and that premultiplied-alpha blending behaves; check for banding with `R11F_G11F_B10F`. Also test `draw_texture_2d`
  with and without `is_scene_linear_with_rec709_srgb_target`.
- S4. Wake-up and pacing. Latency from native render-thread frame completion to pixels on screen using
  `tag_redraw()` from a non-main thread vs `bpy.app.timers` polling; does `view_draw` get called promptly during
  orbit, and what does Blender do when `view_draw` takes 10-20 ms?
- S5. GIL and threading. Is the GIL held during `GPUStorageBuf.update` / `GPUTexture(...)` construction? Does the
  native render thread stall during `view_draw`?
- S6. Resource churn. Allocation rate and p99 frame time with a new 4K `GPUTexture` per frame vs a persistent one.
- S7. Out-of-process. Cost of D2H into a named file-mapping (including whether `cudaHostRegister` works on the view)
  vs in-process pinned buffer, and the added cross-process sync latency.
- S8. Resolution-divider quality and cost: divider 1/2/4 at 4K with nearest vs linear sampling, and the visible seam when
  switching back to full resolution.
- S9. Interop sanity (informational): run Cycles OptiX viewport on this machine on Vulkan and read its viewport frame
  time at 1080p/4K as the best-case reference that the engine's presenter is judged against.

## Unverified or conflicting

- Whether OpenGL remains selectable or is removed in 5.3: the 5.3 notes do not say, but they carry an OpenGL vs Vulkan benchmark table covering 5.3, hinting OpenGL is still present (REPORTED, weak); Phoronix discussion of removal in
  6.0 is REPORTED in recon only (not fetched here).
- The recon item "5.0 bug report measured about 200x slower display updates without interop: #147815" is wrong
  (F1). The 200x-class numbers are #130106 (4.4 era) and their cause is not settled. No current-version viewport
  frame-time data for Cycles on Vulkan was found.
- That the render-engine viewport draws into a scene-linear framebuffer is inferred from the colour-management design
  page, which does not mention `view_draw` (S3). The same page says non-Rec.709 scene-linear working spaces were not
  fully working when written; whether that is still true in 5.3 is unknown.
- Whether `vkCopyMemoryToImageEXT` host-image-copy is active on this GPU/driver for Blender's textures.
- Behaviour of `tag_redraw()` from a worker thread (flag only is verified; polling site and thread safety are not).
- Whether the GIL is held during Python GPU uploads.
- Whether `cudaHostRegister` works on a Windows file-mapping view.
- Python docs for `GPUTexture`, `GPUStorageBuf`, `RenderEngine` were not readable as pages (truncated); everything
  about their behaviour comes from the C++ source, which is authoritative but could differ from documented intent.
- `gpu.11.py` example content (compute image-store) was not opened; only its existence and description are REPORTED.
- Octane, Redshift, Malt, appleseed presentation paths; the Spectra daemon's actual presentation method.
- `gpu.types.Buffer` UBYTE element assignment uses a signed format code (values 128-255 may not round-trip). Source
  observation only; irrelevant unless 8-bit upload via Buffer is attempted.
- 5.4 is in alpha until 2027-02-03 (per recon). Its Python API notes list no GPU changes today, and that can change
  before release. Re-run F2 checks (texture constructor, `GPUStorageBuf`, `bind_display_space_shader` body) against
  the 5.4 branch when it is cut.

## Verification pass

Independent re-check on 2026-11-14 against primary sources. Result: the Recommendation is unchanged.

- Claim 1, gpu module has no external-memory, native-handle or in-place texture update; `GPUTexture(data=)` needs a
  `FLOAT` Buffer. CONFIRMED. `gpu_py_texture.cc` (5.3 branch) raises "Only Buffer of format `FLOAT` is currently
  supported"; methods are clear, read, free (conditional), extend/filter/mipmap/anisotropic setters only.
  https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/source/blender/python/gpu/gpu_py_texture.cc
  The 42-file directory listing was not re-counted.
- Claim 1, `GPUStorageBuf.update()` in place. CONFIRMED. Buffer protocol input, padded to 16 B, update size must match
  the original, not resized; also clear_to_zero, read, conditional free.
  https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/source/blender/python/gpu/gpu_py_storagebuffer.cc
  5.3 notes: "A new `gpu.types.GPUStorageBuf` was added" and `bl_write_viewport_depth`; nothing else GPU.
  https://developer.blender.org/docs/release_notes/5.3/python_api/
- Claim 2, Cycles Vulkan-CUDA interop. CONFIRMED with a date nuance: commit b174e5f0 was authored 2025-04-12 and
  committed 2025-04-28; message cites CUDA external memory and a Vulkan UUID match; touches Cycles display_driver,
  CUDA and HIP graphics_interop only.
  https://projects.blender.org/api/v1/repos/blender/blender/git/commits/b174e5f0d1b02c71938068a0892c65ef87db3699
  The 4.5 inclusion, the opaque Win32 handle and `cuImportExternalMemory` details were not re-fetched this pass.
- Claim 3, Vulkan upload conversions. CONFIRMED. Float32 to RGBA16F is `parallel_for` with 4 Mi-element chunks calling
  `float_to_half_make_finite_array`; float32 to RGBA8 is a serial per-component loop; float32 to RGBA32F is
  PASS_THROUGH, a plain `memcpy`. At 1080p RGBA16F (8.29 M elements) that is about 2 chunks.
  https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/source/blender/gpu/vulkan/vk_data_conversion.cc
  Async-via-render-graph and blocking `read()` were not re-read.
- Claim 4, display-space shader. CONFIRMED. 5.3 `rna_render.cc`: bind gets `GPU_SHADER_3D_IMAGE` and binds it,
  support returns `true`, unbind calls `GPU_shader_unbind()`.
  https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/source/blender/makesrna/intern/rna_render.cc
- Claim 4, display transform at the window stage and viewport-render target. PARTLY CONFIRMED. The colour-management
  page says certain regions, naming 3D Viewport Render, have a Scene Linear framebuffer for user content.
  https://developer.blender.org/docs/features/gpu/color_management/
  `external_engine.cc` shows `view_draw` runs into the DRW `default_fb`, cleared transparent black; that this buffer is
  the scene-linear one is not shown in code (still DERIVED, S3).
  https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/source/blender/draw/engines/external/external_engine.cc
- Claim 5, Vulkan default in 5.3 with PR #158803, minimum Vulkan 1.1. CONFIRMED. The page does not say whether OpenGL
  remains selectable; its OpenGL vs Vulkan benchmark table for 5.3 hints that it does (REPORTED).
  https://developer.blender.org/docs/release_notes/5.3/eevee/
- Claim 5, no GPU import API in 5.2 to 5.4 Python notes. CONFIRMED for 5.3 (above); 5.2 and 5.4 pages were not
  re-fetched and 5.4 is alpha, so only "none announced so far" holds.
- Claim 5, ProRender and LuxCore do a CPU round trip with a new Buffer-backed GPUTexture per draw. COULD NOT VERIFY
  in this pass (source files not re-read); the first researcher's reading stands. The Hydra in-tree GPU path was also
  not re-read.
- Still unresolved from the main list, unchanged: #147815 vs #130106 attribution, #130106 timings, `gpu.11.py`
  content, `vkCopyMemoryToImageEXT` use, `tag_redraw` from a worker thread, GIL behaviour, `cudaHostRegister` on
  file-mapping views.
- Effect on Recommendation: none. One CPU hop per presented frame, scene-linear output, option A first with option B
  prototyped in the spike, backend-neutral presentation. Item 2 keeps its "verify with S3" condition.
