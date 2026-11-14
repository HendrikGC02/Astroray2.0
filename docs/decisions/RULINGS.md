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
