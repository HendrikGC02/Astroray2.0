# Astroray 2.0

A scientifically accurate spectral path tracer, usable from Blender, built for
astrophysical rendering on top of standard techniques (OpenPBR materials,
physically based light transport). Correctness is judged against physics,
analytic tests and the OpenPBR specification, not against another renderer.

**Phase: planning.** No engine code until `docs/ARCHITECTURE.md` and
`docs/ROADMAP.md` are signed off by the owner. `spikes/` holds throwaway
measurement code only.

## Where things are

- `docs/PLANNING_PLAN.md` — how the planning phase runs.
- `docs/research/` — one file per research question, with sources.
- `docs/decisions/` — architecture decision records. A decision changes only by
  a new record that supersedes the old one.
- `docs/v1-lessons/` — everything carried over from Astroray 1.0. This is the
  only channel. Do not open the v1 repository; if a question needs it, ask the
  lead for a cited lookup.

## Rules

1. **State assumptions; ask when unsure.** Surface real tradeoffs. Push back on
   a request when a simpler approach exists.
2. **Minimum code that solves the problem.** No speculative features, options
   or abstractions.
3. **Surgical changes.** Every changed line traces to the task. Mention
   unrelated problems; do not fix them in passing.
4. **Evidence, not claims.** "Done" means you built it, ran the check, and can
   show the output. A measurement names the command that produced it. Look at
   rendered images as well as numbers.
5. **Cite before implementing.** Any non-trivial physics, sampling or numerical
   method comes from a published source with a licence-compatible reference;
   name it in the code.
6. **Write once.** A formula, a material or a sampler exists in one place and
   is compiled for every target. Never hand-port.
7. **Fail loudly.** Unsupported input raises a visible error or warning. No
   silent fallbacks, no features that land switched off.
8. **No process-global mutable state** in the engine.
9. **Budgets are requirements** (provisional until measured): incremental build
   under 30 s, clean build under 5 min, pre-commit tests under 10 s, merge-gate
   tests under 3 min. A change that breaks a budget is not finished.
10. **Keep documents small.** One page of status. Comments explain why, never
    ticket history.

## Environment

- Windows 11, PowerShell. RTX 5070 Ti, 8 cores / 16 threads.
- Do not run two GPU builds or two GPU test runs at once.
