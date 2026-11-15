# 113 Fault dumps written out of process
Goal: the engine-thread fault filter calls `MiniDumpWriteDump` on its own process, which Microsoft documents as unreliable (it suspends every other thread, then takes heap and loader locks); the fault tests hang in 10-20% of runs under concurrent load -> the filter starts a small helper process that writes the dump from outside, waits for it with a bound, and a fault never hangs the engine or the tests.
Read first: 0001 D2.5 (fault handling, minidump beside the context log, poisoning), ruling 65; src/host/fault.{h,cpp}; src/host/thread.h; tests/cpu/test_fault.cpp; CMakeLists.txt (astroray_core, astroray_extension staging, astroray_cli).
Scope: src/host/fault.cpp, src/dump/main.cpp (new helper), CMakeLists.txt (helper target, copy beside astroray_core.dll in out/bin and in the extension stage's bin/), tests/cpu/test_fault.cpp, tests/tools/fault_stress.ps1 (new), tools/README.md.
Out of scope: changing what poisons a context, the poison message text or the log lines' meaning; kernel faults (CUDA); a global unhandled-exception filter (0001 D2.5 forbids one).
Reference: https://learn.microsoft.com/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump (remarks: call it from a separate process; `MINIDUMP_EXCEPTION_INFORMATION.ClientPointers = TRUE` when the pointers are in the target process).
Do:
1. `astroray_dump.exe` (src/dump/main.cpp, static CRT like the engine, no other deps than dbghelp): arguments pid, thread id, address of the EXCEPTION_POINTERS in the target, output path; opens the process with PROCESS_QUERY_INFORMATION | PROCESS_VM_READ (and what MiniDumpWriteDump needs), writes MiniDumpNormal | MiniDumpWithThreadInfo with ClientPointers = TRUE, exits 0 on success, non-zero with one stderr line otherwise.
2. fault.cpp: the filter locates the helper beside the module that contains it (GetModuleHandleEx on a function address, then GetModuleFileNameW), builds the command line in a fixed-size stack buffer (no heap, no CRT locks before the helper runs), CreateProcessW, waits at most 30 s (PROVISIONAL), and records "dump at <path>" on exit code 0, else "dump could not be written at <path> (<reason>)", where reason is "helper missing", "helper exit <n>" or "helper timeout". Poisoning is unchanged and happens whatever the dump outcome.
3. CMake: build the helper with the engine, copy it beside astroray_core.dll in out/bin and into the extension stage's bin/, so tests, CLI and Blender find it.
4. tests/tools/fault_stress.ps1 -Runs n -Parallel p: runs `astroray_tests_cpu.exe -tc=fault*` n times in each of p parallel loops with a 20 s timeout per run; prints runs, hangs, failures; non-zero exit on any hang or failure. Not part of any tier (it needs minutes).
5. test_fault.cpp keeps its assertions (one dump file per fault, non-empty, log names it, context poisoned); add that the dump is a valid minidump (first four bytes "MDMP").
Acceptance: `pwsh tools/check.ps1 fast` and `pwsh tools/check.ps1 gate` pass; `pwsh tests/tools/fault_stress.ps1 -Runs 40 -Parallel 4` reports 0 hangs and 0 failures in 160 runs (paste the summary line); before the change the same command shows hangs (paste that line too).
Look at: none.
Canary: tasks/113.canary.patch must turn red: astroray_tests_cpu "fault/access violation poisons context" (for example by passing a wrong pid to the helper).
Budget: none.
Depends on: none.
Size: S (about 200 lines); stop and report at 2x.
Progress:
