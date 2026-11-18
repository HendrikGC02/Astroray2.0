# 119 Material sort without the per-tile column sum, measured against B-19
Goal: 093 measured the shade_surface material sort at 34.5 % of a B-5M pass (three sorts, 7.9 ms each, longest launch 11.5 ms in k_sort_scatter): sort.h has no scan kernel, so every scatter tile sums all earlier tiles' histogram columns, which grows with tiles squared -> the sort reuses 118's single-pass scan over its material histograms and its share is re-measured.
Read first: 0002 D7 step 3 (sort rule, PROVISIONAL thresholds), D8 (kernel list), Risks item 7; src/kernel/stages/sort.h; 118's scan; 093's and 118's PR bodies (method and numbers).
Scope: src/kernel/stages/sort.h, src/kernel_gpu/sort_*.cu, src/kernel_cpu/sort_*.cpp, src/kernel/launch_config.h (rows, kSortMinItems if re-measured), src/kernel/budget.toml (rows), src/host/scheduler.cpp (sort launch sequence only), tests/cpu/test_sort.cpp, tests/gpu/test_sort_agree.cpp, docs/decisions/0002-kernel-architecture.md (D7 step 3, D8). Lead ruling (option A, a k_sort_scan kernel): src/kernel/stages/queue_build.h (the scan split into one function taking (counts, rows, bins); queue_build keeps its behaviour and calls it), src/host/device.h (KernelId::SORT_SCAN after FILM_BUILD_ACTIVE, kKernelIdCount), src/device/cpu/cpu_device.cpp (registration, tiled_grid case), src/device/cuda/cuda_device.cpp (kKernelNames entry), src/kernel/kernel_globals.h (the comment), tests/cpu/test_scheduler.cpp (the registration check) join Scope. ar::popc is used in queue_build's ballot ranking only; the sort keeps one tile per thread (no vote, no __match_any_sync).
Out of scope: changing what is sorted or the sort key (Cycles' rule: shade_surface by material id); fixed grids (120).
Reference: Cycles device/gpu/parallel_sorted_index.h (Apache-2.0) for the sort pattern; Merrill and Garland 2016 (via 118's scan) for the offsets.
Do:
1. Compute the per-material offsets with one scan over the (tile x material) histogram (118's scan function, one source), so scatter tiles read their offsets instead of summing columns; output order identical to today (stable within a material).
2. Tests: sorted order bit-identical to the current implementation and CPU vs GPU, with 300 materials and >= 1,000,000 items; the existing sort tests unchanged.
3. Re-measure b5m (and b1m, single material: the sort must not launch) with tools/bench.ps1 and nsys as in 093: sort GPU time as % of pass, longest launch (B-17); with the sort cheap, re-check kSortMinItems by one measurement on b5m (sorted vs unsorted pass time) and record it; PR body Result: with commands.
4. From 118: 0002 D6 still calls the partition a reduce-then-scan (Blelloch 1990); point it at D7 (c)'s single-pass scan. The warp ranking's popcount is a loop nvcc happens to turn into POPC: add an explicit portable `ar::popc` (src/kernel/portability.h joins Scope) and use it in queue_build and the sort.
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the before/after table.
Look at: none.
Canary: tasks/119.canary.patch must turn red: astroray_tests_cpu with the new sort case (the patch shifts one material's offset), astroray_tests_gpu sort agreement case.
Budget: no gate time added.
Depends on: 118.
Size: S (about 200 lines); stop and report at 2x.
Progress:
- started (task/119-sort-scan).
