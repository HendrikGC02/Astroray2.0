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
