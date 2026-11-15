# 051 film_present, film_heatmap and the display ring
Goal: no frame can be shown -> `film_present` converts the display mean to scene-linear working-space RGBA16F, `film_heatmap` maps sample count or bounce count through viridis into RGBA8, and a three-slot pinned host ring publishes frames tagged with their generation.
Read first: 0008 d12 (heat-map display, viridis, default ranges), d1 (rows bottom-up); 0007 d29 (present path, ring, clamp to half range), d30 (format), d40 (display modes), R57; 0002 D8 (film_present, film_heatmap rows).
Scope: src/kernel/film/present.h, src/kernel_cpu/film_present.cpp, src/kernel_cpu/film_heatmap.cpp, src/kernel_gpu/film_present.cu, src/kernel_gpu/film_heatmap.cu, src/host/display/display_ring.{h,cpp}, src/kernel/launch_config.h, src/kernel/budget.toml, cmake/kernels.cmake (film_present reads fp64 moments, so it joins AR_GPU_F64_MODULES), src/kernel/kernel_globals.h (the `display` member only), src/kernel/film/film_write.h and src/host/film/film_stats.cpp (the shared bounce-mean helper only), tests/cpu/test_present.cpp, tests/gpu/test_present_gpu.cpp.
Out of scope: the ABI calls `ar_display_acquire`/`release` (104); the addon's draw (085); heat-map modes beyond sample and bounce count (M8).
Reference: Smith and van der Walt 2015 (viridis, CC0): the 256-entry table copied with provenance into src/kernel/film/viridis.h.
Do:
1. film_present: per pixel display mean (S1/n) in working-space RGB, alpha 1, clamped to the finite half range +-65504, converted to IEEE half (round to nearest even), written to a device RGBA16F buffer of the render sub-rectangle, rows bottom-up.
2. film_heatmap: sample count over [0, max spp] or bounce mean over [0, max depth] through viridis into sRGB RGBA8 (the one 8-bit exception, R57); pixels with no data get a fixed grey.
3. display_ring: 3 pinned host slots; an async copy on a copy stream (CPU device: memcpy) fills the next free slot with {frame_id, generation, width, height, format, row bytes, data}; `acquire(after_frame)` returns the newest frame newer than `after_frame` or none; `release(frame_id)`; a slot being read is never overwritten.
4. Rows in launch_config.h and budget.toml for both kernels.
5. Tests: CPU (fast) half conversion of chosen values (0, 1, 65504, 70000 -> 65504, NaN never produced because invalid samples never reach the film), heat-map colours equal viridis entries at the range ends, ring ordering (newest wins, released slots reused, generations non-decreasing); GPU (gate) present output bit-identical to the CPU for the same moments.
Acceptance: `pwsh tools/check.ps1 gate` passes with `present/*` on both devices; kernelgate rows exist for both kernels.
Look at: none.
Canary: tasks/051.canary.patch must turn red: astroray_tests_cpu "present/half clamp", "present/ring newest frame".
Budget: two device TUs within B-05; spills 0.
Depends on: 047, 049.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- Kernels take only the KernelGlobals pointer (0002 D4): `DisplayTarget` (out, mode, range_max, in film/present.h) is the last member of KernelGlobals, `display`.
- Bounce mean has one source, `film_bounce_mean_value` in film_write.h; the heat map and host `film_bounce_mean` both call it.
- Ring slots are heap memory behind SlotAllocator and the copy is the synchronous Device::download until the Device seam has pinned allocation and a copy stream (104).
- Gate: k_film_present 37 regs, k_film_heatmap 25 (re-measured below), spills 0, 1.21 s compile each.
