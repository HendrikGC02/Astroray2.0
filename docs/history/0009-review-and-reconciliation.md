# 0009: review and reconciliation

Moved unchanged from the end of `docs/decisions/0009-build-test-process.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ.

1. ACCEPTED: release kernels are embedded in the DLL (decision 3); dev mode swaps files only under `ASTRORAY_KERNEL_DIR`, which `ar_build_info()` reports (decision 5). The NVRTC question goes to the lead (challenge, Risks a), and 0007 D45's loose modules are flagged (Risks b).
2. ACCEPTED IN PART: `fast` takes no machine lock, timers exclude lock wait, and a per-tree lock was added (decision 8); "exactly one tree" became one `dev` tree per checkout, ≤ 3 (decision 4, spine reading). REJECTED: "worktrees build `cpu` only". `fast` already builds only CPU targets, so no worktree pays a GPU build at task start; kernel tasks need their register numbers before `land` (0002 D10 "Declare").
3. ACCEPTED: the timestamp rule is replaced by a post-build `ninja -n` no-work check plus the sha check (decision 5).
4. ACCEPTED: `land` writes the task file and diff to `out/land/NNN/` for the reviewer; the canary is a committed patch that `land` applies, requiring the named tests to fail (decisions 26, 28; appendix rule 6).
5. ACCEPTED: 0002 D1 and 0001 V2-V4/V9 lints in `fast`; V1, V5 (dependents), V6 and V11 in `gate`; V5 in Blender in `full`; `tests/abi/` and `astroray_kernels_cpu_f64` added. Also aligned the source layout with 0001 D5.2 and 0002 D4/D11.
6. ACCEPTED: B-05/B-06 now point to 0002 D10, which owns the baseline (`src/kernel/budget.toml`), the growth rule, the stack limit and spill policy; compile times are serial, never from `.ninja_log`, at most three TUs per gate. The nightly conflict with 0002 is flagged (Risks c, d) rather than resolved here.
7. ACCEPTED: a constant declared family size per suite, lint-enforced; a rotated-seed failure is never dismissed (20 re-seeds, more than one failure is a defect, otherwise the seed is pinned and a task opened).
8. ACCEPTED: 17(v) now uses v1's validated configuration (8 seeds, Holm at 0.05, 1% margin, zero rejected tiles) with the correct citation, plus a null-calibration task.
9. ACCEPTED: A1 is limited to lossless configurations (truth 1, or ρ); lossy lobes go to the energy test and O3, and 15(a) states that quadrature comparison is consistency only.
10. ACCEPTED: the toolset line 14.44 is pinned locally; CI requires ≥ 19.44 and logs it; M1 measures the hosted image.
11. ACCEPTED: per-merge work counters (+2%) and a 1.25x timing tripwire are in `gate` (B-16, decision 18). BVH node counts are unobservable on OptiX hardware traversal, so they are omitted.
12. ACCEPTED: exporter equivalence compares dumps, not call sequences (since ruling 59: deterministic AOV dumps).
13. ACCEPTED: spike code is kept at tag `spikes-final`; the CUDA 13 acceptance uses 0001's 10% rule and the product gates.
14. ACCEPTED: B-13 and B-15 now point to 0007 D33/V8 and D35 (≤ 1.5x, goal 1.0x); the unsourced 1.25x and the 4K 2 ms figure are gone. Like-for-like Cycles sync is flagged to 0007 (Risks g); "comparable" is owner question 1.
15. ACCEPTED: tier budgets fail at the limit, with no warning band (the 1.2x had no source in PLANNING_PLAN either); the 80-line ADR cap is adopted for build-phase ADRs, with the planning records exempt.
16. ACCEPTED: Wilcoxon signed-rank on paired log ratios, confirmed across 3 allocation shifts.
17. ACCEPTED: dispersion is adopted with pbrt-v4 plus the analytic A10 as second witness; layered coat is informational only (OpenPBR's coat is an albedo-scaling model); single-witness status is recorded in `meta.toml`.
- Also changed, found while resolving 6: the 17(ii)/(iii) thresholds now defer to 0002 D12 instead of restating different numbers. Unchallenged by the review: decision 25 (posted status instead of a runner).

## Reconciliation (rulings applied)

- G1: "What lands when" reduced to the two lists; milestone assignments removed from decisions 2, 7, 12, 13, 15, 17(v), 20 and Verification.
- G2: every time budget in the project is stated once in decision 13 (B-17..B-25 added from 0002, 0004, 0005, 0007, 0008); kernel resources cite 0002 D10, first frame cites 0007 D35.
- G3: nothing runs on a timer; every "nightly" check (uncapped registers, every-TU timing, SASS-architecture check, OptiX registers, traversal rays/s) is in `check full`; owner question 2 removed.
- G4: the "scene and harness hooks exist from M1" line for A13/A14 deleted; no other reserved item remained.
- Ruling 1: analytic tests read the film's fp64 moments resolved from 0008's per-sample buffer (decision 16, assumptions).
- Ruling 2: light-tracing splats use 0008's pixel filter (assumptions).
- Ruling 3: seeds are hash(test name, repo constant) with no special value; 0003 owns the sampler (decision 15, assumptions).
- Ruling 12: OIDN and Embree are our builds, produced once as release assets, outside the normal build (decision 6).
- Ruling 13: release kernels embedded, loose modules only in `dev` (decision 3); Risks (b) removed.
- Ruling 14: launch duration B-17 (target 20 ms, fails above 100 ms).
- Ruling 15: CPU code built with `/arch:AVX2` (decision 2); Risks (f) removed.
- Ruling 16: nvcc only; B-04 is the reopen trigger; NVRTC challenge, Risks (a) and the NVRTC identity test removed.
- Ruling 17: gate checks spills and stack of capped cubins; the probe build moves to `check full` as a warning (decisions 3, 11, 12, 18, B-06, Verification); register challenge and Risks (e) removed.
- Ruling 20: `deps.toml` mirrors 0001 D9.1 and lint rejects unlisted shipped dependencies (decision 6).
- Ruling 21: B-15 cites 0007 D35 without restating it; owner question 1 and Risks (g) removed.
- Rulings 4-11, 18, 19, 22, 23: checked; nothing in this record restated or contradicted them beyond the items above (Risks (c), (d) removed as settled by G3 and 0002 D10's matching three-TU rule).
- Ruling 29: decision 13's table carries every time budget: B-14 and B-25 restated, B-26 (OIDN share), B-27 (DLSS share), B-28 (resolve share), B-29 (statistics overhead), B-30 (VM overhead), B-31 (main-thread sync), B-32 (cancel, new-view and orbit latency), B-33 (controller response), B-34 (progressive batching) and B-35 (curved-region branch cost) added from 0003, 0004, 0005, 0007 and 0008; Verification, Risks and Assumptions follow.
- Ruling 30: B-14 targets 30/s at full 1080p with 24/s the floor and 60/s a later goal; B-25 to B-27 use that target; B-33 and B-34 hold the controller and batching numbers.
- Ruling 31: toolchain pins stated only in decision 2 (Assumptions).
- Ruling 32: tools run on Blender's bundled Python and NumPy; the tool venv removed (decisions 1, 2); astropy's install location raised in Risks.
- Ruling 34: the NGX headers fetched by `fetch.ps1 deps` (decision 6).
- Ruling 39: `src/denoise/dlss/` added to the layout; V3's directories follow the include rule (decision 1, Assumptions).
- Ruling 41: the f64 kernel build is never shipped (decision 3).
- Ruling 49: Review section headed as history; the `.npy` dump assumption corrected to 0008's FITS dumps; the settled V3 directory note removed.
- Ruling 29 (second pass): B-36 (light-pass share) and B-37 (timing constants of 0003, 0007 and 0008) added; the basis of B-04, B-13, B-15, B-18 and B-19 takes the measured and derived times other records no longer state (0001 D6, 0002 D7/D11, 0005, 0007 D23/D28); ranges in decisions 12-13, Verification, Risks and Assumptions extended to B-37.
