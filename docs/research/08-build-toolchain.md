# Build and toolchain for fast iteration on Windows (2026-11-14)

Tags: **V** = I read the primary source (URL given). **L** = checked on this machine today. **R** = reported by a secondary or forum source. **U** = unverified (inference or not found).

## Question

How should Astroray 2.0 build on Windows (RTX 5070 Ti, 8c/16t, 63 GB RAM) so that an incremental build stays under 30 s, a clean build under 5 min, and kernel edits show up in seconds? That covers the host compiler, nvcc, the Blender 5.3 ABI, extension packaging, caching, the Python boundary, runtime kernel compilation, dependencies, CI and a repository layout. The GPU backend (NVIDIA-only or vendor-neutral) is decided elsewhere, so each recommendation says whether it depends on that decision.

## Findings

### 1. Host compilers and CUDA 13.x

- **V** The CUDA 13.5 Windows guide lists only MSVC as a host compiler: 195x (VS 2026), 193x (VS 2022), 192x (VS 2019). Clang-cl is not mentioned. https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html
- **V** The CUDA 13.0 archive table lists 193x and 192x only, and says VS 2017 was dropped in 13.0. https://docs.nvidia.com/cuda/archive/13.0.0/cuda-installation-guide-microsoft-windows/index.html
- **V** The 13.0 release notes add Clang 20 and GCC 15 as host compilers; I could not tell whether that is Linux-only. They also remove Maxwell, Pascal and Volta offline compilation, and stop bundling the Windows display driver. https://docs.nvidia.com/cuda/archive/13.0.0/cuda-toolkit-release-notes/index.html
- **V** Blender's own `platform_win32.cmake` builds C/C++ with clang-cl but sets `CUDA_HOST_COMPILER` to `cl.exe`. Its comment says that without this CMake would pick clang-cl "and the cuda build will fail due to a non-supported compiler" (re-read in the verification pass; the earlier quoted wording was a paraphrase). The `cl.exe` default applies only if `CUDA_HOST_COMPILER` is not already defined. https://raw.githubusercontent.com/blender/blender/main/build_files/cmake/platform/platform_win32.cmake
- **R** CMake issue titles "CUDA: using Microsoft compiler with Clang is not working on Windows" (#25648). I could not open the page (403), so the status is unknown. https://gitlab.kitware.com/cmake/cmake/-/issues/25648
- **V** The LLVM docs say Clang compiling CUDA on Windows "should work, but your mileage may vary". https://llvm.org/docs/CompileCudaWithLLVM.html
- **V** Toolkit versions seen today: CUDA 13.5 is the latest, with 13.3 on 2026-05-27 (**R**, Phoronix). Driver branches per release: 13.0 R580, 13.1 R590, 13.2 R595, 13.3 R610, 13.4 R615, 13.5 R620 for new features. The minor-version-compatibility floor is R580. https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html
- **L** This machine has CUDA 12.6 and 12.8 (no 13.x), VS 2022 Community 17.10, VS Build Tools 2022 17.14, CMake 4.2.1, Ninja 1.13.2, sccache 0.17.0, driver 621.24, compute capability 12.0 (so `sm_120`). There is no ccache or clang-cl.
- **L** HAGS is on here (`HwSchMode` = 2), so the HAGS report in section 5 applies to this machine.

### 2. Blender 5.3: Python, runtime, ABI

- **V** The 5.3 release branch pins Python 3.13.13, NumPy 2.3.4 and OIDN 2.5.0. https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/build_files/build_environment/cmake/versions.cmake `main` (5.4 dev) shows the same three versions. The fetch tool summarised both files, so the version strings are second-hand.
- **V** Python moved from 3.11 to 3.13 in 5.1 "matching the VFX platform 2026". https://developer.blender.org/docs/release_notes/5.1/python_api/ VFX CY2027 also specifies 3.13.x, NumPy 2.4.x and MSVC toolset 14.44+. https://vfxplatform.com/
- **L** Blender 5.1 and 5.2 installs here contain `python313.dll`, `python3.dll` (the stable-ABI stub), `_ctypes.pyd`, `libffi-8.dll`, and NumPy 2.3.4. Their `blender.crt\` folder holds `vcruntime140.dll` and `msvcp140*.dll` version 14.44.35211 (VS 2022 17.14).
- **V** Blender 5.3 requires MSVC 17.14.14 or newer to build, uses `/MD`, and prefers Python 3.13. Same `platform_win32.cmake` URLs as above (release branch). So 5.3 very likely ships a 14.44+ runtime, but I have no 5.3 install to confirm (**U**).
- **V** Microsoft: VS 2015 to 2026 binaries are mutually compatible, but the redistributable must be at least as new as the newest toolset used by any component. Static libraries built with `/GL` or `/LTCG` are not compatible across versions. https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017?view=msvc-170
- **L/V** Blender 5.3's own minimum is MSVC 19.44.35216 (17.14.14), which is newer than the 14.44.35211 runtime in the local 5.1/5.2 installs. So 5.3 probably ships a runtime of 35216 or later, but this is still unconfirmed. A local toolset 14.44.x at or below that build number is the safe pin.
- Consequence (my inference): a DLL built with a toolset newer than the runtime Blender loaded can fail at load or call time. Pin the MSVC toolset to Blender's bundled runtime or older, or link the engine DLL's CRT statically (`/MT`). Pinning by build number is fragile because the runtime Blender ships can lag the toolset in a current VS update.
- **V** Python stable ABI (`abi3`): a module built with `Py_LIMITED_API` set to the 3.13 value runs on 3.13 and later. On Windows it links `python3.dll`. The limited API disables some inlining and macros. https://docs.python.org/3.13/c-api/stable.html
- **V** `ctypes.CDLL` and `WinDLL` release the GIL during calls; `PyDLL` does not. Pass a full path to `CDLL`; a missing dependent DLL gives `WinError 126`. https://docs.python.org/3.13/library/ctypes.html
- **R** 5.4 is in alpha until 2027-02-03 (from the recon file); I did not find a 5.4 Python version. Both the 5.4-dev `versions.cmake` and CY2027 say 3.13.

### 3. Extension packaging of native binaries

- **V** Manifest `platforms` accepts `windows-x64`, `windows-arm64`, `macos-x64`, `macos-arm64`, `linux-x64`. If omitted, the extension is offered on all platforms. `[build] paths` lists files to include. https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/advanced/extensions/getting_started.rst
- **V** `blender --command extension build` builds the zip, `blender --command extension validate` checks it, and `--split-platforms` builds one zip per listed platform. https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/advanced/extensions/python_wheels.rst
- **V** The manual says wheels are the documented route for compiled files ("wheels for all supported platforms should be included") and that wheels "must be bundled unmodified from Python's package index". The same page says wheels can severely increase extension size.
- **V** The moderation guidelines require add-ons to be self-contained and say nothing about raw DLLs or `.pyd` files; they send doubtful cases to the moderators room. https://developer.blender.org/docs/features/extensions/moderation/guidelines/
- **V** `blender_ext.py` documents no file-type or size restriction on zips. https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/scripts/addons_core/bl_pkg/cli/blender_ext.py
- **U** Whether extensions.blender.org accepts a raw `.dll` in the zip (outside a wheel) is not documented. A self-hosted repository or a local install avoids the question. The extension licence must also be GPL-compatible for that site; I did not check that rule.
- **U** The 5.3 manual pages are too large for my fetch tool. I used the manual source files from projects.blender.org, which are the same text but may lag the 5.3 branch.

### 4. CMake, Ninja, nvcc on Windows

- **V** CMake `CUDA_ARCHITECTURES`: `native` needs 3.24, `all`/`all-major` 3.23. A suffix `-real` or `-virtual` limits the output; no suffix gives both. An empty value is an error for CUDA targets (CMP0104). https://cmake.org/cmake/help/latest/prop_tgt/CUDA_ARCHITECTURES.html
- **V** nvcc: "When `-arch=native` is specified, nvcc detects the visible GPUs and generates code for them"; no PTX is generated for it. `-arch=sm_NNN` is shorthand that also embeds PTX. https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/index.html (covers 13.5)
- **V** On Windows nvcc uses `cl.exe` from the search path unless `-ccbin` is given. Forum reports: run configure and build from a developer shell where `cl` works, and keep `-ccbin` and PATH pointing to the same `cl.exe`. **R** https://gitlab.kitware.com/cmake/cmake/-/issues/20778 and https://discourse.cmake.org/t/how-to-set-path-for-try-compile-runs-and-during-build/10256
- **V** CMake unity builds support CUDA since 3.31. They skip sources with per-file options, and ODR clashes are possible. https://cmake.org/cmake/help/latest/prop_tgt/UNITY_BUILD.html
- **V** The `target_precompile_headers` page lists no CUDA support, and `Modules/Compiler/NVIDIA-CUDA.cmake` defines no PCH variables. So CMake PCH for `.cu` files is probably absent (**U**, inference from absence). https://cmake.org/cmake/help/latest/command/target_precompile_headers.html
- **V** nvcc has its own `--pch/--use-pch`; the exact syntax is not in the pages I read. https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/compile-time.html
- **V** Ninja 1.13+ supports a GNU jobserver, but nvcc's `--jobserver` is "only supported on Linux". Ninja pools (`depth`) cap concurrent edges. `deps = msvc` parses `/showIncludes` natively. https://ninja-build.org/manual.html
- **V** `-j` default follows the CPU count, so Ninja alone will launch about 16 nvcc jobs. Each nvcc can spawn cicc and ptxas processes and its own threads.
- **V** CMake `FetchContent`: `FETCHCONTENT_FULLY_DISCONNECTED` skips all downloads once populated (from 3.30 the source dir must already exist). Keep `FETCHCONTENT_BASE_DIR` short on Windows. https://cmake.org/cmake/help/latest/module/FetchContent.html
- **V** vcpkg binary caching is on by default (`%LOCALAPPDATA%\vcpkg\archives`); restoring a binary package "takes seconds", and it only helps after a first from-source build. https://learn.microsoft.com/en-us/vcpkg/users/binarycaching

### 5. nvcc compile-time controls, with numbers

- **V** `--threads N` overlaps `-gencode` targets only. `--split-compile N` parallelises per-function optimisation inside one translation unit and has "minimal" run-time impact; it is supported by nvcc, nvrtc, nvlink, nvJitLink and ptxas. `--split-compile-extended` is LTO-only and may cost speed. The page prints that flag two ways, so check `nvcc --help` before use. https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/compile-time.html
- **V** `-fdevice-time-trace <file>` writes a Perfetto-readable JSON; the Compile Time Advisor (`ctadvisor`) reads it and reports expensive templates, expensive headers, parallelisable compilation, split-compile candidates and a phase breakdown. `-time <file>` gives per-phase times. `--Ofast-compile min|mid|max` (`-Ofc`) trades run time for compile time. https://docs.nvidia.com/cuda/cuda-compile-time-advisor/index.html
- **V** Published numbers: none found in the pages read (the programming-guide compile-time page was re-checked in the verification pass and contains no flag speedups; I did not re-open the two blogs). The NVIDIA pages give only toy examples (an NVRTC compile: frontend 30 ms, NVVM optimizer 1000 ms, PTX translation 14 ms) and no speedup for any flag. The two NVIDIA blogs I read (https://developer.nvidia.com/blog/optimizing-compile-times-for-cuda-c/ and https://developer.nvidia.com/blog/reducing-application-build-times-using-cuda-c-compilation-aids/) contain figures only as images.
- **R** A 2023 forum post on CUDA 12.1 found `--split-compile` accelerated only cicc, not ptxas. https://forums.developer.nvidia.com/t/why-acceleration-of-split-compiler-option-is-not-obvious/249312 The current guide lists ptxas as supported, so this may be fixed (**U**).
- **Recon (v1, internal)**: one shade TU took about 1,800 s of a 1,870 s build (cicc 910 s, ptxas 834 s, ptxas peak 8.4 GB RSS). Splitting it into 12 TUs cut a fresh build to about 8 min. This is the only measured data on the problem, and it came from register pressure and template variants, not from the compiler.

### 6. OptiX-IR vs PTX, and the HAGS crash

- **V** The OptiX 9.1 programming guide says programs are encoded as OptiX-IR or PTX. It recommends moving from PTX to OptiX-IR when using nvcc and says PTX "may not provide all OptiX-IR features". `__restrict__` helps live-state analysis in OptiX-IR only. Example: `nvcc -optix-ir -I <sdk>/include --use_fast_math prog.cu -o prog.optixir`. Output must be `--optix-ir` or `--ptx`, not obj or cubin. https://raytracing-docs.nvidia.com/optix9/guide/optix_guide.251219.A4.pdf (sections 6.1, 6.11; read as extracted PDF text)
- **V** The same guide: module and pipeline compilation happens in `optixModuleCreate`, `optixProgramGroupCreate` and sometimes `optixPipelineCreate`. It is cached on disk by default (`optixDeviceContextSetCacheEnabled`) and "cache entries are compatible with the same driver version and GPU type only". So the first run after a driver update recompiles everything.
- **V** OptiX 9.1 release notes: needs R590+; module creation can be cancelled; a new API lets the application provide its own compile cache; new pipeline options for build performance; NVRTC support for cooperative vectors. https://forums.developer.nvidia.com/t/optix-9-1-release/354119 I found no OptiX 9.2 or 9.3 release (**U**; the search index may lag).
- **V** The HAGS thread (posted 2026-11-01, last reply 2026-11-09): RTX 4090, Windows 11 25H2, OptiX 9.1.0, CUDA 13.1, drivers 610.88 and 617.42. Intermittent CUDA 719 (and one 700) when the window loses focus while rendering continues, with SER and HAGS both on. OptiX-IR failed first, then PTX failed too, "less reliably". Turning SER off avoided the crash in testing; HAGS off only reduced it. NVIDIA asked for code and suggested validation, synchronisation and variable-initialisation checks, and gave no cause, fix or confirmation. A HAGS-off run of about 46,000 samples also crashed. OptiX-IR was about 6.5% slower than PTX in that workload. https://forums.developer.nvidia.com/t/optix-9-1-ser-optix-ir-crashes-with-hags-enabled-cuda-719-ptx-appears-stable/387608
- So the thread title overstates the case: the cause is not specific to OptiX-IR, SER looks like the main factor, and it is an unconfirmed single report on a different GPU (Ada, not Blackwell).

### 7. Compiler caching

- **V** (re-read in the verification pass; the source states "Never assume the outer `nvcc` call is cacheable") sccache never caches the outer nvcc call. It runs `nvcc --dryrun` (twice), then caches sub-steps: cudafe++ and cicc are cacheable, ptxas only when it has an output file, host compile steps (cl) are cacheable. `fatbinary` and `nvlink` are not cacheable. `--optix-ir`, `-ptx`, `-cubin`, `-fatbin` are treated as compile outputs with the flag hashed. `-fdevice-time-trace` and `-time` make a call uncacheable. Multi-arch calls are allowed (each arch is its own group). MSVC preprocessing uses `-Xcompiler=-EP`. https://raw.githubusercontent.com/mozilla/sccache/main/src/compiler/nvcc.rs (read via the fetch tool's summary, not line by line)
- **V** sccache hashes absolute paths unless base directories are set (`SCCACHE_BASEDIRS`, plural, absolute paths only). With CMake, MSVC `/Zi` PDBs defeat caching; use `/Z7` (CMake 3.25+: `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`). https://raw.githubusercontent.com/mozilla/sccache/main/README.md
- **L** The installed sccache is 0.17.0 and shows "Base directories (none)". The latest release is v0.19.0 (2026-10-24) (**R**, GitHub API). Its notes list one nvcc fix (exit codes) and MSVC items.
- A likely cause of v1's "0% cross-tree hits" is the missing or mis-named base directory variable, but I did not test it (**U**).
- **R** NVIDIA's numba-cuda moved to a RAPIDS sccache fork because it "supports caching fatbins and ltoir". Upstream may not. https://github.com/NVIDIA/numba-cuda/pull/674 The `.rs` file above shows `fatbinary` uncacheable, consistent with that.
- **R** On Windows, sccache cannot wrap `cl.exe` through a `.bat` rename; a cuda-python PR needed a real shim executable. With CMake's `CMAKE_<LANG>_COMPILER_LAUNCHER` this is not needed. https://github.com/NVIDIA/cuda-python/pull/1185
- **V** ccache 4.14.3 lists NVCC and `.cu` files as level A (supported) and MSVC as level A; `.cuh` is level C. The manual does not say how nvcc is handled. https://ccache.dev/platform-compiler-language-support.html https://ccache.dev/manual/latest.html The v1 report ("0% device hits") came from sccache only; ccache is untested here (**U**).
- **V** NVRTC has a driver-side cache since CUDA 12.9 (`CUDA_CACHE_PATH`, `CUDA_CACHE_MAXSIZE`, default 1 GiB, max 4 GiB). https://docs.nvidia.com/cuda/nvrtc/index.html

### 8. Runtime kernel compilation

- **V** NVRTC compiles CUDA C++ strings to PTX, cubin, LTO-IR or OptiX-IR, runs without a GPU, and supports Windows x86_64. Options: `--split-compile=N`, `--pch` and explicit `--create-pch/--use-pch` (12.8+), `--time`, `-fdevice-time-trace`, `-Ofast-compile`, `--optix-ir` (retrieved with `nvrtcGetOptiXIR`, not usable with `-dlto`). It compiles device code only; there is no linker (use nvJitLink). CUDA 13.3+ can install the toolkit and CCCL headers from `libnvrtc-builtins` (`--use-bundled-headers`), though CUB and Thrust umbrella headers are unsupported. https://docs.nvidia.com/cuda/nvrtc/index.html
- **Conflict**: the OptiX 9.1 guide says OptiX-IR comes from nvcc and PTX from nvcc or NVRTC; the NVRTC guide says NVRTC can emit OptiX-IR. The NVRTC page is newer; test it (spike S2).
- **V** Slang's CUDA target is "preliminary": `-target cuda` and `-target ptx` via NVRTC. OptiX ray tracing is supported but the matrix lists it as experimental, with documented gaps (samplers ignored, no half textures). Binary `.slang-module` files can be precompiled and checked with `isBinaryModuleUpToDate`. https://github.com/shader-slang/slang/blob/master/docs/cuda-target.md (**R** for the module cache: search-result text only)
- **U** I found no published iteration times for NVRTC compiling a path-tracer kernel, and none for Slang.
- Hot-reload of a CPU kernel DLL on Windows: the loaded DLL is locked, so the usual pattern is to build to a new file name and load that (**U**, general practice; not sourced).

### 9. Binding layer

- **V** nanobind 3.1.0 (2026-09-28): needs Python 3.10+; "split mode" gives a stable-ABI frontend (3.10+) so one wheel per platform fits all Pythons, but stable-ABI builds are slower; clang-cl is tested in CI; `nb::ndarray` works with NumPy. https://nanobind.readthedocs.io/en/latest/changelog.html
- **V** Its benchmark claims about 2.7 to 4.4x faster compiles and 3 to 5x smaller binaries than pybind11, and about 3x cheaper simple calls. The workload is 720 trivial functions on Linux with clang 15; numbers are relative only, with no MSVC data. https://nanobind.readthedocs.io/en/latest/benchmark.html
- **V** `ctypes` is in the stdlib and `_ctypes.pyd` is shipped in Blender 5.1 and 5.2 (**L**). No ctypes call overhead is documented; the nanobind page says only that pybind11 is slower (**U** for ctypes per-call cost).

### 10. CI with a GPU

- **V** GitHub-hosted standard Windows runners: public repos 4 vCPU, 16 GB RAM, 14 GB disk; private repos 2 vCPU, 8 GB, 14 GB. GPU runners need the Team or Enterprise Cloud plan. https://docs.github.com/en/actions/reference/runners/github-hosted-runners
- **V** GitHub recommends self-hosted runners only for private repositories, because a fork PR can run arbitrary code on the runner. It recommends ephemeral runners; a Windows service install wants `C:\actions-runner`. https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/add-runners and https://docs.github.com/en/actions/reference/runners/self-hosted-runners
- **R** The announced $0.002/min charge on self-hosted runners (Dec 2025, for March 2026) was postponed and, per sources up to mid-2026, has not taken effect. https://www.techzine.eu/news/devops/137396/github-bends-to-criticism-and-delays-paid-self-hosting-of-runners/
- **V** Dev Drive (ReFS, Defender performance mode) must be created at format time (50 GB minimum, existing volumes cannot be converted); it targets source, package caches and build output. Microsoft's average-improvement figures are in a linked blog I did not open. https://learn.microsoft.com/en-us/windows/dev-drive/
- **L** Defender exclusions need admin rights to read; I could not see this machine's list. C: has 78 GB free, so a 50 GB Dev Drive is possible but tight.

## Options and tradeoffs

**A. Host compiler.** One MSVC `cl` for everything is supported by every source above and it is what nvcc uses as host anyway; the CPU kernel build and the nvcc host pass then parse the shared headers with the same front end. Clang-cl as the C++ compiler beside nvcc-with-cl (Blender's setup) gives a second front end that can diverge on the shared kernel source, which is the thing "write once" depends on. MinGW is out: v1's experience (recon) and no CUDA support.

**B. CRT linkage of the engine DLL.** `/MT` removes any dependence on the runtime Blender bundles, at the price of a second CRT in the process (so nothing may be allocated in one DLL and freed in the other) and no `/MD` with Python-embedded objects. `/MD` with the toolset pinned at or below Blender's 14.44 avoids that but ties every developer and CI image to an old toolset. A C-ABI boundary makes `/MT` safe; a nanobind module wants `/MD` (it shares Python objects).

**C. Python boundary.**
- Plain C ABI plus `ctypes`: no `Python.h`, no Python-version coupling, the engine DLL builds with no Python installed, tiny compile cost, and the GIL is released per call. Costs: struct layouts written twice (once in C, once in ctypes), no automatic exception mapping (error codes plus a `last_error` call), buffers passed as raw pointers. Generating the Python side from the header removes the duplication.
- nanobind: nicer array handling and error mapping, still cheap to compile, but adds a Python-version axis (abi3 split mode or one wheel per Python), `python3x.lib` in the build, and `/MD`.
- pybind11: no advantage here (slower compiles per the nanobind benchmark, same Python coupling).
The surface is small (lifecycle, scene upload by arrays, settings, film readback, progress polling), so the extra convenience of nanobind buys little; the question is how many struct-shaped arguments appear.

**D. Kernel dev loop.**
- *AOT only* (nvcc to cubin/OptiX-IR in the build): simplest, one compiler path, but every shared-header edit costs nvcc time on every dependent TU. No published numbers; v1 saw minutes to half an hour.
- *NVRTC at runtime in dev mode, nvcc AOT for release*: a kernel edit costs one NVRTC compile of the affected stage with no `cl.exe`, no Ninja, no relink, and the NVRTC and driver caches absorb repeats. Costs: two compile paths for one source (flag and header drift, NVRTC cannot see host-only headers, so kernel headers must be freestanding), a first-run delay when caches are cold.
- *Slang runtime compile or precompiled modules*: only if the backend decision picks Slang; it has its own session cache but weaker OptiX and CPU maturity (section 8).
- *CPU kernel DLL reload*: independent of the GPU choice; useful because the CPU backend is the test oracle.

**E. Caching.** sccache caches the device steps (cicc, ptxas) and host steps separately, cannot cache fatbinary, and hashes absolute paths. ccache is "level A" for nvcc but unmeasured here. Both need `/Z7`-style debug info on MSVC. A cache only pays off when edits are small relative to what a clean cache would rebuild, which the TU layout controls more than the cache does.

**F. Unity builds and PCH.** Unity builds help many small host `.cpp` files (tests, host glue) and hurt device TUs, where one big unity TU is exactly v1's 1,800 s file. PCH works for host C++ in CMake; for `.cu` files it is probably unsupported in CMake (section 4), so keep device includes lean instead and use the Compile Time Advisor's "expensive headers" report.

**G. Dependencies.** Vendoring header-only code and pinning prebuilt binaries by hash gives a configure with no network and nothing built from source. FetchContent adds network access and a source build per dependency to every clean build unless you disconnect it. vcpkg removes the source-build cost only after the first build and adds a second tool and cache to maintain; it earns its place only if several source-built libraries appear.

**H. CI.** A hosted CPU-only job checks clean-checkout correctness (missing files, stale-artifact failures, which v1 hit repeatedly) but cannot run any GPU test (v1's "CI has no GPU" gap). A self-hosted runner on the dev PC runs the real GPU gate but shares the GPU and CPU with interactive work (CLAUDE.md: never two GPU runs at once), is unsafe on a public repository, and goes offline when the PC does. A hosted GPU runner needs a paid plan. A local gate script that every merge must pass, plus hosted CPU CI, is the least machinery.

## Recommendation for Astroray 2.0

Everything below is provisional until the spikes in the next section run. Each item names the evidence it rests on.

1. **Toolchain:** CMake 4.x + Ninja + `cl.exe` (VS 2022 17.14 Build Tools toolset 14.44, already installed) + nvcc 13.x, driven by `CMakePresets.json` presets that call `vcvars64` themselves so nothing depends on how the shell was started. Evidence: sections 1 and 4; Blender's own setup uses `cl.exe` as CUDA host. No clang-cl, no MinGW. Linux is not a target for 2.0 unless the owner asks; keep the CMake free of Windows-only assumptions outside the toolchain files.
2. **CUDA version:** pin 13.3 or newer (NVRTC bundled headers need 13.3+; driver 621.24 here satisfies R620 for 13.5). Run the first spike on the installed 12.8 and on a 13.x install so the toolkit change is measured, not assumed. Risk to test first (U): the installed 12.8 may reject toolset 14.44 (`_MSC_VER` 1944) in its host-compiler check; the 13.x tables list "193x" for VS 2022 17.x, which covers it, and Blender builds CUDA with 14.44, so 13.x is expected to work.
3. **Architectures:** dev preset `CMAKE_CUDA_ARCHITECTURES=native` (this machine reports 12.0). Release preset: `75-real;86-real;89-real;120-real;120-virtual` or similar, built only in release jobs. Evidence: CUDA 13.0 dropped pre-Turing; `native` emits no PTX. A cache key that includes `native` is machine-specific, which is fine for a single dev machine.
4. **Boundary:** a C ABI (`include/astroray/astroray.h`, opaque handles, POD structs, array pointers, error codes) in `astroray_core.dll`, loaded from Python with `ctypes.CDLL` by full path, with the Python struct definitions generated from the header and checked by a `sizeof` table at load time. Build the DLL `/MT`. Start without nanobind; adopt it only if the generated bindings become the maintenance cost. Evidence: ctypes and libffi ship in Blender; GIL release is documented; removes the Python-version and CRT axes. This decision holds for either GPU backend.
5. **Extension:** one Blender extension with `platforms = ["windows-x64"]`, containing the DLL(s) and kernel blobs via `[build] paths`, built by `blender --command extension build`. Self-host the repository first; ask the moderators before targeting extensions.blender.org. Evidence: section 3, including the unverified raw-DLL policy.
6. **Kernel source discipline:** kernel headers are freestanding (no `<vector>`, `<string>`, no exceptions), included by one `.cu` and one `.cpp` per wavefront stage. A script reports include fan-out per header (`ninja -t deps` plus the `/showIncludes` data) and fails the gate when a volatile header (material, integrator) reaches more than a set number of device TUs. Evidence: v1's 128-variant TU and header fan-out cost (recon section "build-tests").
7. **Dev loop, if the backend is CUDA/OptiX:** NVRTC for kernel edits in dev mode (`--split-compile`, bundled headers, `CUDA_CACHE_PATH` on the build drive, OptiX-IR output if S2 confirms it), nvcc AOT for the merge gate and release, with a test that both paths produce the same image on the furnace scene. CPU kernels in `astroray_kernels_cpu.dll`, reloaded by loading a freshly named copy. If the backend is vendor-neutral, replace NVRTC with the chosen shader compiler's runtime session and keep the CPU DLL reload.
8. **OptiX modules:** OptiX-IR by default (the guide recommends it), PTX behind a CMake switch as a diagnostic. SER behind a runtime flag until it passes the stress test in S4 on this GPU with HAGS on. Do not treat "PTX is stable" as established: the forum thread says otherwise. Use the 9.1 compile-cache and task APIs only if S4 shows cold-start time is a problem.
9. **Caching:** sccache via `CMAKE_{C,CXX,CUDA}_COMPILER_LAUNCHER`, `SCCACHE_BASEDIRS` set to the checkout parent, `/Z7` debug info, Ninja pool for nvcc jobs sized by RAM (S9). Keep `-fdevice-time-trace` out of normal builds because it makes sccache skip the call; use a separate `profile` preset. Re-run the cache test (S3) before trusting it; fall back to ccache if hits stay at zero.
10. **Unity/PCH:** unity builds (CMake 3.31+) only for the unit-test target and host glue; no unity for `.cu`; PCH for host C++ only, if configure and compile profiles show header parsing matters.
11. **Dependencies:** no vcpkg, no FetchContent in the default configure. Header-only code is vendored in `third_party/`; prebuilt binaries (OIDN 2.5.0 matches Blender's own version; OptiX headers from the NVIDIA `optix-dev` repo, licence not yet read) are fetched by one script with pinned SHA-256 into `third_party/_cache`. Revisit vcpkg if more than about five libraries need building from source.
12. **CI:** the authoritative gate is a local script (`tools/gate.ps1`) that takes a GPU lock file, builds, and runs the merge-gate tests; a hosted Windows job does a clean configure+build+CPU tests on every push (CPU backend, `ASTRORAY_CUDA=OFF`, no CUDA install needed). Add a nightly job on a self-hosted ephemeral runner on the dev PC only if the repository stays private. Evidence: section 10.
13. **Windows hygiene:** keep build trees and caches out of OneDrive (v1 lived in one), on a single local drive; test a Dev Drive or Defender exclusion in S8 before recommending either.

**Repository layout** (each directory exists because a build or test target needs it)

```
CMakeLists.txt  CMakePresets.json  cmake/        toolchain, cuda, warnings, kernel-embed modules
include/astroray/astroray.h                       the only public header (C ABI)
src/kernel/        freestanding shared headers: math, spectrum, rng, bsdf, openpbr, stages
src/kernel_gpu/    one .cu per stage (includes src/kernel/ only)
src/kernel_cpu/    one .cpp per stage (same includes)
src/host/          scene, acceleration structure glue, film, denoise glue, backend interface
src/capi/          C ABI implementation -> astroray_core.dll
blender/           extension: manifest, __init__.py, generated ctypes bindings, engine class
tests/unit/        CPU-only, doctest, unity-built;  tests/gpu/ smoke + furnace + chi-squared
tools/             gate.ps1, fetch_deps.ps1, include_fanout.py, build_timing.py
third_party/       vendored headers + pinned binary cache
spikes/            throwaway measurement code (already in the plan)
```

**Targets**

| Target | Kind | Notes |
|---|---|---|
| `astroray_kernel_headers` | INTERFACE | include dirs and compile definitions shared by CPU and GPU stages |
| `astroray_kernels_cpu` | SHARED | one TU per stage; reload target for the dev loop |
| `astroray_kernels_gpu` | custom (cubin / OptiX-IR files) | `nvcc` AOT; skipped in the CPU-only CI job |
| `astroray_host` | STATIC | no CUDA headers in its public interface |
| `astroray_core` | SHARED, `/MT` | C ABI; the only DLL Python loads |
| `astroray_tests_unit` | exe, unity | runs on the CPU backend |
| `astroray_tests_gpu` | exe | smoke, furnace, chi-squared per sampler/BSDF |
| `astroray_extension` | custom | stage files, run `blender --command extension build` |
| `astroray_gate` | custom | lock, build, run T0/T1 tests, print the timing table |
| `astroray_profile` (preset) | - | `-fdevice-time-trace`, `-time`, no cache; feeds `ctadvisor` |

**Build-time budgets.** The first five restate CLAUDE.md rule 9 (owner budgets); the rest are my proposals and are derived, not measured.

| Event | Budget | Why |
|---|---|---|
| Edit one host `.cpp`, rebuild + relink `astroray_core` | 5 s | one `cl` job plus a link; no device code involved (proposal) |
| Edit one stage `.cu` or `.cpp` | 30 s with nvcc AOT; 10 s on the NVRTC dev path | CLAUDE.md incremental budget; the 10 s is a target to test in S2 |
| Edit a volatile shared kernel header, dev path | 30 s | only the active backend's stages recompile |
| Clean build, warm cache | 5 min | CLAUDE.md |
| Clean build, cold cache | 10 min | v1 fresh build was 8 min after splitting (recon); keep it in that range |
| Single device TU, cold | 60 s | so no TU can become the critical path on 16 threads; the profile preset fails the gate above this |
| Configure | 5 s | no network (dependency decision) |
| Pre-commit tests | 10 s | CLAUDE.md |
| Merge gate (incremental build + T1) | 3 min | CLAUDE.md |

Arithmetic behind the 60 s cap: with N device TUs and 8 concurrent nvcc jobs, device wall time is about N x t / 8 plus the longest TU. For a 5 min clean budget that leaves roughly 40 s average per TU at N = 50. nvcc concurrency is capped at 8 by RAM, not CPU: v1's ptxas peaked at 8.4 GB for one TU, and 8 x 8.4 GB is 67 GB against 63 GB installed. The cap is a starting point for S9, not a result.

## What a measurement spike must settle

Each spike names the command or tool that produces its numbers (CLAUDE.md rule 4). All run serially; no two GPU or nvcc jobs at once.

- **S1, nvcc floor and profile.** Compile (a) an empty `.cu` and (b) one representative stage-sized kernel with `-time`, `-fdevice-time-trace` and `ctadvisor`, on CUDA 12.8 (installed) and a 13.x install. Vary `--split-compile` 0/4/16, `-Ofc` min/mid/max, `--threads`, and `-arch=native` vs `sm_120` vs a 5-arch list. Settles: the per-TU floor, whether split-compile helps ptxas on current toolkits, the cost of multi-arch, and whether 13.x compiles faster or slower than 12.8.
- **S2, NVRTC vs nvcc.** Compile the same stage with NVRTC (`--split-compile`, `--use-bundled-headers`, `--optix-ir` if OptiX), cold and warm driver cache. Compare wall time, register count and SASS size against the nvcc build, and compare rendered output. Settles: whether a 10 s edit loop is real, whether the two paths agree, and whether NVRTC can emit OptiX-IR in practice.
- **S3, cache behaviour.** With sccache 0.17.0 and 0.19.0, and with ccache: two checkouts at different paths, `SCCACHE_BASEDIRS` set, `/Z7`; build a `.cu` to object, `--optix-ir` and `-cubin`; read `--show-stats` (hits, misses, non-cacheable) after each. Settles: what is cached in practice, whether v1's 0% was a configuration error, whether the >10 min drop v1 saw still occurs, and sccache vs ccache.
- **S4, OptiX cold start and HAGS.** Measure `optixModuleCreate` + `optixPipelineCreate` for the stage set with the disk cache cleared and warm, IR vs PTX, with and without the task API. Then run the forum's failure condition (window loses focus, viewport keeps rendering, SER on, HAGS on, which is this machine's state) for a fixed number of samples. Settles: first-launch latency, whether IR vs PTX matters for speed and stability on a 5070 Ti, and whether SER can ship on by default.
- **S5, extension packaging.** Build a zip with the DLL via `[build] paths`, install it into Blender 5.3 (and 5.2 for comparison), `ctypes.CDLL` it by full path, call it from a render engine class. Test `/MT` and `/MD`, and the unload/replace behaviour while Blender runs. Settles: that a raw DLL in an extension works, the loader pitfalls, and what Blender 5.3's bundled runtime version is.
- **S6, ctypes vs nanobind.** Bind the same 40-function surface both ways. Measure compile time with `cl`, per-call overhead for a small call, and upload time for a 1M-triangle mesh and a 4K RGBA float film readback. Settles: whether ctypes is enough and the build-time difference on MSVC (the published numbers are Linux/clang).
- **S7, CI.** Time a clean CPU-only configure+build+test on a hosted Windows runner (is the 14 GB disk enough, does `windows-2025-vs2026` or `windows-2022` give the pinned toolset), and a `CUDA_INSTALL` step if wanted. Time the GPU gate on the dev PC. Settles: whether hosted CI fits the 5 min and 3 min budgets and how long the gate occupies the machine.
- **S8, filesystem.** Rebuild the same tree on NTFS with and without a Defender exclusion, and on a Dev Drive. Settles: whether either is worth the admin step.
- **S9, concurrency.** Sweep Ninja job count and an nvcc pool depth from 2 to 16 on a clean build, recording wall time, peak RAM and any instability. v1 recorded hard resets under combined load, so stop at the first sign. Settles: the real nvcc pool size.
- **S10, include fan-out.** After the first stage skeleton exists, run the fan-out script and check that one edit to a volatile header rebuilds only the stages that need it.

## Verification pass

Independent check on 2026-11-14. Sources were fetched directly; no builds were run, and local (L) claims were not re-measured.

| Claim | Verdict | Source |
|---|---|---|
| CUDA 13.5 Windows host compilers: MSVC 195x/193x/192x only | CONFIRMED | https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html |
| CUDA 13.0 table: 193x/192x only; VS 2017 dropped in 13.0 | CONFIRMED (the 13.5 page's footnote wording on the drop release was summarised inconsistently; I rely on the 13.0 page) | https://docs.nvidia.com/cuda/archive/13.0.0/cuda-installation-guide-microsoft-windows/index.html |
| Blender sets `CUDA_HOST_COMPILER` to `cl.exe` because clang-cl would fail | CORRECTED: it is a default used only if the variable is unset, and the comment says the build "will fail due to a non-supported compiler" rather than quoting the earlier wording. The substance holds | https://raw.githubusercontent.com/blender/blender/main/build_files/cmake/platform/platform_win32.cmake |
| One `cl.exe` for CPU kernels and nvcc's host pass | CONFIRMED as supported; "one front end for shared headers" is a design inference, not a source claim | same as above |
| Blender 5.3 release branch: Python 3.13.13, NumPy 2.3.4, OIDN 2.5.0 | CONFIRMED (exact `set(...)` lines) | https://raw.githubusercontent.com/blender/blender/blender-v5.3-release/build_files/build_environment/cmake/versions.cmake |
| Blender MSVC minimum 17.14.14 | CONFIRMED (19.44.35216) | platform_win32.cmake URL above |
| Blender 5.3 bundled CRT is 14.44.35211 | COULD NOT VERIFY. Only 5.1/5.2 installs showed 35211; 5.3 needs 35216 to build, so it is probably newer | same as above |
| Redistributable must be at least as new as the newest toolset; `/GL` and `/LTCG` libraries are not cross-version compatible | CONFIRMED. The page also states a linker rule (link with tools at least as new as any input) | https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017?view=msvc-170 |
| sccache never caches the outer nvcc call; uses `--dryrun` twice; cudafe++, cicc, host cl cacheable; ptxas only with an output file; fatbinary and nvlink not cacheable; `-fdevice-time-trace` and `-time` uncacheable | CONFIRMED. Host preprocessing is also marked non-cacheable | https://raw.githubusercontent.com/mozilla/sccache/main/src/compiler/nvcc.rs |
| `SCCACHE_BASEDIRS` exists; `/Z7` or `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` needed for MSVC | CONFIRMED | https://raw.githubusercontent.com/mozilla/sccache/main/README.md |
| sccache latest is v0.19.0 (2026-10-24), one nvcc fix | CONFIRMED | https://api.github.com/repos/mozilla/sccache/releases/latest |
| Local sccache 0.17.0 has no base directories | COULD NOT VERIFY (not re-run; "do not run anything") |  |
| No NVIDIA source publishes speedups for `--split-compile`, `--Ofast-compile`, `--threads` | CONFIRMED for the compile-time guide page, which also spells the extended flag two ways. The two blogs and any other NVIDIA page were not re-read, so "none found", not "none exist". The only measured data remains v1's single TU | https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/compile-time.html |
| OptiX 9.1 guide recommends OptiX-IR; module cache keyed to driver version and GPU type | COULD NOT VERIFY. The PDF is compressed and unreadable by my tools; searches found only consistent forum and Blender reports of disk-cache invalidation across drivers | https://raytracing-docs.nvidia.com/optix9/guide/optix_guide.251219.A4.pdf |
| HAGS thread: single report, RTX 4090, PTX also failed, SER the main suspect, nothing confirmed by NVIDIA | CONFIRMED. Also: a HAGS-off run still crashed, which weakens HAGS as the cause further | https://forums.developer.nvidia.com/t/optix-9-1-ser-optix-ir-crashes-with-hags-enabled-cuda-719-ptx-appears-stable/387608 |
| HAGS on locally (`HwSchMode` 2) | COULD NOT VERIFY (local, not re-measured) |  |
| CUDA 13.0 to 13.5 driver branches R580 to R620 | CONFIRMED. Release dates are not on the page, so "13.3 on 2026-05-27" stays unresolved | https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html |
| NVRTC: `--optix-ir` with `nvrtcGetOptiXIR`, not with `-dlto`; `--use-bundled-headers` from 13.3; cache default 1 GiB, max 4 GiB | CONFIRMED. This resolves the "NVRTC can emit OptiX-IR" side of the conflict in the NVRTC docs' favour; whether it works for a real OptiX pipeline is still for S2 | https://docs.nvidia.com/cuda/nvrtc/index.html |

Effect on the recommendation: none of the five load-bearing claims is overturned. The CRT item was tightened (pin by runtime build number is fragile, so `/MT` behind a C ABI is the primary answer), and a CUDA 12.8 plus toolset 14.44 compatibility risk was added to item 2. The OptiX-IR default remains a reading of a guide I could not open, so S4 must confirm it on the 5070 Ti.

## Unverified or conflicting

- **Blender 5.3 runtime.** I verified Python 3.13.13 and NumPy 2.3.4 from the 5.3 release-branch source, and the bundled CRT (14.44.35211) from a local 5.2 install. I have no 5.3 install, and blender.org returned 403, so the 5.3 CRT version and any 5.3 platform notes are unconfirmed.
- **Raw DLLs in extensions.** The docs cover wheels only. Whether extensions.blender.org accepts a raw DLL, and whether a self-built wheel is allowed there, is unknown.
- **CUDA host compilers for 13.1 to 13.4.** I read the 13.0 and 13.5 tables only; VS 2022 is listed in both. The 13.0 notes add Clang 20 and GCC 15 without saying for which OS.
- **clang-cl as an nvcc host.** Blender's CMake comment and CMake issue #25648's title say it is unsupported; I could not read the issue.
- **PCH for `.cu` in CMake.** Inferred from absence in the docs and in `NVIDIA-CUDA.cmake`.
- **sccache vs ccache on nvcc.** The sccache mechanism comes from reading its source summary; v1's 0% hit rate is unexplained; ccache's nvcc mechanism is undocumented in the manual; no hit-rate numbers exist from any source.
- **nvcc compile-time flags.** No published speedup for `--split-compile`, `--Ofast-compile` or `--threads`; a 2023 report that only cicc benefits conflicts with the current guide listing ptxas.
- **OptiX-IR vs PTX and the recon file.** The recon file said "build PTX first because of the HAGS crash". The thread I read says PTX crashed too, SER looks like the main factor, the GPU was an RTX 4090, and NVIDIA confirmed nothing. The OptiX guide recommends IR. My recommendation follows the guide and keeps PTX as a switch; neither claim has been tested on a 5070 Ti.
- **NVRTC and OptiX-IR.** The OptiX guide says OptiX-IR comes from nvcc; the NVRTC guide says NVRTC can produce it. Not tested.
- **OptiX versions.** I found no OptiX 9.2 or 9.3; the official CUDA-version support list for 9.1 was not in what I read (the forum reporter used CUDA 13.1).
- **Slang.** The CUDA doc contradicts itself on half support; OptiX is "experimental" in one table and "supported" in the prose; no compile-speed data.
- **nanobind speedups** are from a Linux/clang synthetic benchmark; no MSVC data. ctypes call overhead is undocumented.
- **GitHub pricing.** The self-hosted runner fee was postponed, not cancelled; the 2026 status after June is unconfirmed.
- **Dev Drive gain.** I did not open Microsoft's measurement blog, so I give no number.
