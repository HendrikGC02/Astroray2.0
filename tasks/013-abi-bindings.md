# 013 Generated bindings, identity exports and the build-id check
Goal: the header is not usable from Python and binaries carry no identity -> abigen generates the C layout table and `_abi.py`, `astroray_core.dll` exports the identity functions, every executable prints the build record, and `check fast` fails when the record's sha is not HEAD.
Read first: 0001 D1.3 (`_abi.py` generated, never committed), D2.7 (hash, layout, build info), V1; 0009 d5 (build identity, rewrite only on change, sha check), d3 (targets); tools/abigen.py.
Scope: tools/abigen.py, CMakeLists.txt, cmake/sources.cmake, cmake/buildinfo.cmake, src/capi/identity.cpp, src/host/build_info.h, tests/abi/test_abi_match.py, tests/cpu/main.cpp, tools/check.ps1.
Out of scope: context and device implementation (014); recorder (033); the CLI (017).
Reference: none.
Do:
1. abigen emits into out/build/gen/: `astroray_layout.c` (for every struct a row of `sizeof`, `alignof` and `offsetof` per field, so MSVC computes the numbers), `abi_hash.h` (`#define AR_ABI_HASH_VALUE 0x...ull`), `_abi.py` (Apache-2.0 SPDX line; ctypes structs in header order, `AR_*` constants, function prototypes with argtypes/restype, `ABI_HASH`, and `check(lib)` that compares the hash and every struct's size, alignment and offsets with ctypes' own and raises `AbiMismatch` naming the hash values or `Struct.field`).
2. Target `astroray_buildinfo` (STATIC): a generated `build_info.cpp` holding version, git sha, dirty flag, toolchain versions (CUDA, OptiX, Embree, OIDN: "none" until present) and build type; the generator writes the file only when its content changes, and its custom command depends on `.git/HEAD`, the current branch ref file and `.git/index`, so a new commit relinks and `ninja -n` stays clean after a build.
3. Targets by 003's source rule: `astroray_engine` (STATIC: globbed src/host/**/*.cpp and src/device/cpu/*.cpp, created when the first such file exists) and `astroray_core` (SHARED = globbed src/capi/*.cpp + astroray_engine + the generated table, /MT, defines `AR_BUILDING_CORE`); test executables link astroray_engine, never the DLL. src/capi/identity.cpp implements `ar_abi_hash`, `ar_abi_layout_count`, `ar_abi_layout` (from the generated table) and `ar_build_info` (`kernel_source` = "embedded", or the value of `ASTRORAY_KERNEL_DIR` when set in a build compiled with `AR_DEV_KERNEL_DIR`, which only the `dev` preset defines). Every entry point catches all C++ exceptions and returns `AR_ERR_INTERNAL`.
4. astroray_tests_cpu links astroray_buildinfo (never the DLL) and prints `build: sha=<sha> dirty=<0|1> kernels=<source>` first.
5. check.ps1 `fast`: fail when the printed sha differs from `git rev-parse HEAD` or kernels is not `embedded`.
6. tests/abi/test_abi_match.py (unittest, loads out/bin/astroray_core.dll through out/build/gen/_abi.py): real binding passes `check`; a temp copy with `ABI_HASH` edited is refused naming both hashes; a temp copy with two fields of one struct swapped is refused naming `Struct.field`.
Acceptance: `pwsh tools/check.ps1 fast` prints PASS and the build line; `<python> -m unittest discover -s tests/abi -p "test_abi_match.py"` passes 3 tests; after `git commit --allow-empty -m x` (local, then reset) `pwsh tools/build.ps1` relinks and the printed sha changes; running `pwsh tools/check.ps1 fast` with `$env:ASTRORAY_KERNEL_DIR` set fails naming the kernel source (0009 Verification, decision 5).
Look at: none.
Canary: tasks/013.canary.patch must turn red: tests/abi/test_abi_match.py "TestMatch.test_swapped_field_refused".
Budget: the build-info relink must stay within B-03 (report it).
Depends on: 012.
Size: M (about 400 lines); stop and report at 2x.
Progress:
