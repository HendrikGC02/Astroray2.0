# M1b retrospective: numbers from the landing history

Reader: the lead and M2's planning session. Every number comes from `tools/land_history.py`, a `git`
command in the appendix, or the gate ledger in `tasks/README.md` ("ledger"). The history file holds merged
landings only: refusals, causes, review rounds and machine load come from the ledger. Transcripts and
tokens are not in the repository and not measured. Section 6 holds facts only the lead's brief gives.

## 1. Outcome against ROADMAP's M1b exit criteria

| Criterion | Met by | State |
|---|---|---|
| 0001 V5 | none: `check.ps1` runs its dumpbin part only; packaging the OIDN DLLs has no task (BACKLOG row from 198, milestone M2) | open |
| 0003 check 26; blackbody-light half of check 15 | 193; 177 (a4_blackbody_lamp) | met |
| Analytic A9 | 173 | met |
| B-19 on the alpha-card scene | 170: pq 11.0 % against 10 % | red, accepted (owner decision 44) |
| 0004 D3 alpha cards | 156 (d3_alpha_cards, d3_alpha_nee) | met |
| 0005 V6, V7, V10 for every op; V8 order angles | 189 (node values); kernel rows at every landing, ratified by 213; 165/166 (V10 cases), 151 (matrix); 188 under ruling 113 | met |
| 0006 V1, V6, stack fixture | 181 and 183 (slab, tracking cases); 193 (cap 1 against 4,096); 180 | met |
| 0008 V9, V10, V11 (final) | V9 negative-compile step (M1a); 174 guide cases; 201 on three scenes under ruling 110 | met; b_tx loses under the final denoise (236, owner question 5) |
| M-1 ratified (traversal, guides, GRATING) | 213 at M1b scope without 209 (owner decision 45) | met; the full-scope proxy is over 0002 D10 |
| B-14 and V7a re-gated | 214: b1m 67.0/s median against 30/s; V7a at most 1.05x Cycles against 1.5x | met; b_foliage 23.2/s recorded, not gated |
| Look: grating orders, OIDN, metal edge | 216 gallery, read by the lead; the owner's look is not recorded | owner look open |

Slice D rows: M-1 (213) as above; B-05 (215) `shade_surface` 7.33 s quiet of 8.0 s, met; B-19 (215) b1m
idle 11.9 %, red as accepted (owner decision 37); B-20 (215) 12.36 / 12.94 ms against 9.5 / 12.5, not met;
gallery (216) 25 images, CPU/CUDA mean 1.0000; `cuda none` (223) met; PLAN-M2 written (5839e8a6).

## 2. Counts against the plan

96 tasks of ids 136-237 landed by squash merge; 217 and 236 are open; 207 (merged into 213), 209 (to M8)
and 235 (closed with its finding) were deleted unlanded; 184 was never used: 98 task files against
PLAN-M1b's 110-135 and ROADMAP's estimate of about 22 (96 / 22 = 4.4x). The debts 238-247 run as M2's slice
0. PLAN-M2's "96 landed, 4 open" predates 214's and 216's merges; by merges it is 96 and 2 today.

| Slice (plan) | Landed | Ids (this page's assignment, from the README group labels) |
|---|---|---|
| 0 Tooling and debts (18) | 34 | 136-144, 155, 157, 159-162, 185, 203, 204, 210-212, 221-224, 226-229, 231-234, 237 |
| A Materials (30) | 24 | 145-151, 156, 158, 163-170, 186-189, 202, 206, 220 |
| B World, lights, guides, denoise (30) | 22 | 153, 154, 171-177, 190, 191, 195-201, 208, 218, 225, 230; 236 open |
| C Media core (20) | 12 | 152, 178-183, 192-194, 205, 219 |
| D Close (15) | 4 | 213-216; 217 open |

Product slices A-D landed 62 against 95 planned; tooling and debts 34 against 18 (35 % of all landings).
Task files added after their batch's commit: 159-162, 185, 202-204 and 224-237 (225 never had a file on
main): 22 of 101 written, 22 %, the same share as M1a's 22 % and above the fifth reserved. Gaps left:
of BACKLOG's 130 rows, 101 carry milestone M1b (settle by then at the latest), 83 of them from M1b tasks.

Remedy tasks per budget row (limit two, then the owner): B-09 two, 157 and 212 (ruling 99), plus clock
changes by rulings 94 and 100; B-08 three tasks aimed at its landing refusals, 203, 210 and 227 (the
ledger calls 227 "remedy one"); B-05 one remedy (228) and three raises for `shade_surface` (owner decisions
40, 41, 43: 7.0, 7.5, 8.0 s) plus a clock change (237, owner decision 44); `k_shade_background` spills two
remedies inside 208 (ruling 114). Rows `k_shade_surface` (rulings 93, 103), `k_route_hit` (ruling 95) and
`k_shade_shadow_medium` (ruling 116) moved by lead ruling inside 0002 D10, not by remedy tasks.

## 3. Landing tables (history file, 77 lines, 2026-11-23 to 2026-11-28)

19 landings have no line: 136-138, 142-144, 146, 149-155, 157, 159, 162 predate 138; 164 and 233 were
finished by `land.ps1 merge`, which writes none.

| Phase | n | Median s | Max s (task) | Sum h |
|---|---|---|---|---|
| canary | 77 | 73.7 | 1496.6 (189) | 2.96 |
| blender suites | 77 | 0.0 | 842.3 (198) | 1.08 |
| gate | 77 | 165.0 | 375.3 (226) | 3.78 |
| hosted ci wait | 77 | 346.4 | 793.6 (226) | 6.86 |
| lock wait | 77 | 4.9 | 6269.5 (186) | 4.86 |
| perf / perf shares | 39 / 9 | 6.3 / 23.4 | 375.7 (177) / 180.7 (213) | 0.34 / 0.10 |
| landing total | 77 | 730.5 | 6737.6 | 20.3 |

Rebase, scenes and merge sum to 0.29 h. `gate_s` is land's gate phase, with the build and compile-timing
steps B-09 excludes; daily medians 146.2, 167.0, 173.2, 179.8, 157.3, 149.4 s from 11-23 to 11-28 (all in
ISO week 2026-W48).

Either side of a change: 233's order (232 was the first landing under it) took the ci wait median from
451.5 s (52 lines) to 79.8 s (24), lock wait 16.4 -> 0.2 s, landing total 843.8 -> 407.5 s; after 234 the
gate phase median is 149.4 s, max 211.5 s (213, 32 s of compile timing), over 22 lines, against 168.9 s
over 54 before; the ledger's B-08 refusals (199, 163, 164, 189) all predate 210 and 227.

First try: the history reads 75 of 77 (189 and 226 needed a fresh prepare after a refused merge). The
ledger names 29 refused submits over 21 tasks (151, 153, 156, 158, 163, 164 six tries, 166, 168, 170, 172,
175, 181, 183, 186, 189, 193, 194, 196, 199, 208, 233), so 60 of the 77 history tasks landed on the first
submit. Causes, from the ledger text (a task can have more than one): hosted ci only (164, 168, 183, 186,
189, 193, 208); B-08 under load or a cold lint (163, 164, 189, 199); other timing under load, B-05, B-09,
`check perf` or a B-19 share (172, 175, 181, 194, 196); main moved or a rebase broke tests (156, 175, 183);
GitHub or merge-status errors (153, 164, 170, 233); races and flakes in tools and tests (151, 158, 166).

Gate seconds, declared against measured, for the 12 tasks whose ledger and README rows carry both: 24.2 s
declared, 16.5 s measured (0.68): render cases mostly under (139 8 -> 1, 156 6 -> 2.8, 169 3 -> 0.6; 177
1 -> 3.6), tool and ABI fixtures over (204 0.3 -> 2.7, 181 0.5 -> 0.9, 203 0.5 -> 0.7).

Lanes of the 77 history tasks (README rows; 13 lead-written or shared rows have none): kernel 24 (13 Opus),
cpu 13 (3), blender 11, tools 8 (1), gpu 7 (2), docs 1 (1). Review outcomes are in the ledger only from the
fifth lead on: of 17 landings with one noted, Opus FAIL at least once in 6 (194, 170, 201, 215, 214, 208),
PASS WITH FIXES in 5 more (237, 220, 230, 206, 188), three rounds in 2 (231, 197), a clean PASS in 1
(213), review SKIP in 3 (198, 218, 216).

## 4. PLAN-M1b section 1, row by row

| Lesson row | Measured effect in M1b |
|---|---|
| 25 estimated became 135 | 98 task files against 110-135 planned (section 2): the 4.4x multiplier held; the plan's lower bound was not reached because slices A-D landed 62 of 95 |
| 22 % added mid-milestone; remedy cap | 22 % again; the cap held for B-09, was exceeded for B-08 (three tasks), and was bypassed for B-05 by three owner raises |
| Landing mechanics the largest cost | still the largest: 20.3 h of landing phases over 77 lines, ci wait 6.86 h and lock wait 4.86 h of it; 29 refused submits; after 232 the landing total median halved |
| Scope lines wrong in 11 of 13 | not measurable from the history or the ledger; PLAN-M2 names two misses (suites only hosted ci runs, stale Budget lines) |
| Three defect classes had no gate | 139, 140 (with 160) and 138 closed them; hosted ci alone refused submits of seven tasks, the class 238 and 245 now target |
| Session 2 slowed by one lock | lanes, then owner decision 42 (two implementers); lock wait median 16.4 -> 0.2 s after 232 |
| Opus review found real defects | 13 of 14 reviewed landings with a noted outcome had a finding (section 3) |
| B-08 failed 60+ times | build outside the window (owner decision 36); four landing refusals remained, none after 210 and 227 |

## 5. What the M1a recommendations did

Applied: R1 (owner decision 36; section 4's last row); R2 as 137, without the one-item format (its hunk
matrix stops at 8 hunks, LEAD lesson 13); R3 as 138 and 233 (29 refused submits remain); R4, R5, R9
(agent definitions, scope pre-flight, process/guards); R6 as 138-140, 160; R7 as 141, 231, 232; R8 as
PLAN-M1b section 5; R12 as 155, 185, BACKLOG; R13; R14 as 143; R15; R16 as 142; R20 as 138 (77 lines, but
no refusals, causes or review rounds). Changed: R11's 25-30 landings per lead became 15-20 (LEAD.md; eight
leads in the ledger); R17 became owner decision 42. Not applied: R10's `next`, `task`, `sweep` (`tools/lead`
holds two scripts); R18 beyond deny rules; R19 unmeasured.

## 6. Reported by the lead, not in any repository file

From the eighth lead's brief: 215 and 214 each passed review only after an Opus FAIL (also in the ledger);
239 failed review on an emission-estimate defect; a full disk was cleared by removing stale agent
worktrees (about 45 GB, the lead's figure); the permission classifier refused a budget-row edit, resolved
by owner-approved ruling 116; the lead refused 214's first submit after its own Look at. Q1 is why.

## 7. Recommendations for M2

| # | Recommendation | Evidence | PLAN-M2 |
|---|---|---|---|
| Q1 | `land` appends a line for every submit outcome (REFUSE or FAIL with the step that refused), for `land.ps1 merge` finishes, and with review rounds and dispatch time | 29 refused submits and all review outcomes exist only as ledger prose; 164 and 233 have no line; history first-try 75/77 against ledger 60/77 | not applied |
| Q2 | Re-date BACKLOG at the M1b close: each M1b row gets an M2 slice, a later milestone or deletion | 101 of 130 rows carry milestone M1b | partly: slices C and D take rows from 174, 179, 196, 197, 219 |
| Q3 | Size each slice's tooling and debt share from M1b's, not a fifth | tooling and debts 34 landings against 18 planned, 35 % of 96 | partly: slice 0 is 21 of 118 (18 %) |
| Q4 | Count a raised limit as a remedy against the two-per-row cap | B-05 raised three times, its clock moved once; B-08 took three tasks | applied: "raising rows is spent as a method", no B-05 raise without the owner |
| Q5 | Declare tool and ABI fixtures at a measured fixture's cost, not a nominal one | fixtures 0.3 -> 2.7, 0.5 -> 0.9, 0.5 -> 0.7 s against render cases 8 -> 1, 6 -> 2.8 s | applied: its projection reads fixtures at about 3x |
| Q6 | Bring hosted-only suites to the landing before more breadth lands | hosted ci alone refused submits of seven tasks (section 3) | applied: 238 (ci logs), 245 (headless suite in landings) |
| Q7 | Keep 233's order and decision 42's lanes; watch ci wait per landing in the history | ci wait median 451.5 -> 79.8 s, landing total median 843.8 -> 407.5 s | applied (PLAN-M2 section 5) |
| Q8 | Keep Opus review on kernel, physics and process diffs | 13 of 14 noted reviews had a finding; 6 FAILs | applied (section 5) |
| Q9 | Give 0001 V5 an owner task in M2 slice 0 | the only M1b exit criterion with no task | not applied |
| Q10 | Record the owner's look at each slice close in a file the next retrospective can read | M1b's look criterion has no record | not applied |

## Appendix: commands and commit

Main at c435b5a8; the main checkout's `out/land/history.jsonl` (77 lines, last task 214) read in place.
- `<python> -I tools/land_history.py <history> --ledger tasks/README.md --ledger <first-batch README>`, the
  second being `git show 1a85b5cb:tasks/README.md` (lanes, declared seconds); again with `--before 232`,
  `--after 232`, `--after 234`. Landed ids and merge times: `git log main --format="%ad %s"`, subjects
  `NNN ... (#PR)` (96). Deletions: `git log main --diff-filter=D --name-only -- "tasks/*.md"` (98).
  Batch commits: `git log main --diff-filter=A --name-only --format="@@ %h %ad" -- "tasks/[0-9]*.md"`.
  BACKLOG: `git grep -c -E "\| M1b \|[^|]*\|$" HEAD -- docs/BACKLOG.md` (101; 83 with an id 136-237 first).
