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
During M1a: deleting superseded records (0009 q4). Answered: open-weight
worker (O32); GPU performance counters enabled for all users (0002 q1, task 093). Later ones:
ROADMAP "Open items".

## Red items
None. Follow-ups noted for later tasks: the ABI exposes no CUDA probe reason or
driver version (017); JH table round trip is 1.1-1.6e-3 off-grid near white and
up to 4.3e-3 per channel on saturated emitters (023). The material VM copies 056's kSlotSemantic into
vm/store_slot.h (nvcc rejects the shared inline constexpr; a test pins the rows),
which should become one device-visible definition in constant_block.h. 0006's
required interval rules in the opcode generator have no owning task yet. M1b (Mix/Add
Shader): 058's compiler rejects one value feeding two EMIT_LE nodes; insert a
copy or clone shared constants. Tooling: a new test file once needed a CMakeLists
touch to reconfigure. 032's CUDA init uses its own Ledger, not the context's
(fix with 035/041), and the CUDA context is created on the init thread, so
workers must make it current. The fault tests leave astroray_fault_*_<pid>
directories in the system temp dir. Some files mix CRLF and LF (CMakeLists.txt,
tests/cpu/test_fault.cpp). Flaky once, passed on rerun: `cpu_device/parallel_for rethrows`
(0.515 s against B-12's 0.5 s) and `wavelengths/stratified lane positions` (068). stack_sample's eta_clamped has no kernel counter, so
SPECTRUM_OUT_OF_RANGE is never reported from shading; no task owns it yet (from 073). Light-tracing splats must reject negative spectral
radiance where they are written, before conversion: the resolve checks only
NaN and Inf on the converted SPLAT plane (047). 051's display ring copies with a
synchronous Device::download into heap slots: the pinned slots and copy stream of
0007 d29 need the Device seam to gain both; no task owns this yet, and it must be
written before 096 measures the viewport rate. 052 starts an epoch for every
activated commit, because a commit's Pending does not record whether it is display-only
(0007 d12); the flag and its use belong with settings sync, and no task owns them yet. Each CPU
session owns its Ledger (052), so session allocations appear in no context or scene total;
104's ar_progress ledger fields read the session's own.

## Budget headroom
Landing gates run 43-55 s of B-09's 180 s (kernel part 11-21 s of 120 s); `check fast` 5-7 s warm,
9-10 s after a header rebuild (B-08 is 10 s). Spike numbers: docs/spikes/*/RESULTS.md.
