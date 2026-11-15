# 043 queue_build, the step table and shadow-slot reservation
Goal: there is no partition of work by stage -> the three queue_build kernels stably partition both pools by `next_stage` on CPU and GPU, fill the step table with segments, launch counts, refill count and shadow-slot bases, and reserve shadow slots deterministically without atomics; the first stage cubins are embedded in the DLL.
Read first: 0002 D5 (pools, deterministic slot assignment), D6 (partition, step table), D4 (stage shape), D10 (budget rows), D5 verification row (shadow pool below demand); 0009 d3 (embedding).
Scope: src/kernel/stages/queue_build.h, src/kernel/sched/step_table.h, src/kernel_cpu/queue_build_{count,scan,scatter}.cpp, src/kernel_gpu/queue_build_{count,scan,scatter}.cu, src/kernel/launch_config.h, src/kernel/budget.toml, tests/cpu/test_queue_build.cpp, tests/gpu/test_queue_build_agree.cpp.
Out of scope: the host loop and refill launches (045); the material sort (044).
Reference: Blelloch 1990, "Prefix Sums and Their Applications" (reduce-then-scan); Cycles `device/gpu/parallel_prefix_sum.h`, `parallel_active_index.h` (Apache-2.0) as patterns.
Do:
1. step_table.h: `StepTable` (< 256 B): per Stage the segment start, launch count and, for producers, shadow-slot base; the refill count; per pool the high-water mark. Main and shadow pools have their own TERMINATED values (TERMINATED, SHADOW_TERMINATED).
2. queue_build_count: block histograms of next_stage over both pools up to their high-water marks; queue_build_scan: reduce-then-scan over blocks and fills the step table; queue_build_scatter: stable scatter of pool-local indices into per-stage segments (item-index order inside each segment). The same stage functions run on both targets.
3. Shadow reservation: free shadow slots are the shadow pool's SHADOW_TERMINATED segment in index order; each producer stage's `k` comes from launch_config.h; producers in Stage order receive consecutive ranges; an item at position i owns slots [base + k*i, base + k*(i+1)); if free slots cannot cover a producer's segment, its launch count becomes floor(free_left / k) and the remaining items keep their next_stage.
4. Rows in launch_config.h and budget.toml for the three kernels (T = 256, B = 0, stack class OTHER, spills 0).
5. Tests: CPU (fast): partition of 10^6 random next_stage values equals a reference stable partition; a shortage case launches exactly floor(free/k) items and drops none (0002 D5 verification); GPU (gate): CPU and GPU produce identical segments and step tables for the same input (0009 d17(i), integer code).
6. With these first stages, astroray_kernels_cpu/_f64 exist and the GPU cubins are embedded: `ar_build_info` reports `kernels=embedded` and a GPU test loads `k_queue_build_count` from RCDATA.
Acceptance: `pwsh tools/check.ps1 gate` passes; kernelgate prints rows for the three kernels with 0 spills; `out/bin/astroray_tests_gpu.exe --tier=gate -tc="queue_build/*"` prints `segments identical`.
Look at: none.
Canary: tasks/043.canary.patch must turn red: astroray_tests_cpu "queue_build/stable partition", "queue_build/shadow shortage drops nothing".
Budget: three new device TUs within B-05; declared spills 0, stack <= 128 B.
Depends on: 025, 028, 030.
Size: M (about 450 lines); stop and report at 2x.
Progress:
- Done. Three kernels + step_table.h + CPU/GPU TUs; canary tasks/043.canary.patch turns "stable partition" and "shadow shortage drops nothing" red (applied, rebuilt, reverted).
- StepTable moved out of kernel_globals.h into sched/step_table.h (adds high_water[2], invalid_items; 196 B); KernelGlobals gained queue_counts (last member). Scope exceptions: kernel_globals.h, cmake/kernels.cmake (link kernels_cpu into tests_cpu), CMakeLists.txt (tests_gpu embeds core resources, links kernels_cpu).
- Choices the records do not give: refill_count = size of the main TERMINATED segment (free slots below high_water); shadow_base is an index into shadow_queue; invalid next_stage values counted in invalid_items and placed in no segment; producers (k=1) are SHADE_SURFACE, _EXT, SHADE_LIGHT_PATH, _EXT, SHADE_VOLUME; tile = 1024 items, scan is one GPU thread (PROVISIONAL, 093/B-19).
