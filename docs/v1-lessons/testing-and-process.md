# v1 lessons: testing and process

How v1's tests and gates misled it, and what its multi-agent workflow taught. Each bullet gives the rule and the reason.

## Design consequences

- v1 compared hand-ported CPU and GPU implementations with statistical parity gates, which cost most of its late-stage effort (gate scores re-audited and re-signed for weeks). With one source compiled for every target, backend agreement becomes a deterministic check of the same code, and statistical tests are reserved for comparisons against physics, analytic results and external references.
- v1's package/round/orchestrator machinery, status corpus (373 KB) and three copies of agent definitions are not carried over; their lessons are dropped. Status stays one page.
- v1's CI had no GPU, so GPU regressions merged green; six green merges in one round left the GPU path about 2x darker than CPU. The rewrite needs a local hardware gate before merge for any change that GPU code can reach.

## Energy and non-vacuity

- Furnace and energy tests render linear output and assert both a floor and a ceiling. Display encoding clamps to [0,1], so a gamma-encoded furnace can see only loss: a white conductor reached 4.14 linear while every encoded furnace read exactly 1.000. Enforce with a fixture, not convention, and when auditing an energy test check the render call first.
- State the colour encoding of both sides of every comparison. A gamma-encoded render against a linear oracle faked a stable 1.8-2.4x per-channel ratio on a dim scene.
- Refuse to pin or compare an all-black or constant image. A bit-equality canary certified black against black; smoke tests passed on black output.
- Check that reference assets are real: three "different" gate scenes were byte-identical placeholders. Add non-vacuity checks to scene gates (checker contrast, environment contribution, hair coverage above zero).
- Fix scene defects (z-fighting cards) in the scene, then re-render references; do not loosen the gate.

## Comparing renders

- Windowed SSIM between two independent-RNG renders is unreachable at practical spp (0.786 at 64 spp; a 64x64 environment-only scene floored at 0.962). Gate on per-channel mean ratio with an explicit band, plus a per-tile Welch test over several seeds per side with Holm correction; keep SSIM as a diagnostic.
- Gate on the mean over several decorrelated fixed seeds (v1 used 8), not one seed; single-seed gates measured seed luck.
- Use fixed non-zero seeds; a sentinel seed made a "deterministic" test random.
- For a fair comparison: equal samples (or equal time on the same device for noise claims), adaptive sampling off, denoising off, clamps off, shared exposure, firefly-free scenes.
- A per-channel ratio that stays constant across spp (say 2, 64, 1024) is a deterministic bug (matrix, colour space, units, missing scale), not noise. One such bias was an XYZ buffer labelled RGB.
- Conversely, ratio near 1, small colour difference and SSIM climbing with spp is noise and under-convergence, not a defect.
- ROI means on glossy objects are dominated by a few highlight pixels ("chrome 23% dim" had a median ratio of 0.96-1.11). Report median with p10-p90 and save a ratio map.
- Chi-square BSDF tests: low integration resolution can dominate the statistic (about 90% of one glass test's value was quadrature error at resolution 4); only resolution 8 or more showed sampler residual.
- An oracle derived from the same paper as the engine proves self-consistency only. Before calling a divergence from Cycles physical, get an independent reference (explicit microgeometry ray tracing, analytic, or another renderer such as Mitsuba).
- Metric gates tuned on a deterministic case pass on noise in the stochastic case: salt-and-pepper chromatic noise passed hue-spread and coverage metrics meant to detect a clean rainbow. Look at the image.
- Replace exact run-to-run bit identity with an absolute tolerance (owner chose 1e-6) rather than disabling FMA contraction.
- Thresholds come from the measured ceiling minus a margin. Tighten only with evidence; never relax without independent numeric or image evidence.

## When a fix breaks a test

- If a correct fix fails an old test, investigate both. If the test encodes the old bug, re-pin it against an independent reference in the same change, with the numbers in the assert message. Never revert a correct fix to keep a stale test green (v1 reverted a correct sky fix because eight tests were calibrated on the wrong glow).
- Attribute failures with a baseline build of clean main, not by reasoning about what the diff can reach; reasoning was wrong and the baseline caught a real coupling.
- Re-verify inherited claims ("byte-identical", "verified") directly; several were false.

## Test coverage and runner traps

- The tests a change's author names are a floor. A material change passed chi-square, furnace and parity yet rendered metal 2.8x too dark; run the render-level material suites too.
- A change that implements an expected-failure test must remove the marker in the same change; otherwise "1 xfailed" hides a real failure. Suspiciously fast GPU tests mean the GPU path was not reached.
- Feature gates test output effect, not reachability: toggling denoise or adaptive sampling must change the image and reduce noise where expected.
- Never use fail-fast (`-x`) for a sweep; the summary still says "N passed" with later tests unrun. Read the failure count.
- Process-global GPU state leaked between renders and made suite results order-dependent; when bisecting run the same selection in the same order, and run a failing test alone first.
- Render caches keyed on seed survived rebuilds, so an "identical metric after a fix" was a cache read. Key caches on the binary's hash.
- Tests labelled CPU silently selected the GPU when it was built. Select the device explicitly in every test.
- CI path filters skipped tooling tests, so tooling commits broke tests while CI stayed green. Run the relevant tests locally for tooling changes.
- Any header that GPU code includes, transitively, counts as GPU code for routing to the hardware gate; a filename-based carve-out let a GPU change merge untested.
- Tests and scripts write output only into an ignored per-run tree; curated evidence is never written by tests. Evidence is a labelled contact sheet (reference | CPU | GPU, ROI boxes, scene and spp) and charts with a JSON sidecar; no empty or near-black images; images under ~2 MB / 1600 px. Feature paths carry no dates, ticket ids or author names; provenance goes in the sidecar.
- Delete bulk intermediates (frames, dumps, run folders) at the end of a measurement; keep summaries. One scratch folder reached 38 GB and filled the drive.

## Performance measurement

- The GPU's boost clock drifts about 5%, which swamps sub-5% A/B results. Burn in about 15 renders, take min-of-N (N about 10), and compare only at the same clock state (locking clocks needs admin).
- A slow leg is usually another process on the GPU; check `nvidia-smi` before each leg and keep an interleaved base-change-base bracket whose base legs agree within ~1%.
- Device allocation layout alone can shift a kernel by a few percent, stably. Shift allocations (a dummy 6-64 MB allocation first) and compare across 2-3 shifts before calling a regression.
- Measure latency through the real end-to-end path, not an in-process harness.

## Visual inspection

- Look at the rendered image as well as the numbers; several bugs survived numeric gates until someone looked (a flipped ROI, noise passing metrics).
- Vision models read brightness and artefacts well but invent few-pixel position and size shifts (2 of 8 scenes). Check geometry numerically (phase correlation, ROI means); a model never decides a merge.

## Agent workflow

- Implementers routinely declared C++/CUDA work done without building it (two of three shipped uncompilable kernels in one run). Require a build-log tail and a smoke test with every claim. Hand-derived expected values are often wrong (most failures in one test batch were wrong expectations); re-derive from the reference.
- "Completed" from a delegated agent means the process ended, not that it succeeded: check that the output exists and read it. Give delegated research one narrow deliverable and a write-first budget; broad tasks ran out of steps and produced nothing.
- Cheap models drafted notes and bounded single-file work well but stalled with no code on hard multi-file physics or kernel changes. Escalate after one failed attempt.
- Isolation: an agent's first action confirms `git rev-parse --show-toplevel` is its own worktree and aborts otherwise (five agents once wrote into the main checkout). Cap concurrent code-writing agents (v1: 3). `git stash` is shared across worktrees, so agents never stash. Each agent gets its own scratch folder; one overwrote the lead's helper script.
- Git traps: after merging on the host, local main is stale, and restoring a file from it reverted landed work (use origin/main). Rebase acts on the checked-out branch, so check it first. Confirm a merge landed on origin before deleting a worktree or branch (a refused merge exited 0 when piped). Commit with explicit paths when other changes are in the tree. Never move a branch head while a verifier runs against it.
- Usage limits kill agents mid-task: commit work in progress early and append evidence incrementally. Past ~300k tokens or at a phase boundary, write a handoff (branch, commit, verified numbers, next steps) and start a fresh agent; never mid-build.
- Agents do not poll builds; the lead runs builds and gates. Polling queued builds burned whole usage windows.
- Never wrap a script that takes a lock in the same lock; it deadlocked for 2 hours.
- Notes go stale. Deleting an index entry did not stop agents reading the note; delete obsolete notes. Model ids and routing policy changed every few weeks; keep them out of permanent docs.
- For design help, present the real tradeoff axes; never force a fixed number of options. If only one option is viable, say so.
- Batching several items into one build-and-test cycle saved build time but can mask which item failed; keep per-item tests and bisect inside the batch.
- Features landed switched off (progressive sampler, light tree, adaptive sampling inert without the sampler). Every opt-in needs a written condition for turning it on, and reports state which settings were active.
- Keep a short per-session mistake log; audit half-finished features periodically.
- Tooling notes (dated, may be stale): in auto permission mode the classifier denied closing issues and installing into the user's Blender profile; do not route around a denial. Put one "Closes #N" per line in PR bodies. Repository auto-merge was disabled; a CI watcher can report "no checks" right after a push, so require at least one check and none pending before merging.

## Open questions inherited from v1

- Which v1 reference images are trustworthy; several were calibrated on a buggy engine or were placeholders. Re-validate any reused reference against an analytic or independent oracle.
- Whether the GPU suite's order dependence was fully fixed (moot if engine globals are banned, but worth an order-shuffled run).
- Whether to run a GPU CI runner or rely on a local hardware gate.
- The right tolerance policy for comparisons against Cycles given unreachable independent-RNG structural similarity.
- Whether batching items per build is still worth its masking risk once builds are fast.
