# 0000 — Owner decisions

Decisions made by the owner. Later records build on these; they change only if the owner says so.

## 2026-11-14

1. **Correctness target:** physics and the OpenPBR specification. No pixel-parity gates against Cycles.
2. **One kernel source** compiled for CPU and GPU. No second hand-written implementation.
3. **Blender:** ship on 5.3, design for 5.4.
4. **Implementation language is the lead's call.** The owner wants whatever is fastest, most efficient and best to work in; C++ is not a requirement.
5. **Licence:** fully open source, no commercial option needed. Engine core Apache-2.0, Blender addon GPL-3.0-or-later.
6. **Repository:** public, on the owner's GitHub profile.
7. **First usable release is feature-complete in breadth:** surfaces, volumes, sky and world lighting, textures, lights. Not a surface-only renderer with volumes later.
8. **First astrophysics target:** a black hole inside an emission nebula.
9. **Viewport targets:** comparable to Cycles. 60 updates per second desirable, 30 acceptable, 24 the floor for interactive work. Time to first frame comparable to Cycles, including on large scenes, where v1 blew up.
10. **Viewport resolution:** stay at or near full resolution; reduce it dynamically only when the update rate falls below the floor. No fixed navigation downscale.
11. **DLSS** should be built into the engine's viewport if it can be.
12. **Merge authority:** the lead merges and keeps working without asking.
13. **GPU testing:** this PC may be used as the GPU test machine.
14. **Addon only; no Blender fork.** Astroray ships as a normal extension for stock Blender. Blender's source is used as a reference: to find the fast paths the Python API really has, to copy how Cycles structures scene sync and display, and to match node behaviour exactly. Proposing small API additions upstream is allowed but nothing may depend on them.
15. **Diagnostics and reporting:** heat maps of sample count, bounce count and similar statistics; benchmark reports with graphs; showcase renders at each milestone.

16. **Custom and advanced material types must exist**, not only standard OpenPBR surfaces. Catalogue: `research/17-materials-and-custom-types.md`.
17. **Diffraction gratings are in the first release.** Consequence accepted by the lead: the first release also needs the light-tracing or photon path that makes a grating useful (a light through a grating onto a surface), so that machinery is designed in from the first milestone rather than added later.

18. **One extension behaviour per material is enough.** Stacks such as a coated grating are not required.
19. **Fluorescence should cover all the use cases:** optical brighteners, fluorescent minerals and pigments, and astrophysical fluorescence. No single one takes priority.
20. The owner has no further items to add to the materials catalogue for now.

## 2026-11-14, checkpoint 2

21. **Viewport rate for 2.0:** a steady 30 updates per second at full resolution is the target; 60 is a later goal; 24 remains the floor.
22. **Progressive viewport batching:** like Cycles, the viewport shows the first samples one at a time for fast feedback and then does more samples per displayed update as the image settles, so less time goes to display and more to rendering.
23. **Time to first frame:** gate at no more than 1.5x Cycles, aim for equal.
24. **In the first release:** fluorescence, hair (reusing the knowledge from v1), DLSS together with the other denoisers, and scientific outputs (filter-band images, photon counts, data cubes).
25. **Caustics:** point lamps and spotlights must cast caustics onto surfaces. Caustics from such lamps that are only visible through glass or in a mirror, or that light the scene again after another bounce, may be missing in 2.0 with a visible warning.
26. **Ambient Occlusion and Bevel nodes** may have no effect in 2.0, reported in the render report.
27. **First milestone** is a small working slice (meshes, lamps, textures in the viewport); everything else is designed in and built in later milestones before the first release.
28. **Metal thin film** is physically computed, even where less saturated than Cycles.
29. **Left to the lead:** no scheduled unattended jobs; releases built for other NVIDIA generations and labelled untested; the engine stays in Blender's process unless GPU faults prove frequent.

## Lead defaults, standing unless the owner objects

- Distribution from a self-hosted extension repository and GitHub releases.
- Unsupported shader nodes render magenta and are listed in a per-render report.
- Windows only for 2.0.
- The CPU target is for correctness and tests, and can also render.

## Lead defaults on the questions the roadmap needs before M1a (owner decision 29's "follow your judgment"; standing unless the owner objects)

- **No contributor licence agreement.** Inbound equals outbound. Consequence: the project cannot be relicensed without every contributor.
- **CIE 1931 values** are shipped from pbrt-v4 at a pinned commit with provenance and DOI recorded (0001 D8.2); no permission request is sent first.
- **OptiX and DLSS licences:** follow the pattern Blender itself ships (compiled OptiX modules in an open-source product; DLSS loaded from the driver, never bundled). This is a reading, not legal advice.
- **Unit anchor:** as 0003 decision 11. Science channels from Blender-unit emitters are flagged, not refused.
- **OptiX denoiser:** not built for 2.0. "DLSS together with the other denoisers" (decision 24) is read as OIDN plus DLSS; it is added only with a measured win (0008).

## 2026-11-15, build start

30. **Sign-off.** The owner signed off the planning records; the build phase starts.
31. **Speed over ceremony.** Build to the first release as fast as possible. The lead may run more than two implementers at once (0009 d27's limit is lifted; the machine lock still serialises builds). Use Sonnet implementers for the work, Haiku digests for bulk reading, Opus for review and critical thinking; testing stays at what the task files and gates require, with no extra testing beyond them. Opus review is required for kernel, physics, sampling, ABI and process diffs; other diffs land on green checks and their canary.
32. **Open-weight worker (0009 q3):** yes. OpenCode's DeepSeek flash model may do mechanical edits through one wrapper that returns a diff and a transcript.
33. **Subagent cache:** subagents keep the one-hour prompt cache (`subagentPromptCacheTtl`), overriding 0009 Appendix A.
34. **Repository:** public on the owner's GitHub from the start of the build phase (O6).

## 2026-11-19, M1a

35. **V7a inside the process.** V7a (0007 d35, the first-frame gate) is measured inside Blender's process instead of by screen capture, which cannot see Blender's window on the build machine; ruling 83 defines it. The 1.5x gate (decision 23) is unchanged.

## 2026-11-23, after M1a

36. **Process changes by the planning session (confirmed by the owner, 2026-11-23).** The owner's instruction for the M1b planning session, as relayed by the lead: "It will also apply any changes to agents, hooks, connectors, and any tooling etc. that will improve how development is conducted and allow it to be sped up. All agents including you are allowed and encouraged to also use the deepseek models from opencode to offload some usage". Applied under it, each reversible by the owner, evidence in `docs/retro/M1a-retrospective.md`:
    - B-08 times lint and tests; the build step is outside the timed window (retrospective R1: 60+ false failures). The 10 s limit is unchanged. 0009 d10 and the B-08 row carry it.
    - Implementers are limited per lane, not globally: any number of CPU-only tasks the lead can rule for, one GPU lane, one Blender lane (R17; replaces 0009 d27's "at most two", already lifted by decision 31).
    - The reviewer also reads the canary patch and main sources and gives PASS, PASS WITH FIXES or FAIL (R9). The implementer file carries the brief boilerplate (R4).
    - The open-weight worker of decision 32 is `tools/lead/deepseek.sh` (opencode, DeepSeek); repository text only.
    The owner then also approved branch `process/guards`, merged: deny rules for direct cmake, ninja, nvcc and blender and an allow-list for `pwsh tools/*` and read-only git and gh in `.claude/settings.json`, and CLAUDE.md rule 1's standing scope clause (an implementer may change what its change forces: its own budget and counter rows, registration lists, existing tests it breaks, each named in its report). 0009 d31's zero-hooks rule and the lint caps stand.

37. **M1a closed on the planning session's recommendations.** The owner accepted, for the M1a close: B-19's GPU idle row at 11.0 % on B-1M (partition row met at 9.4 %, ruling 85; no regressing commit), with 10 % kept as the target for M1b's re-measurement and B-5M measured after task 136; B-20 at 2.00x / 1.63x Cycles per 1080p sample, with 1.25x kept as the release target and performance gated on regression (task 140) until M2's lobe set is complete; B-31 at 39.8 / 88.5 ms per 10k instances warm / cold, with one task scheduled in M1b's slice 0. V7a is met (worst S1 1.17x). M1b starts with the first batch in tasks/ (docs/PLAN-M1b.md).

## 2026-11-23, M1b gate ledger

38. **Gate projection counts measured seconds.** With M1b's first landings the declared ledger projected 176-186 s against the 170 s hold line of B-09's 180 s, while measured costs ran far below declared (137: 0.6 s of +4; 152: 0.4 s of +2). The owner chose the measured ledger: once a task's gate cost is measured, the projection uses that number instead of its declared one, and a landing is held only when the measured gate max would pass 170 s. If it does, named long-stable statistical cases move to `check full`, which runs every 10 landings; B-09 stays 180 s and no check is weakened.

## 2026-11-23, M1b slice 0

39. **The `check perf` pass-time limit is 1.03, PROVISIONAL.** Task 140 measured the B-1M and B-5M pass time in two sessions 35 minutes apart (five back-to-back runs each, nothing else on the GPU): session 1 spread 0.15 % / 0.27 %, session 2 0.96 % / 0.31 %, the b1m mean drifting +0.25 % between them, and an unchanged binary reading 1.008 against the session-1 limit 1.009. The owner set the limit (task 140's new 0009 d13 row) at three times the larger measured spread, a pass-time ratio of 1.03, still PROVISIONAL until more sessions are measured. The same answer confirmed decision 38 for the gate: when concurrent grouping is used up, named long-stable statistical cases move to `check full`, and the lead proposes the list with each case's history before any moves.

## 2026-11-24, M1b second batch

40. **Open questions closed; the lead decides the rest.** The owner accepted B-31 as measured: the instance loop is met warm (S2 28.0 ms per 10k after task 161, limit 30) and accepted cold at 82.7 ms per 10k; reaching 30 ms cold needs the loop moved out of Python through an ABI change, which is not scheduled. For task 145's compile time and the other open questions the owner said to use the lead's judgement; the lead decided, with these reasons:
    - **B-05 for `shade_surface` is 7.0 s for M1b (amends ruling 72's 5.0 s).** Task 145 measured 5.16 s on a quiet machine (minimum of three serial runs, `check gate`), and the spine queue (ruling 91: 158, 163, 173, 174, 175) adds to the same TU. The alternatives are unchanged from ruling 72: the D10 split costs about 22 % render time (spike A) and `__noinline__` is forbidden (0002 D1), while the limit costs only that TU's rebuild. The row is re-measured at the M1b close; growth past 7.0 s returns to 0002 D10's ladder with the split measured, as an owner item. Every other TU keeps 3.0 s.
    - Opus implements kernel, sampling and concurrency tasks as marked O in tasks/README.md (kept from M1a; decision 31's Sonnet default holds for the rest).
    - Rulings 64-90 are folded into the records they amend, RULINGS.md keeping one index line each (task 185). A superseding ADR deletes the superseded file in the same commit and names it with `git show <sha>:<path>` (0009 q4: yes).
    - Exact JPEG parity (ruling 78) is left: 2 codes per channel stands.

## 2026-11-26, M1b spine (light tree)

41. **B-05 for `shade_surface` is 7.5 s for M1b (amends decision 40's 7.0 s).** Task 175 (light tree in the kernel) measured 7.31-7.40 s with `kernelgate.py --time` under the machine lock, alternating with main's 6.76-6.78 s (+0.55 to +0.64 s for the traversal). The timing still includes about 0.77 s of vcvarsall per compile that the build never pays (BACKLOG, 228); the measurement is kept as it is and the limit raised. Alternatives not taken: removing vcvarsall from the timing at 7.0 s, or holding 7.0 s by splitting 175 and cutting code through the 0002 D10 ladder. `tools/kernelgate.py` COMPILE_LIMIT_OVERRIDE_S carries the value.

## 2026-11-26, where a task's time goes

42. **Less waiting per task.** Ten tasks (221, 210, 223, 229, 222, 166, 173, 187, 174, 175), measured from dispatch to merge using the agent transcripts, `out/land/history.jsonl` and the PR and ci times: agent work 13 %, builds and test runs 15 %, waits on the machine lock or on background runs 18 %, waits for a lead or owner answer 22 %, the PC crash 12 %, review 6 %, landing and hosted ci 14 %. Worst cases: an 8 s build of 166 waited 3,978 s behind a hung gate; 175 sat idle 5.1 h overnight on the B-05 question; every landing waits 6-10 min for hosted ci after its local gate. The owner accepted these changes; each amends the records it names:
    - **Two implementers at once**, all lanes together, at most one of them in the gpu or blender lane (replaces decision 36's per-lane limits and 0009 d27's lane counts). The lead's landing runs beside them; its hosted ci wait leaves the machine to them.
    - **No waiting loops.** No agent polls, sleeps or loops on a log or output file. A tool runs in the foreground (it waits for the lock itself), or in the background with the agent's turn ended until the notification. A run still waiting for the lock after 10 min: the implementer commits, reports the holder the tool names, and stops; the lead deals with the holder. An agent idle for more than the one-hour cache (decision 33) is resumed only if its context is small; otherwise a fresh implementer continues from Progress and the git log.
    - **An owner question pauses the landing, not the work.** Unless the answer would change the design, the implementer finishes the remaining steps on the task's stated default or the lead's provisional ruling, the question goes to STATUS's owner list, and the task lands after the answer. Questions decision 40 leaves to the lead are decided by the lead at once.
    - **Hung lock holders and short jobs** (task 231): a tool holding the machine lock past its own ceiling stops itself loudly and releases, and a waiter names a hung holder within 15 min instead of 60. A short job (one build, one test case) goes ahead of a whole tier waiting for the lock.
    - **Timing checks under load** (task 232): a timing budget (B-08, B-12, the B-16 tripwire, B-38) that fails while another process loaded the machine re-runs once by itself and says so. It is red when the re-run fails, or when no load was seen. No limit changes.
    - **Hosted ci early and lean** (task 233; amends 0009 d12, d26 and B-11): `land` opens the PR when it pushes the landing branch, so ci runs beside the canary and the gate instead of after them. `check ci` keeps only what this PC cannot catch (clean checkout, the `cpu` preset build, the fast tier, the headless `bpy` suite) and drops the CPU gate part, which every landing gate runs here. `ci` and `astroray/gate` stay required.
    - **Tests are fast** (task 234): quiet-main medians target fast ≤ 6 s and gate ≤ 120 s; B-08's 10 s and B-09's 180 s stay the failing limits. A new gate case costing more than 1 s states why the fast tier cannot carry it. Under decisions 38 and 39, long-stable statistical cases move to `check full` until the gate meets its target.

43. **Answers to the open M1b questions (2026-11-27).**
    - **B-05 for `shade_surface` is 8.0 s for M1b** (amends decision 41's 7.5 s). Task 205 (false interfaces and the medium stack in shade_surface) measured 7.73-7.75 s quiet, interleaved with main's 7.19-7.29 s, min of 3 per round. The limit is raised; the alternatives (removing vcvarsall from the timing and re-basing; 0006 D17's fallback, the medium lookup in the intersect routing step) are not taken. `tools/kernelgate.py` COMPILE_LIMIT_OVERRIDE_S carries the value; growth past 8.0 s is an owner item again.
    - **0004 D10 tree row: 1.8x is accepted for this configuration.** Task 190 measured 1.795 on CPU and CUDA (d10_many_lights: 64 area lamps 0.2-1.0 m, 256 spp, full MIS with BSDF hits); light-only NEE reads 3.0-3.4 (175's gate case). The row's other conditions (no tile rejected after Holm, finite everywhere) are unchanged.
    - **The v1 "1 % relative margin" in the tile Welch test (0009 d17(v)) is the shifted statistic** t = max(|d| - 0.01 |ref|, 0) / SE, with ref the mean of the two tile means and the Welch df unchanged (the lead's provisional ruling, confirmed).
    - **V11 final (task 201):** a remedy task for small textured detail under the denoiser (b_tx's denoised relMSE 3x worse than raw, in the manner of 0008 Risk 6), a different third scene in place of b_tx, and `tools/fetch.ps1` may download the Cornell and shader-ball/HDRI assets so 201 re-runs with them.

44. **B-05 clock and B-19 on b_foliage (2026-11-27).**
    - **The timed compile leaves out vcvarsall before the spine continues.** Quiet main read `shade_surface` at 7.75-8.10 s interleaved against decision 43's 8.0 s, with about 0.35 s of scatter, and the spine queue (220, 208, 206, 209) adds code to that TU. `kernelgate --time` runs nvcc outside the MSVC environment, so every timed compile includes about 0.77 s of vcvarsall that the build never pays (BACKLOG, 228). Task 237 times nvcc inside the environment the build uses; the limit is re-based on that real compile time (the lead's reading: `shade_surface` keeps 8.0 s on the new clock, which leaves the spine about 0.8 s; the owner corrects this reading if the intent was 8.0 s minus the measured vcvarsall share). 220 starts after 237 lands. Holding 8.0 s on the old clock with the 0002 D10 ladder per task was not chosen.
    - **B-19's pq row is accepted red on b_foliage, as B-1M's idle row was at the M1a close (decision 37).** Task 170 measured pq_share 11.0 % against 10 % (idle 9.4 %, within), because each alpha layer costs two scheduler steps and a pass runs 132 steps. No remedy task is written in M1b; the row is revisited with M2's transparent-shadow work.

45. **The DLSS specular input moves to M8 as specular motion vectors (2026-11-28).** Task 209 (the specular hit distance, 0008 d15) could not be specified: the DLSS-RR Integration Guide (fetched with the owner's OK into the refs cache, section 3.4.9) defines the distance and the sky value 65504 only, and is silent on a diffuse first bounce, an extension delta bounce and accumulation. Cycles 5.3 feeds DLSS-RR specular motion vectors instead (integrator/denoiser_dlss.cpp line 442), written at bounce 1 after any reflected first bounce (kernel/film/denoising_passes.h lines 216-228), and spike F part A passed with specular motion vectors. 209 is withdrawn from M1b; M8's DLSS work (0008 d19, beside the motion vectors) writes specular motion vectors after Cycles 5.3, and the hit-distance plane is not built. Amends 0008 d15 and d22, ROADMAP M1b's front-loading line, 0003 d21 bit 21 (reserved, unused in M1b); 214 and 215 measure M1b's kernels without 209.

46. **M1b's close and M2's first approvals (2026-11-28, the owner's answers to the eighth lead's questions).**
    - **0001 V5 is deferred to M2.** M1b closes without it; V5 (the packaged-runtime check, of which `check.ps1` runs only the dumpbin part; OIDN DLL packaging) becomes an M2 slice-0 task (217's Q9).
    - **236: option (b).** The final denoise keeps Cycles' ACCURATE prefilter (0008 d18 unchanged); b_tx under the final denoise is reported, not asserted. 236 lands only its REF_VERSION 2 bump and the b_tx-reported row (the NONE candidate e8619050 is dropped), and a task exposes Cycles' Denoising Prefilter setting (None / Fast / Accurate, default Accurate) in the addon. The guide-side remedy (option c) is M2 material.
    - **The owner's look closes M1b.** M1b closes after the owner has looked at the M1b gallery and report (`tools/gallery.py --milestone M1b`, `tools/report.py --milestone M1b`: grating orders, OIDN before and after, metal edge colour); the lead records the verdict when given.
    - **M1b closes before the 238-247 debts land** (PLAN-M2 question 9); they land as M2 slice 0.
    - **B-05's 8.0 s and ruling 93's 16 / 16 B spill line for `shade_surface` carry into M2** (PLAN-M2 question 2).
    - **Downloads approved** (PLAN-M2 question 4): the ltc-sheen fit, the Heitz 2016 supplemental, open author PDFs of the papers the M2 lobe tasks cite, Byrnes 2016 (arXiv:1603.02720), Mitsuba 3 and pbrt-v4 as oracle programs, and the M2 gallery assets, each through `tools/fetch.ps1` into the refs or assets cache with its licence recorded.
    - **M3's first slice may start beside M2** (PLAN-M2 question 7).
    - Still open: PLAN-M2 questions 1 (lobe stack out of line, 0002 D1), 3 (does B-20 gate M2's close), 6 (B-17's cap) and 8 (B-10).

## 2026-11-28, M1b close and the second machine

47. **M1b "First light" is closed; the open questions are settled; development moves to a second PC.** The owner looked at the M1b gallery and report (25 images: grating orders, OIDN before and after, metal edge colour) and approved all of it, which was decision 46's last condition. The owner then left the remaining open questions to the planning lead ("make some decisions to close some of the open questions"); the answers below are the lead's under that delegation and stand unless the owner corrects them.
    - **Accepted at the close, as measured:** B-19 idle 11.9 % on B-1M and pq 10.9 % on b_foliage (10 % stays the target; decisions 37, 44); B-20 12.36 / 12.94 ms against 9.5 / 12.5 ms; b_tx under the final denoise reported, not asserted (decision 46); 0001 V5 deferred (task 269). M-1 is ratified at M1b scope only; the full-scope proxy is over 0002 D10 and is task 248's question.
    - **PLAN-M2 question 1 (lobe stack out of line):** allowed on one condition, decided now so nobody waits later. If task 248's table shows candidate O as the fastest admissible variant (inside D10 on proxy and production kernel, pass time within 1.03 on all measured scenes), the lead amends 0002 D1 and D10's "Never" list for exactly that use: `stack_eval` and `stack_sample` out of line, record and register file by pointer, callee spills attributed to the entry by kernelgate. Any other `AR_NOINLINE` use still needs the owner. Ruling 114's single use (208) stands.
    - **Question 3 (B-20):** B-20 does not gate M2's close. 1.25x Cycles stays the release target (M9); B-38's 1.03 per landing gates regressions; the cumulative line is reported at every slice close.
    - **Question 6 (B-17, the per-launch medium cap):** 4,096 stays until slice C measures with real majorants. The lead then sets the cap by rule, without a further question: the largest power of two whose longest `shade_volume` launch holds B-17's 100 ms limit on the reference machine, with the 20 ms target reported.
    - **Question 8 (B-10):** `check full` is limited to 20 min on the reference machine and enforced by script (old: 12 min, enforced by nothing, with quiet runs at 740-855 s). Task 272. Past 20 min, work moves out of the tier; the limit is not raised again without the owner.
    - **Coincident-hit tie rule (task 235's finding):** nothing is built. A scene with coincident faces of different materials declares the tie (0009 d17(iv)); image-mean CPU/CUDA comparisons mask the pair.
    - **B-11:** 450 s stays as an absolute limit for hosted `ci`; its "1.3 x the old max" basis is dropped (measured 335-379 s).
    - **Provisional rulings confirmed:** 94 and 100 (B-09's clock excludes build steps and compile timing), 109 (the D10 tree row's floor 1.70), 110 (V11's device-agreement row), 112 (8.0 s for the Ext TU), 114. B-38's 1.03 stays PROVISIONAL (decision 39).
    - **The owner's look is recorded here** (M1b retrospective Q10): from now on each slice close and milestone close records the owner's look as a line in STATUS, and a close as a numbered decision.
    - **Machines.** The desktop (`rtx5070ti-8c16t`, the id `cli/bench.cpp` computes) is the **reference machine**: every absolute budget row (0009 d13, `tools/budgets.toml`), every kernel resource row and every bench baseline keeps the meaning it was measured with there. Development continues on a laptop with an RTX 3000-class GPU, cloned from GitHub (`docs/SETUP.md`). On a machine that is not the reference machine: (a) correctness checks gate exactly as on the reference machine; (b) kernel resource rows stay gated, on an sm_120 compile-only build of the device TUs (ptxas needs no sm_120 GPU); (c) a wall-clock or rate row is compared with that machine's own recorded profile where one exists and otherwise printed as `off-reference` with the machine id, never failed and never silently skipped; (d) B-38 gates against that machine's own baseline; (e) an A/B measurement that decides by a ratio between variants on one machine (task 248's table, B-38) is valid on any machine and is recorded with its machine id; an absolute row (B-14, B-17, B-18, B-20, V7a against the limits) is re-measured on the reference machine before the milestone that gates it closes; while the owner travels the reference machine cannot be reached, so a slice or milestone whose only missing evidence is such a re-measurement closes as "closed, reference rows pending" with each pending row listed in STATUS, the laptop's own numbers reported beside it, and the re-measurement run as the first work when the desktop is reachable again (a row that then fails reopens as a remedy task, it does not undo the close); (f) no limit is edited to fit a machine. Tasks 267 (the CUDA architecture is chosen at configure time; today the kernels build for sm_120 only) and 268 (machine profiles in the tools) implement this and are the first two tasks on the laptop; until both land, the laptop runs `check fast`, CPU work and its own two landings through the tools of their own worktrees.
