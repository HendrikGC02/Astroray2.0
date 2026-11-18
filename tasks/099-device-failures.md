# 099 Device failure contract: injected failures and GPU fault containment
Goal: device failure paths are untested -> each 0002 D15 case other than the dev-module rows is injected and produces its status and report entry with nothing rendered silently, a GPU kernel fault ends CUDA for the process while a CPU session in the same process keeps rendering, and the longest launch stays within B-17.
Read first: 0002 D15 (failure contract table, injection tests), 0001 D2.5 (sticky errors, messages), D2.8, V14; 0009 B-17; src/host/session/test_hooks.h (101).
Scope: src/device/cuda/cuda_device.cpp, src/device/cuda/module_loader.cpp, src/host/scene/scene.cpp (init failure path), src/host/session/test_hooks.{h,cpp} (fault and capability hooks), src/kernel_gpu/test_fault.cu (prefix `test_`: gate-hook builds only, 027), tests/gpu/test_device_failures.cpp, tests/abi/gpu_test_fault_cli.py. Lead ruling adds: tools/check.ps1 (gpu full tier in `check full`), cmake/kernels.cmake (test_*.cu under the engine's AR_GATE_HOOKS condition), src/kernel/launch_config.h and src/kernel/budget.toml (ar_test_fault rows), src/host/session/session.{h,cpp} and src/capi/session_api.cpp (DeviceError keeps AR_ERR_DEVICE), src/host/device.h (untested_architecture), src/host/scene/scene.h, include/astroray/astroray.h, src/host/report_codes.def and 0001 D2.11 (DEVICE_INIT_FAILED, ruling 63), src/device/cuda/cuda_device.h.
Out of scope: dev-module reload and corrupted dev modules (108); fault containment inside Blender (109); OIDN and DLSS failures (M1b, M8); trim and spill (later milestones).
Reference: CUDA Driver API error codes (sticky errors require a process restart, URL in 0001 Sources).
Do:
1. Injection per D15 row, each with its `ar_status` and report entry: corrupted embedded cubin (a test build reads a damaged copy: GPU device unavailable, AR_ERR_DEVICE naming the architecture); mocked old driver version (context refused, found and required driver in the message); failing init worker (error posted when it happens, re-raised at the first upload, host scene data kept); a non-sm_120 device or PTX JIT -> `UNTESTED_DEVICE` (a test hook reports another compute capability).
2. GPU fault (0001 V14 GPU part): with `ASTRORAY_TEST_GPU_FAULT=1` a child `astroray_cli` process runs test_fault.cu's out-of-bounds write: the session gets AR_ERR_DEVICE with 0001's restart message and `SESSION_FAILED`, a second CUDA session is refused, and a CPU session in the same process still renders a non-black image.
3. Launch duration (V14 last part): the longest launch at the largest work count on b5m at 1080p stays within B-17's 100 ms cap (target 20 ms reported).
Acceptance: `out/bin/astroray_tests_gpu.exe --tier=gate -tc="device_failures/*"` passes every injection case; `<python> -m unittest discover -s tests/abi -p "gpu_test_fault_cli.py"` passes; the longest launch is printed.
Look at: none.
Canary: tasks/099.canary.patch must turn red: astroray_tests_gpu "device_failures/old driver refused", tests/abi/gpu_test_fault_cli.py "TestFault.test_cpu_survives_gpu_fault". (lead ruling: renamed file; also test_cli_session_fails)
Budget: test_fault.cu carries a budget row gated in `full`.
Depends on: 089, 015, 076.
Size: M (about 350 lines); stop and report at 2x.
Progress: started (implementer)
- Done: injections (corrupt image, old driver/cc floor, failing init worker, untested arch), fault kernel + CLI/ctypes child tests, B-17 timing hook; 11.2 ms longest launch (sort_scatter) on b5m 1080p.
- Found: a dead device's frees threw from Film/ActivePixels destructors (std::terminate): CudaDevice::free_memory is a no-op once dead. SESSION_FAILED is now raised before the FAILED state (a host polling state then report raced).
- Lead ruling: abi test renamed gpu_test_fault_cli.py (local gpu abi tests step, no skip); check full now runs `astroray_tests_gpu --tier=full` (whole tier, 1 case, ~4 s). Gate 168 s of 180 (compile timing +17 s: launch_config.h changed).
