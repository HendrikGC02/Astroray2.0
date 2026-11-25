# Rulings on conflicts between records 0001-0009 (lead, 2026-11-14)

The nine records were written in parallel and disagree in places. These rulings are binding. Each record is edited to comply; where a record is named as owner of a topic, the other records refer to it and do not restate it.

## General

- **G1. Milestones live only in `docs/ROADMAP.md`.** Every record's "What lands when" section is reduced to two lists: "must exist from the first milestone because retrofitting is expensive" (each item with its reason) and "can be added later without redesign". No record assigns features to M1, first release or later.
- **G2. One owner per topic.** A number, name, layout or budget is stated once, in the owning record below. Other records cite it by decision id.
- **G3. No scheduled jobs.** Nothing runs unattended on a timer. "Nightly" checks become `check full`, run on demand.
- **G4. Reserved hooks.** A hook stays only if removing it would force a later change to the path state layout, the C ABI, or a kernel's signature. Everything else reserved "for later" is deleted from the records.

## Ownership

| Topic | Owner |
|---|---|
| C ABI rules, naming, versioning, error and status model, the per-render report (schema, codes, deduplication, how it is polled) | 0001 |
| Kernel list and names, queues, pools, launch bounds, register and compile gates, OptiX pipeline and program list, the 8-bit instance visibility mask allocation | 0002 |
| Path state layout and its byte budget (complete table, every field any record needs), shadow item layout, wavelength packet operations and their names, termination and flag enums, the sampler and its dimension map | 0003 |
| Transport algorithms, MIS form, light sampling, sky, light-tracing pass behaviour | 0004 |
| Material record, VM, extension kinds, node mapping | 0005 |
| Medium interface, grid format, spacetime implementation | 0006 |
| ABI function list for scene edits and render control, sync, threading, presentation, VRAM budget and reserve rule | 0007 |
| Film, sample accumulation and resolve, passes, denoiser interface | 0008 |
| Repository layout, toolchain, every time budget, test tiers, process | 0009 |

## Technical rulings

1. **Accumulation.** The path state carries no accumulated radiance. During a pass, contributions (including shadow-ray results that arrive after their path has ended) are added to a per-sample buffer in fp32; atomics are allowed there. At the end of the pass it is resolved into the film's fp64 moments with one writer per pixel and no atomics. 0008 owns the buffer and the resolve; 0002's `film_add` is renamed accordingly.
2. **Light-tracing splats** use the same normalised pixel filter footprint as camera samples (0008), not single-pixel deposits.
3. **Sampler.** 0003 owns it: definition, dimension map, and an "independent" mode for denoisers that need decorrelated frames. No seed value is special.
4. **Hero lane.** A random hero lane per path (0003 decision 6, with its bias counterexample) is binding everywhere, including media and geodesics.
5. **MIS.** The balance heuristic in the r_u / r_l form (0004) is used everywhere, including volumes.
6. **Names.** 0003's names win for packet operations and enums (`apply_frequency_shift`, `rewrite_wavelength`, `MAX_BOUNCE`, `GEODESIC_NUMERICAL`). 0002's names win for kernels and stages (`shade_light_path`, `generate_light`, `ShadeKind`). Cross-references use decision ids as they stand after this pass.
7. **Light-path wavelengths** are drawn from the same sensor wavelength table as camera paths and recomputed from the seed (0 bytes of state). Sampling proportional to an emitter's spectrum is not designed in; narrow-line emitters are a known noise case to measure.
8. **Medium stack** is 20 B per path (0006); the photon medium slot matches it. 0003 updates the budget.
9. **Path state** must total at most 256 B and shadow items at most 128 B with every field any record requires. If the sum does not fit, 0003 lists what is cut or moved to a side buffer and says which record loses the field.
10. **C ABI.** 0001's model stands: header `astroray.h`, handles `ar_context` / `ar_scene` / `ar_session`, every function returns `ar_status`, version is the header hash with exact match, no cross-version promise.
11. **Volume grids** cross the ABI as sparse 8³ leaves (0006).
12. **OIDN.** Our own build with renamed DLLs (0001), at one pinned version, produced once as a release asset and not part of the normal build.
13. **Kernel modules** are embedded in the release DLL; loose files only in development.
14. **Launch duration.** Target 20 ms per kernel launch for viewport responsiveness; hard cap 100 ms.
15. **CPU target** requires AVX2.
16. **One GPU compiler.** nvcc only; NVRTC is dropped (measured gain about 1 s per edit, not worth a second compile path). Revisit only if the measured edit loop exceeds 5 s.
17. **Register gate.** 0002's challenge is accepted and the spine is amended: the merge gate checks spills and stack in the shipping capped build against recorded allowances; the uncapped register count is a warning from `check full`.
18. **Geodesics.** `advance_geodesic` is a CUDA kernel that emits ray segments; segments go through the ordinary intersect kernel. No material, medium or VM code runs inside OptiX programs. Spine section 2 stands; 0006 is revised.
19. **VRAM.** One reserve rule and one budget table, in 0007.
20. **Texture decoding libraries** must be on 0001's dependency list or not used.
21. **First-frame gate.** At most 1.5x Cycles end to end as the gate, 1.0x as the goal, provisional until the owner answers; stated once in 0007.
22. **Mipmaps.** Texture filtering with mipmaps is part of the design; when it lands is the roadmap's business.
23. **Spine amendments accepted:** a marcher for media with no scattering and linear Mix for volumes (0006); a fixed upsampling convention per slot and the Add Shader policy (0005); the register gate (0002).

# Second round (2026-11-14, after the first reconciliation and the owner's checkpoint-2 answers)

The first pass left specific disagreements. These rulings close them. One editor applies them across all nine records.

24. **Lane after dispersion or a wavelength rewrite.** The surviving lane is lane 0, as 0003 defines. 0004 and 0005 use that wording; no record says "the hero lane continues".
25. **Chord length.** `ray_tmax` in the main pool is the only place the intersect stage reads a segment length; the geodesic record does not duplicate it.
26. **Event index** is stored explicitly (2 B) in the path state, not derived.
27. **Random-walk subsurface.** 0005 owns the model and the record's fields; 0002 lists its kernels; 0003 allocates the side buffer.
28. **Interior medium of a surface.** 0005 adds `interior_ior` and `medium_priority` to the material record and lowers OpenPBR `transmission_depth` / `transmission_scatter` to the coefficient-level medium 0006 defines.
29. **Every time budget is a row in 0009** with an id. This includes the rows 0002, 0004, 0005, 0007 and 0008 reported as moved out (context start-up, idle and scheduling shares, kernel time per sample, clean and incremental build, compile per file, launch duration 20 ms target and 100 ms cap, reset latency, light-structure build, statistics overhead, material compile and translation, VM overhead, main-thread call time, view_draw time, sync times, cancel and new-view latency, denoise, DLSS and resolve shares). Other records cite the id and contain no time numbers.
30. **Viewport rate (owner decisions 21-22).** The target is 30 updates per second at full 1080p, 24 the floor, 60 a later goal. The viewport shows single samples while the view is changing or has just stopped, then increases the number of samples per displayed update. 0007 owns the batching rule; 0009 owns the numbers.
31. **Toolchain pins** are stated only in 0009; 0001 cites them.
32. **Tools run on Blender's bundled Python and NumPy.** No separate virtual environment.
33. **C ABI handles.** `ar_edit` is a fourth handle type and `ar_key` (64-bit) an allowed typedef; 0001's grammar gains an output-array annotation. Device selection is on `ar_context`. 0007 owns the render-control, scene-edit and film/pass/statistics readback function lists and gives every descriptor a field list (an appendix file `0007-appendix-abi.md` is allowed); 0008 owns what the readback returns.
34. **Dependencies.** MikkTSpace, stb_image, tinyexr (reading and writing) and the NGX headers (fetched at configure time, never committed) are added to 0001's closed list.
35. **Unit anchor** is 0003's; 0004 cites it.
36. **Later extension kinds** (measured BSDF, multilayer) keep an enum value and one line in 0005; their design stays in `research/17`.
37. **Names.** The per-sample key is `sample_id`. "Generation" means a version of scene state; "epoch" means an accumulation reset counter; 0007 defines both and all records use them that way.
38. **Command-line host** subcommands: info, render, bench, dump.
39. **Include rule.** CUDA and OptiX headers may be included in `src/device` and `src/kernel_gpu`; the DLSS backend lives in `src/denoise/dlss`.
40. **Report codes.** 0001's table gains a code for every condition another record raises.
41. **Double precision.** Geodesic integration uses explicit double on the CPU in every build. The full double-precision CPU build is a test reference and is not shipped.
42. **Path state additions.** A front-face bit and a "first bounce was specular" bit; shadow-catcher flag bits; 0008's flag allocation shrinks to what it uses. The VRAM table in 0007 includes the side buffers.
43. **Grid descriptor** carries an object-to-world matrix. Instance mask: bit 6 is the medium boundary, bit 7 spare, as 0002 states.
44. **Fluorescent receivers and the light pass.** The light-tracing pass never connects at a surface that rewrites wavelengths; camera paths own those contributions. No prefolded-weight flag and no extra shadow-item field.
45. **Light and shadow linking** is not designed in. Its fields and hooks are removed; the roadmap lists it as an open item.
46. **Geodesic stepping.** `advance_geodesic` takes as many steps per launch as it can while the current chord cannot touch scene geometry (tested against the scene bounds), marching media in-kernel, and emits a segment for the ordinary intersect stage only when a chord overlaps the bounds. Every curved-ray result, hit or miss, returns to it.
47. **Kernel list.** 0002 enumerates the `Stage` enum and names every kernel other records need, including the film, denoiser-preparation and heat-map kernels, `intersect_subsurface`, `shade_light_path_ext`, `bake_environment`, a hair shade kernel if split, and the motion pipeline variant.
48. **Owner decisions 24-28** are reflected: fluorescence, hair, DLSS with the other denoisers and scientific outputs are first-release features; point lamps and spotlights cast caustics through the light-tracing pass; Ambient Occlusion and Bevel are reported no-ops; thin film is physically computed.
49. **Stale text.** Risks, open items and cross-references that describe a superseded state are corrected. "Review and resolution" sections stay as history, each headed by one line saying later reconciliation supersedes them.

# Third round (2026-11-14): closing reconciliation

Two verification rounds applied rulings 1-49 almost everywhere; the layouts and budgets check out (path state 157 B unconditional, 225 B maximum; shadow item 124 B). What remains is listed here. No further reconciliation pass is run: the records are corrected when a task next touches them.

50. **Precedence.** Owner decisions, then these rulings, then the spine, then the records. Where a record conflicts with a later ruling, the ruling wins and the task that touches that part of the record fixes the text.
51. **Ruling 29 covers budgets** (limits that a check enforces), not measurements quoted as evidence. Measured times may stay where they explain a decision. B-37 is split into one id per constant when 0009 is next edited.
52. **Cancellation** is checked between kernel launches. "One step" wording is superseded.
53. **Oracle and science-format checks** (astropy, Mitsuba 3, pbrt-v4) use a separate tools environment under `third_party/_cache`, used only by `check full`. Ruling 32 applies to the build and the merge gate.
54. **tinyexr is for reading only.** The engine writes no EXR.
55. **SDK headers** (OptiX, NGX) are fetched by `tools/fetch.ps1 deps`, never at configure time.
56. **OIDN-CUDA and DLSS glue** live under `src/device/`, where CUDA types are allowed.
57. **Presentation** is RGBA16F for images; heat maps are the single 8-bit exception.
58. **The AO render pass is dropped for 2.0**, consistent with the AO node being a reported no-op.
59. **`dump`** in the command-line host covers film and science output only; there is no scene-state read-back.
60. **Sampler mode and frame number** are fields of `ar_settings`; the engine hashes the frame into the seed.
61. **Node mapping rows** are validated against the OpenPBR definition and analytic expectations. Cycles renders are a qualitative look check and never a shipping condition.
62. **Tool and script names:** 0009's repository layout is authoritative.
63. **Designed when built.** These have no design yet. Each is decided in the task that builds it, from a published source, and written back into the owning record in the same change:
    - ray-origin offset, tmin, self-intersection and shadow-terminator handling (0002);
    - shadow catcher and holdout, and the alpha path (0008);
    - camera and lens models, including panoramic types (0004);
    - ownership and compaction of the active-pixel list (0008);
    - which kernel shades the subsurface exit vertex (0002, 0005);
    - medium-stack handling on light paths (0006);
    - texture footprint on light paths (0005);
    - array count fields and light descriptor fields in the ABI appendix (0007);
    - report codes still missing (0001);
    - version pins, including Embree (0009).

## Build-phase rulings

Rulings 64-90 are folded into the record sections they amend (owner decision 40, task 185); each keeps its number here as an index line naming that section. Ruling 91 and later stand here in full.

64. **Spike F part A, criterion (d).** Folded into 0008 V12 part A and 0007 d14's DLSS row (owner decision 40).

65. **Fault dumps out of process (0001 D2.5).** Folded into 0001 D2.5 (owner decision 40).

66. **Instance identity (0007 d18).** Folded into 0007 d18 (owner decision 40).

67. **Degenerate faces (0007 d9, V2).** Folded into 0007 d9 (owner decision 40).

68. **Representative motion step.** Folded into 0007 d15 (owner decision 40).

69. **Generation numbers of latched views (0007 d9, d25).** Folded into 0007 d25 and d12 (owner decision 40).

70. **Device uploads and their failure (0007 d9, d11-d14).** Folded into 0007 d9 and d14 (owner decision 40).

71. **CLI overrides and bench fields (0001 D2.10, 0009 d22).** Folded into 0001 D2.10 and 0009 d22 (owner decision 40).

72. **B-05 for shade_surface (owner decision, 0009 B-05).** Folded into 0009 B-05 (owner decision 40), at the 7.0 s for M1b that decision sets.

73. **The light alias test's uniform (0003 d23, 0004 D10).** Folded into 0003 d23 (owner decision 40).

74. **Requested passes the engine does not produce (0001 D2.11, 0008 d10).** Folded into 0008 d10, 0001 D2.11 and 0007 d5 (owner decision 40).

75. **Pass indices across a film rebuild (0007 decision 5, ruling 74).** Folded into 0007 d5 (owner decision 40).

76. **Gate family sizes measured (0009 d15, Verification 14-15).** Folded into 0009 d15 and Verification 14-15 (owner decision 40).

77. **NEE test modes (0004 D1 and D8 Verification rows).** Folded into 0004 Verification D1 and D8 (owner decision 40).

78. **JPEG decode parity (0007 d21, task 110).** Folded into 0007 d21 and V19 (owner decision 40).

79. **Order of the wavefront remedies (0002 D7, task 093).** Folded into 0002 D7 and 0009 B-20 (owner decision 40).

80. **Remedy (a) as built, and when the material sort runs (0002 D7, tasks 120, 122).** Folded into 0002 D7 (owner decision 40).

81. **The first context create and B-21 (0001 D2.8, 0009 B-21, tasks 094, 124).** Folded into 0001 D2.8 and 0009 B-21 (owner decision 40).

82. **V12's frame-per-pass rule and the B-34 publish cap (0007 V12, d50, 0009 B-34, task 130).** Folded into 0007 V12 and d50, and 0009 B-34 (owner decision 40).

83. **V7a measured in the process (owner decision 35, 0007 d35, task 127).** Folded into 0007 d35 (owner decision 40).

84. **The shadow pool's partition range (0002 D5, D6, D7; task 133).** Folded into 0002 D5, D6 and D7, as amended by ruling 85 (owner decision 40).

85. **Partition marks that follow live slots (0002 D5, D6, D7; amends ruling 84; task 134).** Folded into 0002 D5, D6 and D7 (owner decision 40).

86. **Review tails may move (amends ruling 49).** Folded into 0009 d29 (owner decision 40).

87. **Write-backs beyond ruling 63's list.** Folded into 0009 d29 (owner decision 40).

88. **The pdf-consistency row on a piecewise-constant table (0004 Verification D7/D11).** Folded into 0004 Verification D7/D11 (owner decision 40).

89. **M-1 probe spills belong to the stand-in lobes (task 144).** Folded into 0002 D10 (owner decision 40).

90. **Light-tree node energy is per-type on-axis intensity (0004 D10).** Folded into 0004 D10 (owner decision 40).

91. **The spine queue, its stop trigger and statistical cases for M1b's second batch (PLAN-M1b "The spine").** The edits to `k_shade_surface` run one at a time in the order 158, 163, 173, 174, 175 (173 changes the stage through the light dispatch without editing `shade_surface.h`); false interfaces and extension routing follow in the next batch. The row stands at the 128-register cap that `__launch_bounds__` enforces, so the plan's "registers at 128" can no longer trigger: a spine landing stops for 0002 D10's ladder when spills appear or the stack passes 512 B, and compile time past 7.0 s (ruling 72 as amended by owner decision 40) stays an owner item. Each new statistical case runs its CPU and CUDA parts in one concurrent group (task 157's method) and the PR states the measured gate seconds. A new budget row (`k_bake_environment`, `k_shade_volume`) takes its first values from its task's first measurement, stated in the PR.
92. **The spine queue after 175, and the coating's complex IOR in M1b (extends ruling 91).** Ratio tracking's shadow attenuation (task 192) needs the NEE emit in `shade_surface.h` to write the shadow item's MEDIA fields (`medium_stack`, `medium_rng`, `r_u`, `r_l`), so it joins the `k_shade_surface` queue: 158, 163, 173, 174, 175, then 192, then false interfaces, then extension routing, one at a time, with ruling 91's stop trigger (spills, or stack past 512 B) and owner decision 40's 7.0 s compile limit. 192 and false interfaces are the two media edits of the stage and land consecutively so its media cost is measured together. In M1b a grating coating's complex IOR (0005 item 24) is one wavelength-independent constant (n, k) pair through `fresnel_conductor`, PROVISIONAL: SPINE section 5 already calls complex IOR a per-material constant pair, and a spectrum table carries pointers that the 64 B `ext_constants` block cannot hold; a TABLE form follows item 12's precedent later with its own descriptor field.
93. **A small spill allowance on `k_shade_surface` for M1b (amends ruling 91's stop trigger; task 148).** Mix Shader selection must carry the event's rescaled lobe-choice uniform out of the surface entry. 148 measured three carriers on the 128-register row: a VM register read back after the entry (stack 384 to 688 B: the read-back stopped the material record's scalar replacement, +304 B = sizeof(OpenPBRParams)); a scalar in the VM state behind a pointer (stack 400 B, spills 4 / 4 B); the record unchanged in every case. 0002 D10 allows a VM kernel 128 / 192 B of spills (ruling 89); ruling 91's "spills appear" trigger was the lead's stricter line for the spine. For M1b `k_shade_surface` may therefore hold up to 16 / 16 B of spills (old 0 / 0); registers stay 128, the stack stays at most 512 B, compile time within owner decision 40's 7.0 s, and every spine landing still passes `check perf` at B-38's 1.03. Spills past 16 / 16 B, or a `check perf` failure, stop the spine for D10's ladder as ruling 91 says. The budget row records the measured values.
94. **B-09 counts the gate's work, not its rebuild (amends the 0009 B-09 row's "incl. incremental build"; owner decision 36's B-08 precedent; PROVISIONAL, for the owner).** A landing rebases, runs the canary (which builds broken trees) and then rebuilds before its gate, so every landing gate now pays 16-31 s of compilation the branch's own gate did not: 164's fourth try read 183.3 s on a quiet machine with every implementer paused, of which `build dev` 16.2 s and the fast step's build 14.6 s (152.5 s without them), while main's quiet gate without a rebuild is 145.5 s (6f68a28 ledger). The tier clock of `check gate` therefore excludes its build steps (`build dev`, the nofma build and the build inside the fast step) as B-08 already does for `check fast`; the limit stays 180 s, the hold line 170 s (owner decision 38) and the kernel part's 120 s (which keeps counting device builds, 0002 D10). Old: B-09 = gate wall time including the incremental build. New: B-09 = gate wall time excluding build steps, the build seconds printed beside the tier total. Task 204 implements it in `tools/check.ps1` and writes the row.
95. **`k_route_hit` holds 40 registers (task 182; a lead ruling on a 0002 D10 row).** Medium routing needs the back-face test of a medium-bearing instance inside `route_hit`: the triangle's three vertices and the instance transform behind the instance's medium index, plus the ray direction the three callers now pass. 182 measured 28 to 40 registers, stack 0, spills 0 / 0. `k_route_hit` is a small non-VM kernel far below the 128-register cap, so the row moves. Old: regs 28. New: regs 40; stack, spills and compile_s unchanged.
96. **CPU/CUDA agreement through the RGB-to-spectrum lift is gated by its parts (0002 D12; task 172).** A saturated colour environment reads CPU vs CUDA radiance max 1.36e-5 (p99.9 9.0e-6) against D12's evaluation limit 1e-5, while TEX_ENVIRONMENT's pre-lift RGB agrees to 4.4e-7 and the lift agrees bit for bit on identical inputs (test_vm_agree). 172 traced the gap to the rgb2spec sigmoid argument c0*l^2 + c1*l + c2 cancelling about 29x (terms -32.3, +49.7, -19.1 summing to -1.74), so a one-ulp libm difference in the texel position becomes about 2e-5 in the argument: conditioning of the existing lift, not a device difference in the op. For a chain that ends in the lift, D12 is met by (a) the pre-lift colour agreement at 1e-6, (b) the lift's bit-identity on identical inputs and (c) a grey (analytic-lift) radiance case at 1e-6, all gated; the colour radiance through the lift is printed, ungated, so a jump stays visible. No threshold changes: D12's 1e-5 stands for every other evaluation.
