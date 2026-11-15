# 076 Integrator checks: Russian roulette, shadow lifetime, closed box, radiance scale; work-counter baselines
Goal: the M1a integrator is not checked end to end -> renders verify Russian roulette, shadow delivery after slot refill with starved pools, termination and bounce statistics and the radiance scale, and the B-16 work counters and timing tripwire become gate checks.
Read first: 0004 Verification rows D2, D4/D5, D28; 0002 Verification row D5 (shadow pool below demand); 0008 V8; 0003 check 15; 0009 d18 (work counters, tripwire), B-16; src/host/session/test_hooks.h (101).
Scope: scenes/d2_rr_furnace/, scenes/d4_lifetime/, scenes/d28_closed_box/, scenes/d15_radiance_scale/, scenes/lib/truths.py, tools/integrator_checks.py, src/host/session/test_hooks.{h,cpp} (`ASTRORAY_TEST_POOL`, `ASTRORAY_TEST_RADIANCE_SCALE`), src/host/scheduler.cpp and src/host/light/lamps.cpp (read those hooks), tools/check.ps1 (gate steps, B-16), tests/budgets/counters.toml.
Out of scope: transparent-shadow layers in the lifetime test (M2: 0004's row asks for 8 non-opaque layers, which M1a cannot build; this task uses opaque occluders); edit-and-revert, determinism, replay and report checks (107); per-type limits (M2).
Reference: none beyond the cited verification rows.
Do:
1. d2_rr_furnace: camera inside a closed Lambertian sphere of albedo 0.8 with emission Le: L = Le/(1 - rho) with Russian roulette on and off, both within the harness threshold (0004 D2).
2. d4_lifetime: main pool forced to 2^10 and the shadow pool forced small through `ASTRORAY_TEST_POOL`, Russian roulette at every bounce, many occluded and unoccluded NEE items: mean equals the large-pool run within 1e-6 relative and matches its analytic value; CPU threads=1 twice bit-identical (0004 D4/D5 opaque part, 0002 D5).
3. d28_closed_box: closed box, Russian roulette off, max depth D: sum of sample counts = launched camera samples, sum of termination counts = sample count, mean bounces = D exactly (0004 D28; 0008 V8 bounce statistics).
4. d15_radiance_scale: film bit-identical with the automatic s and with s forced to 1 through `ASTRORAY_TEST_RADIANCE_SCALE=1` on an in-range scene (0003 check 15, first part; the 1e9 K / 3 K part needs BLACKBODY lights and lands with them in M1b).
5. B-16: record per-scene counters (rays closest, shadow, steps, launches, bytes) for B-1M-free fixtures (A1, A3, d28) on CPU and GPU into tests/budgets/counters.toml; gate fails above +2% unless the diff edits that file; the timing tripwire (kernel ms per sample at 1 and 64 spp, burn-in, min of 5, fail above 1.25x) runs on the largest fixture until B-1M exists (089 switches it).
6. From 101/069: active test hooks are visible in reports: add an INFO report code (for example AR_REPORT_TEST_HOOK_ACTIVE, one entry per hook, detail = hook name) raised by the session at create in gate builds, listed in the 0001 D2.11 code table; the CLI report table shows them. include/astroray/astroray.h, src/host/session/session.cpp, cli/render.cpp and the 0001 report-code table join Scope for this.
Acceptance: `pwsh tools/check.ps1 gate` passes with every check above listed in its summary; a local edit adding a second shadow ray per vertex makes the counter check fail (0009 Verification row 18).
Look at: none.
Canary: tasks/076.canary.patch must turn red: `tools/integrator_checks.py --check d4_lifetime` (the patch drops late deliveries), `tools/integrator_checks.py --check d28_closed_box` (the patch skips the termination count at MAX_BOUNCE).
Budget: gate time +20 s at most; report the total against B-09.
Depends on: 106.
Size: M (about 300 lines); stop and report at 2x.
Progress:
