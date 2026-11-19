# Task files for M1a (first pixels)

Scope: docs/ROADMAP.md, M1a. One file per task, `tasks/NNN-slug.md`, <= 60 lines, fields in this order (lint section `taskfile`, task 005): Goal, Read first, Scope, Out of scope, Reference, Do, Acceptance, Look at, Canary, Budget, Depends on, Size, Progress. `land.ps1` deletes a task file when it lands. Numbers are ids, not order: 101-111 came from the plan review and sit in the order below.
Tasks 001-009 are landed by the lead by hand; from 010 on every task goes through `tools/land.ps1 prepare|merge`. Measurements (078, 091-098) put their result, with the producing command, in the PR body and state what each outcome triggers. Designed-when-built decisions (R63) are made and written back in 007, 031, 038, 053, 054.
Source files: a task that adds a file under a globbed directory (003 step 7) edits no CMake; a task that adds a target does. Gate steps are tagged `cpu` or `local`; hosted `ci` runs the `cpu` ones (030).
Not here (roadmap "Before M1a"): spike F part A, spike G, and the owner answers listed there (022 needs the CIE answer; 093 needs Nsight counters).

## Order (number and short name)
Tooling: 001 process files, 002 layout and licences, 003 CMake and build.ps1, 004 script library and locks, 005 lint core, 006 check fast and budgets.toml, 007 Embree asset, 008 deps.toml and fetch, 009 hosted CI, 010 land and status, 011 bench history and report skeleton.
C ABI: 012 astroray.h and generator, 013 bindings and build identity, 014 contexts, devices and log, 015 structured exceptions, 016 report table, 017 CLI info.
Kernel foundation: 018 kernel dialect, 019 spectral packet and vectors, 020 statistics harness, 021 sampler, 022 CIE observer and specgen, 023 Jakob-Hanika upsampling, 024 wavelength table and Planck, 025 path state and enums, 026 CpuDevice and ledger, 027 GPU build rules, 028 CudaDevice and GPU tests, 029 agreement harness, 030 kernel gate and check gate.
Scene and traversal: 031 scene ABI declarations, 032 host scene and generations, 034 triangulation, 035 mesh ingest, 033 recorder, 036 instances, 037 Embree, 038 ray offset, 039 route_hit and CPU intersect, 040 OptiX acceleration, 041 OptiX pipeline, 042 traversal agreement.
Scheduler, film, session: 043 queue_build, 044 material sort, 045 scheduler, 046 response channels, 047 film resolve, 048 pixel filter, 049 statistics and AOVs, 050 FilmView and FITS, 051 present and display ring, 052 sessions, 104 session readback, 053 perspective camera, 054 generate_camera, 101 camera paths in the session.
Materials: 055 ops.def, 056 material layouts, 057 VM interpreter, 058 compiler core, 059 compiler outputs and magenta, 060 image ingest, 061 TEX_IMAGE, 062 EON lobe, 063 GGX lobe, 064 E table and lobe stack.
Lights: 065 point and sphere, 066 rectangle and disk, 067 triangles, world and alias table, 068 lamp ingest and instances, 105 lamp primitives.
Device data and first shading: 102 KernelGlobals and CUDA upload, 103 shade_background, 114 production acceleration structures, 115 CUDA traversal stages in sessions, 116 emissive-hit light index.
Rendering and checks: 069 CLI render, 070 scene builders, 071 shade_surface emission, 072 analytic harness and A5, 073 BSDF continuation and A1, 074 NEE and shadows, 075 shade_light and A4, 106 lamp scenes and NEE hook, 076 integrator checks, 107 render invariants, 077 AOV agreement, D1, D2, 078 detection power.
Blender: 079 extension skeleton, 080 sync core, 081 mesh and instances, 117 session film passes, 082 lamps, camera, settings, 083 materials and world, 110 images and node values, 084 F12 and gate smoke, 085 viewport present, 111 viewport edits and replay, 086 batching and heat maps, 087 API surface.
Scenes: 088 assets, 089 B-1M and B-5M, 090 Cornell box and S3.
Measurements and close: 091 process budgets, 092 M-1 probe, 093 wavefront overhead, 094 start-up and sync, 095 first frame, 096 viewport rate, 097 Blender probes, 098 OIDN static TBB, 099 device failures, 108 dev kernel reload, 109 Blender fault, 118 partition scan, 119 sort scan, 120 fixed grids and graph capture (from 093, ruling 79), 121 gate overlap (B-09 headroom), 122 sort by program and 123 pass gap (from 120, ruling 80), 124 main-thread calls, 125 light build, 126 engine accounting (from 094), 100 gallery and report.

## Dependency sketch (exact edges: each file's "Depends on")
```
001-011 tooling --> 012-017 ABI --> 018-030 kernel foundation (check gate from 030)
030 --> 031-042 scene + traversal --> 043-054, 104, 101 scheduler, film, session, camera
018-024 --> 055-064 materials      023 --> 065-068, 105 lights (067 needs 059; 105 needs 041)
101 + 064 + 068 --> 102 upload --> 103 --> 069-078, 106, 107 rendering and analytic checks
013 + 069 --> 079-087, 110, 111 Blender (084 adds the gate's Blender smoke)   008 --> 088-090
076 + 086 + 089 + 090 --> 091-099, 108, 109 measurements --> 100 gallery and report
```
Lanes that can run side by side (at most two implementers, 0009 d27): 055-064 beside 043-054; 065-067 beside 037-042; 080-083 beside 071-078; 088-090 beside 079-087.
