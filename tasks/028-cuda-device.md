# 028 CudaDevice, astroray_tests_gpu and sampler bitwise agreement
Goal: nothing runs on the GPU -> `CudaDevice` implements the Device interface through the driver API (modules from RCDATA or the dev kernel directory, launches, streams, events, allocation under the reserve rule), the GPU test executable exists with a latent-error check after each test, and the sampler is bitwise equal on CPU and GPU.
Read first: 0002 D14 (driver API, no `<<<>>>`), D15 (failure contract rows for allocation and sticky errors), D4 (launch shape); 0007 d14 (reserve rule, R = 1.5 GiB); 0009 d11 (`astroray_tests_gpu` serial, latent-error check, explicit device), d17(i); 0003 check 24.
Scope: src/device/cuda/cuda_device.{h,cpp}, src/device/cuda/module_loader.{h,cpp}, src/host/device.h (factory), CMakeLists.txt and cmake/sources.cmake (`astroray_tests_gpu`), tests/gpu/main.cpp, tests/gpu/test_sampler_agree.cpp, tests/gpu/kernels/sampler_agree.cu.
Out of scope: asynchronous device init and failure injection (099 adds the injection tests; the init thread is 032's); OptiX (040-041); per-stage agreement harness (029).
Reference: CUDA Driver API reference (`cuModuleLoadData`, `cuLaunchKernel`, `cuMemGetInfo`), reached through `cudaGetDriverEntryPoint` from static cudart.
Do:
1. CudaDevice: retains the primary context of its device; one stream and event set per session; `allocate` asks the ledger and applies the reserve rule: proceed only if need + R <= free bytes from `cuMemGetInfo` (R = 1.5 GiB, PROVISIONAL, constant beside a comment citing 0007 d14), else the ledger's out-of-memory error; every CUDA call checked; sticky errors (illegal address, launch failure, launch timeout, ECC and the others CUDA documents as needing restart) mark the device dead for the process as observed from CUDA state, with the message of 0001 D2.5.
2. module_loader: loads a named cubin from the DLL's RCDATA (`FindResource`/`LoadResource`); in builds with `AR_DEV_KERNEL_DIR` and the env var set, from that directory instead; a host-only test entry loads a module from an explicit file path for test modules.
3. `launch(id, count)`: looks up the kernel function `k_<name>`, grid from `launch_config.h` (task 030 creates it; until then a default of 256 threads), passes one pointer to the device-resident `KernelGlobals`.
4. astroray_tests_gpu: globbed tests/gpu/test_*.cpp and tests/common/*.cpp (003's rule), links astroray_engine; doctest main like the CPU one (`--tier` including `all`, `--seed`, prints the build line); after every test case calls `cudaDeviceSynchronize` and `cudaGetLastError` and fails that case on a latent error; every test creates its device explicitly and never falls back to the CPU device.
5. test_sampler_agree.cpp (gate): sampler_agree.cu evaluates `sample()` and `sample_independent()` for 10^6 host-generated (id, index, stream, d) tuples; the CPU build of the same function (compiled into the test) must match bit for bit (0003 check 24, 0009 d17(i)); prints the count compared and differences (must be 0).
Acceptance: `pwsh tools/build.ps1` passes; `out/bin/astroray_tests_gpu.exe --tier=gate` passes and prints `compared=1000000 differences=0`; a test that launches selftest with an out-of-range pointer is not added here (099 owns fault injection).
Look at: none.
Canary: tasks/028.canary.patch must turn red: astroray_tests_gpu "sampler/cpu gpu bitwise".
Budget: first GPU test run; report its time (counts toward B-09's kernel part).
Depends on: 026, 027.
Size: M (about 450 lines); stop and report at 2x.
Progress:
