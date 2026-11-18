# 078 Detection-power measurement of the statistical gate
Goal: it is unknown whether the gate's sample counts catch real bugs -> a measurement injects known lobe and integrator bugs one at a time and records which of the energy, chi-squared and analytic tests catch each at gate counts, then resets the PROVISIONAL family sizes and gate sample counts from the result.
Read first: 0009 d15 (tests, family sizes), Verification rows 14-15 (detection-power task: inject missing cos, wrong Jacobian, 1% albedo error, dropped transmission, flipped front face), Risks (power numbers are derived, not measured); tools/budgets.toml `[family]`.
Scope: tools/detection_power.ps1 (one-off; deleted when this task lands, per 0009 d31), tools/budgets.toml (family sizes, if reset), tests/cpu/ gate sample counts (if raised), tasks/078 result in the PR body.
Out of scope: dropped transmission (no transmission lobe exists in M1a; the row is run when M2 adds the dielectric lobe, and the PR body says so); oracle scenes (M2).
Reference: 0009 d15 and research 09 section 3 (derived power estimates).
Do:
1. For each injected bug, a patch applied in a scratch worktree: (a) missing cosine in the shade stage's throughput; (b) wrong Jacobian in GGX VNDF sampling (drop the 1/(4 wo.h) factor); (c) 1% albedo error in EON (rho * 1.01); (d) flipped front face in the shading context; plus (e) the sampler hero lane fixed at lane 0 against check 14.
2. Run `check gate` per patch and record which tests fail (energy, chi-squared, analytic scene, other) with their z or p values.
3. Measurement result (PR body section `Result:`): the exact command per row, the table of bug x test outcomes, and the gate wall time.
4. On each outcome: every bug caught by at least one test -> mark the family sizes and gate counts as measured (remove `provisional` for the gate family) and keep them; a bug not caught -> raise that test's sample count or m (a reviewed budget change in budgets.toml) and re-run that row until caught, keeping B-09; if B-09 cannot hold, report to the lead with the numbers instead of raising further.
5. Adding a test beyond m fails (0009 Verification row 14-15 second part): demonstrate with a temporary extra test in the scratch worktree and quote the failure line.
Acceptance: the PR body holds the result table with the commands; tools/budgets.toml changes only as the outcomes dictate; `pwsh tools/check.ps1 gate` passes on the final diff; tools/detection_power.ps1 is deleted in the landing commit.
Look at: none.
Canary: none if no test file changes; if outcome 4 raises a test's sample count, tasks/078.canary.patch must turn red: astroray_tests_cpu with the changed test names (the patch re-applies the undetected bug that motivated the raise).
Budget: may raise gate statistical counts within B-09; report the gate time before and after.
Depends on: 064, 073, 020.
Size: S (scripts plus a result table); stop and report at 2x.
Progress:
- started
