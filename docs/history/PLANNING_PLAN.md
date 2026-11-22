# Astroray 2.0 — plan for the planning phase

## Context

Astroray v1 works but carries a year of structural debt. The owner wants a ground-up rebuild in `C:\Astroray2.0` (empty today) that keeps the north star — a scientifically accurate spectral renderer, usable from Blender, for astrophysical work — and drops what cannot be fixed in place. This document plans the **planning phase**. Its output is a set of evidence-backed architecture decisions, a scrap/keep list, and an implementation roadmap that Sonnet-class agents can build from. No engine code is written until the owner signs off on that output.

## Owner decisions already made (2026-11-14)

- Correctness target: **physics + OpenPBR spec**. No pixel-parity gates against Cycles.
- CPU role: **one kernel source, two targets**. No separately written CPU oracle.
- Blender: **ship on 5.3, design 5.4-ready**.
- GPU backend: **research decides** (NVIDIA-only vs. vendor-neutral).

## What reconnaissance found (13 read-only agents)

**v1 costs a rewrite can remove by construction**
- One fused shade kernel: 128 template variants, registers saturated at 254, one translation unit took 1,800 s of a 1,870 s build.
- Every material and light tree hand-ported twice (CPU plugin + GPU twin); two CPU integrators; three spectral generations (380–780 double, 360–830 float); GR is CPU-only.
- Viewport: about five full-frame host copies per present and a new GPU texture each time; geometry edits trigger a full re-export; addon `__init__.py` is 9,618 lines.
- Shader nodes: 340 of 527 sockets dropped silently; anything after a texture is constant-folded.
- Five build trees, MinGW miscompiles, CI on Linux/GCC without a GPU; suite 24 min, GPU tier alone 16 min.
- Process weight: STATUS.md 373 KB, 334 package specs, three copies of agent roles, CLAUDE.md full of superseded directives.

**v1 knowledge worth keeping** (as lessons and data, not code): physics rules from the memory notes, one-RNG-draw-per-number contract, Jakob–Hanika tables and CIE data, Dormand–Prince Kerr–Schild integrator design, depsgraph change-mask classification, camera-hash trick, degradation reporting, evidence-not-claims delegation, cite-before-implement.

**Corrections to starting assumptions**
- Blender 5.3 (released 2026-11-10) makes **Vulkan the default backend**. OpenGL interop is a dead end.
- **DLSS in 5.3 is a Cycles-internal viewport denoiser.** No API for external engines was found. NVIDIA's Streamline SDK targets DX11/Vulkan, and its licence terms for a GPL Blender addon are unknown.
- **Principled BSDF is already OpenPBR-based since 4.5.** A separate OpenPBR node in 5.4 is reported only by a third-party source; unconfirmed.
- Cycles' zero-copy display path is a C++ interface unavailable to Python addons. No Python route to import external GPU memory was found.
- The "Spectral" project is most likely **Spectra Render** (Rust + Vulkan RT + Slang, out-of-process daemon talking to a thin addon, closed source).
- Not yet researched: PBRT-v4, Mitsuba 3, Manuka, Hydra maturity, licensing, colour management, VRAM budget.

**The honest case against rewriting:** most pain is local and fixable, and no evidence yet shows a new design avoids the register pressure of spectral BSDF + NEE. The spikes in Stage 2 exist to prove that before commitment.

## Working hypotheses (to test, not assume)

1. A standalone engine library with a small C API; Blender is one front end, a CLI is another.
2. An out-of-process or native render thread owns the GPU; Python never touches per-frame pixels.
3. Kernels written once (Slang or a Cycles-style C++ layer) and compiled for CPU and GPU.
4. Small per-stage kernels with bounded material state instead of a fused shade kernel.
5. Rays carry physical state (position, wavevector, wavelengths, units) so curved spacetime and non-visible bands are a special case of the core, not a bolt-on.
6. OpenPBR is the internal material model; Principled and other Blender nodes translate into it; unsupported nodes fail loudly.
7. MSVC + Ninja only; one build tree; hard budgets on build and test time.

## Planning phase stages

### Stage 0 — Bootstrap (lead, ~1 hour)
- `git init` in `C:\Astroray2.0`; create `docs/` with `research/`, `decisions/`, `v1-lessons/`.
- Write a v2 `CLAUDE.md` of at most ~80 lines: behavioural rules, cite-before-implement, evidence-not-claims, build/test budgets. No model IDs, no dated directives, no hooks.
- Save the reconnaissance digests into `docs/research/00-recon.md`.

### Stage 1 — Research workstreams (Sonnet research, Haiku reading)
One workflow per group; each workstream ends in one file under `docs/research/` of at most ~400 lines with sources, plus a list of unverified claims.

| # | Workstream | Key questions |
|---|---|---|
| W1 | Blender boundary | RenderEngine vs. Hydra delegate vs. out-of-process daemon; incremental sync; what 5.3's inlined node-tree API gives; extension packaging |
| W2 | Viewport presentation | Fastest legal path from engine GPU memory to a Vulkan-backed Blender viewport; shared memory; what Spectra Render and other engines do |
| W3 | Kernel language and backend | Slang vs. C++ macro layer; CUDA/OptiX vs. Vulkan RT; NVIDIA-only vs. portable; compile-time behaviour |
| W4 | Integrator architecture | PBRT-v4, Mitsuba 3, Cycles kernel structure; path-state layout; samplers; light sampling; guiding; volumes |
| W5 | Spectral and material model | OpenPBR 1.1/1.1.1 in full; MaterialX reference; hero wavelengths; upsampling; dispersion; thin film; band range and units |
| W6 | Astrophysics core | Spacetime interface; geodesic integration on GPU (KPolaris, Odyssey); polarised transfer; emission tables; validation problems |
| W7 | Blender feature surface | Node coverage target; sky, textures, volumes, lights, hair, instancing, motion blur, AOVs, colour management; which are v2.0 vs. later |
| W8 | Build and toolchain | MSVC/Ninja/CUDA layout; binding (C API + nanobind); caching that actually hits; CI with a GPU |
| W9 | Test and validation | Tiers with time budgets; statistical, furnace, analytic tests; small canonical scene set; which v1 references are trustworthy |
| W10 | Denoise and upscaling | OIDN 3, OptiX, DLSS-RR licence and integration feasibility |
| W11 | Licensing | GPL obligations, NVIDIA SDK terms, atomic data provenance |
| W12 | v1 lessons distillation | Haiku reads all 144 memories and key v1 docs in full; Opus condenses to `docs/v1-lessons/` (physics, Blender API, toolchain, process), each under ~150 lines |
| W15 | v1 research harvest | Every research note in v1's planning docs (302 files, 11 topics; 119 process files skipped) read in full by Haiku and distilled by Sonnet into `docs/v1-lessons/research/<topic>.md` with citations, measured numbers and dead ends. Stage 3 decision drafters must read the matching topic file |
| W13 | Agent process for v2 | Minimal rules, spec format, model routing, how lanes build and verify without the v1 machinery |

**Owner checkpoint 1:** research summary and provisional scrap/keep list.

### Stage 2 — Measurement spikes (throwaway code in `C:\Astroray2.0\spikes\`, Sonnet builds, Opus reviews)
Each spike answers one question with a number and is deleted or archived afterwards.
- **S1 Presentation:** native module pushes a 1080p float frame into Blender 5.3's viewport three ways; measure ms per present.
- **S2 Kernel language:** one OpenPBR-style layered BSDF with 4 hero wavelengths and NEE, written in Slang and in shared C++; compile for CPU and GPU; record compile seconds, register count, and equality of results.
- **S3 Toolchain:** MSVC + Ninja + CUDA + binding loaded inside Blender 5.3's Python; cold and incremental build seconds.
- **S4 Sync:** incremental scene delta for a 1M-triangle scene; measure time per transform, material and geometry edit.
- **S5 Curved rays:** Kerr geodesic stepping on the GPU inside the same path-state layout; rays per second and error against the analytic photon ring.

### Stage 3 — Architecture decisions
- One decision record per item in `docs/decisions/` (context, options, evidence, decision, consequences), in dependency order: scope, spectral model, ray model, backend/language, integrator and state, material model, host boundary, scene model, sampling contract, toolchain, verification.
- Each record drafted by Opus from the research and spike numbers, then attacked by an independent Opus reviewer told to refute it. The lead (this session) adjudicates.
- **Owner checkpoint 2:** decisions that are genuinely the owner's are raised as questions; the rest are presented for veto.

### Stage 4 — Implementation roadmap
- `docs/ARCHITECTURE.md` (one document, under ~500 lines).
- `docs/ROADMAP.md`: milestones that each end in something visible in Blender, starting with a vertical slice (triangle → viewport) before breadth.
- `docs/TESTING.md`: tiers, budgets, the canonical scene list, reference sources.
- First milestone broken into specs small enough for a Sonnet agent, each with a build-and-test acceptance check.
- **Owner sign-off → build phase starts in a fresh session rooted at `C:\Astroray2.0`.**

## Keeping v1 out of v2's context

- v2 is its own repository and project directory, so v1's CLAUDE.md, hooks, skills and memory never load there.
- v1 knowledge crosses only through `docs/v1-lessons/`, written once in this phase. Build agents are told not to open the v1 repository; a question that needs v1 goes to a single read-only Haiku lookup that returns a cited answer.
- Planning research runs from this session because it needs read access to v1; subagents use types that do not inherit v1's CLAUDE.md, with explicit models.
- v2 status lives in one page; decisions live in decision records; no package-spec corpus, no session journal.

## Owner requirements designed in from the start (added 2026-11-14)

- **Diagnostic outputs.** The integrator writes statistics buffers alongside the image: sample count per pixel, bounce count, variance/error estimate, path termination reason, time per pixel, plus the usual albedo/normal/depth. Shown as heat maps in the viewport and saved with final renders. The path state and film must carry these from the first milestone; more can be added later.
- **Benchmarks produce graphs.** One benchmark command writes a self-contained HTML report: convergence curves (error vs. samples and vs. time), samples per second, build and test time history, and CPU/GPU comparison, with the renders embedded. History is stored as small data files so trends plot across commits.
- **Pretty renders are a deliverable.** Each milestone ends with showcase renders in a gallery, not only test images. Showcase scenes are part of the canonical scene set.
- **Scenes.** W16 catalogues standard public test scenes and models (`docs/research/16-test-scenes-and-assets.md`) and lists the scenes that do not exist publicly, for the owner to model. Modelling rules: real-world scale in metres, closed solids with outward normals, no coincident faces, smooth shading on glass and liquids, Principled BSDF only, CC0 textures and HDRIs only, saved camera views.

## Tooling for the v2 project

- Connectors (Blender, browser, docs lookup) are attached to the user account and the desktop app, not to a project folder, so they are available in the v2 project without setup.
- `.claude/settings.json` in v2: prompt-cache settings, one session-start hook that prints `git status`, the docs-lookup plugin on, the Superpowers plugin off for this project (it injects a mandatory-skill preamble into every session, which works against the lean-context goal). No other hooks; a hook is added only when it prevents a failure that has actually happened.
- Blender 5.3 is not installed on this machine yet (5.1 and 5.2 are). It is needed before spike S1.

## Which session does what

- **Planning phase (Stages 0-4): the existing v1-rooted session.** It needs read access to v1 and writes only into `C:\Astroray2.0`.
- **Build phase: a new project rooted at `C:\Astroray2.0`.** It loads only v2's `CLAUDE.md`; no v1 hooks, skills or memory. The handover is the owner's sign-off on `ARCHITECTURE.md` and `ROADMAP.md`.

## Reference implementations (workstream W14)

Open-source engines are used as reference code, not as parity targets. W14 produces `docs/research/14-reference-implementations.md`: for each algorithm the roadmap needs, the paper, the exact reference file to read, its licence, and whether code may be adapted or only studied.

- **PBRT-v4** — primary reference for spectral design, the wavefront GPU integrator and samplers.
- **Mitsuba 3** — spectral and polarised transport, statistical test design, and an independent oracle for comparison scenes.
- **Cycles** — Blender semantics, node behaviour, single-source kernel layout, display path. Licence checked per file.
- **OpenPBR / MaterialX reference** — the material model and ground truth for material tests.
- **LuxCore** — a third-party Blender integration to learn from.
- **RTXPT / Falcor** — ReSTIR-class viewport methods and denoiser inputs; ideas only until licence terms are checked.
- **ipole, RAPTOR, Blacklight, GYOTO** — geodesics, covariant transfer, validation problems; cite papers, do not copy GPL code.

Rule for the build phase: every algorithm spec names its reference file and paper, and the implementing agent reads that file rather than browsing the engine.

## Model allocation

- Haiku 5.5: bulk reading and digests. Sonnet 5.5: web research, spikes, later implementation. Opus 5.5: decision drafting, adversarial review, debugging. DeepSeek V4.1 Flash (paid `opencode-go` id): bounded mechanical work. No Fable subagents; every workflow agent sets its model explicitly.

## Provisional budgets (to be confirmed by S3 and W9)

- Incremental build after one kernel edit under 30 s; clean build under 5 min.
- Pre-commit tests under 10 s without a GPU; merge gate under 3 min on the 5070 Ti; nightly under 20 min.
- A 20% regression against a budget fails the check.

## How this phase is verified

- Every decision record cites a research file or a spike measurement; none rests on reasoning alone.
- Each research file lists what it could not verify; a completeness critic runs after Stage 1 and Stage 3.
- Spikes report measured numbers with the command that produced them.
- The owner reviews at checkpoint 1, checkpoint 2 and final sign-off.

## Files this phase creates

All under `C:\Astroray2.0`: `CLAUDE.md`, `docs/research/*.md`, `docs/v1-lessons/*.md`, `docs/decisions/*.md`, `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, `docs/TESTING.md`, `spikes/` (temporary). Nothing in the v1 repository is modified.
