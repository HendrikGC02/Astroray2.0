# 014 Contexts, device enumeration, the context log and last_error
Goal: the DLL exports only identity -> `ar_device_count`, `ar_device_get_info`, `ar_context_create` and `_destroy` work for the CPU and CUDA devices with the synchronous driver and AVX2 checks, each context writes its log line by line, and every handle carries its last error.
Read first: 0001 D2.1 (handles, context desc, device info), D2.5 (log file, exceptions), D2.6 (threading), D2.8 (explicit device selection, synchronous checks), D3.5-D3.6 (driver floor, AVX2), V6.
Scope: src/capi/context.cpp, src/host/log.{h,cpp}, src/host/device_probe.{h,cpp}, src/device/cuda/cuda_probe.{h,cpp}, CMakeLists.txt, cmake/sources.cmake, tests/cpu/test_device_probe.cpp, tests/abi/test_context.py.
Out of scope: structured-exception handling (015); scenes and sessions (032, 052); the CLI (017); OptiX (041).
Reference: CUDA Toolkit release notes table "CUDA Toolkit and Corresponding Driver Versions" for the driver version that ships with the R590 branch; cite it beside the constant.
Do:
1. Device list: index 0 is the CPU device; CUDA devices follow (index 1..N) when the build has `ASTRORAY_GPU`. `ar_device_info` fields per D2.1; `status` is the `ar_status` that `ar_context_create` would return and `reason` its text.
2. CPU probe: `IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)`; the pure function `cpu_device_status(bool avx2)` returns AR_OK or AR_ERR_DEVICE with "CPU device needs AVX2".
3. CUDA probe in src/device/cuda/ (the only place CUDA headers are included): with `ASTRORAY_GPU` ON this task adds `find_package(CUDAToolkit 12.8 REQUIRED)` (host compiler only; device code starts in 027) and globs src/device/cuda/*.cpp into astroray_engine (003's rule); the cpu preset never looks for CUDA. Static cudart (`CUDA::cudart_static`), `cudaDriverGetVersion`, device count, name, memory, compute capability. Pure function `cuda_device_status(int driver_version, int cc_major, int cc_minor)` refuses a driver below the R590 minimum with "driver <found> is older than required <min>" and compute capability below 7.5. With no NVIDIA driver, `ar_device_count` is 1 and the CPU entry's `reason` stays empty; the CLI (017) prints "no CUDA device" from the probe's own reason, exposed as a host function.
4. `ar_context_create`: refuses with the device's status (the reason is read through `ar_device_get_info`, since no context exists on failure); on success opens the log at `log_path` (append, one `WriteFile` per line, line = `<UTC ISO time> <LEVEL> <text>`), logs the build record and device, stores `record_path` for task 033. No silent fallback to another device.
5. Every exported function body runs inside one guard macro that catches C++ exceptions and returns AR_ERR_INTERNAL with the message stored as the handle's last error; `ar_context_last_error` copies it NUL-terminated, truncated to `capacity`.
6. Tests: tests/cpu/test_device_probe.cpp (suite fast) for both pure functions, including old driver and missing AVX2 (0001 V6 unit part); tests/abi/test_context.py creates and destroys a CPU context through `_abi.py`, checks the log file has the build line, and checks that requesting CUDA index 99 returns AR_ERR_INVALID_ARGUMENT.
Acceptance: `pwsh tools/check.ps1 fast` passes; `<python> -m unittest discover -s tests/abi -p "test_context.py"` passes; on this PC `ar_device_count` reports 2 (CPU and the RTX 5070 Ti) in test_context's printed device list.
Look at: none.
Canary: tasks/014.canary.patch must turn red: astroray_tests_cpu "device/old driver refused", "device/no avx2 refused".
Budget: none.
Depends on: 013.
Size: M (about 350 lines); stop and report at 2x.
Progress:
