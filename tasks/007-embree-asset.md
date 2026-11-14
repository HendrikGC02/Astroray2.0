# 007 Embree release asset: recorded build script and version pin
Goal: the CPU device needs Embree without TBB and with the static CRT, which no upstream binary provides -> a recorded script builds our Embree archive once, the lead publishes it as a release asset, and the Embree version pin is decided and written into 0009.
Read first: 0001 D9.1-D9.3 (Embree static, no TBB, release assets), 0009 d6 (dependencies, recorded scripts outside the normal build), 0009 d2 (pins), R12, R63 (version pins designed when built).
Scope: tools/deps/build_embree.ps1, docs/decisions/0009-build-test-process.md (decision 2: one added pin sentence), tools/README.md.
Out of scope: deps.toml and fetching (008); OIDN (098); linking Embree into the engine (037).
Reference: Embree 4 build options from the RenderKit/embree README and CMakeLists.txt (Apache-2.0).
Do:
1. Designed when built (R63): choose the newest Embree 4.x release tag at task time; record it, with the reason (newest 4.x with Windows static-library support), as a pin sentence in 0009 decision 2 in this same diff.
2. tools/deps/build_embree.ps1 (ASCII): takes the machine lock; shallow-clones the pinned tag into third_party/_cache/src/embree-<ver>; configures with the same MSVC environment as tools/build.ps1 and `EMBREE_STATIC_LIB=ON`, `EMBREE_TASKING_SYSTEM=INTERNAL`, `EMBREE_ISPC_SUPPORT=OFF`, `EMBREE_TUTORIALS=OFF`, `EMBREE_TESTING_INTEGRATION=OFF`, `EMBREE_RAY_MASK=ON` (037's `rtcSetGeometryMask` is ignored without it; confirm the option name in Embree's CMakeLists.txt at the pinned tag), `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, Release; keeps the default ISA set (Embree dispatches its own ISA) and the triangle, user, curve and point geometry types.
3. The script installs into a staging dir and zips `include/`, `lib/` and the upstream LICENSE into `embree-<ver>-astroray-win64-mt.zip` in out/deps/, then prints the zip's SHA-256 and size. It is never called by build.ps1 or check.ps1.
4. Lead step: publish the zip with `gh release create deps-embree-<ver> <zip> --notes "<script commit sha>"`; write the asset URL and SHA-256 into task 008's Do step 1 (tasks/008-deps-fetch.md) before dispatching 008, since this file is deleted when it lands.
Acceptance: `pwsh tools/deps/build_embree.ps1` ends with a line `embree-<ver>-astroray-win64-mt.zip sha256=<64 hex> bytes=<n>`; `dumpbin /directives` on the archived embree4.lib shows `/DEFAULTLIB:LIBCMT` and no `msvcrt`; `dumpbin /symbols` output contains no `tbb::` symbol; the configure log shows `EMBREE_RAY_MASK=ON`; 0009 d2 names the Embree version.
Look at: none.
Canary: none: no test added or changed.
Budget: none on the normal build (the script is outside it); report the script's run time.
Depends on: 003, 004.
Size: S (about 120 lines); stop and report at 2x.
Progress:
