# GPU architecture, wavefront design, traversal, performance attribution, build and test speed

Source: research notes from Astroray v1 (a CUDA spectral path tracer with a CPU oracle), measured on one RTX 5070 Ti
(sm_120) workstation unless stated. Numbers are v1's, not predictions for the rewrite. Where the notes were thin or
unverified, this file says so.

## Established findings

### Megakernel versus wavefront

- Laine, Karras, Aila (HPG 2013, "Megakernels Considered Harmful") is the foundational argument. v1's reading: a
  megakernel had 2-3x lower throughput on a GTX 580 with five materials; wavefront was 2x or better on mixed scenes;
  queue overhead 5-15%. Cycles X reported 2.2x on one production scene (52 s vs 116 s) and 3-5x on volume scenes. These
  are secondary figures; v1 did not reproduce them.
- v1 adopted wavefront: structure-of-arrays path state, one queue per stage (camera init, intersect, shade, shadow/NEE,
  accumulate), shade work bucketed by material type with a radix sort (skipped when one bucket is occupied). An
  uber-shader stage was acceptable only as a stub. An OptiX shader-binding-table dispatch was rejected because it did
  not fit v1's backend split; it is still a legitimate alternative for an OptiX-first GPU backend.
- Path state is about 220 B per path, of which 48 B is spectral (4 wavelengths, 4 pdfs, 4 throughputs). 65,536 paths is
  about 14.4 MB (3.1 MB spectral); Cycles-scale concurrency (1.72 M paths on this GPU, from its device-queue state-count
  heuristic) is about 374 MB. Half-precision state was deferred.
- Random numbers must be keyed by (pixel, sample, bounce), not thread or queue position, or wavefront ordering changes
  the image and CPU/GPU twins cannot be compared.
- Path pool size matters: 256^2 (65k paths) gave 18% lower throughput than 1024^2 (13.4 vs 16.3 Msamples/s). Launch
  overhead was under 1% (about 1,400 launches for a 64 spp render). Clamp the pool to total work; only float
  accumulation order changes.
- v1's wavefront started about 1.4x faster than its megakernel (1.41-1.46x), then feature accretion made it a large
  regression against the early megakernel: 20.3 ms kernel time for a simple scene (256^2, 64 spp) early on versus about
  290 ms later (scaled Cycles about 77 ms). v1 blamed accumulated features, not spectral sampling (Wilkie 2014 cost
  argued comparable to RGB). One profile showed 4.84x and 5.61x slowdowns on two simple scenes (20.29 to 98.25 ms; 21.87
  to 122.69 ms per render).
- Metric hygiene: compare summed kernel milliseconds per render, not per-launch means (not comparable across
  architectures). Per-kernel deltas do not sum to runtime, so confirm with wall-time A/B.

### Register pressure and the shade stage

- The shade stage dominated: 71% (diffuse), 72% (glass), 35% (heavy-geometry scene) of GPU time (512^2, 64 spp, depth
  8); 43.8% and 52.4% in an earlier profile. It ran 221-254 registers, 1 block/SM, 3.8-8.7 KB stack/thread. 128
  registers would allow 2 blocks/SM at 256 threads. A lean traversal-only kernel needs 40-70; v1's intersect stage ran
  148-223 (128 earlier).
- Cycles' CUDA config for comparison: 384 threads, 168 max registers. v1 used 256 threads and 254 registers.
- Attribution by stubbing and rebuilding: baseline 254 regs / 2576 B stack; NEE off 254 / 2064; BSDF bypassed 255 /
  2320; both off 95 / 584. Either consumer alone saturates 254. So per-material shade splitting cannot raise occupancy
  while lighting alone pins the register file; it is justified only for extensibility. An earlier "shared
  infrastructure, not material code" claim was partly superseded: the spectral variants of the material functions had
  never been stubbed.
- Nsight Compute on the shade kernel (254 regs, 3776 B stack, 16.7% theoretical occupancy): local memory was 72% of L1
  sector traffic. PC sampling: the kernel shell (copying by-value parameters for the ABI) 27%; shade body 37%; the
  spectral-upsampling evaluator 14% of samples but 54% of instructions (a linear search of about 39 iterations).
  Divergence was not the bottleneck (98.5% uniform).
- Under relocatable device code (`-rdc`) a per-kernel register cap cannot go below the largest out-of-line callee (254),
  so the fix is inlining, not capping. Inlining the body halved stack. A cap of 128 with full inlining gave 2.85x,
  2.55x, 1.93x, 1.58x and 1.65x speedups across five scenes. Build time with partial inlining was 509 s (1.1x); inlining
  all 128 material variants took 1189 s, so the build cost is real.
- Compile-time register counts under `-rdc` are unreliable (up to 5x off; `-Xptxas -v` reported 127 versus 221 actual).
  Use a runtime profile or `cuobjdump -res-usage` of the linked binary. `cuobjdump --list-ptx` needs `-all`.
- Kept change: a compile-time "deferred" template flag removing a dead immediate-NEE branch: -1.1% time (1.1446 to
  1.1318 s), shade stack 2576 to 2256 B. Pattern: gate an opt-in path by compile-time flag plus runtime flag so a heavy
  feature does not spill the fused kernel.
- Rejected: a `__device__` function-pointer registry for materials (indirect calls defeat inlining).
- Noted, not executed: binary search on the Jakob-Hanika scale axis (rgb2spec interval-find pattern) instead of linear
  search; scene-specialised kernels.

### OptiX hardware traversal

- Settled design: OptiX does traversal only. Closest-hit writes (t, primitive index, two barycentrics, instance id) to a
  side buffer; existing CUDA code rebuilds the hit record, so shading semantics are unchanged. Shadow rays use
  `TERMINATE_ON_FIRST_HIT | DISABLE_CLOSESTHIT | DISABLE_ANYHIT`; a miss clears "occluded".
  `optixGetTriangleBarycentrics()` matches the (u, v) of Moller-Trumbore.
- Edge semantics: RT-core triangle tests are watertight (Woop, Benthin, Wald 2013). A software Moller-Trumbore is not,
  and v1's used an absolute `|det| < 1e-6` epsilon that dropped small or grazing triangles. Fixed-ray mismatches at
  edges between hardware and software traversal are expected, not bugs. NaN triangles are inactive in OptiX 9.1.
- Eligibility: all-triangle scenes without deformation motion. Spheres, curves and alpha-shadow walks stayed on a
  software BVH. An environment switch forced software traversal (useful as a reference).
- Benefit: Cycles' OptiX/CUDA ratio was 1.0x on a trivial scene and 2.4x on a 2M-triangle scene, so RT cores matter once
  geometry does. v1 projected a heavy scene from 35 s to about 15-17 s; that projection is not backed by a measurement
  in the notes.
- Cycles builds an instance acceleration structure over per-object GAS in its OptiX kernel.

### Two-level BVH and instancing

- Cycles packs all per-mesh BLAS nodes and primitives into one flat array. Per object, a node index records the BLAS
  root (negative marks a single-leaf root). Instance leaves are sign-coded in the primitive-address field; an entry
  sentinel pushed at instance entry triggers the pop. Instancing is single-level, so the object index is a scalar.
- Ray transform convention: modern Cycles keeps the object-space direction un-normalised, so `t` is identical in object
  and world space; classic Cycles normalises and scales `t`. Pick one; every intersector must match. v1 chose
  un-normalised.
- Normals use the inverse transpose. The front-face flag must be recomputed in world space or mirrored instances shade
  backwards. Instance bounds come from the 8 transformed corners.
- Complexity: build O(M log M + N log N), memory O(M + N). pbrt's Moana is about 4 GB instanced versus about 516 GB
  flattened.
- Rejected: Cycles' negated-leaf encoding, normalise-and-scale-t, motion blur in the first cut, an any-hit shadow fast
  path.
- Deferred fork: NEE to instanced emissive geometry needs its own design (a light record holding a primitive index
  becomes ambiguous under instancing).
- Plan: (1) identity-instance parity probe, no production change; (2) multi-instance with hardware parity; (3) BLAS
  cache plus TLAS-only rebuild on a transform edit.

### BVH build and caching

- Binned SAH with 12 buckets (Wald 2007). A parallel build matches the serial tree node for node if leaf offsets are
  deterministic (no atomics; reductions over a fixed 64 chunks). MSVC OpenMP 2.0 has no tasks, so work is split into
  prim ranges of at most 16384. pbrt-v4's recursive build is the reference.
- v1 rebuilt the whole BVH single-threaded on every render call: 1.7 s (CPU) and 2.6 s (GPU) per call on a 2M-triangle
  scene at 16^2, 1 spp. This fixed cost dominates interactive and animation use. Cache keyed on a dirty flag set by
  every scene-mutation entry point (Cycles `need_update_rebuild` is the pattern). Scene upload also repeated every
  render call (cost not quantified).

### Why GPU lagged the reference engine, ranked

At 1024^2, 256 spp, 8 bounces: simple scene 16.5 s vs Cycles OptiX 4.9 s (3.4x); same plus 2M triangles 35.1 s vs 5.3 s
(6.6x). v1's ranked levers (its scores): BVH cache plus parallel build (25); OptiX traversal (6.3); shade diet via
specialisation and register budget (4); path-pool floor (12); Embree 4 CPU backend (3; owner declined); CUDA 13 ptxas
(2-5, speculative); CWBVH (Ylitie 2017; 1.9-2.1x on incoherent rays; fallback if OptiX unavailable); SAH-quality TLAS;
time-to-quality levers.

### CPU/GPU equivalence tiers

- Deterministic paths: ULP-level gates (post-init ULP 2, post-intersect at most 32, NEE/RR at p99.9). Stochastic paths
  (caustics, photon maps): SSIM about 0.97 or better plus energy bounds.
- The forward photon map (Jensen 1996) was the single caustic path for CPU and GPU, GPU port as a uniform hash grid, not
  a kd-tree. Manifold sampling was frozen as legacy on GPU.
- A CPU oracle that reused one hit record across bounces left a "delta" flag set, so every vertex after glass skipped
  lamp NEE. A fresh record per bounce moved oracle/production agreement on a dielectric ladder from 0.94-0.97 to
  0.994-0.997. When twins disagree, the oracle can be the buggy side.
- Uniform-environment gates (ratio 0.96) never caught a missing-specular bug; graded ladders (increasing material
  complexity) did.

### GPU twin bugs found by ladders (a checklist)

- Metal too dim at moderate roughness: GPU lacked multiple-scattering energy compensation (Kulla and Conty 2017),
  layering, furnace scale and sheen/ clearcoat table lookups. After porting: roughness 0.3 from 0.8628 to 0.9984; 0.6
  from 0.8570 to 1.0000 (white-furnace ratios). Also a stale divide and a 0.5-instead-of-0.25 constant in clearcoat.
- Rough-glass furnace deficit: an eta^2 magnitude guard applied only to delta lobes, so rough transmission with f > 1
  was clipped by an albedo lookup. GPU furnace at roughness 0.1: 0.1295 to 0.9987; at 0.3: 0.2833 to 0.9993.
- Spectral evaluation upsampled f*cos through an albedo table clamped to [0,1], capping bright sun NEE. Factoring
  magnitude out as sampling did: grey 0.8 at roughness 0.1 went 0.076 to 0.99; copper at 0.5, red 0.939 to 0.998.
- GPU ignored the specular parameter (lowered to diffuse plus conductor normalised by 2): predicted 0.3177, measured
  0.3178, versus CPU 0.1853.
- Environment lookups used floor(u*W) instead of texel-centre bilinear sampling (Cycles `interp_bilinear`): probe error
  0.59 to 5e-6.
- Hero wavelengths: the GPU sampler put the first wavelength in the first 1/N of the band instead of uniformly, giving
  violet-only dispersive caustics. Also the secondary-termination step never divided the hero pdf by N (Wilkie et al.
  2014; pbrt-v4 `TerminateSecondary`), so dispersive caustics were about 4x dark on both CPU and GPU. Naive
  terminate-only, hero x N, and divide-by-alive-count fixes all failed gates.

### Build speed

- Fresh CUDA builds took 1637-2122 s; the translation unit holding all shade code was 96% of wall time (front end 910 s,
  assembler 834 s, 8.4 GB peak RSS). Device link (`-rdc`) took 44-73 s and is serial. Parallelism was about 1.2.
- Splitting that unit into 12 parts (explicit template instantiation, `extern __constant__` for shared constants under
  `-rdc`): fresh 477 s quiet (704 s loaded); touching one part 208 s; touching the former big file 132 s. All 356
  kernels kept identical register and stack counts; constant memory grew about 1.2 KB. Touching a header those units
  reach still cost about 11 min. Device link stayed 52-75 s on every incremental path.
- Deferred: `-split-compile` (needs an identity gate), native sm_120 SASS (small benefit), device LTO (rejected: changes
  codegen), CUDA 13 A/B.
- A compiler-cache server with a 600 s idle timeout was suspected of dropping long CUDA compiles; never tested.
- A CMake OptiX finder module expected an 8.x SDK while 9.1 was installed.

### Test speed

- Baseline: 4587 cases, 45:11 wall; the heaviest 15 files were 55% of time.
- Automatic CPU/GPU tagging by source/filename classifier, CPU tests parallel (`-n auto`), GPU tests serial: 407.5 s to
  about 335 s (18%) on a 214-file suite (85 GPU, 129 CPU). CPU pass 1166 tests in about 197 s on 8 workers; GPU pass 424
  tests in about 137 s.
- Later: 45.2 min to 23.8 min (CPU parallel 451 s plus GPU serial 977 s); fast profile 7.2 min covering 3593 of 4646
  tests. Targets of 15 and 5 min were not met.
- Rules worth keeping: never remove a gate or loosen a tolerance for speed; for floor-governed tolerances cut the sample
  budget only if k*sigma is at most half the floor; fixed-threshold tests need at least 5 seeds with numbers recorded;
  share identical renders via a cache; mark slow tests, never delete; no zero seeds (zero meant random);
  expected-failure tests follow the same rules.
- Remaining floor: about 1130 sub-second tests cost about 200 s of the serial GPU pass because CPU-only tests lived in
  GPU-tagged files. Per-test tags are the next lever.
- Budget cuts that held: 8192 to 2048/4096 spp; 256 to 64 spp; 256^2 at 256 spp to 128^2 at 64 spp; 320 to 160 spp
  (stricter).
- CI baseline: four samples, 1385-1861 s end to end; the test step was 90-92% of its job; CUDA compile 716-985 s; 2133
  passed, 269 skipped, 15 xfailed, 4 xpassed. Draft claims of queue time and compute-seconds were rejected as
  unmeasured. xdist for CPU-marked tests was the proposed next experiment, not adopted.
- A test file calling `sys.exit(0)` at import silently ends collection; use skip-on-import instead.

### First-frame latency

First CUDA viewport frame took about 12,079 ms (context init plus JIT from embedded PTX) versus about 14 ms later. The
planned fix was a trivial render at init, which moves the cost rather than removing it. The acceptance target (first
frame at most 100 ms) was never measured.

## Sources and reference implementations

| Source | Authors, year | Used for |
|---|---|---|
| "Megakernels Considered Harmful: Wavefront Path Tracing on GPUs", HPG 2013, DOI 10.1145/2492045.2492060 | Laine, Karras, Aila, 2013 | Wavefront rationale |
| "Understanding the Efficiency of Ray Traversal on GPUs", HPG 2009 | Aila, Laine, 2009 | Traversal, instancing context |
| Compressed wide BVH traversal (CWBVH), HPG 2017 | Ylitie, Karras, Laine, 2017 | BVH fallback (1.9-2.1x claimed on incoherent rays) |
| Watertight ray/triangle intersection, JCGT | Woop, Benthin, Wald, 2013 | Hardware triangle edge semantics |
| Binned SAH BVH build, 12 buckets | Wald, 2007 | Parallel BVH build |
| "Embree: A Kernel Framework for Efficient CPU Ray Tracing", ACM TOG 33(4):143 (SIGGRAPH 2014), DOI 10.1145/2601097.2601199 | Wald et al., 2014 | Instancing design |
| "Hero Wavelength Spectral Sampling", EGSR 2014, CGF 33(4):123-131, DOI 10.1111/cgf.12419, https://cgg.mff.cuni.cz/publications/hero-wavelength-spectral-sampling/ | Wilkie, Nawaz, Droske, Weidlich, Hanika, 2014 | Hero sampling, secondary termination |
| Multiple-scattering compensation for microfacet BRDFs, SIGGRAPH course notes | Kulla, Conty, 2017 | Energy compensation |
| Non-symmetric scattering in light transport | Veach, 1997 | Refraction radiance scaling |
| Photon mapping | Jensen, 1996 | Caustic path |
| Shader Execution Reordering, I3D 2020 (1.3-2.0x; an arXiv 14.7x figure said not to transfer) | Meister et al., 2020 | Reordering idea |
| H-PLOC GPU BVH builder, HPG 2024 | Benthin et al., 2024 | BVH build (moot with hardware traversal) |
| Dr.Jit, SIGGRAPH 2022 | Jakob et al., 2022 | Runtime kernel specialisation precedent (BSD-3) |
| Stochastic texture filtering, I3D 2024 | Pharr et al., 2024 | Listed in research set |
| Tracked collisions with local majorants | Miller et al., 2019 | Media variance analysis |
| pbr-book 4th ed. wavefront chapter; developer.blender.org kernel_scheduling; OptiX 9.0 release forum post | various | Background |
| OptiX SDK 9.1.0 samples (BSD-3); OptiX 9.1 Programming Guide (ray flags pp. 147-148); OptiX 8 guide (SBT only) | NVIDIA | Traversal-only OptiX design |

External-engine files read (as recorded):

- Cycles `src/bvh/bvh2.cpp` (`pack_instances`), `src/kernel/bvh/traversal.h`, `bvh.h`, `util.h`,
  `src/kernel/geom/object.h` (`object_fetch_transform`), `src/util/transform.h` (Apache-2.0).
- Cycles `intern/cycles/kernel/device/optix/kernel.cu` (IAS over per-object GAS); `kernel/device/cuda/config.h`.
- Cycles `intern/cycles/device/cuda/device.cpp` `reserve_local_memory()` (Apache-2.0): cited only from search snippets
  and a third-party fork (wchargin), not Blender main; v1's excerpt was paraphrased and unverified.
- Cycles @ c9227ff `src/kernel/integrator/state.h`, `state_template.h`, `intersect_closest.h`, `shade_surface.h`,
  `init_from_camera.h`, `src/device/cuda/queue.cpp`; `CUDADeviceQueue::num_concurrent_states`.
- Cycles `src/util/stats.h`, `src/scene/stats.h` (Apache-2.0); `Geometry::need_update_rebuild`.
- Cycles `bsdf_microfacet.h`, `bsdf_util.h`, `svm/closure.h`, `closure_principled.h` (hero branch);
  `intern/cycles/kernel/device/cpu/image.h` (`interp_bilinear`).
- pbrt-v4 @ eef3a6e `workitems.soa`, `workqueue.h`, `integrator.cpp`; `src/pbrt/util/stats.h`; `optix.cu`;
  `aggregate.cpp`/`aggregates.cpp` (`buildRecursive`); `primitive.cpp` (`TransformedPrimitive`); `transform.h`;
  `src/pbrt/util/spectrum.h` (`SampledWavelengths::TerminateSecondary`); `bxdfs.h` (`DielectricBxDF::Sample_f`). pbrt-v4
  is Apache-2.0 (v1 corrected an earlier BSD-3 claim). pbrt book Ch. 7 section 7.1.2.
- Mitsuba 3 `profiler.h` (BSD-3, `ProfilerPhase` list). LuxMark (GPL-3) excluded by licence. Blender Open Data was
  look-and-feel only (site returned 403).

## Measured numbers worth keeping

- Shade stage: 254 registers, 2.5-8.7 KB stack/thread, 16.7% theoretical occupancy, local memory 72% of L1 sector
  traffic.
- Register cap 128 with full inlining: 1.58x to 2.85x faster across five scenes; build 1.1x (partial inlining) or 1189 s
  (full).
- Native sm_120 SASS versus compute_89 PTX JIT: 1.80x slower (97.84 to 175.62 ms) on a diffuse scene, 1.68x slower on
  glass, shade 2.52x slower (239 to 604 ms, regs 221 to 229). Register pressure is intrinsic, not a JIT artifact.
- Clock drift: the same build measured 1.120 s and 1.179 s minutes apart (about 5%). What worked: burn in to 2887 MHz P0
  before timing, min of 10. Clock locking needs administrator rights.
- Cycles reference at 1024^2/256 spp/8 bounces: simple scene 4.9 s, heavy 5.3 s.
- Tests: 45.2 to 23.8 min full; fast profile 7.2 min; `OMP_NUM_THREADS=1` on the parallel CPU pass 856 s versus about
  197 s unpinned.
- A glass-energy plus VNDF (Heitz 2018) rewrite moved GPU white furnace from 0.705 to 0.991.
- Spectral CPU/GPU SSIM 0.999 at 8192 spp; Sellmeier BK7 IOR error at most 1e-4 relative; manifold-sampling GPU SSIM
  0.928 (legacy).
- Variance investigations: media full variance 4.27x versus a 3x gate; dispersion 20-seed ratio 2.82x with the glint
  class owning 96% of tail variance (correct transport: the sun disc seen through glass); photon gather 12.4x trimmed
  variance on a prism floor (documented divergence, not suppressed). A spot-light cone using the outer angle where
  Cycles uses 0 inflated trimmed variance 80.5x; fixing it gave 25x. A distant-light power estimate starved sun
  selection (weight about 6e-5).

## Dead ends and negative results

- Native sm_120 SASS to cure the register blow-up: slower, not faster.
- `__launch_bounds__(256,2)`: byte-identical codegen; ignored on sm_120.
- `__noinline__` on a spectral-conversion helper: 2.7% slower, larger stack.
- Per-material shade split to raise occupancy: impossible while NEE alone saturates registers.
- Moving one kernel to its own translation unit lost 560 B of stack because a helper stopped inlining. Rule: move
  kernels only with a before/after `cuobjdump -res-usage` comparison.
- A 1.0 s target on a reference scene under a "performance only, correctness frozen" scope: baseline 1.145 s, best about
  1.13 s; the 1.5 s ceiling stayed, owner decision pending.
- Reading compile-time stack or register counts as runtime truth under `-rdc`.
- Kulla-Conty compensation as the cure for the rough-glass furnace deficit: zero effect; the real cause was the
  delta-only eta^2 guard.
- Treating a legacy manifold-sampling SSIM drop (0.8352 vs floor 0.85) as a regression: GPU output had legitimately
  changed and the gate's targets had not moved. The owner xfailed that gate and retired a PSNR gate recomputed against a
  moving reference; no floor was silently lowered.
- `pytest-testmon` for impacted-test selection: fingerprints bytecode and cannot see changes inside the native
  extension; a keyword map replaced it.
- SSIM gates on independent RNG streams read about 0 by construction; unreachable gates, not evidence.
- Pinning OpenMP to one thread in the parallel test pass: 4x slower.
- Nsight Compute counters blocked by driver permission until enabled with administrator rights.
- Rejected or not pursued: CUDA graphs and streams, returning to a megakernel (regressed identically), neural radiance
  cache (biased), offline ReSTIR PT (no equal-time evidence), H-PLOC (moot with hardware traversal), an umbrella CPU/GPU
  equivalence spec before the wavefront port landed.
- A 43-PR-window bisect protocol (register growth plus channel drift) was designed; its results are not in the notes.
  Re-baselining was explicitly not recommended before a bisect proved the cause.

## Design ideas worth reconsidering in the rewrite

- Treat register budget as a first-class architectural constraint: decide at design time which stages fuse and what each
  may spend. In v1, lighting and BSDF evaluation each saturated 254 registers independently. Consider NEE sampling in
  its own stage (pbrt-v4 style).
- Make intersect a thin traversal-only kernel (40-70 registers) with hardware OptiX traversal from day one; keep
  software traversal only as a reference and for non-triangle primitives.
- Compile one kernel source for CPU and GPU. v1's twins drifted in at least six distinct ways, each invisible to
  average-scene gates; use graded material ladders as the check.
- Make acceleration structures cacheable from the start: dirty-flag BVH, persistent scene upload, per-object BLAS with
  transform-only TLAS update for interactive edits, deterministic parallel build.
- Fix the instancing convention early: un-normalised object-space direction so `t` is unit-consistent; recompute the
  front face in world space.
- Spectral: replace the linear-search spectral-upsampling evaluation with binary search or a lookup; consider sharing
  wavelength samples across a warp (`__shfl_sync`, speculative in v1's notes).
- Runtime scene-specialised kernels via NVRTC (Dr.Jit precedent) so unused features cost nothing in the fused kernel.
  Owner-approved in v1 after other levers; never measured. Shader Execution Reordering, Blackwell linear swept spheres,
  opacity micromaps, cluster acceleration structures and cooperative vectors were listed as speculative bets.
- Size the path pool by the Cycles heuristic (about 1.7 M paths) clamped to total work; about 220 B/path with 48 B
  spectral; half-precision state later.
- Build architecture: partition the heaviest kernel into many translation units from the start, with explicit
  instantiation and header discipline; expect a serial 50-75 s `-rdc` device-link tax. v1 went from about 30 min fresh
  to under 8 min with the split.
- Test architecture: tag CPU/GPU per test (not per file) from day one; keep deterministic GPU gates exact; use a keyword
  impacted-test map rather than coverage tools blind to native code.
- Enable GPU profiler counters (administrator rights) up front.
- Time to first frame: initialise the GPU context at device creation, or ship native SASS for target architectures to
  avoid the 12 s JIT.

## What no longer applies

- Matching pbrt-v4's work-item/queue layout to v1's integrator list.
- v1's backend-split reasons for rejecting an OptiX shader-binding-table dispatch.
- Cycles pixel-parity benchmarks and the benchmark CSV schema.
- v1's engine-benchmark collector scope (untyped stat record, fixed seed, psutil RSS at 50 ms polling); only the
  untyped-stats idea carries over.
- v1's factory-registry plugin architecture (six registries, static-initialiser macros, CMake glob); only
  registry-by-name is portable.
- The CUDA/pybind11 separation (static library, pimpl renderer, POD structs padded to fixed sizes, curand, firefly clamp
  at luminance 20, in-kernel gamma): built for a mixed host/GPU build and legacy tonemapping.
- Architecture list 75;86;89 only: predates the sm_120 target.
- Manifold-sampling GPU caustics (frozen legacy).
- Commit-level bisect commands, workstation-specific OneDrive file-lock failures.
- Cycles-specific quirks kept only to match its pixels (conductor/dielectric linear mix versus F0 blend, leaving a 7-9%
  half-metal gap).

## Open questions

- Real hardware counters (occupancy, stall reasons) for the shade stage: never obtained.
- CUDA 13 ptxas effect on register allocation and build time: A/B never run.
- Actual OptiX traversal speedup on a heavy scene (projected, not measured).
- Viability of device LTO or `-split-compile` with an identity gate.
- Cause of compiler-cache drops on long CUDA compiles.
- Whether the shade stage can reach 128 registers without impractical build times.
- Whether Shader Execution Reordering gains transfer to this workload.
- Whether a 100 ms first-frame target is achievable with pre-warm.
- Residual small-roughness (at most 0.10) metal dimness on GPU (ratios 0.60-0.77) was unresolved; candidates are the
  plain-NDF sampler at small alpha, or precision.
- Whether CPU shows the same 4x dispersive-caustic darkening: not verified.
- Teardown probe cost (315 s over 4569 test items) was to be measured before any cheapening.
- The notes are thin on memory-bandwidth analysis of path state, multi-GPU, and any non-NVIDIA backend; none was
  researched.

