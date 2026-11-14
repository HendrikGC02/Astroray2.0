# v1 lessons: toolchain and build

Compiler, CUDA, Windows and build-structure lessons from v1, phrased as constraints on the new design. The dev machine is Windows 11, RTX 5070 Ti (sm_120), 8 cores / 16 threads.

## Design consequences

- **No MinGW.** v1 hit three MinGW GCC 15.2 defects: structs of 32 bytes or more passed by value corrupted the callee frame, AVX spills to a 16-byte-aligned stack crashed (GCC bug 54412), and -O2 miscompiled stb_image's flat HDR decoder. If MinGW is ever reconsidered, these are the first three things to test.
- **No template-axis explosion in one translation unit.** v1's shade kernel had seven boolean template parameters (128 instantiations) in one TU, which took about 1,800 s of a 1,870 s clean CUDA build with ptxas peaking at 8.4 GB. Every boolean template axis doubles instantiations and compile time; prefer a runtime flag unless a measurement shows the runtime branch spills.
- **One build tree, one artifact.** v1 had five build trees (dev, CUDA, addon, addon-CUDA, per-worktree) and an OpenMP on/off split that forced a second full build (the addon CUDA build alone took 1,781 s). Each extra tree was a source of stale binaries.
- **Narrow headers.** A change to one layout-critical GPU header rebuilt everything (30 min before splitting). The rewrite's 30 s incremental budget requires that a header change touches few TUs.

## CUDA build structure

- When splitting or moving kernels between TUs, compare per-kernel resource usage (REG, STACK, LOCAL, SHARED, CONSTANT) before and after with `cuobjdump -res-usage` on the post-link module. Inlining depends on what else is in the TU: moving one kernel alone lost 560 B of stack because a helper stopped being inlined.
- Put the heaviest TUs first in the Ninja manifest; Ninja dispatches in manifest order, and late-starting heavy TUs became the critical path.
- Cap concurrent heavy CUDA compiles (v1 used a Ninja pool of 6). Eight concurrent ptxas runs at 2-8 GB each plus other load hard-reset the machine twice. Do not add `-split-compile` while the pool is already saturated.
- Define each `__constant__` symbol once and declare it `extern` elsewhere (legal with `-rdc`). Per-TU copies of header-local constant data grew the constant bank by about 1.2 KB of the 64 KB limit.
- Compiling device-only SASS (`120-real`) skips the fatbinary step but loses the PTX JIT fallback; keep embedded PTX if anything relies on JIT. OptiX-IR vs PTX for OptiX modules was unresolved (see open questions).
- A stale `CMAKE_CUDA_ARCHITECTURES` in the CMake cache produced bogus resource numbers (STACK 2640 vs true 3608). Check `cuobjdump --list-elf` shows the intended arch before trusting any number.
- Concurrent `-rdc` nvcc builds on one machine killed each other silently. Serialise GPU builds (already a rule in CLAUDE.md) and retry solo before debugging code.
- A guard that wipes the build when a layout-critical header's hash changes caught ABI-mixed binaries; keep an equivalent check, but design so it rarely fires.

## CUDA performance mechanics (observed on sm_120)

These came from v1's register-saturated fused kernel, which the rewrite avoids, but the mechanics apply to any hot kernel.
- At the register cap, any added live state spills to local memory and dominates wall-clock; leaner source can still produce more stack. Budget registers and stack per kernel and measure, do not infer from source.
- Large structs passed or returned by value on a shared hot path are paid by every caller: growing one shared material struct cost +52% on scenes that never used the new fields. Pass compact view structs by const reference and `static_assert` their size.
- Adding pointer parameters (out-flags, `const T*`) to a hot device function cost about 4% even when unused, because address-taken locals are forced into local memory. Pass by value.
- An opt-in alternate path can be free when off: body in a `__noinline__` device function, selected by a runtime constant flag. Inlining it spilled the off path.
- `if constexpr` removal of dead code in a specialised kernel cut stack 2576 to 2256 B and time by 1.1%.

## Compiler caches

- sccache: cross-worktree hits reached 97.7% for host C++ only when `SCCACHE_BASEDIR` was set to the checkout root; without it every compile missed. CUDA device code got 0% cross-tree hits.
- The sccache server dropped connections 10-12 minutes into very long nvcc compiles. Suspected cause: the default 600 s idle timeout (`SCCACHE_IDLE_TIMEOUT=0`, server restarted at build start), unconfirmed. Short CUDA TUs make this moot.
- After any cache change, confirm hits in the build log (`sccache --show-stats`) before believing it helps.

## Build correctness and stale artifacts

- Never trust a build's exit code alone. Confirm the output artifact is newer than the build start (or check its hash). v1 had several false greens that built nothing.
- The Visual Studio generator could exit 0 without relinking the Python module (mtime skew on a OneDrive-synced folder, or its build-state tracker). Keep the repository and build trees out of synced folders.
- After a header signature change, incremental builds linked stale objects; an ABI-mixed binary crashed after branch switches and gave a phantom first-bad commit when bisecting. Use clean-build baselines for any bisect or branch-switching investigation.
- Stale copies of the Python extension (repo root, test folders, a `Release/` subfolder) shadowed fresh builds because the test helper put them earlier on `sys.path`. Emit the module to exactly one location, print `module.__file__` and its mtime in every test session, and fail if a second copy is importable.
- A guard that rejects a compiled module older than HEAD caught real mistakes; keep it.
- A fresh worktree needed a one-time configure before the build script, which only built; the failure was confusing. Make the single build command configure when needed.
- A local MSVC (or MinGW) green build did not mean Linux GCC CI passed: GCC is stricter about templates and `std::variant` holding non-default-constructible members (a `unique_ptr` member broke CI three times). Require CI on the actual head commit.

## Windows shell traps

- `.bat` files with LF-only endings make cmd.exe mis-parse `^` continuations and exit 0 in about a second having run nothing. Force CRLF for `.bat` in `.gitattributes`.
- Running a `.bat` through Git Bash as `cmd.exe /c` lets MSYS rewrite `/c`, so cmd opens an interactive shell and exits 0. Run from PowerShell, or set `MSYS_NO_PATHCONV=1`.
- Backslashes in Windows paths inside a Bash heredoc that feeds Python are mangled (`\b` becomes backspace). Write helper scripts as files; build paths with `os.path.join`.
- Python text-mode writes on Windows convert LF to CRLF, turning a small patch into a whole-file diff. Use `newline=""` or bytes. Files with mixed line endings produce huge whitespace hunks when an editor normalises them; normalise in a separate commit and pin endings in `.gitattributes`. Check `git diff --stat` against `--ignore-space-at-eol`.
- `.ps1` scripts run by Windows PowerShell 5 (`powershell.exe -File`) without a UTF-8 BOM are read as cp1252; a single non-ASCII character makes them fail to parse, and a hook then silently never runs. Keep them ASCII or BOM-encoded.
- In PowerShell, `Set-Location` does not change the .NET process directory, so `[IO.File]` relative paths resolve elsewhere; a scripted merge resolution wrote into the wrong checkout. Use absolute paths.
- Redirecting native output to a file from PowerShell 5 produces UTF-16.
- Never kill processes by matching a keyword in their command line; the agent's own shell matched and the session died (9 hours lost). Kill by a PID file written at launch, list first, kill second.
- A global `*.png` ignore rule silently dropped evidence images from commits. Force-add exact files only (forcing a directory pulled in ignored run folders) and confirm with `git ls-files`.

## Machine load

- The PC hard-reset (Kernel-Power 41) while the GPU test suite, CUDA builds and several CPU-heavy jobs ran together. Run one heavy job at a time; the GPU suite never overlaps a GPU build; cap CPU jobs' thread counts. After a reset, clear stale lock files whose PIDs are dead.

## Open questions inherited from v1

- Whether the sccache connection drops are really the idle timeout.
- OptiX module format: one v1 source said build PTX first because of an unconfirmed crash with hardware-accelerated GPU scheduling; another recommended OptiX-IR.
- Whether a 5-minute clean build is achievable with CUDA plus OptiX on this machine; v1's best after splitting was about 8 minutes.
- Which MSVC runtime and Python ABI Blender's bundled Python requires of an extension module.
- Whether to keep a Linux CI job at all, and whether a GPU CI runner exists (v1 CI had no GPU).
- A CUDA 13 rebuild was approved as a trial but its outcome is not recorded.
