# 109 GPU fault containment inside Blender
Goal: whether Blender survives an engine GPU fault is untested -> with the addon loaded, a gate-hook GPU fault leaves Blender alive, shows the error through the report, offers only the CPU device afterwards and completes a CPU render.
Read first: 0007 d44 (addon after a fault), 0007 V15 (Blender fault part); 0001 D2.5, V14; blender/astroray/native.py; task 099's `ASTRORAY_TEST_GPU_FAULT` hook.
Scope: tests/blender/gui_fault.py, blender/astroray/native.py and blender/astroray/engine.py (only if the CPU-only fallback misbehaves).
Out of scope: engine-side injection (099); OIDN failures (M1b).
Reference: none.
Do:
1. gui_fault.py (GUI Blender run, A1 .blend): start a viewport render on CUDA with `ASTRORAY_TEST_GPU_FAULT=1`; check that Blender is alive, the report shows AR_ERR_DEVICE with 0001's restart message, the device setting now offers only CPU, and an F12 render on the CPU completes with a non-black image.
2. If a check fails, fix the addon (native.py's OSError handling or engine.py's device list) in this task.
Acceptance: gui_fault.py passes through `Invoke-ArBlender -Gui` and prints each check's result.
Look at: none.
Canary: tasks/109.canary.patch must turn red: tests/blender/gui_fault.py "TestFault.test_cpu_only_after_fault".
Budget: none on tiers (`check full` Blender suite).
Depends on: 099, 085.
Size: S (about 150 lines); stop and report at 2x.
Progress:
