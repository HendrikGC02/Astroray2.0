# 098 Measurement: our OIDN build with static oneTBB (0001 Risk 1) and its recorded script
Goal: whether OIDN's CPU device can link oneTBB statically, with renamed modules and the static CRT, is untested and decides the DLL set M1b certifies -> a recorded script builds OIDN at the pinned version that way, and the result decides between the planned build and 0001 D9.4's fallback.
Read first: 0001 D9.1-D9.4 (our OIDN build, renamed modules, static CRT, static oneTBB, fallback), Risks 1 and 3, V5; R12; 0009 d6 (recorded scripts, release assets outside the normal build).
Scope: tools/deps/build_oidn.ps1, third_party/deps.toml (OIDN entry, once published), docs/decisions/0001-product-shape.md (only if the fallback applies: one superseding note in D9.4's place), tools/README.md.
Out of scope: loading OIDN in the engine and V5 inside Blender (M1b); ISPC in the normal build (never).
Reference: OIDN README and CMake options (`OIDN_LIBRARY_NAME`, `OIDN_STATIC_LIB`, `OIDN_DEVICE_CPU`, `OIDN_DEVICE_CUDA`, TBB prerequisite) from RenderKit/oidn at the pinned 2.5.0 tag (Apache-2.0); oneTBB build documentation (static build caveats).
Do:
1. build_oidn.ps1 (machine lock, ASCII): fetch OIDN 2.5.0 sources with submodules, oneTBB at the version OIDN 2.5.0 documents, ISPC per OIDN's requirement into third_party/_cache/tools; build oneTBB static with the static CRT; build OIDN with `OIDN_LIBRARY_NAME=astroray_oidn`, `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, CPU and CUDA devices on (CUDA 12.8, sm_120 included), linking the static oneTBB into the CPU device module; zip the DLLs, headers and licences with a SHA-256.
2. Check: `dumpbin /dependents` on every produced DLL (only system DLLs, `astroray_oidn*` siblings and, for the CUDA device module, nvcuda.dll; no tbb*, vcruntime*, msvcp*); a tiny smoke program (in the script's scratch dir, not committed) denoises a 64x64 noisy image on the CPU device and on the CUDA device.
3. Result (PR body `Result:` with commands and the dumpbin output). Outcomes: links and runs -> the lead publishes the zip as release asset `deps-oidn-2.5.0`, its URL and hash go into deps.toml (kind fetched-binary), and M1b uses it; static oneTBB fails -> per 0001 D9.4 the CPU device module is left out, CPU-device renders will report `DENOISER_UNAVAILABLE` "denoiser unavailable on the CPU device", and that consequence is written into 0001 D9.4 as a note naming this measurement; the CUDA device module still ships if it passes.
Acceptance: the PR body holds the dumpbin output and the smoke result for both devices with the commands; deps.toml has the entry if the build passed.
Look at: none.
Canary: none: measurement only.
Budget: none on the normal build (script outside it).
Depends on: 008.
Size: S (script about 150 lines plus result); stop and report at 2x.
Progress:
