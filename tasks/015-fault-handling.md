# 015 Structured-exception handling, minidumps and poisoned contexts
Goal: an access violation on an engine thread would kill Blender -> every engine-owned thread body runs inside one wrapper that writes a minidump beside the context log and poisons the context, after which every call on it, its scenes and sessions returns the fixed message.
Read first: 0001 D2.5 (structured exceptions, poisoning, log), D2.1 (no process-global state), V14 (CPU fault part); ARCHITECTURE section 8.
Scope: src/host/fault.{h,cpp}, src/host/thread.{h,cpp} (engine thread helper), src/capi/context.cpp (poison check in the guard macro), CMakeLists.txt (link dbghelp), tests/cpu/test_fault.cpp.
Out of scope: GPU faults and sticky CUDA errors (099); the CLI and Blender parts of V14 (099); process-wide handlers (forbidden by D2.5).
Reference: Microsoft docs for `__try/__except`, `GetExceptionInformation` and `MiniDumpWriteDump` (URLs in 0001 Sources).
Do:
1. `run_engine_thread(ContextState&, fn)`: runs `fn` inside `__try/__except(filter)`; the filter writes `MiniDumpWriteDump` (MiniDumpNormal plus thread info) to `<log_path>.<pid>.<n>.dmp`, logs one line with the exception code and dump path, sets the context's atomic `poisoned` flag with the message `Engine fault, dump at <path>. Save to a new file and restart Blender.`, and returns EXCEPTION_EXECUTE_HANDLER. No `SetUnhandledExceptionFilter` or vectored handler anywhere (lint-free grep in acceptance).
2. Every thread the engine creates (scene workers, init threads, session render threads in later tasks) is started through one helper in src/host/thread.cpp that calls `run_engine_thread`; add a comment there that new engine threads must use it.
3. The capi guard macro (014) first checks the poisoned flag of the handle's context and returns AR_ERR_INTERNAL with the message as last error.
4. A test hook compiled only when `AR_GATE_HOOKS` is defined (dev preset yes, release no): `fault_test_access_violation(ContextState&)` starts an engine thread that writes through a null pointer.
5. tests/cpu/test_fault.cpp (suite gate): with a temp log path, trigger the hook; assert the dump file exists and is non-empty, the context is poisoned with the exact message, the process is alive, and the log contains the lines written before the fault plus the fault line.
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="fault/*"` passes and leaves one .dmp under the test's temp directory; `pwsh -c "Select-String -Path src -Recurse -Pattern 'SetUnhandledExceptionFilter','AddVectoredExceptionHandler'"` prints nothing.
Look at: none.
Canary: tasks/015.canary.patch must turn red: astroray_tests_cpu "fault/access violation poisons context".
Budget: none (table-based SEH costs nothing on the no-fault path).
Depends on: 014.
Size: S (about 200 lines); stop and report at 2x.
Progress:
