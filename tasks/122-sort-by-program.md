# 122 Sort shade_surface only when material programs differ (ruling 80)
Goal: 120 measured that the material sort makes B-5M slower (16.92 ms sorted vs 15.52 ms unsorted per pass; k_shade_surface 5.27 vs 4.35 ms): its 300 materials share one program and differ only in constants, so sorting buys no SIMT coherence and scatters path-state accesses (DRAM read 690 -> 835 MB, L2 hit 75 -> 68 %) -> sort_wanted counts distinct material programs, not material headers, and the sort runs only when at least kSortMinMaterials programs are present in the segment.
Read first: 0002 D7 step 3 (sort rule, kSortMinItems, kSortMinMaterials), Risks item 7; RULINGS.md ruling 80; 120's PR body (Do 5 table and Nsight Compute numbers); src/kernel/stages/sort.h; src/host/scheduler.cpp (sort_wanted); src/kernel/vm/ (program ids in the material layout).
Scope: src/kernel/stages/sort.h, src/kernel/stages/queue_build.h (if the histogram or sort_wanted is computed there), src/host/scheduler.{h,cpp} (sort_wanted only), src/kernel/launch_config.h (thresholds), tests/cpu/test_sort.cpp, tests/cpu/test_scheduler.cpp (sort host contract), tests/gpu/test_sort_agree.cpp, docs/decisions/0002-kernel-architecture.md (D7 step 3); by lead ruling: src/host/material/material_store.{h,cpp} (count in rebuild_locked), a host-only field next to scene_host in src/host/scene/scene_tables.{h,cpp}, SchedulerConfig::material_program_count in src/host/scheduler.h, the one line at src/host/session/session.cpp:674, and one small b5m variant builder under scenes/ (not in the bench sets).
Out of scope: a locality-preserving sort key (path-slot block, then material; Cycles' sort partitions): only if a multi-program scene shows the locality loss outweighing the gain, written by the lead from that measurement.
Reference: Cycles integrator/path_trace_work_gpu.cpp and kernel/integrator/state_flow.h (Apache-2.0): what Cycles sorts by (shader id) and when; read it and cite it, do not guess.
Do:
1. Define "program" from the material layout (the compiled program a header points to, or CONSTANT_SURFACE as one program); the key stays the material id, so the order within a sorted segment is unchanged (stable, bit-identical to today when the sort runs).
2. sort_wanted: segment size >= kSortMinItems and distinct programs among the segment's materials >= kSortMinMaterials (the count comes from the histogram the sort already builds or a per-scene table built at upload; no extra pass over items on the host).
3. Tests: B-5M-like (300 materials, one program) does not sort; 300 materials over >= 2 programs sorts and matches the bucket reference on CPU and GPU; existing sort tests unchanged.
4. Re-measure b5m (no sort expected) and a two-program variant of b5m built by tools/scenes.py if one exists (else say so): pass ms sorted vs unsorted, k_shade_surface ms; PR body Result: with commands (120's method).
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the table.
Look at: none.
Canary: tasks/122.canary.patch must turn red: astroray_tests_cpu "sort/one program among many materials stays unsorted" "sort/program count" "sort/300 materials over two programs sort and match a bucket reference" "sort/one real material plus default hits stays unsorted" "scheduler/sort host contract" "session/shade_surface items are sorted once the scene has two material programs", astroray_tests_gpu "sort/300 materials over two programs cpu gpu agree with a bucket reference"
Budget: no gate time added.
Depends on: 120.
Size: S (about 150 lines); stop and report at 2x.
Progress:
- started; stopped before code: the per-scene program count needed files outside Scope.
- lead ruling: program = CONSTANT_SURFACE as one, else program_offset (no bytecode dedup, default excluded); count from a per-scene table at upload; Scope widened as above.
- sort_wanted counts surface programs (sort_program_count at upload, MaterialArrays -> SceneTableSet -> SchedulerConfig); CPU, GPU and session tests green.
- lead ruling: tests/cpu/test_session.cpp joins Scope (fixture: material 1 gets its own program; case renamed).
- measured (nsys period / shade_surface): b5m 16.12 / 4.36 ms unsorted vs 17.62 / 5.29 forced; b5m_two_programs 16.56 / 4.65 sorted vs 15.77 / 4.13 unsorted.
- canary: 5 hunks, each red alone.
