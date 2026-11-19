# 125 Light-structure build over B-23 (many lamps, emissive triangles)
Goal: 094 measured the light-structure build (commit to film reset on a settled session, an upper bound) at about 89 ms for 10,000 lamps and 8.2 s for b1m with its material made emissive (1 M emissive triangles), against B-23's 50 ms per 10k lights (5 s per 1 M); with 1 M emissive triangles later passes take 86-196 ms instead of 17 ms -> time the build inside the engine, parallelise it on the scene workers (0004 D25), and find why passes slow down with many emissive triangles.
Read first: 0004 D25 (light structure, alias table, build), D6-D8 (light selection, emissive triangles), 0009 B-23; 094's PR body (B-23 table); the light builder under src/host/scene/ and selection under src/kernel/lights/.
Scope: src/host/scene/ (light builder and a log line with its time), src/kernel/lights/ (only if the pass slowdown is in selection), tests/cpu/ (builder equivalence: parallel build bit-identical to serial), tools/budgets.toml (B-23 only if measured with headroom).
Out of scope: a light BVH or a many-light sampling change (a separate decision if the slowdown is inherent to the alias table).
Reference: the alias method as 0004 D25 cites it; Cycles scene/light.cpp (Apache-2.0) for how Cycles builds the emissive-triangle distribution.
Do:
1. Add an engine timer for the light build (log line, ms) so B-23 is measured directly, not as commit to reset.
2. Attribute the 8.2 s (per-triangle power, alias construction, upload, or something unexpected such as per-triangle allocations); fix what is needlessly serial and parallelise the rest on the scene workers; the result is bit-identical to the serial build (test).
3. Attribute the pass slowdown with many emissive triangles (nsys: which kernel grows); fix it if it is a defect (for example a linear search), else report it with numbers for a lead decision.
4. Re-measure 094's variants (1 lamp, 10k lamps, b1m emissive): build ms, first pass ms, steady pass ms; PR body Result: with commands.
Acceptance: `pwsh tools/check.ps1 gate` passes; B-23 met on both variants or the remainder reported with numbers.
Look at: none.
Canary: tasks/125.canary.patch must turn red: the builder equivalence test (name it in land format).
Budget: no gate time added.
Depends on: 094.
Size: M (about 250 lines); stop and report at 2x.
Progress:
