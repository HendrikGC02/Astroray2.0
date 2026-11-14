# 079 Blender extension skeleton, identity check and packaging
Goal: Blender cannot load the engine -> the `astroray` extension registers a `RenderEngine` with 0007's flags only after the ABI hash and every struct layout match, loads the DLL by full path, and the `astroray_extension` target stages and builds an installable zip that installs and enables in an isolated profile.
Read first: 0007 d1-d3 (in-process, engine flags, build identity), d45 (zip contents, DLL by full path, OptiX cache dir), d44 (OSError means a poisoned context), 0001 D1.3 (`_abi.py` generated, staged, never committed), D10.2 (manifest fields), V4 (built zip part), 0007 V1, V16.
Scope: blender/astroray/blender_manifest.toml, blender/astroray/__init__.py, blender/astroray/engine.py, blender/astroray/native.py, CMakeLists.txt (target `astroray_extension`), tools/lint.py (zip check in section `nvidia`/`spdx` for built zips), tests/blender/test_install.py.
Out of scope: sync (080-083), rendering (084), viewport (085); OIDN DLLs (M1b).
Reference: Blender 5.3 extension documentation (`blender --command extension build`, `extension install-file`, manifest schema).
Do:
1. Manifest: `id = "astroray"`, `platforms = ["windows-x64"]`, `license = ["SPDX:GPL-3.0-or-later"]`, `blender_version_min = "5.3.0"`, permission `files` with its reason; no "Blender" in the name. Every .py carries the GPL SPDX line except the staged `_abi.py` and `_ops.py`.
2. native.py: `os.add_dll_directory(<extension>/bin)`, `ctypes.CDLL(<full path to astroray_core.dll>)`, then `_abi.check(lib)`; on mismatch registration fails with the message naming the hashes or the struct field; prints `ar_build_info()` to the console. Wraps engine calls so a ctypes `OSError` marks the context poisoned and afterwards only the CPU device is offered (0007 d44).
3. engine.py: `AstrorayRenderEngine(bpy.types.RenderEngine)`, `bl_idname = "ASTRORAY"`, `bl_label = "Astroray"`, flags exactly as 0007 d2; `update`, `render`, `view_update`, `view_draw` stubs that report "not implemented until task 084/085" through `self.report` (fail loudly). No Python threads.
4. `astroray_extension` target: stages blender/astroray, out/build/gen/_abi.py, out/build/gen/_ops.py and out/bin/astroray_core.dll (into bin/) under out/extension/astroray, runs `blender --command extension build --source-dir ... --output-dir out/extension` through tools/lib/blender.ps1's environment (machine lock), producing `astroray-<ver>-windows-x64.zip`; no loose kernel files (embedded, R13).
5. lint checks the built zip when present: every file has an SPDX line matching its rule, no NVIDIA file, `_abi.py` is Apache-2.0 (0001 V4, zip part).
6. tests/blender/test_install.py (run through Invoke-ArBlender): installs the zip with `extension install-file` into the isolated profile, enables it, asserts `ASTRORAY` is among `bpy.types.RenderEngine` subclasses and the build line was printed (0007 V1, V16 install part); a copy with an edited `_abi.py` hash fails registration with the hash message.
Acceptance: `pwsh tools/build.ps1 -Target astroray_extension` produces the zip; `pwsh -c ". tools/lib/blender.ps1; Invoke-ArBlender -Script tests/blender/test_install.py"` exits 0 and prints both cases' results.
Look at: none.
Canary: tasks/079.canary.patch must turn red: tests/blender/test_install.py "TestInstall.test_mismatched_hash_refused".
Budget: extension build time reported; not part of `fast`.
Depends on: 013, 004, 055.
Size: M (about 350 lines); stop and report at 2x.
Progress:
