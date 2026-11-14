# 048 Pixel filters by importance sampling with Blender's width map
Goal: camera samples have no sub-pixel distribution -> Box, Gaussian and Blackman-Harris filters are importance-sampled exactly as Cycles maps Blender's filter width, each sample contributes to one pixel with weight 1, and the normalised splat footprint exists for light-path splats.
Read first: 0008 d5 (filter importance sampling, filters, default, width map copied from Cycles), d4 (splat footprint), 0003 d23 (dims 0-1 are the filter offset), 0008 V3; R2.
Scope: src/kernel/film/filter.h, src/host/film/filter_table.{h,cpp}, tests/cpu/test_filter.cpp.
Out of scope: generate_camera's use of it (054); splat producers (M5).
Reference: Ernst, Stamminger and Greiner 2006 (filter importance sampling); Cycles `intern/cycles/scene/film.cpp` (Apache-2.0: `filter_func_box`, `filter_func_gaussian`, `filter_func_blackman_harris`, the filter table construction and width handling, copied with the licence header and marked as changed); pbrt-v4 `src/pbrt/filters.h` `FilterSampler` (Apache-2.0) as the alternative read.
Do:
1. filter_table (host): builds the 1-D inverse-CDF table for the chosen filter and width the way Cycles' film.cpp does (same table size and width mapping), uploaded once per settings change.
2. filter.h (kernel): `sample_filter(table, u0, u1) -> (dx, dy)` in pixels, separable, so every sample lands in its own pixel with weight 1; `splat_weight(table, dx, dy)` normalised to unit integral over the footprint, and `splat_footprint(px, py)` listing the pixels whose centres lie within the radius (used from M5; tested now).
3. Default Blackman-Harris at 1.5 px (Blender's default).
4. Tests (fast unless noted): 0008 V3 chi-squared of 2^20 sampled offsets against each filter's density integrated per cell (harness, gate); the table built for widths 0.5, 1.5 and 3.0 equals values computed in the test from Cycles' formulas (1e-6); for each filter the footprint weights of an arbitrary splat position sum to 1 (1e-6); Box at 1 px puts every sample and every splat in exactly one pixel.
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="filter/*"` passes the three chi-squared tests.
Look at: none.
Canary: tasks/048.canary.patch must turn red: astroray_tests_cpu "filter/blackman harris chi2", "filter/footprint sums to one".
Budget: none.
Depends on: 020, 018.
Size: S (about 250 lines); stop and report at 2x.
Progress:
