# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from
`tools/status.ps1` once it exists, never from this page.

## Phase
Build phase. The owner signed off the planning records (O30) and asked for
speed (O31): several implementers at once, review for kernel, physics, ABI and
process diffs.

## Current milestone
M1a "First pixels" (`docs/ROADMAP.md`): textured, lamp-lit meshes render
progressively in the Blender 5.3 viewport and F12 on CUDA and CPU through the
final C ABI, with every gate live from the first task. Open task files and
their order: `tasks/README.md`.

## Open owner questions
During M1a: deleting superseded records (0009 q4); exact JPEG decode parity with Blender (libjpeg-turbo) or ruling
78's 2-code bound. Answered: open-weight worker (O32); GPU performance counters for all users (0002 q1, task 093).
Later ones: ROADMAP "Open items".

## Red items
None. Unowned follow-ups (a task that touches the area takes them):
- ABI/host: no CUDA probe reason or driver version (017); 032's CUDA init uses its own Ledger and creates the context
  on the init thread (workers make it current); CPU session Ledgers sit outside context and scene totals (052, 104);
  052 starts an epoch per activated commit (no display-only flag, 0007 d12); 051's display ring downloads
  synchronously (0007 d29's pinned slots and copy stream are needed before 096); PASS_UNSUPPORTED entries persist for
  a session's life (ruling 74).
- Kernel: JH round trip 1.1-1.6e-3 off-grid near white, 4.3e-3 on saturated emitters (023); vm/store_slot.h copies
  kSlotSemantic (should be one device-visible definition); 0006's interval rules have no opcode-generator owner;
  eta_clamped has no counter, so SPECTRUM_OUT_OF_RANGE never fires from shading (073); light splats must reject
  negative radiance before conversion (047).
- Addon: translate.py copies RecordSlot ids, SEM_REFLECTANCE and TEX bits from C headers, guarded by a drift test;
  opsgen/abigen should export them (083). CMake's astroray_extension target is not in ALL (084). The build record prints `cuda=none` on CUDA runs (106).
- Tests: check 14 (sampler/hero lane independence) fails on a constant hero only through a NaN chi2 exception and
  cannot see a hero fixed in generate_camera (078); the per-pixel analytic z of 5.50 misses 1 % albedo errors on
  A1 and a4_sphere (energy tests catch them, 078). Flaky once: `cpu_device/parallel_for rethrows` (0.515 s vs B-12),
  `wavelengths/stratified lane positions`. Fault tests leave astroray_fault_* dirs in temp; some files mix CRLF/LF.
- To settle (076, 107): Russian roulette starts at depth >= max(2, min_bounces) (0004 D2), so min = max bounces does
  not disable it, unlike kernel_globals.h, ar.py and the addon text (check Cycles, fix kernel or text); AUTO emission
  needs an estimate > 0.5 (067; confirm vs Cycles); NaN material inputs render black with no INVALID_SAMPLE (report it).
- M1b: Mix/Add Shader needs 058's compiler to accept one value feeding two EMIT_LE; textured Roughness and Specular
  IOR Level need a colour-to-float op (Cycles: luminance); 083 Do 7 (auto-converted Emission flag, per-term RGB for
  AUTO); a spectral AR_SPECTRUM_BLACKBODY lamp option (082 folds temperature to RGB as Cycles does).

## Budget headroom
Landing gates ~135-145 s of B-09's 180 s after 089 (B-1M tripwire ~19 s, Blender smoke ~18-21 s, analytic ~20 s per
device); new gate work must find time. `check fast` 5-7 s warm, 9-10 s after a header rebuild (B-08 is 10 s).
Spike numbers: docs/spikes/*/RESULTS.md.
