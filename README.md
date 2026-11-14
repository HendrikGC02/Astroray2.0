# Astroray 2.0

A spectral, GPU-accelerated path tracer for Blender, aimed at scientifically
accurate and astrophysical rendering. It is designed as a normal extension for
stock Blender 5.3 (not a fork), on Windows with NVIDIA GPUs, with a CPU target
that serves as the correctness reference.

Correctness is judged against physics, analytic results and the OpenPBR
specification, not against another renderer.

## Status

**Pre-alpha. The planning phase is complete; the build phase has not started.**
There is no engine code, no installable extension and no release. What exists
is research, design records, a roadmap and two throwaway measurement spikes.
Nothing described in `docs/` is implemented yet.

## Licence

- The engine core, command-line host, tools and tests: Apache-2.0.
- The Blender extension (`blender/`): GPL-3.0-or-later.

Each source file carries its own SPDX header. The split is stated in
`docs/decisions/0001-product-shape.md` (decision 7).

## How the documentation is organised

- `docs/ARCHITECTURE.md`: start here; a map of the design, citing the records.
- `docs/decisions/`: the architecture decision records and the owner's
  decisions; `RULINGS.md` settles conflicts between them.
- `docs/ROADMAP.md`: the milestones and what each one delivers.
- `docs/research/`: the research behind the decisions, with sources.
- `docs/v1-lessons/`: lessons carried over from the first Astroray.
- `spikes/`: throwaway measurement code and results (`RESULTS.md` in each).
- `docs/STATUS.md`: the current state on one page. `tasks/`: open work.
- `CLAUDE.md`: rules for AI agents that work on the repository.
