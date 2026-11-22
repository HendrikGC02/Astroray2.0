# M1a retrospective: what was built and how

Written at the close of M1a "First pixels", while tasks 134, 135 and 100 were still in flight.
Reader: the planning agent for M1b and later, and the owner. Evidence sources are in the appendix.
Numbers marked (est.) are estimates from transcript timestamps; numbers from sub-agent digests that
were not re-checked against the raw source are marked (digest).

**Lead's correction of the original notice.** The retrospective agent reported that no owner go-ahead
existed for this session. That is wrong: the owner's second message in the closing lead session, sent
mid-turn (which the transcript extract missed), told the lead to spawn this retrospective agent once the
milestone was ready, and then the planning agent. The lead started the retrospective a little before the
M1a close because it only reads, and said so to the owner. The planning step is authorized by the same
message. Q4-U1 below is withdrawn; R15's limits ledger still stands on its own.

## 1. Executive summary

**Outcome.** In nine calendar days (one planning day, about 47 hours of lead-session wall-clock
over three build sessions) the project went from an empty repository to a spectral wavefront path
tracer that renders textured, lamp-lit meshes in the Blender 5.3 viewport and F12 on CUDA and CPU
through the final C ABI. 132 of 135 tasks are landed through 107 PRs. The viewport target (B-14),
first-frame gate (V7a, worst 1.17x Cycles against 1.5x) and launch-length limit (B-17) are met.
Three numeric gates are open and need an owner decision at the close: B-19 (partition 12.0 % and
11.8 %, idle 13.9 % and 12.1 %, limit 10 %), B-20 (2.0x and 1.63x Cycles, limit 1.25x) and B-31
(instance loop 39.8 ms per 10k, limit 30). The M-1 probe's spills are over allowance (130/376 B
against 128/192), which ROADMAP already makes the first M1b task. The gallery (task 100) is not
landed.

**How it went.** The process worked, and almost without the owner: about 10 real owner messages
per session and three owner rulings in the whole build. It had two very different phases:

| | Session 1 (kernel, host, film; CPU-heavy) | Session 2 (Blender, measurements, GPU) |
|---|---|---|
| Tasks landed | about 91 in 22 h | 40 in 24.6 h |
| Median dispatch-to-merge | 27 min (p90 48) | 99 min (p90 285, max 415) |
| Median implementer active time | 19 min | 65 min |
| Concurrent implementers | 5 to 11 | 2 to 5 |

The fourfold slowdown is not model quality. It is one machine lock, serialized landings, windowed
Blender, gate time creeping to its limit, and measurement tasks that each spawned follow-ups.

**Top findings.**
1. **Opus review is the best-value step in the process.** 97 review runs used about 2 % of all
   tokens. Roughly 40 to 48 % of reviews found a real code or measurement defect that tests had
   not (races, use-after-free, NaN paths, silent drops, self-comparing tests). Only 4 to 8 % were
   wording-only. Keep it; feed it better (it cannot see the canary patch).
2. **Landing mechanics were the largest avoidable cost.** At least 14 post-merge cleanup
   failures, 12 or more canary-line or canary-patch refusals, 3 stale-recording refusals, 2 hosted
   CI runs that never started, 6 hosted CI failures, and repeated manual reverts of the "(land)"
   commit. In the first 14 hours of session 2, 20 landings summed to about 15 hours of landing
   wall-clock (est.).
3. **The fast-tier budget B-08 misfires.** At least 60 `check fast` runs failed B-08 only because
   a rebuild was inside the timed window. Every one passed on rerun. The rule "a failed budget is
   yours to fix" was replaced in practice by "rerun once", and the lead pushed to main seven times
   with it red.
4. **Task Scope lines were wrong more often than right.** In the middle of the build, 11 of 13
   consecutive tasks needed a file outside Scope. About 40 implementer runs stopped before writing
   code and waited for a ruling. Each stop was rule-compliant and cost one lead round trip.
5. **Three defect classes escaped because no tier covers them:** an end-to-end F12 render on CPU
   of a scene with image jobs (found by the gallery, task 135); GPU performance regression (idle
   share went from 9.2 % to 13.2 % between tasks 123 and 133 unnoticed, task 134); and `check full`
   content (red on main for hours after task 124, because landing runs only the gate).
6. **Measurement-first worked, but as an unbounded chain.** 093 led to 118, 119, 120, then 122,
   123, then 133, then 134, and B-19 is still open. 24 tasks (22 % over the 111 at sign-off) were
   added mid-milestone. The pattern needs a pre-agreed remedy ladder, a quiet-machine protocol and
   a stop condition.
7. **Limits moved in response to failing runs** without a single place that lists them: the
   orbit-gap bound three times in one landing (130), B-31 positions-only from 1.1 to 6.0 ms (129),
   B-10, B-18 and B-28 reset by implementers. Each has a defensible derivation; together they are
   the least protected part of the process.
8. **The documentation system no longer has one place that states current truth.** 21 build-phase
   rulings override record text; only about 8 record lines mention them. ROADMAP was never edited
   after sign-off. STATUS is a 35-item prose backlog at its 50-line cap, hand-condensed at least
   six times.

**Top recommendations** (full list in section 8): fix B-08's definition (R1); make the canary
machine-checked before landing (R2); harden `land` and fold `land-pr.sh` into it (R3); move brief
boilerplate and the canary recipe into the implementer definition and add a scope pre-flight (R4,
R5); add the three missing gates (R6); give measurement tasks a protocol and a ladder (R8); route
models by task class and keep review (R9); fold rulings back into records and make ROADMAP and the
backlog live tables (R12); re-estimate later milestones at 4.4x to 5.4x (R13).

## 2. Outcome (question 1)

### 2.1 Delivered against ROADMAP's M1a goal

| Goal element | State | Evidence |
|---|---|---|
| Final C ABI with generator, bindings, build identity | Done | tasks 012-017, 031 |
| CpuDevice and CudaDevice, one kernel source | Done | 026-028; agreement harness 029, 042, 077 |
| Wavefront scheduler, sort, sessions, film, AOVs, FITS | Done | 043-054, 101, 104, 117 |
| Material VM and compiler, EON and GGX lobes, E table, textures | Done (M1a subset) | 055-064 |
| Point, sphere, rectangle, disk, triangle and world lights, NEE | Done | 065-068, 074, 075, 105, 116 |
| Analytic and integrator checks, detection power | Done | 072-078, 106, 107 |
| Blender sync, F12, viewport present, batching, heat maps | Done, with gaps listed in STATUS | 079-087, 110, 111 |
| Every gate live from the first task | Mostly: `fast`, `gate`, `ci`, `land` live; 29 of 37 budget rows are text only | tools digest |
| Bench scenes, Cornell box, S3 | Done | 088-090 |
| Gallery and report | Not landed (task 100, blocked on 135 and 134) | worktree state |

### 2.2 Gates

| Gate | Limit | Measured at close | State |
|---|---|---|---|
| B-14 viewport rate, B-1M | p5 at least 30/s; floor 24/s all scenes | median 31.4, p5 30.4; B-5M median 28.5, min 24.4 | Met, thin margin (PR #100) |
| V7a first frame vs Cycles | at most 1.5x | S1 1.14x/1.17x, S2 0.81x/0.77x, S3 0.79x/0.39x | Met; S1 margin is about one redraw (PR #106) |
| B-17 longest launch | 20 ms target, fail above 100 | 3.85 / 3.88 ms | Met |
| B-19 partition + sort share | at most 10 % | 12.0 % (B-1M), 11.8 % (B-5M) | **Open** (bench/history, PR #107) |
| B-19 GPU idle share | at most 10 % | 13.9 %, 12.1 %; was 9.2 %, 9.4 % after 123 | **Open, regressed**; task 134 bisects |
| B-20 pass time vs Cycles | at most 1.25x | 2.00x, 1.63x (from 5.5x, 6.4x) | **Open**, no remedy scheduled |
| B-31 sync on S2 | loop 30 ms/10k; full read 65 ms/1M; move 0.1 ms | loop 39.8 warm / 88.5 cold; cold read about 100; cold move 0.110 | **Open** |
| M-1 probe (0002 D10) | stack 512 B; spills 128/192 B | stack 480 B; spills 130/376 B | Stack met; spills over; first M1b task |
| B-18 OptiX ready | 260 / 490 ms (reset from measurement) | 157 warm / 364 cold | Met |
| B-21 main-thread calls | 5 ms | 0.05 ms; first context create exempt (ruling 81) | Met with exemption |
| B-23 light build | 50 ms / 10k lamps | 33 ms; 1M emissive 196 ms (was 8.1 s) | Met |
| B-29, D13 | 2.4 %; 64 MiB | +1.83 %; gap 13.8 MiB | Met |
| B-08 fast | 10 s | 6.2-8.5 s warm; 10-77 s after a rebuild | Met warm only |
| B-09 gate | 180 s | 121 s after task 121; 168.6 s at PR #107 | Met, headroom gone again |
| B-10 full | 12 min | net 497-514 s; not enforced by any script | Met on paper |
| Spike C trigger | B-14 fails | B-14 passes | Not run; rests on B-14 alone while B-19 and B-20 fail |

Owner decisions due at the close: accept, re-limit or schedule B-19, B-20 and B-31; whether the
"wavefront is adequate" conclusion (no spike C) stands with B-20 at 1.6-2.0x; JPEG decode parity
(ruling 78); deletion of superseded records (0009 q4).

Known functional gaps carried forward (from STATUS): orthographic camera renders black in the
viewport; viewport sync over-sends on material, image and light edits; `film_exposure` has no
viewport effect; Russian-roulette `min_bounces` text and kernel disagree; NaN material inputs
render black without INVALID_SAMPLE; three once-flaky tests are unowned.

## 3. Throughput (question 2)

### 3.1 Timeline

| Phase | UTC window | Wall | Lead active | Output |
|---|---|---|---|---|
| Planning | 11-14 | 1 day | not in transcripts | 111 task files, 9 records, 63 rulings |
| Session 1 | 11-15 00:35 to 22:39 | 22.1 h | 17.2 h | about 91 tasks (PRs #3-#66 plus about 20 hand-landed) |
| Session 2 | 11-18 10:38 to 11-19 11:15 | 24.6 h | 16.6 h | 40 tasks (PRs #67-#106) |
| Session 3 | 11-22 10:12 onward | 3+ h | | 133 landed; 134, 135, 100 in flight |

Session 1 lost 3 h 20 min to a usage-limit stop (four implementers were stopped mid-task with
uncommitted work). The owner ran `/compact` five times across the two long sessions.

### 3.2 Cycle time, first dispatch to merge (100 tasks with both timestamps)

| Slice | n | min | p25 | median | p75 | p90 | max |
|---|---|---|---|---|---|---|---|
| Session 1 (excluding the 4 paused tasks) | 60 | 9 | 19 | 27 | 35 | 48 | 164 |
| Sessions 2-3 | 40 | 25 | 64 | 99 | 166 | 285 | 415 |
| Sonnet-implemented | 69 | 9 | 19 | 29 | 57 | 197 | 415 |
| Opus-implemented | 31 | 27 | 46 | 60 | 110 | 164 | 207 |

Slowest: 127 (415 min, three attempts, blocked on screen capture until an owner ruling), 108 (395,
three review rounds, stale canary, driver hang), 111 (389, landing took about 4 h), 085 (285,
three review rounds), 097, 131, 086, 109.

### 3.3 Estimate against actual

| | Tasks |
|---|---|
| ROADMAP estimate for M1a | 25 |
| Task files at sign-off | 111 (4.4x) |
| Added during the build (112-135) | 24 (22 % over 111) |
| Total | 135 (5.4x) |

Of the 24 added tasks, about 17 came from measurements (118-134), 4 from engine gaps found while
implementing (114-117), 2 from tooling (112, 113) and 1 from the gallery (135). ROADMAP's later
milestones sum to about 267 estimated task files. At the same ratio that is 1,170 to 1,440 tasks.
Diff size was also underestimated: at least 20 tasks finished at more than twice their Size and
every overrun was accepted. Median landed diff was 872 added lines (Sonnet tasks) and 624 (Opus).

### 3.4 Where wall-clock went (est.)

| Component | Session 1 | Sessions 2-3 | Basis |
|---|---|---|---|
| Implementer working | median 19 min | median 65 min | transcript active time |
| Stop-and-wait for a ruling | 1-5 min each, about 25 stops | 1-5 min each, about 15 stops; 4 h for 127 | digests |
| Review (Opus, read-only) | 1-7 min, in parallel | 2-5 min, in parallel | reviewer transcripts |
| Review fix rounds | 5-15 min | 5-60 min; three rounds on 085 and 108 | digests |
| `land prepare`: canary baseline and patched builds, then gate | 3-6 min | 5-10 min; about 72 min with nine windowed-Blender canaries (111) | land logs (digest) |
| Hosted CI wait | about 2-4 min | 4-6 min (241-345 s) | PR open-to-merge, PR #102 |
| Machine-lock wait | seconds to 10 min (hung test) | commonly 20-180 s; 570 s (120), 997 s (111), 15-25 min (100) | reports |
| Landing retries | about 10 refusals | about 25 refusals | digests |

In session 1 the binding constraint was the lead's own attention (dispatch, ruling, landing by
hand until `land` existed). In session 2 it was the machine: one lock, one landing at a time, and a
gate that grew from 77 s to 187 s before task 121 overlapped its halves.

### 3.5 Tokens

| Role | Runs | Output tokens | Cache-read tokens | Share of cache-read |
|---|---|---|---|---|
| Lead (Opus), two sessions | 2 | 1.24 M | 850 M | 28 % |
| Implementer, Sonnet | 101 | 6.84 M | 1,131 M | 37 % |
| Implementer, Opus | 31 | 3.54 M | 978 M | 32 % |
| Reviewer, Opus | 95 | 1.47 M | 57 M | 2 % |
| Other (spikes, one investigator) | 3 | 0.15 M | 19 M | 1 % |

Per run, an Opus implementer read about 31.6 M cached tokens against 11.2 M for a Sonnet one,
before the price difference. The tasks differ (Opus got the harder ones), so this is not a
controlled comparison, but it sets the size of the lever. The lead is the second lever: 28 % of
volume at Opus price, much of it re-reading landing logs, dependency scans and hand-backs.
Session 3 and the planning day are not in these totals.

## 4. Friction catalogue, ranked by cost (question 3)

Cost is lead attention plus machine wall-clock, estimated. "Count" is a lower bound from digests.

| # | Friction | Count | Cost | Root cause | Fix |
|---|---|---|---|---|---|
| F1 | Scope line omits files the task needs; implementer stops before code | about 40 stops; 11 of 13 tasks in one stretch | 1 lead round trip each; blocks an implementer slot | Scope written from task text, not from call sites; registration files, budget rows and forced test fixtures never listed | R5 |
| F2 | Serialized landing behind one lock and one gate | every landing in sessions 2-3 | about 15 h landing wall-clock for 20 tasks; 092 waited 50 min in the queue | "Do not commit to main while a landing runs"; prepare repeats everything when main moves | R3, R7 |
| F3 | B-08 fails after any rebuild | 60+ implementer runs, 7 on main | one rerun each (1-2 min) plus a retry of prepare several times (090 three times) | rebuild is inside the timed tier | R1 |
| F4 | Canary line and patch mechanics | 12+ landing refusals; about 55 review findings on canary coverage | 5-70 min per refusal (097: four attempts) | free-prose `Canary:` line parsed by regex at landing only; `*.py` tokens in prose run as tests; patch includes itself; stale after rebase; hunks mask each other; CRLF | R2 |
| F5 | Post-merge cleanup fails | 14+ | 5-10 min of lead time each; non-zero exit after a successful merge | harness holds a lock on a stopped agent's worktree; a shell cwd inside the worktree | R3 |
| F6 | Windowed-Blender canaries | 111 (72 min prepare), 130, 085 | hours | each canary case starts a fresh windowed Blender under the lock, baseline and patched | R2, R7 |
| F7 | Hosted CI | 6 failures (037 twice, 115, 090, 118, 111); 2 never started | 10 min to 1.5 h each | CI differs from local: 2-4 cores, no GPU, bpy wheel headless suite, no cached assets; `pull_request` event lost | R3, R6 |
| F8 | "(land)" commit left on the branch after a failed prepare | 5+ | manual revert; one agent rebased it away | prepare commits the task-file deletion before the gate | R3 |
| F9 | Stale `out/scenes` recordings after an ABI change on main | 3 refusals (076 twice, 089) | about 25 min | land does not regenerate recordings before the canary baseline | R3 |
| F10 | Gate budget squeeze B-09 | 2 failed landings (185 s, 187 s); 106 reworked (+38.5 s) | about 1.5 h plus task 121 | every task adds gate time; no per-task allowance | R7 |
| F11 | Machine-lock waits are invisible | pervasive | up to 25 min; a hung test held it 10 and 28 min | no timeout, no owner, no wait message; Blender waits uncounted | R7 |
| F12 | Measurement chains | 093 to 134 (8 tasks); 094 to 126; 095 to 129; 096 to 130; 091 to 132 | 24 unplanned tasks | see section 5.4 | R8 |
| F13 | Lead hand edits with heredocs | about 60 | 1-3 min each; several broke files or the build | no helper for Scope, Progress, STATUS or ruling edits; backslash and CRLF damage | R10 |
| F14 | STATUS 50-line cap | 6+ hand condensations; main lint-red twice | 2-5 min each | a prose backlog in a capped file | R12 |
| F15 | Flaky and hanging tests | minidump deadlock (10-20 % of parallel runs); driver hang 1 in 36; 4 one-off flakes | 2 investigations; 28 min and 10 min lock holds | in-process MiniDumpWriteDump; unvalidated ELF to the driver; no step timeout until 132 | done (113, 132); R6 |
| F16 | Usage-limit stop | 1 | 3 h 20 min; four agents lost uncommitted work | no checkpoint discipline before the limit | R11 |
| F17 | Agents cite sources from memory | 8+ | review rounds; one false statement written into a record (053) | no local copy of pbrt-v4, Cycles, OpenPBR | R14 |
| F18 | Permission classifier refusals | about 12 | acceptance left unverified until landing (054, 102, 114); 2 denied pushes | no allow-list; no tool to run one test case under the lock | R10 |
| F19 | Environment variables missing in agent shells | 9+ | one retry each until tools read the user environment | fixed in tools mid-build; still repeated in every brief | R4 |
| F20 | Screen capture of Blender impossible on this machine | 095, 096, 127 (two attempts) | about 4 h parked; one owner ruling | acceptance assumed a method never tried | R8 |
| F21 | Agent turn limit (150) | 126, 100 | one resume each | long measurement and gallery tasks | R4 |
| F22 | Analysis scripts lost with worktrees | 3 (119, 120, 123) | rewritten each time | nsys and ncu analysis kept in scratch | R8 |
| F23 | Re-review for record text | 1 (133) | one fix round | reviewer treats record numbers as must-fix | R9 |
| F24 | Stale worktrees and branches | 40 `worktree-agent-*` branches, 3 unregistered directories, `bisect-134`, `keep/120-graphs-B` | clutter; prefix match broke a merge (120) | no sweep | R10 |
| F25 | Shared scratch and stray files | patch collision (124); minidumps in cwd (108 committed eight); a PNG at the drive root | minor | fixed names in a shared scratchpad | R4 |

nsys and Nsight oddities were minor: ncu text output fails to convert (read through `ncu_report`),
OptiX refuses CUDA graph capture, and a context log slows Cycles by 20-25 %.

## 5. Process design (question 4)

### 5.1 Task granularity

The 60-line task file is the right size. Tasks that fit it and touched one subsystem cycled in
under 30 minutes. What failed was not length but three fields:

- **Scope** (F1). Fix the way Scope is produced, not the rule that enforces it.
- **Acceptance numbers.** At least 10 tasks carried a number that could not be met as written
  (1e-6 in float, "1,0xx,xxx triangles", exact JPEG, exact sRGB through POST_PIXEL, V7a by screen
  capture). Each cost a ruling.
- **Size.** Ignored about 20 times, always accepted afterwards. It counts tests, which are often
  two thirds of a diff. As written it protects nothing.

### 5.2 Parallelism

0009 d27's two-implementer limit was lifted by O31 and the build shows why both numbers are wrong
in different places. Session 1 ran 5 to 11 implementers on CPU-only kernel and host tasks with
tolerable conflict cost (about 20 rebase conflicts, all in a handful of shared list files:
`lint.py` SECTIONS, `CMakeLists.txt`, `deps.toml`, `budget.toml`, `launch_config.h`). Session 2
could not use more than about three because every Blender or GPU step takes the single machine
lock. The limit should be per lane: many CPU-only implementers, one GPU or Blender lane. LEAD.md
and tasks/README still state the lifted limit.

### 5.3 Model split and review policy

- **Sonnet implements, Opus for physics and shared contracts:** sound. The lead's own judgement
  mid-build was that Sonnet's weakness is judgement, not code: it chose instead of asking (051,
  117, 110's tolerance, 086's threshold). Opus implementers stopped and asked more (mean 3.3
  follow-ups against 1.8), which is the behaviour the rules ask for, at about 2.8x the token volume
  per run.
- **Review (O31):** well calibrated and cheap. First-round FAIL rate was about 85 %, which sounds
  like miscalibration but is not: 35-48 % of reviews found a real defect, about 80 % found a test
  that could not fail. The reviewer's limits are the issue: it reads only `task.md` and
  `diff.patch`; the canary patch is deleted by the land commit and so is absent from the diff it
  judges; it cannot run anything or see a render; it has no reference sources. Verdict labels are
  inconsistent (FAIL on one minor in 107 and 121; a bare "no findings" on a kernel diff in 119).
- **Skipped reviews (17 PRs, "REVIEW SKIP (O31)")** were mostly right, with one visible cost:
  task 124's cache-path change broke `check full`, and Blender-side Python was the class routinely
  skipped. The fix is a gate (R6), not more review.
- **Fix rounds without re-review** were the norm (about 9 of 13 in the late phase) and three
  re-reviews that did run found new defects in the fix (033, 085, 108). A fix that changes control
  flow in concurrency or teardown code should be re-reviewed; a fix that adds a test should not.

### 5.4 Measurement-first

The principle (measure, then decide; never loosen to pass) held under pressure and paid off:
B-5M pass time 67 ms to 16.9 ms, 1M-emissive light build 8.1 s to 0.2 s, S2 first frame 4.1 s to
1.5 s. Its weaknesses:

- **No stop condition.** B-19 was chased through eight tasks and is still open. The ROADMAP
  ladder (a), (b), (c) was a pre-measurement guess; the real order was (c), (a) as built, (b)
  measured and dropped.
- **Measurements were taken on a busy machine.** Several reports say other agents' Blender or
  gate runs inflated numbers. The idle-share "regression" handed to 134 may be partly contention;
  133's captures are not shown to have run with the machine otherwise idle.
- **Predictions were relayed as outcomes.** Ruling 84 said the fix "should take the row to about
  6.7 %"; measured 12.0 %.
- **History was not recorded.** `bench/history` had no committed lines until session 3, when 25
  lines were back-filled from PR bodies by a Sonnet agent. The tool that should have caught the
  idle regression had no data.
- **Limits were reset by the measuring task** (B-18, B-28, B-10, B-31 positions-only), usually as
  1.3x the measurement. That is a documented rule, but it turns a budget into a description.

### 5.5 Documentation and records

Over-engineered:
- Nine records (3,412 lines, about 160k tokens), each with about 50 lines of superseded review
  history; 84 rulings; a spine; 35 owner decisions; a 413-line ROADMAP with 90 exit-check ids.
  To learn the current wavefront design a reader opens five or six files.
- 59 of 249 post-sign-off commits touch STATUS, LEAD or tasks/README; 33 task landings edit
  records; 22 commits are rulings. About one commit in four is coordination text.
- Caps (0009 d31) produced dense 300-600 character lines rather than brevity (STATUS,
  tools/README), and LEAD.md's handover section grew uncapped into a 50-line lore file.

Under-protected:
- **Q4-U1. Owner gates.** Withdrawn (see the lead's correction at the top): the owner had authorized
  this session. Making owner-gated actions explicit in LEAD.md's "Never" list is still cheap.
- **Q4-U2. Limit changes.** No ledger of budgets or bounds changed during a milestone
  (finding 7 of the summary).
- **Q4-U3. Record truth.** Rulings override records without editing them (R72 against 0009 B-05,
  R83 against 0007 V7a, R82 against V12, and others). No lint resolves cross-references.
  ARCHITECTURE cites CLAUDE.md rule numbers that no longer exist. 0009 d28's field list differs
  from the lint's. The land skill still describes the pre-030 gate.
- **Q4-U4. Rules enforced by prose only.** "Change only Scope files", "do not call cmake or
  blender directly", "do not run test executables outside tools" and "lead only" for `land` have
  no mechanical guard. There were violations of each (115 and 107 ran test executables directly;
  108 committed crash dumps; about 25 out-of-Scope edits were accepted after the fact).
- **Q4-U5. The lead on main.** The lead pushed lint-red and B-08-red commits to main at least
  nine times, weakened one test itself (052's stub count), and issued three instructions that an
  implementer or lint had to correct (052, 115, 069). The lead's direct commits bypass review and
  gate by design; the design assumed they would be rare.
- **Q4-U6. 29 of 37 budget rows are enforced by nothing.** `astroray_tests_cpu --tier=full`
  appears never to be invoked by any tier (tools digest; to be confirmed).

## 6. Quality (question 5)

### 6.1 Defects that escaped a landing

| Defect | Introduced | Found by | Latency | Why it escaped | Cheapest gate that would have caught it |
|---|---|---|---|---|---|
| F12 on CPU returns a black frame for scenes with image jobs (session latches generation 0) | 084 with 110 | the gallery in task 100; fixed in 135 | 4 days | the gate's Blender smoke renders A1, which has no image jobs; CPU session tests commit before start; CUDA's slower start hid the race | a headless F12 render of S3 on both devices with a mean-luminance floor, in `gate` (135 adds the CPU half) |
| GPU idle share 9.2 % to 13.2 % | between 123 and 133 | 133's own baseline; 134 bisects | 3 days | no performance regression check; bench is in no tier; no history lines existed | `check perf`: bench on B-1M and B-5M against the last history line, run on landings that touch kernel, device or session |
| `check full` red at Blender `test_images` | 124 | 091's full run, first misattributed | 3+ h, about 5 landings | landing runs `gate` only; 124 was landed with review skipped | run the Blender Python suites when a diff touches `blender/`; or `full` every N landings |
| GPU gate hang, 1 run in 36 | 108's corrupted-module test | 131's full run | hours; 28 min lock hold | truncated ELF passed to a driver call with no length; no step timeout | step timeouts (added by 132) |
| Fault tests deadlock 10-20 % of parallel runs | 015 | two gate hangs | 3 h | in-process minidump | step timeouts; fixed by 113 |
| Test cases in an untiered suite never ran | 045, 058 | lead reading a log | under 1 h | runner ran only named tiers | runner refuses untiered cases (added) |
| 103 wrote film by pool slot, not sample id | 103 | 071's implementer | 1.5 h | no test with pool slot different from sample id | covered by 071 |
| Double-precision trig in 062/063 lobes | 062, 063 | nvcc `-Werror` in 073 | 17 h | lobes were not yet compiled for GPU | compile every kernel header for GPU at its own landing |
| 32-bit key truncation on the Blender side | 083 | 083's review | none | 060's review fixed one side only | lint for `& 0xFFFFFFFF` and hand-copied ABI constants |
| Statistics default, d8 block test, y-anchor self-comparison | 049, 106, 072 | review | none | tests that cannot fail | canary matrix (R2) |

### 6.2 What caught defects

- Opus review: the main net. Highest yield on concurrency, lifetime and teardown code (032, 033,
  035, 036, 052, 059, 085, 102, 104, 115, 126, 132), on numerics at boundary inputs (062, 064, 065,
  071, 103), and on statistical-test validity (072, 076, 084, 106).
- Canary discipline: the reason "a test that cannot fail" was the commonest finding instead of an
  invisible one.
- Implementers reading neighbouring code (071 on 103; 113 on the dump deadlock).
- The lead looking at images: 17 evidence comments, none found a defect. The one visual defect of
  the milestone (the black F12 frame) was caught by an automated near-black check in the gallery
  script. Looking is worth keeping for new scenes; numeric image checks catch more.

### 6.3 Recurring defect classes worth a mechanical check

Rejected-generation state not rolled back (032, 035, 036, 059); pointer identity used as a key
(067, 068); snapshot read field by field (102, 104); silent drop of an unsupported input (039, 056,
068, 069, 083, 105); invented constants without a `Ref:` (019, 020, 021, 023, 063, 066, 085, 086);
hand-copied ABI constants in Python (083, 084, 085, 110); expected values computed by the code
under test (071, 082, 102); `unittest.main()` instead of `support.main()` (080, 087, 131);
ticket numbers in comments (108, 110, 118, 124).

## 7. What to keep

- Worktree-per-task with a canary per test and a gate before merge.
- Opus review on kernel, physics, ABI, concurrency and process diffs.
- "Stop and ask, never choose" for record conflicts and loosened thresholds. It prevented at
  least four loosened limits (110, 086, 074, 106).
- Fail-loud lint: it twice refused the lead's own shortcut (a skip without a task id).
- Out-of-process fault dumps, step timeouts, and the untiered-test refusal: each was a tooling fix
  made the day the problem appeared.
- The lead ruling in the task file or RULINGS and replying to the same agent.
- One-hour prompt cache for sub-agents (cache reads dominate token volume).

## 8. Recommendations (question 6)

Ordered by expected gain per cost. **[A]** the planning agent can apply as file changes;
**[O]** needs an owner decision first; **[A/O]** file change that also alters a record or an owner
decision. Gains are estimates from the counts in sections 4 to 6.

### Tier 1: do before the first M1b task

| # | Recommendation | Expected gain | Cost | Who |
|---|---|---|---|---|
| R1 | **Redefine B-08 as test time, with the build outside the timed window** (or time the build under its own row). In `tools/check.ps1` build first, then start the tier clock; keep the 10 s limit for lint plus tests. Remove "rerun once" from LEAD.md and briefs. | Removes 60+ false failures per milestone and several prepare retries; restores "a failed budget is yours to fix" | Small script change; edits 0009 d10 and the B-08 row | [A/O] (record text) |
| R2 | **Machine-checked canaries.** (a) Replace the prose `Canary:` line with one item per line, `exe::"suite/case"` or `path.py::case`. (b) Move `Get-CanaryItems` and the runner into `tools/lib/canary.ps1`; add `tools/canary.ps1 NNN` that implementers run, the same code `land` runs. (c) Lint the line in `check fast`. (d) Report a per-hunk matrix: each hunk applied alone, which named cases go red, which new or changed cases stay green. (e) Copy the patch to `out/land/NNN/canary.patch` and tell the reviewer to read it. (f) Skip the baseline run when the same sha just passed. | Removes 12+ landing refusals and most of about 55 canary review findings; shortens windowed-Blender prepares; gives the reviewer what it lacks | Medium: one task for the tool, a mechanical edit of open task files | [A] |
| R3 | **Harden `land` and fold `land-pr.sh` into it** as `land.ps1 submit NNN PASS\|SKIP [-Evidence text]`. Prepare undoes its own "(land)" commit on any failure. Regenerate `out/scenes` before the canary baseline. Refuse to start if another `land.ps1` runs or any process has the worktree's DLL loaded. Merge waits for `ci` with a deadline and does the close-and-reopen itself when no run appears. After a successful squash, report PASS and do cleanup as a retried best-effort step (`git worktree unlock`, `remove -f -f`, exact branch name, never a prefix match). Print elapsed time per phase into the PR body. Update the land skill. | Removes F5, F8, F9, most of F7's lost-event cases; about 30 manual interventions per milestone; gives per-phase timing for the next retrospective | Medium: one tooling task with fixture tests | [A] |
| R4 | **Rewrite `.claude/agents/implementer.md`** to carry what every brief repeated: worktree and branch check, early first commit, environment (tools already read it), canary recipe from R2, "stop and ask" rules, B-08 note (until R1), TEST_SUITE tier rule, no direct test executables (use R10's `tools/test.ps1`), task-named scratch folder, no dumps or stray files in commits, 2-core hosted CI constraint for threaded tests, `support.main()` for Blender tests, report shape. Briefs shrink to: task id, contracts on main, task-specific warnings. Raise `maxTurns` to about 250 for tasks tagged measurement or gallery. | Halves brief length (about half of each 1.2-2.3k-character brief was boilerplate), removes the classes of mistake the boilerplate existed for, removes two turn-limit resumes | Small: the file is capped at 60 lines by lint, so either raise the cap or move the canary recipe into a rule file | [A]; cap change [O] |
| R5 | **Scope pre-flight when a task is written.** The planning agent (or a Sonnet agent per batch) greps for every symbol the task's Do list names and lists: registration sites, the budget and counter rows it will change, CMake or glob lists, test fixtures that will break, records it must edit. Add to CLAUDE.md a short standing-scope clause: a task may always touch its own budget rows, registration lists, and existing tests its change forces, and must name them in the report. Add a "plan first" convention: the implementer's first reply for an M or L task is its file list and open questions. | Removes most of about 40 stop-before-code round trips; fewer after-the-fact Scope edits | Small per task; a CLAUDE.md rule change | [A/O] (rule 1 wording) |
| R6 | **Close the three coverage holes.** (a) Gate: headless F12 on CPU and CUDA of a textured, lamp-lit scene with a mean-luminance floor and a CPU-to-CUDA mean comparison. (b) `check perf`: bench B-1M and B-5M, compare pass time, idle share and partition share with the last history line under a tolerance, append the line; `land` runs it when the diff touches `src/kernel`, `src/device`, session or scheduler, with the machine otherwise idle. (c) `land` runs the Blender Python suites when the diff touches `blender/` or `tests/blender/`, and the lead runs `check full` every 10 landings and before a milestone close. Also confirm whether the CPU `--tier=full` suite runs anywhere, and either enforce or delete the budget rows nothing checks. | Would have caught all three escaped defects of section 6.1 within one landing instead of hours to days | Medium: about three tasks; adds about 20-40 s to affected landings, so pair with R7 | [A]; tolerance values [O] |

### Tier 2: during M1b

| # | Recommendation | Expected gain | Cost | Who |
|---|---|---|---|---|
| R7 | **Lock and gate throughput.** Write an owner file on lock entry (pid, tool, start) and print "waiting for X since T" every 30 s; count Blender lock waits in `waited`; add a timeout that fails loudly. Run the gate's `cpu`-tagged steps before taking the machine lock. Skip `fast` inside `gate` when prepare just ran it on the same sha. Give every task a gate-time allowance line (default +0 s; new scenes state theirs) and have `land` print the delta. Consider landing two or three green branches in one gate run when their file sets are disjoint. | Shorter and predictable landings; no silent 10-25 minute waits; stops B-09 creep before it fails a landing | Medium | [A] |
| R8 | **A protocol for measurement tasks.** Template additions: the metric's exact definition and producing command; a quiet-machine clause (hold the machine lock for the whole capture; no other GPU process); a decision table written before measuring (pass: nothing; miss by under X: remedy 1; over X: stop and ask); a cap of two remedy tasks per budget row per milestone, after which the row goes to the owner with numbers; analysis scripts committed under `tools/`; the task appends its own `bench/history` line; any limit it proposes to change goes in a "Limits changed" section of the PR body. Reserve 20-25 % of a milestone's task count for follow-ups. Never accept an acceptance method that has not been tried once (the screen-capture case). | Bounds chains like 093 to 134; makes numbers comparable; removes three script rewrites and the back-filled history | Small: template and README text; lint the template fields | [A]; the cap and the reserve [O] |
| R9 | **Model routing and review rules.** Keep Sonnet as the default implementer. Use Opus for tasks tagged concurrency, lifetime, teardown, sampling or estimator, and for record edits; these are also the classes where review found the most defects. Keep Opus review on those plus ABI and process; raise its effort to high for kernel and concurrency diffs as 0009 d27 already says. Change the reviewer definition: read `canary.patch`; may read main sources and records; three verdicts, PASS, PASS WITH FIXES, FAIL, where FAIL requires a code, measurement or cannot-fail-test finding; record-text and wording findings are never must-fix; a PASS must list what was checked. Re-review only fixes that change control flow in concurrency, lifetime or teardown code. Add a standing checklist for the recurring classes in section 6.3. | Keeps the 2 %-of-tokens step that found defects in 40-48 % of diffs; removes label noise and wording-only rounds; shifts Opus implementer volume (32 % of tokens for 31 runs) to where it pays | Small: agent file and LEAD.md | [A]; the model split is O31 [O] if changed materially |
| R10 | **Lead tooling to cut attention per task** (the lead is 28 % of token volume). `tools/lead/next.ps1`: tasks whose dependencies have landed. `tools/lead/task.ps1`: add a Scope entry, a Progress or ruling line, create a task from the template (lint-clean, committed before dispatch). `tools/lead/sweep.ps1`: list and remove merged agent branches, locked or unregistered worktrees, leftover Blender and land processes. `tools/test.ps1 -Exe -Case`: run one test case under the lock, so the permission classifier stops blocking acceptance runs. A commit hook or wrapper so the lead's commits to main run lint. Send long landing logs to a `digest` agent instead of reading them. Add a project permission allow-list for `pwsh tools/*`, read-only git and `gh pr view/list/checks`, and deny rules for direct `cmake`, `ninja`, `nvcc`, `blender`. | Removes about 60 heredoc edits, 14 cleanups and most classifier refusals per milestone; fewer lint-red pushes | Medium: several small scripts; settings change | [A]; settings allow-list [O] (permissions are the owner's) |
| R11 | **Session hygiene.** Hand the lead over every 25-30 landings or at the first compaction, from a generated handover (R12's backlog plus `status.ps1`), instead of five compactions in two sessions. Before a usage limit: implementers commit work in progress at every acceptance step (already rule 2; four agents had not). Replace the hourly heartbeat with a notification on agent or landing completion. | Less context re-reading at Opus price; no lost work at a limit | Small | [A] |
| R12 | **Documentation slimming.** (a) Fold rulings 64-84 into the records they override, and from now on a ruling is an edit to the record in the same commit, with RULINGS.md keeping a one-line index. (b) Move each record's review and reconciliation tail, SPINE's superseded parts, PLANNING_PLAN and CHECKPOINT-1 to `docs/history/`. (c) Make ROADMAP's milestone list a live table: status, estimated and actual task count, gate outcomes, link to the closing report; edit it at each close. (d) Replace STATUS's prose backlog with `docs/BACKLOG.md`, one row per item with id, area, milestone and owner task; STATUS keeps milestone, red gates and open owner questions. (e) Turn LEAD.md's handover lore into tooling fixes (R3) and delete the lore. (f) Fix the stale statements: the two-implementer limit in LEAD.md and tasks/README, 0009 d28's field list, the land skill, ARCHITECTURE's rule numbers. (g) Lint that every `Rnn`, `Onn`, `B-nn` and `000N dNN` reference resolves. | One place states current truth; about 20-25k fewer tokens per record read; no more hand condensing of STATUS | Medium: two or three documentation tasks, best done by one Opus agent with the planning agent reviewing | (b)-(g) [A]; (a) and deleting superseded records [O] (0009 q4 is open) |
| R13 | **Re-estimate.** Multiply the remaining milestones' task estimates by 4.4 for decomposition and add 22 % for mid-milestone tasks. Plan M1b in slices of 25-30 tasks with a gate review between slices rather than one 100-task milestone. Drop the Size stop rule or redefine Size as source lines excluding tests. Make acceptance numbers "reachable or amend": the task author states how each number was checked. | Honest schedule; fewer rulings on unreachable acceptance | Small | [A]; milestone re-slicing [O] |

### Tier 3: worth doing, lower gain

| # | Recommendation | Expected gain | Cost | Who |
|---|---|---|---|---|
| R14 | **Local reference sources.** A `tools/fetch.ps1 refs` step that puts pinned pbrt-v4, the Cycles kernel and the OpenPBR specification in a read-only cache outside the tree; tasks cite file and line. | Ends citations from memory (8+ cases, one false claim in a record) | Small; licence check for reading only | [A/O] |
| R15 | **Protect owner gates and limits.** Add to LEAD.md "Never": starting anything the owner has made conditional on a go-ahead. Add a "Limits changed" ledger to the milestone report, filled from PR bodies. A bound may not be changed in the same landing whose test it failed without a ruling that names the old and new value. | Closes Q4-U1 and Q4-U2 | Small | [A]; the owner may want stronger wording [O] |
| R16 | **Lints for recurring classes:** `unittest.main()` in `tests/blender`; integer literals in `blender/astroray` that duplicate generated ABI constants; "task NNN" in comments; `catch (...)` without a report or rethrow; `Ref:` paths that do not exist; module-level mutable state in the addon; tests under `tests/` changed without a canary item (already in land, move it to `fast`). | Removes about 30 nit-level review findings per milestone and three repeated mistakes | Small each | [A] |
| R17 | **Lanes instead of a global implementer limit.** State in LEAD.md: any number of CPU-only implementers that the lead can rule for (5-8 worked), one GPU or Blender lane. Tag each task `lane: cpu\|gpu\|blender`. Make the shared list files one-entry-per-line so parallel edits merge. | Recovers session-1 parallelism where it is safe | Small | [A]; replaces 0009 d27 text [O] |
| R18 | **One PreToolUse hook as a scope and tool guard** (deny edits outside the worktree, deny direct `cmake`, `ninja`, `nvcc`, `blender`, deny `git stash`). 0009 d31 sets zero hooks and lint forbids the key, so this is a change of an owner-level rule. | Mechanical enforcement of rules now held by prose (Q4-U4) | Small to build; a process-record change | [O] |
| R19 | **Offload to a cheap open-weight model (O32).** It was authorised and never used in M1a. Candidates with low risk: condensing landing logs, drafting bench-history lines from PR bodies, STATUS and backlog table upkeep, mechanical renames. Route through one wrapper, never with owner details. This retrospective did not test it, so the gain is unmeasured; Haiku digests already cover most of the same ground cheaply. | Small token saving on the lead | Small | [A] to trial |
| R20 | **Report timing from data, not transcripts.** Have `land` append one line per landing to `out/land/history.jsonl` (task, phase seconds, lock wait, retries, review rounds, gate seconds) and commit it at milestone close. | The next retrospective takes minutes and its numbers are exact | Small (part of R3) | [A] |

### Suggested order of application

1. R1, R2, R3 as the first three M1b tooling tasks (they unblock everything else), with R20 inside R3.
2. R4, R5, R9, R15 as file edits before the first dispatch.
3. R6 and R7 next, before any performance work resumes on B-19 or B-20.
4. R8 and R13 while writing the M1b task list; R12 in parallel by a documentation agent.
5. R10, R11, R14, R16, R17 as they come up; R18 and R19 only with the owner's answer.

Owner decisions collected: B-19, B-20 and B-31 at the close; whether spike C stays unrun; R1's
record change; R4's cap; R5's rule wording; R6's tolerances; R8's cap and reserve; R9 if the model
split changes; R10's permission allow-list; R12 (a) and record deletion; R13's re-slicing; R18;
and the go-ahead for the planning session itself.

## Appendix: evidence sources and limits

- Repository at commit 13edb94: CLAUDE.md, docs/STATUS.md, docs/LEAD.md, tasks/README.md read
  directly; docs/ROADMAP.md, docs/decisions/*, docs/ARCHITECTURE.md, docs/spikes/*/RESULTS.md,
  tools/*, .claude/agents/*, .claude/settings.json, .claude/skills/land digested by two Sonnet
  agents with file and line citations.
- Git: 261 commits (154 task landings, 22 ruling commits), `git worktree list`, `git branch`,
  bench/history/2026.jsonl (29 lines).
- GitHub: 107 PRs with bodies, comments and check results (`gh pr list`, `gh pr view`), digested
  by one Haiku agent.
- Transcripts of the Astroray 2.0 project folder only: lead sessions 1 and 2 in full, session 3 up
  to 11-22 13:14 UTC, and 230 sub-agent transcripts (132 implementer, 95 reviewer, 3 other).
  Token, timing and tool-call statistics were computed by script from the raw records. Condensed
  timelines, briefs, follow-ups and reports were digested by ten Haiku and two Sonnet agents.
- Not used: the Astroray 1.0 repository and its transcripts; the opencode route (not needed).

Limits. The planning day's transcripts are not in the project folder. Session 3 was still running;
tasks 134, 135 and 100 are described as of their last visible report. The condensed dumps clip
long reports, so finding counts are lower bounds. Per-phase landing times are estimates because
`land` does not record them (R20). Token totals exclude session 3 and are volumes, not costs.
Digest-derived counts were spot-checked against scripts where a script could answer (cycle times,
PR counts, owner messages, commit classes) and otherwise taken as reported.
