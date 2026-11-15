# Spike F, part A: DLSS Ray Reconstruction from our own process

Date 2026-11-15. Branch `spike/F-dlss`. Spike code is throwaway and lives in `out/spikes/F/` (git-ignored);
nothing from NVIDIA is committed. This file is the only committed artefact.

## Question

Can our own process use DLSS-RR through NVIDIA NGX loaded from the driver, with no NVIDIA DLL shipped by us
and our own project ID (0008 decisions 16-20, V12 part A (a)-(e), Risks 4)?

## Verdict

| Criterion | Result |
|---|---|
| (a) `GetFeatureRequirements` supported, no DLL beside either executable | PASS, with one ordering condition (a CUDA context must already exist) |
| (b) project-ID init works without Blender's ID | PASS |
| (c) pack + evaluate, per preset, 1080p and 4K, at least one preset meets 0009 B-27 | PASS (every preset meets it) |
| (d) memory within the guide's 154-579 MB | FAIL as worded; see (d). The guide's figure is reproduced for the Performance preset at 1080p, and memory grows with the preset. The criterion needs a lead ruling. |
| (e) recorded: cold init, recreate per preset | recorded below |

(a), (b), (c) hold in a command-line host and, with identical results, inside Blender 5.3 through ctypes.
V12 says all of (a)-(d) must pass for DLSS to ship as an option, so the strict reading of (d) leaves Part A
open for a lead ruling. I did not change any threshold.

## Setup

- GPU RTX 5070 Ti, **NVIDIA driver 621.24** (`nvidia-smi --query-gpu=name,driver_version --format=csv,noheader`).
- Windows 11, MSVC 14.44 (`vcvarsall x64 -vcvars_ver=14.44`, `/Zc:preprocessor` is required by the SDK macros),
  CUDA 12.8 (`nvcc -arch=sm_120`), Blender 5.3.0 (hash 02f0899d78e3), Python 3.13.13.
- `tools/` has no build or check scripts yet, so `out/spikes/F/build.cmd` called nvcc and cl directly. This is a
  departure from CLAUDE.md's environment rule that only applies because the scripts do not exist.
- SDK: `https://github.com/NVIDIA/DLSS`, commit `9981c6350dc58c0ce21f024fd65e31e851c31689`
  (2026-10-16, "DLSS 310.9.16 SDK"), cloned to `out/spikes/F/_sdk`. Only `include/*.h` and `doc/*.pdf` were used.
  Licence: `LICENSE.txt` "NVIDIA RTX SDKs LICENSE" (v. March 14, 2024), sha256
  `3027f23ca5a46dd9cb8183fbd522983a86f64d7daac5982912bf9f214671f294`. The SDK's `lib/` was fetched by a git
  mistake and deleted; no SDK binary was used by any test below.
- Integration: header-only loader (`NVSDK_NGX_HEADER_ONLY`), `NVSDK_NGX_CUDA_Init_with_ProjectID`, a random
  spike-only GUID `b2a5d417-9c1d-496a-9572-4ada992358b7`, `NVSDK_NGX_ENGINE_TYPE_CUSTOM`. Feature create/evaluate via
  `NGX_CUDA_CREATE_DLSSD_EXT` / `NGX_CUDA_EVALUATE_DLSSD_EXT`, flags `IsHDR | MVLowRes`, packed roughness in
  normals.w, linear depth, RGBA16F colour and guides, RG16F motion vectors, Halton(2,3) jitter with 64 phases.
- Synthetic scene (CUDA kernels in `out/spikes/F/src/kernels.cu`): checker floor, striped glossy sphere with an HDR
  highlight, sky; per-pixel noise 0.7 plus rare fireflies; known analytic ground truth.
- Commands (all from `out/spikes/F`):
  - `cmd /c .\build.cmd` builds `bin\spikef.exe` and `bin\spikef_dll.dll`.
  - `.\bin\spikef.exe <outdir> abc 0` is the command-line host (stages a, b, c; `0` = full frame counts).
  - `blender.exe -b --factory-startup --python blender_spikef.py` loads `spikef_dll.dll` with ctypes and calls
    `spf_run("abc", ...)`, the same code as the host.
- Timing method: 40 burn-in frames then 400 timed frames per configuration, min of N reported with the median.

## (a) GetFeatureRequirements, no DLL beside the exe: PASS

`.\bin\spikef.exe ...\run_final_exe abc 0`:

```
host executable: ...\out\spikes\F\bin\spikef.exe
  no nvngx*/_nvngx*/*dlss* file beside the executable (criterion (a) precondition holds)
  a1 GetFeatureRequirements, cold (no CUDA, no NGX init) -> 0xBAD00002 FAIL_PlatformError
  a2 GetFeatureRequirements, CUDA ctx exists, before NGX init -> 0x00000001 Success
    FeatureSupported=0x0 (0 = Supported) MinHWArchitecture=0x160 MinOSVersion='10.0.0'  [3304.84 ms]
  a3 GetFeatureRequirements, after NGX project-ID Init -> 0x00000001 Success
    FeatureSupported=0x0 (0 = Supported) MinHWArchitecture=0x160 MinOSVersion='10.0.0'  [1.80 ms]
after Init: loaded NGX-related modules:
    C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_81cce6c273d405a6\_nvngx.dll
    C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_81cce6c273d405a6\ngx-builtins\nvngx_dlssd.dll
```

- Supported with nothing beside the exe. The loader takes `_nvngx.dll` from the driver store (path from the
  `D3DKMT` adapter registry value) and the driver supplies the DLSS-RR snippet from its own `ngx-builtins\`
  folder. The NGX log line: `Found dlssd driver builtin snippet ... ngx-builtins ... nvngx_dlssd.dll version: 310.9.0`.
- Same result inside Blender (`run_blender_full.txt`): `ngx/dlss files beside blender.exe: none`, `a3 ... Success`, the
  same driver-store `nvngx_dlssd.dll` loaded.
- Condition found: the call returns `FAIL_PlatformError` when no CUDA context exists yet (a1). The engine's order
  in decision 19 (CUDA context, init, then query) is the passing one; a query before the context exists must not be
  read as "unsupported".
- The first call that loads NGX is slow (1.6 s to 5.7 s across runs, that is a2 above); the later call is 1.4-2.6 ms.
- For information, with the driver's own `nvngx_dlssd.dll` (version 310.9.0) copied beside the exe
  (`bin_withdll`, `run_withdll.txt`): it also works, and NGX then loads the local copy
  (`...\bin_withdll\nvngx_dlssd.dll` in the module list). So the DLL is neither required nor harmful, and a stray local
  copy would override the driver's. No failure to map to (a)-(d) occurred.

## (b) Project-ID init without Blender's ID: PASS

```
  project id b2a5d417-9c1d-496a-9572-4ada992358b7 engine CUSTOM version 0.0.0-spike-F, NGX_Version_API=0x15
  CUDA_Init_with_ProjectID           -> 0x00000001 Success
  SuperSamplingDenoising.Available=1  NeedsUpdatedDriver=0  MinDriver=537.2  FeatureInitResult=0x1
```

- Only `NVSDK_NGX_CUDA_Init_with_ProjectID` was called; no application ID appears anywhere in the program.
- The NGX log shows how the driver treated the ID: `MapProjectId: Found cms id 876232c for engine: custom
  engineVersion 0.0.0-spike-F projectID b2a5d417-...`, and every feature reports `denial value 0 for Cms ID
  876232c`. A GUID-shaped ID with the CUSTOM engine type maps to NVIDIA's generic custom entry, so no registration
  is needed, and the ID buys no application-specific over-the-air presets.
- Same result inside Blender 5.3.

## (c) Pack plus evaluate per preset: PASS

The gated number is the wall-clock time of the pack kernel plus `EvaluateFeature`, with `cudaDeviceSynchronize`
before and after each frame, so it includes host call cost. The CUDA-event numbers in the logs are lower and
unreliable (NGX evaluate does not serialise fully against events on our stream: evaluate-only events read 0.000 ms),
so they are not used. The pack kernel converts fp32 film-style linear planes (colour, albedo, specular albedo, normals
with roughness, depth, motion vectors) to the half-float arrays behind the `CUtexObject`s.

B-27 is "at most B(rate)/2" with B = 1/rate minus t_present (0009 B-27, 0008 Context). t_present is not measured in
this spike, so both limits are shown: t_present = 0 and a 6 ms placeholder. Rates: 30/s for 1080p, 24/s floor for 4K.

```
  1080p: B = 33.33 ms (t_present=0) or 27.33 ms (6 ms placeholder); B-27 limit = 16.67 / 13.67 ms
    DLAA         render 1920x1080 pack+eval WALL min 3.558 ms  median 3.775 ms
    Quality      render 1280x720  pack+eval WALL min 1.861 ms  median 1.927 ms
    Balanced     render 1114x626  pack+eval WALL min 1.543 ms  median 1.590 ms
    Performance  render  960x540  pack+eval WALL min 1.235 ms  median 1.273 ms
    UltraPerf    render  640x360  pack+eval WALL min 0.915 ms  median 0.950 ms
  4K: B = 41.67 ms or 35.67 ms; B-27 limit = 20.83 / 17.83 ms
    DLAA         render 3840x2160 pack+eval WALL min 13.691 ms  median 14.475 ms
    Quality      render 2560x1440 pack+eval WALL min 6.818 ms   median 7.234 ms
    Balanced     render 2227x1253 pack+eval WALL min 5.634 ms   median 5.917 ms
    Performance  render 1920x1080 pack+eval WALL min 4.497 ms   median 4.744 ms
    UltraPerf    render 1280x720  pack+eval WALL min 2.843 ms   median 2.963 ms
(c) PASS
```

- All ten configurations meet the limit under both t_present assumptions, DLAA at 4K included (13.7 ms against 17.8 ms).
  The render resolutions come from `NGX_DLSSD_GET_OPTIMAL_SETTINGS` and match decision 20's presets (0.667, 0.58, 0.5, 0.333).
- Occasional frames take several times the median (p95 reached 10-64 ms in loaded runs); min-of-N is the stable figure.
- The Blender run gave min times within 3% of the host run (for example 1080p Quality 1.855 ms, 4K DLAA 14.039 ms).
- What this does not cover: the real pack kernel reads the engine's film planes; render time of the guides; the
  specular-hit-distance variant (decision 22), because this spike passed specular motion vectors equal to the
  camera motion vectors; hit distance plus matrices was not exercised.
- Image check (Read of `run_img/q1080_out.png` and `q1080_noisy.png`, 1080p Quality, 1280x720 noisy input): the
  output is clean and 1920x1080, checker edges and the sphere's stripes and highlight are kept. Numerically, the
  high-frequency energy on the floor drops from 32.1 (noisy, nearest-upscaled) to 0.38, and the tonemapped RMSE against
  the analytic ground truth is 0.02-0.10 for the output against 0.15-0.17 for the noisy input in the host log (the run-to-run spread is large because the temporal history here is only the synthetic pan).
  Zero non-finite output pixels in all ten configurations. This is a sanity check of the API wiring, not a
  quality evaluation (that is part B).
- Inconclusive: a matrix of motion-vector sign and jitter sign gave RMSE values that changed order between runs, so
  this spike makes no claim about the correct sign conventions (prev minus current motion, positive jitter was used
  for the timing runs). Part B must settle them.

## (d) Memory: FAIL as worded (guide figure reproduced for Performance)

Measured as NGX's own `NGX_DLSSD_GET_STATS` with `FreeMemOnReleaseFeature = 1` (cache empty before each create,
stats back to 0.00 MB after release), cross-checked with `cudaMemGetInfo`:

```
    1080p DLAA         NGX stats  366.41 MB, cudaMemGetInfo delta  392.00 MB
    1080p Quality      NGX stats  210.34 MB, cudaMemGetInfo delta  222.00 MB
    1080p Balanced     NGX stats  179.77 MB, cudaMemGetInfo delta  194.00 MB
    1080p Performance  NGX stats  153.98 MB, cudaMemGetInfo delta  178.00 MB
    1080p UltraPerf    NGX stats  115.93 MB, cudaMemGetInfo delta  122.00 MB
    4K    DLAA         NGX stats 1446.40 MB, cudaMemGetInfo delta 1464.00 MB
    4K    Quality      NGX stats  814.41 MB, cudaMemGetInfo delta  844.00 MB
    4K    Balanced     NGX stats  699.86 MB, cudaMemGetInfo delta  722.00 MB
    4K    Performance  NGX stats  596.68 MB, cudaMemGetInfo delta  628.00 MB
    4K    UltraPerf    NGX stats  440.60 MB, cudaMemGetInfo delta  460.00 MB
```

- The guide's Table 1 (RTX 50 series, presets Default/F) gives 154.08 MB at 1080p and 578.84 MB at 4K. Performance
  at 1080p gives 153.98 MB, so the guide's 154 MB figure is the Performance-mode value. At 4K Performance we measure
  596.68 MB, 3.1% above 578.84 MB.
- Read literally, "within 154-579 MB" is not met: 4K Performance is 18 MB over, and the heavier presets are far over
  (4K Quality 814 MB, 4K DLAA 1446 MB; 1080p DLAA 366 MB). Memory depends on render plus output resolution and the preset,
  so one range cannot hold for all presets. This is a mismatch between the criterion and the measured quantity, not
  a driver failure. The numbers above are what 0007's VRAM ledger needs for the denoiser category, per preset.
  The criterion's wording is the lead's to rule on.
- The default `ReleaseFeature` keeps the memory cached inside NGX (the recreate timings below use that behaviour);
  `FreeMemOnReleaseFeature = 1` frees it on release.

## (e) Recorded: cold init and feature recreate

- Cold paths in a fresh process, driver 621.24, CUDA context already created:
  - `NVSDK_NGX_CUDA_Init_with_ProjectID` alone (`spikef.exe <dir> b 1`, three runs): 2155, 2355, 2387 ms.
    Other runs measured 1762-3356 ms, one first-ever run 10417 ms, and 4065-4882 ms inside Blender.
    The first `GetFeatureRequirements` that loads NGX before init costs 1.6-5.7 s; the engine's order pays this
    once, inside init, off the first-frame path (decision 16).
  - First `CreateFeature` in the process (the DLSS-RR snippet is already loaded by init): 37-149 ms.
  - First `EvaluateFeature` in the process: 0.7-2.0 ms.
- Feature recreate per preset step (decision 20): `ReleaseFeature` (about 0.02 ms) plus `CreateFeature`,
  median of 6 recreates in the final host run (memory cached; the run before it, on a quieter machine, gave medians
  of 29.5-50.2 ms, minimum 26.2 ms):

```
  output   preset       create+release median
  1080p    DLAA          51.1 ms
  1080p    Quality       64.2 ms
  1080p    Balanced      67.6 ms
  1080p    Performance   76.5 ms
  1080p    UltraPerf     73.2 ms
  4K       DLAA          95.1 ms
  4K       Quality       91.7 ms
  4K       Balanced     134.9 ms
  4K       Performance   56.4 ms
  4K       UltraPerf     72.0 ms
```

  Recreate cost is therefore 26-135 ms depending on machine load, with the first evaluate after it 0.3-0.6 ms.
  History resets with the new feature (`InReset = 1` on the first frame). 0007's hysteresis should assume a recreate of
  up to about 150 ms per preset step.

## Other findings for the implementation

- The header-only loader declares but does not define the `NVSDK_NGX_Parameter_Set*/Get*` C helpers (the SDK's static
  library normally does). They forward to the parameter interface's virtual `Set`/`Get`; we defined them in the one
  translation unit. `src/denoise/dlss/` needs the same, and MSVC needs `/Zc:preprocessor`.
- `NVSDK_NGX_CUDA_GetFeatureRequirements`, `GetCapabilityParameters`, `NGX_DLSSD_GET_OPTIMAL_SETTINGS` and
  `NGX_DLSSD_GET_STATS` all worked on the capability parameter object. The log callback works with
  `DisableOtherLoggingSinks = true` (without it the NGX console sink floods stdout).
- Evaluate inputs are pointers to `CUtexObject` handles (arrays with `cudaArraySurfaceLoadStore`, texture objects for
  reads); the output is a pointer to a surface object. The driver 621.24 default snippet was version 310.9.0, and the
  log reports the default preset as F for all modes.
- Not tested: driver 620.x itself (only 621.24), other GPU generations, Blender running Cycles DLSS in the same
  process (a second NGX user in one process), the depth-of-field guide, alpha upscaling, and hit-distance guides.

## What 0008 says follows

- V12 Part A: all of (a)-(d) must pass for DLSS to ship as an option; otherwise the DLSS-only items (specular hit
  distance, Halton jitter, the pack kernel, preset mapping) are not built and the OIDN guides and motion stay.
  (a), (b), (c) pass. (d) fails as worded for a reason that is the criterion's, so DLSS can go ahead only if the lead
  rules (d) satisfied by per-preset memory figures (the table above) or rewrites it; otherwise 0008's fallback applies.
- Risks item 4 ("DLSS availability to a third-party process without a local DLL on driver 620"): resolved on driver
  621.24 for our process and for Blender's process; driver 620.x was not available to test.
- CHECKPOINT-1: `docs/ARCHITECTURE.md` sign-off waits on V12 part A; the open item is the (d) ruling.
- Decision 20: the recreate times above are the input to 0007's hysteresis. Decision 19: the CUDA context must exist
  before the first `GetFeatureRequirements`, and the first NGX load is 2-5 s, which decision 16 already keeps off the
  first-frame path. Part B (quality, ghosting, jitter and motion-vector conventions) is unchanged.
