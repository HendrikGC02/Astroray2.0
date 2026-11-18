# 107 Edit-and-revert, determinism, record/replay and report checks on renders
Goal: render-level invariants are unchecked -> an edited-and-reverted render equals a fresh one, GPU repeats and permuted CPU runs agree, a CLI replay equals the in-process render bit for bit, and 0001 V15's render parts hold.
Read first: 0004 Verification row D25; 0002 Verification rows D6-D7, D12; 0001 V11 (CLI part), V15; tools/integrator_checks.py (076).
Scope: scenes/d25_edit_revert/, tools/integrator_checks.py (checks `d25`, `determinism`, `record_replay`), tools/check.ps1 (gate steps), tests/abi/test_v15_render.py.
Out of scope: viewport record/replay inside Blender (111); light-image determinism (M5).
Reference: none beyond the cited verification rows.
Do:
1. D25: render n passes, edit a material and revert it, render again; CPU threads=1 bit-identical with a fresh render at the same seed, GPU within 1e-6 relative.
2. Determinism and permutation (0002 D12, D6-D7): GPU twice gives film within 1e-6 relative and bit-identical deterministic AOVs; with `AR_DEBUG_PERMUTE` the CPU film is bit-identical at threads=1.
3. Record/replay (0001 V11, CLI part): a builder recording rendered by `astroray_cli render` is bit-identical on CPU at a fixed seed to the same builder driving the ABI directly in-process; the recording holds no AR_QUERY call.
4. test_v15_render.py (0001 V15 render parts): a TEST_INJECT_NAN material gives an INVALID_SAMPLE count equal to the injected samples and `astroray_cli render` exits non-zero only when an ERROR entry exists; replacing the magenta material by an edit removes its entries; a film reset removes FILM entries.
5. From 076: CPU threads=1 runs use the gate-build hook ASTRORAY_TEST_THREADS (076 adds it with ASTRORAY_TEST_POOL); comparison statistics that depart from per-pixel z at MIN_SPP need a null calibration of the exact statistic (106, tools/analytic.py --null); the gate has about 10 s left for this task (B-09).
Acceptance: `pwsh tools/check.ps1 gate` passes with the three integrator checks listed in its summary; `<python> -m unittest discover -s tests/abi -p "test_v15_render.py"` passes.
Look at: none.
Canary: tasks/107.canary.patch must turn red: `tools/integrator_checks.py --check record_replay` (the patch skips the recorder's capture of the UV staged array), tests/abi/test_v15_render.py "TestV15.test_invalid_count".
Budget: gate time +10 s at most.
Depends on: 076, 033.
Size: M (about 300 lines); stop and report at 2x.
Progress:
Started.
Do 1-3, 5 done: integrator_checks d25, determinism, record_replay (scene d25_edit_revert); gate +3.2 s, no family change (the checks are exact, not statistical).
Do 4 partial: test_v15_render.py holds the exit-status and magenta-replacement tests only. INVALID_SAMPLE count and film-reset tests are blocked: graph.cpp refuses TEST_INJECT_NAN and the compiler has no encoding for it; the ABI clamps inf and NaN inputs. Needs a lead ruling.
Canary: tasks/107.canary.patch turns d25, determinism, record_replay and both v15 tests red; no hunk for test_invalid_count (does not exist).
