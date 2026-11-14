# 044 Material sort of the shade_surface segment
Goal: shading runs in arbitrary material order -> `sort_count` and `sort_scatter` stably counting-sort the SHADE_SURFACE segment by material id when it is large enough and the scene has several materials, identically on CPU and GPU.
Read first: 0002 D7 step 3 (sort rule and PROVISIONAL thresholds), D8 (kernel list), D6 (segments); src/kernel/sched/step_table.h.
Scope: src/kernel/stages/sort.h, src/kernel_cpu/sort_{count,scatter}.cpp, src/kernel_gpu/sort_{count,scatter}.cu, src/kernel/launch_config.h, src/kernel/budget.toml, tests/cpu/test_sort.cpp, tests/gpu/test_sort_agree.cpp.
Out of scope: deciding when to call it (045 reads the thresholds); other segments (only shade_surface is sorted, as in Cycles).
Reference: Cycles `device/gpu/parallel_sorted_index.h` (Apache-2.0).
Do:
1. Material id per item = the material of its hit (instance material table + triangle slot, via scene_view.h).
2. sort_count: histogram over material ids; sort_scatter: stable scatter into a sorted copy of the segment's index array that the shade stage reads in place of the unsorted one.
3. Thresholds in launch_config.h: `kSortMinItems = 65536`, `kSortMinMaterials = 2` (PROVISIONAL, comment cites 0002 D7).
4. Rows in launch_config.h and budget.toml.
5. Tests: CPU (fast) stability and correctness on random ids with duplicates; GPU (gate) bitwise-equal output to the CPU for the same input.
Acceptance: `pwsh tools/check.ps1 gate` passes with `sort/*` cases and kernelgate rows for both kernels.
Look at: none.
Canary: tasks/044.canary.patch must turn red: astroray_tests_cpu "sort/stable by material".
Budget: two device TUs within B-05; spills 0.
Depends on: 043, 039.
Size: S (about 200 lines); stop and report at 2x.
Progress:
