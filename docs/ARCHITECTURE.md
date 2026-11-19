# Astroray 2.0 architecture

Read this first. It is a map, not a rulebook: every decision, number and layout below belongs to a record in `docs/decisions/`, cited as `0003 d19` (decision 19 of record 0003), `0002 D8`, `R17` (ruling 17 in `RULINGS.md`) or `O25` (owner decision 25). When this page and a source disagree, the source wins. Precedence among sources (R50): `0000-owner-decisions.md` > `RULINGS.md` > `SPINE.md` > records 0001-0009. Where a record contradicts a later ruling, the ruling holds and the task that next touches that part of the record fixes its text. Milestones live only in `docs/ROADMAP.md` (G1); every time budget is a row `B-nn` in 0009 d13 (R29); kernel resource limits are 0002 D10's; the first-frame gate is 0007 d35's (R21).

## 1. What Astroray 2.0 is, and is not

**Is.** A spectral, physically based path tracer for stock Blender 5.3 (designed for 5.4), aimed at scientifically accurate and astrophysical rendering (O3, O8). Correctness means physics, analytic results and the OpenPBR Surface specification (O1, 0005 d1); Cycles is a qualitative look check only (R61). One kernel source runs on the CPU and the NVIDIA GPU (O2). The first release is complete in breadth: surfaces, volumes, sky and world, textures, lights, diffraction gratings with the light-tracing pass that makes them useful, fluorescence, hair, OIDN and DLSS, scientific outputs (O7, O17, O24). The first astrophysics target is a black hole inside an emission nebula (O8). The viewport targets are owner decisions O9, O10, O21-O23, with numbers in 0009 B-14 and 0007 d35. Licences: engine Apache-2.0, addon GPL-3.0-or-later (O5, 0001 D7).

**Is not** (0001 D11): a Blender fork or a USD/Hydra delegate; anything other than Windows x64 and NVIDIA (the CPU target is the correctness oracle and can also render); multi-GPU; an out-of-process daemon (a gated fallback only, 0001 D6); a stable embedding ABI or a version-independent scene format; a pixel-parity clone of Cycles. Light and shadow linking (R45), polarisation (0003 d31, 0005 d13), ReSTIR and path guiding (0004 D26), PSF and detector noise (0008 d14) are not designed. Ambient Occlusion and Bevel nodes are reported no-ops and the AO pass is dropped (O26, R58). Caustics from delta lights seen through a delta chain or bouncing again may be missing, with a warning (O25, 0004 D24).

## 2. Shape: deliverables and the boundary

```
 Blender 5.3 process
 +------------------------------------------------------------------------+
 | Blender: depsgraph, RenderEngine API, gpu module, view transform        |
 |      | bpy reads (main thread; F12 job thread)              ^ draw       |
 | +----v--------------------------------------------------------+-------+ |
 | | astroray extension (pure Python, GPL-3.0-or-later)                   | |
 | |  sync/ (depsgraph diff)  node -> op translation  present  UI, report | |
 | |  _abi.py (generated from astroray.h, never edited)                   | |
 | +----+--------------------------------------------------------^-------+ |
 |      | ctypes; C ABI astroray.h: POD structs, annotated arrays,          |
 |      | ar_status everywhere, no callbacks (everything is polled)         |
 | +----v--------------------------------------------------------+-------+ |
 | | astroray_core.dll (C++17, Apache-2.0; release kernels embedded)      | |
 | |  capi + recorder | host: scene, material compiler, scheduler, film,  | |
 | |                  |       VRAM ledger, report   | denoise: OIDN, DLSS | |
 | |  ------------------------- Device seam ------------------------------| |
 | |  CpuDevice: kernels (MSVC, AVX2) + Embree | CudaDevice: CUDA + OptiX | |
 | +---------------------------------------------------------------------+ |
 +------------------------------------------------------------------------+
   beside the DLL: astroray_oidn*.dll (our renamed build)
   from the driver: nvcuda, nvoptix, DLSS          (nothing NVIDIA ships)
   astroray_cli.exe (Apache-2.0) --- same C ABI ---> astroray_core.dll
       ^ .arrec ABI recordings, from the addon or scenes/*.py builders
```

Deliverables (0001 D1): the engine DLL, the only binary Blender loads, which knows nothing about Blender or Python; the public header `include/astroray/astroray.h`; the Blender extension; the command-line host `astroray_cli` (`info`, `render`, `bench`, `dump`; R38, R59), which is the primary development and test host. The boundary is the C ABI: handles `ar_context`, `ar_scene`, `ar_session`, `ar_edit`, the `ar_key` typedef, a restricted header grammar checked by a generator, and an exact header-hash match with no cross-version promise (0001 D2, R10, R33). Everything Blender-specific (node mapping, depsgraph diffing, UI) is on the Python side; the core receives host-neutral data. The only scene format is an ABI recording (0001 D1.4). Function and descriptor lists: 0007 d5 and `0007-appendix-abi.md`; what readback returns: 0008 d13a. Dependencies are a closed list (0001 D9, amended by R20, R34, R54, R55). Distribution: GitHub releases and a self-hosted extension index (0001 D10).

## 3. One viewport frame, from a Blender edit to pixels

1. **Edit arrives** (Blender main thread, `view_update`). The addon deduplicates `depsgraph.updates` by (ID type, original `session_uid`) and applies one action per ID type in a fixed order (0007 d20). It opens `ar_scene_edit_begin`, then per changed datablock: geometry through staged arrays that `foreach_get` fills in place (`ar_edit_mesh_begin`, or `positions_begin` when topology provably cannot have changed, 0007 d17, d22); materials as op graphs translated from `inline_shader_nodes()` (`ar_edit_set_graph`, 0005 item 29, 0007 d8); lights, world, images, instances, settings by their setters. `ar_edit_item_commit` hands each item to the engine's workers at once; `ar_edit_commit` does constant-time checks and returns the **generation** the edit will produce (0007 d9).
2. **Scene workers** (engine threads, 0007 d23-d24) validate, triangulate natively, decode images and build mips, compile material programs with the core compiler (0005 items 29-31), build per-geometry GAS and the TLAS (0007 d11), allocate through the VRAM ledger (0007 d14) and upload. Light trees, alias tables, sky and grating tables and majorant grids are built on the host; the environment table by `bake_environment` (0002 D8, 0004 D10-D12). A failed validation drops the whole generation and keeps the old image (0007 d9).
3. **View change.** Orbit and pan reach the addon only as `view_draw` calls; it hashes the view and calls `ar_session_set_view`, which records pending state (0007 d49).
4. **Latch** (session render thread). At a pass boundary, after the previous pass is resolved, all pending view changes and finished commits become one new generation. If it changes the image it starts a new **epoch**: film, light image, adaptive state and statistics reset (0007 d25, 0004 D25). An epoch-starting change interrupts the pass in flight at the next launch boundary only if the current generation has already presented a frame (0007 d25, R52).
5. **The pass** (0002 D5-D8; 0004 D25). The host runs steps of one loop shared by both targets. Each step: `queue_build_count`, `queue_build_scan`, `queue_build_scatter` partition both pools by `next_stage` and fill the step table; the host reads the step table; `sort_count`, `sort_scan`, `sort_scatter` sort the `shade_surface` segment by material when worthwhile; refill launches `generate_camera` (active-pixel list, `sample_id`, filter-importance-sampled offset, film wavelengths from the wavelength table, camera ray; 0008 d3, d5, 0003 d2) and, when the scene has caustic casters, `generate_light` (0004 D20); then every non-empty stage once, in `Stage` order: `intersect_closest` (OptiX raygen; `route_hit` picks the next stage), `intersect_subsurface`, `intersect_shadow`, `shade_background`, `shade_light`, `shade_surface` (VM fills the OpenPBR record, alpha pass-through, false-interface stack update, emission with MIS, NEE emitting a shadow item, BSDF sample, Russian roulette, first-hit denoiser guides), `shade_surface_ext`, `shade_light_path`, `shade_light_path_ext`, `shade_shadow`, `shade_volume`, `shade_subsurface`, `advance_geodesic`. Contributions go to the per-sample buffer through `sample_add`; light-path camera connections are splatted to the `SPLAT` plane over the pixel-filter footprint (R1, R2). The pass ends when its work counter is exhausted and only the two `TERMINATED` segments are non-empty.
6. **Resolve.** `film_resolve`, one thread per active pixel, applies `film_weights` and the radiance-scale removal, converts each sample to every response channel and adds it to the fp64 moments with no atomics; it folds the splat plane and clears both buffers (0008 d3-d4, d7). `film_adaptive` runs on adaptive check passes (0004 D16).
7. **Denoise and present.** When the denoiser state is ready, `film_prepare_denoise` feeds OIDN on the accumulated mean (or `film_pack_dlss` feeds DLSS-RR in DLSS mode, with a per-frame colour reset and the independent sampler mode), producing a `DisplayImage` (0008 d15-d19). `film_present` converts to scene-linear working-space RGBA16F (a heat-map mode uses `film_heatmap` instead, R57); an async copy places it in a pinned host ring, tagged with its generation (0007 d29). During interaction every displayed update is one pass; afterwards several passes run per update, growing as the image settles (0007 d50, O22).
8. **Draw** (main thread, `view_draw`). `ar_display_acquire` returns the newest frame; the addon updates persistent storage-buffer stripes, unpacks them with a compute shader into one persistent texture and draws it inside Blender's display-space shader, so Blender's view transform applies (0007 d29-d32); then `ar_display_release` and `tag_redraw`. The addon polls progress and the per-render report, reading entries only when the report revision changes (0001 D2.11). The dynamic-resolution controller lowers render scale only when the update rate falls below the floor (0007 d34, O10).

## 4. One final-render sample

F12 creates its own `ar_scene` and `ar_session` (mode FINAL); nothing is shared with viewports, which pause (0007 d28). If VRAM is short the addon trims paused viewport scenes, then textures and grids may spill, otherwise the render fails loudly (0007 d14). Per view layer, `update()` on Blender's job thread starts with `ar_edit_reset` and syncs everything with full reads; motion blur samples transforms through `frame_set` and selects the motion pipeline variant (0007 d15, d48, 0002 D11). `render()` polls `ar_session_wait` and progress, posts previews with `update_result` and maps `test_break()` to `ar_session_cancel`, which stops at the next launch boundary (0007 d27, R52).

The sample itself: its slot is assigned by `generate_camera` from the pass's active-pixel list (0008 d3); its random numbers come from the counter-based sampler keyed on pixel, sample index, dimension and seed (0003 d23); its film wavelengths are recomputed from that key wherever needed and never stored (0003 d2). The path walks the stages of section 3. At each non-delta vertex it takes one light sample and one BSDF or phase sample, combined by the balance heuristic in r_u/r_l form (0004 D1, R5). Wavelength-dependent delta events terminate secondary lanes and lane 0 continues (0003 d5, R24); fluorescence rewrites the transport wavelengths (0005 item 26). Shadow items delivered after the path has ended still land in its slot (0004 D5). Adaptive sampling stops converged pixels with a colour-aware metric and the tail rule (0004 D16); the light pass runs in each pass when casters exist (0004 D18-D25). At the end, final-quality OIDN denoises Combined into a `DisplayImage`; `ar_session_pass_count`, `ar_session_pass_info` and `ar_session_read_pass` copy Combined, `Noisy Image`, AOVs, Cryptomatte and statistics into Blender's `RenderResult`, which Blender displays and writes as PNG or EXR; band, cube and photon channels go undenoised to the engine's FITS writer through `ar_session_write_fits` (0008 d13, d13a, d17).

## 5. Main data structures

**Scene representation** (0007 C, d10-d15, d18). Host-assigned `ar_key`s identify datablocks; a geometry key follows Cycles' real-object-data rule (0007 d18). One record per geometry: triangle meshes with corner-indexed attributes, curves, points, sparse grids that cross the ABI as leaves and become NanoVDB in the engine (R11, 0006 d5). One compacted GAS per geometry key and one TLAS, always rebuilt, never refit (0007 d11); Embree mirrors the structure on the CPU. Instances carry transforms, motion steps, visibility bits mapped onto the instance mask (0002 D11), flags and a material table. Textures are stored at source precision with mip chains (0007 d13, 0005 item 34). Every change produces a generation; edits are atomic per generation (0007 d9, d25).

**Path state** (0003 d19-d22; storage mechanism 0002 D13). Structure of arrays, one array per field, declared once in an X-macro with feature tags so fields of disabled features are not allocated. It holds no accumulated radiance (R1). Camera and light paths share it, told apart by a flag bit. The table in 0003 d19 is complete: every field any record needs, its condition and its consumer; the flag word and termination enum are 0003 d21-d22. **Shadow items** are self-contained visibility work (0003 d20, 0002 D5): NEE items deliver to a per-sample slot, camera connections to a film position. Bulky per-feature state lives in side buffers indexed by slot (0003 d20). Changing any of these layouts amends 0003.

**Material record and VM** (0005). Each material has a small header, a constant block with every OpenPBR input at its authored value, and per shading point a resolved `OpenPBRParams` record in thread-local memory (0005 item 2). Colour and IOR slots hold `SpectrumRef`s whose lift convention is fixed by the slot (0005 items 9-10). A Blender node graph compiles to bytecode for one kernel-owned register file; control flow is forward only; Mix and Add Shader select stochastically on surfaces; programs have entries for surface, opacity, emission and volume, and lights and worlds compile to their own programs (0005 items 30-32, 38). One opcode table `ops.def` generates the enum, interpreter, compiler metadata, the addon's Python op schema and the node coverage matrix (0005 item 30). The compiler also emits the flags other stages read: `MAY_BE_DELTA`, `NON_OPAQUE`, `EXTENSION`, `READS_RAY_TYPE`, the sky split and the emission estimate (0005 item 43).

**Medium description** (0006 A-C). Every medium is a VM volume program filling a coefficient-level `MediumSample` (absorption, scattering, emission as j, phase lobes, optional velocity) at the path's wavelengths (0006 d1-d2). `MediumDesc` holds no material parameters (0006 d3). Majorant grids come from interval evaluation of the program, which is why every volume-reachable opcode needs an interval rule (0006 d6-d7). Scattering media use null-scattering tracking with hero-lane spectral MIS; non-scattering media use the deterministic emission-absorption marcher shared with `advance_geodesic` (0006 d8-d9, R23). One per-path medium stack serves media and nested dielectrics with priorities; a false interface only updates the stack (0006 d16-d17, R8).

**Film** (0008). A `FilmLayout` lists channels with a kind (`MOMENT`, `ADD`, `COUNT`, `CRYPTO`, `SPLAT`); unrequested channels cost nothing; kernels write only through `film_*` functions in `film_write.h` (0008 d1-d2). Camera samples go to the per-sample buffer and are resolved into fp64 first and second moments (plus a class-A sum for adaptive sampling) with one writer per pixel (0008 d3). Display, bands, instruments and cubes are all response channels (0008 d7). Planes are reachable only through read-only `FilmView`; denoisers write only `DisplayImage`, so science data cannot be denoised by construction (0008 d17).

## 6. One source for CPU and GPU

Kernel code is restricted C++17 in freestanding headers under `src/kernel/` (0002 D1). `src/kernel/portability.h` is the only file that knows its target: function qualifiers, read-only loads, atomics, clocks, warp helpers and math wrappers (0002 D2). Each stage is one function `stage_<name>(const KernelGlobals&, item, const StepEntry&)` in `src/kernel/stages/`; `AR_GPU_STAGE` wraps it as a CUDA kernel in `src/kernel_gpu/<stage>.cu`, `AR_CPU_STAGE` as a chunked loop on the host thread pool in `src/kernel_cpu/<stage>.cpp`; both read their segment from the same step table, which the same code computes (0002 D4, D6). The scheduler loop is host C++ written once against a two-method device interface (0002 D7). `Real` is float on the GPU and in the default CPU build, double in an unshipped CPU reference build; CPU geodesic integration is always double (0002 D3, R41). Random numbers use integer arithmetic only, so every build draws identical values (0003 d23). Traversal is the one algorithmic difference: OptiX on the GPU, Embree on the CPU, sharing the any-hit recording function (0002 D11). nvcc is the only GPU compiler; modules load through the driver API and are embedded in release DLLs (0002 D14, R13, R16). The vendor seam is the host `Device` interface with `CpuDevice` and `CudaDevice` (0001 D5.2). Agreement is measured, not assumed: a no-FMA GPU build and a ladder of bitwise, per-stage, AOV and image tests (0002 D12, 0009 d17).

## 7. How things plug in

- **Shader nodes.** A new Blender node is a lowering in the addon's table plus a test; a new operation is one `ops.def` line, one kernel-library function and one test (0005 items 29-30). Anything unsupported renders magenta and raises a report entry; the coverage matrix has a row for every `ShaderNode` type (0005 items 40-41).
- **Spectra.** Spectrum sources are a closed `SpectrumRef` kind list; new spectra are data composed on the host into tables (0005 item 9). A new band or instrument is a response curve (0008 d7).
- **Extension kinds.** A material carries at most one extension behaviour from the closed list in 0005 item 14 (O18, R36); it replaces the whole OpenPBR lobe stack (0005 item 15). Kinds implement sample, eval and pdf with `TransportMode` and declare the record fields they read in `ext_fields.def` (0005 items 16-17). Their code lives only in `shade_surface_ext` and `shade_light_path_ext`, one named split for hair (0005 item 18, 0002 D8). A new kind needs a superseding record.
- **Lights.** Each light type implements one interface for NEE, BSDF-hit MIS, light-path emission, power and tree bounds (0004 D7); the light tree, alias table and light pass then follow.
- **Media.** A new volume node or coefficient model is one VM op with an interval rule; a new phase function is one `PhaseKind` with normalisation and chi-squared tests (0006 d12, d21).
- **Spacetime.** Curved transport is a compile-time policy type instantiated only in `advance_geodesic` (0003 d28, 0006 d23); the shipping policy is Cartesian Kerr-Schild and the flat policy exists for tests. `advance_geodesic` is a CUDA kernel that steps and marches in-kernel and emits a chord for the ordinary `intersect_closest` only when it can touch scene geometry (R18, R46, 0006 d22). Ordinary stages contain no policy code; `route_hit` takes one launch-uniform branch when a curved region exists (0003 d28). One black hole per scene (0006 d25).
- **Stages.** A new stage is one `Stage` value, one header, one `.cu`, one `.cpp` and rows in `launch_config.h` and `budget.toml` (0002 Consequences). New `ShadeKind` identities are flat enum values, never template products (0002 D8).

## 8. Threading and failure model

**Threads** (0007 d23, 0001 D2.6). Blender's main thread runs `view_update`, `view_draw` and all `bpy` reads; the F12 job thread runs `update()` and `render()`. The addon has no Python threads and never calls Blender from another thread (0007 d26). Per scene the engine owns one device-init thread and a worker pool; per session, one render thread that owns its streams and launches; the CPU device runs stages on a thread pool. A handle is used by one thread at a time; sessions run concurrently; calls return promptly except named waits (0001 D2.6, 0009 B-21). Nothing crosses the ABI as a callback (0001 D2.4). The engine holds no process-global mutable state; the CUDA primary context is the one unavoidable shared thing (0001 D2.1). Viewports, F12 and previews each own their scene and session (0007 d28).

**Failure** (0001 D2.5, D2.8, D2.11; 0002 D15; 0007 d9, d14, d44). Every function returns `ar_status`; C++ exceptions never cross the boundary. A structured exception on an engine thread writes a minidump and poisons the context. A sticky CUDA error ends CUDA for the process while the CPU device keeps working. Device selection is explicit and never falls back silently. A bad edit drops its generation and keeps the previous image; an unreadable image or unsupported node renders magenta. Out of VRAM is trim, then spill, then fail with a message, never a silent downscale. Kernels never write report entries; they increment counters that the host turns into keyed, deduplicated entries with closed codes (0001 D2.11); a new condition adds a code to that table (R40). Non-finite samples are counted and fail every test (0004 D17, 0008 d6). Cancellation is checked between kernel launches (R52), and launches are sized to 0009 B-17 (R14). Every line of the per-context log reaches the OS as written (0001 D2.5).

## 9. Where things live

The layout in 0009 d1 is authoritative (R62). In short:

| Path | Contents |
|---|---|
| `include/astroray/astroray.h` | the C ABI, the only public header (0001 D1.2, D2.10) |
| `src/kernel/` | kernel headers, `stages/`, `portability.h`, `launch_config.h`, `budget.toml` (0002) |
| `src/kernel_gpu/`, `src/kernel_cpu/` | one `.cu` / one `.cpp` per stage; `optix_traverse.cu` (0002 D4, D11) |
| `src/device/cpu/`, `src/device/cuda/` | the only vendor-specific host code; OIDN-CUDA and DLSS glue (R39, R56) |
| `src/denoise/dlss/` | DLSS backend, the only place NGX headers are included (R39) |
| `src/host/` | scene, scheduler, film, denoise glue, module loader, allocator (0009 d1) |
| `src/capi/` | ABI implementation, recorder and replayer |
| `cli/`, `blender/astroray/` | command-line host; the extension (GPL, no engine code) |
| `data/` | generated tables and `MANIFEST.toml` (0001 D8, 0003 d18) |
| `scenes/`, `tests/`, `bench/`, `assets/` | scene builders and `meta.toml`; tests by tier; bench history; asset manifest |
| `tools/` | `build`, `check`, `land`, `bench`, `status`, `fetch`, `release` scripts, `lint.py`, `budgets.toml` |
| `third_party/` | vendored headers, `deps.toml`; `_cache/` holds fetched binaries and SDK headers (0009 d6, R53, R55) |
| `docs/`, `tasks/`, `out/` | records and status; open task files; git-ignored build tree, logs, evidence |

Include rule: CUDA and OptiX headers only in `src/device/` and `src/kernel_gpu/`, Embree only in `src/device/cpu/`, NGX only in `src/denoise/dlss/` (0001 D5.2, R39, R56).

## 10. Vocabulary

| Term | Meaning |
|---|---|
| generation | a version of the scene state; every commit and every latched view or settings change produces one (0007 d25, R37) |
| epoch | the accumulation reset counter; advances when a new generation changes the image; writes carry it and stale ones are dropped (0007 d25, 0004 D25) |
| pass | one sample for every active pixel (k in the adaptive tail) plus one light pass when casters exist; resolved once (0004 D25, 0008 d3) |
| step | one round of partition, step-table read, sort, refill and stage launches (0002 D7) |
| stage | one kernel that owns one queue segment and writes `next_stage` for each item; the `Stage` enum (0002 D8) |
| pool | a structure-of-arrays pool of work items: the main path pool (camera and light paths) and the shadow pool (0002 D5) |
| segment, step table | the slice of a pool partitioned to one stage; the device table of segment starts, counts and shadow-slot bases (0002 D6) |
| shadow item | a self-contained visibility query with its contribution and delivery target (0003 d20, 0002 D5) |
| camera path, light path | paths started by `generate_camera` and `generate_light`, distinguished by a flag bit (0003 d21) |
| light pass, caster, connection | the light-tracing pass for caustic paths; an object whose material may sample a delta lobe; a light-path vertex joined to the camera (0004 D18-D23) |
| lane | one of the `kLanes` transport wavelengths a path carries (0003 d1, d4) |
| film wavelengths | the wavelengths a sample was generated for, recomputed from its key, never stored; used by the film (0003 d2) |
| hero lane | a per-path random lane that drives hero-sampled decisions (0003 d6, R4) |
| lane 0 rule | after secondary termination or a wavelength rewrite, lane 0 continues (0003 d5, R24) |
| `sample_id` | a camera sample's slot in the per-sample buffer, or a light path's rank in the light work (0003 d19, R37) |
| per-sample buffer | fp32 slots that collect a pass's contributions, resolved into fp64 moments (0008 d3, R1) |
| splat | a light-path contribution spread over the pixel-filter footprint into the `SPLAT` plane (0008 d4, R2) |
| event index | the stored per-path counter that selects the sampler's per-vertex dimension block (0003 d23, R26) |
| independent mode | sampler mode with mutually independent samples, for DLSS and exact-variance tests (0003 d23) |
| `ShadeKind` | flat enum of shade-kernel identities, one TU each (0002 D8) |
| extension kind | the one optional non-OpenPBR behaviour of a material (0005 item 14) |
| VM kernel | a kernel that runs the node VM and so holds its register file; has its own stack class (0002 D10, 0005 item 32) |
| capped build, probe build | the shipping build under launch bounds, gated on spills and stack; the uncapped build whose register count is a warning (0002 D10, R17) |
| curved region, chord | the sphere around the black hole where rays follow geodesics; a straight piece of a geodesic traced as an ordinary ray (0006 d22, d25) |
| marcher | the deterministic emission-absorption integrator for non-scattering media (0006 d9) |
| majorant | an upper bound on extinction (and emission) per cell, from interval evaluation (0006 d6-d7) |
| medium stack, false interface | per-path stack of interiors with priorities; a crossing that only updates the stack (0006 d16-d17) |
| ledger, reserve rule | the tagged record of every device allocation; the single rule deciding whether an allocation may proceed (0007 d14, R19) |
| report entry | a keyed, deduplicated diagnostic with a closed code, polled per session (0001 D2.11) |
| ABI hash | hash of the normalised header; the extension and CLI refuse a mismatch (0001 D2.7) |
| recording (`.arrec`) | the input ABI calls of a session, replayable by the CLI (0001 D1.4, D2.10) |
| staged array | an engine-owned buffer returned by a `_begin` call and filled in place by the host (0001 D2.3) |
| poisoned context | a context that refuses every call after a structured exception (0001 D2.5) |
| B-nn | a time budget row in 0009 d13 |
| `fast`, `gate`, `full`, `ci` | the check tiers (0009 d10-d12) |
| task, canary | a lead-written task file under `tasks/`; the patch that must turn a new test red (0009 d26, d28) |

## 11. If you are working on X, read these decisions

| Working on | Read |
|---|---|
| the C ABI, report codes, errors, dependencies, licences | 0001 D2, D7, D9; 0007 d4-d9 and the appendix; 0008 d13a |
| a kernel or stage, scheduling, queues, OptiX | 0002 D1-D15; 0003 d19-d23; 0009 d18 |
| path state, flags, sampler, wavelengths, units, colour | 0003 (all); R3, R4, R7, R9, R24-R26 |
| transport, NEE/MIS, lights, sky, adaptive sampling | 0004 A-E, G; 0003 d6-d7, d23 |
| the light pass, gratings, caustics | 0004 F; 0005 items 21-25; 0008 d4; R2, R44 |
| materials, nodes, the VM, textures | 0005 (all); 0003 d15-d17; 0007 d8, d13, d21 |
| hair, fluorescence, subsurface | 0005 items 4a, 26, 27; 0006 d15; R27 |
| volumes, grids, the medium stack | 0006 A-D; 0005 items 2a, 39; 0007 d47 |
| the black hole, geodesics, redshift, the nebula | 0006 E-I; 0003 d26-d30; R18, R41, R46 |
| Blender sync, keys, update classification | 0007 C-E; spike B |
| viewport presentation, batching, resolution controller, first frame | 0007 F-H; 0009 B-13, B-14, B-33, B-34 |
| VRAM | 0007 d14; 0002 D13; R19 |
| film, passes, Cryptomatte, statistics, FITS | 0008 d1-d14, d23; 0003 d19-d21 |
| denoising, OIDN, DLSS | 0008 d15-d20; 0001 D9; R12, R39, R56 |
| builds, tests, budgets, landing, scenes | 0009 (all); 0002 D10, D12 |
| anything marked "designed when built" | R63, then the owning record |

## 12. Non-negotiable rules for anyone changing the engine

From `CLAUDE.md`, the records and the rulings. The checks that enforce them are in 0009.

1. **Physics is the reference.** Correct means physics, analytic results and the OpenPBR specification; references come only from analytic results or independent spectral oracles, never from self-generated golden images or Cycles pixels (O1, 0009 d16, R61).
2. **Cite before implementing.** Every non-trivial physics, sampling or numerical method comes from a published source with a licence-compatible reference, named in the code (`CLAUDE.md` rule 5). Items listed in R63 are decided in the task that builds them, from a published source, and written back into the owning record in the same change.
3. **One source.** A formula, material, sampler or lobe exists once and compiles for both targets; never hand-port, never add a second implementation or a second compile path (O2, 0002 D4, R16).
4. **Kernel dialect.** No virtuals, exceptions, recursion, heap, STL, function pointers, `__noinline__`, `-rdc`, fast-math or mutable globals in kernel code; structs over the size limit go by reference; no template variant matrices, only the scalar type and `ShadeKind` (0002 D1, D8, D14).
5. **Kernel budgets.** Spills and stack in the capped build stay within `budget.toml`; a breach follows 0002 D10's ladder in order; splits and fusions need the A/B evidence of 0002 D9. Every task touching a shade kernel declares its expected delta.
6. **OptiX is traversal only.** No material, medium, VM or spacetime-policy code in OptiX programs (R18, 0002 D11).
7. **Accumulation.** The path state carries no radiance; every contribution goes through `sample_add` or the splat plane; moments are written only by the resolve (R1, R2).
8. **Randomness and lanes.** Random numbers come only from 0003's sampler; no seed is special; surface decisions use permutation-invariant lane reductions, hero-sampled decisions use the hero lane, and lane 0 survives termination (0003 d5-d6, d23; R3, R4, R24). The balance heuristic in r_u/r_l form is the only MIS form (R5).
9. **One owner per fact.** Numbers, names and layouts are stated once in the owning record (G2, R29); changing a layout amends 0003, a kernel list 0002, a report code 0001's table, a budget 0009's table. Records change only by a superseding record.
10. **Fail loudly.** Unsupported input raises a visible error or report entry: magenta plus an entry for nodes, a dropped generation for bad edits; no silent fallback, no silent clamp, no feature that lands switched off (`CLAUDE.md` rule 7, O25-O26).
11. **No process-global mutable state** in the engine, no callbacks across the ABI, no Python threads in the addon, no Blender call off the main thread (`CLAUDE.md` rule 8, 0001 D2.4, 0007 d23, d26).
12. **The ABI is generated.** Change `astroray.h` within its grammar; `_abi.py`, layouts, recorder and replayer are generated, never edited (0001 D2.7, D2.10).
13. **Every device allocation goes through the ledger** and obeys the reserve rule (0007 d14).
14. **Science data is never denoised, clamped or exposure-scaled**; a biased control with a science output refuses the render (0008 d6, d17; 0004 D17).
15. **Dependencies and data** come only from 0001's closed list and data policy; NVIDIA headers and binaries never enter the repository (0001 D7-D9).
16. **Budgets are requirements.** A change that breaks a B-row, a kernel limit or a lint cap is not finished; budgets fail at their limit, with no warning band (`CLAUDE.md` rule 9, 0009 d13).
17. **Evidence, not claims.** "Done" means built, checked, with the output shown; every new or changed test has a canary; no skip without an open task; look at rendered images as well as numbers (`CLAUDE.md` rule 4, 0009 d15, d26, d28).
18. **Surgical, minimal changes** within the task's scope; no speculative hooks (G4, `CLAUDE.md` rules 2-3). Nothing runs on a timer (G3); rules are enforced by scripts, not hooks (0009 d31).
19. **v1 is reached only through `docs/v1-lessons/`** (`CLAUDE.md`, 0009 d30).

## 13. Open before and during the build

Not decisions, only pointers. The records are "proposed" until sign-off. Before sign-off: whether spike C must run (0002 Risk 2) and DLSS spike F part A (0008 V12). Measurements that ratify layouts and limits: the material record and stack split (0005 M-1), float geodesics (0006 S1), wavelength sampling (spike E, 0003 Risk 1), grid ingestion (0006 spike G, 0007 P6), the first-frame gate on real sync (0007 d35). Every record's "Risks and open items" and owner questions remain the list of what is unsettled; R63 lists what is designed when built.
