# 017 astroray_cli with `info`
Goal: there is no command-line host -> `astroray_cli info` prints the build record, ABI hash and every device with status and reason using only the public ABI, refuses a hash mismatch, and works with no CUDA device visible.
Read first: 0001 D1.4 (CLI is the primary host, public ABI only), D2.7 (hash check on load), D2.8, V6 (CLI part); R38; 0009 d5 (print build info at start).
Scope: cli/main.cpp, cli/CMakeLists.txt or CMakeLists.txt, tests/abi/test_cli_info.py.
Out of scope: `render`, `bench`, `dump` (069; until then they print "not implemented until task 069" and exit 2).
Reference: none.
Do:
1. Target `astroray_cli` (/MT, links only `astroray_core`'s import library and reads `out/build/gen/abi_hash.h`); prints `build: sha=... dirty=... kernels=...` first.
2. On start compares `AR_ABI_HASH_VALUE` with `ar_abi_hash()`; on mismatch prints both and exits 3 (ABI_MISMATCH).
3. `info`: driver line, ABI hash, then per device `index kind name cc memory status reason`; when the CUDA probe reports no device, prints `no CUDA device: <reason>`.
4. `info --create <cpu|cuda[:i]>`: creates and destroys a context on that device and prints `context ok` or the status name and reason; never falls back to another device (exit 3 on AR_ERR_DEVICE).
5. tests/abi/test_cli_info.py (unittest): `info` lists device 0 as CPU; with `CUDA_VISIBLE_DEVICES=-1` in the child environment `info` prints `no CUDA device` and `info --create cuda` exits 3 with `AR_ERR_DEVICE` (0001 V6, CLI part); `info --create cpu` prints `context ok`.
Acceptance: `out/bin/astroray_cli.exe info` lists the CPU and the RTX 5070 Ti with status AR_OK; `<python> -m unittest discover -s tests/abi -p "test_cli_info.py"` passes 3 tests; `pwsh tools/check.ps1 fast` passes.
Look at: none.
Canary: tasks/017.canary.patch must turn red: tests/abi/test_cli_info.py "TestCli.test_no_cuda_no_fallback".
Budget: none.
Depends on: 014.
Size: S (about 200 lines); stop and report at 2x.
Progress:
