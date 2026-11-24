# 185 Fold build-phase rulings 64-90 into their records; superseding records delete what they supersede
Goal: rulings 64-90 state current truth in RULINGS.md while the records they amend still say the old thing (readers must hold both), and 0009 d29 keeps superseded files pending owner question 4 -> each ruling's binding text is written into the record section it amends, RULINGS.md keeps one index line per folded ruling naming where the text now lives, and 0009 d29 and its owner-question line say that a superseding ADR deletes the superseded file in the same commit (owner decision 40). [lane docs; Opus]
Read first: docs/decisions/0000-owner-decisions.md decision 40; RULINGS.md (G2 one owner per topic, 49, 50, 63, 86, 87 and 64-91); docs/retro/M1a-retrospective.md R12 (a) and (g); 0009 d29 and its "Owner questions" line; each record section a ruling names (the ruling's own parentheses give it, e.g. "0007 d18", "0002 D5, D6, D7").
Scope: docs/decisions/RULINGS.md (64-90 become index lines); docs/decisions/0001-0009 (only the sections the folded rulings name); docs/decisions/0009-build-test-process.md d29 and the owner-question line; docs/ARCHITECTURE.md only where it cites a folded ruling's text instead of its number; tools/lint.py only if an existing reference lint breaks (name it).
Out of scope: rulings 1-63 and G1-G4 (already applied to the records), ruling 91 and later (live for the second batch), owner decisions in 0000 (never folded), changing any number, gate or rule (a fold is a move, never a new decision), deleting any record file (none is wholly superseded today; the rule applies from now on), SPINE.md, reference lints of R12 (g).
Reference: the rulings' own text. Where a ruling amends another (85 amends 84, 86 amends 49, 87 widens 63) the record gets the amended result once.
Do:
1. Per ruling 64-90: find the owning record section (G2); write the ruling's binding sentences there in the record's voice, replacing the text it overrides (0002 D7 (b), 0007 d35's "superseded" parenthesis and the like become the current statement; measured history that a record already carries stays). Ruling 72 carries owner decision 40's amendment: write the amended limit, not 5.0 s.
2. RULINGS.md: each folded ruling becomes `NN. **Title.** Folded into 000N dNN (owner decision 40).`, numbering unchanged so every `RULINGS NN` reference still resolves.
3. 0009 d29: replace "(pending owner question 4; the current CLAUDE.md keeps superseded files)" with the decided rule; drop (4) from the owner-question line.
4. `grep -rn "RULINGS\? \?[6-9][0-9]\|ruling [6-9][0-9]" docs src tools tests tasks` before and after; every hit still resolves (number kept); a hit that quoted the old record wording is updated.
Acceptance: `pwsh tools/check.ps1 fast` passes (lint `taskfile`, records and references); paste the last lines; the PR body lists, per ruling, the record section it went to and the sentence it replaced; `git diff --stat` shows only Scope files.
Look at: none.
Canary: none (no test or code changes; documentation only, ruling 86's precedent).
Budget: gate +0 s; no kernel row.
Depends on: owner decision 40 on main; land after 145 (its 0005 items 9-10 write-back) and before the next record write-back that touches the same sections; the later one rebases.
Size: M (records text, about 300 lines moved); stop and report at 2x.
Progress:
- Rulings 64-90 written into 0001 D2.5/D2.8/D2.10/D2.11, 0002 D5-D7/D10, 0003 d23, 0004 D10 and Verification D1/D7-D11/D8, 0007 d5/d9/d12/d14/d15/d18/d21/d25/d35/d50/V12/V19, 0008 d10/V12/risk 4, 0009 d13 (B-05 at 7.0 s, B-20, B-21, B-34)/d15/d22/d29/Verification 14-15.
- RULINGS.md 64-90 are index lines, numbers kept; 0009 d29 states decision 40's rule, q4 dropped; ARCHITECTURE's generation line follows 0007 d25.
- Reference grep: 207 hits before, 237 after (records now cite the ruling numbers); every number resolves.
- check fast PASS (needed ASTRORAY_DEPS_CACHE=C:\Astroray2.0\third_party\_cache in the shell: fresh worktree has no deps cache).
