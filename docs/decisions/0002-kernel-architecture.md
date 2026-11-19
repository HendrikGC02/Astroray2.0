# 0002 — Kernel architecture, GPU execution model and budgets

Status: proposed (2026-11-14), revised after independent review (2026-11-14), reconciled with `RULINGS.md` (2026-11-14)

## Context

Astroray 2.0 needs one kernel source that runs on the RTX 5070 Ti (sm_120) and on the CPU (owner decision 2). The viewport has to hold 30 updates/s at full resolution and never fall below 24, with 60 a later goal and first frame comparable to Cycles (decisions 9, 10, 21, 23). The first release covers the full feature breadth (decision 7), including gratings and the light-tracing pass they need, designed in from the start (decision 17). Builds and tests have hard budgets (CLAUDE.md rule 9) and failures must be loud (rule 7). Most code will be written by Sonnet-class agents from small task specs, so the structure must make the cheap thing the right thing and must catch drift mechanically.

v1 failed exactly here. Its fused shade kernel had 128 template variants, ran at 221-254 registers with 2.5-8.7 KB of stack, and one translation unit took about 1,800 s of a 1,870 s build (`v1-lessons/research/gpu-build.md`, `v1-lessons/toolchain-and-build.md`). The spine (`SPINE.md` §2) settles the language (restricted C++), the width (4 wavelengths), the fused surface kernel under `__launch_bounds__`, the per-kernel budgets and one GPU compiler (nvcc, ruling 16). Spike A measured these on a kernel with the right shape but less than full scope. This record turns them into rules an implementer can follow without guessing, and adds what the spine leaves open: queue and scheduling mechanics, the complete kernel and OptiX program list, the instance visibility mask, memory layout mechanics, how the gate measures budgets, FMA and determinism policy, device failures, and what happens when the kernel grows.

Evidence: spike A (`spikes/A-kernel-budget/RESULTS.md`) for registers, spills, compile times, OptiX module timings and CPU/GPU agreement; spike B (`spikes/B-blender-end-to-end/RESULTS.md`) for Cycles' per-sample cost; research 03, 04 and 17 for how Cycles and pbrt-v4 do the same job. Spike C (wavefront vs megakernel) was not run; D7 gives an overhead estimate in its place and Risks 2 deals with the gap.

## Decision

### Challenges to the spine

- **C1 (correction of a cited number).** Spine §2 says CPU/GPU agreement is tested "on the throughput weight f·cos/pdf ... (1.1e-6 agreement measured)". The 1.1e-6 is the max error of **BSDF eval f** with FMA off. For the **throughput weight** with FMA off, spike A measured p99.9 1.4e-5 and a max of 1.6e-2 on one input (§6). Gates therefore use p99.9 thresholds with a separate outlier rule (D12), not a 1e-6 max.
- **C2 (register gate): accepted by ruling 17; spine §2 is amended.** The merge gate checks spills and stack of the **shipping (capped) cubin** against recorded allowances; the uncapped count is a warning from `check full` (D10). Evidence kept for the record: the uncapped count does not predict shipped speed (the fused kernel capped at 128 spills 68/112 B and runs 56% faster than uncapped; even a 64-register cap with 472/836 B of spills beats uncapped, spike A §2-3), and fused + in-kernel VM is already 150 uncapped before upsampling, textures, the light tree and denoiser writes (spike A §8.1), so a hard uncapped gate would force a split that measured 22% slower.
- **C3 (caveat on "Real switchable to double").** Embree traverses in single precision, so the f64 CPU build has float traversal. The double reference is exact only for stages that do not trace rays (lobes, geodesic stepping). 0006 must know this.

### Kernel dialect and portability layer

**D1. Language and allowed constructs.** Kernel code is C++17 in freestanding headers under `src/kernel/`, compiled by nvcc for the GPU (ruling 16) and MSVC `cl` for the CPU.
- *Allowed:* free functions; POD structs with member functions; `constexpr`, `if constexpr`, `static_assert`; enums; fixed-size arrays; references; `<cstdint>`, `<cfloat>`. Function templates may take only two parameters: the scalar type (`Real`/`float`/`double`) and the one kernel-identity parameter of D8. The spectral width is 0003 decision 1's constant `kLanes = 4`, not a template parameter.
- *Forbidden:* virtual functions, RTTI, exceptions, `new`/`delete`/`malloc`, recursion, function pointers, STL containers and algorithms, direct `<cmath>` calls (use D2's wrappers), `__noinline__`, `-rdc`, inline PTX or asm outside the portability header, `long double`, thread-local storage, mutable globals (rule 8), `printf` outside debug builds, unsuffixed floating literals in float code, host-only headers. `__noinline__` is banned because spike A measured -37% at N=4 with up to 1.4 KB of stack.
- *Passing:* everything is force-inlined. Structs larger than 32 B go by `const&`. Results return by value as small structs, never through scalar out-pointers (v1: address-taken out-parameters cost about 4%).
- *Enforcement:* CPU kernel TUs build with `/EHs-c- /GR- /W4 /WX`. Every GPU kernel except `advance_geodesic_f64` (D8) builds with `-Xptxas --warn-on-double-precision-use`, warnings as errors. A lint script owned by 0009 checks banned tokens and the include allow-list.

**D2. Portability header `src/kernel/portability.h`** is the only file that knows its target. It provides: `AR_FN` (`__host__ __device__ __forceinline__` under `__CUDACC__`, else `inline`); `AR_GPU` (constexpr bool) and `AR_ASSERT` (compiled out of release); `ar::load_ro(p)` (`__ldg` on GPU); `ar::atomic_add` for `float` and `uint32_t`, `ar::atomic_max` and `ar::atomic_cas` for `uint32_t` (0008 decisions 2, 11, 12); `ar::clock` (`clock64()` on GPU, `__rdtsc` on CPU; 0008 decision 12); `ar::ballot`, `ar::lane_id` with single-lane CPU fallbacks; `ar::bit_cast`; math wrappers (`ar::sqrt`, `exp`, `log`, `sin`, `cos`, `pow`, `fma`, ...) overloaded on `float` and `double`. No address-space layer (CUDA has none; HIP/Metal are not built, spine §1). Pattern: Cycles `kernel/device/cuda/compat.h`, pbrt `PBRT_CPU_GPU`.

**D3. `Real`.** `float` in the GPU build and the default CPU build (`astroray_kernels_cpu`); `double` in a second CPU build (`astroray_kernels_cpu_f64`, `AR_REAL_DOUBLE`), a test reference that is never shipped (ruling 41). CPU geodesic integration is double in every build (0006 D39); GPU geodesic precision is 0006 D40's, through the two `advance_geodesic` kernels of D8.
- `Real` is used for kernel arithmetic and path-state fields (D13), so the f64 build stays double between stages.
- Scene and asset buffers (vertices, textures, tables, material records) are explicit `float` or `half`, converted on load. Literals are written `Real(0.5)`.
- Cost: one extra CPU compile (spike A: same compile time as float; throughput -6%).

**D4. One stage function, two targets.**

```
// src/kernel/stages/<stage>.h
AR_FN void stage_<name>(const KernelGlobals& kg, uint32_t item, const StepEntry& step);
```

- **GPU (`src/kernel_gpu/<stage>.cu`):** `AR_GPU_STAGE(name)` expands to `extern "C" __global__ void __launch_bounds__(T, B) k_<name>(const KernelGlobals* __restrict__ kg)`. The kernel reads its `StepEntry` (segment start, launch count, shadow-slot base) from the device-resident step table (D7) and runs a grid-stride loop over its launch count. `T` and `B` come from one table, `src/kernel/launch_config.h`, which the host launcher also reads.
- **CPU (`src/kernel_cpu/<stage>.cpp`):** `AR_CPU_STAGE(name)` loops over the same segment in chunks of 1,024 items on the host thread pool (pbrt `ParallelFor`).
- **Arguments:** one 8-byte pointer. `KernelGlobals` holds the scene, state, queue, step-table and settings pointers, lives in device memory (one copy per session), and is never passed by value: v1 found by-value parameter copies cost 27% of PC samples. Because counts are read on the device and the loop is grid-stride, the host may size grids from read-back counts or launch fixed grids; switching between them changes only host code (D7 remedy).

### Wavefront execution model

**D5. Pools.** A session has two structure-of-arrays pools; their fields and byte budgets are 0003's (ruling 9).
- **Main path pool:** camera paths and light paths (D8), told apart by the `path_kind` state bit. Capacity `min(work in the pass, 2^21, the path-state row of 0007's VRAM budget table)`, stated in 0001's per-render report. CPU default 2^16.
- **Shadow pool:** self-contained visibility items emitted by shading stages: NEE shadow rays, volume NEE, camera connections from light paths. An item holds its ray, tmax, contribution and wavelengths, plus a delivery target: a slot of 0008's per-sample buffer for NEE items (ruling 1), or a film position for camera connections, which 0008 splats over the normalised pixel-filter footprint (ruling 2). It never depends on the path that created it. Capacity 2x main (PROVISIONAL).
- **Slot assignment is deterministic and atomic-free.** Free shadow slots are the shadow pool's `TERMINATED` segment, in index order (D6). Each shadow-producing stage declares in `launch_config.h` the most shadow items `k` one item can emit in one step (default 1). The step table gives the producers, in `Stage` order, consecutive ranges of the free list. An item at position `i` of its segment owns free slots `[base + k·i, base + k·(i+1))`, whether or not it emits. Unused reserved slots stay `TERMINATED`. If the free slots cannot cover a producer's whole segment, the producer launches on the first `floor(free_left / k)` items. The rest keep their `next_stage` and run next step, so no item is dropped. No compaction kernel is needed, because freed slots return to the free list at the next partition.

**D6. Queues (deterministic partition, no append queues).**
- Every pool item carries `next_stage` (u8, closed enum `Stage`). Each pool has its own `TERMINATED` value.
- `queue_build` performs one stable partition over both pools by `next_stage`, each pool up to its high-water mark (the largest slot index used this pass). It writes per-stage segments of pool-local indices. It runs as three kernels (block histogram, scan over blocks, stable scatter); the scan is D7 remedy (c)'s single-pass scan with decoupled look-back (Merrill and Garland 2016; scatter pattern from Cycles `device/gpu/parallel_active_index.h`).
- The scan kernel also fills the **step table**: per stage, the segment start and launch count, and for producers the shadow-slot base (D5), plus the refill count (D7). The table is computed on the device by the same stage function on both targets.
- Segments are in item-index order. The main `TERMINATED` segment is the free-slot list for refill. The main pool is not physically compacted unless D7 remedy (b) is adopted by measurement.

**D7. Scheduling (one host loop shared by both targets).** Host C++ written against a two-method device interface (`launch(stage)`, `read_step_table()`), implemented once for CUDA and once for the CPU. A **step** is:
1. **Partition.** Run `queue_build` (fills the step table).
2. **Read the step table** (under 256 B, pinned memory) and wait on one event. It is the step's only host wait: the step's other host-to-device inputs (high-water marks, refill entries) are staged in pinned memory and enqueued without waiting, and device copies (the sort's) never wait (remedy (a), task 120).
3. **Sort.** If the `shade_surface` segment holds at least 65,536 paths and the scene's materials run at least 2 distinct surface programs (PROVISIONAL; RULINGS 80), counting-sort that segment by material id (Cycles `parallel_sorted_index.h`). Only `shade_surface` is sorted, as in Cycles. The per-material offsets come from remedy (c)'s look-back scan over the (tile x material) histogram, a third kernel `sort_scan` (task 119), instead of each scatter tile summing every earlier tile's column. Measured on B-5M (1080p, 1 spp, 64 passes after 8 burn-in, nsys): the sort's three launches per pass took 10.07 ms, 33.0 % of the pass (was 23.59 ms, 53.6 %), the longest sort launch about 1.8 ms (was 11.3 ms); With `sort_count` and `sort_scatter` at one warp per tile (task 120; lanes rank equal materials with one warp match, `ar::match_any`), the three launches take 0.47 ms per B-5M pass (62, 33 and 62 µs per launch). Threshold re-check (task 120, same method): B-5M passes take 16.92 ms sorted and 15.52 ms with the sort disabled (bench wall), and `shade_surface` is slower sorted, 5.27 against 4.35 ms per pass. Nsight Compute over one pass: the sort changes neither the instructions executed (993 M against 996 M warp instructions) nor the SIMT efficiency (16.3 active threads per warp instruction either way), because B-5M's 300 materials differ only in constants and run the same code; it raises DRAM traffic from 690 to 835 MB read and 389 to 571 MB written, with the L2 hit rate of the second bounce down from 75 % to 68 %, because consecutive threads then touch scattered path-state slots. On B-5M the sort does not pay. Hence (RULINGS 80, task 122) `kSortMinMaterials` counts distinct surface programs, not material headers: every `CONSTANT_SURFACE` material is one shared program, any other is the program at its header's `program_offset` (identical bytecode at two offsets counts twice), the default material is not counted; the material store counts them once per table set at upload (`sort_program_count`, `sort.h`) and the scheduler compares that count; the key stays the material id. Cycles sorts `shade_surface` by shader id on every launch (`intersect_closest.h`, `path_trace_work_gpu.cpp`); a Cycles shader is its own SVM program. Measured (task 122, same method, nsys period / `shade_surface` per pass): B-5M (one program) now unsorted, 16.12 / 4.36 ms, against 17.62 / 5.29 ms with the sort forced; `scenes/b5m_two_programs` (half the materials UV-modulated, 151 programs, two distinct codes) sorted 16.56 / 4.65 ms, against 15.77 / 4.13 ms unsorted, so on that scene the sort still costs more than it gains. The thresholds stay PROVISIONAL.
4. **Refill.** If fewer than a quarter of the main slots are active and pass work remains, launch `generate_camera` / `generate_light` on the refill count. This is Cycles' rule (refill when active < max/4, `integrator/path_trace_work_gpu.cpp`, research 04). The pass's work is one counter over its camera samples followed by its light samples. 0004 sets the light-sample count, which is zero when the scene has no casters.
5. **Launch** every stage with a non-zero launch count once, in `Stage` enum order.

A pass ends when the work counter is exhausted and every segment except the two `TERMINATED` segments is empty; then 0008's `film_resolve` runs once (D8, ruling 1). Each stage touches only its own segment's items, its reserved shadow slots, and 0008's per-sample buffer or splat plane (fp32 atomic adds), so the order between stages does not change results; the fixed order gives the same schedule on CPU and GPU. This departs from Cycles' "largest queue first" rule. The loop checks for a pending generation (0007 decision 25) between steps, so cancellation latency is one step.

**Launch duration.** Every launch stays within 0009 B-17 (ruling 14). Stages whose per-item work is unbounded (medium tracking, the emission-absorption marcher, geodesic stepping, shadow evaluation over many layers) cap work per item per launch and re-queue the item into their own segment, as 0006 specifies for media and geodesics; capped runs stay bit-identical because the sampler is counter-based.

**Overhead estimate (derivation, not a measurement).** One bounce takes two steps (intersect, shade), and its shadow item is traced in the next step alongside. At a default maximum depth of 12, a pass needs about 26 steps, plus one per alpha layer on the longest path (D11). A step costs 3 partition launches, up to about 8 stage launches and one round trip (unmeasured under WDDM). Partition traffic at 1080p 1 spp is about 40 MB per step. The resulting overhead share, derived in 0009 B-19's basis from these counts, is **above B-19**. It is measured before breadth work (Risks 2). Remedies, in order:
- (a) Fixed grids, and the step sequence captured as a CUDA graph. **Adopted as stream-ordered transfers, without graphs (task 120, ruling 79 order: third).** The step waits once, on its table read (step 2); its uploads are staged in a pinned ring that waits only when it wraps; the pass-start identity queue is a device copy from a buffer filled once per allocation (an 8 MiB host upload per pass at 2^21 slots had left the GPU idle about 1.5 ms at every pass start); `queue_build` count and scatter launch one warp per tile on the GPU. Graphs were built and measured (1080p, 1 spp, 64 passes after 8 burn-in, nsys) and dropped: `optixLaunch` refuses stream capture (it queries its own events: CUDA error 900 in global capture mode, an illegal address in relaxed mode), so the intersect launches stay outside the graph; fixed grids launch every stage every step, about 0.8 ms of empty launches per B-1M pass; and the round trip into a graph launch costs about 62 µs per step against about 37 µs for a direct first launch. Images were bit-identical either way.

  | 1080p, 1 spp | B-1M pass (bench wall / nsys period) | B-1M GPU idle | B-5M pass | B-5M GPU idle |
  |---|---|---|---|---|
  | before (task 119) | 16.97 / 18.51 ms | 29.9 % | 28.84 / 30.20 ms | 18.9 % |
  | stream-ordered transfers | 15.03 / 16.09 ms | 19.5 % | 26.94 / 27.96 ms | 12.4 % |
  | fixed grids + graphs (dropped) | 16.02 / 16.75 ms | 21.2 % | 27.61 / 28.14 ms | 12.1 % |
  | transfers + warp-per-tile `queue_build` | 14.82 / 15.92 ms | 21.8 % | 26.59 / 27.18 ms | 11.5 % |
  | the same + warp-per-tile sort | 14.73 / 15.60 ms | 19.8 % | 16.92 / 17.51 ms | 17.7 % |

  B-19 still fails: B-1M idle 19.8 % and partition 16.7 %; B-5M idle 17.7 % and partition plus sort 17.7 %. The largest single idle is between passes (about 1.7 ms from `film_resolve` to the next pass's first kernel, host work in the session).
- (b) Compact the main pool when live paths drop below a quarter of its high-water mark (Cycles `compact_main_paths`).
- (c) Fuse the three partition kernels into one single-pass scan. **Adopted (task 118, ruling 79 order: first).** The scan over tile histograms is a single-pass scan with decoupled look-back (Merrill and Garland 2016, NVR-2016-002, sections 4-5) across many warps instead of D6's one-block reduce-then-scan; count and scatter take one warp per tile. The kernels stay three: the scatter needs every segment's total (its start) before it places an item, which no single pass over the tiles knows. Measured at 1080p, 1 spp, 64 passes after 8 burn-in (nsys): `queue_build` is 15.0 % of the B-1M pass (was 59.8 %; 3.05 ms of 19.85 ms) and 6.8 % of B-5M (was 38.9 %; 3.07 ms of 45.04 ms); scan 826 to 8 µs per launch. B-19 still fails on B-1M (partition 15.0 %, GPU idle 33.4 %) and on B-5M through the sort (52.5 %, task 119).

**D8. Kernel list (complete).** One kernel per TU. Each step kernel owns one queue segment and writes `next_stage` for every item it processes. Semantics belong to the cited record; names, pools, launch bounds and budget rows belong here (ruling 6).

| Kernel | Pool | Owns |
|---|---|---|
| `queue_build_{count,scan,scatter}` | both | partition, step table (D6): count and scatter one warp per tile, scan a single-pass decoupled look-back over the tile histograms (D7 remedy (c)) |
| `sort_{count,scan,scatter}` | main | material sort of `shade_surface` (D7 step 3): count and scatter one warp per tile (equal materials ranked by a warp match), scan the `queue_build` look-back scan over the (tile x material) histogram |
| `film_build_active` | per pass (film) | writes the film-owned active-pixel list at pass start: every pixel of the render sub-rectangle in M1a, adaptive compaction in M4 (0008 d3); one thread per listed pixel, no `Stage` value |
| `generate_camera` | main | refill launch: slot init from the active-pixel list (read-only): pixel, sample, wavelengths (0003), camera ray with time, converged pixels are absent from the list (`film_build_active`, 0004 D16) |
| `generate_light` | main | light-path init (method 0004; wavelengths from the sensor table, recomputed from the seed, ruling 7) |
| `intersect_closest` (OptiX raygen) | main | closest hit, hit record, `route_hit` (D11); also flat segments emitted by `advance_geodesic` |
| `intersect_subsurface` (OptiX raygen) | main | random-walk local rays against one instance's GAS (D11) |
| `intersect_shadow` (OptiX raygen) | shadow | opaque test; non-opaque hits recorded by `ah_record` (D11) |
| `shade_background` | main | camera-path miss: world/sky emission with MIS, guide-buffer writes |
| `shade_light` | main | lamp crossing (0004 D8): camera paths add MIS-weighted Le, light paths pass through; re-queues `intersect_closest` |
| `shade_surface` (`ShadeKind::Common`, D9) | main | node VM to OpenPBR record, alpha pass-through, false-interface stack update (0006), emission with MIS, NEE emitting a shadow item, BSDF sample, Russian roulette, first-hit denoiser buffers (0008), diagnostics |
| `shade_surface_ext` (`ShadeKind::Ext`) | main | 0005's extension kinds on camera paths; HAIR may split once into `shade_hair` (`ShadeKind::Hair`) and, on light paths, `shade_light_path_hair` (`ShadeKind::LightHair`), on measurement (0005 item 18) |
| `shade_light_path` (`ShadeKind::Light`) | main + shadow | light-path surface interaction, no extension code; camera connections are shadow items with a film position (method 0004) |
| `shade_light_path_ext` (`ShadeKind::LightExt`) | main + shadow | 0005's extension kinds on light paths (0004 D21) |
| `shade_shadow` | shadow | recorded non-opaque hits in t order through the VM `opacity` entry, medium transmittance (0006), deliver or re-queue |
| `shade_volume` | main + shadow | 0006's medium stage: null-scattering tracking and the emission-absorption marcher (ruling 23), volume NEE and light-path connection items |
| `shade_subsurface` | main | random-walk step (model 0005 item 4a; ruling 27); local rays go to `intersect_subsurface` |
| `advance_geodesic` | main | CUDA kernel: integrates the geodesic (0006 D22), stepping and marching media in-kernel while a chord cannot touch scene geometry, and emits a chord as a ray segment for `intersect_closest` only when it overlaps the scene bounds; every curved-ray result, hit or miss, returns here (rulings 18, 46). Geodesic scalar float |
| `advance_geodesic_f64` | main | the same stage function with the geodesic scalar double (D1's scalar template parameter): science renders and S1's double variant; S1's mixed variant, if chosen, is this TU calling the float instantiation outside r_mix (0006 D37, D40). Own `launch_config.h` and `budget.toml` rows; the only GPU kernel allowed FP64 instructions. The host launches one of the two per session for `ADVANCE_GEODESIC` |
| `k_route_hit` | test | `route_hit` compiled as a plain CUDA kernel for the fixture test and register gate |

**`Stage` enum** (u8, the `next_stage` values, in launch order; ruling 47): `INTERSECT_CLOSEST`, `INTERSECT_SUBSURFACE`, `INTERSECT_SHADOW` (shadow pool), `SHADE_BACKGROUND`, `SHADE_LIGHT`, `SHADE_SURFACE`, `SHADE_SURFACE_EXT`, `SHADE_HAIR` (only if split), `SHADE_LIGHT_PATH`, `SHADE_LIGHT_PATH_EXT`, `SHADE_LIGHT_PATH_HAIR` (only if split), `SHADE_SHADOW` (shadow pool), `SHADE_VOLUME`, `SHADE_SUBSURFACE`, `ADVANCE_GEODESIC`, `TERMINATED` (main pool), `SHADOW_TERMINATED` (shadow pool). `generate_camera` and `generate_light` are refill launches over the main `TERMINATED` segment (D7 step 4), and the partition and sort kernels are scheduler launches, so none of them is a `Stage` value.

Per-pass and scene-build kernels, launched by the host outside the step loop, under D1-D4 and D10:

| Kernel | Semantics | Owns |
|---|---|---|
| `film_resolve` | 0008 | per-sample buffer and splat plane into the fp64 moments, one writer per pixel, no atomics; clears both (ruling 1) |
| `film_adaptive` | 0008, 0004 D16 | convergence test and mask dilation |
| `film_present` | 0008, 0007 | display conversion to the half-float present buffer |
| `film_heatmap` | 0008 | statistic to display-referred RGBA8 |
| `film_prepare_denoise` | 0008 | denoiser inputs: colour with negatives clamped, albedo and normal guides into the denoiser's buffers (0008 decisions 15-16) |
| `film_pack_dlss` | 0008 | guides into DLSS inputs |
| `bake_environment` | 0004 D11 | world program on the equal-area grid for the environment importance table; a VM kernel for D10's stack class |

Light trees and alias tables (0004), sky tables (0004 D12), majorant grids (0006) and grating tables (0005) are built on the host. A record that moves one to the device adds a row here.

**Light paths in the scheduler.** `route_hit` sends a light-path hit to `shade_light_path` (or `_ext`), a lamp hit to `shade_light` (pass-through) and a light-path miss to `TERMINATED`. Light paths share the main pool, the refill rule and the end-of-pass rule (D7); their camera connections take shadow slots like NEE items. A scheduler test runs a stub `generate_light` whose paths terminate at the first hit; the stub is test-only.

**Specialisation.** The only compile-time parameters are D1's scalar type (used to build `advance_geodesic_f64`) and the kernel identity `ShadeKind`, a flat closed enum: `Common`, `Ext`, `Light`, `LightExt` (plus `Hair` and `LightHair` if 0005's split is measured). Each value is instantiated in exactly one TU and has its own `launch_config.h` and `budget.toml` rows; values are never formed as products of template axes. The enum grows by one entry, one TU and two rows when 0005 or 0004 justify a new identity. The `Common` kernel contains no extension or light-path code. Feature switches are runtime fields in `KernelGlobals`, uniform per launch.

**D9. The fused surface kernel and the rule for splitting.** `shade_surface` is one kernel that runs the node VM in-kernel (spike A: the fused VM about 30% faster than a separate parameter pass plus split), BSDF sample and eval, and NEE.
- Production launch bounds `(256, 2)`, i.e. at most 128 registers (MEASURED on the fused kernel without VM: 148 to 128 registers, 56% faster than uncapped, 68 B spill stores). Other shade identities start with the same bounds. All other kernels start at 256 threads with no minimum-blocks clause until a benchmark sets one (PROVISIONAL).
- **Pre-split by structure** (spine §2): extension identities, light paths, lamp crossings, subsurface, volumes, shadow evaluation, geodesic stepping.
- **Every other split or fusion** lands only with an A/B on the benchmark scene set at 1080p 1 spp and 64 spp, measured with 0009's wall-time A/B protocol (decision 18). Pass criteria:
  - as a performance change: at least 5% gain at both sample counts;
  - as a remedy for a budget breach (D10 ladder): fastest of the measured alternatives, including re-tuning the cap.
  - There is no register-count trigger: under a cap, growth shows as spills, which D10 gates.

### Budgets and how the gate measures them

**D10. Per-kernel resource budgets.** Gated on sm_120. The baseline lives in `src/kernel/budget.toml`: per kernel or OptiX program, T, B, registers, stack, spill allowance, and whether it is gated per merge or in `check full`. The benchmark report plots it (0009 decision 22).

| Budget | Limit | Status |
|---|---|---|
| Production cubin: registers ≤ 65536/(T·B) for every kernel with B set | must hold | launch bounds honoured on 12.8 non-rdc (spike A) |
| Production spill stores/loads | allowance in `budget.toml`: 0 by default; VM kernels ≤ 128 / 192 B | ruling 17; PROVISIONAL (68/112 measured without VM; fused+VM capped unmeasured) |
| Production stack: VM kernels / all others | ≤ 512 B / ≤ 128 B | PROVISIONAL (fused+VM 304 B, others 32 B measured) |

**VM kernels** are the kernels that run the node VM and so hold its register file: every kernel 0005 item 32 lists (the `ShadeKind` identities, `shade_shadow`, `shade_background`, `shade_light`, `shade_volume`, `generate_light`), `bake_environment` and both `advance_geodesic` kernels, whose marcher runs volume programs (0006 D9). Every other kernel is in the second class.
| Growth vs `budget.toml` | +5% registers (kernels without B) or +10% stack fails unless the change updates the baseline | PROVISIONAL |
| Uncapped registers (probe build, `check full`) | warning at 150, loud warning above 168; never a merge failure | ruling 17 |

**Time budgets are 0009's** (G2, ruling 29); this design must meet, and the gate measures, these rows there: B-05 compile time per device TU; B-01 and B-02 clean and incremental build; B-04 dev kernel edit to running engine; B-18 CUDA context and modules ready, warm and cold; B-19 GPU idle and `queue_build` + sort shares of a 1080p 1 spp pass; B-20 kernel time per 1080p sample; B-17 launch duration (ruling 14); B-09 the kernel share of `check gate`.

**What the merge gate measures for kernels** (serially, never two GPU builds at once):
- **Production device build** (`-gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xptxas -v -c`, changed TUs). Registers and stack come from `cuobjdump -res-usage`, spills from the `ptxas -v` lines; with no `-rdc` the compile-time count is final.
- **Agreement build** (`--fmad=false`, changed TUs) and both CPU kernel builds (f32 and f64).
- **Fixture and agreement suites** (D12), 10^6 inputs per stage (CPU 1.7-2.9 M evaluations/s per thread, spike A §7).
- **Compile timing:** the at most 3 changed TUs with the largest baseline times, each by wall clock around one serial nvcc call with the compiler cache off and the OS file cache warm. A result within 10% of the limit is re-measured as the minimum of 3.

**`check full`** (run on demand, never scheduled, G3) adds: the probe build (`AR_LAUNCH_BOUNDS=0`) for uncapped registers; compile timing of every TU; a compile-only budget check of every SASS architecture the release ships (D14; 0001 V13); the OptiX modules' registers via Nsight Compute (`launch__registers_per_thread`); and traversal rays/s on a fixed scene through 0009's wall-time A/B. The OptiX modules have no per-merge register gate because the driver compiles them. Their non-traversal code is limited to `route_hit` and `ah_record` (D11); `route_hit` is register-gated per merge through `k_route_hit`.

**Growth toward full scope.** Spike A is a lower bound. Growth is handled four ways:
1. *Front-load.* The first `shade_surface` contains every register-heavy part before breadth work: in-kernel VM, Jakob-Hanika upsampling lookups, texture sampling, the `LightSampler` interface with tree-shaped traversal, environment sampling, emission MIS and first-hit buffer writes.
2. *Watch.* Every merge records production spills and stack; `check full` records uncapped registers; the report plots both over time.
3. *Declare.* Each task spec touching a shade kernel states its expected production spill and stack delta; the gate prints the actual delta.
4. *Fallback ladder,* triggered by a production spill or stack breach or by a confirmed throughput regression. Follow it in order, measuring before each next step:
   1. Attribute the growth by stubbing features and rebuilding (v1's method; the probe build shows the live-state delta).
   2. Fix it locally: `const&` for any struct copy, recompute cheap values instead of keeping them live, narrow stored types.
   3. Re-tune the cap: A/B (D9) the kernel at register caps of 168, 128, 96 and 64 via `(T, B)` rows. Ship the fastest and record its spills as the new allowance. Spike A: the 64-register cap with 472 B of spills still beat uncapped.
   4. Move a rare feature to its own queue and identity; it must beat step 3's best.
   5. Split light selection into a `sample_light` stage, or split into `shade_surface_bsdf` + `shade_surface_nee`; it must beat step 3's best. Spike A measured the split pair 22% slower than the capped fused kernel.

   Never: template variant matrices, `__noinline__`, `-rdc`, or 8 wavelengths (197 registers measured).

### OptiX, compilation, memory, failures

**D11. OptiX is traversal only; no material, medium or VM code in OptiX programs (ruling 18).** One module, `src/kernel_gpu/optix_traverse.cu`, and one pipeline per device. When the scene has transform motion (0007 D15), a motion variant of the same module and pipeline (motion blur enabled, `ALLOW_MOTION` traversable flag) is created as well, only for such scenes.

| Program | Entry | Role |
|---|---|---|
| raygen | `__raygen__intersect_closest` | over its segment: `optixTrace`, write the hit record (t, primitive, instance, u, v), run `route_hit` |
| raygen | `__raygen__intersect_subsurface` | local rays traced directly against the hit instance's GAS in object space; writes the hit record |
| raygen | `__raygen__intersect_shadow` | over its segment: `TERMINATE_ON_FIRST_HIT \| DISABLE_CLOSESTHIT`; writes the outcome |
| miss | `__miss__traverse` | sets instance to `~0u` |
| closest-hit | `__closesthit__{tri,curve,sphere,lamp}` | write t, primitive, instance, u, v; the primitive type follows from the instance's GAS (one build-input type per GAS) |
| any-hit | `__anyhit__record` (`ah_record`) on the tri, curve and sphere hit groups | shadow rays only, below |
| intersection | built-in curve and sphere modules (`optixBuiltinISModuleGet`); `__intersection__lamp` | curves (hair), spheres (points, sphere lamps); `__intersection__lamp` is a purely geometric disk/ellipse test for area lamps (0004 D8; Embree: user geometry). Rectangle lamps are two triangles. |

- **`route_hit`** is a table-driven function: primitive type and per-primitive material id → material and lamp flags → `Stage`, plus `path_kind`. Lamp hits go to `shade_light`, extension materials to the `_ext` identities. When a curved region exists it also applies 0003 D28's launch-uniform clamp of tmax at the region sphere, routing entering rays to `advance_geodesic`. Volume-stack updates and all VM, texture and BSDF code run in CUDA stages.
- **`ah_record`** (Cycles pattern, `__anyhit__kernel_optix_shadow_all_hit` in `kernel/device/optix/kernel.cu`). If the primitive's material is opaque, it accepts the hit, which terminates the ray as occluded. Otherwise it keeps the K nearest hits (t, primitive, instance, u, v), ties broken by (t, primitive, instance) so the result does not depend on traversal order, sets an overflow flag beyond K, and ignores the intersection. K = 4 (PROVISIONAL, set by the foliage benchmark). It runs no VM or texture code. The CPU uses the same `record_shadow_hit` function in an Embree occlusion filter.
- **Shadow rule, one pass:** instances whose materials are all opaque (0005's `NON_OPAQUE` flag clear) carry `OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT`, so in opaque scenes `ah_record` never runs. Outcomes:
  - occluded → item terminated;
  - miss with no records → deliver (D5);
  - miss with records → `shade_shadow` evaluates them in t order and delivers, or on overflow re-queues `intersect_shadow` from the K-th t.
  - Rays through more than K non-opaque layers cost one extra step per K layers.
  - Delivery is never done in `__raygen__intersect_shadow`: it writes `SHADE_SHADOW` for an unoccluded item and `shade_shadow` adds the contribution to the target slot and frees the item, with no VM and no recorded hits in M1a (task 074), until M2 extends it with `ah_record`'s hits and opacity.
- **Camera, indirect and subsurface rays** use `DISABLE_ANYHIT`. An alpha or Transparent-BSDF hit is resolved in `shade_surface` as a pass-through that re-queues `intersect_closest` from the hit, one step per layer, bounded by a transparent-bounce limit (0004). Cycles handles camera-ray transparency the same way.
- v1's opaque-only design (`TERMINATE_ON_FIRST_HIT | DISABLE_CLOSESTHIT | DISABLE_ANYHIT`) covered all-triangle opaque scenes; v1 kept alpha-shadow walks on a software BVH (`gpu-build.md:73`). It is the opaque special case here, not a precedent for the whole rule.
- **Instance visibility mask (8 bits, owned here).** A ray traces with the bit of its ray type, OR `MEDIUM_BOUNDARY` when the scene has media; an instance is hit when the masks share a bit.

  | Bit | Name | Set on instances | Traced by |
  |---|---|---|---|
  | 0 | `CAMERA` | visible to camera rays (Blender ray visibility) | camera rays |
  | 1 | `DIFFUSE` | visible to diffuse rays | rays after a diffuse sample |
  | 2 | `GLOSSY` | visible to glossy rays | rays after a glossy or delta reflection |
  | 3 | `TRANSMISSION` | visible to transmission rays | rays after a transmission sample |
  | 4 | `SHADOW` | casting shadows; never lamps (0004 D8) | shadow items: NEE and camera connections |
  | 5 | `VOLUME` | visible to volume-scatter rays | rays after a medium scatter |
  | 6 | `MEDIUM_BOUNDARY` | volume-object hulls and every medium-bearing mesh (0006 D16, D20), whatever its ray visibility | every ray when the scene has media; a hit through this bit alone (the instance lacks the ray type's bit) is a false interface: stack update only, no shading (0006 D17) |
  | 7 | unallocated | — | assigning it amends this record |

  Lamp instances carry the light's own ray-visibility bits (0004 D8). Holdout and shadow catcher are per-instance shading flags read by shade stages, and non-opacity is the any-hit instance flag above; none of them is a mask bit.
- **SBT:** one record per program; hit groups per primitive type (tri, curve, sphere, lamp), never per material.
- **Pipeline options:** traversable graph flags `ALLOW_SINGLE_LEVEL_INSTANCING | ALLOW_SINGLE_GAS` (the second for `intersect_subsurface`; verify, Risks 5), primitive types triangle, round curves, sphere and custom, `maxTraceDepth = 1`, no SER.
- **Format: PTX** from nvcc. Spike A measured PTX and OptiX-IR equal within 7% in cold load time and 2% in launch time, and Cycles ships PTX.
- Module and pipeline creation start asynchronously when the engine instance is created, in parallel with scene sync, and use OptiX's disk cache (warm and cold: 0009 B-18) in the directory the host supplies (0007 D45).

**D12. FMA and determinism policy.**
- **Production GPU:** `--fmad=true`, no `--use_fast_math`, `-prec-div=true -prec-sqrt=true -ftz=false` (spike A's numbers were measured this way). **Production CPU:** `/O2 /arch:AVX2 /fp:precise` (AVX2 required, ruling 15), no `/fp:contract`. An explicit `ar::fma` is allowed: it is correctly rounded on both targets.
- **Agreement build:** GPU `--fmad=false`. Per-stage tests run on 10^6 fixed host-generated inputs (spike A protocol) and compare 4-wavelength relative L2 error. Thresholds sit about 3-10x above spike A's measurements:
  - *No-FMA GPU vs CPU float:* eval f max ≤ 1e-5 (measured 1.1e-6); throughput p99.9 ≤ 1e-4 (1.4e-5); direction p99.9 ≤ 1e-5 rad (1.2e-6); lobe/light choice disagreement ≤ 1e-5 of inputs (0).
  - *Production GPU vs CPU f64:* throughput p99.9 ≤ 5e-4 (1.3e-4); eval p99.9 ≤ 1e-4 (2.7e-5); direction p99.9 ≤ 1e-5 rad (3.7e-6).
  - *Outliers:* every max is printed; fewer than 0.01% of inputs may exceed 10x a p99.9 threshold.
  - Raw f at a sampled direction is never gated (ill-conditioned for near-delta lobes, spike A).
- **Determinism:**
  - Random numbers come only from 0003's counter-based sampler (ruling 3), keyed on the sample's identity. A path's computation never depends on queue position, slot, thread count or sort order.
  - Queues, shadow-slot assignment (D5) and any-hit records (D11) are deterministic; per-item stage outputs are bit-identical run to run on the same device and build.
  - Contributions reach 0008's per-sample buffer by fp32 atomic add, so per-sample totals on the GPU and on the multithreaded CPU are reproducible up to summation order (about 1e-6 relative; v1 measured this). The resolve into the fp64 moments has one writer per pixel and no atomics (ruling 1). Light-tracing splats (ruling 2) are reproducible only up to atomic order. CPU at `threads=1` is bit-exact run to run.
  - Bit-exact GPU images are not offered (owner question 2).

**D13. Memory layout mechanics.**
- **Path state and shadow items:** 0003 owns the field lists, sizes and the 256 B / 128 B budgets (ruling 9). This record provides the storage mechanism: one X-macro list (Cycles `state_template.h` pattern) declares every field with type and feature tag; fields whose feature is off are not allocated. Each field is its own array, 256-B aligned. 4-wide spectra are `float4` (one 16-B load; double4-equivalent in f64). The fields this record needs are listed in D5, D6 and D11 (`next_stage`, `path_kind`, the delivery target, `ray_tmax`, and the `ah_record` side array of K × 20 B plus a count, allocated only when the scene has non-opaque instances).
- **Scene data:** flat arrays read through `ar::load_ro`; material records fixed-size and 16-B aligned (0005); upsampling tables 9.4 MB per colour space. Textures are linear memory, device-resident or host-mapped pitch-linear (0007's spill step), read with `ar::load_texel`, with filter math, mipmaps included, in shared kernel code (0005 item 34, ruling 22). Hardware filtering is not used: its 8-bit fractional weights (CUDA Programming Guide, "Texture Fetching") would differ from the CPU. 0005 may revisit with a measurement.
- **Accounting:** every device allocation goes through 0007's VRAM ledger, which owns the categories, the reserve rule and the budget table (ruling 19). Pool capacities derive from that table (D5).

**D14. Compilation route (one compiler, ruling 16).** All GPU code is loaded as module images through the CUDA driver API (`cuModuleLoadData`, `cuLaunchKernel`); the host never uses `<<<>>>`.
- nvcc builds one cubin per stage TU for sm_120. Release builds add 0001 D3.4's architecture list as SASS plus the newest PTX. Release modules are embedded in `astroray_core.dll` (ruling 13). Every shipped SASS architecture passes the compile-only budget check in `check full` (registers, spills, stack against `budget.toml`); runtime behaviour is tested on sm_120 only, and D15 warns on any other device. Banned flags: `--Ofast-compile` (spike A: 222 registers); `--split-compile` (148 to 162 registers); `--use_fast_math`; `-rdc`.
- Development builds may load modules from `ASTRORAY_KERNEL_DIR` (0009 decision 5); a changed module is swapped at the next step boundary and accumulation resets. Release builds never load loose modules.
- NVRTC is not used; it returns only if the measured edit loop exceeds 5 s (ruling 16). The CPU target has no hot reload; it rebuilds within 0009 B-08 (spike A).

**D15. Device failure contract (rule 7).** Every CUDA and OptiX call is checked. Every failure reaches the C ABI as an `ar_status` and an entry in 0001's per-render report with the text below, and the addon shows it as a Blender report. The engine never switches device on its own.

| Case | Detected at | Effect |
|---|---|---|
| Allocation failure | allocator | render fails; message names category, size, free VRAM |
| Dev module in `ASTRORAY_KERNEL_DIR` fails to load (dev only) | module loader | previous image of that stage keeps running; persistent viewport warning with the loader error and module name until it loads |
| OptiX module or pipeline failure | device init | GPU device unavailable for the session; render fails with the OptiX log |
| Driver too old, unsupported GPU | device enumeration and context creation (`cuDriverGetVersion` vs the build's minimum; compute capability) | context refused with `AR_ERR_DEVICE` (0001 D2.8); message gives found and required driver and the GPU architecture |
| No module image for this GPU | device init (module-load result) | GPU device unavailable; the scene's sessions fail with `AR_ERR_DEVICE` (0001 D2.8), message names the GPU architecture |
| Sticky CUDA error (illegal address, launch failure, ECC) | the per-step event wait | GPU device dead for the process; render fails naming stage and step; message says Blender must be restarted to use the GPU (CUDA requires a process restart); CPU device stays usable |
| Other launch error | the per-step event wait | render fails naming stage and step |
| Asynchronous device init fails during scene sync | init worker | error posted when it happens; the first device upload re-raises it; host scene data is kept |
| GPU architecture other than sm_120, or PTX JIT used | device init | warning once per session ("not covered by runtime tests"; JIT seconds if JIT ran); render proceeds |

Debug builds wait after every launch so an error names its stage.

**D16. Self-intersection (triangles and lamps).** Task 038; `src/kernel/geom/ray_offset.h`, one source for CPU and GPU (float only: Embree and OptiX traverse in float, C3).
- Every ray traces with `tmin = 0`. A spawned ray's origin comes from `offset_ray_origin(p, ng, side)`: Waechter and Binder 2019 (Ray Tracing Gems ch. 6, section 6.2), which moves the origin along the unit geometric normal by int(256·|n_i|) float ulps per component (a fixed 1/65536 along the normal for components below 1/32), to the side of the outgoing direction. It is used for continuation rays, the origin end of NEE shadow rays, and lamp pass-through re-queues (0004 D8), which offset along the lamp's geometric normal toward the ray direction (the direction itself as normal can leave a grazing crossing on the incoming side at large coordinates, where per-component steps differ, and the lamp would count twice). No hit carries an error bound.
- A shadow ray from a shading point to a point on an emitting triangle offsets both ends toward each other and traces `[0, tmax]` with `tmax` the distance between the offset points. A lamp point needs no far-end offset because lamp instances lack the SHADOW bit (D11).
- Considered and not chosen: pbrt-v4 `OffsetRayOrigin` (per-hit floating-point error bounds, Apache-2.0). It would add an error bound to the hit record and to both traversal backends; the ulp offset needs only the hit point and the geometric normal and scales with the distance from the origin. Cycles' primitive self-skip is rejected because it needs any-hit on rays D11 traces without it.
- Shadow terminator: no correction in M1a. Hanika 2021 (Ray Tracing Gems II ch. 4) and Chiang et al. 2019 are decided in M3 with bump and normal maps; until then a smooth-normal coarse mesh can show the classic terminator artefact, and the reviewer looks for it.
- Known limit: the offset is 256 ulps, so two surfaces closer than that (about 6e-5 m at 3 m from the origin, more farther out) can be stepped over by a ray leaving one toward the other. `ray_offset/near surface found` pins the 1e-4 m case at 0 m and 3 m.
- Test: `tests/cpu/test_ray_offset.cpp` (10^6 rays from a 10^5-triangle sphere 1e4 m away: no own-triangle hit, no start below the surface).

## Alternatives considered

- **Megakernel, with or without SER.** Traversal only exists inside OptiX programs, so a megakernel would put the whole integrator into a driver-compiled raygen (spike A: about 1 s cold for the fused body alone), sized by the union of all features (v1: 254 registers), with sorting replaced by SER, which is unmeasured here and has an open crash report (research 08 §6). Against it: Laine et al. 2013; pbrt-v4 ch. 15; Cycles X; one preprint at 14% for wavefront (research 04 §2). Not chosen; the overhead risk is tracked in D7 and Risks 2.
- **Split surface kernels as the default.** Measured 22% slower than the capped fused kernel (spike A §3). Kept only as ladder step 5.
- **A hard uncapped register gate (spine §2 before ruling 17).** Would force splits on an uncapped number that does not predict shipped speed (C2).
- **`__noinline__` lobes or callables.** -37% (N=4), -42% (N=8), stack up to 1.4 KB (spike A).
- **Template variants or per-material kernels.** v1's 128-variant TU took 1,800 s. With a single OpenPBR record the per-type axis disappears (research 04 B). The `ShadeKind` enum is one flat axis, one TU per value.
- **A template parameter for the wavelength count.** It has no user once N = 4 is fixed (0003) and 8 is banned.
- **8 wavelengths.** 197 registers uncapped, -41% capped throughput (spike A).
- **Append queues or atomic bump allocation for shadow slots** (pbrt, Cycles). They work on both targets (`ar::atomic_add` exists on the CPU), but slot order would depend on atomic order. That makes shadow-pool layout and the permutation test nondeterministic and needs a compaction kernel. The reservation in D5 costs no extra kernel.
- **Transparent shadows with no any-hit, one step per layer** (the earlier draft). Foliage can put tens of layers on one shadow ray, and every step carries a partition and a round trip.
- **Alpha evaluation inside any-hit** (pbrt `gpu/optix/optix.cu`). Puts texture or VM code in driver-compiled programs with no register gate.
- **Geodesic stepping as an OptiX raygen in a second module** (0006's earlier draft). Puts integrator and medium code in driver-compiled programs; ruling 18 keeps `advance_geodesic` a CUDA kernel.
- **`-rdc` with device linking.** v1 paid 50-75 s per serial link; compile-time register counts came out up to 5x wrong.
- **OptiX-IR modules.** Measured equal to PTX. Revisit behind one switch if a measurement shows a gain.
- **`--use_fast_math`** (Cycles and pbrt). Loosens division, sqrt and denormals and widens the CPU/GPU gap; the budgets were measured without it.
- **Slang, Vulkan RT, Dr.Jit.** Rejected in research 03.
- **Hardware texture filtering.** A second, lower-precision filter the CPU cannot reproduce.
- **NVRTC dev loop** (spine §2 before ruling 16). Saves about 1 s per edit at the cost of a second compile path and an identity test.
- **CUDA runtime launches (`<<<>>>`).** Embedded module images need the driver API anyway; one launch path.
- **Silent CPU fallback on device failure.** Violates rule 7.

## Consequences

- **Easy:** a kernel edit costs one nvcc TU (within 0009 B-05) plus a module swap; a new stage means one enum value, one header, one `.cu`, one `.cpp` and two table rows; the CPU runs the same stage functions and step-table logic, so CPU/GPU differences reduce to arithmetic and traversal edge cases; spill, stack and compile drift is visible on every merge; each stage can be unit-tested on fixtures on the CPU.
- **Hard:** every feature must fit a stage boundary and route through `next_stage`; state crosses global memory between stages (bandwidth unmeasured); agents cannot reach for STL, virtuals, recursion or fast-math; camera rays cost one step per alpha layer and per lamp crossed, shadow rays one extra step per K non-opaque layers; host scheduling costs a round trip per step, estimated above budget at 1 spp (D7); a GPU fault inside Blender's process needs a Blender restart.
- **Cost of unused features:** an empty queue costs one partition bucket and no launch; a feature-tagged state field costs no memory; a runtime flag costs a uniform branch plus its share of the shade kernel's spills, which the gate counts; light-path support costs one state bit, routing entries and one work-counter range; the motion pipeline variant costs nothing in scenes without motion; the f64 CPU build costs one CPU compile; the any-hit costs nothing in opaque scenes (instance flag).

## What must exist first, and what can come later

Milestones are `docs/ROADMAP.md`'s (G1).

**Must exist from the first milestone, because retrofitting is expensive:**
- D1-D4 with the lint: every kernel written before them would be rewritten.
- `KernelGlobals`, the state X-macro and allocation through 0007's ledger: they are in every stage's signature and every allocation path.
- `queue_build` with the step table and shadow-slot reservation, and the sort: every stage reads its segment from the step table.
- `next_stage` on both pools, `path_kind`, light-path routing, the work counter over camera then light samples, and the stub-`generate_light` scheduler test: a second path kind added later changes the state layout and every routing table.
- Self-contained shadow items delivering into 0008's per-sample buffer: moving accumulation later changes the path state and every shade stage (ruling 1).
- `intersect_closest`, `intersect_shadow` (opaque), `route_hit` and `k_route_hit` on OptiX and Embree, and the CPU driver: the CPU is the oracle for every later stage.
- `shade_surface` front-loaded (D10) with alpha pass-through, plus `generate_camera` and `shade_background`: register growth found after breadth work forces the ladder onto finished stages.
- The merge gate with `budget.toml`, the agreement build and its tests: drift before the gate exists goes undetected (v1).
- Asynchronous device init and the D15 contract with failure-injection tests: failure paths added late are found by users.
- Measured before breadth work: `shade_surface` capped at `(256,2)`, and scheduling overhead on the spike B scenes and an alpha-foliage scene (steps per pass, GPU idle, partition time). If 0009 B-19 is breached, remedy (a) of D7 lands before breadth work, while host scheduling is still small.

**Can be added later without redesign** (each is one enum value, one TU and two table rows, or a host-side change):
- `shade_surface_ext`, `shade_light_path_ext`, `shade_volume`, `shade_subsurface` with `intersect_subsurface`, `shade_light` with the lamp primitives, the real `generate_light` and `shade_light_path`, each with measured budget rows.
- Transparent shadows (`ah_record`, `shade_shadow`, the hit-record array) with K set on the foliage scene.
- OptiX curves and spheres; the motion pipeline variant.
- `advance_geodesic` and the curved-region clamp in `route_hit`.
- `bake_environment` and the film kernels as their features land.
- Release images for 0001 D3.4's architectures with the compile-only check.
- D7 remedies (b) and (c); an SER or fused-composition experiment; a second traversal backend if a vendor port is ever approved.

## Verification

| Decision | Check | Pass |
|---|---|---|
| D1-D3 | lint; `/EHs-c- /GR- /WX` CPU build; ptxas double-use warnings as errors | zero findings; no GPU kernel except `advance_geodesic_f64` has FP64 instructions |
| D4 | per-stage fixture test: same inputs through the CPU and GPU (no-FMA) builds | D12 thresholds |
| D5 | shadow pool sized below demand on a test scene | no item dropped; film equals the large-pool run within 1e-6 relative |
| D6-D7 | debug flag that randomly permutes indices within each segment before each launch (pool large enough that nothing is deferred) | main-pool state bit-identical to the unpermuted run; shadow pool equal as a multiset (items sorted by bytes) |
| D7 | 1080p 1 spp pass on the spike B scenes and the foliage scene; Nsight Systems timeline | 0009 B-19 and B-17 |
| D8 | each kernel has a CPU fixture test and rows in `launch_config.h` and `budget.toml`; OptiX programs have `check full` rows | the gate fails on any CUDA kernel or OptiX program missing from either table |
| D8 | stub light-path scheduler test | every light path terminates; pass ends; camera image unchanged |
| D9 | any split or fuse change carries an A/B report | D9 criteria |
| D10 | merge gate and `check full` as described | all limits; growth rule; kernel part of the gate within 0009 B-09 |
| D11 | traversal agreement: Embree (`RTC_SCENE_FLAG_ROBUST`) vs OptiX on 10^6 fixed rays per canonical scene; transparent-shadow analytic test with n layers of opacity a for n = 1, K and 3K+1; mask fixture with one instance per bit traced by every ray type | hit/miss mismatch ≤ 1e-4 of rays, all within 1e-4 relative of a triangle edge (watertight-edge mismatches are expected, v1); transmittance (1-a)^n within 3σ; each ray type hits exactly the instances whose bits it traces |
| D12 | agreement suite; CPU `threads=1` twice gives bit-identical film; GPU twice gives bit-identical state dumps and film within 1e-6 relative | thresholds as listed |
| D13 | after scene load: drop in `cuMemGetInfo` free memory vs the ledger total | within 64 MiB |
| D14 | cold and warm start timing; per-architecture compile-only check; dev module swap at a step boundary | 0009 B-18 and B-04; D10 limits; accumulation resets, no stale module runs |
| D15 | injection per case: corrupted dev module in `ASTRORAY_KERNEL_DIR`; corrupted PTX; mocked old driver version; failing init worker; a debug-only out-of-bounds kernel run in a child process | each case produces its `ar_status` and report entry; nothing renders silently |

## Risks and open items

1. **Full-scope register growth** remains the central risk. Fused + VM measured 150 uncapped with **no capped measurement**; it must be measured under `(256,2)` before breadth work. Under ruling 17 a breach shows as spills, and the D10 ladder applies.
2. **Spike C was not run.** CHECKPOINT-1 listed it as required before the architecture is written. This record replaces it with the D7 estimate, which puts scheduling overhead of a 1 spp 1080p pass above 0009 B-19. Overhead is measured before breadth work, and remedy (a) is host-only. **The lead must decide whether that is acceptable or whether spike C runs before sign-off.**
3. **State bandwidth.** Bytes per path and DRAM traffic per pass are estimates (research 04 S3); measure with Nsight Compute before breadth work.
4. **OptiX module cold compile** (traversal + `route_hit` + `ah_record`, twice in scenes with motion) is unmeasured.
5. **Direct GAS traversal** for `intersect_subsurface` assumes `ALLOW_SINGLE_GAS` can be combined with single-level instancing in one pipeline. Verify before `shade_subsurface`. Fallback: a second any-hit filtering on instance id, which is integer-only and consistent with D11.
6. **Toolkit change.** The numbers are from CUDA 12.8; 0009 may pin 13.x. The gate re-baselines on the toolkit-change commit, and only there.
7. **PROVISIONAL values:** sort threshold, pool and shadow capacities, K, launch configs other than `shade_surface`, stack limits and spill allowances. All are set by build-phase measurement.
8. **Software texture filtering cost** is unmeasured (v1: texture and upsampling work was 14% of samples).
9. **The flag name `--warn-on-double-precision-use`** needs checking against the pinned ptxas.
10. **Nsight counters** need a one-time administrator enable (owner question 1).
11. **Sticky CUDA errors** require a process restart, so a kernel fault ends GPU rendering for the Blender session (Cycles has the same property). This follows from 0001's in-process engine; an out-of-process engine is the only remedy (CHECKPOINT-1 keeps it as a fallback).
12. **Untested architectures.** The release SASS list covers GPUs that are never runtime-tested; the owner accepts building them labelled untested (owner decision 29), so D14/D15 warn rather than refuse.
13. **Self-intersection for curves, spheres and the random walk is unspecified.** D16 fixes triangles and lamps. Curves (0005 item 27's HAIR), spheres as geometry and the random walk's local rays need their own rule (M7, per the roadmap's designed-when-built table); D16's ulp offset is the first candidate.

## Assumptions about other records

- **0001:** the engine is an in-process C-ABI DLL, Windows and NVIDIA only; every function returns `ar_status`; 0001 owns the per-render report (D15 posts to it, D5 states pool capacity in it) and the release architecture list (D3.4). The CPU target can render.
- **0003:** owns the path state and shadow item layouts and their budgets, including the fields D13 lists; the sampler (ruling 3); packet operations and the termination enum (ruling 6); the flat spacetime policy and the curved-region clamp `route_hit` calls.
- **0004:** owns stage semantics on D5-D8: NEE/MIS, light tree, lamp crossings, transparent-bounce limit, adaptive sampling, and the light-pass method with its light-sample count per pass. It may need further `ShadeKind` values, and may need `k > 1` shadow items per item.
- **0005:** a fixed-size OpenPBR record, the VM in-kernel, the closed `ext_kind` list mapped onto `ShadeKind::Ext`/`LightExt` (and `Hair` if split), the per-material `NON_OPAQUE` flag that sets the any-hit instance flag (not a mask bit), and texture filtering with mipmaps in kernel code.
- **0006:** geodesic chords go through `intersect_closest` from the CUDA kernel `advance_geodesic` (ruling 18); volume hulls and medium-bearing meshes carry `MEDIUM_BOUNDARY`; S1 decides which `advance_geodesic` kernel the viewport launches (0006 D40); media and geodesics cap work per launch (D7); C3 applies.
- **0007:** triggers device init at engine creation; supplies the module cache directory; owns the VRAM ledger, reserve rule and budget table (ruling 19) and the first-frame gate (ruling 21); uses per-pass timing for its resolution controller; requests the motion pipeline variant.
- **0008:** owns the per-sample buffer, its add function `sample_add` (formerly this record's `film_add`) and its resolve (ruling 1), the splat footprint (ruling 2), and the semantics of the film kernels in D8, under D1-D4.
- **0009:** provides the gate script, the toolkit pin, every time budget D10 lists, the test tiers, and `ASTRORAY_KERNEL_DIR`.

## Sources

- Spike A `spikes/A-kernel-budget/RESULTS.md` (§§2-8); spike B `spikes/B-blender-end-to-end/RESULTS.md` (§3).
- Research `03-kernel-language-backend.md`, `04-integrator-architecture.md` (refill rule, lines 53 and 233), `08-build-toolchain.md`, `17-materials-and-custom-types.md` (architectural consequence; review findings), `10-denoise-upscale.md`.
- `docs/decisions/RULINGS.md` (G1-G4, rulings 1-3, 6, 7, 9, 10, 13-19, 21-23).
- v1: `v1-lessons/research/gpu-build.md` (lines 37-73), `v1-lessons/toolchain-and-build.md`, `v1-lessons/research/transport.md`.
- Laine, Karras, Aila, "Megakernels Considered Harmful", HPG 2013.
- Pharr, Jakob, Humphreys, *Physically Based Rendering*, 4th ed. (2023), ch. 15.
- Blelloch, "Prefix Sums and Their Applications", CMU-CS-90-190, 1990.
- Woop, Benthin, Wald, "Watertight Ray/Triangle Intersection", JCGT 2013.
- Wald et al., "Embree", ACM TOG 33(4), 2014.
- Meister et al., "On Ray Reordering Techniques for Faster GPU Ray Tracing", I3D 2020.
- Padilla, Webster, Kim, arXiv 2605.27323 (preprint, wavefront vs megakernel).
- Cycles (Apache-2.0): `kernel/device/cuda/config.h`, `compat.h`; `kernel/device/gpu/kernel.h`; `kernel/device/optix/kernel.cu` (shadow any-hit recording); `kernel/integrator/state_template.h`, `shade_surface.h`, `shade_light.h`; `integrator/path_trace_work_gpu.cpp` (refill, `compact_main_paths`); `device/gpu/parallel_prefix_sum.h`, `parallel_active_index.h`, `parallel_sorted_index.h`.
- pbrt-v4 (Apache-2.0): `src/pbrt/pbrt.h`, `wavefront/integrator.h`, `wavefront/workitems.h`, `gpu/optix/optix.cu`.
- NVIDIA: OptiX 9.1 Programming Guide (built-in curve and sphere intersection, motion traversables); CUDA Programming Guide ("Texture Fetching"); CUDA Driver API `CUresult` (sticky errors require a process restart).

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ (for example "nightly", now `check full` on demand).

1. BLOCKER, D10 gate and ladder: **ACCEPTED.** Added challenge C2 (it absorbs the former C2 and is the reviewer's C4). The merge gate now checks production spills and stack, and uncapped registers are a nightly warning. The ladder re-tunes the cap before any split, and every split must beat the re-tuned cap.
2. MAJOR, D8 extension stage: **ACCEPTED.** `ShadeKind` is now a flat enum that can grow, one TU per value. 0005 partitions the extension kinds into identities.
3. MAJOR, D5/D6 shadow pool: **ACCEPTED.** Atomic slot allocation is replaced by per-producer reservation from the deterministic free list, computed in the step table. Producers that run short of slots launch on a prefix, so D7 step 5 is consistent. The permutation test compares shadow items as a multiset. `shadow_compact` is removed. The Alternatives entry is corrected.
4. MAJOR, D11 any-hit: **ACCEPTED.** The v1 citation is corrected. Shadows use one pass with the Cycles-style recording any-hit (no material code in OptiX), and camera rays pass through alpha in `shade_surface`. The mask bit is replaced by the instance any-hit flag, the pipeline flags are made consistent with Risks 5, and an alpha-foliage benchmark is added to M1.
5. MAJOR, light pass in M1: **ACCEPTED** for mechanics (`path_kind`, routing, shared pool and refill, end-of-pass, a stub scheduler test in M1, and a budget row for the light kernels). The method and the light-sample count stay with 0004, per spine §4.
6. MAJOR, device failures: **ACCEPTED.** Added D15, which gives one contract per case, plus failure-injection tests.
7. MAJOR, gate time: **ACCEPTED.** The record now lists what the merge gate runs, with a 120 s kernel share. The probe, NVRTC identity and all-TU timing moved to nightly. Shipped architectures get a nightly compile-only check and a runtime warning on untested GPUs; whether to refuse them instead is owner question 3.
8. MINOR, refill rule: **ACCEPTED.** Changed to "active < 1/4 of capacity", which is Cycles' rule.
9. MINOR, VRAM arithmetic: **ACCEPTED.** Corrected to about 1.05 GiB, plus 320 MiB of hit records when non-opaque instances exist.
10. MINOR, D9 contradiction: **ACCEPTED.** Removed the "8 registers / minority" trigger. There is now one A/B rule with two pass criteria.
11. MINOR, templates on N: **ACCEPTED.** N is now `constexpr kLambdas = 4`.
12. MINOR, OptiX work and step overhead: **ACCEPTED.** Raygen code is limited to `route_hit` and is gated through `k_route_hit`. OptiX programs get nightly `budget.toml` rows, so the D8 check is consistent. The unsourced "20-40 steps" is replaced by a derivation and an overhead estimate (20-40%, above budget). Partitioning is one pass over both pools' high-water marks. Kernels read counts on the device so that a CUDA graph is a host-only remedy. Whether spike C must run first is a lead question (Risks 2).

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the must-exist-first and can-come-later lists; the D8 "Lands" column and every M1/first-release assignment elsewhere removed.
- G2: time budgets (compile, build, edit loop, context ready, overhead shares, per-sample time, launch duration, gate share) moved to 0009 by name; VRAM reserve and pool arithmetic to 0007; path-state and shadow-item budgets to 0003; A/B protocol to 0009; per-render report to 0001.
- G3: every "nightly" item is now part of `check full`, run on demand.
- G4: the geodesic enum value and routing bit hook deleted; light-path mechanics kept because a second path kind changes the state layout.
- Ruling 1: path state carries no radiance; contributions go to 0008's per-sample buffer (`sample_add`, formerly `film_add`), resolved by `film_resolve` with one writer per pixel; D7, D12 and the 0008 assumption updated.
- Ruling 2: camera-connection shadow items carry a film position splatted over 0008's normalised filter footprint.
- Ruling 3: D12 cites 0003's sampler instead of restating its key.
- Ruling 6: 0002 names kept for kernels and `ShadeKind`; 0005's `SurfaceKind::Ext` and `photon_*` names mapped to `shade_surface_ext`, `shade_light_path(_ext)`, `generate_light`.
- Ruling 7: `generate_light` recomputes light-path wavelengths from the seed.
- Ruling 9: D13 defers field lists and the 256 B / 128 B budgets to 0003.
- Ruling 10: D15 reports through `ar_status` and 0001's per-render report.
- Ruling 13: release modules embedded in the DLL; loose modules only via `ASTRORAY_KERNEL_DIR` in development.
- Ruling 14: D7 adds launch-duration sizing with per-item caps and re-queue; the number lives in 0009.
- Ruling 15: D12 states AVX2 as required.
- Ruling 16: NVRTC removed from D1, D10, D14, D15, Verification, Risks and Alternatives.
- Ruling 17: C2 recorded as accepted and the spine conflict (old Risk 11) removed.
- Ruling 18: `advance_geodesic` is a CUDA kernel emitting segments for `intersect_closest`; D11 bans material, medium and VM code in OptiX programs.
- Ruling 19: VRAM accounting goes through 0007's ledger, reserve rule and budget table.
- Ruling 21: first-frame gate cited as 0007's.
- Ruling 22: texture filtering with mipmaps cited in D13 as part of the design.
- Ruling 23: `shade_volume` carries the emission-absorption marcher; the register gate amendment is C2.
- Ownership (0002): D8 now lists every kernel other records need (`shade_light`, `shade_surface_ext`, `shade_light_path_ext`, `intersect_subsurface`, film and environment kernels); D11 gives the complete OptiX program list (lamp intersection, curves, spheres, motion variant) and the 8-bit instance mask allocation.
- Ruling 27: `shade_subsurface` and `intersect_subsurface` cite 0005 item 4a for the walk's model (D8).
- Ruling 29: time budgets cited by id (D7 B-17/B-19, D10 B-01/B-02/B-04/B-05/B-09/B-17..B-20, Verification, must-exist).
- Ruling 30: Context states the 30/s target, 24/s floor and 60/s later goal.
- Ruling 41: the f64 CPU build is an unshipped test reference; CPU geodesic integration is double in every build (D3).
- Ruling 46: the `advance_geodesic` row steps in-kernel and emits a segment only when a chord overlaps the scene bounds (D8).
- Ruling 47: D8 enumerates the `Stage` enum and adds `film_prepare_denoise` and the HAIR split pair `shade_hair` / `shade_light_path_hair` (`ShadeKind::Hair`, `LightHair`).
- Ruling 49: Review section headed as history; Risk 12 records owner decision 29 (untested architectures built and labelled).
- Ruling 6 (second pass): D1 cites 0003's `kLanes`; D7's cancellation check reads the pending generation, not an epoch (ruling 37).
- Ruling 29 (second pass): D3, D7, D9, D11, D14, Risk 2 and Consequences carry no time numbers; the D7 overhead derivation moved to 0009 B-19's basis.
- Ruling 41 (second pass): `advance_geodesic_f64` added (D8) for 0006 D37/D40's double and mixed GPU geodesics, the one kernel exempt from the FP64 check (D1, Verification).
- Ruling 43 (second pass): bit 6 is set on every medium-bearing mesh whatever its ray visibility, a bit-6-only hit being a false interface (D11).
- Ruling 47 (second pass): D10 defines the VM stack class by kernel (0005 item 32's list, `bake_environment`, `advance_geodesic`); D2 adds `atomic_max`, `atomic_cas` and `clock` for 0008; D15 splits synchronous driver checks from asynchronous module failures (0001 D2.8); Risk 13 records the unspecified self-intersection rule.
