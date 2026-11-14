# 0002 — Kernel architecture, GPU execution model and budgets

Status: proposed (2026-11-14), revised after independent review (2026-11-14)

## Context

Astroray 2.0 needs one kernel source that runs on the RTX 5070 Ti (sm_120) and on the CPU (owner decision 2). The viewport has to hold 60 updates/s where possible and never fall below 24, with first frame comparable to Cycles (decisions 9, 10). The first release covers the full feature breadth (decision 7), including gratings and the light-tracing pass they need, designed in from the first milestone (decision 17). Builds and tests have hard budgets (CLAUDE.md rule 9) and failures must be loud (rule 7). Most code will be written by Sonnet-class agents from small task specs, so the structure must make the cheap thing the right thing and must catch drift mechanically.

v1 failed exactly here. Its fused shade kernel had 128 template variants, ran at 221-254 registers with 2.5-8.7 KB of stack, and one translation unit took about 1,800 s of a 1,870 s build (`v1-lessons/research/gpu-build.md`, `v1-lessons/toolchain-and-build.md`). The spine (`SPINE.md` §2) settles the language (restricted C++), the width (4 wavelengths), the fused surface kernel under `__launch_bounds__`, the per-kernel budgets and the NVRTC/nvcc split. Spike A measured these on a kernel with the right shape but less than full scope. This record turns them into rules an implementer can follow without guessing, and adds what the spine leaves open: queue and scheduling mechanics, OptiX use, memory layout, how the gate measures budgets, FMA and determinism policy, device failures, and what happens when the kernel grows.

Evidence: spike A (`spikes/A-kernel-budget/RESULTS.md`) for registers, spills, compile times, NVRTC, OptiX module timings and CPU/GPU agreement; spike B (`spikes/B-blender-end-to-end/RESULTS.md`) for Cycles' per-sample cost; research 03, 04 and 17 for how Cycles and pbrt-v4 do the same job. Spike C (wavefront vs megakernel) was not run; D7 gives an overhead estimate in its place and Risks 2 deals with the gap.

## Decision

### Challenges to the spine

- **C1 (correction of a cited number).** Spine §2 says CPU/GPU agreement is tested "on the throughput weight f·cos/pdf ... (1.1e-6 agreement measured)". The 1.1e-6 is the max error of **BSDF eval f** with FMA off. For the **throughput weight** with FMA off, spike A measured p99.9 1.4e-5 and a max of 1.6e-2 on one input (§6). Gates therefore use p99.9 thresholds with a separate outlier rule (D12), not a 1e-6 max.
- **C2 (register gate; needs the lead to amend spine §2).** The spine gates "at most 168 registers uncapped, no spills when capped at 168". Evidence against making the uncapped count a merge gate:
  - The uncapped count does not predict the shipped kernel's speed. Launch bounds are honoured; the fused kernel capped at 128 spills 68/112 B and runs 56% faster than uncapped, and even a 64-register cap with 472/836 B of spills beats uncapped (spike A §2-3).
  - Fused + in-kernel VM is already 150 uncapped, and spike A omits upsampling, textures, the light tree, environment sampling, emission MIS and denoiser writes, which spike A §8.1 names as the likely source of v1's 254. A hard 168 gate will probably trip at full scope. Lowering the production cap cannot clear it, because the probe build compiles launch bounds out, so the gate would force a split, which measured 22% slower. That contradicts the spine's own "split only when a measurement says so".
  - "No spills capped at 168" follows from "≤ 168 uncapped" and says nothing about the cap that ships.
  - Position taken: the merge gate checks the **shipping (capped) cubin** for spill bytes and stack against recorded allowances. Raising an allowance needs the throughput A/B of D9. The uncapped count is reported nightly, with warnings at 150 and 168 (D10).
- **C3 (caveat on "Real switchable to double").** Embree traverses in single precision, so the f64 CPU build has float traversal. The double reference is exact only for stages that do not trace rays (lobes, geodesic stepping). 0006 must know this.

### Kernel dialect and portability layer

**D1. Language and allowed constructs.** Kernel code is C++17 in freestanding headers under `src/kernel/`, compiled by nvcc/NVRTC for the GPU and MSVC `cl` for the CPU.
- *Allowed:* free functions; POD structs with member functions; `constexpr`, `if constexpr`, `static_assert`; enums; fixed-size arrays; references; `<cstdint>`, `<cfloat>`. Function templates may take only two parameters: the scalar type (`Real`/`float`/`double`) and the one kernel-identity parameter of D8. The spectral width is the constant `constexpr int kLambdas = 4` (0003), not a template parameter.
- *Forbidden:* virtual functions, RTTI, exceptions, `new`/`delete`/`malloc`, recursion, function pointers, STL containers and algorithms, direct `<cmath>` calls (use D2's wrappers), `__noinline__`, `-rdc`, inline PTX or asm outside the portability header, `long double`, thread-local storage, mutable globals (rule 8), `printf` outside debug builds, unsuffixed floating literals in float code, host-only headers. `__noinline__` is banned because spike A measured -37% at N=4 with up to 1.4 KB of stack.
- *Passing:* everything is force-inlined. Structs larger than 32 B go by `const&`. Results return by value as small structs, never through scalar out-pointers (v1: address-taken out-parameters cost about 4%).
- *Enforcement:* CPU kernel TUs build with `/EHs-c- /GR- /W4 /WX`. GPU float kernels build with `-Xptxas --warn-on-double-precision-use`, warnings as errors. A lint script owned by 0009 checks banned tokens and the include allow-list.

**D2. Portability header `src/kernel/portability.h`** is the only file that knows its target. It provides: `AR_FN` (`__host__ __device__ __forceinline__` under `__CUDACC__`, else `inline`); `AR_GPU` (constexpr bool) and `AR_ASSERT` (compiled out of release); `ar::load_ro(p)` (`__ldg` on GPU); `ar::atomic_add` for `float` and `uint32_t`; `ar::ballot`, `ar::lane_id` with single-lane CPU fallbacks; `ar::bit_cast`; math wrappers (`ar::sqrt`, `exp`, `log`, `sin`, `cos`, `pow`, `fma`, ...) overloaded on `float` and `double`. No address-space layer (CUDA has none; HIP/Metal are not built, spine §1). Pattern: Cycles `kernel/device/cuda/compat.h`, pbrt `PBRT_CPU_GPU`.

**D3. `Real`.** `float` in the GPU build and the default CPU build (`astroray_kernels_cpu`); `double` in a second CPU build (`astroray_kernels_cpu_f64`, `AR_REAL_DOUBLE`) used as the reference.
- `Real` is used for kernel arithmetic and path-state fields (D13), so the f64 build stays double between stages.
- Scene and asset buffers (vertices, textures, tables, material records) are explicit `float` or `half`, converted on load. Literals are written `Real(0.5)`.
- Cost: one extra CPU compile (spike A: 0.65 s float, 0.65 s double; throughput -6%).

**D4. One stage function, two targets.**

```
// src/kernel/stages/<stage>.h
AR_FN void stage_<name>(const KernelGlobals& kg, uint32_t item, const StepEntry& step);
```

- **GPU (`src/kernel_gpu/<stage>.cu`):** `AR_GPU_STAGE(name)` expands to `extern "C" __global__ void __launch_bounds__(T, B) k_<name>(const KernelGlobals* __restrict__ kg)`. The kernel reads its `StepEntry` (segment start, launch count, shadow-slot base) from the device-resident step table (D7) and runs a grid-stride loop over its launch count. `T` and `B` come from one table, `src/kernel/launch_config.h`, which the host launcher also reads.
- **CPU (`src/kernel_cpu/<stage>.cpp`):** `AR_CPU_STAGE(name)` loops over the same segment in chunks of 1,024 items on the host thread pool (pbrt `ParallelFor`).
- **Arguments:** one 8-byte pointer. `KernelGlobals` holds the scene, state, queue, step-table and settings pointers, lives in device memory (one copy per session), and is never passed by value: v1 found by-value parameter copies cost 27% of PC samples. Because counts are read on the device and the loop is grid-stride, the host may size grids from read-back counts or launch fixed grids; switching between them changes only host code (D7 remedy).

### Wavefront execution model

**D5. Pools.** A session has two structure-of-arrays pools.
- **Main path pool:** camera paths and light paths (D8), with a `path_kind` state bit from the first milestone. Capacity `min(work in the pass, 2^21, VRAM-derived cap)`, stated in the per-render report. CPU default 2^16.
- **Shadow pool:** self-contained visibility items emitted by shading stages: NEE shadow rays, volume NEE, camera connections from light paths. Each item holds ray, tmax, contribution, film wavelengths and target pixel, and does not depend on the path that created it. Capacity 2x main (PROVISIONAL).
- **Slot assignment is deterministic and atomic-free.** Free shadow slots are the shadow pool's `TERMINATED` segment, in index order (D6). Each shadow-producing stage declares in `launch_config.h` the most shadow items `k` one item can emit in one step (default 1). The step table gives the producers, in `Stage` order, consecutive ranges of the free list. An item at position `i` of its segment owns free slots `[base + k·i, base + k·(i+1))`, whether or not it emits. Unused reserved slots stay `TERMINATED`. If the free slots cannot cover a producer's whole segment, the producer launches on the first `floor(free_left / k)` items. The rest keep their `next_stage` and run next step, so no item is dropped. No compaction kernel is needed, because freed slots return to the free list at the next partition.

**D6. Queues (deterministic partition, no append queues).**
- Every pool item carries `next_stage` (u8, closed enum `Stage`). Each pool has its own `TERMINATED` value.
- `queue_build` performs one stable partition over both pools by `next_stage`, each pool up to its high-water mark (the largest slot index used this pass). It writes per-stage segments of pool-local indices. It runs as three kernels (block histogram, scan over blocks, stable scatter), a reduce-then-scan prefix sum (Blelloch 1990; pattern from Cycles `device/gpu/parallel_active_index.h`, `parallel_prefix_sum.h`).
- The scan kernel also fills the **step table**: per stage, the segment start and launch count, and for producers the shadow-slot base (D5), plus the refill count (D7). The table is computed on the device by the same stage function on both targets.
- Segments are in item-index order. The main `TERMINATED` segment is the free-slot list for refill. There is no physical compaction of the main pool in the first milestone.

**D7. Scheduling (one host loop shared by both targets).** Host C++ written against a two-method device interface (`launch(stage)`, `read_step_table()`), implemented once for CUDA and once for the CPU. A **step** is:
1. **Partition.** Run `queue_build` (fills the step table).
2. **Read the step table** (under 256 B, pinned memory) and wait on one event.
3. **Sort.** If the `shade_surface` segment holds at least 65,536 paths and the scene has at least 2 materials (PROVISIONAL), counting-sort that segment by material id (Cycles `parallel_sorted_index.h`). Only `shade_surface` is sorted, as in Cycles.
4. **Refill.** If fewer than a quarter of the main slots are active and pass work remains, launch `generate_camera` / `generate_light` on the refill count. This is Cycles' rule (refill when active < max/4, `integrator/path_trace_work_gpu.cpp`, research 04). The pass's work is one counter over its camera samples followed by its light samples. 0004 sets the light-sample count, which is zero until the light pass lands.
5. **Launch** every stage with a non-zero launch count once, in `Stage` enum order.

A pass ends when the work counter is exhausted and every segment except the two `TERMINATED` segments is empty. Each stage touches only its own segment's items, its reserved shadow slots, and the film (atomics), so the order between stages does not change results; the fixed order gives the same schedule on CPU and GPU. This departs from Cycles' "largest queue first" rule. Edits bump a session epoch that the loop checks between steps, so cancellation latency is one step.

**Overhead estimate (derivation, not a measurement).** One bounce takes two steps (intersect, shade), and its shadow item is traced in the next step alongside. At a default maximum depth of 12, a pass needs about 26 steps, plus one per alpha layer on the longest path (D11). A step costs 3 partition launches, up to about 8 stage launches and one round trip. Partition traffic at 1080p 1 spp is about 40 MB, about 50 µs at the 5070 Ti's 896 GB/s. The round trip is unmeasured; assume 20-100 µs under WDDM. So 26 × 70-150 µs ≈ 1.8-3.9 ms against a pass of about 9.5 ms (1.25 × spike B's 7.6 ms). That is **20-40%, above the D10 budgets**. The first milestone measures it before breadth work (Risks 2). Remedies, in order:
- (a) Fixed grids, and the step sequence captured as a CUDA graph. This removes the round trip and is host-only because of D4.
- (b) Compact the main pool when live paths drop below a quarter of its high-water mark (Cycles `compact_main_paths`).
- (c) Fuse the three partition kernels into one single-pass scan.

**D8. Kernel list (first release).** One kernel per TU. Each kernel owns one queue segment and writes `next_stage` for every item it processes.

| Kernel | Pool | Owns | Lands |
|---|---|---|---|
| `queue_build_{count,scan,scatter}` | both | partition, step table (D6) | M1 |
| `sort_{count,scatter}` | main | material sort of `shade_surface` | M1 |
| `generate_camera` | main | slot init: pixel, sample, film/transport wavelengths, camera ray with time, adaptive skip | M1 |
| `generate_light` | main | light-path init (method: 0004) | M1 stub (scheduler test only); real kernel first release |
| `intersect_closest` (OptiX raygen) | main | closest hit, hit record, `route_hit` (D11) | M1 |
| `intersect_shadow` (OptiX raygen) | shadow | opaque test; non-opaque hits recorded by any-hit (D11) | M1 (opaque), first release (recording) |
| `shade_background` | main | camera-path miss: world/sky emission with MIS, guide-buffer writes | M1 |
| `shade_surface` (`ShadeKind::Common`, D9) | main | node VM to OpenPBR record, alpha pass-through, emission with MIS, NEE emitting a shadow item, BSDF sample, Russian roulette, first-hit denoiser buffers, diagnostics | M1 |
| `shade_shadow` | shadow | evaluate recorded non-opaque hits in t order through the VM, update transmittance, deliver or re-queue | first release |
| extension identities of `shade_surface` | main | extension behaviours (research 17); partition into identities is 0005's | first release (grating first) |
| `shade_volume` | main | null-scattering medium events (0006 physics) | first release |
| `shade_subsurface` | main | random-walk step; local rays traced against the instance's GAS (D11) | first release |
| `shade_light_path` (`ShadeKind::Light`, plus light variants of extension identities as 0005/0004 decide) | main + shadow | light-path surface interaction; camera connections are shadow items with an explicit pixel (method: 0004) | first release |
| `advance_geodesic` | main | curved-ray segment for `intersect_closest`; a curved-ray miss returns here | after first release; enum value and routing bit in M1 |
| `k_route_hit` | test | `route_hit` compiled as a plain CUDA kernel for the fixture test and register gate | M1 |

Film resolve, adaptive convergence, denoise preparation and present packing are 0008's kernels, under D1-D4 and D10.

**Light paths in the scheduler, from M1.** `route_hit` sends a light-path hit to `shade_light_path` (or its extension variant) and a light-path miss to `TERMINATED`. Light paths share the main pool, the refill rule and the end-of-pass rule (D7). Their camera connections take shadow slots like NEE items. M1 lands the `path_kind` bit, the routing, the work counter and a scheduler test with a stub `generate_light` whose paths terminate at the first hit. The stub is test-only, not a shipped feature.

**Specialisation.** The only compile-time parameter is the kernel identity `ShadeKind`, a flat closed enum. Each value is instantiated in exactly one TU and has its own `launch_config.h` and `budget.toml` rows; values are never formed as products of template axes. The enum grows by one entry, one TU and two rows when 0005 or 0004 justify a new identity. The `Common` kernel contains no extension or light-path code. Feature switches are runtime fields in `KernelGlobals`, uniform per launch.

**D9. The fused surface kernel and the rule for splitting.** `shade_surface` is one kernel that runs the node VM in-kernel (spike A: fused VM 1.59 ms vs a separate parameter pass plus split 2.26 ms), BSDF sample and eval, and NEE.
- Production launch bounds `(256, 2)`, i.e. at most 128 registers (MEASURED on the fused kernel without VM: 148 to 128 registers, 0.82 ms vs 1.27 ms uncapped, 68 B spill stores). Other shade identities start with the same bounds. All other kernels start at 256 threads with no minimum-blocks clause until a benchmark sets one (PROVISIONAL).
- **Pre-split by structure** (spine §2): extension identities, light paths, subsurface, volumes, shadow evaluation.
- **Every other split or fusion** lands only with an A/B on the benchmark scene set at 1080p 1 spp and 64 spp. Protocol: burn-in, then the median of 3 runs of min-of-10 (spike A puts noise at up to 5%; v1 measured about 5% clock drift). Pass criteria:
  - as a performance change: at least 5% gain at both sample counts;
  - as a remedy for a budget breach (D10 ladder): fastest of the measured alternatives, including re-tuning the cap.
  - There is no register-count trigger: under a cap, growth shows as spills, which D10 gates.

### Budgets and how the gate measures them

**D10. Per-kernel budgets.** Gated on sm_120. The baseline lives in `src/kernel/budget.toml`: per kernel or OptiX program, T, B, registers, stack, spill allowance, compile seconds, and whether it is gated per merge or nightly. Every merge plots it in the benchmark report.

| Budget | Limit | Status |
|---|---|---|
| Production cubin: registers ≤ 65536/(T·B) for every kernel with B set | must hold | launch bounds honoured on 12.8 non-rdc (spike A) |
| Production spill stores/loads | allowance in `budget.toml`: 0 by default; shade identities ≤ 128 / 192 B | C2; PROVISIONAL (68/112 measured without VM; fused+VM capped unmeasured) |
| Production stack: shade identities / all others | ≤ 512 B / ≤ 128 B | PROVISIONAL (fused+VM 304 B, others 32 B measured) |
| Growth vs `budget.toml` | +5% registers (kernels without B) or +10% stack fails unless the change updates the baseline | PROVISIONAL |
| Uncapped registers (probe build, nightly) | warning at 150, loud warning above 168; never a merge failure | C2 |
| Compile, one device TU, nvcc AOT | ≤ 3.0 s | spine; MEASURED 1.3-2.5 s |
| NVRTC, one stage (dev) | ≤ 2.0 s | PROVISIONAL (1.06 s CUBIN measured) |
| Clean device build, `ninja -j8` | ≤ 30 s | PROVISIONAL (34 TUs in 11.3 s measured) |
| Dev edit-to-pixels: one stage / shared kernel header | ≤ 2 s / ≤ 5 s | PROVISIONAL (1.1 s one kernel measured) |
| CUDA context + modules ready: warm / cold (after driver update) | ≤ 300 ms / ≤ 1.5 s | PROVISIONAL |
| GPU idle between launches, 1080p 1 spp | ≤ 10% of pass time | PROVISIONAL; estimate breaches (D7) |
| `queue_build` + sort GPU time, 1080p 1 spp | ≤ 10% of pass time | PROVISIONAL; estimate about 14% (D7) |
| Kernel time per 1080p sample, spike B scenes | ≤ 1.25x Cycles (S1 7.6 ms, S2 10 ms) | PROVISIONAL, shared with 0007 |
| Kernel part of the merge gate, wall time | ≤ 120 s worst case (shared kernel header changed); about 20 s for a one-stage edit | PROVISIONAL; 0009 owns the 3-min total |

**What the merge gate runs** (serially, never two GPU builds at once; it prints elapsed time per item and fails if the kernel part exceeds its budget):
- **Production device build** (`-gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xptxas -v -c`, changed TUs; at most 30 s clean). Registers and stack come from `cuobjdump -res-usage`, spills from the `ptxas -v` lines; with no `-rdc` the compile-time count is final.
- **Agreement build** (`--fmad=false`, changed TUs; at most 30 s clean) and both CPU kernel builds (f32 and f64; under 10 s).
- **Fixture and agreement suites** (D12), 10^6 inputs per stage (under 15 s; CPU 1.7-2.9 M evaluations/s per thread, spike A §7).
- **Compile timing:** the at most 3 changed TUs with the largest baseline times, each by wall clock around one serial nvcc call with the compiler cache off and the OS file cache warm. A result within 10% of the limit is re-measured as the minimum of 3 (at most 27 s).

**Nightly:** the probe build (`AR_LAUNCH_BOUNDS=0`) for uncapped registers; compile timing of every TU; the NVRTC-vs-AOT identity test (per stage, `cuFuncGetAttribute(NUM_REGS, LOCAL_SIZE_BYTES)` equal to the AOT cubin, outputs bit-identical on fixtures; spike A found both identical); a compile-only budget check of every SASS architecture the release ships (D14); the OptiX module's registers via Nsight Compute (`launch__registers_per_thread`); and traversal rays/s on a fixed scene, gated at -5%. The OptiX module has no per-merge register gate because the driver compiles it. Its non-traversal code is limited to `route_hit` and the hit-recording any-hit (D11); `route_hit` is register-gated per merge through `k_route_hit`.

**Growth toward full scope.** Spike A is a lower bound. Growth is handled four ways:
1. *Front-load.* M1 builds `shade_surface` with every register-heavy part before breadth: in-kernel VM, Jakob-Hanika upsampling lookups, texture sampling, the `LightSampler` interface with tree-shaped traversal, environment sampling, emission MIS and first-hit buffer writes.
2. *Watch.* Every merge records production spills and stack; nightly records uncapped registers; the report plots both over time.
3. *Declare.* Each task spec touching a shade kernel states its expected production spill and stack delta; the gate prints the actual delta.
4. *Fallback ladder,* triggered by a production spill or stack breach or by a confirmed throughput regression. Follow it in order, measuring before each next step:
   1. Attribute the growth by stubbing features and rebuilding (v1's method; the probe build shows the live-state delta).
   2. Fix it locally: `const&` for any struct copy, recompute cheap values instead of keeping them live, narrow stored types.
   3. Re-tune the cap: A/B (D9) the kernel at register caps of 168, 128, 96 and 64 via `(T, B)` rows. Ship the fastest and record its spills as the new allowance. Spike A: the 64-register cap with 472 B of spills still beat uncapped.
   4. Move a rare feature to its own queue and identity; it must beat step 3's best.
   5. Split light selection into a `sample_light` stage, or split into `shade_surface_bsdf` + `shade_surface_nee`; it must beat step 3's best. Spike A measured the split pair 22% slower than the capped fused kernel.

   Never: template variant matrices, `__noinline__`, `-rdc`, or 8 wavelengths (197 registers measured).

### OptiX, compilation, memory, failures

**D11. OptiX is traversal only; no material code in OptiX programs.** There is one module (`src/kernel_gpu/optix_traverse.cu`) and one pipeline per device.
- **raygen:** `intersect_closest` and `intersect_shadow`, each over its segment. Apart from `optixTrace`, raygen code is limited to writing the hit record and `route_hit`, a table-driven function (per-primitive material id → material flags → `Stage`, plus `path_kind`). Volume-stack updates and all VM, texture and BSDF code run in CUDA stages.
- **closest-hit:** one per primitive type (triangles; built-in curves and spheres when hair or points land). Writes t, primitive, instance, u, v, primitive type.
- **miss:** one, which sets instance to `~0u`.
- **any-hit:** one, `ah_record`, for shadow rays only (Cycles pattern, `__anyhit__kernel_optix_shadow_all_hit` in `kernel/device/optix/kernel.cu`). If the primitive's material is opaque, it accepts the hit, which terminates the ray as occluded. Otherwise it keeps the K nearest hits (t, primitive, instance, u, v), ties broken by (t, primitive, instance) so the result does not depend on traversal order, sets an overflow flag beyond K, and ignores the intersection. K = 4 (PROVISIONAL, set by the foliage benchmark). It runs no VM or texture code. The CPU uses the same `record_shadow_hit` function in an Embree occlusion filter.
- **Shadow rule, one pass:** instances whose materials are all opaque carry `OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT`, so in opaque scenes `ah_record` never runs. Shadow rays use `TERMINATE_ON_FIRST_HIT | DISABLE_CLOSESTHIT`. Outcomes:
  - occluded → item terminated;
  - miss with no records → deliver to film;
  - miss with records → `shade_shadow` evaluates them in t order and delivers, or on overflow re-queues `intersect_shadow` from the K-th t.
  - Rays through more than K non-opaque layers cost one extra step per K layers.
- **Camera, indirect and subsurface rays** use `DISABLE_ANYHIT`. An alpha or Transparent-BSDF hit is resolved in `shade_surface` as a pass-through that re-queues `intersect_closest` from the hit, one step per layer, bounded by a transparent-bounce limit (0004). Cycles handles camera-ray transparency the same way.
- v1's opaque-only design (`TERMINATE_ON_FIRST_HIT | DISABLE_CLOSESTHIT | DISABLE_ANYHIT`) covered all-triangle opaque scenes; v1 kept alpha-shadow walks on a software BVH (`gpu-build.md:73`). It is the opaque special case here, not a precedent for the whole rule.
- **Instance masks:** six bits for Blender ray visibility (camera, diffuse, glossy, transmission, shadow, volume), two spare.
- **SBT:** one record per program; hit groups per primitive type, never per material.
- **Pipeline options:** traversable graph flags `ALLOW_SINGLE_LEVEL_INSTANCING | ALLOW_SINGLE_GAS` (the second for subsurface local rays traced directly against the instance's GAS in object space; verify, Risks 5), `maxTraceDepth = 1`, no SER.
- **Format: PTX** from nvcc (AOT) and NVRTC (dev). Spike A measured PTX and OptiX-IR equal in load time (984 vs 1,054 ms cold) and launch time (1.099 vs 1.118 ms); NVRTC `--optix-ir` is untested, and Cycles ships PTX.
- Module and pipeline creation start asynchronously when the engine instance is created, in parallel with scene sync, and use OptiX's disk cache (0.6 ms warm measured).

**D12. FMA and determinism policy.**
- **Production GPU:** `--fmad=true`, no `--use_fast_math`, `-prec-div=true -prec-sqrt=true -ftz=false` (spike A's numbers were measured this way). **Production CPU:** `/O2 /arch:AVX2 /fp:precise`, no `/fp:contract`. An explicit `ar::fma` is allowed: it is correctly rounded on both targets.
- **Agreement build:** GPU `--fmad=false`. Per-stage tests run on 10^6 fixed host-generated inputs (spike A protocol) and compare 4-wavelength relative L2 error. Thresholds sit about 3-10x above spike A's measurements:
  - *No-FMA GPU vs CPU float:* eval f max ≤ 1e-5 (measured 1.1e-6); throughput p99.9 ≤ 1e-4 (1.4e-5); direction p99.9 ≤ 1e-5 rad (1.2e-6); lobe/light choice disagreement ≤ 1e-5 of inputs (0).
  - *Production GPU vs CPU f64:* throughput p99.9 ≤ 5e-4 (1.3e-4); eval p99.9 ≤ 1e-4 (2.7e-5); direction p99.9 ≤ 1e-5 rad (3.7e-6).
  - *Outliers:* every max is printed; fewer than 0.01% of inputs may exceed 10x a p99.9 threshold.
  - Raw f at a sampled direction is never gated (ill-conditioned for near-delta lobes, spike A).
- **Determinism:**
  - Random numbers are keyed only on (pixel or light-sample index, sample, dimension, seed) (0003). A path's computation never depends on queue position, slot, thread count or sort order.
  - Queues, shadow-slot assignment (D5) and any-hit records (D11) are deterministic; per-item stage outputs are bit-identical run to run on the same device and build.
  - Film accumulation uses float atomics, so GPU images and multithreaded CPU images are reproducible up to summation order (about 1e-6 relative; v1 measured this). CPU at `threads=1` is bit-exact run to run.
  - Bit-exact GPU images are not offered (owner question 2).

**D13. Memory layout and VRAM accounting.**
- **Path state:** one X-macro list (Cycles `state_template.h` pattern) declares every field with type and feature tag; fields whose feature is off are not allocated. Each field is its own array, 256-B aligned. 4-wide spectra are `float4` (one 16-B load; double4-equivalent in f64). 0003 owns the field list.
- **Size budgets (PROVISIONAL):** main state ≤ 256 B/path; shadow item ≤ 128 B, plus a feature-tagged hit-record array (K × 20 B = 80 B) allocated only when the scene has non-opaque instances. Research 04 estimates 124-170 B before 16 B of transport wavelengths; v1 used 220 B. At 2^21 paths:
  - main pool: 512 MiB;
  - shadow pool (2^22 items): 512 MiB;
  - queue and sort indices: about 32 MiB;
  - total about 1.05 GiB; add 320 MiB of hit records in scenes with non-opaque instances.
- **Scene data:** flat arrays read through `ar::load_ro`; material records fixed-size and 16-B aligned (0005); upsampling tables 9.4 MB per colour space. Textures are linear memory read with `ar::load_texel`, with filter math in shared kernel code. Hardware filtering is not used: its 8-bit fractional weights (CUDA Programming Guide, "Texture Fetching") would differ from the CPU. 0005 may revisit with a measurement.
- **Accounting:** every device allocation goes through one allocator tagged by category (geometry, acceleration structures, textures, tables, path state, shadow pool, film, scratch, modules). Totals are exposed through the C ABI and the stats report. Pool capacity is derived from free VRAM minus a 1 GiB reserve for Blender (PROVISIONAL).

**D14. Compilation routes.** All GPU code is loaded as module images through the CUDA driver API (`cuModuleLoadData`, `cuLaunchKernel`); the host never uses `<<<>>>`. AOT and RTC differ only in where the image comes from.
- **AOT (gate and release):** nvcc builds one cubin per stage TU for sm_120. Release builds add 0009's architecture list as SASS plus the newest PTX, embedded in `astroray_core.dll`. Every shipped SASS architecture passes the nightly compile-only budget check (registers, spills, stack against `budget.toml`); runtime behaviour is tested on sm_120 only, and D15 warns on any other device. Banned flags: `--Ofast-compile` (spike A: 222 registers, a 100x slower NVRTC kernel); `--split-compile` (148 to 162 registers); `--use_fast_math`; `-rdc`.
- **RTC (dev mode, `ASTRORAY_KERNEL_RTC=1`):** NVRTC compiles changed stages from the source tree to CUBIN for the local architecture with the same flags, on a background thread pool, cached by hash of preprocessed source, flags and NVRTC version. A changed stage is swapped at the next step boundary and accumulation resets.
- Release ships no NVRTC. The CPU target has no hot reload; it rebuilds in under a second (spike A: 0.65 s).

**D15. Device failure contract (rule 7).** Every CUDA, OptiX and NVRTC call is checked. Every failure reaches the C ABI's error/warning channel (0001) with the text below, and the addon shows it as a Blender report. The engine never switches device on its own.

| Case | Detected at | Effect |
|---|---|---|
| Allocation failure | allocator | render fails; message names category, size, free VRAM |
| NVRTC compile error (dev only) | RTC worker | previous image of that stage keeps running; persistent viewport warning with compiler log, stage and file:line until the stage compiles |
| OptiX module or pipeline failure | device init | GPU device unavailable for the session; render fails with the OptiX log |
| Driver too old, or no image for this GPU | device init (`cuDriverGetVersion` vs the build's minimum; module-load result) | GPU device unavailable; message gives found and required driver and the GPU architecture |
| Sticky CUDA error (illegal address, launch failure, ECC) | the per-step event wait | GPU device dead for the process; render fails naming stage and step; message says Blender must be restarted to use the GPU (CUDA requires a process restart); CPU device stays usable |
| Other launch error | the per-step event wait | render fails naming stage and step |
| Asynchronous device init fails during scene sync | init worker | error posted when it happens; the first device upload re-raises it; host scene data is kept |
| GPU architecture other than sm_120, or PTX JIT used | device init | warning once per session ("not covered by runtime tests"; JIT seconds if JIT ran); render proceeds |

Debug builds wait after every launch so an error names its stage.

## Alternatives considered

- **Megakernel, with or without SER.** Traversal only exists inside OptiX programs, so a megakernel would put the whole integrator into a driver-compiled raygen (spike A: about 1 s cold for the fused body alone), sized by the union of all features (v1: 254 registers), with sorting replaced by SER, which is unmeasured here and has an open crash report (research 08 §6). Against it: Laine et al. 2013; pbrt-v4 ch. 15; Cycles X; one preprint at 14% for wavefront (research 04 §2). Not chosen; the overhead risk is tracked in D7 and Risks 2.
- **Split surface kernels as the default.** Measured 22% slower than the capped fused kernel (spike A §3). Kept only as ladder step 5.
- **A hard uncapped register gate (spine §2 as written).** Would force splits on an uncapped number that does not predict shipped speed (C2).
- **`__noinline__` lobes or callables.** -37% (N=4), -42% (N=8), stack up to 1.4 KB (spike A).
- **Template variants or per-material kernels.** v1's 128-variant TU took 1,800 s. With a single OpenPBR record the per-type axis disappears (research 04 B). The `ShadeKind` enum is one flat axis, one TU per value.
- **A template parameter for the wavelength count.** It has no user once N = 4 is fixed (0003) and 8 is banned.
- **8 wavelengths.** 197 registers uncapped, -41% capped throughput (spike A).
- **Append queues or atomic bump allocation for shadow slots** (pbrt, Cycles). They work on both targets (`ar::atomic_add` exists on the CPU), but slot order would depend on atomic order. That makes shadow-pool layout and the permutation test nondeterministic and needs a compaction kernel. The reservation in D5 costs no extra kernel.
- **Transparent shadows with no any-hit, one step per layer** (the earlier draft). Foliage can put tens of layers on one shadow ray, and every step carries a partition and a round trip.
- **Alpha evaluation inside any-hit** (pbrt `gpu/optix/optix.cu`). Puts texture or VM code in driver-compiled programs with no register gate.
- **`-rdc` with device linking.** v1 paid 50-75 s per serial link; compile-time register counts came out up to 5x wrong.
- **OptiX-IR modules.** Measured equal to PTX; NVRTC IR output unverified. Revisit behind one switch if a measurement shows a gain.
- **`--use_fast_math`** (Cycles and pbrt). Loosens division, sqrt and denormals and widens the CPU/GPU gap; the budgets were measured without it.
- **Slang, Vulkan RT, Dr.Jit.** Rejected in research 03.
- **Hardware texture filtering.** A second, lower-precision filter the CPU cannot reproduce.
- **CUDA runtime launches for AOT plus the driver API for RTC.** Two launch paths; the driver API alone covers both.
- **Silent CPU fallback on device failure.** Violates rule 7.

## Consequences

- **Easy:**
  - A kernel edit costs about 1-2 s in dev and one 2-3 s TU in AOT.
  - A new stage means one enum value, one header, one `.cu`, one `.cpp` and two table rows.
  - The CPU runs the same stage functions and the same step-table logic, so CPU/GPU differences reduce to arithmetic and traversal edge cases.
  - Spill, stack and compile drift is visible on every merge.
  - Each stage can be unit-tested on fixtures on the CPU.
- **Hard:**
  - Every feature must fit a stage boundary and route through `next_stage`.
  - State crosses global memory between stages (bandwidth unmeasured).
  - Agents cannot reach for STL, virtuals, recursion or fast-math.
  - Camera rays cost one step per alpha layer, and shadow rays one extra step per K non-opaque layers.
  - Host scheduling costs a round trip per step, estimated above budget at 1 spp (D7).
  - A GPU fault inside Blender's process needs a Blender restart.
- **Cost of unused features:**
  - an empty queue costs one partition bucket and no launch;
  - a feature-tagged state field costs no memory;
  - a runtime flag costs a uniform branch plus its share of the shade kernel's spills, which the gate counts;
  - the light-pass hook costs one state bit, routing entries and one work-counter range;
  - the geodesic hook costs one enum value and one routing bit;
  - the f64 CPU build costs one CPU compile;
  - the any-hit costs nothing in opaque scenes (instance flag).

## What lands when

- **First milestone:**
  - D1-D4 with the lint;
  - `KernelGlobals`, the state X-macro, the allocator with accounting;
  - `queue_build` with the step table and shadow reservation, and the sort;
  - `generate_camera`, `intersect_closest`/`intersect_shadow` (opaque) on OptiX and Embree, `shade_background`, and `shade_surface` front-loaded with every register-heavy part (D10), including alpha pass-through;
  - the light-pass scheduler hooks with the stub `generate_light` test (D8);
  - the CPU driver;
  - AOT and RTC routes;
  - the merge gate and nightly jobs (D10) with `budget.toml` and history plots;
  - the agreement build and tests;
  - asynchronous device init and the D15 contract with failure-injection tests;
  - the geodesic enum value and routing bit.
  - Before breadth work, M1 measures: `shade_surface` capped at `(256,2)`; scheduling overhead on the spike B scenes and an alpha-foliage scene (steps per pass, GPU idle, partition time; Risks 2). If either overhead budget is breached, remedy (a) of D7 lands in M1.
- **Before first release:**
  - extension identities (grating first), `shade_volume`, `shade_subsurface` with local traversal;
  - transparent shadows (`ah_record`, `shade_shadow`) with K set on the foliage scene;
  - `generate_light`, `shade_light_path` and their budget rows measured;
  - OptiX curves;
  - release fatbins with the per-architecture compile-only check;
  - frame-level gates (overhead, per-sample time against Cycles).
- **After first release:**
  - `advance_geodesic` (black-hole milestone);
  - an SER or fused-composition experiment;
  - a second traversal backend only if a vendor port is ever approved.

## Verification

| Decision | Check | Pass |
|---|---|---|
| D1-D3 | lint; `/EHs-c- /GR- /WX` CPU build; ptxas double-use warnings as errors | zero findings; f32 kernels have no FP64 instructions |
| D4 | per-stage fixture test: same inputs through the CPU and GPU (no-FMA) builds | D12 thresholds |
| D5 | shadow pool sized below demand on a test scene | no item dropped; film equals the large-pool run within 1e-6 relative |
| D6-D7 | debug flag that randomly permutes indices within each segment before each launch (pool large enough that nothing is deferred) | main-pool state bit-identical to the unpermuted run; shadow pool equal as a multiset (items sorted by bytes) |
| D7 | 1080p 1 spp pass on the spike B scenes and the foliage scene; Nsight Systems timeline | both overhead budgets in D10 |
| D8 | each kernel has a CPU fixture test and rows in `launch_config.h` and `budget.toml`; OptiX programs have nightly rows | the gate fails on any CUDA kernel or OptiX program missing from either table |
| D8 | stub light-path scheduler test | every light path terminates; pass ends; camera image unchanged |
| D9 | any split or fuse change carries an A/B report | D9 criteria |
| D10 | merge gate and nightly as described | all limits; growth rule; kernel part of the gate within its time budget |
| D11 | traversal agreement: Embree (`RTC_SCENE_FLAG_ROBUST`) vs OptiX on 10^6 fixed rays per canonical scene; transparent-shadow analytic test with n layers of opacity a for n = 1, K and 3K+1 | hit/miss mismatch ≤ 1e-4 of rays, all within 1e-4 relative of a triangle edge (watertight-edge mismatches are expected, v1); transmittance (1-a)^n within 3σ |
| D12 | agreement suite; CPU `threads=1` twice gives bit-identical film; GPU twice gives bit-identical state dumps and film within 1e-6 relative | thresholds as listed |
| D13 | after scene load: drop in `cuMemGetInfo` free memory vs the allocator total | within 64 MiB |
| D14 | nightly NVRTC vs AOT identity; cold and warm start timing; per-architecture compile-only check | equal registers and stack, bit-identical outputs; D10 limits |
| D15 | injection per case: broken stage source in RTC; corrupted PTX; mocked old driver version; failing init worker; a debug-only out-of-bounds kernel run in a child process | each case produces its ABI error or warning; nothing renders silently |

## Risks and open items

1. **Full-scope register growth** remains the central risk. Fused + VM measured 150 uncapped with **no capped measurement**; M1 must measure it under `(256,2)` before breadth work. Under C2 a breach shows as spills, and the D10 ladder applies.
2. **Spike C was not run.** CHECKPOINT-1 listed it as required before the architecture is written. This record replaces it with the D7 estimate, which puts scheduling overhead at 20-40% of a 1 spp 1080p pass, above budget. M1 measures overhead before breadth work, and remedy (a) is host-only. **The lead must decide whether that is acceptable or whether spike C runs before sign-off.**
3. **State bandwidth.** Bytes per path and DRAM traffic per pass are estimates (research 04 S3); measure with Nsight Compute in M1.
4. **OptiX module cold compile** (traversal + `route_hit` + `ah_record`) and **NVRTC compiling about 17 stages in parallel in-process** are unmeasured.
5. **Direct GAS traversal** for subsurface local rays assumes `ALLOW_SINGLE_GAS` can be combined with single-level instancing in one pipeline. Verify before `shade_subsurface`. Fallback: a second any-hit filtering on instance id, which is integer-only and consistent with D11.
6. **Toolkit change.** The numbers are from CUDA 12.8; 0009 may pin 13.x. The gate re-baselines on the toolkit-change commit, and only there.
7. **PROVISIONAL values:** sort threshold, pool and shadow capacities, K, launch configs other than `shade_surface`, stack limits, spill allowances, and gate-time shares. All are set by build-phase measurement.
8. **Software texture filtering cost** is unmeasured (v1: texture and upsampling work was 14% of samples).
9. **The flag name `--warn-on-double-precision-use`** needs checking against the pinned ptxas.
10. **Nsight counters** need a one-time administrator enable (owner question 1).
11. **Spine conflict:** C2 replaces spine §2's hard "≤ 168 uncapped / no spills at 168" gate. Until the lead amends the spine, D10's merge gate contradicts it.
12. **Sticky CUDA errors** require a process restart, so a kernel fault ends GPU rendering for the Blender session (Cycles has the same property). This follows from 0001's in-process engine; an out-of-process engine is the only remedy (CHECKPOINT-1 keeps it as a fallback).
13. **Owner question 3:** the release SASS list covers GPUs that are never runtime-tested. D14/D15 warn rather than refuse, pending the owner's answer.

## Assumptions about other records

- **0001:** the engine is an in-process C-ABI DLL, Windows and NVIDIA only. The ABI has an error/warning channel that can post asynchronously (D15). The CPU target can render.
- **0003:** owns the state field list (including `path_kind`), the transport/film wavelength split, the counter-based sampler keyed for both camera and light samples, and the flat spacetime policy at zero cost. Uses D13's storage conventions with `kLambdas = 4`.
- **0004:** owns stage semantics on D5-D8: NEE/MIS, light tree, volumes, transparent-bounce limit, adaptive sampling, and the light-pass method with its light-sample count per pass. It may need extra `ShadeKind` values, and may need `k > 1` shadow items per item.
- **0005:** a fixed-size OpenPBR record, the VM in-kernel, a closed `ext_kind` enum with a side table, a per-material opaque flag for `ah_record`, and texture filtering in kernel code. It decides how extension kinds map onto `ShadeKind` identities, answering research 17's review objection, and each identity gets a budget row.
- **0006:** geodesic segments go through `intersect_closest`; C3 applies.
- **0007:** triggers device init at engine creation and uses per-pass timing for its resolution controller.
- **0008:** provides `film_add` with atomics and its own kernels under D1-D4.
- **0009:** provides the gate script, the toolkit pin, the release architecture list, and a 3-min merge gate in which kernels take at most 120 s.

## Sources

- Spike A `spikes/A-kernel-budget/RESULTS.md` (§§2-8); spike B `spikes/B-blender-end-to-end/RESULTS.md` (§3).
- Research `03-kernel-language-backend.md`, `04-integrator-architecture.md` (refill rule, lines 53 and 233), `08-build-toolchain.md`, `17-materials-and-custom-types.md` (architectural consequence; review findings), `10-denoise-upscale.md`.
- v1: `v1-lessons/research/gpu-build.md` (lines 37-73), `v1-lessons/toolchain-and-build.md`, `v1-lessons/research/transport.md`.
- Laine, Karras, Aila, "Megakernels Considered Harmful", HPG 2013.
- Pharr, Jakob, Humphreys, *Physically Based Rendering*, 4th ed. (2023), ch. 15.
- Blelloch, "Prefix Sums and Their Applications", CMU-CS-90-190, 1990.
- Woop, Benthin, Wald, "Watertight Ray/Triangle Intersection", JCGT 2013.
- Wald et al., "Embree", ACM TOG 33(4), 2014.
- Meister et al., "On Ray Reordering Techniques for Faster GPU Ray Tracing", I3D 2020.
- Padilla, Webster, Kim, arXiv 2605.27323 (preprint, wavefront vs megakernel).
- Cycles (Apache-2.0):
  - `kernel/device/cuda/config.h`, `compat.h`;
  - `kernel/device/gpu/kernel.h`;
  - `kernel/device/optix/kernel.cu` (shadow any-hit recording);
  - `kernel/integrator/state_template.h`, `shade_surface.h`;
  - `integrator/path_trace_work_gpu.cpp` (refill, `compact_main_paths`);
  - `device/gpu/parallel_prefix_sum.h`, `parallel_active_index.h`, `parallel_sorted_index.h`.
- pbrt-v4 (Apache-2.0): `src/pbrt/pbrt.h`, `wavefront/integrator.h`, `wavefront/workitems.h`, `gpu/optix/optix.cu`.
- NVIDIA: OptiX 9.1 Programming Guide; CUDA Programming Guide ("Texture Fetching"); CUDA Driver API `CUresult` (sticky errors require a process restart); NVRTC documentation.

## Review and resolution

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
