# Spike A: kernel budget on RTX 5070 Ti (sm_120)

Throwaway measurement spike. Everything below comes from `run_all.ps1` (raw outputs in `out/results/`, ~20 min).
"Measured" = produced by a script command; "inferred" = my reading. The old `C:\Users\hgcom\OneDrive\Astroray` repo was not touched.

## 0. What was built and what it is not

`include/` holds a header-only kernel library (no virtuals/exceptions/STL; `Real` float/double; `KL_HD` macros): `Spec<N>` packets (N=4 and 8), a 20-scalar + 6-spectrum OpenPBR-style record (44 floats at N=4, 68 at N=8), lobes (fuzz/charlie sheen, GGX coat, anisotropic GGX metal with per-wavelength complex Fresnel from Gulbrandsen n,k, dielectric GGX reflection + rough transmission with per-wavelength thin film, Oren-Nayar diffuse, Karis-fit albedo scaling), VNDF sampling, alias-table light choice, sphere (cone) and rect lights with MIS, shadow-ray record emission, PCG counter RNG, SoA path state, a 16-opcode bytecode VM. Sources are cited in `lobes.h`. 34 GPU translation units (10 variants x {fused, bsdf, nee} + 2 param + 2 fused+VM), a CPU target, an NVRTC harness and an OptiX harness.

**What the spike omits (so every register number is a lower bound, inferred):** spectral upsampling / texture fetches (old engine: 14% of samples, 54% of instructions), closure graph and per-material shader variants, env map / mesh lights / light tree, volumes, ray traversal, emission MIS, denoiser buffers. The record has 64 materials in 8 classes, so some divergence exists, but no sorting. Fused kernel is 13.5k SASS instructions. It reproduces the *shape* of the v1 problem (lobes + NEE + spectral state), not its full size.

## 1. Toolchain (measured; `out/results/versions.txt`)

CUDA 12.8 (nvcc/ptxas V12.8.61), MSVC cl 19.44.35208 (BuildTools 17.14; Community has no VC tools), ninja 1.13.2, driver 621.24, GPU cc 12.0 (70 SMs). `nvcc -arch=sm_120` is accepted with this MSVC (exit 0), as is `-gencode arch=compute_120,code=sm_120` (used for all builds, SASS only). NVRTC 12.8 works. OptiX SDK 9.1.0 headers found and used.
GPU load: the first (discarded) benchmark runs overlapped a Blender 5.3 session at ~95% GPU utilisation; the reported `run_all` run had the GPU idle (2-4% before each run, clocks 2887-2902 MHz after warm-up). Run-to-run median spread over 3 repeats is 0.5-2%, so differences under 5% are noise.

## 2. Registers, stack, spills (measured; `out/results/regs.csv`, `regs_raw.txt`)

Command per TU: `nvcc -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xcompiler /MD -Iinclude -Isrc <variant -D flags> -Xptxas -v -c src/k_<kernel>.cu` (non-rdc), cross-checked with `cuobjdump -res-usage <obj>` (identical REG/STACK). Spill = ptxas "spill stores/loads". Occupancy column is inferred from 65536 regs/SM, 256-thread blocks.

| variant | flags | fused regs / stack / spill st-ld | bsdf regs | nee regs | warps/SM (fused) |
|---|---|---|---|---|---|
| base | inline, by value | **148** / 32 / 0-0 | 122 | 116 | 8 |
| ptr | material by pointer | 148 / 32 / 0 | 122 | 116 | 8 |
| lb | `__launch_bounds__(256,2)` | **128** / 104 / 68-112 | 128 (0 spill) | 126 (0 spill) | 16 |
| lb4 | `__launch_bounds__(256,4)` | 64 / 376 / 472-836 | 64 (276-336 spill) | 64 | 32 |
| noinl | lobes `__noinline__` | 185 / **976** / 0 | 129 / 960 | 121 / 800 | 8 |
| noinl_ptr | noinline + by pointer | 124 / 616 / 0 | 96 / 600 | 91 / 616 | 16 |
| noinl_lb | noinline + (256,2) | 128 / 1072 / 228-144 | 128 / 944 / 92-32 | 128 / 656 / 32-0 | 16 |
| w8 | 8 wavelengths | **197** / 32 / 0 | 154 | 152 | 8 |
| w8_lb | 8 wide + (256,2) | 128 / 296 / 316-548 | 128 / 136 / 108-104 | 128 / 104 / 68-68 | 16 |
| w8_noinl | 8 wide + noinline | **246** / 1440 / 0 | 180 | 162 | 8 |
| param (VM) | N=4 / N=8 | 42 / 304 (N=4), 42 / 464 (N=8) | | | |
| fvm (fused + VM in-kernel) | N=4 / N=8 | 150 / 304; 197 / 464 | | | |

Raw ptxas lines for key variants (full set in `regs_raw.txt`):
```
base_fused    Used 148 registers, 32 bytes stack frame, 0 bytes spill stores, 0 bytes spill loads   (cuobjdump REG:148 STACK:32)
lb_fused      Used 128 registers, 104 bytes stack frame, 68 bytes spill stores, 112 bytes spill loads (REG:128 STACK:104)
lb4_fused     Used  64 registers, 376 bytes stack frame, 472 bytes spill stores, 836 bytes spill loads (REG:64 STACK:376)
noinl_fused   Used 185 registers, 976 bytes stack frame, 0 spill                                      (REG:185 STACK:976)
w8_fused      Used 197 registers, 32 bytes stack frame, 0 spill
w8_noinl_fused Used 246 registers, 1440 bytes stack frame, 0 spill
```

Findings (measured):
- **No unmodified N=4 kernel saturates or spills.** Fused 148 (v1 shade kernel: 221-254), split bsdf 122 / nee 116. Pass bar from research 03/04 (< 168, no spills at 168) holds at N=4; it fails for N=8 fused (197) and noinline N=8 (246, near the 255 cap).
- **`__launch_bounds__` IS honoured on sm_120 with CUDA 12.8 non-rdc**: 148 -> 128 (256,2), -> 64 (256,4), 197 -> 128 at N=8. This contradicts the v1 note ("byte-identical codegen"). Under `-rdc=true` ptxas -v also honours it at compile time (`rdc_probe.txt`: base 150, lb 128, noinline 154, noinline+lb 128); device-linked final counts were **not** measured, and v1 recorded that the compile-time count under rdc is unreliable, so the v1 observation may be an rdc-link effect (inferred, untested).
- `__noinline__` on lobes does not reduce the kernel register count; it raises stack to 0.6-1.4 KB and (N=4) raised registers 148 -> 185. Passing the record by pointer instead of by value is a no-op when inlined and large when noinline (185 -> 124 regs, 976 -> 616 B stack): by-value copies of the 176 B record go through the call stack.
- Instruction count: fused 13.5k SASS instrs (N=4), 19.1k (N=8); noinline shrinks code (8.6k) but not live state.
- The 32 B stack in every inlined variant has an unidentified source (some dynamically indexed local); not chased.

## 3. Runtime (measured; `out/results/runtime_summary.csv`, `runtime_raw.txt`)

`bench.exe`: N = 2^20 paths, 64 randomised materials (metal, glass, coated, fuzz, thin film, emissive/alpha), random hits and directions, 100-launch burn-in, then 5 warm-up + 20 timed launches per kernel (CUDA events), throughput state reset between launches, whole bench repeated 3x. Table shows median of the 3 per-run medians (ms) and paths/s. "table" = records read straight from the material table (what fused reads); split kernels read a per-path record buffer, filled here by a copy of the same table entries so fused and split do identical work (checksums of pdf/NEE outputs are bit-equal across all variants and between fused and split; `CHECK` lines in `runtime_raw.txt`).

| variant | fused ms | (Mpaths/s) | bsdf ms | nee ms | split sum ms | split vs fused |
|---|---|---|---|---|---|---|
| base | 1.273 | 824 | 0.609 | 0.454 | 1.063 | split 16% faster |
| lb (256,2) | **0.816** | **1285** | 0.604 | 0.448 | 1.052 | fused 22% faster |
| lb4 (256,4) | 0.956 | 1097 | 0.727 | 0.517 | 1.244 | fused 23% faster |
| noinl | 2.012 | 521 | 1.396 | 1.285 | 2.681 | split worse |
| noinl_lb | 2.684 | 391 | 2.045 | 0.880 | 2.925 | |
| ptr | 1.276 | 822 | 0.610 | 0.451 | 1.060 | = base |
| noinl_ptr | 1.185 | 885 | 1.098 | 0.843 | 1.940 | |
| w8 | 1.877 | 559 | 1.564 | 0.919 | 2.483 | fused 24% faster |
| w8_lb | 1.378 | 761 | 1.005 | 0.644 | 1.649 | fused 16% faster |
| w8_noinl | 3.238 | 324 | 2.681 | 1.591 | 4.272 | |

With the parameter VM (records computed per path, `vm` rows): VM kernel alone 1.180 ms (N=4) / 1.821 ms (N=8); fused+VM in one kernel (`fvm`) 1.590 ms (N=4) / 2.211 ms (N=8); split with VM = param + bsdf + nee = 1.180 + 0.625 + 0.451 = **2.256 ms** (N=4), so the in-kernel VM is 30% faster than a separate parameter pass that materialises 176 B/path (inferred reason: per-path record write + two reads plus a compute-bound VM with locally indexed register file; 304 B stack in the VM kernel).

Findings (measured unless marked): (1) `__launch_bounds__(256,2)` gave +56% fused throughput, bought with 68 B of spills; even the 64-register cap (472 B spills) beat uncapped. This is the largest single lever found. (2) Splitting helps the uncapped fused kernel (occupancy 8 -> 16 warps, inferred) but a capped fused kernel beats the split pair, and splitting costs more when a parameter pass is added. (3) `__noinline__` costs 37% (N=4), 42% (N=8); noinline + by-pointer recovers most of it but still loses 7% against inline. (4) Width 8 costs 32% fused throughput uncapped (824 -> 559 Mpaths/s) and 41% capped (1285 -> 761).

## 4. Compile times (measured; `out/results/compile_times.txt`)

No compiler cache was used. Serial per TU, 3 runs each, `nvcc ... -Xptxas -v -c`: **1.3-2.5 s** per TU (min/median within 1%): fused base 1.94/1.94, bsdf 1.48/1.49, nee 1.31/1.31, w8_fused 2.37/2.40, w8_fvm 2.46/2.47, w8_noinl_fused 1.69, param 0.84. ptxas itself reports ~0 ms; the time is nvcc front end + cl host stub. The old engine's worst TU took ~30 min; the largest TU here is ~2.5 s.

| scenario (34 nvcc TUs + 1 cl + link) | min / median s (3 runs) |
|---|---|
| full clean build, `ninja -j8` | 11.3 / 11.4 |
| full clean build, `ninja -j1` | 55.1 / 55.2 (sum of per-TU minima 53.7) |
| touch `include/lobes.h` (all TUs), -j8 | 11.2 / 11.2 |
| same, -j1 | 55.6 / 55.9 |
| touch `src/k_nee.cu` (10 TUs), -j8 | 3.7 / 3.8 |
| touch `src/k_param.cu` (2 TUs), -j8 | 1.03 / 1.09 |
| touch `src/bench.cpp` (cl + link) | 0.70 / 0.72 |
| CPU target `cl /O2 cpu_main.cpp` float / double / float+FMA | 0.65 / 0.65 / 0.67 |

nvcc option sweep (base_fused, 3 runs): `-arch=sm_120` 1.95 s (same regs); `--split-compile=0` 2.01 s, regs 148 -> 162; `-Xptxas --register-usage-level=1|10` no change (148); `-Xptxas -O1` 150 regs; `--Ofast-compile=max` 1.29 s but **222 regs and a 5.4 KB local frame, and the NVRTC version of it ran 100x slower (141 ms vs 1.28 ms)**; do not use for anything timed. (w8_noinline sweep in the file shows the same pattern.)
Parallel speedup of -j8 over -j1 is 4.9x. Unknown: scaling to a real engine's TU count; these TUs are small.

## 5. NVRTC and OptiX (measured; `out/results/rtc_optix.txt`)

NVRTC 12.8, source `src/rtc_kernel.cu` = same headers and same fused kernel body, N=4, 3 reps each:

| route | compile ms (min/med) | module load ms | note |
|---|---|---|---|
| PTX compute_120, driver JIT cache on | 445 / 447 | 1.5 (cache hit) | |
| PTX, `CUDA_CACHE_DISABLE=1` (cold JIT) | 453 / 454 | 645 / 651 | **~1.1 s total** edit-to-loadable |
| CUBIN sm_120 | 1063-1066 / 1078 | 1.6 (49 with cache disabled) | ~1.1 s total |
| CUBIN `--pch` | 1050 | | no gain |
| CUBIN `--split-compile=0` | 1062 | | 157 regs, 8% slower kernel |

The runtime-compiled kernel is identical in registers (148, 32 B stack), time (1.26-1.28 ms median) and output checksum to the nvcc build. First launch after load 1.3 ms. Conclusion: a one-kernel edit loop is about one second. (Not tested: many kernels, include-heavy sources with `--include-path` over a large tree, NVRTC `--optix-ir`.)

OptiX 9.1 (`optix_test.exe`): the same fused body as a raygen program (no ray traced), compiled `nvcc -ptx|-optix-ir -arch=compute_120 -O3`: PTX 0.87 s (274 KB), OptiX-IR 0.70 s (182 KB). Both load and launch. `optixModuleCreate` with the disk cache disabled (`optixDeviceContextSetCacheEnabled(ctx,0)`): PTX 984 ms, OptiX-IR 1054 ms; with cache 0.6 ms; pipeline create 0.5 ms; context create ~85 ms. Launch median: PTX 1.099 ms, OptiX-IR 1.118 ms (vs 1.273 for the same kernel under plain CUDA, inferred register allocation difference, not inspected), same pdf checksum. So PTX and OptiX-IR are equivalent here for load time and speed; OptiX-IR is a smaller input and cheaper front end. No traversal, SER, or hit programs were tested.

## 6. CPU/GPU agreement (measured; `out/results/agreement.txt`)

1e6 inputs (identical bits on all executables: inputs generated on the host in float by shared code). Stages compared: BSDF eval (value, pdf), BSDF sample (direction, value, pdf, resulting throughput), NEE (contribution, direction, light pdf). Executables: GPU float default (`-fmad=true`), GPU float `-fmad=false`, CPU float (`/O2`, no FMA), CPU float `/arch:AVX2 /fp:contract`, CPU double. Discrete choices (lobe, light) matched on all 1e6 inputs in every pair (0 flips), so the flip class did not occur.

Vector-level error, 4-wavelength relative L2 error for values and angle (rad) for directions (float eps 1.2e-7). Columns: max / p99 / p99.9.

| pair (second = reference) | eval.f | sample throughput | NEE contrib | sample dir angle |
|---|---|---|---|---|
| GPU fma vs CPU float | 1.1e-2 / 3.4e-6 / 2.4e-5 | 0.62 / 1.0e-5 / 1.7e-4 | 2.2e-2 / 7.0e-6 / 5.5e-5 | 9.5e-2 / 6e-7 / 3.4e-6 |
| **GPU no-fma vs CPU float** | 1.1e-6 / 1.1e-7 / 2.6e-7 | 1.6e-2 / 4e-7 / 1.4e-5 | 5.8e-4 / 1.8e-7 / 1.1e-6 | 4.6e-4 / 2.7e-7 / 1.2e-6 |
| GPU fma vs CPU float+FMA | 1.1e-2 / 2.8e-6 / 1.8e-5 | 1.9e-2 / 2.5e-6 / 9.4e-5 | 1.3e-2 / 4.2e-6 / 2.4e-5 | 6.4e-2 / 4.5e-7 / 2.0e-6 |
| CPU float vs CPU double | 8.2e-3 / 4.4e-6 / 2.9e-5 | 0.22 / 1.7e-5 / 1.4e-4 | 8.6e-2 / 1.4e-5 / 1.1e-4 | 0.23 / 6.6e-7 / 3.8e-6 |
| GPU fma vs CPU double | 1.2e-2 / 4.2e-6 / 2.7e-5 | 0.62 / 1.6e-5 / 1.3e-4 | 6.1e-2 / 1.3e-5 / 1.0e-4 | 0.23 / 6.4e-7 / 3.7e-6 |
| GPU no-fma vs CPU double | 8.2e-3 / 4.4e-6 / 2.9e-5 | 0.22 / 1.7e-5 / 1.4e-4 | 8.6e-2 / 1.4e-5 / 1.1e-4 | 0.23 / 6.8e-7 / 3.9e-6 |

Findings:
- **FMA contraction is the dominant GPU-vs-CPU difference.** With `-fmad=false` the GPU matches CPU float to 1e-6 max on eval (max 14 ULP on f, 66 ULP on pdf; light pdf bit-exact), so the differing libm functions (sinf/cosf/expf/powf/sqrt paths) contribute only at the ULP level here. With FMA on, max eval error is 1e-2.
- **GPU(FMA) vs CPU float is no worse than CPU float vs CPU double** (p99.9 2.4e-5 vs 2.9e-5; the tails are float conditioning, not a GPU defect). A GPU-vs-CPU test should compare each to a double reference with float-conditioning tolerances (about 1e-4 at p99.9 for values, 4e-6 rad for directions), not demand bit equality.
- **Badly conditioned classes** (percent of values > 64 ULP vs the double reference): `sample.f` and `sample.pdf` evaluated at a sampled direction: clear glass 98.6%, gold 68.6%, coated 26.5%, thin film 24.6% (near-delta lobes: a 1-ULP direction shift changes f by orders of magnitude). The estimator weight `f cos/pdf` (sample.throughput) is well conditioned: 0.03% (glass), 0.29% (gold), 2.3% (coated), 10.8% (thin film, per-wavelength cosine of a phase) . Test throughput, not raw f at a sampled direction. NEE light pdf and direction show 2.5-6% > 64 ULP vs double independent of material class, from cancellation in `pt - p` (relative/ULP metrics also inflate for components near zero). Eval error is spread across cos(theta_o) bins with no clear grazing concentration (0.6%, 0.4%, 0.8% > 64 ULP for <0.05, <0.25, rest).
- Worst-case single inputs: eval 1e-2 relative (FMA), sampled-direction angle 0.1-0.23 rad on a handful of inputs (specular lobe edge cases), throughput 0.6 relative on one input.

## 7. CPU throughput (measured; `agreement.txt`, best of 3 passes, single thread, 1e6 evaluations)

| stage (includes record unpack + lobe prep) | float | double | float + FMA |
|---|---|---|---|
| BSDF eval + pdf | 2.90 M/s | 2.73 M/s | 3.06 M/s |
| BSDF sample + eval + update | 1.78 M/s | 1.66 M/s | 1.85 M/s |
| NEE + shadow record | 2.17 M/s | 2.04 M/s | 2.27 M/s |

About 0.35 us per BSDF evaluation. A 10 s pre-commit budget allows ~25M evaluations single-threaded (~200M on 8 cores, inferred). A 64x64 furnace test at 256 spp is 1M samples = under 1 s. The CPU target compiles in 0.65 s, so lobe unit tests fit the 10 s budget with a large margin. Double costs only 6% over float.

## 8. Surprises

1. The fused kernel is far below the saturation seen in v1 (148 vs 254) at the same structural scope; v1's extra pressure must come from things this spike omitted.
2. `__launch_bounds__` works and is the best lever (+56%), contradicting the v1 note.
3. `__noinline__` is harmful (37% slower, up to 1.4 KB stack), and the by-value record is the reason: by-pointer reverses most of it.
4. nvcc compile is seconds, not minutes, per TU; the cost is front end, not ptxas. `--Ofast-compile=max` is a trap (100x slower kernel).
5. FMA contraction, not libm, explains the CPU/GPU gap; a trivial `-fmad=false` build collapses the error by 4 orders of magnitude.
6. A separate bytecode-VM parameter kernel is as expensive as the whole fused shade (1.18 vs 1.27 ms) and a fused VM adds only 0.32 ms.
7. The GPU was shared with a running Blender session during early runs (not in the final numbers); a hand-rolled `run_all` initially lost its compile timings to a shell-scoping problem (fixed: each step is its own process).

## Verdict on the bet: **partly holds**

Holds on this spike's scope: (a) no kernel spills or saturates at N=4 (fused 148, split 122/116), launch bounds work, so the registers can be steered; (b) compile is seconds (1.3-2.5 s per TU, 11 s for a full 34-TU build, ~1.1 s NVRTC/OptiX edit loop); (c) CPU and GPU agree to float conditioning, and to 1e-6 with FMA off. Not shown: that this survives full production scope. The spike is a lower bound on live state, N=8 fused already exceeds the 168 target (197), and nothing here includes upsampling, texture fetches, closure graphs or traversal.

Three most decision-relevant numbers:
1. Fused N=4 shade+NEE: **148 registers, 0 spills, 1.27 ms / 2^20 paths; with `__launch_bounds__(256,2)` 128 regs, 68 B spill, 0.82 ms (+56%)**. N=8: 197 regs uncapped.
2. Compile: **1.3-2.5 s per TU, 11.3 s full build at -j8, ~1.1 s NVRTC edit-to-launch** (v1: up to 30 min per TU).
3. CPU/GPU: **max eval error 1.1e-2 with FMA, 1.1e-6 without; GPU-vs-CPU float error equals CPU-float-vs-double error (p99.9 2.4e-5 vs 2.9e-5)**.

What I could not measure: behaviour at v1 scope (spectral upsampling tables, closure graph, many material variants); SER/traversal and OptiX with hit programs; device-linked (-rdc) final register counts (so the cause of the v1 "launch bounds ignored" note is unconfirmed); Nsight occupancy/stall counters (occupancy is inferred); `__launch_bounds__` effect with real material sorting; Slang comparison (not in this spike's brief); compiler-cache hit rates (no cache used); cold-disk compile times (OS file cache was warm).

Follow-up should test: (1) grow the spike toward v1 scope in steps (add Jakob-Hanika upsampling lookups, a closure/layer graph, per-material variants, ray traversal via OptiX hit programs), recording registers at each step to find what pushed v1 to 254; (2) repeat launch-bounds and noinline experiments under `-rdc` with final linked counts, to explain the v1 observation; (3) sweep the launch-bounds value (min blocks 2/3/4 at 128/168 caps) at full scope; (4) Nsight Compute on `lb` vs `base` to confirm the occupancy explanation; (5) CPU/GPU tests per section 6: throughput-weight comparisons and a double reference, FMA policy decision (`-fmad=false` for a test build).

## Reproduce

`powershell -NoProfile -ExecutionPolicy Bypass -File run_all.ps1` (from `C:\Astroray2.0\spikes\A-kernel-budget`). It runs: versions, `gen_ninja.ps1` + clean ninja build, `measure_regs.ps1` (regs.csv), `measure_runtime.ps1` (3x bench), `measure_agree.ps1`, `measure_rtc_optix.ps1`, `rdc_probe.ps1`, `measure_compile.ps1`. The verified end-to-end run predates the addition of `rdc_probe.ps1` to `run_all.ps1`; `rdc_probe.ps1` was run standalone and `out/results/rdc_probe.txt` is its output. Source files in `include/` and `src/`; `build.ninja` is generated.
