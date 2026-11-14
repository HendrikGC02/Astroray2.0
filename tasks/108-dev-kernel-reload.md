# 108 Dev kernel reload and the corrupted dev-module fallback
Goal: kernel edits need an application restart -> with `ASTRORAY_KERNEL_DIR` set in a dev build, a rebuilt stage module is swapped at the next step boundary with the epoch reset within B-04, and a module that fails to load leaves the previous one running with a warning.
Read first: 0002 D14 (dev module swap), D15 (`KERNEL_MODULE_FALLBACK` row); 0009 d5 (dev reload), B-04; src/device/cuda/module_loader.h.
Scope: src/device/cuda/module_loader.cpp, src/device/cuda/cuda_device.cpp, src/host/scheduler.cpp (swap at a step boundary), src/host/session/session.cpp (epoch reset on swap), tests/gpu/test_dev_reload.cpp, tests/gpu/kernels/reload_variant.cu.
Out of scope: release builds (no loose modules, R13); CPU stages (rebuild and restart); other failure injections (099).
Reference: none.
Do:
1. In builds with `AR_DEV_KERNEL_DIR` and the variable set, module_loader checks each stage module's size and mtime at every step boundary (no watcher thread); a changed module is loaded, swapped before the next launch, and starts a new epoch; `ar_build_info` reports the directory.
2. A module that fails to load keeps the previous image of that stage and raises `KERNEL_MODULE_FALLBACK` (WARNING) naming the module and the loader error, until a good module loads.
3. Tests (gate): copy a stage module into a temporary kernel directory and render; replace it with reload_variant (the same stage adding a known constant) and check the next pass uses it and the epoch reset; a truncated module keeps the previous one and raises exactly one entry.
4. Timing: from the end of `tools/build.ps1 -Target <stage>` to the first pass with the new module, against B-04 (5 s), printed.
Acceptance: `out/bin/astroray_tests_gpu.exe --tier=gate -tc="dev_reload/*"` passes; the reload time is printed and within B-04.
Look at: none.
Canary: tasks/108.canary.patch must turn red: astroray_tests_gpu "dev_reload/swap at step boundary", "dev_reload/corrupted module keeps previous".
Budget: none (dev builds only).
Depends on: 099.
Size: S (about 200 lines); stop and report at 2x.
Progress:
