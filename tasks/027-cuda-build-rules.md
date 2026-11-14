# 027 GPU build rules: one cubin per stage, nofma and probe variants, embedding
Goal: no device code can be built -> CMake builds one cubin per stage TU with the production flags, the nofma and probe variants from the same sources, test-only GPU modules, and embeds stage cubins and data files into astroray_core as RCDATA (loose copies in out/bin/kernels for the dev preset only).
Read first: 0002 D1 (enforcement flags), D10 (production build line), D12 (FMA policy), D14 (driver-API module images, banned flags); 0009 d3 (GPU targets), d4 (dev preset), d7 (nvcc pool depth), R13, R16.
Scope: cmake/kernels.cmake (GPU part), cmake/sources.cmake, CMakeLists.txt, cmake/data.cmake (shared RCDATA generation), tools/lint.py (section `flags`), tests/cpu/lint_cases/flags/, tests/gpu/kernels/selftest.cu.
Out of scope: CudaDevice and module loading (028); OptiX PTX (041); the resource gate (030).
Reference: none (flags from 0002).
Do:
1. When `ASTRORAY_GPU` is ON: `enable_language(CUDA)` only to locate nvcc (`CUDAToolkit_NVCC_EXECUTABLE`); every device build is an explicit `add_custom_command` calling nvcc, in a Ninja job pool `nvcc` of depth 8 (PROVISIONAL, 0009 d7).
2. By 003's source rule every src/kernel_gpu/*.cu is a stage built to out/build/kernels/<name>.cubin, except by file-name prefix: `optix_*.cu` take the PTX rule (041), `probe_*.cu` build only into the probe target (`check full`), `test_*.cu` build only with `AR_GATE_HOOKS` and are never embedded in a release. Stage flags: `-cubin -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 --fmad=true -prec-div=true -prec-sqrt=true -ftz=false -Werror all-warnings -Xptxas -v -Xptxas --warn-on-double-precision-use` (verify the last flag's exact spelling with `ptxas --help` from CUDA 12.8 and record it in Progress, 0002 Risk 9); ptxas output saved to out/build/kernels/<name>.ptxas.txt. The same source also builds into target `astroray_kernels_gpu_nofma` (`--fmad=false`, out/build/kernels_nofma/) and `astroray_kernels_gpu_probe` (`-DAR_LAUNCH_BOUNDS=0`, out/build/kernels_probe/, EXCLUDE_FROM_ALL; built by `check full`). An option lets `advance_geodesic_f64` (later) skip the double-use warning; nothing else may.
3. tests/gpu/kernels/*.cu (globbed) are test-only modules with the same production and nofma rules, never embedded; selftest.cu holds one trivial `extern "C" __global__` kernel used by task 028.
4. Embedding: one generated .rc in out/build/gen/ lists every stage cubin (name `K_<NAME>`) and every data file of data/MANIFEST.toml (name `D_<NAME>`) as RCDATA and is compiled into astroray_core; the dev preset defines `AR_DEV_KERNEL_DIR` and copies cubins to out/bin/kernels/ after each build; the release build (later) writes no loose module.
5. lint section `flags`: no `--use_fast_math`, `-rdc`, `--Ofast-compile`, `--split-compile` or `/fp:fast` anywhere under cmake/ or CMakeLists.txt; lint cases.
Acceptance: `pwsh tools/build.ps1` builds selftest.cubin in both variants and its .ptxas.txt contains `Used <n> registers`; `pwsh tools/check.ps1 fast` passes (fast still builds no device code); `<python> tools/lint.py --self-test` includes the `flags` cases; `dumpbin /headers out/bin/astroray_core.dll` shows a `.rsrc` section.
Look at: none.
Canary: tasks/027.canary.patch must turn red: `tools/lint.py --self-test` (the patch drops `-rdc` from the banned list).
Budget: adds the first device compile; report clean `dev` build time (B-01 basis) in Progress.
Depends on: 018, 022.
Size: M (about 300 lines CMake); stop and report at 2x.
Progress:
